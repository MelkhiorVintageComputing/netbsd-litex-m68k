# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A full NetBSD 11 source tree with a new port, `litex68k` (MACHINE=litex68k,
MACHINE_ARCH=m68k), for the LiteX-68K FPGA SoC on a QMTech Wukong V3. The CPU is
an RD68031 (MC68030, optionally with an RD68884 MC68881 FPU) or an RD68041
(MC68040, or an FPU-less LC040). The port was derived from `sys/arch/virt68k`
and runs the stock NetBSD/m68k userland. `sys/arch/litex68k/README` describes
the design.

- Branch `netbsd-11` is an untouched shallow copy of upstream (remote `upstream`,
  github NetBSD/src). All work goes on branch `litex68k`, so
  `git diff netbsd-11..litex68k` shows the whole port.
- The platform repo (gateware, litexboot, `scripts/board.py`, hardware docs) is
  `~/LITEX-68K/litex-68k`. The hardware spec for this port is
  `~/LITEX-68K/litex-68k/doc/porting-bsd.md`.

## Clean-room rule (hard)

The port must stay BSD-licensed. **Never open GPL code**:
- the Linux kernel, including `litex-68k/patches/linux`;
- `linux-on-litex-vexriscv`;
- QEMU sources (`~/qemu-m68k-testbed`);
- the RD68031/RD68884/RD68041 reference inputs (`Inputs/ref/`). For those cores
  read only the LICENSE files and commit metadata.

Device facts come from `porting-bsd.md`, LiteX's own BSD sources (gateware, BIOS
libraries, generated `csr.h`) and the Motorola manuals. New files are
BSD-2-Clause, and files derived from virt68k keep their copyright and add ours.

## Build

Builds go outside the tree. The host has 15 GB and no swap, so never go above `-j4`.

```sh
B=~/LITEX-68K/build/netbsd-litex68k
./build.sh -U -u -j4 -m litex68k -T $B/tools -O $B/obj -D $B/dest -R $B/release tools kernel=GENERIC
./build.sh -U -u -j4 -m litex68k -T $B/tools -O $B/obj -D $B/dest -R $B/release release   # kernel + sets + ramdisk
# fast kernel rebuild after the first build:
cd $B/obj/sys/arch/litex68k/compile/GENERIC && $B/tools/bin/nbmake-litex68k -j4
```

Kernels must build warning-free, because `-Werror` is on.
- `GENERIC`: one kernel for the 030 and the 040, with `FPU_EMULATE`.
- `LITEX68K_MIN`: a small bring-up kernel, for short serial uploads.

Strip a kernel before uploading it with `$B/tools/bin/m68k--netbsdelf-strip`.

The test programs in `sys/arch/litex68k/test/` (fptest, fpstress, fpround,
tlsbench) are cross-compiled standalone and run on the board:

```sh
$B/tools/bin/m68k--netbsdelf-gcc --sysroot=$B/dest -O2 -Wall -Wextra -o tlsbench sys/arch/litex68k/test/tlsbench.c
```

## Running on the board

```sh
source ~/LITEX-68K/litex-venv/bin/activate
cd ~/LITEX-68K/litex-68k
scripts/board.py --bitstream <dir> --out-dir <workdir> boot --kernel K [--initrd R] \
    [--cmdline root=ld0a] --until 'login: | panic:' --timeout N --log F
scripts/board.py --bitstream <dir> --out-dir <workdir> sdboot --until ... --log F
```

- **Bitstreams** are in `~/LITEX-68K/bitstreams/`, for example:
  - `wukong-rd68041-fpu-eth-sd-25m-4300689` for the 040;
  - `wukong-fpu-eth-sd-7c668f4` for the 030.
- **Use a separate `--out-dir` per CPU** (e.g. `~/LITEX-68K/board-work-030`).
  board.py rebuilds litexboot there with make, and make doesn't notice a
  bitstream change, so a shared dir boots the wrong CPU's litexboot.
- **`--cmdline root=ld0a`:** a serial boot needs it. An SD boot gets it from
  `params.bin` on the card.
