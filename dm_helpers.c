#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#include <log.h>
#include <pci_core.h>
#include <vmmapi.h>
#include <pm.h>

#include "shmem.h"
#include "virtio_over_shmem.h"
#include "utils.h"

#ifdef ANDROID
#include <android/log.h>
#endif

bool is_winvm = false;
bool stdio_in_use = false;

static enum vm_suspend_how suspend_mode = VM_SUSPEND_NONE;

int vm_get_suspend_mode(void)
{
	return suspend_mode;
}

void vm_set_suspend_mode(enum vm_suspend_how how)
{
	suspend_mode = how;
}

void output_log(uint8_t level __attribute__((unused)), const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
#ifdef ANDROID
    __android_log_vprint (ANDROID_LOG_INFO, "backend" , fmt, args);
#else
	vprintf(fmt, args);
#endif
	va_end(args);
}

bool vm_allow_dmabuf(struct vmctx *ctx)
{
	struct shmem_info *info = (struct shmem_info *)ctx;

	return info->mem_fd > 0;
}

int
pci_msix_enabled(struct pci_vdev *dev)
{
	return (dev->msix.enabled && !dev->msi.enabled);
}

void *paddr_guest2host(struct vmctx *ctx, uintptr_t gaddr, size_t len __attribute__((unused)))
{
	struct shmem_info *info = (struct shmem_info *)ctx;

	if (gaddr < info->mem_size) {
        	char *char_ptr = (char*)info->mem_base;  // 将 void* 转换为 char* 来执行指针算术操作
	        char_ptr += gaddr;
	        return (void*)char_ptr;  // 将结果转换回 void*
	} else {
        	return NULL;
	}
}

bool
vm_find_memfd_region(struct vmctx *ctx, vm_paddr_t gpa, struct vm_mem_region *ret_region)
{
	struct shmem_info *info = (struct shmem_info *)ctx;

	if (!ret_region)
		return false;

	if (info->mem_fd == 0)
		return false;

	if (gpa >= info->mem_size)
		return false;

	ret_region->fd = info->mem_fd;
	ret_region->fd_offset = gpa;

	return true;
}

void pci_generate_msix_config(struct pci_vdev *dev, int index)
{
	struct shmem_info *info = (struct shmem_info *)dev->vmctx;

	virtio_header->config_event= 1;
	virtio_header->config[0] = 0x1;
	__sync_synchronize();
	info->ops->notify_peer(info, index);
}

void pci_generate_msix(struct pci_vdev *dev, int index)
{
	struct shmem_info *info = (struct shmem_info *)dev->vmctx;

	virtio_header->queue_event = 1;
	__sync_synchronize();
	info->ops->notify_peer(info, index);
}
