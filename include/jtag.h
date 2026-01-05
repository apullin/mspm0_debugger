#pragma once

// JTAG bit-bang interface for RISC-V debug access.
// Implements IEEE 1149.1 TAP controller and RISC-V DTM access.

#include <stdbool.h>
#include <stdint.h>

// JTAG TAP states
typedef enum {
    JTAG_STATE_RESET,
    JTAG_STATE_IDLE,
    JTAG_STATE_SELECT_DR,
    JTAG_STATE_CAPTURE_DR,
    JTAG_STATE_SHIFT_DR,
    JTAG_STATE_EXIT1_DR,
    JTAG_STATE_PAUSE_DR,
    JTAG_STATE_EXIT2_DR,
    JTAG_STATE_UPDATE_DR,
    JTAG_STATE_SELECT_IR,
    JTAG_STATE_CAPTURE_IR,
    JTAG_STATE_SHIFT_IR,
    JTAG_STATE_EXIT1_IR,
    JTAG_STATE_PAUSE_IR,
    JTAG_STATE_EXIT2_IR,
    JTAG_STATE_UPDATE_IR,
} jtag_state_t;

// Initialize JTAG interface and reset TAP to known state.
void jtag_init(void);

// Get current TAP state.
jtag_state_t jtag_get_state(void);

// Clock TMS with given value, update TAP state.
void jtag_tms(int tms);

// Reset TAP state machine (5+ TCK with TMS=1).
void jtag_reset(void);

// Move TAP to Run-Test/Idle state.
void jtag_idle(void);

// Shift data through IR or DR.
// Shifts `bits` bits from `tdi_data` (LSB first), captures TDO into `tdo_data`.
// Leaves TAP in Exit1-IR/DR state (caller must move to Update or Pause).
// If tdo_data is NULL, TDO is ignored.
void jtag_shift_ir(const uint8_t *tdi_data, uint8_t *tdo_data, uint32_t bits);
void jtag_shift_dr(const uint8_t *tdi_data, uint8_t *tdo_data, uint32_t bits);

// Convenience: shift IR/DR and return to Idle.
void jtag_write_ir(const uint8_t *data, uint32_t bits);
void jtag_write_dr(const uint8_t *data, uint32_t bits);
uint32_t jtag_read_dr32(uint32_t bits);

// RISC-V Debug Transport Module (DTM) access via JTAG.
// Standard RISC-V JTAG IR values:
#define JTAG_IR_IDCODE   0x01u
#define JTAG_IR_DTMCS    0x10u
#define JTAG_IR_DMI      0x11u
#define JTAG_IR_BYPASS   0x1Fu

// Read IDCODE (32-bit).
uint32_t jtag_read_idcode(void);

// Read DTMCS register.
uint32_t jtag_read_dtmcs(void);

// DMI read/write (address width and data come from DTMCS).
// Returns true on success, false on error (busy/error in op field).
bool jtag_dmi_read(uint32_t addr, uint32_t *data);
bool jtag_dmi_write(uint32_t addr, uint32_t data);
