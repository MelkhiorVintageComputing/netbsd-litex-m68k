/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 Romain Dolbeau
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * litexsmp: the SMP block of LiteX-68K's two-processor builds (two
 * RD68LC041s on one MC68040 bus, litex-68k's doc/l18-smp.md), found
 * through litexboot's BI_LITEX_SMP record.  Its registers name the CPU
 * reading them (WHOAMI), interrupt a CPU at level 5 (IPI_SET, IPI_ACK)
 * and start CPU 1, which is held in reset until RELEASE: the ROM's crt0
 * then jumps to BOOT_PC with BOOT_SP, the MMU and the caches off.
 *
 * An IPI is a latch per CPU with no payload, so its reasons are in a
 * mailbox in memory (litexsmp_mbox[]): the sender sets them, then the
 * latch; the handler acknowledges the latch, then takes the reasons, so
 * that none set after it looked is lost.
 *
 * CPU 1 ticks on Timer1 (BI_LITEX_TIMER1), at level 2 through its own
 * interrupt mask.  Without it, CPU 0 sends it an IPI on every tick of
 * Timer0.
 *
 * Partly from virt68k's virtsmp.c (virt68k-smp).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_multiprocessor.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/idle.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/lwp.h>

#include <uvm/uvm_extern.h>

#include <machine/bootinfo.h>
#include <machine/cpu.h>
#include <machine/intr.h>
#include <machine/pcb.h>
#include <machine/vectors.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/dev/litexsmpreg.h>
#include <litex68k/dev/litextimerreg.h>
#include <litex68k/litex68k/cpu_mp.h>

struct litexsmp_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	u_int			sc_ncpus;

	/* CPU 1's timer, if the SoC has one */
	bus_space_handle_t	sc_tbsh;
	bool			sc_timer1;
	void			*sc_tih;
	struct evcnt		sc_tev;
};

static struct litexsmp_softc *litexsmp_sc;

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,rd68041-smp" },
	DEVICE_COMPAT_EOL
};

#define	REG_READ(sc, r)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define	REG_WRITE(sc, r, v)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))
#define	TREG_WRITE(sc, r, v)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_tbsh, (r), (v))
#define	TREG_READ(sc, r)	\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_tbsh, (r))

#ifdef MULTIPROCESSOR
extern char cpu_spinup_trampoline[];

/* By CPU number (WHOAMI): the IPI reasons, LITEX68K_IPI_* bits. */
static volatile uint32_t litexsmp_mbox[MAXCPUS];

static struct litex68k_hatch litexsmp_hatch;
static volatile u_int litexsmp_hatched;

static void
litexsmp_ipi_send(struct cpu_info *ci, u_int ipi)
{
	struct litexsmp_softc * const sc = litexsmp_sc;

	atomic_or_32(&litexsmp_mbox[ci->ci_cpuid], 1u << ipi);
	REG_WRITE(sc, LITEXSMP_IPI_SET, ci->ci_cpuid);
}

/* Level 5, from intr_dispatch(). */
static void
litexsmp_ipi_intr(struct clockframe *frame)
{
	struct litexsmp_softc * const sc = litexsmp_sc;
	struct cpu_info * const ci = curcpu();
	uint32_t pending;

	REG_WRITE(sc, LITEXSMP_IPI_ACK, 0);
	pending = atomic_swap_32(&litexsmp_mbox[ci->ci_cpuid], 0);
	if (pending & (1u << LITEX68K_IPI_CLOCK))
		hardclock(frame);
	litex68k_ipi_dispatch(pending & ~(1u << LITEX68K_IPI_CLOCK));
}

/* Timer0's tick on CPU 0, when CPU 1 has no timer of its own. */
static void
litexsmp_clock_ipi(void)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;

	for (CPU_INFO_FOREACH(cii, ci)) {
		if (ci != curcpu() &&
		    (ci->ci_schedstate.spc_flags & SPCF_RUNNING) != 0)
			litexsmp_ipi_send(ci, LITEX68K_IPI_CLOCK);
	}
}

