# Fixes from the litex68k port that matter to other m68k ports

Work on NetBSD/litex68k (branches `litex68k` and `litex68k_smp`) found bugs
that are not specific to LiteX-68K. Each section below gives the issue, how
it was fixed, the commit(s) here, and the ports it affects. Hashes are on
`litex68k_smp`. Those marked (UP) are also on `litex68k`, with the same
hash. None has been sent to NetBSD yet.

The emulated-FPU bugs were all found on the RD68041 built without its FPU, an
MC68LC040: every FP instruction is emulated there, and `litex68k/test/fpround`
makes FP operands fault in the middle of an instruction.

## 1. Bugs in shared code, fixed in shared files

### 1.1 Signal contexts lose the emulated FPU's registers (UP)

- **Commit:** 8ea7f795d, `sys/arch/m68k/m68k/sig_machdep.c`.
- **Issue:** `cpu_getmcontext()` and `cpu_setmcontext()` saved and restored
  the FP state only when there is an FPU. With `FPU_EMULATE` and no FPU, the
  emulator's registers live in the PCB (`pcb_fpregs`), and nothing saved them
  around a signal handler. The handler's FP work, FPCR included, leaked into
  the interrupted code. After a handler set round-to-zero, every later
  division rounded to zero.
- **Fix:** with no FPU, copy `pcb_fpregs` into the mcontext with `_UC_FPU`,
  and restore it on return.
- **Affects:** every port using `FPU_EMULATE` (amiga, atari, hp300, mac68k,
  news68k, next68k, sun2, sun3, x68k) on a CPU without an FPU.

### 1.2 FPU emulator: a faulting memory operand gives no signal (UP)

- **Commit:** d74b95e02, `sys/arch/m68k/fpe/fpu_calcea.c`, `fpu_emulate.c`,
  `fpu_fscale.c`, `fpu_fstore.c`.
- **Issue:** `fpu_load_ea()`/`fpu_store_ea()` ignored the result of
  `copyin()`/`copyout()`, and their callers ignored theirs. An operand on an
  unmapped or protected page was computed from whatever the buffer held, and
  no SIGSEGV was raised.
- **Fix:** return SIGSEGV and pass it up. An address register's predecrement
  is undone, so the instruction can be restarted.
- **Affects:** every `FPU_EMULATE` port.

### 1.3 FPU emulator: static state shared between emulations

- **Commit:** f6a52bd30, `sys/arch/m68k/fpe/fpu_emulate.c`, `fpu_log.c`,
  `fpu_rem.c`.
