# Second-Pass Improvements

Cribbed during the 2026-07 audit/fix pass. None of these are defects — the
first pass fixed those — these are features, optimizations, and hardening
that deserve their own designs.

## Protocol / features

- ~~`X` binary write packets + 0x7d escape decoding~~ — **done** (2026-07-09,
  along with `vFlash*`/memory-map flash programming for MSPM0 targets and
  host-side unit tests in `tests/`).
- **Generic-target flash.** vFlash currently drives the MSPM0 FLASHCTL only;
  other Cortex-M vendors need per-family drivers or CMSIS-Pack-style flash
  loaders staged into target RAM. Until then, non-MSPM0 targets are
  attach/RAM-debug only (flash via the vendor's own tools).
- **Flash size sanity vs. NONMAIN.** The memory map auto-detects main-flash
  geometry from FLASHCTL info registers; consider also refusing vFlashErase
  ranges that overlap a running probe's own address when self-flashing rigs
  are wired probe-to-probe.
- **ADIv6 AP addressing.** The dormant-mode wake (d7a986f) is bit-exact, but
  AP discovery still uses ADIv5 `SELECT.APSEL` addressing, so a woken
  ADIv6/M55 target answers DPIDR and then discovery fails. Needs DPIDR1 /
  BASEPTR0 reads, `SELECT.ADDR[31:4]` addressing, and a ROM-table walk.
- **Vector catch.** DEMCR `VC_CORERESET`/`VC_HARDERR` are never programmed,
  so target resets and hard faults can't be trapped and reported to GDB.
  Small add, big debugging-UX win.
- **`vCont`.** Memory-map transfer and `vFlash*` are implemented; richer
  thread/action control remains a separate protocol enhancement.
- ~~**Detach cleanup.**~~ **Done (2026-07-09):** `D`/`k` disable every
  session-owned FPB/DWT comparator or RISC-V trigger before resuming; failed
  hardware clears retain bookkeeping and return an RSP error for retry.
- ~~**Retransmit support for TINY_RAM builds.**~~ **Done (2026-07-09):**
  C1104 reuses its receive packet buffer after dispatch, avoiding the 520 B
  dedicated reply buffer while still supporting retransmit-on-NACK.

## RISC-V

- **Pipelined DMI.** The busy-status fix made every DMI op two scans
  (request + NOP collect). OpenOCD instead pipelines: check the previous
  op's status in the next op's capture, only inserting a NOP at batch
  boundaries. Roughly halves JTAG traffic for memory transfers.
- **SBA streaming reads/writes.** Use `sbreadondata` + `sbautoincrement` to
  stream sequential words with one DMI op per word instead of three.
- **Program-buffer memory access fallback.** Between SBA and byte-wise
  abstract Access Memory there's a third path (progbuf + abstractauto) that
  most real RV32 DMs support and that is fast; abstract Access Memory is
  optional in 0.13 and rarely implemented.
- **Watchpoint-hit fallback when `mcontrol.hit` is unimplemented** (it's
  optional): e.g. if exactly one watchpoint is armed, report that one;
  otherwise report a plain SIGTRAP.
- **`ndmreset` / reset support** for RISC-V targets (the `R` packet and
  GDB `monitor reset` have no RISC-V path).

## Cortex-M

- ~~**Byte/halfword MEM-AP CSW transfers.**~~ **Done (2026-07-09):** partial
  Cortex-M accesses use native 8-bit MEM-AP transfers with byte-lane placement
  instead of destructive 32-bit read-modify-write cycles.
- **Multi-drop SWD (SWD v2 TARGETSEL)** — explicitly deferred in
  QUESTIONS.md; the dormant-wake plumbing is already in place.
- ~~**NUM_CODE bits [14:12].**~~ **Done (2026-07-09):** FPB discovery combines
  both `NUM_CODE` fields before clamping to the probe's eight tracked slots.

## Infra / build / hardware bring-up

- **SWD speed auto-tuning** (QUESTIONS.md wishlist): start slow, read
  DPIDR, decrease `SWD_DELAY_US` until errors, back off.
- **80 MHz SYSPLL on G5187** (TODO in board file). `delay_us`,
  `systick_init` and `PROBE_CORE_CLK_HZ` are now all keyed off one macro,
  so the switch is one place.
- ~~**Per-probe USB serial.**~~ **Done (2026-07-09):** the G5187 USB
  descriptor derives its serial from TI's factory-programmed TRACEID, so
  multiple probes no longer enumerate with the same `MSPM0-0001` value.
- **CDC TX batching.** `uart_putc` flushes per byte; a `uart_flush()` HAL
  hook called at packet end would cut USB overhead.
- ~~Host-side unit tests~~ — **done** (2026-07-09): `tests/` builds full,
  tiny Cortex, tiny RISC-V, and legacy-no-XML RSP profiles plus register-level
  production FLASHCTL, ADIv5/MEM-AP, Cortex, and RISC-V driver suites under
  ASan/UBSan. Next step: add pty/GDB smoke coverage in CI as tool availability
  permits.
- **LaunchPad-to-LaunchPad pin preset.** For the board-to-board debug-header
  topology: remap SWCLK/SWDIO to the probe's own PA20/PA19 (its debug
  header pins), make NRESET optional, and add a boot-grace delay before
  taking over PA19/PA20 so the probe itself stays reflashable over SWD.
- ~~**CI build matrix.**~~ **Done (2026-07-09):** GitHub Actions runs the
  host tests on Linux/macOS, all 9 device/architecture firmware builds, and
  legacy-no-XML, single-CDC, 32 MHz HFXT, and 32-bit JTAG-IR edge profiles,
  with `-Wall -Wextra` promoted to `-Werror`.
