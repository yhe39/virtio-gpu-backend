#ifndef __BACKENDS_IO_H__
#define __BACKENDS_IO_H__

#include <stdint.h>

static inline uint32_t mmio_read32(void *address)
{
	return *(volatile uint32_t *)address;
}

static inline void mmio_write32(void *address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

#endif  /* __BACKENDS_IO_H__ */
