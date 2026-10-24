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
 * fpround: an FDIV.D whose operand straddles a page boundary, the second
 * page faulting, so that the FPU is saved busy in the middle of the
 * operand transfer.  The result, rounded to nearest, is compared bit for
 * bit with the same division done without a fault.
 *
 *	fpround sig [n]		SIGSEGV; the handler divides rounding to zero
 *	fpround yield [n]	SIGSEGV; the handler yields to "fpround spin"
 *	fpround zfod [n]	zero-fill faults, with "fpround spin" running
 *	fpround ctl [n]		as sig, but the fault is an integer load just
 *				before the division (checks sigreturn's FPCR)
 *	fpround spin		divide rounding to zero, forever
 *
 * A wrong result equal to the one rounded to zero is the instruction
 * having rounded as the work run while it was saved.
 */
#include <sys/mman.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	FPCR_RZ	0x10

static char *base;
static size_t pgsz;
static int yield;

static long double
fdivd(const double *p)
{
	long double r = 1.0L;

	__asm volatile("fdivd %1@,%0" : "+f"(r) : "a"(p) : "memory");
	return r;
}

static void
set_fpcr(unsigned int v)
{

	__asm volatile("fmovel %0,%%fpcr" : : "d"(v));
}

static void
segv(int sig, siginfo_t *si, void *ctx)
{
	static volatile double three = 3.0;

	(void)sig; (void)si; (void)ctx;
	set_fpcr(FPCR_RZ);
	(void)fdivd((const double *)&three);
	if (mprotect(base + pgsz, pgsz, PROT_READ | PROT_WRITE) == -1)
		abort();
	if (yield)
		sched_yield();
}

static void
put(double *p, double v)
{

	memcpy(p, &v, sizeof v);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "sig";
	long n = argc > 2 ? atol(argv[2]) : 20000;
	double *p;
	long double ref, rz, r;
	long i, bad = 0, asrz = 0;
	struct sigaction sa;

	pgsz = (size_t)getpagesize();
	base = mmap(NULL, 2 * pgsz, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (base == MAP_FAILED)
		return 2;
	p = (double *)(base + pgsz - 4);

	if (strcmp(mode, "spin") == 0) {
		put(p, 7.0);
		set_fpcr(FPCR_RZ);
		for (;;)
			(void)fdivd(p);
	}

	/* 1/3 differs in its last bit between nearest and zero. */
	put(p, 3.0);
	if (strcmp(mode, "zfod") == 0) {
		/* The second page reads as zeroes after a zero fill. */
		memset(base + pgsz, 0, 4);
	}
	ref = fdivd(p);
	set_fpcr(FPCR_RZ);
	rz = fdivd(p);
	set_fpcr(0);
	if (memcmp(&ref, &rz, sizeof ref) == 0) {
		printf("fpround: nearest and zero agree, no test\n");
		return 2;
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	yield = strcmp(mode, "yield") == 0;

	for (i = 0; i < n; i++) {
		if (strcmp(mode, "zfod") == 0) {
			if (mmap(base + pgsz, pgsz, PROT_READ | PROT_WRITE,
			    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) ==
			    MAP_FAILED)
				return 2;
		} else if (mprotect(base + pgsz, pgsz, PROT_NONE) == -1)
			return 2;
		if (strcmp(mode, "ctl") == 0)
			(void)*(volatile int *)(base + pgsz);
		r = fdivd(p);
		if (memcmp(&r, &ref, sizeof r) != 0) {
			bad++;
			if (memcmp(&r, &rz, sizeof r) == 0)
				asrz++;
		}
	}
	printf("fpround %s: %ld divisions, %ld wrong, %ld of them rounded "
	    "to zero: %s\n", mode, n, bad, asrz, bad ? "FAILED" : "PASSED");
	return bad != 0;
}
