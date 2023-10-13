/*-
 * Copyright (c) 2013  Chris Torek <torek @ torek net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/uio.h>
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <error.h>
#include <errno.h>

#include <linux/virtio_config.h>
#include <linux/virtio_console.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>

#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "timer.h"
#include <atomic.h>
#include "hsm_ioctl_defs.h"
#include "iothread.h"
#include "vmmapi.h"
#include <errno.h>
#include "vring_size.h"

#include "utils.h"

/*
 * Functions for dealing with generalized "virtual devices" as
 * defined by <https://www.google.com/#output=search&q=virtio+spec>
 */

/*
 * In case we decide to relax the "virtio struct comes at the
 * front of virtio-based device struct" constraint, let's use
 * this to convert.
 */
#define DEV_STRUCT(vs) ((void *)(vs))

static uint8_t virtio_poll_enabled;
static size_t virtio_poll_interval;

void
virtio_set_iothread(struct virtio_base *base __attribute__((unused)),
			  bool is_register __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
}

/**
 * @brief Link a virtio_base to its constants, the virtio device,
 * and the PCI emulation.
 *
 * @param base Pointer to struct virtio_base.
 * @param vops Pointer to struct virtio_ops.
 * @param pci_virtio_dev Pointer to instance of certain virtio device.
 * @param dev Pointer to struct pci_vdev which emulates a PCI device.
 * @param queues Pointer to struct virtio_vq_info, normally an array.
 *
 * @return None
 */
void
virtio_linkup(struct virtio_base *base, struct virtio_ops *vops,
	      void *pci_virtio_dev, struct pci_vdev *dev,
	      struct virtio_vq_info *queues,
	      int backend_type)
{
	int i;

	/* base and pci_virtio_dev addresses must match */
	if ((void *)base != pci_virtio_dev) {
		pr_err("virtio_base and pci_virtio_dev addresses don't match!\n");
		return;
	}
	base->vops = vops;
	base->dev = dev;
	dev->arg = base;
	base->backend_type = backend_type;

	base->queues = queues;
	for (i = 0; i < vops->nvq; i++) {
		queues[i].base = base;
		queues[i].num = i;
	}
}

/**
 * @brief Reset device (device-wide).
 *
 * This erases all queues, i.e., all the queues become invalid.
 * But we don't wipe out the internal pointers, by just clearing
 * the VQ_ALLOC flag.
 *
 * It resets negotiated features to "none".
 * If MSI-X is enabled, this also resets all the vectors to NO_VECTOR.
 *
 * @param base Pointer to struct virtio_base.
 *
 * @return None
 */
void
virtio_reset_dev(struct virtio_base *base)
{
	struct virtio_vq_info *vq;
	int i, nvq;

	base->polling_in_progress = 0;

	nvq = base->vops->nvq;
	for (vq = base->queues, i = 0; i < nvq; vq++, i++) {
		vq->flags = 0;
		vq->last_avail = 0;
		vq->save_used = 0;
		vq->pfn = 0;
		vq->msix_idx = VIRTIO_MSI_NO_VECTOR;
		vq->gpa_desc[0] = 0;
		vq->gpa_desc[1] = 0;
		vq->gpa_avail[0] = 0;
		vq->gpa_avail[1] = 0;
		vq->gpa_used[0] = 0;
		vq->gpa_used[1] = 0;
		vq->enabled = 0;
	}
	base->negotiated_caps = 0;
	base->curq = 0;
	/* base->status = 0; -- redundant */
	if (base->isr)
		pci_lintr_deassert(base->dev);
	base->isr = 0;
	base->msix_cfg_idx = VIRTIO_MSI_NO_VECTOR;
	base->device_feature_select = 0;
	base->driver_feature_select = 0;
	base->config_generation = 0;
}

/**
 * @brief Set I/O BAR (usually 0) to map PCI config registers.
 *
 * @param base Pointer to struct virtio_base.
 * @param barnum Which BAR[0..5] to use.
 *
 * @return None
 */
void
virtio_set_io_bar(struct virtio_base *base __attribute__((unused)), int barnum __attribute__((unused)))
{
}

/**
 * @brief Initialize MSI-X vector capabilities if we're to use MSI-X,
 * or MSI capabilities if not.
 *
 * We assume we want one MSI-X vector per queue, here, plus one
 * for the config vec.
 *
 *
 * @param base Pointer to struct virtio_base.
 * @param barnum Which BAR[0..5] to use.
 * @param use_msix If using MSI-X.
 *
 * @return 0 on success and non-zero on fail.
 */
