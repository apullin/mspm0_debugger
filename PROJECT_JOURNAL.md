# Project Journal

## Development Log

## 2026-07-08 DISCOVERY Full-project audit (pre-hardware)

**What**: Ran a full audit of the firmware (four parallel subsystem reviews: RSP, Cortex-M/SWD, RISC-V/JTAG, infra/build/USB) plus empirical build verification. Found ~40 defects; every variant had at least one bring-up blocker.

**Where**: Findings spanned src/swd_bitbang.c, src/adiv5.c, src/cortex.c, src/jtag_bitbang.c, src/riscv.c, src/rsp.c, src/probe.c, board files, linker scripts, CMakeLists.txt.

**Why**: No hardware has ever run this code, so wire-level and clock/pin-config bugs had nothing to catch them.

**Technical notes**: Headliners: SWD idle cycles driven HIGH (misframes every transfer — a high bit on an idle bus is a start bit); SWDIO sampled after the SWCLK rising edge (races target Tos, CMSIS-DAP samples before); FP_CTRL enable write missing the KEY bit so hardware breakpoints never arm on any core; DWT comparator stride 0x20 instead of 0x10; RSP `g/G` hardcoded 17 registers so RV32 (33 regs) always got E01; RISC-V SBA unaligned-write RMW never actually read target memory (corrupts neighbors); G5187 USBFLL/48 MHz clock never enabled (USB dead); TDI/TDO never IOMUX-configured (JTAG electrically dead); C1104 had 56–104 B of stack because crt0's `_start` (kept as default ELF entry) dragged in exit()/newlib stdio (~440 B RAM) and the linker's `_Min_Stack_Size` was declared but never ASSERTed.

## 2026-07-08 FIX Audit fix pass — all findings addressed across all layers

**What**: Fixed every audit finding in one pass (~950 insertions / 348 deletions across 26 files, uncommitted on branch adiv6-dormant-mode). All 9 device/arch variants build clean under the newly enabled `-Wall -Wextra`.