/* Timer1, on CPU 1 only (its mask). */
static int
litexsmp_timer1_intr(void *v)
{
	struct litexsmp_softc * const sc = litexsmp_sc;

	if ((TREG_READ(sc, TIMER_EV_PENDING) & TIMER_EV_ZERO) == 0)
		return 0;
	TREG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);
	sc->sc_tev.ev_count++;
	hardclock((struct clockframe *)v);
	return 1;
}

static void
litexsmp_start_timer1(struct litexsmp_softc *sc)
{
	uint32_t period;

	period = bootinfo_sys_clk() / hz;
	TREG_WRITE(sc, TIMER_EN, 0);
	TREG_WRITE(sc, TIMER_LOAD, 0);
	TREG_WRITE(sc, TIMER_RELOAD, period - 1);
	TREG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);
	TREG_WRITE(sc, TIMER_EV_ENABLE, TIMER_EV_ZERO);
	TREG_WRITE(sc, TIMER_EN, 1);
}

/* CPU 1, from cpu_spinup_trampoline (locore.s), mapped, at IPL 7. */
void	cpu_hatch(struct cpu_info *);
void
cpu_hatch(struct cpu_info *ci)
{
	struct litexsmp_softc * const sc = litexsmp_sc;
	struct lwp *l = ci->ci_data.cpu_idlelwp;

	KASSERT(curcpu() == ci);
	ci->ci_curlwp = l;
	ci->ci_curpcb = lwp_getpcb(l);

	if (sc->sc_timer1)
		litexsmp_start_timer1(sc);

	atomic_inc_uint(&litexsmp_hatched);
	idle_loop(NULL);
	/* NOTREACHED */
}

static void
litexsmp_boot_secondaries(void)
{
	struct litexsmp_softc * const sc = litexsmp_sc;
	struct litex68k_hatch * const h = &litexsmp_hatch;
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	paddr_t hpa, tpa;
	u_int started = 0;
	int i;

	if (!pmap_extract(pmap_kernel(), (vaddr_t)&h->h_ci, &hpa) ||
	    !pmap_extract(pmap_kernel(), (vaddr_t)cpu_spinup_trampoline,
			  &tpa)) {
		aprint_error_dev(sc->sc_dev, "no PA for the trampoline\n");
		return;
	}

	for (CPU_INFO_FOREACH(cii, ci)) {
		struct lwp *l;

		if (CPU_IS_PRIMARY(ci))
			continue;
		KASSERT(ci->ci_cpuid == 1);	/* RELEASE starts CPU 1 */
		l = ci->ci_data.cpu_idlelwp;
		h->h_ci = ci;
		h->h_sp = (char *)uvm_lwp_getuarea(l) + USPACE - 4;
		h->h_vbr = ci->ci_vectors;
		h->h_srp = Sysseg_pa;

		/*
		 * The hatch is in write-through memory, so CPU 1 reads it
		 * from memory with its caches off.  smp_start_cpu1()'s order.
		 */
		REG_WRITE(sc, LITEXSMP_BOOT_SP, hpa);
		REG_WRITE(sc, LITEXSMP_BOOT_PC, tpa);
		REG_WRITE(sc, LITEXSMP_RELEASE, 1);
		started++;
		for (i = 0; i < 1000000 && litexsmp_hatched < started; i++)
			delay(10);
		if (litexsmp_hatched < started) {
			aprint_error_dev(sc->sc_dev, "cpu%lu did not start\n",
			    (u_long)ci->ci_cpuid);
			REG_WRITE(sc, LITEXSMP_RELEASE, 0);
			REG_WRITE(sc, LITEXSMP_BOOT_PC, 0);
			started--;
		}
	}
	aprint_normal_dev(sc->sc_dev, "%u secondary CPU%s started\n",
	    started, started == 1 ? "" : "s");
}

