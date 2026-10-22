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
 * fpstress: each FPU operation alone, repeated over a table of inputs and
 * compared bit for bit with its first pass.
 *
 *	fpstress [passes]
 *
 * Run two at once (fpstress & fpstress) so that they preempt each other:
 * the FPU is then often busy at a context switch's FSAVE.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>

#define	N	1000

static double
f_mul(double x) { return x * 1.000001 + x * x; }
static double
f_div(double x) { return 1.0 / (x + 0.5); }
static double
f_fsin(double x) { return sin(x); }
static double
f_fcos(double x) { return cos(x); }
static double
f_fatan(double x) { return atan(x); }
static double
f_fsqrt(double x) { return sqrt(x); }
static double
f_fetox(double x) { return exp(-x); }
static double
f_flogn(double x) { return log(1.0 + x); }
static double
f_fmod(double x) { return fmod(x * 3.3, 7.0); }

static const struct {
	const char *name;
	double (*fn)(double);
} ops[] = {
	{ "fmul/fadd", f_mul }, { "fdiv", f_div }, { "fsin", f_fsin },
	{ "fcos", f_fcos }, { "fatan", f_fatan }, { "fsqrt", f_fsqrt },
	{ "fetox", f_fetox }, { "flogn", f_flogn }, { "fmod", f_fmod },
};

int
main(int argc, char **argv)
{
	int passes = argc > 1 ? atoi(argv[1]) : 50;
	static double in[N], ref[N];
	struct rusage r0, r1;
	unsigned o;
	int i, p, bad, nan, total = 0;

	for (i = 0; i < N; i++)
		in[i] = 0.25 + i * 0.00573;
	for (o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
		for (i = 0; i < N; i++)
			ref[i] = ops[o].fn(in[i]);
		getrusage(RUSAGE_SELF, &r0);
		bad = nan = 0;
		for (p = 0; p < passes; p++)
			for (i = 0; i < N; i++) {
				volatile double v = ops[o].fn(in[i]);
				if (memcmp((const void *)&v, &ref[i],
				    sizeof(v)) != 0) {
					if (bad < 3)
						printf("  %s(%a): %a != %a\n",
						    ops[o].name, in[i], v,
						    ref[i]);
					bad++;
					if (isnan(v))
						nan++;
				}
			}
		getrusage(RUSAGE_SELF, &r1);
		printf("%-10s %7d ops, %5d wrong (%d NaN), "
		    "%ld vol / %ld invol switches\n", ops[o].name,
		    passes * N, bad, nan, r1.ru_nvcsw - r0.ru_nvcsw,
		    r1.ru_nivcsw - r0.ru_nivcsw);
		fflush(stdout);
		total += bad;
	}
	printf("fpstress: %s\n", total ? "FAILED" : "PASSED");
	return total != 0;
}