int
virtio_intr_init(struct virtio_base *base, int barnum __attribute__((unused)), int use_msix __attribute__((unused)))
{
	int nvec;

	base->flags |= VIRTIO_USE_MSIX;
	VIRTIO_BASE_LOCK(base);
	virtio_reset_dev(base); /* set all vectors to NO_VECTOR */
	VIRTIO_BASE_UNLOCK(base);
	nvec = base->vops->nvq + 1;

	return 0;
}

int virtio_uses_msix(void)
{
	return 1;
}

/**
 * @brief Initialize MSI-X vector capabilities if we're to use MSI-X,
 * or MSI capabilities if not.
 *
 * Wrapper function for virtio_intr_init() for cases we directly use
 * BAR 1 for MSI-X capabilities.
 *
 * @param base Pointer to struct virtio_base.
 * @param use_msix If using MSI-X.
 *
 * @return 0 on success and non-zero on fail.
 */
int
virtio_interrupt_init(struct virtio_base *base, int use_msix)
{
	return virtio_intr_init(base, 1, use_msix);
}

/*
 * Initialize the currently-selected virtio queue (base->curq).
 * The guest just gave us the gpa of desc array, avail ring and
 * used ring, from which we can initialize the virtqueue.
 * This interface is only valid for virtio modern.
 */
static void
virtio_vq_enable(struct virtio_base *base)
{
	struct virtio_vq_info *vq;
	uint16_t qsz;
	uint64_t phys;
	size_t size;
	char *vb;

	vq = &base->queues[base->curq];
	qsz = vq->qsize;

	/* descriptors */
	phys = (((uint64_t)vq->gpa_desc[1]) << 32) | vq->gpa_desc[0];
	size = qsz * sizeof(struct vring_desc);
	vb = paddr_guest2host(base->dev->vmctx, phys, size);
	if (!vb)
		goto error;
	vq->desc = (struct vring_desc *)vb;

	/* available ring */
	phys = (((uint64_t)vq->gpa_avail[1]) << 32) | vq->gpa_avail[0];
	size = (2 + qsz + 1) * sizeof(uint16_t);
	vb = paddr_guest2host(base->dev->vmctx, phys, size);
	if (!vb)
		goto error;

	vq->avail = (struct vring_avail *)vb;

	/* used ring */
	phys = (((uint64_t)vq->gpa_used[1]) << 32) | vq->gpa_used[0];
	size = sizeof(uint16_t) * 3 + sizeof(struct vring_used_elem) * qsz;
	vb = paddr_guest2host(base->dev->vmctx, phys, size);
	if (!vb)
		goto error;
	vq->used = (struct vring_used *)vb;

	/* Start at 0 when we use it. */
	vq->last_avail = 0;
	vq->save_used = 0;

	/* Mark queue as enabled. */
	vq->enabled = true;

	/* Mark queue as allocated after initialization is complete. */
	mb();
	vq->flags = VQ_ALLOC;
	return;
 error:
	vq->flags = 0;
	pr_err("%s: vq enable failed\n", __func__);
}

/*
 * Helper inline for vq_getchain(): record the i'th "real"
 * descriptor.
 * Return 0 on success and -1 when i is out of range  or mapping
 *        fails.
 */
static inline int
_vq_record(int i, volatile struct vring_desc *vd, struct vmctx *ctx,
	   struct iovec *iov, int n_iov, uint16_t *flags) {

	void *host_addr;

	if (i >= n_iov)
		return -1;
	host_addr = paddr_guest2host(ctx, vd->addr, vd->len);
	if (!host_addr)
		return -1;
	iov[i].iov_base = host_addr;
	iov[i].iov_len = vd->len;
	if (flags != NULL)
		flags[i] = vd->flags;
	return 0;
}
#define	VQ_MAX_DESCRIPTORS	512	/* see below */

