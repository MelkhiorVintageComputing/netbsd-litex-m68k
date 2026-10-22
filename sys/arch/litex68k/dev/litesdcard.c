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
 * LiteSDCard: an SD card host in native mode (1-bit or 4-bit), with
 * hardware CRC and a DMA engine in each direction, as an sdmmc(4) host.
 *
 * The register groups come from the device tree's reg ranges (phy, core,
 * block2mem "reader", mem2block "writer", irq), and the layout within
 * each from the SoC's generated csr.h.  The command and transfer
 * sequences follow LiteX's BSD-licensed BIOS driver (liblitesdcard).
 * Commands are polled: the sdmmc framework runs them from its own
 * thread, and the CPU is the bottleneck anyway.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <machine/bootinfo.h>

#include <dev/sdmmc/sdmmcchip.h>
#include <dev/sdmmc/sdmmcreg.h>
#include <dev/sdmmc/sdmmcvar.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/litex68k/litex_fdt.h>

/* PHY */
#define	PHY_CARD_DETECT		0x00	/* 0: a card is present */
#define	PHY_CLOCKER_DIVIDER	0x04	/* clk = sys_clk / ((div + 1) & ~1) */
#define	PHY_INIT_INITIALIZE	0x08	/* W 1: 80 idle clocks */
#define	PHY_CMDR_TIMEOUT	0x0c
#define	PHY_DATAW_STATUS	0x10
#define	PHY_DATAR_TIMEOUT	0x14
#define	PHY_SETTINGS		0x18	/* data width */
#define	 PHY_SETTINGS_1BIT	0
#define	 PHY_SETTINGS_4BIT	1
#define	 PHY_SETTINGS_8BIT	2
#define	PHY_SIZE		0x1c

/* Core */
#define	CORE_CMD_ARGUMENT	0x00
#define	CORE_CMD_COMMAND	0x04
#define	 CMD_RSP_NONE		0
#define	 CMD_RSP_SHORT		1
#define	 CMD_RSP_LONG		2
#define	 CMD_RSP_SHORT_BUSY	3
#define	 CMD_RSP_CRC		__BIT(2)
#define	 CMD_DATA_NONE		(0 << 5)
#define	 CMD_DATA_READ		(1 << 5)
#define	 CMD_DATA_WRITE		(2 << 5)
#define	 CMD_INDEX(x)		((x) << 8)
#define	CORE_CMD_SEND		0x08
#define	CORE_CMD_RESPONSE	0x0c	/* 128 bits, most significant first */
#define	CORE_CMD_EVENT		0x1c
#define	CORE_DATA_EVENT		0x20
#define	 EV_DONE		__BIT(0)
#define	 EV_ERROR		__BIT(1)
#define	 EV_TIMEOUT		__BIT(2)
#define	 EV_CRC			__BIT(3)
#define	CORE_BLOCK_LENGTH	0x24
#define	CORE_BLOCK_COUNT	0x28
#define	CORE_SIZE		0x2c

/* DMA, both directions */
#define	DMA_BASE_HI		0x00
#define	DMA_BASE_LO		0x04
#define	DMA_LENGTH		0x08
#define	DMA_ENABLE		0x0c
#define	DMA_DONE		0x10
#define	DMA_LOOP		0x14
#define	DMA_OFFSET		0x18
#define	DMA_ERROR		0x1c
#define	DMA_SIZE		0x20

/* Events */
#define	IRQ_STATUS		0x00
#define	IRQ_PENDING		0x04
#define	IRQ_ENABLE		0x08
#define	IRQ_SIZE		0x0c

#define	LITESD_CMD_TIMEOUT_US	1000000
#define	LITESD_DATA_TIMEOUT_US	5000000

enum { REG_PHY, REG_CORE, REG_READER, REG_WRITER, REG_IRQ, REG_COUNT };