- **`CMAKE_EXPORT_COMPILE_COMMANDS=ON`** + a checked-in `.clangd` pointing
  at one build dir, so IDE diagnostics stop being noise.
- **Delete stale `build_*` directories** (~2 GB) and add `build*/` to
  `.gitignore`; the audit builds live in scratch anyway.
- **BOOTRST vs probe state.** `PROBE_ENABLE_SYSOSC_FCL` is sticky until
  BOOTRST; document interaction if the probe firmware ever soft-resets.

## Hardware / bring-up board

- **PIN CHOICE WARNING: PA0/PA1 are ODIO pins** (open-drain only, no
  push-pull high, ~1 MHz rated, 5 V tolerant — they're the I2C pins). The
  current board-file defaults put shared SWCLK/TCK and SWDIO/TMS there as placeholders; the
  build now requires the explicit `PROBE_ALLOW_ODIO_SWD_PINS=ON` bring-up
  acknowledgement, and the real schematic must use SDIO/HSIO-class pins for
  both shared debug signals. An ODIO
  pin IS the ideal home for nRESET (open-drain + pull-up by design).
- **VTref sensing via ADC**: one channel behind a ~100k/100k divider
  (+ ~10 nF) covers 0-6.6 V at ~1.6 mV/LSB — plenty to classify
  off/1.8/3.3/5V. A second undivided channel adds low-range accuracy but
  would itself see 5 V on a 5 V target, so it needs clamping; the divided
  channel alone is the simpler, safer call. Use extended ADC sample time
  for the ~50k source impedance. Firmware follow-ups: read the channel at
  attach, refuse to drive SWD into an unpowered target (VTref < ~1.2 V),
  and report the rail (e.g. a future `monitor vtref`). Note: *sensing* 5 V
  is not *tolerating* 5 V — standard MSPM0 IOs are not 5 V tolerant, so a
  5 V target needs level shifters regardless; the sense line's job is to
  detect and refuse.
- **SWD line conditioning**: 22-33R series resistors on SWCLK/SWDIO,
  10k-47k pull-up on SWDIO (100k is weak for turnaround), optional
  pull-down on SWCLK, short cable.
- **SPI-accelerated SWD backend (firmware-only, if pins are routed for
  it)**: SWD is static-timing — SWCLK may pause indefinitely between SPI
  frames — so the wire protocol can be driven by the SPI peripheral for a
  ~10-20x throughput win over bitbang (~4-8 MHz vs ~200-500 kHz). Wiring:
  SWCLK on a pin muxable to SPI SCLK; SWDIO read directly on POCI; PICO
  drives SWDIO through ~470R (host clocks all-ones during target-driven
  phases, target wins through the resistor — the resistor doubles as the
  SWDIO series resistor). MSPM0 SPI does 4-16-bit frames + LSB-first, so
  the 46-bit transactions (8 req + trn + 3 ack + 33 data + trn) compose
  from mixed frame sizes; the bit-reassembly is the fiddly part. Keep
  GPIO bitbang as the fallback via runtime IOMUX switch. Same idea maps
  even more cleanly to JTAG (TDI=PICO, TDO=POCI, TCK=SCLK, TMS bitbang).
- **SWO/SWV capture (provision now, implement later).** Connector pin 6 is
  SWO/TDO — both are probe *inputs*, so route it to one GPIO that can mux
  to a UART RX function: plain GPIO read covers JTAG TDO, the UART covers
  SWO NRZ capture. Only viable on C1105 (UART1) and G5187 (spare UNICOMM
  + forward over the second USB CDC port); C110x has only UART0 (= the GDB
  link), so no SWO there. Firmware side: UART RX at the SWO baud streamed
  to CDC, plus a `monitor swo <baud>` that programs the target's
  DEMCR.TRCENA / TPIU prescaler / ITM enable.
- **Locked-in schematic checklist (as of 2026-07-09)**: SWCLK/SWDIO on
  SPI-capable SDIO/HSIO pins (never PA0/PA1); nRESET on an ODIO pin +
  pull-up; VTref via 100k/100k divider into an ADC pin with 10 nF from
  the divider midpoint to GND (charge reservoir for the SAR sample cap;
  50k source is too stiff without it); 33R series on SWCLK (ring/double-
  clock damping — the important one); connector pin 6 (SWO/TDO) to a
  UART-RX-capable GPIO; all three GND pins; avoid PA18 (BSL invoke)
  anywhere near the SWDIO net.

  SWDIO net topology (470R is contention limiting for the SPI branch
  ONLY; 33R is optional line conditioning for both paths):

  ```
  SPI PICO pin ----[470R]----+
                             +----[33R]---- connector pin 2 (SWDIO)
  POCI / bitbang pin --------+
        (direct)
  ```

## Hardware notes discovered during the fix pass

- **C1105: JTAG (TDI/TDO on PA3/PA4 = PINCM6/7) and HFXT share pins** —
  now a compile error when both are enabled; the schematic needs to pick
  one or move the JTAG pins.
- **C110x has no PA3** — C1104 TDI/TDO are PA4 (PINCM5) / PA6 (PINCM7).
- **G5187 PA3/PA4 double as LFXT pins** (unused by this firmware, but a
  32 kHz crystal on the board would conflict with JTAG).
- **SWDIO is now push-pull while driving** (Hi-Z + pull-up when
  listening), per the QUESTIONS.md intent — the planned external 100k
  pull-up is still wanted for the idle/turnaround states.