- **`--until` patterns:** don't use a bare `panic:`. It matches the
  `ddb.onpanic` line printed at boot.
- **Serial upload** runs at about 25 KB/s; an SD boot takes about 11 s.

The SD card layout:
- MBR, with FAT p1 = `ld0e`, holding `boot.json`, `netbsd`, `soc.dtb`,
  `params.bin` and `litexboot.bin`;
- root `ld0a`;
- swap `ld0b`.

To install a new kernel, from the running system:
`mount -t msdos /dev/ld0e /boot`, then copy the stripped kernel to `/boot/netbsd`.

The board uses DHCP (192.168.0.110). Files for it are served from `$B/sdcard`
with `python3 -m http.server 8000` (host 192.168.0.55). Ask before
repartitioning the card or committing to `~/LITEX-68K/litex-68k`.

## Architecture (what needs several files to understand)

- **Boot and VA≠PA:** the kernel is linked at VA 0x2000 (as virt68k), but
  litexboot loads it at PA 0x4000_2000 (RAM base + p_paddr) and passes
  Linux/m68k-style bootinfo right after the kernel. The classic m68k pmap maps
  VA = PA − firstpa. litexboot is used unchanged.
  - `locore.s` computes firstpa, then turns on a bootstrap MMU before any C runs:
    - a level-A early-termination table maps VA 0–256 MB to RAM;
    - TT1 maps RAM VA==PA, for `RELOC()` accesses in `pmap_bootstrap.c`;
    - TT0 maps the CSRs at 0xF000_0000 and up, uncached.
  - It then calls `bootinfo_start()` and `pmap_bootstrap()`, loads the real
    SRP/TC and turns TT1 off. TT0 stays on, and kernel VA stays below
    0xF000_0000 (`include/vmparam.h`, `include/pmap.h`).
  - locore and pmap_bootstrap have separate 030 and 040 paths (the 040 has
    different MMU/TT registers and uses FPSP).
- **Device discovery has two sources:**
  - `bootinfo.c` reads the `BI_LITEX_*` records for the core devices:
    - the interrupt controller (`intr.c`);
    - `litexctrl` (reset);
    - `litextimer` (hardclock, `delay()`, the uptime timecounter);
    - `liteuart` (console and `ttyLX0`).
  - `litex_fdt.c` uses libfdt on the DTB that litexboot passes (`BI_LITEX_DTB`)
    to find `litesdcard` (an sdmmc host, giving `ld`) and `if_liteeth` (MDIO
    bit-banged, giving `rgephy`).
  - `dev/mainbus.c` attaches everything.
- **Interrupts:** every device line is autovectored at IPL 2, through the LiteX
  pending/mask CSRs in `intr.c`. Every IPL from VM to SCHED maps to PSL IPL 2.
- **FPU:** `BI_FPUTYPE` selects the hardware FPU or `FPU_EMULATE`. Some fixes
  went into shared code, `sys/arch/m68k/fpe` and `m68k/sig_machdep.c`; those
  are upstream bugs.
- **`LITEX68K_FAST_GETPRIVATE`** (on in GENERIC): `trap0` in `locore.s` answers
  `_lwp_getprivate` without going through `syscall()`. Every m68k TLS access is
  that syscall. A traced or non-native process takes the full path, so ktrace
  still works. The offsets it uses come from `genassym.cf`.
- **Outside `sys/arch/litex68k`**, the port touches:
  - `build.sh`, `share/mk/bsd.own.mk`, `sys/arch/README`, `tools/headerlist`;
  - `etc/etc.litex68k/` (the console is `ttyLX0`);
  - `distrib/litex68k/` (the inst-ramdisk);
  - `distrib/sets/lists/*/md.litex68k`;
  - `usr.sbin/sysinst/arch/litex68k`;
  - `sbin/disklabel`.

  When adding an MD hook, grep for `virt68k` and mirror it.

## Conventions

- Commit messages use NetBSD style: a `litex68k:` (or `m68k:`, `m68k/fpe:`)
  subject, then a body explaining the bug and how it was found.
- Code follows NetBSD KNF and the surrounding virt68k style.
- Commit locally, but ask before pushing or sending anything upstream.
