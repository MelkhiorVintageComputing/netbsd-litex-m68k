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
 * fptest: FPU context switching.
 *
 *	fptest [nproc [rounds]]
 *
 * Each process runs a floating-point workload that keeps many FP
 * registers live, with a seed of its own, and compares each round's
 * result with the one computed once by the parent alone before the
 * fork.  The processes preempt each other (and take SIGALRMs whose
 * handler also uses the FPU), so a lost or mixed-up FPU context on a
 * switch or a signal shows up as a mismatch.
 */

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define	ITER	20000

static volatile double sig_acc;

static void
alrm(int sig)
{
	volatile double a = 1.0, b = 3.0;
	int i;

	/* Clobber the FPU in the handler. */
	for (i = 0; i < 50; i++)
		a = a * 1.0001 + sqrt(b + i) / (b * i + 1.0);
	sig_acc += a;
}

static double
work(int seed)
{
	double a = 1.0 + seed, b = 0.5, c = 0.25, d = 0.125;
	double e = 1.5, f = 2.5, g = 3.5, h = 4.5;
	int i;

	for (i = 0; i < ITER; i++) {
		a = a * 0.999 + sin(b) * 0.01;
		b = b + cos(c) * 0.001 + a * 1e-6;
		c = c * 1.0001 + atan(d) * 1e-3;
		d = sqrt(d * d + e * 1e-4);
		e = e - f * 1e-5 + g * 1e-6;
		f = f + exp(-h) * 1e-3;
		g = g * 0.99999 + log(1.0 + a * a) * 1e-4;
		h = fmod(h + c, 7.0) + 0.5;
	}
	return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}

int
main(int argc, char **argv)
{
	int nproc = argc > 1 ? atoi(argv[1]) : 4;
	int rounds = argc > 2 ? atoi(argv[2]) : 20;
	double *expect;
	struct itimerval it;
	int i, status, failed = 0;

	expect = calloc(nproc, sizeof(*expect));
	for (i = 0; i < nproc; i++)
		expect[i] = work(i);
	printf("fptest: %d processes, %d rounds of %d iterations\n",
	    nproc, rounds, ITER);
	fflush(stdout);

	for (i = 0; i < nproc; i++) {
		if (fork() == 0) {
			int r, bad = 0;

			signal(SIGALRM, alrm);
			memset(&it, 0, sizeof(it));
			it.it_interval.tv_usec = 7000 + 1000 * i;
			it.it_value = it.it_interval;
			setitimer(ITIMER_REAL, &it, NULL);
			for (r = 0; r < rounds; r++) {
				double v = work(i);
				if (v != expect[i]) {
					printf("fptest: proc %d round %d: "
					    "%.17g != %.17g\n", i, r, v,
					    expect[i]);
					bad++;
				}
			}
			_exit(bad != 0);
		}
	}
	for (i = 0; i < nproc; i++) {
		wait(&status);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			failed++;
	}
	printf("fptest: %s (%d of %d processes failed)\n",
	    failed ? "FAILED" : "PASSED", failed, nproc);
	return failed != 0;
}