/*
 * Examine the chain of descriptors starting at the "next one" to
 * make sure that they describe a sensible request.  If so, return
 * the number of "real" descriptors that would be needed/used in
 * acting on this request.  This may be smaller than the number of
 * available descriptors, e.g., if there are two available but
 * they are two separate requests, this just returns 1.  Or, it
 * may be larger: if there are indirect descriptors involved,
 * there may only be one descriptor available but it may be an
 * indirect pointing to eight more.  We return 8 in this case,
 * i.e., we do not count the indirect descriptors, only the "real"
 * ones.
 *
 * Basically, this vets the flags and vd_next field of each
 * descriptor and tells you how many are involved.  Since some may
 * be indirect, this also needs the vmctx (in the pci_vdev
 * at base->dev) so that it can find indirect descriptors.
 *
 * As we process each descriptor, we copy and adjust it (guest to
 * host address wise, also using the vmtctx) into the given iov[]
 * array (of the given size).  If the array overflows, we stop
 * placing values into the array but keep processing descriptors,
 * up to VQ_MAX_DESCRIPTORS, before giving up and returning -1.
 * So you, the caller, must not assume that iov[] is as big as the
 * return value (you can process the same thing twice to allocate
 * a larger iov array if needed, or supply a zero length to find
 * out how much space is needed).
 *
 * If you want to verify the WRITE flag on each descriptor, pass a
 * non-NULL "flags" pointer to an array of "uint16_t" of the same size
 * as n_iov and we'll copy each flags field after unwinding any
 * indirects.
 *
 * If some descriptor(s) are invalid, this prints a diagnostic message
 * and returns -1.  If no descriptors are ready now it simply returns 0.
 *
 * You are assumed to have done a vq_ring_ready() if needed (note
 * that vq_has_descs() does one).
 */
int
vq_getchain(struct virtio_vq_info *vq, uint16_t *pidx,
	    struct iovec *iov, int n_iov, uint16_t *flags)
{
	int i;
	u_int ndesc, n_indir;
	u_int idx, next;

	volatile struct vring_desc *vdir, *vindir, *vp;
	struct vmctx *ctx;
	struct virtio_base *base;
	const char *name;

	base = vq->base;
	name = base->vops->name;

	/*
	 * Note: it's the responsibility of the guest not to
	 * update vq->avail->idx until all of the descriptors
	 * the guest has written are valid (including all their
	 * next fields and vd_flags).
	 *
	 * Compute (last_avail - idx) in integers mod 2**16.  This is
	 * the number of descriptors the device has made available
	 * since the last time we updated vq->last_avail.
	 *
	 * We just need to do the subtraction as an unsigned int,
	 * then trim off excess bits.
	 */
	idx = vq->last_avail;
	ndesc = (uint16_t)((u_int)vq->avail->idx - idx);
	if (ndesc == 0)
		return 0;
	if (ndesc > vq->qsize) {
		/* XXX need better way to diagnose issues */
		pr_err("%s: ndesc (%u) out of range, driver confused?\r\n",
		    name, (u_int)ndesc);
		return -1;
	}

	/*
	 * Now count/parse "involved" descriptors starting from
	 * the head of the chain.
	 *
	 * To prevent loops, we could be more complicated and
	 * check whether we're re-visiting a previously visited
	 * index, but we just abort if the count gets excessive.
	 */
	ctx = base->dev->vmctx;
	*pidx = next = vq->avail->ring[idx & (vq->qsize - 1)];
	vq->last_avail++;
	for (i = 0; i < VQ_MAX_DESCRIPTORS; next = vdir->next) {
		if (next >= vq->qsize) {
			pr_err("%s: descriptor index %u out of range, "
			    "driver confused?\r\n",
			    name, next);
			return -1;
		}
		vdir = &vq->desc[next];
		if ((vdir->flags & VRING_DESC_F_INDIRECT) == 0) {
			if (_vq_record(i, vdir, ctx, iov, n_iov, flags)) {
				pr_err("%s: mapping to host failed\r\n", name);
				return -1;
			}
			i++;
		} else if ((base->device_caps &
		    (1 << VIRTIO_RING_F_INDIRECT_DESC)) == 0) {
			pr_err("%s: descriptor has forbidden INDIRECT flag, "
			    "driver confused?\r\n",
			    name);
			return -1;
		} else {
			n_indir = vdir->len / 16;
			if ((vdir->len & 0xf) || n_indir == 0) {
				pr_err("%s: invalid indir len 0x%x, "
				    "driver confused?\r\n",
				    name, (u_int)vdir->len);
				return -1;
			}
			vindir = paddr_guest2host(ctx,
			    vdir->addr, vdir->len);

			if (!vindir) {
				pr_err("%s cannot get host memory\r\n", name);
				return -1;
			}
			/*
			 * Indirects start at the 0th, then follow
			 * their own embedded "next"s until those run
			 * out.  Each one's indirect flag must be off
			 * (we don't really have to check, could just
			 * ignore errors...).
			 */
			next = 0;
			for (;;) {
				vp = &vindir[next];
				if (vp->flags & VRING_DESC_F_INDIRECT) {
					pr_err("%s: indirect desc has INDIR flag,"
					    " driver confused?\r\n",
					    name);
					return -1;
				}
				if (_vq_record(i, vp, ctx, iov, n_iov, flags)) {
					pr_err("%s: mapping to host failed\r\n", name);
					return -1;
				}
				if (++i > VQ_MAX_DESCRIPTORS)
					goto loopy;
				if ((vp->flags & VRING_DESC_F_NEXT) == 0)
					break;
				next = vp->next;
				if (next >= n_indir) {
					pr_err("%s: invalid next %u > %u, "
					    "driver confused?\r\n",
					    name, (u_int)next, n_indir);
					return -1;
				}
			}
		}
		if ((vdir->flags & VRING_DESC_F_NEXT) == 0)
			return i;
	}
loopy:
	pr_err("%s: descriptor loop? count > %d - driver confused?\r\n",
	    name, i);
	return -1;
}

