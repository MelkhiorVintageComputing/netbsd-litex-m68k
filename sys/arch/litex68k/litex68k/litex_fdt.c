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
 * The flattened device tree litexboot passes (BI_LITEX_DTB), made by
 * LiteX's litex_json2dts_linux from the SoC's csr.json.  The bootinfo
 * describes the SoC's core devices; the device tree is read for the
 * others (LiteEth, LiteSDCard), whose register ranges and parameters it
 * has.  Its /soc is a simple-bus with one address and one size cell, and
 * an interrupt specifier is a line number of the CPU's controller.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <libfdt.h>

#include <machine/bootinfo.h>

#include <litex68k/litex68k/litex_fdt.h>

static const void *litex_fdt;

/*
 * litex_fdt_init --
 *	Map the device tree, if litexboot passed one.  Its pages were
 *	kept out of the VM system when the bootinfo was parsed.
 */
void
litex_fdt_init(void)
{
	paddr_t pa, spa;
	psize_t size, off;
	vaddr_t va;

	if (litex_fdt != NULL || !bootinfo_dtb(&pa, &size)) {
		return;
	}

	spa = m68k_trunc_page(pa);
	size = m68k_round_page(pa + size) - spa;
	va = uvm_km_alloc(kernel_map, size, 0, UVM_KMF_VAONLY);
	if (va == 0) {
		printf("WARNING: no KVA for the device tree\n");
		return;
	}
	for (off = 0; off < size; off += PAGE_SIZE) {
		pmap_kenter_pa(va + off, spa + off, VM_PROT_READ, 0);
	}
	pmap_update(pmap_kernel());

	if (fdt_check_header((const void *)(va + (pa - spa))) != 0) {
		printf("WARNING: bad device tree at 0x%08lx\n", pa);
		return;
	}
	litex_fdt = (const void *)(va + (pa - spa));
}

const void *
litex_fdt_data(void)
{
	return litex_fdt;
}

/*
 * litex_fdt_next_soc_device --
 *	Iterate over the enabled children of /soc: start with -1, stop
 *	at a negative return.
 */
int
litex_fdt_next_soc_device(int node)
{
	const char *status;
	int soc;

	if (litex_fdt == NULL) {
		return -1;
	}
	soc = fdt_path_offset(litex_fdt, "/soc");
	if (soc < 0) {
		return -1;
	}
	for (;;) {
		node = (node < 0) ? fdt_first_subnode(litex_fdt, soc)
				  : fdt_next_subnode(litex_fdt, node);
		if (node < 0) {
			return -1;
		}
		status = fdt_getprop(litex_fdt, node, "status", NULL);
		if (status == NULL || strcmp(status, "okay") == 0 ||
		    strcmp(status, "ok") == 0) {
			return node;
		}
	}
}

/* The node's first compatible string. */
const char *
litex_fdt_compatible(int node)
{
	return fdt_getprop(litex_fdt, node, "compatible", NULL);
}

/* The idx'th (address, size) of the node's reg. */
bool
litex_fdt_reg(int node, int idx, bus_addr_t *addrp, bus_size_t *sizep)
{
	const fdt32_t *reg;
	int len;

	reg = fdt_getprop(litex_fdt, node, "reg", &len);
	if (reg == NULL || len < (idx + 1) * 8) {
		return false;
	}
	*addrp = fdt32_to_cpu(reg[idx * 2]);
	*sizep = fdt32_to_cpu(reg[idx * 2 + 1]);
	return true;
}

bool
litex_fdt_u32(int node, const char *prop, uint32_t *valp)
{
	const fdt32_t *val;
	int len;

	val = fdt_getprop(litex_fdt, node, prop, &len);
	if (val == NULL || len < 4) {
		return false;
	}
	*valp = fdt32_to_cpu(val[0]);
	return true;
}

const void *
litex_fdt_prop(int node, const char *prop, int *lenp)
{
	return fdt_getprop(litex_fdt, node, prop, lenp);
}

/* The node's interrupt line, or -1. */
int
litex_fdt_irq(int node)
{
	uint32_t irq;

	return litex_fdt_u32(node, "interrupts", &irq) ? (int)irq : -1;
}
