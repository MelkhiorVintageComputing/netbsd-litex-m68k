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
 * LiteX Timer0: the hardclock, delay() and the timecounter.
 *
 * A 32-bit down-counter on sys_clk with an event when it reaches 0,
 * reloaded from `reload' (a period of reload + 1 clocks), and a 64-bit
 * uptime counter of sys_clk cycles since power-up (the SoC is built with
 * --timer-uptime), latched by a write and read high word first.
 * sys_clk comes from the bootinfo (BI_LITEX_SYS_CLK).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_multiprocessor.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/systm.h>
#include <sys/timetc.h>

#include <machine/bootinfo.h>
#include <machine/clockvar.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/dev/litextimerreg.h>
#ifdef MULTIPROCESSOR
#include <sys/lock.h>
#include <litex68k/litex68k/cpu_mp.h>
#endif


struct litextimer_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	uint32_t		sc_freq;	/* sys_clk, Hz */
	void			*sc_ih;
	struct clock_attach_args sc_clock_args;
	void			(*sc_handler)(struct clockframe *);
	struct evcnt		*sc_evcnt;
	struct timecounter	sc_tc;
};

#define	REG_READ(sc, r)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define	REG_WRITE(sc, r, v)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static int	litextimer_match(device_t, cfdata_t, void *);
static void	litextimer_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(litextimer, sizeof(struct litextimer_softc),
    litextimer_match, litextimer_attach, NULL, NULL);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,timer" },
	DEVICE_COMPAT_EOL
};

#ifdef MULTIPROCESSOR
/* The latch is shared: another CPU's would change the words we read. */
static __cpu_simple_lock_t litextimer_latch_lock = __SIMPLELOCK_UNLOCKED;
#endif

static uint64_t
litextimer_uptime(struct litextimer_softc *sc)
{
	uint32_t hi, lo;
#ifdef MULTIPROCESSOR
	int s = splhigh();

	__cpu_simple_lock(&litextimer_latch_lock);
#endif
	REG_WRITE(sc, TIMER_UPTIME_LATCH, 1);
	hi = REG_READ(sc, TIMER_UPTIME_HI);
	lo = REG_READ(sc, TIMER_UPTIME_LO);
#ifdef MULTIPROCESSOR
	__cpu_simple_unlock(&litextimer_latch_lock);
	splx(s);
#endif
	return ((uint64_t)hi << 32) | lo;
}

static u_int
litextimer_get_timecount(struct timecounter *tc)
{
	struct litextimer_softc *sc = tc->tc_priv;

	return (u_int)litextimer_uptime(sc);
}

static int
litextimer_intr(void *v)
{
	struct litextimer_softc *sc = clock_devices[CLOCK_HARDCLOCK];

	if ((REG_READ(sc, TIMER_EV_PENDING) & TIMER_EV_ZERO) == 0) {
		return 0;
	}
	REG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);

	sc->sc_evcnt->ev_count++;
	sc->sc_handler((struct clockframe *)v);
#ifdef MULTIPROCESSOR
	/* The other CPUs' ticks, when they have no timer of their own. */
	if (litex68k_clock_ipi != NULL)
		(*litex68k_clock_ipi)();
#endif
	return 1;
}

static void
litextimer_initclock(void *arg, unsigned int interval_us,
    struct evcnt *ev, void (*func)(struct clockframe *))
{
	struct litextimer_softc *sc = arg;
	uint32_t period;

	sc->sc_handler = func;
	sc->sc_evcnt = ev;

	period = (uint32_t)(((uint64_t)sc->sc_freq * interval_us) / 1000000);

	/* Stop, set the period, clear a stale event, then start. */
	REG_WRITE(sc, TIMER_EN, 0);
	REG_WRITE(sc, TIMER_LOAD, 0);
	REG_WRITE(sc, TIMER_RELOAD, period - 1);
	REG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);
	REG_WRITE(sc, TIMER_EV_ENABLE, TIMER_EV_ZERO);
	REG_WRITE(sc, TIMER_EN, 1);

	/* The timecounter: the low 32 bits of the uptime counter. */
	sc->sc_tc.tc_get_timecount = litextimer_get_timecount;
	sc->sc_tc.tc_counter_mask = ~0u;
	sc->sc_tc.tc_frequency = sc->sc_freq;
	sc->sc_tc.tc_name = device_xname(sc->sc_dev);
	sc->sc_tc.tc_quality = 100;
	sc->sc_tc.tc_priv = sc;
	tc_init(&sc->sc_tc);
}

static void
litextimer_delay(device_t self, unsigned int usec)
{
	struct litextimer_softc *sc = device_private(self);
	uint64_t end;

	end = litextimer_uptime(sc) +
	    ((uint64_t)sc->sc_freq * usec + 999999) / 1000000;
	while (litextimer_uptime(sc) < end) {
		continue;
	}
}

static int
litextimer_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
litextimer_attach(device_t parent, device_t self, void *aux)
{
	struct litextimer_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	char strbuf[INTR_STRING_BUFSIZE];

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	if (bus_space_map(sc->sc_bst, ma->ma_addr, TIMER_SIZE, 0,
			  &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}

	sc->sc_freq = bootinfo_sys_clk();
	if (sc->sc_freq == 0) {
		aprint_error(": no sys_clk in the bootinfo\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": LiteX timer, %u.%06u MHz\n",
	    sc->sc_freq / 1000000, sc->sc_freq % 1000000);

	/* Quiet until cpu_initclocks() starts it. */
	REG_WRITE(sc, TIMER_EN, 0);
	REG_WRITE(sc, TIMER_EV_ENABLE, 0);
	REG_WRITE(sc, TIMER_EV_PENDING, TIMER_EV_ZERO);

	sc->sc_ih = intr_establish(litextimer_intr, NULL, ma->ma_irq,
	    IPL_SCHED, 0);
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't install interrupt handler\n");
		return;
	}
	aprint_normal_dev(self, "%s interrupting at %s\n",
	    clock_name(CLOCK_HARDCLOCK),
	    intr_string(sc->sc_ih, strbuf, sizeof(strbuf)));

	sc->sc_clock_args.ca_initfunc = litextimer_initclock;
	sc->sc_clock_args.ca_arg = sc;
	sc->sc_clock_args.ca_which = CLOCK_HARDCLOCK;
	clock_attach(self, &sc->sc_clock_args, litextimer_delay);
}
