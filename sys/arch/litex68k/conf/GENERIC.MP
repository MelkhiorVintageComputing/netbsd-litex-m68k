# $NetBSD$
#
# GENERIC with MULTIPROCESSOR, for the two-processor bitstreams (two
# RD68LC041s, litexsmp(4)).  It also runs on one processor.

include "arch/litex68k/conf/GENERIC"

options 	MULTIPROCESSOR
options 	M68K_PMAP_WRITETHROUGH	# the board's snoop control, setup A

litexsmp*	at mainbus? addr ?	# the SMP block: IPIs, CPU 1