/*
 * Return the currently-first request chain back to the available queue.
 *
 * (This chain is the one you handled when you called vq_getchain()
 * and used its positive return value.)
 */
void
vq_retchain(struct virtio_vq_info *vq)
{
	vq->last_avail--;
}

/*
 * Return specified request chain to the guest, setting its I/O length
 * to the provided value.
 *
 * (This chain is the one you handled when you called vq_getchain()
 * and used its positive return value.)
 */
void
vq_relchain(struct virtio_vq_info *vq, uint16_t idx, uint32_t iolen)
{
	uint16_t uidx, mask;
	volatile struct vring_used *vuh;
	volatile struct vring_used_elem *vue;

	/*
	 * Notes:
	 *  - mask is N-1 where N is a power of 2 so computes x % N
	 *  - vuh points to the "used" data shared with guest
	 *  - vue points to the "used" ring entry we want to update
	 *  - head is the same value we compute in vq_iovecs().
	 *
	 * (I apologize for the two fields named idx; the
	 * virtio spec calls the one that vue points to, "id"...)
	 */
	mask = vq->qsize - 1;
	vuh = vq->used;

	uidx = vuh->idx;
	vue = &vuh->ring[uidx++ & mask];
	vue->id = idx;
	vue->len = iolen;
	vuh->idx = uidx;
}

/*
 * Driver has finished processing "available" chains and calling
 * vq_relchain on each one.  If driver used all the available
 * chains, used_all should be set.
 *
 * If the "used" index moved we may need to inform the guest, i.e.,
 * deliver an interrupt.  Even if the used index did NOT move we
 * may need to deliver an interrupt, if the avail ring is empty and
 * we are supposed to interrupt on empty.
 *
 * Note that used_all_avail is provided by the caller because it's
 * a snapshot of the ring state when he decided to finish interrupt
 * processing -- it's possible that descriptors became available after
 * that point.  (It's also typically a constant 1/True as well.)
 */
void
vq_endchains(struct virtio_vq_info *vq, int used_all_avail)
{
	struct virtio_base *base;
	uint16_t event_idx, new_idx, old_idx;
	int intr;

	if (!vq || !vq->used)
		return;

	/*
	 * Interrupt generation: if we're using EVENT_IDX,
	 * interrupt if we've crossed the event threshold.
	 * Otherwise interrupt is generated if we added "used" entries,
	 * but suppressed by VRING_AVAIL_F_NO_INTERRUPT.
	 *
	 * In any case, though, if NOTIFY_ON_EMPTY is set and the
	 * entire avail was processed, we need to interrupt always.
	 */

	atomic_thread_fence();

	base = vq->base;
	old_idx = vq->save_used;
	vq->save_used = new_idx = vq->used->idx;
	if (used_all_avail &&
	    (base->negotiated_caps & (1 << VIRTIO_F_NOTIFY_ON_EMPTY)))
		intr = 1;
	else if (base->negotiated_caps & (1 << VIRTIO_RING_F_EVENT_IDX)) {
		event_idx = VQ_USED_EVENT_IDX(vq);
		/*
		 * This calculation is per docs and the kernel
		 * (see src/sys/dev/virtio/virtio_ring.h).
		 */
		intr = (uint16_t)(new_idx - event_idx - 1) <
			(uint16_t)(new_idx - old_idx);
	} else {
		intr = new_idx != old_idx &&
		    !(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT);
	}
	if (intr)
		vq_interrupt(base, vq);
}

