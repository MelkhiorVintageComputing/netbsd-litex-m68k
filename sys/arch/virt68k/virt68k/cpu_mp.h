/*	$NetBSD$	*/

/*
 * MULTIPROCESSOR glue between the virt68k MD code and virtsmp(4).
 * (Same license as cpu_mp.c.)
 */

#ifndef _VIRT68K_CPU_MP_H_
#define _VIRT68K_CPU_MP_H_

#ifdef _KERNEL
/* IPI reasons: bit numbers in the virt-smp IPI_PENDING register */
#define	VIRT68K_IPI_NOP		0
#define	VIRT68K_IPI_AST		1
#define	VIRT68K_IPI_XCALL	2
#define	VIRT68K_IPI_GENERIC	3
#define	VIRT68K_IPI_HALT	4
#define	VIRT68K_IPI_DDB		5

struct cpu_info;

extern void	(*virt68k_ipi_send)(struct cpu_info *, u_int);
extern void	(*virt68k_boot_secondaries)(void);
void	virt68k_ipi_dispatch(u_int);
#endif

#endif /* _VIRT68K_CPU_MP_H_ */
