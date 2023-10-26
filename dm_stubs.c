#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <error.h>
#include <pthread.h>

#include <vmmapi.h>
#include <pci_core.h>
#include <monitor.h>
#include <inout.h>
#include <mem.h>
#include <virtio.h>
#include <log.h>

int monitor_register_vm_ops(struct monitor_vm_ops *ops __attribute__((unused)), void *arg __attribute__((unused)), const char *name __attribute__((unused)))
{
	return 0;
}

int pci_emul_alloc_bar(struct pci_vdev *pdi __attribute__((unused)), int idx __attribute__((unused)), enum pcibar_type type __attribute__((unused)), uint64_t size __attribute__((unused)))
{
	return 0;
}

int pci_emul_add_capability(struct pci_vdev *dev __attribute__((unused)), u_char *capdata __attribute__((unused)), int caplen __attribute__((unused)))
{
	return 0;
}

struct pci_vdev* pci_get_vdev_info(int slot __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
	return NULL;
}

void
pci_generate_msi(struct pci_vdev *dev __attribute__((unused)), int index __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
}

void
pci_lintr_assert(struct pci_vdev *dev __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
}

void
pci_lintr_deassert(struct pci_vdev *dev __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
}

int
vm_irqfd(struct vmctx *ctx __attribute__((unused)), struct acrn_irqfd *args __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
	return -ENOTSUP;
}

int
vm_ioeventfd(struct vmctx *ctx __attribute__((unused)), struct acrn_ioeventfd *args __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
	return -ENOTSUP;
}

int virtio_set_modern_pio_bar(struct virtio_base *base __attribute__((unused)), int barnum __attribute__((unused)))
{
	return 0;
}

int vm_map_memseg_vma(struct vmctx *ctx __attribute__((unused)), size_t len __attribute__((unused)), vm_paddr_t gpa __attribute__((unused)), uint64_t vma __attribute__((unused)), int prot __attribute__((unused)))
{
	return 0;
}

int register_inout(struct inout_port *iop __attribute__((unused)))
{
	return 0;
}

int unregister_inout(struct inout_port *iop __attribute__((unused)))
{
	return 0;
}

int register_mem_fallback(struct mem_range *memp __attribute__((unused)))
{
	return 0;
}

int unregister_mem_fallback(struct mem_range *memp __attribute__((unused)))
{
	return 0;
}
