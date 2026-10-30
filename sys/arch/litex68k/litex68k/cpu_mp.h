/*	$NetBSD$	*/

/*
 * MULTIPROCESSOR glue between the litex68k MD code and litexsmp(4).
 * (From virt68k's cpu_mp.h, virt68k-smp.)
 * (Same license as cpu_mp.c.)
 */

#ifndef _LITEX68K_CPU_MP_H_
#define _LITEX68K_CPU_MP_H_

#ifdef _KERNEL
/* IPI reasons: bit numbers in a CPU's mailbox (litexsmp_mbox[]) */
#define	LITEX68K_IPI_NOP		0
#define	LITEX68K_IPI_AST		1
#define	LITEX68K_IPI_XCALL	2
#define	LITEX68K_IPI_GENERIC	3
#define	LITEX68K_IPI_HALT	4
#define	LITEX68K_IPI_DDB		5
#define	LITEX68K_IPI_TLB		6	/* pmap_tlb_shootdown() */
#define	LITEX68K_IPI_CLOCK	7	/* hardclock(), without Timer1 */
#define	LITEX68K_IPI_DCINV	8	/* litex68k_dcinv_others() */

struct cpu_info;

/* Size of the per-CPU scratch stack used by cpu_switchto() */
#define	LITEX68K_TMPSTK_SIZE	1024

/*
 * Where cpu_spinup_trampoline (locore.s) starts: BOOT_SP is the PA of
 * h_ci, so the fields are at the trampoline's %sp, and h_stack below it
 * is its stack while the MMU is off.  The offsets are used there
 * (HATCH_*).
 */
struct litex68k_hatch {
	uint32_t	h_stack[16];	/* stack while the MMU is off */
	struct cpu_info	*h_ci;		/* HATCH_CI */
	void		*h_sp;		/* HATCH_SP: idle lwp stack */
	void		*h_vbr;		/* HATCH_VBR */
	paddr_t		h_srp;		/* HATCH_SRP: Sysseg_pa */
};

extern void	(*litex68k_ipi_send)(struct cpu_info *, u_int);
extern void	(*litex68k_boot_secondaries)(void);
extern void	(*litex68k_clock_ipi)(void);
void	litex68k_ipi_dispatch(u_int);
void	litex68k_dcinv_others(void);
#endif

#endif /* _LITEX68K_CPU_MP_H_ */