**Where**: SWD/ADIv5 (idle-low, sample-before-edge, WAIT retry, sticky-FAULT clear, ABORT bits [4:1], power-up ACK enforced); cortex.c (FP_CTRL `|3u`, FPB v2 encoding keyed off FP_CTRL REV, DWT stride 0x10, v2 DATAVSIZE validation, C_MASKINTS stepping); jtag_bitbang.c/riscv.c (dmireset recovery + busy retry honoring dtmcs.idle, abits clamp ≤30 fixing a 5-byte stack overflow, two-scan DMI so write status isn't off-by-one, SBA sbreadonaddr-before-address RMW, sba_wait_idle fails on timeout and clears sbbusyerror, abstract-memory fallback address via data1, step-bit cleanup on timeout, trigger/dmactive reset in riscv_init); rsp.c/target.c/probe.c (`g/G` sized by target_gdb_reg_count with static rsp_regs, per-arch p/P via new target_map_gdb_regnum/target_pc_regnum, target.xml annex length 10, Ctrl-C idle-only with S02 on confirmed halt, retransmit-on-NACK on non-tiny + QStartNoAckMode everywhere, strict hex parsing, oversized-packet NACK, target re-detection via new probe_attach()); boards (G5187 USBFLL enable + blocking uart_putc servicing tud_task, TDI/TDO IOMUX on all three boards, SWDIO push-pull-when-driving per QUESTIONS.md intent, hal_time_us remainder carry, delay_us half-range chunking); build (`-nostartfiles -Wl,--entry=Reset_Handler`, startups walk `.init_array` directly since newlib's `__libc_init_array` needs crti's `_init`, linker ASSERT enforcing `_Min_Stack_Size` = 0x100 C1104 / 0x400 C1105+G5187, CMake HFXT-before-definitions ordering, TINY_RAM stale-cache guard, HFXT 4–32 MHz validation, G5187 core clock truth = 32 MHz).

**Why**: Every variant had at least one blocker; fixing them pre-PCB avoids debugging spec violations with a logic analyzer during bring-up.

**Technical notes**: Measured wins: C1104 SRAM statics 920→560 B (CM), 664 B (dual) — stack headroom went from 56–104 B to 360–464 B, and the worst frame shrank 128→72 B (register buffer moved off-stack). Flash: C1104 dual 12.4 KB/16 KB. Pin gotchas discovered via SDK device headers: C110x has **no PA3** (C1104 TDI/TDO now PA4/PINCM5 + PA6/PINCM7); on C1105 PA3/PA4 ARE the HFXT pins (PINCM6/7) so JTAG+HFXT is now a #error; G5187 PA3/PA4 = PINCM8/9 (double as unused LFXT). MSPM0 PINCM mapping is per-device — trust `IOMUX_PINCMn_PF_GPIOA_DIOxx` in the device header, not PA_n+1.

## 2026-07-08 PLANNING Second-pass improvement list

**What**: Cribbed all non-defect improvements found during the audit into SECOND_PASS.md.

**Where**: SECOND_PASS.md (new file, repo root).

**Why**: Keep the fix pass strictly corrective; features and optimizations (X binary packets, ADIv6 AP addressing to finish the M55 story, vector catch, pipelined DMI, SBA streaming, byte-lane MEM-AP, program-buffer fallback, host-side RSP unit tests, CI matrix with -Werror, 80 MHz SYSPLL, UNIQUEID USB serial, ~2 GB of stale build_* dirs) each deserve their own pass.

## 2026-07-09 DONE Host unit tests, X packets, and MSPM0-target flash programming

**What**: Added nine ASan/UBSan host-test binaries (four RSP profiles plus production FLASHCTL, ADIv5/MEM-AP, Cortex, and RISC-V register-model suites and compact integer-math coverage), RSP binary `X` write packets with 0x7d escape decoding, and GDB `load` support for MSPM0 targets (`qXfer:memory-map:read` + `vFlashErase/Write/Done` driving the target's FLASHCTL over SWD).

**Where**: `tests/`; `src/rsp.c` (binary unescape, X/vFlash handlers, memory-map and transfer helpers); `src/flash_mspm0.c` + `include/flash_mspm0.h`; and CMake profile/default logic.

**Why**: Tests convert the largest untested surface (the GDB-facing parser) into verified behavior before hardware exists. Flash programming scoped to MSPM0 targets because that's the README's bootstrap goal (one MSPM0 flashes another); generic-target flash needs per-vendor drivers and stays a SECOND_PASS item (non-MSPM0 targets are RAM/attach-only).

**Technical notes**: FLASHCTL now follows TI's command semantics: CLEARSTATUS waits for CMDINPROGRESS to clear; every command unlocks WEPROTA/B/C and requires DONE+PASS. Writes coalesce split RSP packets into one complete 64- or 128-bit physical flash word, enable controller data verification, and program generated ECC. Range, wrap, overlap, revisit, and out-of-order writes are rejected. Geometry sums every instantiated bank reported by GBLINFO0 rather than assuming BANK0. On the measured C1105 Cortex profile, enabling flash adds 2,144 B flash and 360 B static SRAM (16,040/1,304 B enabled versus 13,896/944 B disabled).

## 2026-07-09 FIX Final hardening, release metadata, and verification

**What**: Closed the remaining protocol, transport, target-driver, board, USB,
startup, and build-system defects found by the second independent audit. Added
the project's first authoritative semantic firmware version, 0.2.0. The old
USB-only `bcdDevice` value of 1.00 was descriptor metadata, not a declared
project release.

**Where**: RSP session/framing/register-layout handling; Cortex-M/ADIv5 native
byte access and halt/step cleanup; RISC-V DMI/SBA/trigger cleanup; MSPM0 flash
transactions; board pin/clock/VCOM setup; USB descriptors; startup arrays;
CMake/toolchain defaults and cache migration; CI; host register-model tests;
and release/legal documentation.

**Verification**: Nine ASan/UBSan host suites pass. All nine device/target
firmware profiles plus legacy ARM, single-CDC, HFXT-32 MHz,
fractional-HFXT, and JTAG-IR-32 edge profiles build with warnings as errors.
GCC `-fanalyzer` passes on the largest
dual/USB profile, and stock Arm/RISC-V GDB accept the 42-word legacy ARM and
33-word RV32 register packets, including the C1104 dual profile's dynamic
minimal RV32 XML / legacy ARM split. The first GitHub run exposed a GCC 13
overflow in the C1104 dual profile: a `uint64_t` delay calculation pulled more
than 1 KB of Cortex-M0+ software division helpers into flash. A project-owned
84-byte unsigned 64-by-32 divider preserves clear, exact clock-scaling math;
its remainder output also keeps fractional-MHz timeout accounting exact. The
tightest GCC 12.2.1 image is now C1104 dual at 15,136 B flash and 680 B static
SRAM, leaving 1,248 B; the linker assertion and CI build remain mandatory
release gates.

**Residual hardware gate**: PA0/PA1 remain bring-up-only ODIO placeholders for
shared SWCLK/TCK and SWDIO/TMS. CMake rejects the mapping unless explicitly
acknowledged; the schematic must move both signals to suitable push-pull pins
before production.

Last Updated: 2026-07-09
