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
 * LiteX's UART (LiteUART): console and tty.
 *
 * 16-byte RX and TX FIFOs; the baud rate is fixed in the gateware, and
 * there are no line settings.  Both events are level sources: "rx" is
 * "RX FIFO not empty", and writing 1 to its pending bit pops the byte
 * at the FIFO's head; "tx" is "TX FIFO not full", true nearly always,
 * so it is enabled only while there is output queued.  This follows
 * LiteX's own BSD-licensed driver, libbase/uart.c.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kauth.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/syslog.h>
#include <sys/tty.h>

#include <dev/cons.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/dev/liteuartvar.h>

#include "ioconf.h"

/*
 * LiteUART registers (32-bit CSRs).
 */
#define	LITEUART_RXTX		0x00	/* W: queue a byte; R: RX head */
#define	LITEUART_TXFULL		0x04
#define	LITEUART_RXEMPTY	0x08
#define	LITEUART_EV_STATUS	0x0c
#define	LITEUART_EV_PENDING	0x10
#define	LITEUART_EV_ENABLE	0x14
#define	LITEUART_TXEMPTY	0x18
#define	LITEUART_RXFULL		0x1c

#define	LITEUART_EV_TX		__BIT(0)
#define	LITEUART_EV_RX		__BIT(1)

#define	LITEUART_RXBUFSIZE	256	/* power of 2 */

struct liteuart_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	struct tty		*sc_tty;
	void			*sc_ih;
	void			*sc_si;
	kmutex_t		sc_hwlock;

	bool			sc_is_console;
	uint32_t		sc_ev_enable;	/* soft copy */

	/* Receive ring, filled at interrupt time. */
	u_int			sc_rxget;
	u_int			sc_rxput;
	u_int			sc_rxovf;
	uint8_t			sc_rxbuf[LITEUART_RXBUFSIZE];

	/* Transmit: the piece of t_outq being sent. */
	u_char			*sc_tba;
	u_int			sc_tbc;
	u_int			sc_tblen;
	volatile bool		sc_tx_done;
};

#define	REG_READ(sc, r)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (r))
#define	REG_WRITE(sc, r, v)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (r), (v))

static int	liteuart_match(device_t, cfdata_t, void *);
static void	liteuart_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(liteuart, sizeof(struct liteuart_softc),
    liteuart_match, liteuart_attach, NULL, NULL);

static dev_type_open(liteuart_open);
static dev_type_close(liteuart_close);
static dev_type_read(liteuart_read);
static dev_type_write(liteuart_write);
static dev_type_ioctl(liteuart_ioctl);
static dev_type_stop(liteuart_stop);
static dev_type_tty(liteuart_tty);
static dev_type_poll(liteuart_poll);

const struct cdevsw liteuart_cdevsw = {
	.d_open     = liteuart_open,
	.d_close    = liteuart_close,
	.d_read     = liteuart_read,
	.d_write    = liteuart_write,
	.d_ioctl    = liteuart_ioctl,
	.d_stop     = liteuart_stop,
	.d_tty      = liteuart_tty,
	.d_poll     = liteuart_poll,
	.d_mmap     = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard  = nodiscard,
	.d_flag     = D_TTY,
};

static void	liteuart_start(struct tty *);
static int	liteuart_param(struct tty *, struct termios *);
static int	liteuart_intr(void *);
static void	liteuart_soft(void *);

#define	LITEUART_UNIT(x)	(minor(x) & 0x7ffff)	/* dialout bit off */

/*
 * Console.
 */
static int	liteuart_cngetc(dev_t);
static void	liteuart_cnputc(dev_t, int);
static void	liteuart_cnpollc(dev_t, int);

static bus_space_tag_t		liteuart_cn_bst;
static bus_space_handle_t	liteuart_cn_bsh;
static struct cnm_state		liteuart_cnmagic_state;
static struct consdev liteuart_consdev = {
	.cn_getc  = liteuart_cngetc,
	.cn_putc  = liteuart_cnputc,
	.cn_pollc = liteuart_cnpollc,
	.cn_dev   = NODEV,
	.cn_pri   = CN_NORMAL,
};

