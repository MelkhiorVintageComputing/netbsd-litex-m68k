/*      $NetBSD: bootinfo.c,v 1.9 2025/06/09 21:19:49 andvar Exp $        */

/*-
 * Copyright (c) 2023 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: bootinfo.c,v 1.9 2025/06/09 21:19:49 andvar Exp $");

#include "opt_md.h"

#include <sys/types.h>
#include <sys/cpu.h>
#include <sys/rnd.h>
#include <sys/rndsource.h>

#include <uvm/uvm_extern.h>

#ifdef MEMORY_DISK_DYNAMIC
#include <dev/md.h>
#endif

#include <machine/bootinfo.h>
#include <machine/vmparam.h>

#include "liteuart.h"
#if NLITEUART > 0
#include <litex68k/dev/liteuartvar.h>
#endif

struct bi_record *	bootinfo;
vaddr_t			bootinfo_end;
uint32_t		bootinfo_machtype;
int			bootinfo_mem_segments_ignored;
size_t			bootinfo_mem_segments_ignored_bytes;
struct bi_mem_info	bootinfo_mem_segments[VM_PHYSSEG_MAX];
struct bi_mem_info	bootinfo_mem_segments_avail[VM_PHYSSEG_MAX];
int			bootinfo_mem_nsegments;
int			bootinfo_mem_nsegments_avail;

static paddr_t		bootinfo_console_addr;
static bool		bootinfo_console_addr_valid;

static uint32_t		bootinfo_initrd_start;
static uint32_t		bootinfo_initrd_size;

static paddr_t		bootinfo_dtb_start;
static psize_t		bootinfo_dtb_size;

#if NLITEUART > 0
static bool
bootinfo_set_console(paddr_t pa)
{
	if (! bootinfo_console_addr_valid) {
		bootinfo_console_addr = pa;
		bootinfo_console_addr_valid = true;
		return true;
	}
	return false;
}
#endif

static inline struct bi_record *
bootinfo_next(struct bi_record *bi)
{
	uintptr_t addr = (uintptr_t)bi;

	addr += bi->bi_size;
	return (struct bi_record *)addr;
}

static inline int
bootinfo_get_cpu(struct bi_record *bi)
{
	switch (bootinfo_get_u32(bi)) {
	case BI_CPU_68020:	return CPU_68020;
	case BI_CPU_68030:	return CPU_68030;
	case BI_CPU_68040:	return CPU_68040;
	case BI_CPU_68060:	return CPU_68060;
	default:		return -666;
	}
}

static inline int
bootinfo_get_fpu(struct bi_record *bi)
{
	switch (bootinfo_get_u32(bi)) {
	case BI_FPU_68881:	return FPU_68881;
	case BI_FPU_68882:	return FPU_68882;
	case BI_FPU_68040:	return FPU_68040;
	case BI_FPU_68060:	return FPU_68060;
	case 0:			return FPU_NONE;	/* litexboot's FNOP probe */
	default:		return FPU_UNKNOWN;
	}
}

static inline int
bootinfo_get_mmu(struct bi_record *bi)
{
	switch (bootinfo_get_u32(bi)) {
	case BI_MMU_68851:	return MMU_68851;
	case BI_MMU_68030:	return MMU_68030;
	case BI_MMU_68040:	return MMU_68040;
	case BI_MMU_68060:	return MMU_68040;	/* XXX */
	case BI_MMU_SUN3:	return MMU_SUN;
	case BI_MMU_APOLLO:	/* XXX MMU_HP ??? */
	case BI_MMU_COLDFIRE:
	default:		return FPU_UNKNOWN;
	}
}

