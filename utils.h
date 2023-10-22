#ifndef __BACKENDS_UTILS_H__
#define __BACKENDS_UTILS_H__

#include <stdbool.h>
#include <linux/virtio_ring.h>

#include "virtio_over_shmem.h"

#define min(a, b)  (((a) < (b)) ? (a) : (b))

// #define ACRN_BACKEND_MAIN(info)			\
// int main(int argc, char *argv[]) {		\
// 	run_backend(&info, argc, argv);		\
// 	return 0;				\
// }

void *run_backend(void *info);
int create_backend_thread(struct virtio_backend_info *info);

void dump_hex(void *base, int size);
void dump_desc(volatile struct vring_desc *desc, int idx, bool cond);

#endif