/**
 * @brief Helper function for clearing used ring flags.
 *
 * Driver should always use this helper function to clear used ring flags.
 * For virtio poll mode, in order to avoid trap, we should never really
 * clear used ring flags.
 *
 * @param base Pointer to struct virtio_base.
 * @param vq Pointer to struct virtio_vq_info.
 *
 * @return None
 */
void vq_clear_used_ring_flags(struct virtio_base *base, struct virtio_vq_info *vq)
{
	int backend_type = base->backend_type;
	int polling_in_progress = base->polling_in_progress;

	/* we should never unmask notification in polling mode */
	if (virtio_poll_enabled && backend_type == BACKEND_VBSU && polling_in_progress == 1)
		return;

	vq->used->flags &= ~VRING_USED_F_NO_NOTIFY;
}

struct config_reg {
	uint16_t	offset;	/* register offset */
	uint8_t		size;	/* size (bytes) */
	uint8_t		ro;	/* true => reg is read only */
	const char	*name;	/* name of reg */
};

/* Note: these are in sorted order to make for a fast search */
static struct config_reg modern_config_regs[] = {
	{ VIRTIO_PCI_COMMON_DFSELECT,		4, 0, "DFSELECT" },
	{ VIRTIO_PCI_COMMON_DF,			4, 1, "DF" },
	{ VIRTIO_PCI_COMMON_GFSELECT,		4, 0, "GFSELECT" },
	{ VIRTIO_PCI_COMMON_GF,			4, 0, "GF" },
	{ VIRTIO_PCI_COMMON_MSIX,		2, 0, "MSIX" },
	{ VIRTIO_PCI_COMMON_NUMQ,		2, 1, "NUMQ" },
	{ VIRTIO_PCI_COMMON_STATUS,		1, 0, "STATUS" },
	{ VIRTIO_PCI_COMMON_CFGGENERATION,	1, 1, "CFGGENERATION" },
	{ VIRTIO_PCI_COMMON_Q_SELECT,		2, 0, "Q_SELECT" },
	{ VIRTIO_PCI_COMMON_Q_SIZE,		2, 0, "Q_SIZE" },
	{ VIRTIO_PCI_COMMON_Q_MSIX,		2, 0, "Q_MSIX" },
	{ VIRTIO_PCI_COMMON_Q_ENABLE,		2, 0, "Q_ENABLE" },
	{ VIRTIO_PCI_COMMON_Q_NOFF,		2, 1, "Q_NOFF" },
	{ VIRTIO_PCI_COMMON_Q_DESCLO,		4, 0, "Q_DESCLO" },
	{ VIRTIO_PCI_COMMON_Q_DESCHI,		4, 0, "Q_DESCHI" },
	{ VIRTIO_PCI_COMMON_Q_AVAILLO,		4, 0, "Q_AVAILLO" },
	{ VIRTIO_PCI_COMMON_Q_AVAILHI,		4, 0, "Q_AVAILHI" },
	{ VIRTIO_PCI_COMMON_Q_USEDLO,		4, 0, "Q_USEDLO" },
	{ VIRTIO_PCI_COMMON_Q_USEDHI,		4, 0, "Q_USEDHI" },
};