static inline void
bootinfo_add_mem(struct bi_record *bi)
{
	struct bi_mem_info *m = bootinfo_dataptr(bi);

	if (bootinfo_mem_nsegments == VM_PHYSSEG_MAX) {
		bootinfo_mem_segments_ignored++;
		bootinfo_mem_segments_ignored_bytes += m->mem_size;
	}

	/*
	 * Make sure the start / size are properly aligned.
	 */
	if (m->mem_addr & PGOFSET) {
		m->mem_size -= m->mem_addr & PGOFSET;
		m->mem_addr = m68k_round_page(m->mem_addr);
	}
	m->mem_size = m68k_trunc_page(m->mem_size);
	physmem += m->mem_size >> PGSHIFT;

	bootinfo_mem_segments[bootinfo_mem_nsegments++] = *m;
	bootinfo_mem_segments_avail[bootinfo_mem_nsegments_avail++] = *m;
}

static inline void
bootinfo_add_initrd(struct bi_record *bi)
{
	struct bi_mem_info *rd = bootinfo_dataptr(bi);

	if (bootinfo_initrd_size == 0) {
		bootinfo_initrd_start = rd->mem_addr;
		bootinfo_initrd_size  = rd->mem_size;
	}
}

/*
 * bootinfo_reserve --
 *	Take [start, end) out of the available memory segments: trim a
 *	segment it touches an end of, or split one it falls inside.
 */
static void
bootinfo_reserve(const char *what, paddr_t start, paddr_t end)
{
	int i;

	start = m68k_trunc_page(start);
	end = m68k_round_page(end);

	for (i = 0; i < bootinfo_mem_nsegments_avail; i++) {
		/* Memory segment start/end already page-aligned. */
		paddr_t seg_start = bootinfo_mem_segments_avail[i].mem_addr;
		paddr_t seg_end = seg_start +
		    bootinfo_mem_segments_avail[i].mem_size;

		if (start >= seg_end || end <= seg_start) {
			/* Does not fall within this segment. */
			continue;
		}

		printf("Reserving %s pages %p - %p from memory "
		       "segment %d.\n", what, (void *)start,
		       (void *)(end - 1), i);

		if (start > seg_start && end < seg_end) {
			/* Split it: the part above becomes a new segment. */
			if (bootinfo_mem_nsegments_avail == VM_PHYSSEG_MAX) {
				printf("WARNING: no segment left to split "
				    "around the %s; losing the memory "
				    "above it.\n", what);
			} else {
				struct bi_mem_info *n = &bootinfo_mem_segments_avail[
				    bootinfo_mem_nsegments_avail++];
				n->mem_addr = end;
				n->mem_size = seg_end - end;
			}
			seg_end = start;
		} else {
			if (start <= seg_start) {
				seg_start = MIN(end, seg_end);
			}
			if (end >= seg_end) {
				seg_end = MAX(start, seg_start);
			}
		}

		/* Now adjust the segment. */
		bootinfo_mem_segments_avail[i].mem_addr = seg_start;
		bootinfo_mem_segments_avail[i].mem_size = seg_end - seg_start;
		return;
	}
}

static inline void
bootinfo_add_dtb(struct bi_record *bi)
{
	struct bi_mem_info *m = bootinfo_dataptr(bi);

	bootinfo_dtb_start = m->mem_addr;
	bootinfo_dtb_size = m->mem_size;
}

static inline void
bootinfo_uart_consinit(struct bi_record *bi)
{
#if NLITEUART > 0
	struct bi_litex_dev *ld = bootinfo_dataptr(bi);

	/*
	 * ld_base is the PA; the CSRs are mapped VA==PA by TT0, which
	 * locore has already turned on.
	 */
	if (bootinfo_set_console(ld->ld_base)) {
		bootinfo_md_cnattach(liteuart_cnattach,
		    ld->ld_base, LITEUART_SIZE);
		printf("Initialized LiteUART console @ 0x%08x\n",
		    ld->ld_base);
	}
#endif /* NLITEUART > 0 */
}

/*
 * bootinfo_start --
 *	Parse the boot info during early start-up.
 */