static int
liteuart_cngetc(dev_t dev)
{
	bus_space_tag_t bst = liteuart_cn_bst;
	bus_space_handle_t bsh = liteuart_cn_bsh;
	int c;

	if (bus_space_read_4(bst, bsh, LITEUART_RXEMPTY) & 1) {
		return -1;
	}
	c = bus_space_read_4(bst, bsh, LITEUART_RXTX) & 0xff;
	bus_space_write_4(bst, bsh, LITEUART_EV_PENDING, LITEUART_EV_RX);
	return c;
}

static void
liteuart_cnputc(dev_t dev, int c)
{
	bus_space_tag_t bst = liteuart_cn_bst;
	bus_space_handle_t bsh = liteuart_cn_bsh;

	while (bus_space_read_4(bst, bsh, LITEUART_TXFULL) & 1) {
		continue;
	}
	bus_space_write_4(bst, bsh, LITEUART_RXTX, (unsigned char)c);
}

static void
liteuart_cnpollc(dev_t dev, int on)
{
}

void
liteuart_cnattach(bus_space_tag_t bst, bus_space_handle_t bsh)
{
	liteuart_cn_bst = bst;
	liteuart_cn_bsh = bsh;

	cn_tab = &liteuart_consdev;
	cn_init_magic(&liteuart_cnmagic_state);
	cn_set_magic("+++++");
}

static bool
liteuart_is_console(device_t self)
{
	bool val;

	return cn_tab == &liteuart_consdev &&
	    prop_dictionary_get_bool(device_properties(self), "is-console",
				     &val) && val;
}

/*
 * Autoconfiguration.
 */
static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,liteuart" },
	DEVICE_COMPAT_EOL
};

static int
liteuart_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
liteuart_attach(device_t parent, device_t self, void *aux)
{
	struct liteuart_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	char strbuf[INTR_STRING_BUFSIZE];
	struct tty *tp;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	if (bus_space_map(sc->sc_bst, ma->ma_addr, LITEUART_SIZE, 0,
			  &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": LiteX UART\n");

	mutex_init(&sc->sc_hwlock, MUTEX_DEFAULT, IPL_TTY);

	sc->sc_si = softint_establish(SOFTINT_SERIAL, liteuart_soft, sc);
	if (sc->sc_si == NULL) {
		aprint_error_dev(self,
		    "unable to register software interrupt\n");
		return;
	}

	tp = tty_alloc();
	tp->t_oproc = liteuart_start;
	tp->t_param = liteuart_param;
	tp->t_softc = sc;

	/* Once, here: proplib's locks are not for interrupt time. */
	sc->sc_is_console = liteuart_is_console(self);
	if (sc->sc_is_console) {
		int maj = cdevsw_lookup_major(&liteuart_cdevsw);

		tp->t_dev = cn_tab->cn_dev = makedev(maj, device_unit(self));
		aprint_normal_dev(self, "console\n");
	}

	mutex_spin_enter(&tty_lock);
	sc->sc_tty = tp;
	mutex_spin_exit(&tty_lock);

	tty_attach(tp);

	/*
	 * Events off and stale ones cleared before the line is unmasked:
	 * the BIOS left them enabled, and litexboot turned them off, but
	 * do not depend on it.  RX is turned on at the first open.
	 */
	sc->sc_ev_enable = 0;
	REG_WRITE(sc, LITEUART_EV_ENABLE, sc->sc_ev_enable);

	sc->sc_ih = intr_establish(liteuart_intr, sc, ma->ma_irq, IPL_TTY, 0);
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't install interrupt handler\n");
		return;
	}
	aprint_normal_dev(self, "interrupting at %s\n",
	    intr_string(sc->sc_ih, strbuf, sizeof(strbuf)));
}

/*
 * Interrupts.
 */
static void
liteuart_set_events(struct liteuart_softc *sc, uint32_t set, uint32_t clr)
{
	KASSERT(mutex_owned(&sc->sc_hwlock));

	sc->sc_ev_enable = (sc->sc_ev_enable | set) & ~clr;
	REG_WRITE(sc, LITEUART_EV_ENABLE, sc->sc_ev_enable);
}