static inline const struct config_reg *
virtio_find_cr(const struct config_reg *p_cr_array, u_int array_size,
	       int offset) {
	u_int hi, lo, mid;
	const struct config_reg *cr;

	lo = 0;
	hi = array_size - 1;
	while (hi >= lo) {
		mid = (hi + lo) >> 1;
		cr = p_cr_array + mid;
		if (cr->offset == offset)
			return cr;
		if (cr->offset < offset)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/**
 * @brief Set modern BAR (usually 4) to map PCI config registers.
 *
 * Set modern MMIO BAR (usually 4) to map virtio 1.0 capabilities and optional
 * set modern PIO BAR (usually 2) to map notify capability. This interface is
 * only valid for modern virtio.
 *
 * @param base Pointer to struct virtio_base.
 * @param use_notify_pio Whether use pio for notify capability.
 *
 * @return 0 on success and non-zero on fail.
 */
int
virtio_set_modern_bar(struct virtio_base *base, bool use_notify_pio __attribute__((unused)))
{
	struct virtio_ops *vops;
	int rc = 0;

	vops = base->vops;

	if (!vops || (base->device_caps & (1UL << VIRTIO_F_VERSION_1)) == 0)
		return -1;

	return rc;
}

static inline const struct config_reg *
virtio_find_modern_cr(int offset) {
        return virtio_find_cr(modern_config_regs,
                sizeof(modern_config_regs) / sizeof(*modern_config_regs),
                offset);
}

uint32_t
virtio_common_cfg_read(struct pci_vdev *dev, uint64_t offset, int size)
{
	struct virtio_base *base = dev->arg;
	struct virtio_ops *vops;
	const struct config_reg *cr;
	const char *name;
	uint32_t value;

	vops = base->vops;
	name = vops->name;
	value = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;

	cr = virtio_find_modern_cr(offset);
	if (cr == NULL || cr->size != size) {
		if (cr != NULL) {
			/* offset must be OK, so size must be bad */
			pr_err("%s: read from %s: bad size %d\r\n",
				name, cr->name, size);
		} else {
			pr_err("%s: read from bad offset/size %jd/%d\r\n",
				name, (uintmax_t)offset, size);
		}

		return value;
	}

	switch (offset) {
	case VIRTIO_PCI_COMMON_DFSELECT:
		value = base->device_feature_select;
		break;
	case VIRTIO_PCI_COMMON_DF:
		if (base->device_feature_select == 0)
			value = base->device_caps & 0xffffffff;
		else if (base->device_feature_select == 1)
			value = (base->device_caps >> 32) & 0xffffffff;
		else /* present 0, see 4.1.4.3.1 */
			value = 0;
		break;
	case VIRTIO_PCI_COMMON_GFSELECT:
		value = base->driver_feature_select;
		break;
	case VIRTIO_PCI_COMMON_GF:
		/* see 4.1.4.3.1. Present any valid feature bits the driver
		 * has written in driver_feature. Valid feature bits are those
		 * which are subset of the corresponding device_feature bits
		 */
		if (base->driver_feature_select == 0)
			value = base->negotiated_caps & 0xffffffff;
		else if (base->driver_feature_select == 1)
			value = (base->negotiated_caps >> 32) & 0xffffffff;
		else
			value = 0;
		break;
	case VIRTIO_PCI_COMMON_MSIX:
		value = base->msix_cfg_idx;
		break;
	case VIRTIO_PCI_COMMON_NUMQ:
		value = vops->nvq;
		break;
	case VIRTIO_PCI_COMMON_STATUS:
		value = base->status;
		break;
	case VIRTIO_PCI_COMMON_CFGGENERATION:
		value = base->config_generation;
		break;
	case VIRTIO_PCI_COMMON_Q_SELECT:
		value = base->curq;
		break;
	case VIRTIO_PCI_COMMON_Q_SIZE:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].qsize : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_MSIX:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].msix_idx :
			VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_PCI_COMMON_Q_ENABLE:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].enabled : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_NOFF:
		value = base->curq;
		break;
	case VIRTIO_PCI_COMMON_Q_DESCLO:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_desc[0] : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_DESCHI:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_desc[1] : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILLO:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_avail[0] : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILHI:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_avail[1] : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_USEDLO:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_used[0] : 0;
		break;
	case VIRTIO_PCI_COMMON_Q_USEDHI:
		value = base->curq < vops->nvq ?
			base->queues[base->curq].gpa_used[1] : 0;
		break;
	}

	pr_info("Read %s: 0x%x\n", cr->name, value);

	return value;
}

