# $NetBSD$
#
# GENERIC with MULTIPROCESSOR (QEMU virt with -smp N, see virtsmp(4)).

include "arch/virt68k/conf/GENERIC"

options 	MULTIPROCESSOR

pseudo-device	lockstat		# lock profiling (virtsmp counter)
