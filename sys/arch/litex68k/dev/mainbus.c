/*	$NetBSD: mainbus.c,v 1.1 2024/01/02 07:40:59 thorpej Exp $	*/

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
__KERNEL_RCSID(0, "$NetBSD: mainbus.c,v 1.1 2024/01/02 07:40:59 thorpej Exp $");

#define _LITEX68K_BUS_DMA_PRIVATE
#define _LITEX68K_BUS_SPACE_PRIVATE

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/bus.h>
#include <sys/intr.h>

#include <machine/bootinfo.h>
#include <machine/cpu.h>

#include <litex68k/dev/mainbusvar.h>
#include <litex68k/litex68k/litex_fdt.h>

struct litex68k_bus_dma_tag _mainbus_dma_tag = {
	NULL,
	_bus_dmamap_create,
	_bus_dmamap_destroy,
	_bus_dmamap_load_direct,
	_bus_dmamap_load_mbuf_direct,
	_bus_dmamap_load_uio_direct,
	_bus_dmamap_load_raw_direct,
	_bus_dmamap_unload,
	NULL,			/* Set up at run-time */
	_bus_dmamem_alloc,
	_bus_dmamem_free,
	_bus_dmamem_map,
	_bus_dmamem_unmap,
	_bus_dmamem_mmap
};

struct litex68k_bus_space_tag _mainbus_space_tag = {
	NULL,
	_bus_space_map,
	_bus_space_unmap,
	_bus_space_peek_1,
	_bus_space_peek_2,
	_bus_space_peek_4,
	_bus_space_poke_1,
	_bus_space_poke_2,
	_bus_space_poke_4
};

static int
mainbus_print(void *aux, const char *cp)
{
	struct mainbus_attach_args *ma = aux;

	if (cp) {
		aprint_normal("%s at %s", ma->ma_compatible, cp);
	}

	aprint_normal(" addr 0x%lx", ma->ma_addr);

	return UNCONF;
}

static int
mainbus_match(device_t parent __unused, cfdata_t cf __unused,
    void *args __unused)
{
	static int mainbus_matched;

	if (mainbus_matched)
		return 0;

	return (mainbus_matched = 1);
}

/*
 * The SoC's core devices, as litexboot describes them in the bootinfo.
 * The compatible strings are those of LiteX's device tree, plus
 * "litex,timer" for Timer0, which the device tree does not describe.
 */
static const struct {
	uint16_t	tag;
	const char	*compatible;
	bus_size_t	size;
} mainbus_litex_devs[] = {
	{ BI_LITEX_CTRL,	"litex,soc-controller",	0x0c },
	{ BI_LITEX_TIMER0,	"litex,timer",		0x2c },
	{ BI_LITEX_UART,	"litex,liteuart",	0x20 },
};

static bool
mainbus_attach_litex(struct bi_record *bi, void *v)
{
	struct mainbus_attach_args ma = {
		.ma_st = &_mainbus_space_tag,
		.ma_dmat = &_mainbus_dma_tag,
		.ma_fdt_node = -1,
	};
	device_t self = v;
	struct bi_litex_dev *ld = bootinfo_dataptr(bi);
	int i;

	for (i = 0; i < __arraycount(mainbus_litex_devs); i++) {
		if (bi->bi_tag == mainbus_litex_devs[i].tag) {
			ma.ma_compatible = mainbus_litex_devs[i].compatible;
			ma.ma_addr = ld->ld_base;
			ma.ma_size = mainbus_litex_devs[i].size;
			ma.ma_irq = (bi->bi_tag == BI_LITEX_CTRL) ? -1
								  : ld->ld_irq;
			config_found(self, &ma, mainbus_print, CFARGS_NONE);
			break;
		}
	}
	return true;		/* keep searching */
}

/*
 * The cores the bootinfo does not describe, from the device tree.
 */
static const char * const mainbus_fdt_compats[] = {
	"litex,liteeth",
	"litex,mmc",
};

static void
mainbus_attach_fdt(device_t self)
{
	struct mainbus_attach_args ma = {
		.ma_st = &_mainbus_space_tag,
		.ma_dmat = &_mainbus_dma_tag,
	};
	const char *compat;
	int node, i;

	for (node = litex_fdt_next_soc_device(-1); node >= 0;
	     node = litex_fdt_next_soc_device(node)) {
		compat = litex_fdt_compatible(node);
		if (compat == NULL) {
			continue;
		}
		for (i = 0; i < __arraycount(mainbus_fdt_compats); i++) {
			if (strcmp(compat, mainbus_fdt_compats[i]) == 0) {
				break;
			}
		}
		if (i == __arraycount(mainbus_fdt_compats) ||
		    !litex_fdt_reg(node, 0, &ma.ma_addr, &ma.ma_size)) {
			continue;
		}
		ma.ma_compatible = compat;
		ma.ma_irq = litex_fdt_irq(node);
		ma.ma_fdt_node = node;
		config_found(self, &ma, mainbus_print, CFARGS_NONE);
	}
}

static void
mainbus_attach(device_t parent __unused, device_t self, void *args __unused)
{

	printf("\n");

	_mainbus_dma_tag._dmamap_sync =
	    (mmutype == MMU_68040) ? _bus_dmamap_sync_0460
				   : _bus_dmamap_sync_030;

	bootinfo_enumerate(mainbus_attach_litex, self);

	litex_fdt_init();
	mainbus_attach_fdt(self);
}

int
mainbus_compatible_match(const struct mainbus_attach_args * const ma,
   const struct device_compatible_entry * driver_compats)
{
	const char *device_compats[1] = {
		[0] = ma->ma_compatible,
	};
	return device_compatible_match(device_compats,
	    __arraycount(device_compats), driver_compats);
}

const struct device_compatible_entry *
mainbus_compatible_lookup(const struct mainbus_attach_args * const ma,
    const struct device_compatible_entry * driver_compats)
{
	const char *device_compats[1] = {
		[0] = ma->ma_compatible,
	};
	return device_compatible_lookup(device_compats,
	    __arraycount(device_compats), driver_compats);
}

CFATTACH_DECL_NEW(mainbus, 0,
    mainbus_match, mainbus_attach, NULL, NULL);