void
virtio_common_cfg_write(struct pci_vdev *dev, uint64_t offset, int size,
			uint64_t value)
{
	struct virtio_base *base = dev->arg;
	struct virtio_vq_info *vq;
	struct virtio_ops *vops;
	const struct config_reg *cr;
	const char *name;
	uint64_t features = 0;

	vops = base->vops;
	name = vops->name;

	cr = virtio_find_modern_cr(offset);
	if (cr == NULL || cr->size != size || cr->ro) {
		if (cr != NULL) {
			/* offset must be OK, wrong size and/or reg is R/O */
			if (cr->size != size)
				pr_err("%s: write to %s: bad size %d\r\n",
					name, cr->name, size);
			if (cr->ro)
				pr_err("%s: write to read-only reg %s\r\n",
					name, cr->name);
		} else {
			pr_err("%s: write to bad offset/size %jd/%d\r\n",
				name, (uintmax_t)offset, size);
		}

		return;
	}

	pr_info("Write %s: 0x%x\n", cr->name, value);

        switch (offset) {
	case VIRTIO_PCI_COMMON_DFSELECT:
		base->device_feature_select = value;
		break;
	case VIRTIO_PCI_COMMON_GFSELECT:
		base->driver_feature_select = value;
		break;
	case VIRTIO_PCI_COMMON_GF:
		if (base->status & VIRTIO_CONFIG_S_DRIVER_OK)
			break;
		if (base->driver_feature_select < 2) {
			value &= 0xffffffff;
			if (base->driver_feature_select == 0) {
				features = base->device_caps & value;
				base->negotiated_caps &= ~0xffffffffULL;
			} else {
				features = (value << 32)
					& base->device_caps;
				base->negotiated_caps &= 0xffffffffULL;
			}
			base->negotiated_caps |= features;
			if (vops->apply_features)
				(*vops->apply_features)(DEV_STRUCT(base),
					base->negotiated_caps);
		}
		break;
	case VIRTIO_PCI_COMMON_MSIX:
		base->msix_cfg_idx = value;
		break;
	case VIRTIO_PCI_COMMON_STATUS:
		base->status = value & 0xff;
		if (vops->set_status)
			(*vops->set_status)(DEV_STRUCT(base), value);
		if ((base->status == 0) && (vops->reset))
			(*vops->reset)(DEV_STRUCT(base));
		/* TODO: virtio poll mode for modern devices */
		break;
	case VIRTIO_PCI_COMMON_Q_SELECT:
		/*
		 * Note that the guest is allowed to select an
		 * invalid queue; we just need to return a QNUM
		 * of 0 while the bad queue is selected.
		 */
		base->curq = value;
		break;
	case VIRTIO_PCI_COMMON_Q_SIZE:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->qsize = value;
		break;
	case VIRTIO_PCI_COMMON_Q_MSIX:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->msix_idx = value;
		break;
	case VIRTIO_PCI_COMMON_Q_ENABLE:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		virtio_vq_enable(base);
		break;
	case VIRTIO_PCI_COMMON_Q_DESCLO:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_desc[0] = value;
		break;
	case VIRTIO_PCI_COMMON_Q_DESCHI:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_desc[1] = value;
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILLO:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_avail[0] = value;
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILHI:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_avail[1] = value;
		break;
	case VIRTIO_PCI_COMMON_Q_USEDLO:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_used[0] = value;
		break;
	case VIRTIO_PCI_COMMON_Q_USEDHI:
		if (base->curq >= vops->nvq)
			goto bad_qindex;
		vq = &base->queues[base->curq];
		vq->gpa_used[1] = value;
		break;
	}

	return;

bad_qindex:
	pr_err("%s: write config reg %s: curq %d >= max %d\r\n",
		name, cr->name, base->curq, vops->nvq);
}

/* ignore driver writes to ISR region, and only support ISR region read */
uint32_t
virtio_isr_cfg_read(struct pci_vdev *dev, uint64_t offset __attribute__((unused)), int size __attribute__((unused)))
{
	struct virtio_base *base = dev->arg;
	uint32_t value = 0;

	value = base->isr;
	base->isr = 0;		/* a read clears this flag */
	if (value)
		pci_lintr_deassert(dev);

	return value;
}

uint32_t
virtio_device_cfg_read(struct pci_vdev *dev, uint64_t offset, int size)
{
	struct virtio_base *base = dev->arg;
	struct virtio_ops *vops;
	const char *name;
	uint32_t value;
	uint64_t max;
	int error = -1;

	vops = base->vops;
	name = vops->name;
	value = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	max = vops->cfgsize ? vops->cfgsize : 0x100000000;

	if (offset + size > max) {
		pr_err("%s: reading from 0x%lx size %d exceeds limit\r\n",
			name, offset, size);
		return value;
	}

	if (vops->cfgread) {
		error = (*vops->cfgread)(DEV_STRUCT(base), offset, size, &value);
	}
	if (error) {
		pr_err("%s: reading from 0x%lx size %d failed %d\r\n",
			name, offset, size, error);
		value = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}

	return value;
}

