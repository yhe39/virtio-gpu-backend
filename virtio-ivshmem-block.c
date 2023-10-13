// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtio block over uio_ivshmem back-end device
 *
 * Copyright (c) Siemens AG, 2019
 */

/*
 * HACK warnings:
 *  - little-endian hosts only
 *  - no proper input validation (specifically addresses)
 *  - may miss a couple of barriers
 *  - ignores a couple of mandatory properties, e.g. notification control
 *  - could implement some optional block features
 *  - might eat your data
 */

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>

#include "shmem.h"

#ifndef VIRTIO_F_ORDER_PLATFORM
#define VIRTIO_F_ORDER_PLATFORM		36
#endif

struct virtio_shmem_block {
	uint32_t revision;
	uint32_t size;
	uint32_t device_id;
	uint32_t vendor_id;

        union {
		uint32_t write_transaction;
		struct {
			uint16_t write_offset;
			uint16_t write_size;
		};
	};
	uint8_t config_event;
	uint8_t queue_event;
	uint8_t __rsvd[10];

	struct virtio_pci_common_cfg common_config;
	struct virtio_blk_config config;
};

#define VI_REG_OFFSET(reg) \
	__builtin_offsetof(struct virtio_shmem_block, common_config.reg)

static struct shmem_info shmem_info;
static int image_fd, evt_fds[8], epoll_fd;
static struct stat image_stat;
static struct virtio_shmem_block *vb;
static struct vring vring;
static uint16_t next_idx;
static void *shmem;

static void wait_for_interrupt(void)
{
	struct epoll_event ev[8] = {0};
	eventfd_t val;
	int n, i;

	n = epoll_wait(epoll_fd, ev, 8, -1);
	if (n < 0) {
		printf("epoll wait error: %d\n", n);
	}

	for (i = 0; i < n; i++) {
		eventfd_read(ev[i].data.fd, &val);
	}
}

static int process_queue(void)
{
	struct virtio_blk_outhdr *req;
	struct vring_desc *desc;
	int idx, used_idx, ret;
	size_t size, len;
	uint8_t status;

	if (next_idx == vring.avail->idx)
		return 0;

	idx = vring.avail->ring[next_idx % vring.num];

	desc = &vring.desc[idx];
	assert(desc->len == sizeof(*req));
	assert(desc->flags & 1);
	req = shmem + desc->addr;

	len = 1;

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		desc = &vring.desc[desc->next];
		assert(desc->flags & 1);
		size = desc->len;
		ret = pread(image_fd, shmem + desc->addr, size,
			    req->sector * 512);
		if (ret == size) {
			status = VIRTIO_BLK_S_OK;
			len += size;
		} else {
			status = VIRTIO_BLK_S_IOERR;
		}
		break;
	case VIRTIO_BLK_T_OUT:
		desc = &vring.desc[desc->next];
		assert(desc->flags & 1);
		size = desc->len;
		ret = pwrite(image_fd, shmem + desc->addr, size,
			     req->sector * 512);
		status = ret == size ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
		break;
	case VIRTIO_BLK_T_FLUSH:
		ret = fsync(image_fd);
		status = ret == 0 ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
		break;
	case VIRTIO_BLK_T_GET_ID:
		desc = &vring.desc[desc->next];
		assert(desc->flags & 1);
		len = desc->len > 0 ? 1 : 0;
		memset(shmem + desc->addr, 0, len);
		status = VIRTIO_BLK_S_OK;
		break;
	default:
		printf("unknown request %d\n", req->type);
		status = VIRTIO_BLK_S_UNSUPP;
		break;
	}

	desc = &vring.desc[desc->next];
	assert(!(desc->flags & 1));

	*(uint8_t *)(shmem + desc->addr) = status;

	used_idx = vring.used->idx % vring.num;
	vring.used->ring[used_idx].id = idx;
	vring.used->ring[used_idx].len = len;

	__sync_synchronize();
	vring.used->idx++;
	next_idx++;

	vb->queue_event = 1;
	__sync_synchronize();
	shmem_notify_peer(&shmem_info, vb->common_config.queue_msix_vector);

	return 1;
}

