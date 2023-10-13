#include <stdint.h>
#include <linux/types.h>
#include <linux/virtio_types.h>

static __inline__ unsigned vring_size(unsigned int num, unsigned long align)
{
        return ((sizeof(struct vring_desc) * num + sizeof(__virtio16) * (3 + num)
                 + align - 1) & ~(align - 1))
                + sizeof(__virtio16) * 3 + sizeof(struct vring_used_elem) * num;
}