- **Issue:** `fpu_emulate()` kept its decoded instruction and its whole state
  (`struct instruction`, `struct fpemu`) in static variables. `__fpu_logn()`
  and `__fpu_modrem()` kept their scratch values the same way. Two emulations
  at once overwrite each other's state:
  - on MP, when two CPUs emulate at the same time;
  - on UP too, when an emulation sleeps on a page fault for its operand and
    another lwp emulates in the meantime.

  On two LC040s, two shells or two fpstress runs died of SIGILL (the PC was
  advanced by the other instruction's length) or SIGSEGV.
- **Fix:** the state moves to the stack (about 500 bytes), zeroed as the
  statics were.
- **Affects:** every `FPU_EMULATE` port (rarely seen on UP).

### 1.4 `pmap_release()` walks an empty 4 MB PT window (UP, performance)

- **Commit:** 143e76126, `sys/arch/m68k/m68k/pmap_motorola.c`.
- **Issue:** on every process exit, `pmap_release()` ran `pmap_remove()` and
  `uvm_km_pgremove()` over the whole PT window: 1024 pages, each with a radix
  tree lookup and two swap-slot operations. The window is always empty by
  then. On a 25 MHz 68040 that cost about 60 ms per exit, 19% of kernel time
  in a fork/exec loop.
- **Fix:** free the VA only.
  - fork+exit went from 206 to 130 ms;
  - vfork+exec went from 228 to 119 ms.
- **Affects:** every `pmap_motorola` port (all but sun2/sun3).
- **Note for virt68k-smp:** its patch 0009 also changes `pmap_release()`.
  Here that hunk was dropped in favour of 143e76126 (see 3.1).

## 2. Bugs fixed in a litex68k file that other ports also have

### 2.1 `fpfline` leaks 8 bytes of kernel stack per emulated-FP signal

- **Commit:** 2324d47b9, `sys/arch/litex68k/litex68k/locore.s`.
- **Issue:** the 68LC040/68EC040 report an F-line FP instruction with a
  format 4 frame (16 bytes).
  - When the emulation sends a signal (e.g. its operand faults),
    `cpu_getmcontext()` (shared `sig_machdep.c`) shortens the frame to format
    0. It leaves the 8 dropped bytes to the frame's stack adjust count.
  - `fpfline` returns through `fault`, which ignores that count, so each such
    signal leaves 8 bytes on the kernel stack.
  - The stack creeps down into the PCB: the saved FP context first, then the
    saved registers.

  The symptoms were SIGFPE from an FPCR filled with kernel addresses after a
  few hundred signals and, on MP, CPUs halting silently or stacks corrupted.
- **Fix:** push `fault`'s arguments (with the user SP in the frame) and return
  through `faultstkadj`, as `buserr40` does.
- **Affects:** every port whose `fpfline`/`fpemuli` path ends in
  `jra _ASM_LABEL(fault)` under `FPU_EMULATE`, when run on an LC040/EC040:
  - amiga, hp300, mac68k, next68k, x68k (the most common real case is
    probably an LC040 Quadra/Performa);
  - news68k, luna68k and cesfic have the same code.

  A 68020/030 traps with format 0, so it is unaffected.
- **Not affected:**
  - virt68k and mvme68k have no `FPU_EMULATE` path there;
  - atari's `fpfline` sends an LC040 to `illinst` rather than emulate.

### 2.2 Kernel profiling (`config -p`) faults in `__mcount` (UP)

- **Commit:** 09ed59c08, `sys/arch/litex68k/conf/Makefile.litex68k` and
  `locore.s`.
- **Issue:** m68k's `__mcount` (`m68k/include/profile.h`) follows the %a6
  frame chain (`%a6@(0)@(4)`). Two things break that chain:
  - GCC at -O2 uses %a6 as an ordinary register;
  - locore never sets %a6, so the chain ends in the boot loader's stack,
    which is unmapped once the bootstrap map is gone.
- **Fix:**
  - build with `-fno-omit-frame-pointer` when `PROF` is set;
  - locore pushes a frame that ends the chain (`ENDFRAME`) on each new stack.
- **Affects:**
  - **The compiler half:** probably every m68k port. No m68k
    `Makefile.<port>` adds `-fno-omit-frame-pointer`.
  - **The locore half:** depends on what each boot loader leaves in %a6.

  Not checked on another port.

## 3. Multiprocessor: fixes on top of virt68k-smp's series

NetBSD has no m68k MP yet. The MP base here is the virt68k-smp series
(`~/LITEX-68K/virt68k-smp/patches/netbsd/0001,0003-0011`, BSD-licensed),
applied as d18ae8b27, 4c234cb5b, 936a0b1da, e814b807b, 148325908, 4579cc73f,
5e228048f, 2bd396774, 1dcf4e23a and 43b788426. The fixes below should go
upstream with that series, or into it. 1.3 and 2.1 also showed up first as
MP failures.

### 3.1 Applying the series on top of 143e76126

- **What changed:** 0009's `pmap_release()` hunk was dropped, and 143e76126
  kept.
- **Why this is safe:**
  - under 0009, the segment table and the PT pages' kernel mappings are
    already gone when `pmap_release()` runs (only `uvm_pagefree` is
    deferred);
  - so the window still holds nothing to remove.
- **Reference:** 2bd396774 (0009 as applied) records this in its message,
  together with the gftty hunk it leaves out.

### 3.2 The shootdown's IPL assertion hard-codes level 6

- **Commit:** 02471912b, `sys/arch/m68k/m68k/pmap_motorola.c`.
- **Issue:** `pmap_tlb_send()` asserted `(s & PSL_IPL) < PSL_IPL6`, which is
  virt68k's IPI level. The IPIs arrive at `IPL_SCHED`, which is level 5 on
  litex68k.
- **Fix:** compare with `ipl2psl_table[IPL_SCHED]`.
- **Affects:** any MP port whose IPIs aren't at level 6.

### 3.3 A shootdown IPI clobbers the DFC under an interrupted TBIS

- **Commit:** 2aaf3cb87, `sys/arch/m68k/m68k/pmap_motorola.c`.
- **Issue:** the 68040/060 `TBIS` sets the DFC before each `PFLUSH`
  (supervisor, then user). A shootdown IPI taken between the two runs `TBIS`
  itself and returns with the DFC on user space. The interrupted `TBIS` then
  flushes the user entry twice and leaves a stale supervisor entry.
- **Fix:** `pmap_tlb_shootdown_ipi()` saves and restores the DFC.
- **Affects:** every MP 68040/060, virt68k-smp included.

### 3.4 Snooped write-through memory: `options M68K_PMAP_WRITETHROUGH`

- **Commits:**
  - 63b5829fe: `sys/arch/m68k/conf/files.m68k`,
    `m68k/include/pmap_motorola.h`, `m68k/m68k/pmap_motorola.c`;
  - 2cc8f6318: litex68k's `pmap_bootstrap.c`, the port-side part.
- **Issue:** this is a missing feature, not a bug. On real 68040/060 MP
  hardware where the UPA pins drive the bus's snoop control (UM 4.5), a
  shared page is coherent only if it is write-through with U = 01. Copyback
  lines change in one cache invisibly, and table walks are not snooped.
  virt68k-smp keeps copyback, which is fine on QEMU only.
- **Fix:** under the option, every cacheable mapping is `PG_CACHED`
  (`PG_CWT | PTE40_U0`), read-only ones included: `pmap_enter`,
  `pmap_kenter_pa`, the zero/copy windows and `_pmap_set_page_cacheable`.
  - PT pages and segment tables become CI, and lose their U bits through
    the new mask `PG_CMODE`.
  - Without the option, nothing changes (copyback).
  - virt68k-smp's 0001 (PG_W moved from U0 to UR, d18ae8b27) is a
    prerequisite: otherwise every wired page would set U0.
- **Affects:** any future MP port on real 68040/060 hardware.

## 4. Checked and not upstream material

- **bdea1ca53:** `trap.c` posted signal 0 after every successful emulation.
  - It came from litex68k adding `FPU_EMULATE` to virt68k's `trap.c`, which
    has no such case.
  - The other `FPU_EMULATE` ports already test `ksi.ksi_signo` before
    `trapsignal()`.
- **61906f8d1 and 7cffae5bc:** the CPU 1 trampoline's hatch offset, and
  putting CPU 1 back in reset at halt. Both are specific to LiteX-68K's SMP
  block.
- **0e8cb4890, `LITEX68K_FAST_GETPRIVATE`:** an optimization, not a fix.
  - It answers `_lwp_getprivate` in `trap0`, because every m68k `__thread`
    access is that syscall: 30→7.5 µs per call on a 25 MHz 040, and malloc+free
    104→38 µs.
  - It could become a shared m68k option if wanted.
