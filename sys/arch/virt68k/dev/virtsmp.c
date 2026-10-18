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
 * Driver for the QEMU m68k virt machine SMP controller ("virt-smp",
 * see QEMU's docs/specs/virt-smp.rst).
 *
 * Always: a timecounter on the controller's nanosecond counter.
 * With MULTIPROCESSOR: attachment and start of the secondary CPUs,
 * inter-processor interrupts, and the hardclock of the secondary CPUs
 * from their per-CPU timers.  Device interrupts stay on the boot CPU.
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
#include <sys/timetc.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/pcb.h>
#include <machine/vectors.h>

#include <virt68k/dev/mainbusvar.h>
#include <virt68k/dev/virtsmpreg.h>
#include <virt68k/virt68k/cpu_mp.h>

struct virtsmp_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	u_int			sc_ncpus;
	struct timecounter	sc_tc;
};

static struct virtsmp_softc *virtsmp_sc;

#ifdef __HAVE_CPU_COUNTER
volatile uint32_t *virt68k_counter_reg;	/* <machine/cpu_counter.h> */
#endif

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "netbsd,qemu-virt-smp" },
	DEVICE_COMPAT_EOL
};

#define	REG_READ(sc, r)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define	REG_WRITE(sc, r, v)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static u_int
virtsmp_get_timecount(struct timecounter *tc)
{
	struct virtsmp_softc * const sc = tc->tc_priv;

	return REG_READ(sc, VIRTSMP_COUNTER_LO);
}

#ifdef MULTIPROCESSOR
/* Assembly glue in locore.s */
extern char intrhand_virtsmp_ipi[], intrhand_virtsmp_timer[];
extern char cpu_spinup_trampoline[];

/*
 * Read by cpu_spinup_trampoline (MMU off, so in the kernel image, which
 * is mapped VA == PA).
 */
struct virt68k_hatch virt68k_hatch[MAXCPUS];
static volatile u_int virtsmp_hatched;

static void
virtsmp_ipi_send(struct cpu_info *ci, u_int ipi)
{
	struct virtsmp_softc * const sc = virtsmp_sc;

	REG_WRITE(sc, VIRTSMP_IPI_SEND,
	    (1u << (VIRTSMP_IPI_REASON_SHIFT + ipi)) | (1u << ci->ci_cpuid));
}

/* IPI interrupt, at IPL_SCHED, from intrhand_virtsmp_ipi. */
void	virtsmp_ipi_intr(void);
void
virtsmp_ipi_intr(void)
{
	struct virtsmp_softc * const sc = virtsmp_sc;
	uint32_t pending;

	pending = REG_READ(sc, VIRTSMP_IPI_PENDING);
	REG_WRITE(sc, VIRTSMP_IPI_CLEAR, pending);
	curcpu()->ci_data.cpu_nintr++;
	virt68k_ipi_dispatch(pending);
}

/* Per-CPU timer interrupt, at IPL_SCHED, from intrhand_virtsmp_timer. */
void	virtsmp_clock_intr(struct clockframe);
void
virtsmp_clock_intr(struct clockframe frame)
{
	struct virtsmp_softc * const sc = virtsmp_sc;

	REG_WRITE(sc, VIRTSMP_TIMER_STATUS, VIRTSMP_TIMER_EXPIRED);
	curcpu()->ci_data.cpu_nintr++;
	hardclock(&frame);
}

static void
virtsmp_start_clock(struct virtsmp_softc *sc)
{
	REG_WRITE(sc, VIRTSMP_TIMER_INTERVAL, 1000000000u / hz);
	REG_WRITE(sc, VIRTSMP_TIMER_CTRL,
	    VIRTSMP_TIMER_ENABLE | VIRTSMP_TIMER_PERIODIC);
}

/*
 * First C code run by a secondary CPU, on the stack of its idle lwp,
 * with the MMU on and its VBR loaded.  Does not return.
 */
void	cpu_hatch(struct cpu_info *);
void
cpu_hatch(struct cpu_info *ci)
{
	struct lwp *l = ci->ci_data.cpu_idlelwp;

	KASSERT(curcpu() == ci);
	ci->ci_curlwp = l;
	ci->ci_curpcb = lwp_getpcb(l);

	virtsmp_start_clock(virtsmp_sc);

	atomic_inc_uint(&virtsmp_hatched);
	idle_loop(NULL);
	/* NOTREACHED */
}

