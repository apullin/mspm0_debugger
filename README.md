# Ultra-Minimal GDB RSP Probe (using MSPM0)

## Goals & Intent

This repo builds a tiny GDB Remote Serial Protocol (RSP) probe firmware that speaks SWD to Cortex‑M targets (and optionally JTAG to RISC-V targets) and talks to GDB over UART.
The goal here is to implement a debugger probe on an ultra-low cost part, targeting the <$0.20 MSPM0 for the smallest build.

"But an RP2350 is only $0.70!" ... sure. I still wanted to this self-contained, and in silicon made by an American company.

## Probe MCU Targets

Some of the smallest parts are selected, which sets us RAM and Flash budgets:

- [MSPM0C1104](https://www.ti.com/product/MSPM0C1104) ("tiny"): 16KB flash / 1KB SRAM / 24 MHz SYSOSC / $0.195@1ku
- [MSPM0C1105](https://www.ti.com/product/MSPM0C1105) ("full"): 32KB flash / 8KB SRAM / 32 MHz SYSOSC / $0.376@1ku

Pinning is not yet finalized. 16-pin parts may be viable, TODO.

## Supported Debug Targets

### Cortex-M (SWD) - Default

Default target-core support (can be compiled out with `PROBE_TARGET_*` options):
- M0, M0+, M3, M4, M7, M23, M33, M55
- Detection is CPUID-based at attach time

### RISC-V (JTAG) - Optional

Optional RISC-V debug support via JTAG (`-DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON`):
- RV32 targets via Debug Spec 0.13
- Abstract Commands for register access
- System Bus Access for memory read/write
- Can be built standalone or alongside Cortex-M (runtime auto-detection)

## Build Profiles

Tiny build (C1104, `PROBE_TINY_RAM=ON`):
- Smaller RSP buffers (`PacketSize=0x80`)
- Target XML disabled by default (`PROBE_ENABLE_QXFER_TARGET_XML=OFF`)
- DWT watchpoints disabled by default (`PROBE_ENABLE_DWT_WATCHPOINTS=OFF`)

Full build (C1105, `PROBE_TINY_RAM=OFF`):
- Larger RSP buffers (`PacketSize=0x200`)
- Target XML enabled by default (arch string from CPUID)
- DWT watchpoints enabled by default (`Z2/Z3/Z4`, when DWT is present)

Please note that exact pinning is not yet finalized, as I have not done a schematic or a board build!

## Feature Set

### Cortex-M (default)

- SWD over bit‑banged GPIO (ADIv5 DP/AP + MEM‑AP)
- Cortex‑M halt/run/step, register access (`g/G`, `p/P`), memory read/write (`m/M`)
- Hardware breakpoints via FPB (`Z0/z0`, `Z1/z1`)
- Basic stop replies and `qSupported`

Optional Cortex-M features (in "full" builds):
- `qXfer:features:read` target XML (`PROBE_ENABLE_QXFER_TARGET_XML`)
- DWT watchpoints (`Z2/Z3/Z4`, `PROBE_ENABLE_DWT_WATCHPOINTS`)

### RISC-V (optional, requires `-DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON`)

- JTAG bit-bang wire protocol (IEEE 1149.1 TAP state machine)
- RISC-V Debug Spec 0.13 via DMI (Debug Module Interface)
- RV32 halt/run/step, GPR + PC access, memory read/write
- System Bus Access (SBA) for efficient memory operations
- Runtime auto-detection when built alongside Cortex-M

## Reported Build Sizes (approx)

Latest local builds (C1105, `PROBE_TINY_RAM=OFF`):

| Configuration | Flash | SRAM |
|---------------|-------|------|
| Cortex-M only (default) | 13.5 KB (41%) | 1.4 KB (17%) |
| RISC-V only | 8.7 KB (27%) | 1.2 KB (15%) |
| Dual (Cortex-M + RISC-V) | 16.2 KB (49%) | 1.4 KB (17%) |

Tiny build (C1104, `PROBE_TINY_RAM=ON`, Cortex-M only):

| Configuration | Flash | SRAM |
|---------------|-------|------|
| C1104 tiny | 8.2 KB (50%) | 728 B (71%) |

## Requirements

- CMake 3.20+
- Arm GNU Toolchain (`arm-none-eabi-gcc`, `arm-none-eabi-size`, `arm-none-eabi-objcopy`)
- TI MSPM0 SDK installed locally (default path):
  - `/Applications/ti/mspm0_sdk_2_08_00_03/`

You can override the SDK location with:
- `-DMSPM0_SDK_PATH=/path/to/mspm0_sdk`

## Configure + Build

Tiny C1104 build (recommended for 1KB SRAM validation):
```
cmake -S . -B build_c1104 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1104 -DPROBE_TINY_RAM=ON
cmake --build build_c1104 -j
```

Full C1105 build (defaults to XML + watchpoints):
```
cmake -S . -B build_c1105 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_TINY_RAM=OFF
cmake --build build_c1105 -j
```

RISC-V only build (ultra-minimal, no Cortex-M):
```
cmake -S . -B build_rv -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_ENABLE_CORTEXM=OFF -DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON
cmake --build build_rv -j
```

Dual architecture build (Cortex-M + RISC-V with runtime detection):
```
cmake -S . -B build_dual -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_ENABLE_JTAG=ON -DPROBE_ENABLE_RISCV=ON
cmake --build build_dual -j
```

### Architecture Options

- `-DPROBE_ENABLE_CORTEXM=ON` (default) - Enable Cortex-M debug via SWD
- `-DPROBE_ENABLE_JTAG=OFF` - Enable JTAG bit-bang wire protocol
- `-DPROBE_ENABLE_RISCV=OFF` - Enable RISC-V debug (requires JTAG)

At least one of `PROBE_ENABLE_CORTEXM` or `PROBE_ENABLE_RISCV` must be enabled.

### Other Feature Toggles

- `-DPROBE_ENABLE_QXFER_TARGET_XML=OFF` - Disable target XML
- `-DPROBE_ENABLE_DWT_WATCHPOINTS=OFF` - Disable DWT watchpoints
- `-DPROBE_SWD_DELAY_US=0` - SWD clock delay (increase for slow targets)
- `-DPROBE_USE_HFXT=ON` - Use external crystal (C1105 only)

## Usage (GDB)

Connect the probe UART to your host and point GDB at the serial port:
```
(gdb) target remote /dev/tty.usbserial-XXXX
```

If you need to set the baud rate explicitly:
```
(gdb) set serial baud 115200
(gdb) target remote /dev/tty.usbserial-XXXX
```

## Flashing the Probe (FYI)

Goal:
- Make a schematic + PCB that supports the MSPM0 on‑chip UART bootloader
- Show one MSPM0 debugger flashing another one, so you only bootstrap a single unit
- Flash with JLink, to make sure it works & can debug the project itself

## Clocking Notes

As this has not had a hardware built yet, this is all estimated, pending testing!

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
