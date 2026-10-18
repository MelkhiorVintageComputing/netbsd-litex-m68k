/*	$NetBSD: types.h,v 1.1 2024/01/02 07:41:01 thorpej Exp $	*/

#ifndef _MACHINE_TYPES_H_
#define	_MACHINE_TYPES_H_

#if defined(_KERNEL_OPT)
#include "opt_multiprocessor.h"
#endif

#include <m68k/types.h>

#define	__HAVE_DEVICE_REGISTER

#if defined(_KERNEL) && defined(MULTIPROCESSOR)
/* the virt-smp nanosecond counter, see <machine/cpu_counter.h> */
#define	__HAVE_CPU_COUNTER
#endif

#endif
