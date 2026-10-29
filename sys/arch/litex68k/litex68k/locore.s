/*	$NetBSD: locore.s,v 1.16.2.1 2025/10/19 10:29:21 martin Exp $	*/

/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1980, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: locore.s 1.66 92/12/22$
 *
 *	@(#)locore.s	8.6 (Berkeley) 5/27/94
 */

#include "opt_compat_netbsd.h"
#include "opt_compat_sunos.h"
#include "opt_fpsp.h"
#include "opt_ddb.h"
#include "opt_kgdb.h"
#include "opt_lockdebug.h"
#include "opt_m68k_arch.h"
#include "opt_fpu_emulate.h"
#include "opt_litex68k.h"

#include "assym.h"
#include <machine/asm.h>
#include <machine/trap.h>

#include "ksyms.h"

/*
 * LiteX-68K: main RAM is not at physical 0 (0x4000.0000 on the Wukong
 * builds).  The kernel is linked at VA 0x0000.2000, so that VA==0 is
 * unmapped, and the m68k pmap maps it VA = PA - firstpa.  litexboot puts
 * each PT_LOAD segment at the RAM base + p_paddr (p_paddr = VA here) and
 * jumps to the RAM base + e_entry, with the MMU and caches off, at IPL 7,
 * and with the bootinfo right after the kernel's end.  So we arrive here
 * at PA = VA + firstpa, and firstpa is where `start' is minus its VA.
 *
 * The first 8KB of RAM, below the kernel text, is used for the kernel
 * message buffer.
 */

/*
 * Temporary stack for a variety of purposes.
 * Try and make this the first thing is the data segment so it
 * is page aligned.  Note that if we overflow here, we run into
 * our text segment.
 */
	.data
	.space	PAGE_SIZE
ASLOCAL(tmpstk)

/* Page descriptors in a 68040 page table: 256KB worth of pages. */
#define	NPTE40		(1 << (18 - PGSHIFT))

/*
 * Macro to relocate a symbol, used before MMU is enabled.
 */
#define	_RELOC(var, ar)		\
	lea	var,ar;			\
	addl	%a5,ar

#define	RELOC(var, ar)		_RELOC(_C_LABEL(var), ar)
#define	ASRELOC(var, ar)	_RELOC(_ASM_LABEL(var), ar)

/*
 * Early console: a polled LiteUART at a fixed physical address, for
 * bring-up only (options LITEX68K_EARLYCONS=0xf0005000).  It is reached
 * VA==PA, both before the MMU is on and through TT0 after.
 */
#ifdef LITEX68K_EARLYCONS
#define	EARLY_PUTC(c)				\
	movl	#LITEX68K_EARLYCONS,%a1;	\
9:	movl	%a1@(4),%d0;			\
	btst	#0,%d0;				\
	jne	9b;				\
	movl	#(c),%a1@
#else
#define	EARLY_PUTC(c)	/* nothing */
#endif

BSS(esym,4)

	.globl	_C_LABEL(edata)
	.globl	_C_LABEL(etext),_C_LABEL(end)

/*
 * This is for kvm_mkdb, and should be the address of the beginning
 * of the kernel text segment (not necessarily the same as kernbase).
 */
	.text
GLOBAL(kernel_text)

/*
 * start of kernel and .text!
 */
ASENTRY_NOPROFILE(start)
	movw	#PSL_HIGHIPL,%sr	| no interrupts
	lea	%pc@(_ASM_LABEL(start)),%a5
	subl	#_ASM_LABEL(start),%a5	| firstpa: load PA - link VA (a5)

	/*
	 * Which MMU: the bootinfo's BI_MMUTYPE, read now at its PA (it
	 * follows the kernel), since the bootstrap map depends on it.
	 * bootinfo_start() sets cputype, fputype and mmutype again later.
	 */
	RELOC(end,%a0)
Lbiscan:
	movw	%a0@,%d0		| tag
	cmpw	#BI_LAST,%d0
	jeq	Lbidone
	cmpw	#BI_MMUTYPE,%d0
	jne	Lbinext
	movl	%a0@(4),%d0
	andl	#BI_MMU_68040,%d0
	jeq	Lbinext
	RELOC(mmutype,%a1)
	movl	#MMU_68040,%a1@
Lbinext:
	moveq	#0,%d0
	movw	%a0@(2),%d0		| record size, header included
	addl	%d0,%a0
	jra	Lbiscan
Lbidone:

	RELOC(mmutype,%a0)
	cmpl	#MMU_68040,%a0@
	jne	Lcacheoff030
	moveq	#0,%d0
	movc	%d0,%cacr		| disable the 68040's caches
	.word	0xf4f8			| cpusha bc: push and invalidate them
	jra	Lcacheoff
Lcacheoff030:
	movl	#CACHE_OFF,%d0
	movc	%d0,%cacr		| clear and disable on-chip cache(s)
Lcacheoff:

	EARLY_PUTC(0x41)		| 'A': running

	ASRELOC(tmpstk, %a0)
	movl	%a0,%sp			| give ourselves a temporary stack

	RELOC(edata,%a0)		| clear out BSS
	movl	#_C_LABEL(end) - 4, %d0	| (must be <= 256 kB)
	subl	#_C_LABEL(edata), %d0
	lsrl	#2,%d0
1:	clrl	%a0@+
	dbra	%d0,1b

	/*
	 * Turn the MMU on with a bootstrap map, so that C code linked at
	 * its VA can run before the real page tables exist:
	 *  - CRP: a level A table of early-termination page descriptors,
	 *    16MB each, mapping VA 0-256MB onto the RAM from firstpa;
	 *  - TT1: 0x4000.0000-0x7fff.ffff VA==PA for supervisor accesses,
	 *    so that this code, and RELOC()'d physical accesses in
	 *    pmap_bootstrap(), keep working;
	 *  - TT0: the CSRs, VA==PA, cache-inhibited (as from now on).
	 */
	RELOC(mmutype,%a0)
	cmpl	#MMU_68040,%a0@
	jeq	Lboot040

	ASRELOC(Lboottab,%a0)
	movl	%a5,%d0
	orl	#1,%d0			| DT = page descriptor
	moveq	#15,%d1			| 16 x 16MB
2:	movl	%d0,%a0@+
	addl	#0x01000000,%d0
	dbra	%d1,2b

	ASRELOC(Lbootcrp,%a0)
	ASRELOC(Lboottab,%a1)
	movl	%a1,%a0@(4)		| table address
	pmove	%a0@,%crp
	ASRELOC(Lboottt,%a0)
	.long	0xf0100800		| pmove %a0@,%tt0
	addql	#4,%a0
	.long	0xf0100c00		| pmove %a0@,%tt1
	pflusha
	ASRELOC(Lboottc,%a0)
	pmove	%a0@,%tc		| MMU on
	jmp	Lbootmapped:l		| and go to our VA

	/*
	 * The 68040 has fixed three-level tables and no early termination:
	 *  - Lboot40pt: 128 page tables of 256KB (NPTE40 pages each), VA
	 *    0-32MB onto the RAM from firstpa;
	 *  - Lboot40ptr: the pointer table, one entry per page table;
	 *  - Lboot40root: the root table, its first entry only;
	 *  - ITT1 and DTT1 for RAM VA==PA, DTT0 for the CSRs.
	 */
Lboot040:
	ASRELOC(Lboot40pt,%a0)
	movl	%a5,%d0
	orl	#PG_V,%d0		| PTE40_RESIDENT
	movl	#128*NPTE40-1,%d1
2:	movl	%d0,%a0@+
	addl	#PAGE_SIZE,%d0
	dbra	%d1,2b

	ASRELOC(Lboot40ptr,%a0)
	ASRELOC(Lboot40pt,%a1)
	movl	%a1,%d0
	orl	#SG_V,%d0		| UTE40_RESIDENT
	moveq	#127,%d1
3:	movl	%d0,%a0@+
	addl	#NPTE40*4,%d0		| next page table
	dbra	%d1,3b

	ASRELOC(Lboot40root,%a0)
	ASRELOC(Lboot40ptr,%a1)
	movl	%a1,%d0
	orl	#SG_V,%d0		| UTE40_RESIDENT
	movl	%d0,%a0@		| VA 0-32MB
	movl	%a0,%d0
	.long	0x4e7b0807		| movc %d0,%srp
	.long	0x4e7b0806		| movc %d0,%urp
	moveq	#0,%d0
	.long	0x4e7b0004		| movc %d0,%itt0
	movl	#LITEX68K_TT40_RAM_BOOT,%d0
	.long	0x4e7b0005		| movc %d0,%itt1
	.long	0x4e7b0007		| movc %d0,%dtt1
	movl	#LITEX68K_TT40_IO,%d0
	.long	0x4e7b0006		| movc %d0,%dtt0
	.word	0xf518			| pflusha
	movl	#MMU40_TCR_BITS,%d0
	.long	0x4e7b0003		| movc %d0,%tc: MMU on
	jmp	Lbootmapped:l		| and go to our VA

Lbootmapped:
	lea	_ASM_LABEL(tmpstk),%sp	| the temporary stack, by its VA

#ifdef LITEX68K_EARLYCONS
	/* Until vec_init(), report any exception (Learlytrap). */
	lea	_ASM_LABEL(Learlyvec),%a0
	movc	%a0,%vbr
	movl	#255,%d1
	lea	_ASM_LABEL(Learlytrap),%a1
1:	movl	%a1,%a0@+
	dbra	%d1,1b
#endif

	EARLY_PUTC(0x42)		| 'B': bootstrap map on

	/*
	 * The bootinfo immediately follows the kernel.  Go parse
	 * it to get CPU/FPU/MMU information and figure out where
	 * the end of the loaded image really is.
	 */
	movl	#_C_LABEL(end),%sp@-
	jbsr	_C_LABEL(bootinfo_start) | bootinfo_start(end)
	addql	#4,%sp

	EARLY_PUTC(0x43)		| 'C': bootinfo parsed

	/*
	 * bootinfo_start() recorded the first VA following the
	 * bootinfo in bootinfo_end.  That represents the end of
	 * the loaded image.  Rounding that to a page gives us
	 * the first free physical page.
	 */
	movl	_C_LABEL(bootinfo_end),%d2
	addl	#PAGE_SIZE-1,%d2
	andl	#PG_FRAME,%d2		| round to a page
	movl	%d2,%a4
	addl	%a5,%a4			| convert to PA
	pea	%a5@			| firstpa
	pea	%a4@			| nextpa
	jbsr	_C_LABEL(pmap_bootstrap) | pmap_bootstrap(nextpa, firstpa)
	addql	#8,%sp

	EARLY_PUTC(0x44)		| 'D': page tables built

/* initialize source/destination control registers for movs */
	moveq	#FC_USERD,%d0		| user space
	movc	%d0,%sfc		|   as source
	movc	%d0,%dfc		|   and destination of transfers

/*
 * Switch to the real page tables.  They map the kernel at the same VAs
 * as the bootstrap map, so we just load them.  mmu_tt30[] turns TT1 off,
 * mmu_tt40[] ITT1 and DTT1.
 */
	movl	_C_LABEL(Sysseg_pa),%d1	| system segment table addr (a PA)
	cmpl	#MMU_68040,_C_LABEL(mmutype)
	jne	Lnot040mmu
	.long	0x4e7b1807		| movc %d1,%srp
	.long	0x4e7b1806		| movc %d1,%urp: no more bootstrap map
	pea	_C_LABEL(mmu_tt40)	| TT reg values
	jbsr	_C_LABEL(mmu_load_tt40)
	addql	#4,%sp
	.word	0xf4d8			| cinva bc
	.word	0xf518			| pflusha
	movl	#MMU40_TCR_BITS,%d0
	.long	0x4e7b0003		| movc %d0,%tc
	movl	#0x80008000,%d0
	movc	%d0,%cacr		| turn on both caches
	jra	Lmmuenabled
Lnot040mmu:
	lea	_C_LABEL(protorp),%a0
	movl	%d1,%a0@(4)		| segtable address
	pmove	%a0@,%srp		| load the supervisor root pointer
	pea	_C_LABEL(mmu_tt30)	| TT reg values
	jbsr	_C_LABEL(mmu_load_tt30)
	addql	#4,%sp
	pflusha
	movl	#MMU51_TCR_BITS,%sp@	| value to load TC with
	pmove	%sp@,%tc		| load it
	lea	_ASM_LABEL(Lbootcrp),%a0
	movl	%d1,%a0@(4)		| no more bootstrap map for user space
	pmove	%a0@,%crp
	pflusha
Lmmuenabled:

/*
 * Should be running mapped from this point on
 */
	lea	_ASM_LABEL(tmpstk),%sp	| re-load the temporary stack
	jbsr	_C_LABEL(vec_init)	| initialize the vector table
/* call final pmap setup */
	jbsr	_C_LABEL(pmap_bootstrap_finalize)
/* set kernel stack, user SP */
	movl	_C_LABEL(lwp0uarea),%a1	| get lwp0 uarea
	lea	%a1@(USPACE-4),%sp	| set kernel stack to end of area
	movl	#USRSTACK-4,%a2
	movl	%a2,%usp		| init user SP
	tstl	_C_LABEL(fputype)	| Have an FPU?
	jeq	Lenab2			| No, skip.
	clrl	%a1@(PCB_FPCTX)		| ensure null FP context
	movl	%a1,%sp@-
	jbsr	_C_LABEL(m68881_restore) | restore it (does not kill a1)
	addql	#4,%sp
Lenab2:
	cmpl	#MMU_68040,_C_LABEL(mmutype)	| 68040?
	jeq	Ltbia040		| yes, cache already on
	pflusha
	movl	#CACHE_ON,%d0
	movc	%d0,%cacr		| clear cache(s)
	jra	Lenab3
Ltbia040:
	.word	0xf518
Lenab3:
/*
 * final setup for C code:
 * Create a fake exception frame so that cpu_lwp_fork() can copy it.
 * main() nevers returns; we exit to user mode from a forked process
 * later on.
 */
	jbsr	_C_LABEL(litex68k_init)	| additional pre-main initialization
	/*
	 * Stay at IPL 7: cpu_configure() lowers it once the interrupt
	 * controller has every line masked and the drivers are attached.
	 */
	clrw	%sp@-			| vector offset/frame type
	clrl	%sp@-			| PC - filled in by "execve"
	movw	#PSL_USER,%sp@-		| in user mode
	clrl	%sp@-			| stack adjust count and padding
	lea	%sp@(-64),%sp		| construct space for D0-D7/A0-A7
	lea	_C_LABEL(lwp0),%a0	| save pointer to frame
	movl	%sp,%a0@(L_MD_REGS)	|   in lwp0.l_md.md_regs

	jra	_C_LABEL(main)		| main()

#ifdef LITEX68K_EARLYCONS
/*
 * An exception before vec_init(): print "!" and the format/vector word,
 * the PC and, for a 68040 access error (format $7), the fault address and
 * the SSW; then the SP and the 32 longs from it (the frame, then what the
 * code had stacked), and stop.  No stack: Learlyhex returns through %a2.
 */
ASLOCAL(Learlytrap)
	movw	#PSL_HIGHIPL,%sr
	movl	%sp,%a3
	EARLY_PUTC(0x21)		| '!'
	moveq	#0,%d2
	movw	%a3@(6),%d2		| format/vector
	moveq	#3,%d3
	lea	1f,%a2
	jra	Learlyhex
1:	movl	%a3@(2),%d2		| PC
	moveq	#7,%d3
	lea	2f,%a2
	jra	Learlyhex
2:	movw	%a3@(6),%d2
	andw	#0xf000,%d2
	cmpw	#0x7000,%d2
	jne	4f
	movl	%a3@(0x14),%d2		| FA
	moveq	#7,%d3
	lea	3f,%a2
	jra	Learlyhex
3:	moveq	#0,%d2
	movw	%a3@(0xc),%d2		| SSW
	moveq	#3,%d3
	lea	4f,%a2
	jra	Learlyhex
4:	movl	%a3,%d2			| SP, then the 32 longs from it
	moveq	#7,%d3
	lea	8f,%a2
	jra	Learlyhex
8:	moveq	#31,%d6
9:	movl	%a3@+,%d2
	moveq	#7,%d3
	lea	10f,%a2
	jra	Learlyhex
10:	dbra	%d6,9b
11:	stop	#PSL_HIGHIPL
	jra	11b

	/* Print " " and the low %d3+1 nibbles of %d2; return to %a2. */
Learlyhex:
	EARLY_PUTC(0x20)
	movl	%d3,%d4
	lsll	#2,%d4
	rorl	%d4,%d2			| first nibble to print at the top
	rorl	#4,%d2
5:	roll	#4,%d2
	movl	%d2,%d5
	andl	#0xf,%d5
	cmpl	#10,%d5
	jlt	6f
	addl	#0x41-0x30-10,%d5
6:	addl	#0x30,%d5
	movl	#LITEX68K_EARLYCONS,%a1
7:	movl	%a1@(4),%d0
	btst	#0,%d0
	jne	7b
	movl	%d5,%a1@
	dbra	%d3,5b
	jmp	%a2@
#endif

/*
 * Trap/interrupt vector routines
 */
#include <m68k/m68k/trap_subr.s>

/*
 * Use common m68k bus error and address error handlers.
 */
#include <m68k/m68k/busaddrerr.s>

/*
 * FP exceptions.
 */
ENTRY_NOPROFILE(fpfline)
#if defined(M68040)
	cmpl	#FPU_68040,_C_LABEL(fputype) | 68040 FPU?
	jne	Lfp_unimp		| no, skip FPSP
	cmpw	#0x202c,%sp@(6)		| format type 2?
	jne	_C_LABEL(illinst)	| no, not an FP emulation
#ifdef FPSP
	jmp	_ASM_LABEL(fpsp_unimp)	| yes, go handle it
#else
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save registers
	moveq	#T_FPEMULD,%d0		| denote as FP emulation trap
	jra	_ASM_LABEL(fault)	| do it
#endif
Lfp_unimp:
#endif /* M68040 */
#ifdef FPU_EMULATE
	tstl	_C_LABEL(fputype)	| have an FPU?
	jne	_C_LABEL(illinst)	| yes, a real illegal instruction
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save registers
	moveq	#T_FPEMULI,%d0		| denote as FP emulation trap
	jra	_ASM_LABEL(fault)	| do it
#else
	jra	_C_LABEL(illinst)
#endif

ENTRY_NOPROFILE(fpunsupp)
#if defined(M68040)
	cmpl	#FPU_68040,_C_LABEL(fputype) | 68040 FPU?
	jne	Lfp_unsupp		| No, skip FPSP
#ifdef FPSP
	jmp	_ASM_LABEL(fpsp_unsupp)	| yes, go handle it
#else
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save registers
	moveq	#T_FPEMULD,%d0		| denote as FP emulation trap
	jra	_ASM_LABEL(fault)	| do it
#endif
Lfp_unsupp:
#endif /* M68040 */
	jra	_C_LABEL(illinst)

/*
 * Handles all other FP coprocessor exceptions.
 * Note that since some FP exceptions generate mid-instruction frames
 * and may cause signal delivery, we need to test for stack adjustment
 * after the trap call.
 */
ENTRY_NOPROFILE(fpfault)
	clrl	%sp@-		| stack adjust count
	moveml	#0xFFFF,%sp@-	| save user registers
	movl	%usp,%a0	| and save
	movl	%a0,%sp@(FR_SP)	|   the user stack pointer
	clrl	%sp@-		| no VA arg
	movl	_C_LABEL(curpcb),%a0 | current pcb
	lea	%a0@(PCB_FPCTX),%a0 | address of FP savearea
	fsave	%a0@		| save state
#if defined(M68040) || defined(M68060)
	/* always null state frame on 68040, 68060 */
	cmpl	#FPU_68040,_C_LABEL(fputype)
	jge	Lfptnull
#endif
	tstb	%a0@		| null state frame?
	jeq	Lfptnull	| yes, safe
	clrw	%d0		| no, need to tweak BIU
	movb	%a0@(1),%d0	| get frame size
	bset	#3,%a0@(0,%d0:w) | set exc_pend bit of BIU
Lfptnull:
	fmovem	%fpsr,%sp@-	| push fpsr as code argument
	frestore %a0@		| restore state
	movl	#T_FPERR,%sp@-	| push type arg
	jra	_ASM_LABEL(faultstkadj) | call trap and deal with stack cleanup


/*
 * Other exceptions only cause four and six word stack frame and require
 * no post-trap stack adjustment.
 */

ENTRY_NOPROFILE(badtrap)
	moveml	#0xC0C0,%sp@-		| save scratch regs
	movw	%sp@(22),%sp@-		| push exception vector info
	clrw	%sp@-
	movl	%sp@(22),%sp@-		| and PC
	jbsr	_C_LABEL(straytrap)	| report
	addql	#8,%sp			| pop args
	moveml	%sp@+,#0x0303		| restore regs
	jra	_ASM_LABEL(rei)		| all done

ENTRY_NOPROFILE(trap0)
#ifdef LITEX68K_FAST_GETPRIVATE
	/*
	 * _lwp_getprivate() is what every TLS access does on m68k
	 * (__m68k_read_tp), so jemalloc makes it several times per
	 * malloc/free.  Answer it here, without the full syscall path,
	 * for a native process that isn't traced (ktrace, ptrace) and
	 * has no AST pending; anything else takes the full path.  The
	 * user's registers are all kept but %d0, as syscall() does it.
	 */
	cmpl	#SYS__lwp_getprivate,%d0
	jne	Ltrap0_full
	tstl	_C_LABEL(astpending)
	jne	Ltrap0_full
	movl	%a0,%sp@-		| the user's %a0
	movl	_C_LABEL(curlwp),%a0
	movl	%a0@(L_PROC),%a0
	tstb	%a0@(P_TRACE_ENABLED)	| syscall_fancy() then
	jne	Ltrap0_full_a0
	cmpl	#_C_LABEL(emul_netbsd),%a0@(P_EMUL)
	jne	Ltrap0_full_a0
	movl	_C_LABEL(curlwp),%a0
	movl	%a0@(L_PRIVATE),%d0	| the syscall's value
	addql	#1,_C_LABEL(cpu_info_store)+CI_NSYSCALL+4 | int64_t
	jcc	1f
	addql	#1,_C_LABEL(cpu_info_store)+CI_NSYSCALL
1:	movl	%sp@+,%a0
	andw	#~PSL_C,%sp@		| success: carry clear
	rte
Ltrap0_full_a0:
	movl	%sp@+,%a0
Ltrap0_full:
#endif
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save user registers
	movl	%usp,%a0		| save the user SP
	movl	%a0,%sp@(FR_SP)		|   in the savearea
	movl	%d0,%sp@-		| push syscall number
	jbsr	_C_LABEL(syscall)	| handle it
	addql	#4,%sp			| pop syscall arg
	tstl	_C_LABEL(astpending)	| AST pending?
	jne	Lrei1			| Yup, go deal with it.
	movl	%sp@(FR_SP),%a0		| grab and restore
	movl	%a0,%usp		|   user SP
	moveml	%sp@+,#0x7FFF		| restore most registers
	addql	#8,%sp			| pop SP and stack adjust
	rte

/*
 * Trap 12 is the entry point for the cachectl "syscall" (both HPUX & BSD)
 *	cachectl(command, addr, length)
 * command in d0, addr in a1, length in d1
 */
ENTRY_NOPROFILE(trap12)
	movl	_C_LABEL(curlwp),%a0
	movl	%a0@(L_PROC),%sp@-	| push current proc pointer
	movl	%d1,%sp@-		| push length
	movl	%a1,%sp@-		| push addr
	movl	%d0,%sp@-		| push command
	jbsr	_C_LABEL(cachectl1)	| do it
	lea	%sp@(16),%sp		| pop args
	jra	_ASM_LABEL(rei)		| all done

/*
 * Trace (single-step) trap.  Kernel-mode is special.
 * User mode traps are simply passed on to trap().
 */
ENTRY_NOPROFILE(trace)
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-
	moveq	#T_TRACE,%d0

	| Check PSW and see what happen.
	|   T=0 S=0	(should not happen)
	|   T=1 S=0	trace trap from user mode
	|   T=0 S=1	trace trap on a trap instruction
	|   T=1 S=1	trace trap from system mode (kernel breakpoint)

	movw	%sp@(FR_HW),%d1		| get PSW
	notw	%d1			| XXX no support for T0 on 680[234]0
	andw	#PSL_TS,%d1		| from system mode (T=1, S=1)?
	jeq	Lkbrkpt			| yes, kernel breakpoint
	jra	_ASM_LABEL(fault)	| no, user-mode fault

/*
 * Trap 15 is used for:
 *	- GDB breakpoints (in user programs)
 *	- KGDB breakpoints (in the kernel)
 *	- trace traps for SUN binaries (not fully supported yet)
 * User mode traps are simply passed to trap().
 */
ENTRY_NOPROFILE(trap15)
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-
	moveq	#T_TRAP15,%d0
	movw	%sp@(FR_HW),%d1		| get PSW
	andw	#PSL_S,%d1		| from system mode?
	jne	Lkbrkpt			| yes, kernel breakpoint
	jra	_ASM_LABEL(fault)	| no, user-mode fault

Lkbrkpt: | Kernel-mode breakpoint or trace trap. (d0=trap_type)
	| Save the system sp rather than the user sp.
	movw	#PSL_HIGHIPL,%sr	| lock out interrupts
	lea	%sp@(FR_SIZE),%a6	| Save stack pointer
	movl	%a6,%sp@(FR_SP)		|  from before trap

	| If were are not on tmpstk switch to it.
	| (so debugger can change the stack pointer)
	movl	%a6,%d1
	cmpl	#_ASM_LABEL(tmpstk),%d1
	jls	Lbrkpt2			| already on tmpstk
	| Copy frame to the temporary stack
	movl	%sp,%a0			| a0=src
	lea	_ASM_LABEL(tmpstk)-96,%a1 | a1=dst
	movl	%a1,%sp			| sp=new frame
	movql	#FR_SIZE,%d1
Lbrkpt1:
	movl	%a0@+,%a1@+
	subql	#4,%d1
	jbgt	Lbrkpt1

Lbrkpt2:
	| Call the trap handler for the kernel debugger.
	| Do not call trap() to do it, so that we can
	| set breakpoints in trap() if we want.  We know
	| the trap type is either T_TRACE or T_BREAKPOINT.
	| If we have both DDB and KGDB, let KGDB see it first,
	| because KGDB will just return 0 if not connected.
	| Save args in d2, a2
	movl	%d0,%d2			| trap type
	movl	%sp,%a2			| frame ptr
#ifdef KGDB
	| Let KGDB handle it (if connected)
	movl	%a2,%sp@-		| push frame ptr
	movl	%d2,%sp@-		| push trap type
	jbsr	_C_LABEL(kgdb_trap)	| handle the trap
	addql	#8,%sp			| pop args
	cmpl	#0,%d0			| did kgdb handle it?
	jne	Lbrkpt3			| yes, done
#endif
#ifdef DDB
	| Let DDB handle it
	movl	%a2,%sp@-		| push frame ptr
	movl	%d2,%sp@-		| push trap type
	jbsr	_C_LABEL(kdb_trap)	| handle the trap
	addql	#8,%sp			| pop args
#endif
	/* Sun 3 drops into PROM here. */
Lbrkpt3:
	| The stack pointer may have been modified, or
	| data below it modified (by kgdb push call),
	| so push the hardware frame at the current sp
	| before restoring registers and returning.

	movl	%sp@(FR_SP),%a0		| modified sp
	lea	%sp@(FR_SIZE),%a1	| end of our frame
	movl	%a1@-,%a0@-		| copy 2 longs with
	movl	%a1@-,%a0@-		| ... predecrement
	movl	%a0,%sp@(FR_SP)		| sp = h/w frame
	moveml	%sp@+,#0x7FFF		| restore all but sp
	movl	%sp@,%sp		| ... and sp
	rte				| all done

/*
 * Interrupt handlers.
 *
 * For auto-vectored interrupts, the CPU provides the
 * vector 0x18+level.
 *
 * intrhand_autovec is the entry point for auto-vectored
 * interrupts.
 */

ENTRY_NOPROFILE(intrhand_autovec)
	addql	#1,_C_LABEL(intr_depth)
	INTERRUPT_SAVEREG
	jbsr	_C_LABEL(intr_dispatch)	| call dispatcher
	INTERRUPT_RESTOREREG
	subql	#1,_C_LABEL(intr_depth)

	/* FALLTHROUGH to rei */

/*
 * Emulation of VAX REI instruction.
 *
 * This code deals with checking for and servicing ASTs
 * (profiling, scheduling).
 * After identifying that we need an AST we drop the IPL to allow device
 * interrupts.
 *
 * This code is complicated by the fact that sendsig may have been called
 * necessitating a stack cleanup.
 */
ASENTRY_NOPROFILE(rei)
	tstl	_C_LABEL(astpending)	| AST pending?
	jeq	Ldorte			| Nope. Just return.
	btst	#5,%sp@			| Returning to kernel mode?
	jne	Ldorte			| Yup. Can't do ASTs
	movw	#PSL_LOWIPL,%sr		| lower SPL
	clrl	%sp@-			| stack adjust
	moveml	#0xFFFF,%sp@-		| save all registers
	movl	%usp,%a1		| including
	movl	%a1,%sp@(FR_SP)		|    the users SP
Lrei1:	clrl	%sp@-			| VA == none
	clrl	%sp@-			| code == none
	movl	#T_ASTFLT,%sp@-		| type == async system trap
	pea	%sp@(12)		| fp == address of trap frame
	jbsr	_C_LABEL(trap)		| go handle it
	lea	%sp@(16),%sp		| pop value args
	movl	%sp@(FR_SP),%a0		| restore user SP
	movl	%a0,%usp		|   from save area
	movw	%sp@(FR_ADJ),%d0	| need to adjust stack?
	jne	Laststkadj		| yes, go to it
	moveml	%sp@+,#0x7FFF		| no, restore most user regs
	addql	#8,%sp			| toss SP and stack adjust
Ldorte:	rte				| and do real RTE

Laststkadj:
	lea	%sp@(FR_HW),%a1		| pointer to HW frame
	addql	#8,%a1			| source pointer
	movl	%a1,%a0			| source
	addw	%d0,%a0			|  + hole size = dest pointer
	movl	%a1@-,%a0@-		| copy
	movl	%a1@-,%a0@-		|  8 bytes
	movl	%a0,%sp@(FR_SP)		| new SSP
	moveml	%sp@+,#0x7FFF		| restore user registers
	movl	%sp@,%sp		| and our SP
	rte				| and do real RTE

/*
 * Primitives
 */

/*
 * Use common m68k process/lwp switch and context save subroutines.
 */
#define	FPCOPROC	/* XXX: Temp. Reqd. */
#include <m68k/m68k/switch_subr.s>

#if defined(M68040) || defined(M68060)
ENTRY(suline)
	movl	%sp@(4),%a0		| address to write
	movl	_C_LABEL(curpcb),%a1	| current pcb
	movl	#Lslerr,%a1@(PCB_ONFAULT) | where to return to on a fault
	movl	%sp@(8),%a1		| address of line
	movl	%a1@+,%d0		| get lword
	movsl	%d0,%a0@+		| put lword
	nop				| sync
	movl	%a1@+,%d0		| get lword
	movsl	%d0,%a0@+		| put lword
	nop				| sync
	movl	%a1@+,%d0		| get lword
	movsl	%d0,%a0@+		| put lword
	nop				| sync
	movl	%a1@+,%d0		| get lword
	movsl	%d0,%a0@+		| put lword
	nop				| sync
	moveq	#0,%d0			| indicate no fault
	jra	Lsldone
Lslerr:
	moveq	#-1,%d0
Lsldone:
	movl	_C_LABEL(curpcb),%a1	| current pcb
	clrl	%a1@(PCB_ONFAULT)	| clear fault address
	rts
#endif


ENTRY(ecacheon)
	rts

ENTRY(ecacheoff)
	rts


/*
 * Misc. global variables.
 */
	.data

/* The bootstrap MMU state (see start). */
	.balign	16
ASLOCAL(Lboottab)
	.space	256*4			| level A: 256 x 16MB, invalid
ASLOCAL(Lbootcrp)
	.long	MMU51_CRP_BITS,0	| root pointer: limit, DT short
ASLOCAL(Lboottc)
	.long	0x80c08c00		| E, 4KB pages, TIA 8, TIB 12 (unused)
ASLOCAL(Lboottt)
	.long	MACHINE_TT0		| TT0: the CSRs
	.long	LITEX68K_TT30_RAM_BOOT	| TT1: RAM VA==PA

#ifdef LITEX68K_EARLYCONS
	.balign	4
ASLOCAL(Learlyvec)
	.space	256*4			| all to Learlytrap
#endif

/* The 68040's (see Lboot040). */
	.balign	512
ASLOCAL(Lboot40root)
	.space	128*4			| 128 x 32MB, the first one used
ASLOCAL(Lboot40ptr)
	.space	128*4			| 128 x 256KB
ASLOCAL(Lboot40pt)
	.space	128*NPTE40*4		| 128 x 256KB

GLOBAL(mmutype)
	.long	MMU_68030	| default to MMU_68030

GLOBAL(cputype)
	.long	CPU_68030	| default to CPU_68030

GLOBAL(fputype)
	.long	FPU_NONE	| until the bootinfo says otherwise
