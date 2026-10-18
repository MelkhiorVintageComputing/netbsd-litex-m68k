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

#ifndef _VIRT68K_CPU_COUNTER_H_
#define _VIRT68K_CPU_COUNTER_H_

/*
 * The CPU counter is the nanosecond counter of the virt-smp controller
 * (virtsmp(4)), so it only exists in MULTIPROCESSOR kernels (see
 * <machine/types.h>), and only once virtsmp has attached.
 */

#ifdef _KERNEL

#include <machine/cpu.h>
#include <machine/intr.h>

/* COUNTER_LO of virtsmp(4); COUNTER_HI follows it */
extern volatile uint32_t *virt68k_counter_reg;

#define	cpu_hascounter()	(virt68k_counter_reg != NULL)

static __inline uint32_t
cpu_counter32(void)
{
	volatile uint32_t *r = virt68k_counter_reg;

	return r != NULL ? r[0] : 0;
}

static __inline uint64_t
cpu_counter(void)
{
	volatile uint32_t *r = virt68k_counter_reg;
	uint32_t lo, hi;
	int s;

	if (r == NULL)
		return 0;
	/* reading COUNTER_LO latches COUNTER_HI for this CPU */
	s = splhigh();
	lo = r[0];
	hi = r[1];
	splx(s);
	return ((uint64_t)hi << 32) | lo;
}

static __inline uint64_t
cpu_frequency(struct cpu_info *ci)
{
	return 1000000000;
}

#endif /* _KERNEL */

#endif /* !_VIRT68K_CPU_COUNTER_H_ */