static int process_write_transaction(void)
{
	switch (vb->write_offset) {
	case 0:
		return 0;
	case VI_REG_OFFSET(device_feature_select):
		printf("device_features_sel: %d\n", vb->common_config.device_feature_select);
		if (vb->common_config.device_feature_select == 1) {
			vb->common_config.device_feature =
				(1 << (VIRTIO_F_VERSION_1 - 32)) |
				(1 << (VIRTIO_F_IOMMU_PLATFORM - 32)) |
				(1 << (VIRTIO_F_ORDER_PLATFORM - 32));
		} else {
			vb->common_config.device_feature =
				(1 << VIRTIO_BLK_F_SIZE_MAX) |
				(1 << VIRTIO_BLK_F_SEG_MAX) |
				(1 << VIRTIO_BLK_F_FLUSH);
		}
		break;
	case VI_REG_OFFSET(guest_feature_select):
		printf("guest_features_sel: %d\n", vb->common_config.guest_feature_select);
		break;
	case VI_REG_OFFSET(guest_feature):
		printf("guest_features[%d]: 0x%x\n", vb->common_config.guest_feature_select,
		       vb->common_config.guest_feature);
		break;
	case VI_REG_OFFSET(queue_select):
		printf("queue_sel: %d\n", vb->common_config.queue_select);
		vb->common_config.queue_size = 8;
		break;
	case VI_REG_OFFSET(queue_size):
		printf("queue size: %d\n", vb->common_config.queue_size);
		break;
	case VI_REG_OFFSET(queue_msix_vector):
		printf("queue driver vector: %d\n",
		       vb->common_config.queue_msix_vector);
		break;
	case VI_REG_OFFSET(queue_enable):
		printf("queue enable: %d\n", vb->common_config.queue_enable);
		if (vb->common_config.queue_enable) {
			vring.num = vb->common_config.queue_size;
			vring.desc = shmem + ((uint64_t)vb->common_config.queue_desc_lo + ((uint64_t)vb->common_config.queue_desc_hi << 32));
			vring.avail = shmem + ((uint64_t)vb->common_config.queue_avail_lo + ((uint64_t)vb->common_config.queue_avail_hi << 32));
			vring.used = shmem + ((uint64_t)vb->common_config.queue_used_lo + ((uint64_t)vb->common_config.queue_used_hi << 32));
			next_idx = 0;
		}
		break;
	case VI_REG_OFFSET(queue_desc_lo):
		printf("queue desc lo: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_desc_lo);
		break;
	case VI_REG_OFFSET(queue_desc_hi):
		printf("queue desc hi: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_desc_hi);
		break;
	case VI_REG_OFFSET(queue_avail_lo):
		printf("queue avail lo: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_avail_lo);
		break;
	case VI_REG_OFFSET(queue_avail_hi):
		printf("queue avail hi: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_avail_hi);
		break;
	case VI_REG_OFFSET(queue_used_lo):
		printf("queue used lo: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_used_lo);
		break;
	case VI_REG_OFFSET(queue_used_hi):
		printf("queue used hi: 0x%llx\n",
		       (unsigned long long)vb->common_config.queue_used_hi);
		break;
	case VI_REG_OFFSET(device_status):
		printf("device_status: 0x%x\n", vb->common_config.device_status);
		break;
	default:
		printf("unknown write transaction for %x\n",
		       vb->write_transaction);
		break;
	}

	__sync_synchronize();
	vb->write_transaction = 0;

	return 1;
}

int main(int argc, char *argv[])
{
	int pagesize = getpagesize();
	unsigned long long shmem_sz;
	int event, ret, i;
	char sysfs_path[64];
	char size_str[64];
	char *uio_devname;
	struct epoll_event ev = {0};

	if (argc < 3) {
		fprintf(stderr, "usage: %s UIO-DEVICE IMAGE\n", argv[0]);
		return 1;
	}

	image_fd = open(argv[2], O_RDWR);
	if (image_fd < 0)
		error(1, errno, "cannot open %s", argv[2]);

	ret = fstat(image_fd, &image_stat);
	if (ret < 0)
		error(1, errno, "fstat failed");

	for (i = 0; i < 2; i++) {
		evt_fds[i] = eventfd(0, 0);
		if (evt_fds[i] < 0)
			error(1, errno, "cannot create eventfd");
		printf("create eventfd %d\n", evt_fds[i]);
	}

	ret = shmem_open(argv[1], &shmem_info, evt_fds, 2);
	if (ret < 0)
		error(1, errno, "shmem open failed");
	shmem = shmem_info.mem_base;

	epoll_fd = epoll_create1(0);
	if (epoll_fd < 0)
		error(1, errno, "cannot create epoll fd");

	for (i = 0; i < 2; i++) {
		ev.events = EPOLLIN;
		ev.data.fd = evt_fds[i];
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ev.data.fd, &ev) < 0)
			error(1, errno, "cannot add IRQ %d to epoll", i);
	}

	while (1) {
		vb = shmem;
		memset(vb, 0, sizeof(*vb));
		vb->revision = 1;
		vb->size = sizeof(*vb);
		vb->device_id = VIRTIO_ID_BLOCK;
		vb->vendor_id = 0;

		vb->common_config.queue_size = 8;

		vb->config.capacity = image_stat.st_size / 512;
		vb->config.size_max = (shmem_sz / 8) & ~(pagesize - 1);
		vb->config.seg_max = 1;

		printf("Starting virtio device\n");

		while(1) {
			event = process_write_transaction();

			if (vb->common_config.device_status == 0xf)
				event |= process_queue();

			if (!event)
				wait_for_interrupt();
		}
	}
}