void
bootinfo_start(struct bi_record *first)
{
	struct bi_record *bi;

	bootinfo = first;

	for (bi = bootinfo; bi->bi_tag != BI_LAST; bi = bootinfo_next(bi)) {
		switch (bi->bi_tag) {
		case BI_MACHTYPE:
			bootinfo_machtype = bootinfo_get_u32(bi);
			break;

		case BI_CPUTYPE:
			cputype = bootinfo_get_cpu(bi);
			break;

		case BI_FPUTYPE:
			fputype = bootinfo_get_fpu(bi);
			break;

		case BI_MMUTYPE:
			mmutype = bootinfo_get_mmu(bi);
			break;

		case BI_MEMCHUNK:
			bootinfo_add_mem(bi);
			break;

		case BI_RAMDISK:
			bootinfo_add_initrd(bi);
			break;

		case BI_LITEX_UART:
			bootinfo_uart_consinit(bi);
			break;

		case BI_LITEX_DTB:
			bootinfo_add_dtb(bi);
			break;

		default:
			break;
		}
	}

	/* Set bootinfo_end to be just past the BI_LAST record. */
	bootinfo_end = (vaddr_t)bootinfo_next(bi);

	/*
	 * If we have a RAM disk or a device tree, we need to take them
	 * out of the available memory segments.
	 */
	if (bootinfo_initrd_size != 0) {
		bootinfo_reserve("RAM disk", bootinfo_initrd_start,
		    bootinfo_initrd_start + bootinfo_initrd_size);
	}
	if (bootinfo_dtb_size != 0) {
		bootinfo_reserve("device tree", bootinfo_dtb_start,
		    bootinfo_dtb_start + bootinfo_dtb_size);
	}
}

/*
 * bootinfo_enumerate --
 *	Enumerate through the boot info, invoking the specified callback
 *	for each record.  The callback returns true to keep searching,
 *	false, to stop.
 */
void
bootinfo_enumerate(bool (*cb)(struct bi_record *, void *), void *ctx)
{
	struct bi_record *bi = bootinfo;

	if (bi == NULL) {
		return;
	}

	for (; bi->bi_tag != BI_LAST; bi = bootinfo_next(bi)) {
		if ((*cb)(bi, ctx) == false) {
			break;
		}
	}
}

struct bootinfo_find_ctx {
	uint32_t tag;
	struct bi_record *result;
};

static bool
bootinfo_find_cb(struct bi_record *bi, void *v)
{
	struct bootinfo_find_ctx *ctx = v;

	if (bi->bi_tag == ctx->tag) {
		ctx->result = bi;
		return false;
	}

	return true;
}

/*
 * bootinfo_find --
 *	Scan through the boot info looking for the first instance of
 *	the specified tag.
 */
struct bi_record *
bootinfo_find(uint32_t tag)
{
	struct bootinfo_find_ctx ctx = {
		.tag = tag,
	};

	bootinfo_enumerate(bootinfo_find_cb, &ctx);
	return ctx.result;
}

/*
 * bootinfo_litex_dev --
 *	Find a LiteX core's record (BI_LITEX_*): its CSR base and line.
 */
struct bi_litex_dev *
bootinfo_litex_dev(uint32_t tag)
{
	struct bi_record *bi = bootinfo_find(tag);

	return bi != NULL ? bootinfo_dataptr(bi) : NULL;
}

/*
 * bootinfo_sys_clk --
 *	The frequency the SoC's timers count at, in Hz.
 */
uint32_t
bootinfo_sys_clk(void)
{
	struct bi_record *bi = bootinfo_find(BI_LITEX_SYS_CLK);

	return bi != NULL ? bootinfo_get_u32(bi) : 0;
}

/*
 * bootinfo_dtb --
 *	The flattened device tree litexboot passed, if any (its pages
 *	are kept out of the VM system).
 */
bool
bootinfo_dtb(paddr_t *pap, psize_t *sizep)
{
	if (bootinfo_dtb_size == 0) {
		return false;
	}
	*pap = bootinfo_dtb_start;
	*sizep = bootinfo_dtb_size;
	return true;
}