static const bus_size_t litesdcard_reg_size[REG_COUNT] = {
	[REG_PHY]	= PHY_SIZE,
	[REG_CORE]	= CORE_SIZE,
	[REG_READER]	= DMA_SIZE,
	[REG_WRITER]	= DMA_SIZE,
	[REG_IRQ]	= IRQ_SIZE,
};

struct litesdcard_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh[REG_COUNT];
	bus_dma_tag_t		sc_dmat;
	device_t		sc_sdmmc;
	uint32_t		sc_sys_clk;	/* Hz */
};

#define	RD(sc, g, r)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh[(g)], (r))
#define	WR(sc, g, r, v)		\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh[(g)], (r), (v))

static int	litesdcard_match(device_t, cfdata_t, void *);
static void	litesdcard_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(litesdcard, sizeof(struct litesdcard_softc),
    litesdcard_match, litesdcard_attach, NULL, NULL);

static int	litesdcard_host_reset(sdmmc_chipset_handle_t);
static uint32_t	litesdcard_host_ocr(sdmmc_chipset_handle_t);
static int	litesdcard_host_maxblklen(sdmmc_chipset_handle_t);
static int	litesdcard_card_detect(sdmmc_chipset_handle_t);
static int	litesdcard_write_protect(sdmmc_chipset_handle_t);
static int	litesdcard_bus_power(sdmmc_chipset_handle_t, uint32_t);
static int	litesdcard_bus_clock(sdmmc_chipset_handle_t, int);
static int	litesdcard_bus_width(sdmmc_chipset_handle_t, int);
static int	litesdcard_bus_rod(sdmmc_chipset_handle_t, int);
static void	litesdcard_exec_command(sdmmc_chipset_handle_t,
		    struct sdmmc_command *);
static void	litesdcard_card_enable_intr(sdmmc_chipset_handle_t, int);
static void	litesdcard_card_intr_ack(sdmmc_chipset_handle_t);

static struct sdmmc_chip_functions litesdcard_chip_functions = {
	.host_reset		= litesdcard_host_reset,
	.host_ocr		= litesdcard_host_ocr,
	.host_maxblklen		= litesdcard_host_maxblklen,
	.card_detect		= litesdcard_card_detect,
	.write_protect		= litesdcard_write_protect,
	.bus_power		= litesdcard_bus_power,
	.bus_clock		= litesdcard_bus_clock,
	.bus_width		= litesdcard_bus_width,
	.bus_rod		= litesdcard_bus_rod,
	.exec_command		= litesdcard_exec_command,
	.card_enable_intr	= litesdcard_card_enable_intr,
	.card_intr_ack		= litesdcard_card_intr_ack,
};

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "litex,mmc" },
	DEVICE_COMPAT_EOL
};

static int
litesdcard_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args * const ma = aux;

	return mainbus_compatible_match(ma, compat_data);
}

