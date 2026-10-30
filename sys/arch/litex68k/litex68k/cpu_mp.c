/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
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

/*
 * MULTIPROCESSOR support for litex68k: ASTs, rescheduling and IPIs.
 * The inter-processor interrupts are provided by litexsmp(4); without it
 * (a single CPU) there is never another CPU to interrupt.
 * (From virt68k's cpu_mp.c, virt68k-smp.)
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/cpu.h>
#include <sys/ipi.h>
#include <sys/lwp.h>
#include <sys/xcall.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/intr.h>
#include <machine/pmap.h>

#ifndef M68K_PMAP_WRITETHROUGH
#error "the board's snoop control needs options M68K_PMAP_WRITETHROUGH"
#endif

#include <litex68k/litex68k/cpu_mp.h>

static void	litex68k_dcinv_intr(void);

/* Set by litexsmp(4) when it attaches. */
void	(*litex68k_ipi_send)(struct cpu_info *, u_int);

static void
cpu_send_ipi(struct cpu_info *ci, u_int ipi)
{
	KASSERT(ci != curcpu());
	if (litex68k_ipi_send != NULL)
		(*litex68k_ipi_send)(ci, ipi);
}

void
m68k_send_tlb_ipi(struct cpu_info *ci)
{
	cpu_send_ipi(ci, LITEX68K_IPI_TLB);
}

void
cpu_set_ast(struct lwp *l)
{
	struct cpu_info *ci = l->l_cpu;

	ci->ci_astpending = 1;
	if (ci != curcpu())
		cpu_send_ipi(ci, LITEX68K_IPI_AST);
}

void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int flags)
{
	/* There is no kernel preemption: always an AST on return. */
	ci->ci_astpending = 1;
	if ((flags & RESCHED_REMOTE) != 0)
		cpu_send_ipi(ci, LITEX68K_IPI_AST);
}

void
xc_send_ipi(struct cpu_info *ci)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *t;

	if (ci != NULL) {
		cpu_send_ipi(ci, LITEX68K_IPI_XCALL);
		return;
	}
	for (CPU_INFO_FOREACH(cii, t)) {
		if (t != curcpu())
			cpu_send_ipi(t, LITEX68K_IPI_XCALL);
	}
}

void
cpu_ipi(struct cpu_info *ci)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *t;

	if (ci != NULL) {
		cpu_send_ipi(ci, LITEX68K_IPI_GENERIC);
		return;
	}
	for (CPU_INFO_FOREACH(cii, t)) {
		if (t != curcpu())
			cpu_send_ipi(t, LITEX68K_IPI_GENERIC);
	}
}

/*
 * Handle the IPIs whose bits are set in "pending", on the current CPU,
 * at IPL_SCHED.  Called by litexsmp(4).
 */
void
litex68k_ipi_dispatch(u_int pending)
{
	if (pending & (1u << LITEX68K_IPI_TLB))
		pmap_tlb_shootdown_ipi();
	if (pending & (1u << LITEX68K_IPI_DCINV))
		litex68k_dcinv_intr();
	if (pending & (1u << LITEX68K_IPI_AST)) {
		/* curcpu()->ci_astpending was set by the sender */
	}
	if (pending & (1u << LITEX68K_IPI_XCALL))
		xc_ipi_handler();
	if (pending & (1u << LITEX68K_IPI_GENERIC))
		ipi_cpu_handler();
	if (pending & (1u << LITEX68K_IPI_HALT)) {
		/* XXX tell the boot CPU */
		for (;;)
			__asm volatile("stop #0x2700");
	}
}

/*
 * A device's DMA writes memory from LiteX's side, not over the 68040
 * bus, so no cache snoops it: before the data is used (or run: a
 * program read in), the other CPUs drop their copies of every line, in
 * both caches (the transfer's own CPU purges its lines in
 * bus_dmamap_sync()).  Invalidating a whole data cache loses nothing:
 * with M68K_PMAP_WRITETHROUGH no line is ever dirty.
 * A request is done on a CPU once its dcinv_done has reached it.
 */
static volatile u_int dcinv_req;
static volatile u_int dcinv_done[MAXCPUS];

static void
litex68k_dcinv_intr(void)
{
	const u_int req = dcinv_req;	/* before: covers it and older */

	__asm volatile(".word 0xf4d8");	/* cinva bc */
	dcinv_done[curcpu()->ci_cpuid] = req;
}

void
litex68k_dcinv_others(void)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci, *self;
	u_int req;
	int s;

	if (ncpu == 1)
		return;

	/* The other CPUs' requests must reach us while we wait. */
	s = splvm();
	KASSERT((s & PSL_IPL) < (ipl2psl_table[IPL_SCHED] & PSL_IPL));
	self = curcpu();
	req = atomic_inc_uint_nv(&dcinv_req);
	for (CPU_INFO_FOREACH(cii, ci)) {
		if (ci != self &&
		    (ci->ci_schedstate.spc_flags & SPCF_RUNNING) != 0)
			cpu_send_ipi(ci, LITEX68K_IPI_DCINV);
	}
	for (CPU_INFO_FOREACH(cii, ci)) {
		if (ci == self ||
		    (ci->ci_schedstate.spc_flags & SPCF_RUNNING) == 0)
			continue;
		while ((int)(dcinv_done[ci->ci_cpuid] - req) < 0)
			continue;
	}
	splx(s);
}

void
cpu_boot_secondary_processors(void)
{
	pmap_mp_attach_evcnt();
	/* litexsmp(4) starts them; nothing without it */
	if (litex68k_boot_secondaries != NULL)
		(*litex68k_boot_secondaries)();
}

/* Set by litexsmp(4) when it attaches. */
void	(*litex68k_boot_secondaries)(void);

/* Set by litexsmp(4) if the CPUs have no timer of their own. */
void	(*litex68k_clock_ipi)(void);

/* Set by litexsmp(4): puts the other CPUs back in reset, at halt. */
void	(*litex68k_hold_others)(void);