static void
virtsmp_boot_secondaries(void)
{
	struct virtsmp_softc * const sc = virtsmp_sc;
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	u_int started = 0;

	for (CPU_INFO_FOREACH(cii, ci)) {
		struct virt68k_hatch *h;
		struct lwp *l;
		int n = ci->ci_cpuid, i;

		if (CPU_IS_PRIMARY(ci))
			continue;
		l = ci->ci_data.cpu_idlelwp;
		h = &virt68k_hatch[n];
		h->h_ci = ci;
		h->h_sp = (char *)uvm_lwp_getuarea(l) + USPACE - 4;
		h->h_vbr = ci->ci_vectors;
		REG_WRITE(sc, VIRTSMP_CPU(n) + VIRTSMP_START_PC,
		    (uint32_t)cpu_spinup_trampoline);
		REG_WRITE(sc, VIRTSMP_CPU(n) + VIRTSMP_START_SP,
		    (uint32_t)&h->h_stack[__arraycount(h->h_stack)]);
		REG_WRITE(sc, VIRTSMP_CPU(n) + VIRTSMP_START_ARG, (uint32_t)h);
		REG_WRITE(sc, VIRTSMP_CPU(n) + VIRTSMP_CPU_CTRL, 1);
		started++;
		for (i = 0; i < 1000000 && virtsmp_hatched < started; i++)
			delay(10);
		if (virtsmp_hatched < started) {
			aprint_error("%s: cpu%d did not start\n",
			    device_xname(sc->sc_dev), n);
			started--;
		}
	}
	aprint_normal("%s: %u secondary CPUs started\n",
	    device_xname(sc->sc_dev), started);
}

static void
virtsmp_attach_cpu(struct virtsmp_softc *sc, u_int n)
{
	struct cpu_info *ci, *t;
	void **vectors;
	char *stk;
	int error;

	ci = kmem_zalloc(sizeof(*ci), KM_SLEEP);
	vectors = kmem_alloc(VECTAB_SIZE * sizeof(void *), KM_SLEEP);
	memcpy(vectors, vectab, NVECTORS * sizeof(void *));
	vectors[NVECTORS] = ci;
	stk = kmem_alloc(VIRT68K_TMPSTK_SIZE, KM_SLEEP);

	ci->ci_self = ci;
	ci->ci_cpuid = n;
	ci->ci_vectors = vectors;
	ci->ci_tmpstk = stk + VIRT68K_TMPSTK_SIZE;

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
virtsmp_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
virtsmp_attach(device_t parent, device_t self, void *aux)
{
	struct virtsmp_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	uint32_t vers;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	if (bus_space_map(sc->sc_bst, ma->ma_addr, ma->ma_size, 0,
			  &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	vers = REG_READ(sc, VIRTSMP_VERSION);
	sc->sc_ncpus = REG_READ(sc, VIRTSMP_NCPUS);

	aprint_naive("\n");
	aprint_normal(": Qemu Virtual SMP Controller v%u, %u CPUs\n",
	    vers, sc->sc_ncpus);
	virtsmp_sc = sc;

	sc->sc_tc.tc_get_timecount = virtsmp_get_timecount;
	sc->sc_tc.tc_counter_mask = ~0u;
	sc->sc_tc.tc_frequency = 1000000000;
	sc->sc_tc.tc_name = device_xname(self);
	sc->sc_tc.tc_quality = 100;
	sc->sc_tc.tc_priv = sc;
	tc_init(&sc->sc_tc);
#ifdef __HAVE_CPU_COUNTER
	virt68k_counter_reg = (volatile uint32_t *)
	    (sc->sc_bsh + VIRTSMP_COUNTER_LO);
#endif

#ifdef MULTIPROCESSOR
	u_int n;

	if (REG_READ(sc, VIRTSMP_ID) != 0) {
		aprint_error_dev(self, "not running on cpu0\n");
		return;
	}
	if (sc->sc_ncpus > MAXCPUS) {
		aprint_error_dev(self, "only %d CPUs supported\n", MAXCPUS);
		sc->sc_ncpus = MAXCPUS;
	}
	/* before the vector tables of the other CPUs are copied */
	vec_set_entry(VIRTSMP_IPI_VECTOR, intrhand_virtsmp_ipi);
	vec_set_entry(VIRTSMP_TIMER_VECTOR, intrhand_virtsmp_timer);

	for (n = 1; n < sc->sc_ncpus; n++)
		virtsmp_attach_cpu(sc, n);

	virt68k_ipi_send = virtsmp_ipi_send;
	virt68k_boot_secondaries = virtsmp_boot_secondaries;
#else
	if (sc->sc_ncpus > 1)
		aprint_normal_dev(self,
		    "options MULTIPROCESSOR not configured, using 1 CPU\n");
#endif
}

CFATTACH_DECL_NEW(virtsmp, sizeof(struct virtsmp_softc),
	virtsmp_match, virtsmp_attach, NULL, NULL);
