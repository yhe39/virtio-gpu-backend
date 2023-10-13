#include <stdio.h>
#include <stddef.h>
#include <error.h>
#include <errno.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <linux/virtio_pci.h>
#include <pthread.h>
#include <signal.h>

#include <pci_core.h>
#include <virtio.h>
#include <log.h>
#include <mevent.h>
#include <pm.h>
#include <vmmapi.h>
#include <unistd.h>

#include "virtio_over_shmem.h"
#include "shmem.h"
#include "utils.h"

#define MAX_IRQS    8

struct virtio_shmem_header *virtio_header;

static struct shmem_info shmem_info;
static int evt_fds[MAX_IRQS];
static struct mevent *mevents[MAX_IRQS];
static struct pci_vdev pci_vdev;

static void
sig_handler_term(int signo __attribute__((unused)))
{
	printf("Received SIGINT to terminate application...\n");
	vm_set_suspend_mode(VM_SUSPEND_POWEROFF);
	mevent_notify();
}

static void process_queue(struct pci_vdev *dev)
{
	struct virtio_base *base = dev->arg;
	struct virtio_ops *vops = base->vops;
	struct virtio_vq_info *vq;
	int i;

	/*
	 * Virtio-snd uses virtqueue 0 for control messages and 2/3 for tx/rx data. During playback starting there is an
	 * implicit requirement on the order of message handling: the (typically async) data messages in virtqueue 2
	 * (txq) must be processed before the PCM_START message in virtqueue 0 (controlq). Unfortunately that could be
	 * violated when multiple virtqueues share the same interrupt, and the interrupt handler walks virtqueue 0
	 * first.
	 *
	 * For now we work around that issue by walking through the queues in decremental order. Hopefully no other
	 * device has similar constraints on inter-virtqueue processing order.
	 */
        for (i = base->vops->nvq - 1; i >= 0; i--) {
		vq = &base->queues[i];
		if(!vq_ring_ready(vq))
			continue;

		if (vq->notify)
			(*vq->notify)((void *)base, vq);
		else if (vops->qnotify)
			(*vops->qnotify)((void *)base, vq);
		else
			pr_err("%s: qnotify queue %d: missing vq/vops notify\r\n", vops->name, i);
	}
}

static void process_write_transaction(struct pci_vdev *dev)
{
	void *new_value_p;
	uint64_t new_value;
	uint32_t offset;

	if (virtio_header->write_transaction == 0)
		return;

	new_value_p = (void*)((char*)virtio_header + virtio_header->write_offset);
	new_value =
		(virtio_header->write_size == 1) ? (*(uint8_t  *)new_value_p) :
		(virtio_header->write_size == 2) ? (*(uint16_t *)new_value_p) :
		(virtio_header->write_size == 4) ? (*(uint32_t *)new_value_p) :
		0xffffffff;

	if (virtio_header->write_offset >= offsetof(struct virtio_shmem_header, common_config) &&
	    virtio_header->write_offset < offsetof(struct virtio_shmem_header, config)) {
		offset = virtio_header->write_offset - offsetof(struct virtio_shmem_header, common_config);
		virtio_common_cfg_write(dev, offset, virtio_header->write_size, new_value);

		/* Handle side effects */
		switch (offset) {
		case VIRTIO_PCI_COMMON_DFSELECT:
			/* Force VIRTIO_F_VERSION_1 and VIRTIO_F_ACCESS_PLATFORM to be 1. */
			virtio_header->common_config.device_feature =
				virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_DF, 4) |
				((virtio_header->common_config.device_feature_select == 1) ?
				 ((1 << (VIRTIO_F_ACCESS_PLATFORM - 32)) | (1 << (VIRTIO_F_VERSION_1 - 32))) :
				 0);
			break;
		case VIRTIO_PCI_COMMON_GFSELECT:
			virtio_header->common_config.guest_feature = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_GF, 4);
			break;
		case VIRTIO_PCI_COMMON_Q_SELECT:
			virtio_header->common_config.queue_size = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_SIZE, 2);
			virtio_header->common_config.queue_msix_vector = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_MSIX, 2);
			virtio_header->common_config.queue_enable = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_ENABLE, 2);
			virtio_header->common_config.queue_notify_off = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_NOFF, 2);
			virtio_header->common_config.queue_desc_lo = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_DESCLO, 4);
			virtio_header->common_config.queue_desc_hi = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_DESCHI, 4);
			virtio_header->common_config.queue_avail_lo = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_AVAILLO, 4);
			virtio_header->common_config.queue_avail_hi = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_AVAILHI, 4);
			virtio_header->common_config.queue_used_lo = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_USEDLO, 4);
			virtio_header->common_config.queue_used_hi = virtio_common_cfg_read(dev, VIRTIO_PCI_COMMON_Q_USEDHI, 4);
			break;
		}
	} else if (virtio_header->write_offset >= offsetof(struct virtio_shmem_header, config)) {
		struct virtio_base *base = dev->arg;
		offset = virtio_header->write_offset - offsetof(struct virtio_shmem_header, config);
		base->vops->cfgwrite(dev, offset, virtio_header->write_size, new_value);
	}

	__sync_synchronize();
	virtio_header->write_transaction = 0;
}

