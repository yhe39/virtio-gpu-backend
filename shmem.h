#ifndef __BACKENDS_SHMEM_H__
#define __BACKENDS_SHMEM_H__

#include <stddef.h>

struct shmem_info;

struct shmem_ops {
	const char *name;

	int (*open)(const char *devpath, struct shmem_info *info, int evt_fds[], int nr_ent_fds);
	void (*close)(struct shmem_info *info);
	void (*notify_peer)(struct shmem_info *info, int vector);
};

struct shmem_info {
	struct shmem_ops *ops;

	void *mmio_base;

	int mem_fd;
	void *mem_base;
	size_t mem_size;

	int this_id;
	int peer_id;

	int nr_vecs;

	void *private_data;
};

extern struct shmem_ops uio_shmem_ops;
extern struct shmem_ops ivshm_ivshmem_ops;
extern struct shmem_ops ivshm_guest_shm_ops;
extern struct shmem_ops sock_ivshmem_ops;

#endif  /* __BACKENDS_SHMEM_H__ */
