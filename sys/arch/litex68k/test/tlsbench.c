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
 * tlsbench: what a thread-pointer read costs.  On m68k each one is the
 * _lwp_getprivate syscall (__m68k_read_tp), and jemalloc makes several per
 * malloc/free pair, so this times, in microseconds per iteration:
 *
 *	getppid			an ordinary syscall, for comparison
 *	_lwp_getprivate		the syscall itself
 *	__thread access		what the compiler emits for a TLS variable
 *	malloc+free		a 64-byte malloc and its free
 *	empty loop		the loop alone
 *
 *	tlsbench [n]		n iterations of each (default 10000)
 *
 * The value read is checked against _lwp_getprivate's, so a wrong fast
 * path shows as an error rather than as a good time.
 */
#include <err.h>
#include <lwp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static __thread int tlsvar;

/* Not inlined, so that each call reads the thread pointer. */
static void __noinline
bumptls(void)
{

	tlsvar++;
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void
report(const char *what, double t, int n)
{

	printf("%-18s %9.2f us\n", what, (now() - t) * 1e6 / n);
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 10000;
	void *volatile sink;
	void *tp;
	double t;
	int i;

	if (n <= 0)
		errx(1, "usage: tlsbench [n]");
	setvbuf(stdout, NULL, _IONBF, 0);

	tp = _lwp_getprivate();
	if (tp == NULL)
		errx(1, "_lwp_getprivate returned NULL");

	t = now();
	for (i = 0; i < n; i++)
		sink = (void *)(long)getppid();
	report("getppid", t, n);

	t = now();
	for (i = 0; i < n; i++) {
		sink = _lwp_getprivate();
		if (sink != tp)
			errx(1, "_lwp_getprivate: %p, not %p", sink, tp);
	}
	report("_lwp_getprivate", t, n);

	t = now();
	for (i = 0; i < n; i++)
		bumptls();
	report("__thread access", t, n);
	if (tlsvar != n)
		errx(1, "__thread: %d, not %d", tlsvar, n);

	t = now();
	for (i = 0; i < n; i++) {
		sink = malloc(64);
		if (sink == NULL)
			err(1, "malloc");
		free(sink);
	}
	report("malloc+free", t, n);

	t = now();
	for (i = 0; i < n; i++)
		sink = (void *)(long)i;
	report("empty loop", t, n);
	return 0;
}
