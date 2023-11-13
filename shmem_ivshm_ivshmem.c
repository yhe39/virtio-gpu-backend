#include <errno.h>
// #include <error.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "shmem.h"
#include "io.h"
#include "log.h"
#include "utils.h"

#define MAX_VECTORS 8

#define IVSHMEM_BAR0_SIZE  256

struct ivshm_listener_data {
	int vector;
	int evt_fd;
};

#define IVSHM_ADD_LISTENER	_IOW('u', 100, struct ivshm_listener_data)
#define IVSHM_GET_MMIO_SZ	_IOR('u', 101, unsigned long long)

struct ivshm_regs {
	uint32_t int_mask;
	uint32_t int_status;
	uint32_t ivpos;
	uint32_t doorbell;
};

static int shmem_open(const char *devpath, struct shmem_info *info, int evt_fds[], int nr_ent_fds)
{
	struct ivshm_listener_data data;
	struct ivshm_regs *regs;
	char ivshm_path[64];
	int ivshm_fd, iregion_fd;
	int i;
	char *idx;

	pr_info("%s -1\n", __func__);
	memset(info, 0, sizeof(*info));

	idx = strstr(devpath, ".");
	if (!idx)
		error(1, errno, "cannot infer ivshm path from %s", devpath);
	memcpy(ivshm_path, devpath, idx - devpath);
	ivshm_path[idx - devpath] = '\0';

	pr_info("%s -2 %s\n", __func__, ivshm_path);
	ivshm_fd = open(ivshm_path, O_RDWR);
	if (ivshm_fd < 0)
		error(1, errno, "cannot open %s", devpath);

	pr_info("%s -3 %s\n", __func__, devpath);
	iregion_fd = open(devpath, O_RDWR);
	if (iregion_fd < 0)
		error(1, errno, "cannot open %s", devpath);

	pr_info("%s -4 %d\n", __func__, ivshm_fd);
	info->mmio_base = mmap(NULL, IVSHMEM_BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ivshm_fd, 0);
	if (info->mmio_base == MAP_FAILED)
		error(1, errno, "mmap of registers failed");

	pr_info("%s -5\n", __func__);
	if (ioctl(iregion_fd, IVSHM_GET_MMIO_SZ, &info->mem_size) < 0)
		error(1, errno, "failed to get ivshm mmio size");

	printf("%s, mem_size: 0x%lx___\r\n", __func__, info->mem_size);
	info->mem_base = mmap(NULL, info->mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, iregion_fd, 0);
	if (info->mem_base == MAP_FAILED)
		error(1, errno, "mmap of shared memory failed");
	info->mem_fd = iregion_fd;

	info->nr_vecs = min(MAX_VECTORS, nr_ent_fds);
	for (i = 0; i < info->nr_vecs; i++) {
		data.vector = i;
		data.evt_fd = evt_fds[i];
		if (ioctl(iregion_fd, IVSHM_ADD_LISTENER, &data) < 0)
			error(1, errno, "cannot bind interrupt vector %d", i);

	}

	regs = (struct ivshm_regs *)info->mmio_base;
	info->this_id = mmio_read32(&regs->ivpos);
	info->peer_id = -1;

	pr_info("%s -6\n", __func__);
	close(ivshm_fd);

	info->ops = &ivshm_ivshmem_ops;

	return 0;
}

static void shmem_close(struct shmem_info *info)
{
	if (info->mem_base) {
		munmap(info->mem_base, info->mem_size);
		info->mem_base = NULL;
		info->mem_size = 0;
		close(info->mem_fd);
	}
}

static void shmem_notify_peer(struct shmem_info *info, int vector)
{
	struct ivshm_regs *regs = (struct ivshm_regs *)info->mmio_base;

	mmio_write32(&regs->doorbell, (info->peer_id << 16) | vector);
}

struct shmem_ops ivshm_ivshmem_ops = {
	.name = "ivshm-ivshmem",

	.open = shmem_open,
	.close = shmem_close,
	.notify_peer = shmem_notify_peer,
};