void
virtio_device_cfg_write(struct pci_vdev *dev, uint64_t offset, int size,
			uint64_t value)
{
	struct virtio_base *base = dev->arg;
	struct virtio_ops *vops;
	const char *name;
	uint64_t max;
	int error = -1;

	vops = base->vops;
	name = vops->name;
	max = vops->cfgsize ? vops->cfgsize : 0x100000000;

	if (offset + size > max) {
		pr_err("%s: writing to 0x%lx size %d exceeds limit\r\n",
			name, offset, size);
		return;
	}

	if (vops->cfgwrite) {
		error = (*vops->cfgwrite)(DEV_STRUCT(base), offset, size, value);
	}
	if (error)
		pr_err("%s: writing ot 0x%lx size %d failed %d\r\n",
			name, offset, size, error);
}

/*
 * ignore driver reads from notify region, and only support notify region
 * write
 */
void
virtio_notify_cfg_write(struct pci_vdev *dev, uint64_t offset, int size __attribute__((unused)),
			uint64_t value __attribute__((unused)))
{
	struct virtio_base *base = dev->arg;
	struct virtio_vq_info *vq;
	struct virtio_ops *vops;
	const char *name;
	uint64_t idx;

	idx = offset / VIRTIO_MODERN_NOTIFY_OFF_MULT;
	vops = base->vops;
	name = vops->name;

	if (idx >= vops->nvq) {
		pr_err("%s: queue %lu notify out of range\r\n", name, idx);
		return;
	}

	vq = &base->queues[idx];
	if (vq->notify)
		(*vq->notify)(DEV_STRUCT(base), vq);
	else if (vops->qnotify)
		(*vops->qnotify)(DEV_STRUCT(base), vq);
	else
		pr_err("%s: qnotify queue %lu: missing vq/vops notify\r\n",
			name, idx);
}

/**
 * @brief Handle PCI configuration space reads.
 *
 * Handle virtio standard register reads, and dispatch other reads to
 * actual virtio device driver.
 *
 * @param ctx Pointer to struct vmctx representing VM context.
 * @param vcpu VCPU ID.
 * @param dev Pointer to struct pci_vdev which emulates a PCI device.
 * @param baridx Which BAR[0..5] to use.
 * @param offset Register offset in bytes within a BAR region.
 * @param size Access range in bytes.
 *
 * @return register value.
 */
uint64_t
virtio_pci_read(struct vmctx *ctx __attribute__((unused)), int vcpu __attribute__((unused)), struct pci_vdev *dev __attribute__((unused)),
		int baridx __attribute__((unused)), uint64_t offset __attribute__((unused)), int size)
{
	return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
}

/**
 * @brief Handle PCI configuration space writes.
 *
 * Handle virtio standard register writes, and dispatch other writes to
 * actual virtio device driver.
 *
 * @param ctx Pointer to struct vmctx representing VM context.
 * @param vcpu VCPU ID.
 * @param dev Pointer to struct pci_vdev which emulates a PCI device.
 * @param baridx Which BAR[0..5] to use.
 * @param offset Register offset in bytes within a BAR region.
 * @param size Access range in bytes.
 * @param value Data value to be written into register.
 *
 * @return None
 */
void
virtio_pci_write(struct vmctx *ctx __attribute__((unused)), int vcpu __attribute__((unused)), struct pci_vdev *dev __attribute__((unused)),
		 int baridx __attribute__((unused)), uint64_t offset __attribute__((unused)), int size __attribute__((unused)), uint64_t value __attribute__((unused)))
{
}

/**
 * @brief Get the virtio poll parameters
 *
 * @param optarg Pointer to parameters string.
 *
 * @return fail -1 success 0
 */
int
acrn_parse_virtio_poll_interval(const char *optarg)
{
	char *ptr;

	virtio_poll_interval = strtoul(optarg, &ptr, 0);

	/* poll interval is limited from 1us to 10ms */
	if (virtio_poll_interval < 1 || virtio_poll_interval > 10000000)
		return -1;

	return 0;
}

int virtio_register_ioeventfd(struct virtio_base *base __attribute__((unused)), int idx __attribute__((unused)), bool is_register __attribute__((unused)), int fd __attribute__((unused)))
{
	error(1, -ENOTSUP, "function %s is not expected to be used\n", __func__);
	return -1;
}
