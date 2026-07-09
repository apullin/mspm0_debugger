## GOAL

We want an ultra-minimal GDB Remote Serial Protocol (RSP) “probe” firmware that talks SWD to a Cortex‑M target (initially: generic Cortex‑M0+/Cortex‑M, not MSPM0-specific). The probe runs on a cheap MSPM0 MCU and is controlled over UART by GDB.

## STARTING POINT

This repo now uses a split, module-based layout:

- `main.c`: calls `board_init()` and runs `probe_poll()`.
- `hal.h`: minimal HAL (UART, delay, SWD GPIO).
- `src/board_mspm0c1104.c`, `src/board_mspm0c1105.c`, `src/board_mspm0g5187.c`: MSPM0 DriverLib bring-up + HAL implementation.
- `src/swd_bitbang.c`: SWD wire protocol bit-bang (turnaround, ACK, parity).
- `src/adiv5.c`: ADIv5 Debug Port / Access Port transactions (this is ARM “Debug Interface v5”, not “division”).
- `src/target_mem.c`: memory reads/writes via AHB‑AP.
- `src/cortex.c`: Cortex‑M debug (DHCSR/DCRSR), halt/step/continue, FPB breakpoints.
- `src/intmath.c`: compact unsigned 64-by-32 division for exact clock scaling
  without the large generic Cortex-M0+ runtime helper.
- `src/rsp.c`: UART RSP packet parser + command handling.
- `src/probe.c`: glue between RSP and SWD/ADIv5/Cortex.

## Hardware Notes

Probe clocks:

- `MSPM0C1104` (“tiny”): use SYSOSC base frequency (24 MHz per MSPM0C110x datasheet/DFP metadata).
- `MSPM0C1105` (“bigger”): use SYSOSC base frequency (32 MHz).
- `MSPM0G5187` (“USB”): currently use SYSOSC at 32 MHz; USB has a separate 48 MHz USBFLL clock.

Timing uses a free-running SysTick (24-bit). `delay_us()` and `hal_time_us()`
retain sub-MHz clock fractions through the project-owned divider/remainder path.

The schematic is not set. The placeholder PA0/PA1 mapping for shared
SWCLK/TCK and SWDIO/TMS uses ODIO pins, which cannot drive a production debug
clock; configuration fails unless the developer
explicitly acknowledges it with `PROBE_ALLOW_ODIO_SWD_PINS=ON`. Final hardware
must move both shared debug signals to SDIO/HSIO-capable pins.

When we have this all building, we check the consumed resources (linker prints memory usage, and we also run `arm-none-eabi-size`).

## TARGET HARDWARE

Current focus is the MSPM0C1 series:

- `MSPM0C1104` (tiny target; 16KB flash / 1KB SRAM / 24 MHz)
- Optional: `MSPM0C1105` (more headroom; 32KB flash / 8KB SRAM / 32 MHz)

The original L-series notes are considered deprecated for this repo’s current direction.

## BUILD SYSTEM

Toolchain: `arm-none-eabi-gcc` via `cmake/toolchains/arm-gcc.cmake`.

SDK path is a CMake cache variable:

- `MSPM0_SDK_PATH` defaults to `/Applications/ti/mspm0_sdk_2_09_00_01/`

Targets:

- C1104 tiny (recommended for “does it fit in 1KB SRAM?”):
  - `cmake -S . -B build_c1104 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1104 -DPROBE_TINY_RAM=ON -DPROBE_ALLOW_ODIO_SWD_PINS=ON`
- C1105 bigger (“full feature”):
  - `cmake -S . -B build_c1105 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-gcc.cmake -DPROBE_DEVICE=MSPM0C1105 -DPROBE_TINY_RAM=OFF -DPROBE_ALLOW_ODIO_SWD_PINS=ON`
    - Defaults to enabling `qXfer:features:read` target XML and DWT watchpoints; override with:
      - `-DPROBE_ENABLE_QXFER_TARGET_XML=OFF`
      - `-DPROBE_ENABLE_DWT_WATCHPOINTS=OFF`

Then build:

- `cmake --build build_c1104 -j`
- `cmake --build build_c1105 -j`

Notes:

- Link uses `-Wl,--print-memory-usage` (and produces `mspm0_debugger.map`).
- Device-dependent generated defaults and the derived board clock migrate when
  `PROBE_DEVICE` changes, but separate build directories remain easier to inspect.
