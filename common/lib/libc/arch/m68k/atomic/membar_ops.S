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

#include "atomic_op_asm.h"

/*
 * Memory barriers.  The MC68040 completes reads in order and writes in
 * order, but may delay a write while later reads complete ("reordering
 * only occurs with writes relative to reads", M68040UM 7.7): the same
 * model as x86 TSO.  So only store-before-load ordering needs an
 * instruction; NOP waits until all pending bus cycles have completed
 * (M68040UM 7.7).  The other barriers need nothing but the compiler
 * barrier that the function call is.
 */

	.text

ENTRY(_membar_sync)
	nop
	rts
END(_membar_sync)

ENTRY(_membar_release)
	rts
END(_membar_release)

ATOMIC_OP_ALIAS(membar_sync,_membar_sync)
CRT_ALIAS(__sync_synchronize,_membar_sync)
/* membar_enter (deprecated) orders stores before later loads too */
ATOMIC_OP_ALIAS(membar_enter,_membar_sync)
STRONG_ALIAS(_membar_enter,_membar_sync)

ATOMIC_OP_ALIAS(membar_release,_membar_release)
ATOMIC_OP_ALIAS(membar_acquire,_membar_release)
STRONG_ALIAS(_membar_acquire,_membar_release)
ATOMIC_OP_ALIAS(membar_exit,_membar_release)
STRONG_ALIAS(_membar_exit,_membar_release)
ATOMIC_OP_ALIAS(membar_producer,_membar_release)
STRONG_ALIAS(_membar_producer,_membar_release)
ATOMIC_OP_ALIAS(membar_consumer,_membar_release)
STRONG_ALIAS(_membar_consumer,_membar_release)
