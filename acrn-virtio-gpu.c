#include <pci_core.h>
#include <vdisplay.h>

#include "utils.h"

extern struct pci_vdev_ops pci_ops_virtio_gpu;

static void init_vdpy(struct virtio_backend_info *info __attribute__((unused))) {
//     vdpy_parse_cmd_option(info->opts);
}

static struct virtio_backend_info virtio_gpu_info = {
       .pci_vdev_ops = &pci_ops_virtio_gpu,

       .hook_before_init = init_vdpy,
};

ACRN_BACKEND_MAIN(virtio_gpu_info)

