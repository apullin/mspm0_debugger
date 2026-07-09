# Ultra-Minimal GDB RSP Probe (using MSPM0)

## Goals & Intent

This repo builds a tiny GDB Remote Serial Protocol (RSP) probe firmware that speaks SWD to Cortex‑M targets (and optionally JTAG to RISC-V targets) and talks to GDB over UART.
The goal here is to implement a debugger probe on an ultra-low cost part, targeting the <$0.20 MSPM0 for the smallest build.

Current firmware version: **0.2.0**. `CMakeLists.txt` is authoritative; the
G5187 USB `bcdDevice` value is derived from that project version.

"But an RP2350 is only $0.70!" ... sure. I still wanted this to be self-contained, and in silicon made by an American company.

## Probe MCU Targets

Multiple MSPM0 variants supported with different cost/feature trade-offs:

- [MSPM0C1104](https://www.ti.com/product/MSPM0C1104) ("tiny"): 16KB flash / 1KB SRAM / 24 MHz / $0.195@1ku — UART only
- [MSPM0C1105](https://www.ti.com/product/MSPM0C1105) ("full"): 32KB flash / 8KB SRAM / 32 MHz / $0.376@1ku — UART only
- [MSPM0G5187](https://www.ti.com/product/MSPM0G5187) ("usb"): 128KB flash / 32KB SRAM / 32 MHz currently / ~$1@1ku — Native USB-CDC

The G5187 variant includes native USB 2.0 Full Speed support with up to two CDC ports:
- **CDC Port 0**: GDB RSP communication (replaces UART on C1104/C1105)
- **CDC Port 1**: Optional target VCOM bridge (connects to target's serial output, like J-Link VCOM)

Pinning is not yet finalized. 16-pin parts may be viable for C110x, TODO.

## Supported Debug Targets

### Cortex-M (SWD) - Default

Default target-core support (can be compiled out with `PROBE_TARGET_*` options):
- M0, M0+, M3, M4, M7, M23, M33
- Detection is CPUID-based at attach time

Cortex-M55 is not currently supported because it requires ADIv6 AP discovery.
`PROBE_TARGET_M55` therefore defaults off and is rejected if enabled rather
than advertising a target that cannot attach.

### RISC-V (JTAG) - Optional

Optional RISC-V debug support via JTAG (`-DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON`):
- RV32 targets via Debug Spec 0.13
- Abstract Commands for register access
- System Bus Access for memory read/write
- Can be built standalone or alongside Cortex-M (runtime auto-detection)

## Build Profiles

`PROBE_TINY_RAM` is auto-selected based on device:

C1104 (auto `PROBE_TINY_RAM=ON`):
- Profile-sized RSP buffers: `PacketSize=0x100` for Cortex/XML, `0x109` for
  RV32/XML, and `0x151` for the legacy ARM register layout
- Full target XML enabled (`PROBE_ENABLE_QXFER_TARGET_XML=ON`), which GDB needs
  to interpret the compact Cortex-M register packet. The C1104 dual-architecture
  profile disables the large Cortex XML strings because they cannot fit 16 KB;
  Cortex uses GDB's correct legacy ARM layout, while an RV32-only architecture
  description is served dynamically when the attached target is RISC-V.
- DWT watchpoints disabled (`PROBE_ENABLE_DWT_WATCHPOINTS=OFF`)

C1105 (auto `PROBE_TINY_RAM=OFF`):
- Larger RSP buffers (`PacketSize=0x200`)
- Target XML enabled (arch string from CPUID)
- DWT watchpoints enabled (`Z2/Z3/Z4`, when DWT is present)

Please note that exact pinning is not yet finalized, as I have not done a schematic or a board build!

## Feature Set

### Cortex-M (default)

- SWD over bit‑banged GPIO (ADIv5 DP/AP + MEM‑AP), with WAIT retry and
  sticky-FAULT recovery so one bad access doesn't kill the session
- Cortex‑M halt/run/step (interrupt-masked stepping via `C_MASKINTS`),
  register access (`g/G`, `p/P`), memory read/write (`m/M`, binary `X`)
- Hardware breakpoints via FPB (`Z0/z0`, `Z1/z1`), FPB v1 and v2 encodings
- Basic stop replies, `qSupported`, `QStartNoAckMode`, and
  retransmit-on-NACK in every build profile
- GDB `load` to MSPM0-target flash (`PROBE_ENABLE_FLASH_MSPM0`, default on
  for non-tiny Cortex-M builds): `qXfer:memory-map:read` with geometry auto-detected
  from the target's FLASHCTL, plus `vFlashErase/Write/Done` — this is the
  "one MSPM0 flashes another" bootstrap path. Non-MSPM0 targets remain
  RAM/attach-only (flash them with vendor tools).

Configurable Cortex-M features:
- `qXfer:features:read` target XML (`PROBE_ENABLE_QXFER_TARGET_XML`, enabled
  for every default profile except C1104 dual-architecture; that profile keeps
  only a minimal runtime RV32 description and uses legacy Cortex registers)
- DWT watchpoints (`Z2/Z3/Z4`, `PROBE_ENABLE_DWT_WATCHPOINTS`)

### RISC-V (optional, requires `-DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON`)

- JTAG bit-bang wire protocol (IEEE 1149.1 TAP state machine)
- RISC-V Debug Spec 0.13 via DMI (Debug Module Interface)
- RV32 halt/run/step, GPR + PC access, memory read/write
- System Bus Access (SBA) for efficient memory operations
- Hardware breakpoints and watchpoints via Trigger Module (Sdtrig)
- Runtime auto-detection when built alongside Cortex-M

## Measured Build Sizes

Measured with Arm GNU 12.2.1 and TI MSPM0 SDK 2.09.00.01 after the audit fix
pass. Flash includes initialized data; SRAM is initialized data plus BSS.

SRAM figures are statics only; the linker now ASSERTs that at least
`_Min_Stack_Size` bytes remain for the stack above them (256 B on C1104,
1 KB on C1105/G5187). The link uses `-nostartfiles` (boot enters via the vector
table's `Reset_Handler`), which keeps crt0/newlib-stdio out of RAM.

### MSPM0C1104 (16 KB Flash / 1 KB SRAM) — `PROBE_TINY_RAM=ON`

Profile-sized buffers and no DWT watchpoints. The dual profile uses legacy ARM
registers plus a minimal runtime RV32 descriptor. Retransmit-on-NACK remains
enabled by reusing the receive packet buffer.

| Configuration | Flash | SRAM |
|---------------|-------|------|
| Cortex-M only | 13,208 B (80.6%) | 512 B (50.0%) |
| RISC-V only | 11,680 B (71.3%) | 496 B (48.4%) |
| Dual (legacy ARM + minimal RV XML) | 15,136 B (92.4%) | 680 B (66.4%) |

The GCC 12.2.1 dual image has 1,248 B of flash headroom. This remains the
tightest and most compiler-sensitive profile, so the linker capacity check and
CI matrix are release gates; re-check size whenever the toolchain or feature
set changes.

### MSPM0C1105 (32 KB Flash / 8 KB SRAM) — `PROBE_TINY_RAM=OFF`

Larger buffers (`PacketSize=0x200`), target XML, DWT watchpoints,
retransmit-on-NACK enabled.

| Configuration | Flash | SRAM |
|---------------|-------|------|
| Cortex-M only | 16,040 B (49.0%) | 1,304 B (15.9%) |
| RISC-V only | 11,224 B (34.3%) | 872 B (10.6%) |
| Dual (CM + RV) | 21,832 B (66.6%) | 1,368 B (16.7%) |

### MSPM0G5187 (128 KB Flash / 32 KB SRAM) — USB-CDC

Native USB-CDC with dual ports by default (GDB RSP + target VCOM bridge).
Disabling VCOM produces a single-port descriptor. TinyUSB stack included.

| Configuration | Flash | SRAM |
|---------------|-------|------|
| Cortex-M only + USB | 25,520 B (19.5%) | 3,264 B (10.0%) |
| RISC-V only + USB | 20,720 B (15.8%) | 2,832 B (8.6%) |
| Dual (CM + RV) + USB | 31,328 B (23.9%) | 3,328 B (10.2%) |

## Host-Side Unit Tests

The RSP layer has a desktop test suite (no hardware needed):

```
cmake -S tests -B build_tests
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

Nine binaries run under ASan/UBSan: full, tiny Cortex, tiny RISC-V, and
no-XML legacy RSP profiles; register-level suites that compile the production
FLASHCTL, ADIv5/MEM-AP, Cortex, and RISC-V drivers rather than their RSP mocks;
and boundary/randomized coverage for the compact 64-by-32 divider.

GitHub Actions runs these host tests on Linux and macOS and builds all nine
device/architecture combinations, plus C1104 legacy-no-XML and G5187
single-CDC, C1105 32 MHz and fractional-MHz HFXT, and 32-bit JTAG-IR profiles,
with warnings promoted to errors. Firmware CI checks out TI's tagged
`mspm0_sdk_2_09_00_01` source repository.

## Requirements

- CMake 3.20+
- Arm GNU Toolchain 9 or newer (`arm-none-eabi-gcc`, `arm-none-eabi-size`,
  `arm-none-eabi-objcopy`)
- TI MSPM0 SDK installed locally (default path):
  - `/Applications/ti/mspm0_sdk_2_09_00_01/`

You can override the SDK location with:
- `-DMSPM0_SDK_PATH=/path/to/mspm0_sdk`

## Configure + Build

C1104 build (Cortex-M only, auto-selects smaller buffers):
```
cmake -S . -B build_c1104 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1104 -DPROBE_ALLOW_ODIO_SWD_PINS=ON
cmake --build build_c1104 -j
```

C1105 build (Cortex-M only, auto-selects full features):
```
cmake -S . -B build_c1105 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_ALLOW_ODIO_SWD_PINS=ON
cmake --build build_c1105 -j
```

RISC-V only build:
```
cmake -S . -B build_rv -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_ALLOW_ODIO_SWD_PINS=ON -DPROBE_ENABLE_CORTEXM=OFF -DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON
cmake --build build_rv -j
```

`PROBE_ENABLE_FLASH_MSPM0` auto-selects off when Cortex-M is disabled; an
explicit incompatible `PROBE_ENABLE_FLASH_MSPM0=ON` is rejected.

Dual architecture build (Cortex-M + RISC-V with runtime detection):
```
cmake -S . -B build_dual -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_ALLOW_ODIO_SWD_PINS=ON -DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON
cmake --build build_dual -j
```

G5187 USB build (native USB-CDC, no external UART bridge needed):
```
cmake -S . -B build_g5187 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0G5187 -DPROBE_ALLOW_ODIO_SWD_PINS=ON
cmake --build build_g5187 -j
```

### Architecture Options

- `-DPROBE_ENABLE_CORTEXM=ON` (default) - Enable Cortex-M debug via SWD
- `-DPROBE_ENABLE_JTAG=ON` - Enable JTAG bit-bang wire protocol
- `-DPROBE_ENABLE_RISCV=ON` - Enable RISC-V debug (requires JTAG)

At least one of `PROBE_ENABLE_CORTEXM` or `PROBE_ENABLE_RISCV` must be enabled.

### Other Feature Toggles

- `-DPROBE_ENABLE_QXFER_TARGET_XML=OFF` - Disable the full target XML set
- `-DPROBE_ENABLE_DWT_WATCHPOINTS=OFF` - Disable DWT watchpoints
- `-DPROBE_SWD_DELAY_US=0` - SWD clock delay (increase for slow targets)
- `-DPROBE_RISCV_JTAG_IR_LEN=5` - RISC-V DTM instruction-register length
  (5-32 bits; override for wider single-TAP implementations)
- `-DPROBE_USE_HFXT=ON` - Use a 4-32 MHz external crystal (C1105 only)
- `-DPROBE_ENABLE_VCOM=OFF` - Remove the target VCOM bridge and its second
  USB CDC interface (G5187 only)
- `-DPROBE_ALLOW_ODIO_SWD_PINS=ON` - Explicitly acknowledge the current
  bring-up-only PA0/PA1 placeholder mapping

On C1104, disabling the full target XML selects GDB's 42-register legacy ARM
layout, raising `PacketSize` from `0x100` to `0x151`. The C1104
dual-architecture profile selects this mode automatically and dynamically
serves only the tiny RV32 architecture description when a RISC-V target is
attached. Explicitly forcing the full XML set on for that profile is rejected
at configure time because it overflows flash.

## Usage (GDB)

### C1104/C1105 (UART)

Connect the probe UART to your host via USB-UART bridge and point GDB at the serial port:
```
(gdb) set serial baud 115200
(gdb) target remote /dev/tty.usbserial-XXXX
```

### G5187 (USB-CDC)

Connect the probe directly via USB. With VCOM enabled, two CDC ports enumerate:
- First port: GDB RSP (use this for debugging)
- Second port: Target VCOM (optional serial monitor)

```
(gdb) target remote /dev/tty.usbmodemXXXX
```

The VCOM port can be opened with any serial terminal to monitor target serial
output. With `PROBE_ENABLE_VCOM=OFF`, only the GDB port is described.

## Flashing the Probe (FYI)

Goal:
- Make a schematic + PCB that supports the MSPM0 on‑chip UART bootloader
- Show one MSPM0 debugger flashing another one, so you only bootstrap a single unit
- Flash with JLink, to make sure it works & can debug the project itself

## Clocking Notes

As this has not had a hardware built yet, this is all estimated, pending testing!

The checked-in board mappings still place shared SWCLK/TCK and SWDIO/TMS on
PA0/PA1. Those are ODIO/open-drain pins and cannot generate a
production-quality SWD or JTAG clock. CMake
therefore refuses to build unless that mapping is explicitly acknowledged with
`PROBE_ALLOW_ODIO_SWD_PINS=ON`. Replace the mapping with SDIO/HSIO-capable pins
when the schematic is finalized, then remove the gate; do not treat the opt-in
as hardware approval.

This project currently runs the probe MCU from SYSOSC and only needs "good enough" clock accuracy for UART baud rate and `delay_us()`.

SYSOSC Frequency Correction Loop (FCL) / ROSC:
- See TI MSPM0 reference manual (SLAU893) section "2.3.1.2.1 SYSOSC Frequency Correction Loop"
- FCL can improve SYSOSC accuracy using a reference current derived from either an internal resistor or an external resistor (ROSC), depending on device support
- External‑resistor mode uses a resistor between the ROSC pin and VSS
- Enabling FCL is sticky until BOOTRST
- Register: write `KEY=0x2A` and set `SETUSEFCL=1` in `SYSOSCFCLCTL`
- In this repo you can enable FCL at boot with `-DPROBE_ENABLE_SYSOSC_FCL=ON`

External clocks (for higher accuracy or PLL):
- Populate the relevant pins (HFXT crystal pins or an HFCLK_IN digital clock pin)
- Add boot‑time code to configure IOMUX, start the clock, wait for "GOOD", and switch MCLK
- See SLAU893 sections "2.3.1.5 High Frequency Crystal Oscillator (HFXT)" and "2.3.1.6 HFCLK_IN (Digital clock)"

## Notes

- The probe firmware is MSPM0‑specific, but the target side is generic Cortex‑M over SWD (or RISC-V over JTAG).
- Build outputs: `mspm0_debugger.elf`, `.hex`, `.bin`, and `mspm0_debugger.map`.

## Licensing

Project-specific terms are provided in [`LICENSE`](LICENSE). Third-party and
derived material remains under the notices embedded in those files and in the
corresponding upstream SDK distribution.

This repository contains source-available code released under the custom "US-Only Source-Available License v1.0".
The author is committed to prioritizing American interests and restricting access accordingly.
This is not an open-source license in the conventional sense, and use is strictly limited as follows:

1. **Geographic Restriction**: No person or entity located outside the United States of America, its territories, possessions, or protectorates may access, download, use, modify, or distribute this code in any form, for any purpose, without exception.
2. **Permitted Use Within the United States**:
   - Non-commercial use (including personal, educational, research, or hobbyist purposes) is permitted within the United States, provided attribution is given to the original author in all copies or derivative works.
   - Derivative works are allowed under the exact same terms of this license.
3. **Commercial Use**: Any commercial use, even within the United States, requires explicit prior written permission from the author.
4. **Enforcement Note**: Violations of these terms constitute copyright infringement. The author reserves all rights and will pursue enforcement to the fullest extent permitted by law.

All rights not expressly granted are reserved by the author.
