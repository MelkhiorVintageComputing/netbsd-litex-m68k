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
#define	VIRT68K_IPI_TLB		6	/* pmap_tlb_shootdown() */

struct cpu_info;

/* Size of the per-CPU scratch stack used by cpu_switchto() */
#define	VIRT68K_TMPSTK_SIZE	1024

/*
 * Passed to cpu_spinup_trampoline (locore.s) in START_ARG; the offsets
 * are used there (HATCH_*).
 */
struct virt68k_hatch {
	struct cpu_info	*h_ci;		/* HATCH_CI */
	void		*h_sp;		/* HATCH_SP: idle lwp stack */
	void		*h_vbr;		/* HATCH_VBR */
	uint32_t	h_stack[64];	/* stack while the MMU is off */
};

extern void	(*virt68k_ipi_send)(struct cpu_info *, u_int);
extern void	(*virt68k_boot_secondaries)(void);
void	virt68k_ipi_dispatch(u_int);
#endif

#endif /* _VIRT68K_CPU_MP_H_ */
