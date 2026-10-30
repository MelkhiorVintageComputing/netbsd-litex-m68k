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
 * The SMP block of LiteX-68K's two-processor builds (litex-68k's
 * doc/l18-smp.md, litex_68k/m68040_smp.py): long words, at
 * BI_LITEX_SMP's base ($FFFF.0000, under the I/O TT).
 */

#ifndef _LITEX68K_DEV_LITEXSMPREG_H_
#define _LITEX68K_DEV_LITEXSMPREG_H_

#define	LITEXSMP_WHOAMI		0x00	/* R: the processor reading */
#define	LITEXSMP_IPI_SET	0x04	/* W: interrupt CPU n (the data) */
#define	LITEXSMP_IPI_ACK	0x08	/* W: acknowledge one's own */
#define	LITEXSMP_RELEASE	0x0c	/* W: non-zero lets CPU 1 out of reset */
#define	LITEXSMP_BOOT_PC	0x10	/* RW: where the ROM sends CPU 1 */
#define	LITEXSMP_BOOT_SP	0x14	/* RW: its stack */
#define	LITEXSMP_NCPUS		0x18	/* R */
#define	LITEXSMP_STATS		0x20	/* R: the counters; W: clears them */
#define	LITEXSMP_NSTATS		11
#define	LITEXSMP_SIZE		0x100

/* The interprocessor interrupt: level 5, autovectored (vector 29). */
#define	LITEXSMP_IPI_IPL	5

#endif /* _LITEX68K_DEV_LITEXSMPREG_H_ */
