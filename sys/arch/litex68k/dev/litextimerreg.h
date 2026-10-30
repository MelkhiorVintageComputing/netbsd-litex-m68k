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
 * The registers of a LiteX timer (Timer0, and Timer1 on the two-CPU
 * builds), relative to its CSR base.
 */

#ifndef _LITEX68K_DEV_LITEXTIMERREG_H_
#define _LITEX68K_DEV_LITEXTIMERREG_H_

#define	TIMER_LOAD		0x00
#define	TIMER_RELOAD		0x04
#define	TIMER_EN		0x08
#define	TIMER_UPDATE_VALUE	0x0c
#define	TIMER_VALUE		0x10
#define	TIMER_EV_STATUS		0x14
#define	TIMER_EV_PENDING	0x18
#define	TIMER_EV_ENABLE		0x1c
#define	TIMER_UPTIME_LATCH	0x20
#define	TIMER_UPTIME_HI		0x24
#define	TIMER_UPTIME_LO		0x28
#define	TIMER_SIZE		0x2c

#define	TIMER_EV_ZERO		__BIT(0)

#endif /* _LITEX68K_DEV_LITEXTIMERREG_H_ */
