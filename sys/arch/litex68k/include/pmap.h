/*	$NetBSD$	*/

#ifndef _LITEX68K_PMAP_H_
#define	_LITEX68K_PMAP_H_

#include <m68k/pmap_motorola.h>
#include <m68k/mmu_30.h>
#include <m68k/mmu_40.h>

/*
 * LiteX-68K puts every core's CSRs at 0xf000.0000 and up, in the SoC's
 * I/O region (the bus bridge never lets the CPU cache it).  TT0 maps
 * 0xf000.0000-0xffff.ffff VA==PA, cache-inhibited, for supervisor data
 * accesses only; the kernel's VA space stops below it (vmparam.h).
 * Other I/O (LiteEth's buffers at 0x8000.0000) is mapped by bus_space
 * through page tables.
 */
#define	LITEX68K_IO_BASE	0xf0000000

#define	LITEX68K_TT30_IO	(LITEX68K_IO_BASE |			\
				 __SHIFTIN(0x0f, TT30_LAM) |		\
				 TT30_E | TT30_CI | TT30_RWM |		\
				 TT30_SUPERD)

/* The same on the 68040: DTT0, noncachable and serialized. */
#define	LITEX68K_TT40_IO	(LITEX68K_IO_BASE |			\
				 __SHIFTIN(0x0f, TTR40_LAM) |		\
				 TTR40_E | TTR40_SUPER |		\
				 PTE40_CM_NC_SER)

/*
 * Main RAM is not at physical 0 (0x4000.0000 on the Wukong builds),
 * while the kernel runs at VA = PA - firstpa, from VA 0.  To run C
 * (bootinfo_start, pmap_bootstrap) before the real page tables exist,
 * locore turns the MMU on with a bootstrap map: VA 0-256MB onto the
 * first 256MB of RAM, plus TT1 mapping 0x4000.0000-0x7fff.ffff VA==PA
 * for supervisor accesses, so that physical addresses (RELOC()) keep
 * working too.  TT1 is turned off when the real tables are loaded.
 */
#define	LITEX68K_TT30_RAM_BOOT	(0x40000000 |				\
				 __SHIFTIN(0x3f, TT30_LAM) |		\
				 TT30_E | TT30_RWM |			\
				 __SHIFTIN(4, TT30_FCBASE) |		\
				 __SHIFTIN(3, TT30_FCMASK))

/*
 * The 68040's bootstrap map has no early-termination descriptors: locore
 * builds three-level tables for VA 0-32MB instead, and ITT1 and DTT1 map
 * RAM VA==PA for supervisor accesses, write-through (caches are off).
 */
#define	LITEX68K_TT40_RAM_BOOT	(0x40000000 |				\
				 __SHIFTIN(0x3f, TTR40_LAM) |		\
				 TTR40_E | TTR40_SUPER |		\
				 PTE40_CM_WT)

#endif /* _LITEX68K_PMAP_H_ */