static void
litesdcard_attach(device_t parent, device_t self, void *aux)
{
	struct litesdcard_softc * const sc = device_private(self);
	struct mainbus_attach_args * const ma = aux;
	struct sdmmcbus_attach_args saa;
	bus_addr_t addr;
	bus_size_t size;
	int i;

	sc->sc_dev = self;
	sc->sc_bst = ma->ma_st;
	sc->sc_dmat = ma->ma_dmat;

	for (i = 0; i < REG_COUNT; i++) {
		if (!litex_fdt_reg(ma->ma_fdt_node, i, &addr, &size) ||
		    size < litesdcard_reg_size[i]) {
			aprint_error(": bad reg range %d\n", i);
			return;
		}
		if (bus_space_map(sc->sc_bst, addr, size, 0,
				  &sc->sc_bsh[i]) != 0) {
			aprint_error(": couldn't map registers\n");
			return;
		}
	}

	sc->sc_sys_clk = bootinfo_sys_clk();
	if (sc->sc_sys_clk == 0) {
		aprint_error(": no sys_clk in the bootinfo\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": LiteSDCard\n");

	/*
	 * The PHY's time-outs, in sys clocks, default to 10 ms: too short
	 * for a card's read access time (up to 100 ms) or for a busy
	 * signal at 400 kHz.  Use 250 ms for both.
	 */
	WR(sc, REG_PHY, PHY_CMDR_TIMEOUT, sc->sc_sys_clk / 4);
	WR(sc, REG_PHY, PHY_DATAR_TIMEOUT, sc->sc_sys_clk / 4);

	/* The card is reset to a 1-bit bus before it is used. */
	WR(sc, REG_PHY, PHY_SETTINGS, PHY_SETTINGS_1BIT);

	/* Polled: no events, ever. */
	WR(sc, REG_IRQ, IRQ_ENABLE, 0);
	WR(sc, REG_IRQ, IRQ_PENDING, RD(sc, REG_IRQ, IRQ_PENDING));
	WR(sc, REG_READER, DMA_ENABLE, 0);
	WR(sc, REG_WRITER, DMA_ENABLE, 0);

	memset(&saa, 0, sizeof(saa));
	saa.saa_busname = "sdmmc";
	saa.saa_sct = &litesdcard_chip_functions;
	saa.saa_sch = sc;
	saa.saa_dmat = sc->sc_dmat;
	saa.saa_clkmin = 400;
	saa.saa_clkmax = MIN(25000, sc->sc_sys_clk / 2 / 1000);
	saa.saa_caps = SMC_CAPS_4BIT_MODE | SMC_CAPS_DMA |
	    SMC_CAPS_POLL_CARD_DET;
	saa.saa_max_seg = MAXPHYS;

	sc->sc_sdmmc = config_found(self, &saa, NULL, CFARGS_NONE);
}

static int
litesdcard_host_reset(sdmmc_chipset_handle_t sch)
{
	struct litesdcard_softc *sc = sch;

	WR(sc, REG_READER, DMA_ENABLE, 0);
	WR(sc, REG_WRITER, DMA_ENABLE, 0);
	WR(sc, REG_PHY, PHY_SETTINGS, PHY_SETTINGS_1BIT);
	return 0;
}

static uint32_t
litesdcard_host_ocr(sdmmc_chipset_handle_t sch)
{
	return MMC_OCR_3_2V_3_3V | MMC_OCR_3_3V_3_4V | MMC_OCR_HCS;
}

static int
litesdcard_host_maxblklen(sdmmc_chipset_handle_t sch)
{
	return 512;
}

static int
litesdcard_card_detect(sdmmc_chipset_handle_t sch)
{
	struct litesdcard_softc *sc = sch;

	/* 0 is a card (and always 0 on a board without the pin). */
	return (RD(sc, REG_PHY, PHY_CARD_DETECT) & 1) == 0;
}

static int
litesdcard_write_protect(sdmmc_chipset_handle_t sch)
{
	return 0;
}

static int
litesdcard_bus_power(sdmmc_chipset_handle_t sch, uint32_t ocr)
{
	/* The card's power is fixed. */
	return 0;
}

static int
litesdcard_bus_clock(sdmmc_chipset_handle_t sch, int freq)
{
	struct litesdcard_softc *sc = sch;
	uint32_t div;

	if (freq == 0) {
		return 0;
	}
	div = howmany(sc->sc_sys_clk, (uint32_t)freq * 1000);
	div = MAX(2, MIN(div, 256));
	WR(sc, REG_PHY, PHY_CLOCKER_DIVIDER, div);
	return 0;
}

static int
litesdcard_bus_width(sdmmc_chipset_handle_t sch, int width)
{
	struct litesdcard_softc *sc = sch;
	uint32_t settings;

	/*
	 * The PHY's data width must follow every change the card is
	 * given; it is never assumed (it may be left at 1-bit or 4-bit).
	 */
	switch (width) {
	case 1:
		settings = PHY_SETTINGS_1BIT;
		break;
	case 4:
		settings = PHY_SETTINGS_4BIT;
		break;
	case 8:
		settings = PHY_SETTINGS_8BIT;
		break;
	default:
		return EINVAL;
	}
	WR(sc, REG_PHY, PHY_SETTINGS, settings);
	return 0;
}

static int
litesdcard_bus_rod(sdmmc_chipset_handle_t sch, int on)
{
	return ENOTSUP;
}

static void
litesdcard_card_enable_intr(sdmmc_chipset_handle_t sch, int enable)
{
}

static void
litesdcard_card_intr_ack(sdmmc_chipset_handle_t sch)
{
}

/* Wait for an event register's done bit; returns the event bits. */
static uint32_t
litesdcard_wait(struct litesdcard_softc *sc, bus_size_t reg, int timeout_us)
{
	uint32_t ev;

	for (;;) {
		ev = RD(sc, REG_CORE, reg);
		if (ev & EV_DONE) {
			return ev;
		}
		if (timeout_us <= 0) {
			return EV_TIMEOUT;
		}
		delay(10);
		timeout_us -= 10;
	}
}

static int
litesdcard_wait_dma(struct litesdcard_softc *sc, int dir)
{
	int timeout_us = LITESD_DATA_TIMEOUT_US;

	while ((RD(sc, dir, DMA_DONE) & 1) == 0) {
		if (timeout_us <= 0) {
			return ETIMEDOUT;
		}
		delay(10);
		timeout_us -= 10;
	}
	return 0;
}

static int
litesdcard_event_error(uint32_t ev)
{
	if (ev & EV_TIMEOUT) {
		return ETIMEDOUT;
	}
	if (ev & (EV_CRC | EV_ERROR)) {
		return EIO;
	}
	return 0;
}

static void
litesdcard_exec_command(sdmmc_chipset_handle_t sch, struct sdmmc_command *cmd)
{
	struct litesdcard_softc *sc = sch;
	const bool data = cmd->c_datalen > 0;
	const bool rd = ISSET(cmd->c_flags, SCF_CMD_READ);
	const int dir = rd ? REG_READER : REG_WRITER;
	uint32_t command, ev;
	bus_addr_t pa = 0;
	int error = 0;

	/*
	 * CMD0 puts the card back to a 1-bit bus: so must the PHY be
	 * (sdmmc does not tell us, and litexboot leaves it at 4-bit).
	 * Then the 80 idle clocks a card needs.
	 */
	if (cmd->c_opcode == MMC_GO_IDLE_STATE) {
		WR(sc, REG_PHY, PHY_SETTINGS, PHY_SETTINGS_1BIT);
		WR(sc, REG_PHY, PHY_INIT_INITIALIZE, 1);
		delay(1000);
	}

	if (!ISSET(cmd->c_flags, SCF_RSP_PRESENT)) {
		command = CMD_RSP_NONE;
	} else if (ISSET(cmd->c_flags, SCF_RSP_136)) {
		command = CMD_RSP_LONG;
	} else if (ISSET(cmd->c_flags, SCF_RSP_BSY)) {
		command = CMD_RSP_SHORT_BUSY;
	} else {
		command = CMD_RSP_SHORT;
	}
	if (ISSET(cmd->c_flags, SCF_RSP_CRC)) {
		command |= CMD_RSP_CRC;
	}
	command |= CMD_INDEX(cmd->c_opcode);

	if (data) {
		/* A single, word-aligned segment (no MULTI_SEG_DMA). */
		KASSERT(cmd->c_dmamap != NULL);
		KASSERT(cmd->c_dmamap->dm_nsegs == 1);
		pa = cmd->c_dmamap->dm_segs[0].ds_addr;
		if ((pa & 3) != 0 || (cmd->c_datalen & 3) != 0 ||
		    cmd->c_blklen == 0) {
			error = EINVAL;
			goto out;
		}
		command |= rd ? CMD_DATA_READ : CMD_DATA_WRITE;

		/* The DMA and block count are set before the command. */
		WR(sc, REG_CORE, CORE_BLOCK_LENGTH, cmd->c_blklen);
		WR(sc, REG_CORE, CORE_BLOCK_COUNT,
		    cmd->c_datalen / cmd->c_blklen);
		bus_dmamap_sync(sc->sc_dmat, cmd->c_dmamap, 0, cmd->c_datalen,
		    rd ? BUS_DMASYNC_PREREAD : BUS_DMASYNC_PREWRITE);
		WR(sc, dir, DMA_ENABLE, 0);
		WR(sc, dir, DMA_BASE_HI, 0);
		WR(sc, dir, DMA_BASE_LO, (uint32_t)pa);
		WR(sc, dir, DMA_LENGTH, cmd->c_datalen);
		WR(sc, dir, DMA_ENABLE, 1);
	}

	WR(sc, REG_CORE, CORE_CMD_ARGUMENT, cmd->c_arg);
	WR(sc, REG_CORE, CORE_CMD_COMMAND, command);
	WR(sc, REG_CORE, CORE_CMD_SEND, 1);

	ev = litesdcard_wait(sc, CORE_CMD_EVENT, LITESD_CMD_TIMEOUT_US);
	error = litesdcard_event_error(ev);
	if (error != 0) {
		goto out;
	}

	if (ISSET(cmd->c_flags, SCF_RSP_PRESENT)) {
		if (ISSET(cmd->c_flags, SCF_RSP_136)) {
			/*
			 * The core keeps the response's last 128 bits,
			 * CID/CSD[127:0] (its CRC7 and end bit in the low
			 * byte), most significant word first.  sdmmc wants
			 * [127:8] right-justified, in 32-bit words least
			 * significant first.
			 */
			uint32_t w[4];
			int i;

			for (i = 0; i < 4; i++) {
				w[i] = RD(sc, REG_CORE, CORE_CMD_RESPONSE + 4 * i);
			}
			cmd->c_resp[0] = (w[3] >> 8) | (w[2] << 24);
			cmd->c_resp[1] = (w[2] >> 8) | (w[1] << 24);
			cmd->c_resp[2] = (w[1] >> 8) | (w[0] << 24);
			cmd->c_resp[3] = w[0] >> 8;
		} else {
			/* A short response's 32 bits are the last word. */
			cmd->c_resp[0] = RD(sc, REG_CORE, CORE_CMD_RESPONSE + 12);
		}
	}

	if (data) {
		ev = litesdcard_wait(sc, CORE_DATA_EVENT,
		    LITESD_DATA_TIMEOUT_US);
		error = litesdcard_event_error(ev);
		if (error == 0) {
			error = litesdcard_wait_dma(sc, dir);
		}
		if (error == 0 && RD(sc, dir, DMA_ERROR) != 0) {
			error = EIO;
		}
		if (error == 0 && !rd &&
		    (RD(sc, REG_PHY, PHY_DATAW_STATUS) & 0x6) != 0) {
			error = EIO;	/* the card's CRC or write error */
		}
	}

out:
	if (data) {
		WR(sc, dir, DMA_ENABLE, 0);
		bus_dmamap_sync(sc->sc_dmat, cmd->c_dmamap, 0, cmd->c_datalen,
		    rd ? BUS_DMASYNC_POSTREAD : BUS_DMASYNC_POSTWRITE);
	}
	if (error != 0 && data) {
		device_printf(sc->sc_dev, "CMD%d (%d bytes): error %d, "
		    "cmd event %#x, data event %#x, dma done %#x error %#x\n",
		    cmd->c_opcode, cmd->c_datalen, error,
		    RD(sc, REG_CORE, CORE_CMD_EVENT),
		    RD(sc, REG_CORE, CORE_DATA_EVENT),
		    RD(sc, dir, DMA_DONE), RD(sc, dir, DMA_ERROR));
	}
	cmd->c_error = error;
	SET(cmd->c_flags, SCF_ITSDONE);
}