static void
litexsmp_attach_cpu(struct litexsmp_softc *sc, u_int n)
{
	struct cpu_info *ci, *t;
	void **vectors;
	char *stk;
	int error;

	ci = kmem_zalloc(sizeof(*ci), KM_SLEEP);
	vectors = kmem_alloc(VECTAB_SIZE * sizeof(void *), KM_SLEEP);
	memcpy(vectors, vectab, NVECTORS * sizeof(void *));
	vectors[NVECTORS] = ci;
	stk = kmem_alloc(LITEX68K_TMPSTK_SIZE, KM_SLEEP);

	ci->ci_self = ci;
	ci->ci_cpuid = n;
	ci->ci_vectors = vectors;
	ci->ci_tmpstk = stk + LITEX68K_TMPSTK_SIZE;

	error = mi_cpu_attach(ci);
	if (error != 0) {
		aprint_error_dev(sc->sc_dev, "cannot attach cpu%u: %d\n",
		    n, error);
		return;
	}
	/* append to the CPU list, which vec_set_entry() walks */
	for (t = cpu_info_list; t->ci_next != NULL; t = t->ci_next)
		continue;
	t->ci_next = ci;
}
#endif /* MULTIPROCESSOR */

static int
litexsmp_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
litexsmp_attach(device_t parent, device_t self, void *aux)
{
	struct litexsmp_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	if (bus_space_map(sc->sc_bst, ma->ma_addr, LITEXSMP_SIZE, 0,
			  &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	sc->sc_ncpus = REG_READ(sc, LITEXSMP_NCPUS);

	aprint_naive("\n");
	aprint_normal(": RD68041 SMP board, %u CPUs\n", sc->sc_ncpus);
	litexsmp_sc = sc;

#ifdef MULTIPROCESSOR
	struct bi_litex_dev *ld;
	char strbuf[INTR_STRING_BUFSIZE];
	u_int n;

	if (REG_READ(sc, LITEXSMP_WHOAMI) != 0) {
		aprint_error_dev(self, "not running on cpu0\n");
		return;
	}
	if (sc->sc_ncpus > 2) {
		/* RELEASE and the interrupt masks know of CPU 1 only */
		aprint_error_dev(self, "only 2 CPUs supported\n");
		sc->sc_ncpus = 2;
	}

	for (n = 1; n < sc->sc_ncpus; n++)
		litexsmp_attach_cpu(sc, n);

	/* CPU 1's clock: Timer1, or Timer0's ticks by IPI. */
	ld = bootinfo_litex_dev(BI_LITEX_TIMER1);
	if (ld != NULL &&
	    bus_space_map(sc->sc_bst, ld->ld_base, TIMER_SIZE, 0,
			  &sc->sc_tbsh) == 0) {
		TREG_WRITE(sc, TIMER_EN, 0);
		TREG_WRITE(sc, TIMER_EV_ENABLE, 0);
		TREG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);
		evcnt_attach_dynamic(&sc->sc_tev, EVCNT_TYPE_INTR, NULL,
		    device_xname(self), "timer1");
		sc->sc_tih = intr_establish_cpu(litexsmp_timer1_intr, NULL,
		    ld->ld_irq, IPL_SCHED, 0, 1);
	}
	if (sc->sc_tih != NULL) {
		sc->sc_timer1 = true;
		aprint_normal_dev(self, "cpu1's hardclock on timer1 at %s\n",
		    intr_string(sc->sc_tih, strbuf, sizeof(strbuf)));
	} else {
		litex68k_clock_ipi = litexsmp_clock_ipi;
		aprint_normal_dev(self, "no timer1: cpu1's hardclock by IPI\n");
	}

	litex68k_ipi_intr = litexsmp_ipi_intr;
	litex68k_ipi_send = litexsmp_ipi_send;
	litex68k_boot_secondaries = litexsmp_boot_secondaries;
#else
	if (sc->sc_ncpus > 1)
		aprint_normal_dev(self,
		    "options MULTIPROCESSOR not configured, using 1 CPU\n");
#endif
}

CFATTACH_DECL_NEW(litexsmp, sizeof(struct litexsmp_softc),
	litexsmp_match, litexsmp_attach, NULL, NULL);