/* Fill the TX FIFO from the current piece of output. */
static void
liteuart_txfill(struct liteuart_softc *sc)
{
	KASSERT(mutex_owned(&sc->sc_hwlock));

	while (sc->sc_tbc > 0 && (REG_READ(sc, LITEUART_TXFULL) & 1) == 0) {
		REG_WRITE(sc, LITEUART_RXTX, *sc->sc_tba++);
		sc->sc_tbc--;
	}
}

static int
liteuart_intr(void *v)
{
	struct liteuart_softc *sc = v;
	uint32_t stat;
	bool sched = false;

	mutex_spin_enter(&sc->sc_hwlock);

	stat = REG_READ(sc, LITEUART_EV_PENDING) & sc->sc_ev_enable;
	if (stat == 0) {
		mutex_spin_exit(&sc->sc_hwlock);
		return 0;
	}

	if (stat & LITEUART_EV_RX) {
		while ((REG_READ(sc, LITEUART_RXEMPTY) & 1) == 0) {
			int c = REG_READ(sc, LITEUART_RXTX) & 0xff;

			REG_WRITE(sc, LITEUART_EV_PENDING, LITEUART_EV_RX);
			if (sc->sc_is_console) {
				cn_check_magic(sc->sc_tty->t_dev, c,
				    liteuart_cnmagic_state);
			}
			if (sc->sc_rxput - sc->sc_rxget < LITEUART_RXBUFSIZE) {
				sc->sc_rxbuf[sc->sc_rxput++ &
				    (LITEUART_RXBUFSIZE - 1)] = c;
			} else {
				sc->sc_rxovf++;
			}
		}
		sched = true;
	}

	if (stat & LITEUART_EV_TX) {
		liteuart_txfill(sc);
		if (sc->sc_tbc == 0) {
			/* Done: TX off until there is more output. */
			liteuart_set_events(sc, 0, LITEUART_EV_TX);
			sc->sc_tx_done = true;
			sched = true;
		}
	}

	mutex_spin_exit(&sc->sc_hwlock);

	if (sched) {
		softint_schedule(sc->sc_si);
	}
	return 1;
}

static void
liteuart_soft(void *v)
{
	struct liteuart_softc *sc = v;
	struct tty *tp = sc->sc_tty;
	uint8_t buf[LITEUART_RXBUFSIZE];
	u_int n = 0, ovf;
	bool tx_done;

	mutex_spin_enter(&sc->sc_hwlock);
	while (sc->sc_rxget != sc->sc_rxput) {
		buf[n++] = sc->sc_rxbuf[sc->sc_rxget++ &
		    (LITEUART_RXBUFSIZE - 1)];
	}
	ovf = sc->sc_rxovf;
	sc->sc_rxovf = 0;
	tx_done = sc->sc_tx_done;
	sc->sc_tx_done = false;
	mutex_spin_exit(&sc->sc_hwlock);

	if (ovf != 0) {
		log(LOG_WARNING, "%s: %u bytes lost (input overflow)\n",
		    device_xname(sc->sc_dev), ovf);
	}

	if (ISSET(tp->t_state, TS_ISOPEN)) {
		for (u_int i = 0; i < n; i++) {
			(*tp->t_linesw->l_rint)(buf[i], tp);
		}
	}

	if (tx_done) {
		ttylock(tp);
		CLR(tp->t_state, TS_BUSY);
		if (ISSET(tp->t_state, TS_FLUSH)) {
			CLR(tp->t_state, TS_FLUSH);
		} else {
			ndflush(&tp->t_outq, sc->sc_tblen);
		}
		sc->sc_tblen = 0;
		/* ttstart() wants the tty lock, as from ttwrite(). */
		(*tp->t_linesw->l_start)(tp);
		ttyunlock(tp);
	}
}

/*
 * tty glue.
 */
static void
liteuart_start(struct tty *tp)
{
	struct liteuart_softc *sc = tp->t_softc;
	int n;

	KASSERT(ttylocked(tp));

	if (ISSET(tp->t_state, TS_BUSY | TS_TIMEOUT | TS_TTSTOP)) {
		return;
	}
	if (!ttypull(tp)) {
		return;
	}

	/* Send the first contiguous piece of the queue. */
	n = ndqb(&tp->t_outq, 0);
	SET(tp->t_state, TS_BUSY);

	mutex_spin_enter(&sc->sc_hwlock);
	sc->sc_tba = tp->t_outq.c_cf;
	sc->sc_tbc = sc->sc_tblen = n;
	liteuart_txfill(sc);
	if (sc->sc_tbc == 0) {
		sc->sc_tx_done = true;
		softint_schedule(sc->sc_si);
	} else {
		liteuart_set_events(sc, LITEUART_EV_TX, 0);
	}
	mutex_spin_exit(&sc->sc_hwlock);
}