static void handle_requests(int fd, enum ev_type t __attribute__((unused)), void *arg __attribute__((unused)))
{
	eventfd_t val;
	eventfd_read(fd, &val);

	if ((shmem_info.peer_id == -1) && (virtio_header->frontend_flags != 0)) {
		shmem_info.peer_id = virtio_header->frontend_id;
		printf("Frontend peer id: %d\n", shmem_info.peer_id);
	}

        process_write_transaction(&pci_vdev);
	if (virtio_header->common_config.device_status == 0xf)
		process_queue(&pci_vdev);
}

int vos_backend_init(struct virtio_backend_info *info)
{
	int ret = -1, i;
//	struct epoll_event ev = {0};
	struct virtio_base *base;

	ret = mevent_init();
	if (ret < 0)
		return ret;

	for (i = 0; i < MAX_IRQS; i++) {
		evt_fds[i] = eventfd(0, EFD_NONBLOCK);
		if (evt_fds[i] < 0) {
			ret = errno;
			goto close_evt_fds;
		}
	}

        ret = info->shmem_ops->open(info->shmem_devpath, &shmem_info, evt_fds, MAX_IRQS);
	if (ret < 0) {
		ret = errno;
		goto close_evt_fds;
	}

	printf("Shared memory size: 0x%lx\n", shmem_info.mem_size);
	printf("Number of interrupt vectors: %d\n", shmem_info.nr_vecs);
	printf("This ID: %d\n", shmem_info.this_id);

	for (i = 0; i < MAX_IRQS; i++) {
		if (i < shmem_info.nr_vecs) {
			mevents[i] = mevent_add(evt_fds[i], EVF_READ, handle_requests, NULL, NULL, NULL);
			if (mevents[i] == NULL)
				goto deregister_mevents;
		} else {
			close(evt_fds[i]);
			evt_fds[i] = 0;
		}
	}

	virtio_header = shmem_info.mem_base;
	memset(virtio_header, 0, sizeof(struct virtio_shmem_header));
	virtio_header->backend_status = (shmem_info.this_id << 16) | BACKEND_FLAG_PRESENT;
	virtio_header->revision = 1;

	pci_vdev.vmctx = (struct vmctx *)&shmem_info;
	pci_vdev.dev_ops = info->pci_vdev_ops;
	if (pci_vdev.dev_ops->vdev_init(pci_vdev.vmctx, &pci_vdev, info->opts)) {
		ret = -1;
		goto deregister_mevents;
	}

	virtio_header->device_id = pci_get_cfgdata16(&pci_vdev, PCIR_SUBDEV_0);
	virtio_header->vendor_id = pci_get_cfgdata16(&pci_vdev, PCIR_SUBVEND_0);

	base = pci_vdev.arg;
	virtio_header->size = sizeof(struct virtio_shmem_header) + base->vops->cfgsize;
	base->vops->cfgread(base, 0, base->vops->cfgsize, (void *)virtio_header->config);

	pci_vdev.msix.enabled = 1;

	return 0;

deregister_mevents:
	for (i = 0; i < MAX_IRQS; i++) {
		if (mevents[i])
			mevent_delete(mevents[i]);
	}

//close_shmem:
//	info->shmem_ops->close(&shmem_info);

close_evt_fds:
	for (i = 0; i < MAX_IRQS; i++) {
		if (evt_fds[i])
			close(evt_fds[i]);
	}

	return ret;
}

int vos_backend_run(void)
{
	if (signal(SIGHUP, sig_handler_term) == SIG_ERR)
		fprintf(stderr, "cannot register handler for SIGHUP\n");

	if (signal(SIGINT, sig_handler_term) == SIG_ERR)
		fprintf(stderr, "cannot register handler for SIGINT\n");

	printf("Starting virtio device\n");
	mevent_dispatch();
}

void vos_backend_deinit(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		mevent_delete(mevents[i]);
		close(evt_fds[i]);
	}
	shmem_info.ops->close(&shmem_info);
	mevent_deinit();
}
