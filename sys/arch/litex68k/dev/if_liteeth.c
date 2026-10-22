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
 * LiteEth: LiteX's Ethernet MAC, with its packet buffers in on-chip SRAM.
 *
 * There is no DMA: frames are copied in and out of `slots' through an
 * uncached mapping.  The MAC adds the preamble and FCS on transmit and
 * pads short frames, checks and strips the FCS on receive, and does no
 * address filtering (ether_input() does).  The PHY is managed through a
 * bit-banged MDIO.  The register layout follows the SoC's generated
 * csr.h, the sequences LiteX's BSD-licensed libliteeth.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/bpf.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mii/mii_bitbang.h>

#include <machine/bootinfo.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/litex68k/litex_fdt.h>

/* MAC CSRs */
#define	MAC_WRITER_SLOT		0x00	/* receive: the oldest frame's slot */
#define	MAC_WRITER_LENGTH	0x04
#define	MAC_WRITER_ERRORS	0x08
#define	MAC_WRITER_EV_STATUS	0x0c
#define	MAC_WRITER_EV_PENDING	0x10	/* W1: release the oldest frame */
#define	MAC_WRITER_EV_ENABLE	0x14
#define	MAC_READER_START	0x18
#define	MAC_READER_READY	0x1c
#define	MAC_READER_LEVEL	0x20
#define	MAC_READER_SLOT		0x24
#define	MAC_READER_LENGTH	0x28
#define	MAC_READER_EV_STATUS	0x2c
#define	MAC_READER_EV_PENDING	0x30
#define	MAC_READER_EV_ENABLE	0x34
#define	MAC_PREAMBLE_CRC	0x38
#define	MAC_SIZE		0x3c

#define	MAC_EV			__BIT(0)

/* PHY CSRs: reset and MDIO */
#define	PHY_CRG_RESET		0x00
#define	PHY_MDIO_W		0x04
#define	 MDIO_W_MDC		__BIT(0)
#define	 MDIO_W_OE		__BIT(1)
#define	 MDIO_W_DO		__BIT(2)
#define	PHY_MDIO_R		0x08
#define	 MDIO_R_DI		__BIT(0)
#define	PHY_SIZE		0x0c

struct liteeth_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_mac_bsh;
	bus_space_handle_t	sc_phy_bsh;
	bus_space_handle_t	sc_buf_bsh;
	bus_size_t		sc_buf_size;
	struct ethercom		sc_ec;
	struct mii_data		sc_mii;
	callout_t		sc_tick_ch;
	void			*sc_ih;

	uint32_t		sc_rx_slots;
	uint32_t		sc_tx_slots;
	uint32_t		sc_slot_size;
	uint32_t		sc_txnext;	/* the next transmit slot */

	uint32_t		sc_txbuf[2048 / 4];	/* staging, aligned */
};

#define	MAC_RD(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_mac_bsh, (r))
#define	MAC_WR(sc, r, v) \
	bus_space_write_4((sc)->sc_bst, (sc)->sc_mac_bsh, (r), (v))
#define	PHY_RD(sc, r)	bus_space_read_4((sc)->sc_bst, (sc)->sc_phy_bsh, (r))
#define	PHY_WR(sc, r, v) \
	bus_space_write_4((sc)->sc_bst, (sc)->sc_phy_bsh, (r), (v))

static int	liteeth_match(device_t, cfdata_t, void *);
static void	liteeth_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(liteeth, sizeof(struct liteeth_softc),
    liteeth_match, liteeth_attach, NULL, NULL);

static int	liteeth_init(struct ifnet *);
static void	liteeth_stop(struct ifnet *, int);
static void	liteeth_start(struct ifnet *);
static int	liteeth_ioctl(struct ifnet *, u_long, void *);
static int	liteeth_intr(void *);
static void	liteeth_tick(void *);

static int	liteeth_mii_readreg(device_t, int, int, uint16_t *);
static int	liteeth_mii_writereg(device_t, int, int, uint16_t);
static void	liteeth_mii_statchg(struct ifnet *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,liteeth" },
	DEVICE_COMPAT_EOL
};

/*
 * MDIO, bit-banged.
 */
static uint32_t
liteeth_mdio_read(device_t self)
{
	struct liteeth_softc *sc = device_private(self);

	return PHY_RD(sc, PHY_MDIO_R);
}

static void
liteeth_mdio_write(device_t self, uint32_t val)
{
	struct liteeth_softc *sc = device_private(self);

	PHY_WR(sc, PHY_MDIO_W, val);
	delay(1);		/* MDC well below 2.5 MHz */
}

static const struct mii_bitbang_ops liteeth_mbo = {
	liteeth_mdio_read,
	liteeth_mdio_write,
	{
		MDIO_W_DO,	/* MII_BIT_MDO */
		MDIO_R_DI,	/* MII_BIT_MDI */
		MDIO_W_MDC,	/* MII_BIT_MDC */
		MDIO_W_OE,	/* MII_BIT_DIR_HOST_PHY */
		0,		/* MII_BIT_DIR_PHY_HOST */
	}
};

static int
liteeth_mii_readreg(device_t self, int phy, int reg, uint16_t *val)
{
	/* PHY address 0 is the RTL8211E's broadcast alias of its own. */
	if (phy == 0) {
		return EIO;
	}
	return mii_bitbang_readreg(self, &liteeth_mbo, phy, reg, val);
}

static int
liteeth_mii_writereg(device_t self, int phy, int reg, uint16_t val)
{
	return mii_bitbang_writereg(self, &liteeth_mbo, phy, reg, val);
}

static void
liteeth_mii_statchg(struct ifnet *ifp)
{
	/* Nothing to set in the MAC. */
}

/*
 * Autoconfiguration.
 */
static int
liteeth_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
liteeth_get_enaddr(struct liteeth_softc *sc, int node, uint8_t *enaddr)
{
	const uint8_t *prop;
	char buf[32];
	int len;

	/* The device tree's, when LiteX's macaddr constants gave one. */
	prop = litex_fdt_prop(node, "local-mac-address", &len);
	if (prop != NULL && len == ETHER_ADDR_LEN) {
		memcpy(enaddr, prop, ETHER_ADDR_LEN);
		return;
	}

	/* liteeth.mac=xx:xx:xx:xx:xx:xx on the command line. */
	if (bootinfo_getarg("liteeth.mac", buf, sizeof(buf)) &&
	    ether_aton_r(enaddr, ETHER_ADDR_LEN, buf) == 0) {
		return;
	}

	/*
	 * Otherwise a fixed, locally administered one ("LX68"), so that
	 * the board keeps its DHCP lease from one boot to the next.
	 */
	enaddr[0] = 0x02;
	enaddr[1] = 0x00;
	enaddr[2] = 'L';
	enaddr[3] = 'X';
	enaddr[4] = '6';
	enaddr[5] = '8';
}

static void
liteeth_attach(device_t parent, device_t self, void *aux)
{
	struct liteeth_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	struct mii_data *mii = &sc->sc_mii;
	uint8_t enaddr[ETHER_ADDR_LEN];
	bus_addr_t addr;
	bus_size_t size;
	char strbuf[INTR_STRING_BUFSIZE];
	const int node = ma->ma_fdt_node;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;

	if (!litex_fdt_u32(node, "litex,rx-slots", &sc->sc_rx_slots) ||
	    !litex_fdt_u32(node, "litex,tx-slots", &sc->sc_tx_slots) ||
	    !litex_fdt_u32(node, "litex,slot-size", &sc->sc_slot_size) ||
	    sc->sc_slot_size > sizeof(sc->sc_txbuf) ||
	    sc->sc_rx_slots == 0 || sc->sc_tx_slots == 0) {
		aprint_error(": bad slot description\n");
		return;
	}

	/* reg: mac, mdio (the PHY's CSRs), buffer. */
	if (!litex_fdt_reg(node, 0, &addr, &size) ||
	    bus_space_map(sc->sc_bst, addr, MAC_SIZE, 0, &sc->sc_mac_bsh)) {
		aprint_error(": couldn't map the MAC\n");
		return;
	}
	if (!litex_fdt_reg(node, 1, &addr, &size) ||
	    bus_space_map(sc->sc_bst, addr, PHY_SIZE, 0, &sc->sc_phy_bsh)) {
		aprint_error(": couldn't map the PHY's CSRs\n");
		return;
	}
	if (!litex_fdt_reg(node, 2, &addr, &size) ||
	    size < (sc->sc_rx_slots + sc->sc_tx_slots) * sc->sc_slot_size ||
	    bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_buf_bsh)) {
		aprint_error(": couldn't map the buffers\n");
		return;
	}
	sc->sc_buf_size = size;

	aprint_naive("\n");
	aprint_normal(": LiteEth, %u+%u slots of %u bytes%s\n",
	    sc->sc_rx_slots, sc->sc_tx_slots, sc->sc_slot_size,
	    MAC_RD(sc, MAC_PREAMBLE_CRC) ? "" : ", no FCS in hardware!");

	liteeth_get_enaddr(sc, node, enaddr);
	aprint_normal_dev(self, "Ethernet address %s\n",
	    ether_sprintf(enaddr));

	/* Quiet. */
	MAC_WR(sc, MAC_WRITER_EV_ENABLE, 0);
	MAC_WR(sc, MAC_READER_EV_ENABLE, 0);
	MAC_WR(sc, MAC_READER_EV_PENDING, MAC_EV);

	/* Reset the PHY, as LiteX's eth_init does. */
	PHY_WR(sc, PHY_CRG_RESET, 1);
	delay(200000);
	PHY_WR(sc, PHY_CRG_RESET, 0);
	delay(200000);

	callout_init(&sc->sc_tick_ch, 0);
	callout_setfunc(&sc->sc_tick_ch, liteeth_tick, sc);

	strlcpy(ifp->if_xname, device_xname(self), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = liteeth_init;
	ifp->if_stop = liteeth_stop;
	ifp->if_start = liteeth_start;
	ifp->if_ioctl = liteeth_ioctl;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	IFQ_SET_READY(&ifp->if_snd);

	mii->mii_ifp = ifp;
	mii->mii_readreg = liteeth_mii_readreg;
	mii->mii_writereg = liteeth_mii_writereg;
	mii->mii_statchg = liteeth_mii_statchg;
	sc->sc_ec.ec_mii = mii;
	ifmedia_init(&mii->mii_media, 0, ether_mediachange, ether_mediastatus);
	mii_attach(self, mii, 0xffffffff, MII_PHY_ANY, MII_OFFSET_ANY, 0);
	if (LIST_FIRST(&mii->mii_phys) == NULL) {
		ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_MANUAL, 0, NULL);
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_MANUAL);
	} else {
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_AUTO);
	}

	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, enaddr);

	sc->sc_ih = intr_establish(liteeth_intr, sc, ma->ma_irq, IPL_NET, 0);
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't install interrupt handler\n");
		return;
	}
	aprint_normal_dev(self, "interrupting at %s\n",
	    intr_string(sc->sc_ih, strbuf, sizeof(strbuf)));
}

/*
 * Receive: every frame waiting, oldest first; its slot is released
 * once it has been copied out.
 */
static void
liteeth_rxintr(struct liteeth_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	struct mbuf *m;
	uint32_t slot, len;

	while (MAC_RD(sc, MAC_WRITER_EV_PENDING) & MAC_EV) {
		slot = MAC_RD(sc, MAC_WRITER_SLOT);
		len = MAC_RD(sc, MAC_WRITER_LENGTH);

		if (slot >= sc->sc_rx_slots || len < ETHER_HDR_LEN ||
		    len > sc->sc_slot_size) {
			if_statinc(ifp, if_ierrors);
			goto next;
		}

		MGETHDR(m, M_DONTWAIT, MT_DATA);
		if (m == NULL) {
			if_statinc(ifp, if_iqdrops);
			goto next;
		}
		if (len + 3 > MHLEN) {
			MCLGET(m, M_DONTWAIT);
			if ((m->m_flags & M_EXT) == 0) {
				m_freem(m);
				if_statinc(ifp, if_iqdrops);
				goto next;
			}
		}
		/* 32-bit reads: the buffers are uncached SRAM. */
		bus_space_read_region_4(sc->sc_bst, sc->sc_buf_bsh,
		    slot * sc->sc_slot_size, mtod(m, uint32_t *),
		    howmany(len, 4));
		m->m_pkthdr.len = m->m_len = len;
		m_set_rcvif(m, ifp);
		if_percpuq_enqueue(ifp->if_percpuq, m);
next:
		MAC_WR(sc, MAC_WRITER_EV_PENDING, MAC_EV);
	}
}

static int
liteeth_intr(void *v)
{
	struct liteeth_softc *sc = v;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	bool claimed = false;

	if (MAC_RD(sc, MAC_WRITER_EV_PENDING) & MAC_EV) {
		liteeth_rxintr(sc);
		claimed = true;
	}
	if (MAC_RD(sc, MAC_READER_EV_PENDING) & MAC_EV) {
		/* A frame has gone: there is room again. */
		MAC_WR(sc, MAC_READER_EV_PENDING, MAC_EV);
		MAC_WR(sc, MAC_READER_EV_ENABLE, 0);
		if_schedule_deferred_start(ifp);
		claimed = true;
	}
	return claimed;
}

/*
 * Transmit, round-robin over the slots.  Commands complete in order, so
 * the next slot is free while fewer commands are queued than there are
 * slots.
 */
static void
liteeth_start(struct ifnet *ifp)
{
	struct liteeth_softc *sc = ifp->if_softc;
	struct mbuf *m;
	uint32_t len;

	if ((ifp->if_flags & IFF_RUNNING) == 0) {
		return;
	}

	for (;;) {
		if (MAC_RD(sc, MAC_READER_LEVEL) >= sc->sc_tx_slots ||
		    MAC_RD(sc, MAC_READER_READY) == 0) {
			/* Come back when a frame has gone. */
			MAC_WR(sc, MAC_READER_EV_PENDING, MAC_EV);
			MAC_WR(sc, MAC_READER_EV_ENABLE, MAC_EV);
			return;
		}

		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL) {
			return;
		}

		len = m->m_pkthdr.len;
		if (len > sc->sc_slot_size) {
			m_freem(m);
			if_statinc(ifp, if_oerrors);
			continue;
		}
		m_copydata(m, 0, len, sc->sc_txbuf);
		bpf_mtap(ifp, m, BPF_D_OUT);
		m_freem(m);

		bus_space_write_region_4(sc->sc_bst, sc->sc_buf_bsh,
		    (sc->sc_rx_slots + sc->sc_txnext) * sc->sc_slot_size,
		    sc->sc_txbuf, howmany(len, 4));
		MAC_WR(sc, MAC_READER_SLOT, sc->sc_txnext);
		MAC_WR(sc, MAC_READER_LENGTH, len);
		MAC_WR(sc, MAC_READER_START, 1);
		sc->sc_txnext = (sc->sc_txnext + 1) % sc->sc_tx_slots;
		if_statinc(ifp, if_opackets);
	}
}

static int
liteeth_init(struct ifnet *ifp)
{
	struct liteeth_softc *sc = ifp->if_softc;
	int s;

	liteeth_stop(ifp, 0);

	s = splnet();
	/* Drop what came in while we were down. */
	while (MAC_RD(sc, MAC_WRITER_EV_PENDING) & MAC_EV) {
		MAC_WR(sc, MAC_WRITER_EV_PENDING, MAC_EV);
	}
	MAC_WR(sc, MAC_WRITER_EV_ENABLE, MAC_EV);
	ifp->if_flags |= IFF_RUNNING;
	splx(s);

	(void)ether_mediachange(ifp);
	callout_schedule(&sc->sc_tick_ch, hz);

	return 0;
}

static void
liteeth_stop(struct ifnet *ifp, int disable)
{
	struct liteeth_softc *sc = ifp->if_softc;
	int s;

	callout_stop(&sc->sc_tick_ch);

	s = splnet();
	MAC_WR(sc, MAC_WRITER_EV_ENABLE, 0);
	MAC_WR(sc, MAC_READER_EV_ENABLE, 0);
	ifp->if_flags &= ~IFF_RUNNING;
	splx(s);

	mii_down(&sc->sc_mii);
}

static int
liteeth_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	int s, error;

	s = splnet();
	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET) {
		/* No filter to program: the MAC passes everything. */
		error = 0;
	}
	splx(s);
	return error;
}

static void
liteeth_tick(void *arg)
{
	struct liteeth_softc *sc = arg;
	int s;

	s = splnet();
	mii_tick(&sc->sc_mii);
	splx(s);

	callout_schedule(&sc->sc_tick_ch, hz);
}