/*
 * bootinfo_addr_is_console --
 *	Tests to see if the device at the specified address is
 *	the console device.
 */
bool
bootinfo_addr_is_console(paddr_t pa)
{
	return bootinfo_console_addr_valid && bootinfo_console_addr == pa;
}

/*
 * bootinfo_setup_initrd --
 *	Check for a BI_RAMDISK record and, if found, set it as
 *	the root file system.
 */
void
bootinfo_setup_initrd(void)
{
#ifdef MEMORY_DISK_DYNAMIC
	if (bootinfo_initrd_size != 0) {
		paddr_t rdstart, rdend, rdpgoff;
		vaddr_t rdva, rdoff;
		vsize_t rdvsize;

		printf("Initializing root RAM disk @ %p - %p\n",
		    (void *)bootinfo_initrd_start,
		    (void *)(bootinfo_initrd_start + bootinfo_initrd_size - 1));

		rdend = m68k_round_page(bootinfo_initrd_start +
		    bootinfo_initrd_size);
		rdstart = m68k_trunc_page(bootinfo_initrd_start);
		rdvsize = rdend - rdstart;
		rdpgoff = bootinfo_initrd_start & PAGE_MASK;

		rdva = uvm_km_alloc(kernel_map, rdvsize, PAGE_SIZE,
		    UVM_KMF_VAONLY);
		if (rdva == 0) {
			printf("WARNING: Unable to allocate KVA for "
			       "RAM disk.\n");
			return;
		}
		for (rdoff = 0; rdoff < rdvsize; rdoff += PAGE_SIZE) {
			pmap_kenter_pa(rdva + rdoff, rdstart + rdoff,
			    VM_PROT_READ | VM_PROT_WRITE, 0);
		}
		md_root_setconf((void *)(rdva + rdpgoff),
		    bootinfo_initrd_size);
	}
#endif /* MEMORY_DISK_DYNAMIC */
}

/*
 * bootinfo_setup_rndseed --
 *	Check for a BI_RNG_SEED record and, if found, use it to
 *	seed the kernel entropy pool.
 */
void
bootinfo_setup_rndseed(void)
{
	static struct krndsource bootinfo_rndsource;
	struct bi_record *bi = bootinfo_find(BI_RNG_SEED);
	if (bi != NULL) {
		struct bi_data *rnd = bootinfo_dataptr(bi);
		rnd_attach_source(&bootinfo_rndsource, "bootinfo",
		    RND_TYPE_RNG, RND_FLAG_DEFAULT);
		rnd_add_data(&bootinfo_rndsource,
		    rnd->data_bytes, rnd->data_length,
		    rnd->data_length * NBBY);
		explicit_memset(rnd->data_bytes, 0, rnd->data_length);
	}
}

/*
 * bootinfo_getarg --
 *	Get an argument from the BI_COMMAND_LINE bootinfo record.
 */
bool
bootinfo_getarg(const char *var, char *buf, size_t buflen)
{
	const size_t varlen = strlen(var);
	struct bi_record *bi = bootinfo_find(BI_COMMAND_LINE);

	if (bi == NULL) {
		return false;
	}

	const char *sp = bootinfo_dataptr(bi);
	const char *osp = sp;
	for (;;) {
		sp = strstr(sp, var);
		if (sp == NULL) {
			return false;
		}

		if (sp != osp &&
		    sp[-1] != ' ' && sp[-1] != '\t' && sp[-1] != '-') {
			continue;
		}
		sp += varlen;
		char ch = *sp++;
		if (ch != '=' && ch != ' ' && ch != '\t' && ch != '\0') {
			continue;
		}
		/* Found it. */
		break;
	}

	while (--buflen) {
		if (*sp == ' ' || *sp == '\t' || *sp == '\0') {
			break;
		}
		*buf++ = *sp++;
	}
	*buf = '\0';

	return true;
}
