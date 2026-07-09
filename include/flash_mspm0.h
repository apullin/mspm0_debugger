#pragma once

// MSPM0-target flash programming over SWD (drives the target's FLASHCTL
// via MEM-AP accesses). Only valid when the attached target is an MSPM0;
// used by the RSP vFlash* handlers.

#include <stdbool.h>
#include <stdint.h>

// Read main-flash geometry from the target's FLASHCTL info registers.
// Returns false if the registers can't be read or look implausible.
bool flash_mspm0_geometry(uint32_t *main_size_bytes, uint32_t *sector_size_bytes);

// Erase every sector overlapping [addr, addr+len). Target must be halted.
bool flash_mspm0_erase_range(uint32_t addr, uint32_t len);

// Stage/program bytes from monotonically advancing vFlashWrite ranges. Packet
// boundaries may split a physical 64- or 128-bit flash word, but ranges must
// not overlap or revisit an already-programmed word. The range must have been
// erased first.
bool flash_mspm0_write(uint32_t addr, const uint8_t *data, uint32_t len);

// Flush the final partial flash word and finish the programming transaction.
// Must be called for vFlashDone; its failure must be reported to the host.
bool flash_mspm0_done(void);

// Discard a staged partial word and reset transaction/order tracking. Use when
// an RSP flash transaction or connection is abandoned.
void flash_mspm0_abort(void);
