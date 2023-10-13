#ifndef __BACKENDS_VIRTIO_OVER_SHMEM_H__
#define __BACKENDS_VIRTIO_OVER_SHMEM_H__

#include <stddef.h>
#include <stdint.h>
#include <linux/virtio_pci.h>

#include <pci_core.h>

#include "shmem.h"

struct virtio_backend_info {
	struct shmem_ops *shmem_ops;
	const char *shmem_devpath;

	char *opts;

	struct pci_vdev_ops *pci_vdev_ops;

	void (*hook_before_init)(struct virtio_backend_info *info);
};

#define BACKEND_FLAG_PRESENT   0x0001

struct virtio_shmem_header {
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
	uint8_t __rsvd[2];
	union {
		uint32_t frontend_status;
		struct {
			uint16_t frontend_flags;
			uint16_t frontend_id;
		};
	};
	union {
		uint32_t backend_status;
		struct {
			uint16_t backend_flags;
			uint16_t backend_id;
		};
	};

	struct virtio_pci_common_cfg common_config;
	char config[];
};

#define VI_REG_OFFSET(reg) \
	__builtin_offsetof(struct shmem_virtio_header, reg)

extern struct virtio_shmem_header *virtio_header;

int vos_backend_init(struct virtio_backend_info *info);
void vos_backend_run(void);
void vos_backend_deinit(void);

#endif  /* __BACKENDS_VIRTIO_OVER_SHMEM_H__ */
