/*	$NetBSD: intr.c,v 1.4 2024/11/01 14:28:08 mlelstv Exp $	*/

/*-
 * Copyright (c) 1996, 2023 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Adam Glass, Gordon W. Ross, and Jason R. Thorpe.
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
 * Link and dispatch interrupts.
 *
 * LiteX-68K's CPU interrupt controller is two CSRs at the base the
 * bootinfo gives (BI_LITEX_IRQCTRL):
 *
 *	+0	irq_pending	RO	bit n: line n is up now
 *	+4	irq_mask	RW	bit n: line n may raise IPL 2
 *
 * Any line with pending & mask set holds IPL 2 up, autovectored.
 * Lines are levels and are acknowledged at the device, never here.
 * A line is unmasked only once a handler is established for it, which
 * drivers do after setting up their core's events.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#define _LITEX68K_INTR_PRIVATE

#include <sys/param.h>
#include <sys/bitops.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/vmmeter.h>
#include <sys/device.h>
#include <sys/cpu.h>
#include <sys/bus.h>
#include <sys/intr.h>

#include <machine/bootinfo.h>
#include <machine/vectors.h>

#include <uvm/uvm_extern.h>

#define	IRQCTRL_PENDING		0x0
#define	IRQCTRL_MASK		0x4

struct intrhand {
	LIST_ENTRY(intrhand)	ih_link;
	int			(*ih_func)(void *);
	void			*ih_arg;
	struct evcnt		*ih_evcnt;
	int			ih_irq;
};

static const char * const cpu_irq_group = "cpu irq";
static const char * const litex_irq_group = "litex irq";

static const char * const litex_irq_names[] = {
	"line 0",  "line 1",  "line 2",  "line 3",
	"line 4",  "line 5",  "line 6",  "line 7",
	"line 8",  "line 9",  "line 10", "line 11",
	"line 12", "line 13", "line 14", "line 15",
	"line 16", "line 17", "line 18", "line 19",
	"line 20", "line 21", "line 22", "line 23",
	"line 24", "line 25", "line 26", "line 27",
	"line 28", "line 29", "line 30", "line 31",
};
__CTASSERT(__arraycount(litex_irq_names) == NIRQ);

static struct evcnt cpu_evcnt[] = {
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "spur"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev1"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev2"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev3"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev4"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev5"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "lev6"),
	EVCNT_INITIALIZER(EVCNT_TYPE_INTR, NULL, NULL, "nmi"),
};

static struct evcnt litex_evcnt[NIRQ];

static LIST_HEAD(intrhand_list, intrhand) intrhands[NIRQ];

volatile unsigned int	intr_depth;	/* used in locore.s */

static volatile uint32_t *irqctrl;	/* the CSRs, mapped by TT0 */
static uint32_t		irq_mask;	/* soft copy of irq_mask */

/*
 * intr_init --
 *	Initialize the interrupt subsystem.
 */
void
intr_init(void)
{
	struct bi_litex_dev *ld;
	int i;

	for (i = 0; i < __arraycount(cpu_evcnt); i++) {
		cpu_evcnt[i].ev_group = cpu_irq_group;
		evcnt_attach_static(&cpu_evcnt[i]);
	}
	for (i = 0; i < NIRQ; i++) {
		LIST_INIT(&intrhands[i]);
		evcnt_attach_dynamic(&litex_evcnt[i], EVCNT_TYPE_INTR, NULL,
		    litex_irq_group, litex_irq_names[i]);
	}

	ld = bootinfo_litex_dev(BI_LITEX_IRQCTRL);
	if (ld == NULL) {
		panic("no interrupt controller in the bootinfo");
	}
	if (ld->ld_irq != LITEX_IRQ_IPL) {
		panic("interrupt controller on IPL %u, expected %d",
		    ld->ld_irq, LITEX_IRQ_IPL);
	}
	irqctrl = (volatile uint32_t *)ld->ld_base;

	/* Everything masked until a handler is established. */
	irq_mask = 0;
	irqctrl[IRQCTRL_MASK / 4] = irq_mask;
}