static void
liteuart_stop(struct tty *tp, int flag)
{
	KASSERT(ttylocked(tp));

	if (ISSET(tp->t_state, TS_BUSY)) {
		if (!ISSET(tp->t_state, TS_TTSTOP)) {
			SET(tp->t_state, TS_FLUSH);
		}
	}
}

static int
liteuart_param(struct tty *tp, struct termios *t)
{
	/* The line is fixed in the gateware: accept whatever is asked. */
	tp->t_ispeed = t->c_ispeed;
	tp->t_ospeed = t->c_ospeed;
	tp->t_cflag = t->c_cflag;
	return 0;
}

static int
liteuart_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp;
	int error;

	if (sc == NULL) {
		return ENXIO;
	}
	mutex_spin_enter(&tty_lock);
	tp = sc->sc_tty;
	mutex_spin_exit(&tty_lock);
	if (tp == NULL) {
		return ENXIO;
	}

	if (kauth_authorize_device_tty(l->l_cred, KAUTH_DEVICE_TTY_OPEN, tp)) {
		return EBUSY;
	}

	ttylock(tp);

	if (ISSET(tp->t_state, TS_KERN_ONLY)) {
		ttyunlock(tp);
		return EBUSY;
	}

	tp->t_dev = dev;

	if (!ISSET(tp->t_state, TS_ISOPEN) && tp->t_wopen == 0) {
		struct termios t;

		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		t.c_cflag = TTYDEF_CFLAG;
		t.c_ispeed = t.c_ospeed = TTYDEF_SPEED;
		(void) liteuart_param(tp, &t);
		ttsetwater(tp);

		mutex_spin_enter(&sc->sc_hwlock);
		liteuart_set_events(sc, LITEUART_EV_RX, 0);
		mutex_spin_exit(&sc->sc_hwlock);
	}
	SET(tp->t_state, TS_CARR_ON);

	ttyunlock(tp);

	error = ttyopen(tp, 0, ISSET(flag, O_NONBLOCK));
	if (error == 0) {
		error = (*tp->t_linesw->l_open)(dev, tp);
		if (error != 0) {
			ttyclose(tp);
		}
	}

	return error;
}

static int
liteuart_close(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp = sc->sc_tty;

	ttylock(tp);
	/* XXX This is for cons.c. */
	if (!ISSET(tp->t_state, TS_ISOPEN) ||
	    ISSET(tp->t_state, TS_KERN_ONLY)) {
		ttyunlock(tp);
		return 0;
	}
	ttyunlock(tp);

	(*tp->t_linesw->l_close)(tp, flag);
	ttyclose(tp);

	return 0;
}

static int
liteuart_read(dev_t dev, struct uio *uio, int flag)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp = sc->sc_tty;

	return (*tp->t_linesw->l_read)(tp, uio, flag);
}

static int
liteuart_write(dev_t dev, struct uio *uio, int flag)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp = sc->sc_tty;

	return (*tp->t_linesw->l_write)(tp, uio, flag);
}

static int
liteuart_poll(dev_t dev, int events, struct lwp *l)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp = sc->sc_tty;

	return (*tp->t_linesw->l_poll)(tp, events, l);
}

static struct tty *
liteuart_tty(dev_t dev)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));

	return sc->sc_tty;
}

static int
liteuart_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct liteuart_softc *sc =
	    device_lookup_private(&liteuart_cd, LITEUART_UNIT(dev));
	struct tty *tp = sc->sc_tty;
	int error;

	error = (*tp->t_linesw->l_ioctl)(tp, cmd, data, flag, l);
	if (error != EPASSTHROUGH) {
		return error;
	}
	return ttioctl(tp, cmd, data, flag, l);
}
