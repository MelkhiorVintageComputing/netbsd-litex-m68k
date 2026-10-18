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
 * MULTIPROCESSOR support for virt68k: ASTs, rescheduling and IPIs.
 * The inter-processor interrupts are provided by virtsmp(4); without it
 * (a single CPU) there is never another CPU to interrupt.
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

#include <virt68k/virt68k/cpu_mp.h>

/* Set by virtsmp(4) when it attaches. */
void	(*virt68k_ipi_send)(struct cpu_info *, u_int);

static void
cpu_send_ipi(struct cpu_info *ci, u_int ipi)
{
	KASSERT(ci != curcpu());
	if (virt68k_ipi_send != NULL)
		(*virt68k_ipi_send)(ci, ipi);
}

void
m68k_send_tlb_ipi(struct cpu_info *ci)
{
	cpu_send_ipi(ci, VIRT68K_IPI_TLB);
}

void
cpu_set_ast(struct lwp *l)
{
	struct cpu_info *ci = l->l_cpu;

	ci->ci_astpending = 1;
	if (ci != curcpu())
		cpu_send_ipi(ci, VIRT68K_IPI_AST);
}

void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int flags)
{
	/* There is no kernel preemption: always an AST on return. */
	ci->ci_astpending = 1;
	if ((flags & RESCHED_REMOTE) != 0)
		cpu_send_ipi(ci, VIRT68K_IPI_AST);
}

void
xc_send_ipi(struct cpu_info *ci)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *t;

	if (ci != NULL) {
		cpu_send_ipi(ci, VIRT68K_IPI_XCALL);
		return;
	}
	for (CPU_INFO_FOREACH(cii, t)) {
		if (t != curcpu())
			cpu_send_ipi(t, VIRT68K_IPI_XCALL);
	}
}

void
cpu_ipi(struct cpu_info *ci)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *t;

	if (ci != NULL) {
		cpu_send_ipi(ci, VIRT68K_IPI_GENERIC);
		return;
	}
	for (CPU_INFO_FOREACH(cii, t)) {
		if (t != curcpu())
			cpu_send_ipi(t, VIRT68K_IPI_GENERIC);
	}
}

/*
 * Handle the IPIs whose bits are set in "pending", on the current CPU,
 * at IPL_SCHED.  Called by virtsmp(4).
 */
void
virt68k_ipi_dispatch(u_int pending)
{
	if (pending & (1u << VIRT68K_IPI_TLB))
		pmap_tlb_shootdown_ipi();
	if (pending & (1u << VIRT68K_IPI_AST)) {
		/* curcpu()->ci_astpending was set by the sender */
	}
	if (pending & (1u << VIRT68K_IPI_XCALL))
		xc_ipi_handler();
	if (pending & (1u << VIRT68K_IPI_GENERIC))
		ipi_cpu_handler();
	if (pending & (1u << VIRT68K_IPI_HALT)) {
		/* XXX tell the boot CPU */
		for (;;)
			__asm volatile("stop #0x2700");
	}
}

void
cpu_boot_secondary_processors(void)
{
	pmap_mp_attach_evcnt();
	/* virtsmp(4) starts them; nothing without it */
	if (virt68k_boot_secondaries != NULL)
		(*virt68k_boot_secondaries)();
}

/* Set by virtsmp(4) when it attaches. */
void	(*virt68k_boot_secondaries)(void);