/*
 * intr_establish --
 *	Establish an interrupt handler at the specified system IRQ,
 *	and unmask its line.
 *	XXX We don't do anything with the flags yet.
 */
void *
intr_establish(int (*func)(void *), void *arg, int irq, int ipl,
    int flags __unused)
{
	struct intrhand *ih;
	int s;

	if (irq < 0 || irq >= NIRQ) {
		return NULL;
	}
	if (ipl < IPL_VM || ipl > IPL_SCHED) {
		return NULL;
	}

	ih = kmem_zalloc(sizeof(*ih), KM_SLEEP);
	ih->ih_func = func;
	ih->ih_arg = arg;
	ih->ih_irq = irq;
	ih->ih_evcnt = &litex_evcnt[irq];

	s = splhigh();
	LIST_INSERT_HEAD(&intrhands[irq], ih, ih_link);
	irq_mask |= __BIT(irq);
	irqctrl[IRQCTRL_MASK / 4] = irq_mask;
	splx(s);

	return ih;
}

/*
 * intr_disestablish --
 *	Remove an interrupt handler, masking its line if it was the
 *	last one.
 */
void
intr_disestablish(void *v)
{
	struct intrhand *ih = v;
	int s;

	s = splhigh();
	LIST_REMOVE(ih, ih_link);
	if (LIST_EMPTY(&intrhands[ih->ih_irq])) {
		irq_mask &= ~__BIT(ih->ih_irq);
		irqctrl[IRQCTRL_MASK / 4] = irq_mask;
	}
	splx(s);

	kmem_free(ih, sizeof(*ih));
}

/*
 * intr_string --
 *	Return a string in the specified buffer describing the
 *	interrupt.
 */
const char *
intr_string(void *v, char *buf, size_t bufsize)
{
	struct intrhand *ih = v;

	snprintf(buf, bufsize, "%s %s (IPL %d)", ih->ih_evcnt->ev_group,
	    ih->ih_evcnt->ev_name, LITEX_IRQ_IPL);
	return buf;
}

/* Auto-vectored interrupts start at vector 0x18. */
#define	VEC_AVINTR	0x18

void
intr_dispatch(struct clockframe frame)
{
	const int ipl = VECO_TO_VECI(frame.cf_vo) - VEC_AVINTR;
	struct intrhand *ih;
	uint32_t pending;
	int irq;

	curcpu()->ci_data.cpu_nintr++;

	if (__predict_false(ipl != LITEX_IRQ_IPL)) {
		if (ipl >= 0 && ipl < __arraycount(cpu_evcnt)) {
			cpu_evcnt[ipl].ev_count++;
		}
		return;
	}
	cpu_evcnt[ipl].ev_count++;

	/*
	 * Run the handlers of every line that is up, lowest first, until
	 * none is.  A level 2 interrupt with nothing pending (a line
	 * that dropped before we got here) is ignored.
	 */
	while ((pending = irqctrl[IRQCTRL_PENDING / 4] & irq_mask) != 0) {
		irq = ffs32(pending) - 1;
		bool claimed = false;
		LIST_FOREACH(ih, &intrhands[irq], ih_link) {
			void *arg = ih->ih_arg ? ih->ih_arg : &frame;
			if (ih->ih_func(arg)) {
				ih->ih_evcnt->ev_count++;
				claimed = true;
			}
		}
		if (__predict_false(!claimed)) {
			/*
			 * Nobody acknowledged it at its device: it would
			 * hold IPL 2 up forever.  Mask it.
			 */
			printf("litex irq line %d: stray interrupt, "
			    "masking it\n", irq);
			irq_mask &= ~__BIT(irq);
			irqctrl[IRQCTRL_MASK / 4] = irq_mask;
		}
	}
}

bool
cpu_intr_p(void)
{
	return intr_depth != 0;
}
