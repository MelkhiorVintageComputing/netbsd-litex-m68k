/*	$NetBSD$	*/

/*
 * QEMU m68k virt machine SMP controller registers
 * (QEMU docs/specs/virt-smp.rst).  Same license as virtsmp.c.
 */

#ifndef _VIRT68K_VIRTSMPREG_H_
#define _VIRT68K_VIRTSMPREG_H_

/* banked: registers of the accessing CPU */
#define	VIRTSMP_ID		0x000
#define	VIRTSMP_NCPUS		0x004
#define	VIRTSMP_VERSION		0x008
#define	VIRTSMP_IPI_PENDING	0x010
#define	VIRTSMP_IPI_CLEAR	0x014
#define	VIRTSMP_IPI_CONFIG	0x018
#define	VIRTSMP_TIMER_CTRL	0x020
#define	VIRTSMP_TIMER_ENABLE	__BIT(0)
#define	VIRTSMP_TIMER_PERIODIC	__BIT(1)
#define	VIRTSMP_TIMER_INTERVAL	0x024
#define	VIRTSMP_TIMER_STATUS	0x028
#define	VIRTSMP_TIMER_EXPIRED	__BIT(0)
#define	VIRTSMP_TIMER_CONFIG	0x02c

/* global */
#define	VIRTSMP_IPI_SEND	0x100
#define	VIRTSMP_IPI_REASON_SHIFT 16
#define	VIRTSMP_DEVIRQ_ROUTE	0x110
#define	VIRTSMP_COUNTER_LO	0x120
#define	VIRTSMP_COUNTER_HI	0x124
#define	VIRTSMP_CPU(n)		(0x200 + 16 * (n))
#define	VIRTSMP_START_PC	0x0
#define	VIRTSMP_START_SP	0x4
#define	VIRTSMP_START_ARG	0x8
#define	VIRTSMP_CPU_CTRL	0xc

/* reset values of IPI_CONFIG and TIMER_CONFIG (both at IPL 6) */
#define	VIRTSMP_IPI_VECTOR	0x40
#define	VIRTSMP_TIMER_VECTOR	0x41

#endif /* _VIRT68K_VIRTSMPREG_H_ */
