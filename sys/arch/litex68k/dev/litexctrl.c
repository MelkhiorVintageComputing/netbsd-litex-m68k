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
 * LiteX SoC controller: reset.  Writing 1 to bit 0 of `reset' resets the
 * whole SoC, and the BIOS runs again.  There is no power-off: halting
 * just spins.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/reboot.h>
#include <sys/systm.h>

#include <machine/cpu.h>

#include <litex68k/dev/mainbusvar.h>

#define	CTRL_RESET		0x00
#define	CTRL_SCRATCH		0x04
#define	CTRL_BUS_ERRORS		0x08
#define	CTRL_SIZE		0x0c

#define	CTRL_RESET_SOC		__BIT(0)

#define	CTRL_SCRATCH_RESET	0x12345678

struct litexctrl_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
};

static int	litexctrl_match(device_t, cfdata_t, void *);
static void	litexctrl_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(litexctrl, sizeof(struct litexctrl_softc),
    litexctrl_match, litexctrl_attach, NULL, NULL);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,soc-controller" },
	DEVICE_COMPAT_EOL
};

static void
litexctrl_reset(void *arg, int howto)
{
	struct litexctrl_softc *sc = arg;

	(void)splhigh();
	if ((howto & RB_HALT) == 0) {
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, CTRL_RESET,
		    CTRL_RESET_SOC);
	}
	for (;;) {
		/* spin forever. */
	}
}

static int
litexctrl_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
litexctrl_attach(device_t parent, device_t self, void *aux)
{
	struct litexctrl_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	uint32_t scratch;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	if (bus_space_map(sc->sc_bst, ma->ma_addr, CTRL_SIZE, 0,
			  &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}

	scratch = bus_space_read_4(sc->sc_bst, sc->sc_bsh, CTRL_SCRATCH);
	aprint_naive("\n");
	aprint_normal(": LiteX SoC controller%s, %u bus errors\n",
	    scratch == CTRL_SCRATCH_RESET ? "" : " (scratch changed)",
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, CTRL_BUS_ERRORS));

	cpu_set_reset_func(litexctrl_reset, sc);
}
