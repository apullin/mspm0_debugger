// JTAG bit-bang implementation for RISC-V debug.
// Implements IEEE 1149.1 TAP state machine and RISC-V DTM access.

#include "jtag.h"

#if defined(PROBE_ENABLE_JTAG) && (PROBE_ENABLE_JTAG)

#include <stddef.h>

#include "hal.h"

#ifndef JTAG_DELAY_US
#define JTAG_DELAY_US 0u
#endif

static jtag_state_t g_tap_state = JTAG_STATE_RESET;

// IR length for RISC-V DTM (typically 5 bits)
#define JTAG_IR_LEN 5u

// DMI parameters (read from DTMCS)
static uint8_t g_dmi_abits = 7u;  // default, will be updated from DTMCS

static inline void jtag_delay(void)
{
    if (JTAG_DELAY_US) {
        delay_us(JTAG_DELAY_US);
    }
}

static inline void jtag_clock(int tms, int tdi)
{
    jtag_tms_write(tms);
    jtag_tdi_write(tdi);
    jtag_delay();
    jtag_tck_write(1);
    jtag_delay();
    jtag_tck_write(0);
}

static inline int jtag_clock_capture(int tms, int tdi)
{
    jtag_tms_write(tms);
    jtag_tdi_write(tdi);
    jtag_delay();
    jtag_tck_write(1);
    int tdo = jtag_tdo_read();
    jtag_delay();
    jtag_tck_write(0);
    return tdo;
}

// TAP state machine transitions
static const jtag_state_t tap_next[16][2] = {
    // [current_state][TMS] = next_state
    [JTAG_STATE_RESET]      = { JTAG_STATE_IDLE,       JTAG_STATE_RESET },
    [JTAG_STATE_IDLE]       = { JTAG_STATE_IDLE,       JTAG_STATE_SELECT_DR },
    [JTAG_STATE_SELECT_DR]  = { JTAG_STATE_CAPTURE_DR, JTAG_STATE_SELECT_IR },
    [JTAG_STATE_CAPTURE_DR] = { JTAG_STATE_SHIFT_DR,   JTAG_STATE_EXIT1_DR },
    [JTAG_STATE_SHIFT_DR]   = { JTAG_STATE_SHIFT_DR,   JTAG_STATE_EXIT1_DR },
    [JTAG_STATE_EXIT1_DR]   = { JTAG_STATE_PAUSE_DR,   JTAG_STATE_UPDATE_DR },
    [JTAG_STATE_PAUSE_DR]   = { JTAG_STATE_PAUSE_DR,   JTAG_STATE_EXIT2_DR },
    [JTAG_STATE_EXIT2_DR]   = { JTAG_STATE_SHIFT_DR,   JTAG_STATE_UPDATE_DR },
    [JTAG_STATE_UPDATE_DR]  = { JTAG_STATE_IDLE,       JTAG_STATE_SELECT_DR },
    [JTAG_STATE_SELECT_IR]  = { JTAG_STATE_CAPTURE_IR, JTAG_STATE_RESET },
    [JTAG_STATE_CAPTURE_IR] = { JTAG_STATE_SHIFT_IR,   JTAG_STATE_EXIT1_IR },
    [JTAG_STATE_SHIFT_IR]   = { JTAG_STATE_SHIFT_IR,   JTAG_STATE_EXIT1_IR },
    [JTAG_STATE_EXIT1_IR]   = { JTAG_STATE_PAUSE_IR,   JTAG_STATE_UPDATE_IR },
    [JTAG_STATE_PAUSE_IR]   = { JTAG_STATE_PAUSE_IR,   JTAG_STATE_EXIT2_IR },
    [JTAG_STATE_EXIT2_IR]   = { JTAG_STATE_SHIFT_IR,   JTAG_STATE_UPDATE_IR },
    [JTAG_STATE_UPDATE_IR]  = { JTAG_STATE_IDLE,       JTAG_STATE_SELECT_DR },
};

void jtag_init(void)
{
    jtag_tck_write(0);
    jtag_tms_write(1);
    jtag_tdi_write(0);
    jtag_reset();
}

jtag_state_t jtag_get_state(void)
{
    return g_tap_state;
}

void jtag_tms(int tms)
{
    tms = tms ? 1 : 0;
    jtag_clock(tms, 0);
    g_tap_state = tap_next[g_tap_state][tms];
}

void jtag_reset(void)
{
    // 5+ TCK with TMS=1 guarantees reset from any state
    for (int i = 0; i < 6; i++) {
        jtag_clock(1, 0);
    }
    g_tap_state = JTAG_STATE_RESET;
}

void jtag_idle(void)
{
    // Navigate to Run-Test/Idle from any state
    if (g_tap_state == JTAG_STATE_RESET) {
        jtag_tms(0);  // Reset -> Idle
    } else {
        // From any other state, go through Reset first
        jtag_reset();
        jtag_tms(0);
    }
}

// Navigate from Idle to Shift-IR or Shift-DR
static void jtag_goto_shift_ir(void)
{
    if (g_tap_state != JTAG_STATE_IDLE) {
        jtag_idle();
    }
    jtag_tms(1);  // Idle -> Select-DR
    jtag_tms(1);  // Select-DR -> Select-IR
    jtag_tms(0);  // Select-IR -> Capture-IR
    jtag_tms(0);  // Capture-IR -> Shift-IR
}

static void jtag_goto_shift_dr(void)
{
    if (g_tap_state != JTAG_STATE_IDLE) {
        jtag_idle();
    }
    jtag_tms(1);  // Idle -> Select-DR
    jtag_tms(0);  // Select-DR -> Capture-DR
    jtag_tms(0);  // Capture-DR -> Shift-DR
}

// Shift bits through the currently selected register (IR or DR).
// LSB first. On last bit, TMS=1 to exit to Exit1 state.
static void jtag_shift_bits(const uint8_t *tdi, uint8_t *tdo, uint32_t bits)
{
    for (uint32_t i = 0; i < bits; i++) {
        int tdi_bit = 0;
        if (tdi) {
            tdi_bit = (tdi[i / 8] >> (i % 8)) & 1;
        }

        // On last bit, set TMS=1 to exit to Exit1
        int tms = (i == bits - 1) ? 1 : 0;
        int tdo_bit = jtag_clock_capture(tms, tdi_bit);

        if (tdo) {
            if (i % 8 == 0) {
                tdo[i / 8] = 0;
            }
            tdo[i / 8] |= (uint8_t)(tdo_bit << (i % 8));
        }
    }

    // Update TAP state to Exit1
    if (g_tap_state == JTAG_STATE_SHIFT_IR) {
        g_tap_state = JTAG_STATE_EXIT1_IR;
    } else if (g_tap_state == JTAG_STATE_SHIFT_DR) {
        g_tap_state = JTAG_STATE_EXIT1_DR;
    }
}

void jtag_shift_ir(const uint8_t *tdi_data, uint8_t *tdo_data, uint32_t bits)
{
    jtag_goto_shift_ir();
    jtag_shift_bits(tdi_data, tdo_data, bits);
}

void jtag_shift_dr(const uint8_t *tdi_data, uint8_t *tdo_data, uint32_t bits)
{
    jtag_goto_shift_dr();
    jtag_shift_bits(tdi_data, tdo_data, bits);
}

void jtag_write_ir(const uint8_t *data, uint32_t bits)
{
    jtag_shift_ir(data, NULL, bits);
    jtag_tms(1);  // Exit1-IR -> Update-IR
    jtag_tms(0);  // Update-IR -> Idle
}

void jtag_write_dr(const uint8_t *data, uint32_t bits)
{
    jtag_shift_dr(data, NULL, bits);
    jtag_tms(1);  // Exit1-DR -> Update-DR
    jtag_tms(0);  // Update-DR -> Idle
}

uint32_t jtag_read_dr32(uint32_t bits)
{
    uint8_t tdo[4] = {0};
    uint8_t tdi[4] = {0};  // shift in zeros

    if (bits > 32) bits = 32;

    jtag_shift_dr(tdi, tdo, bits);
    jtag_tms(1);  // Exit1-DR -> Update-DR
    jtag_tms(0);  // Update-DR -> Idle

    uint32_t result = 0;
    for (uint32_t i = 0; i < (bits + 7) / 8 && i < 4; i++) {
        result |= ((uint32_t)tdo[i]) << (i * 8);
    }
    return result;
}

// --- RISC-V DTM Access ---

uint32_t jtag_read_idcode(void)
{
    uint8_t ir = JTAG_IR_IDCODE;
    jtag_write_ir(&ir, JTAG_IR_LEN);
    return jtag_read_dr32(32);
}

uint32_t jtag_read_dtmcs(void)
{
    uint8_t ir = JTAG_IR_DTMCS;
    jtag_write_ir(&ir, JTAG_IR_LEN);
    uint32_t dtmcs = jtag_read_dr32(32);

    // Extract abits for DMI operations
    g_dmi_abits = (uint8_t)((dtmcs >> 4) & 0x3Fu);
    if (g_dmi_abits == 0) {
        g_dmi_abits = 7;  // fallback default
    }

    return dtmcs;
}

// DMI register format:
// [1:0]   op    - 0=nop, 1=read, 2=write
// [33:2]  data  - 32-bit data
// [33+abits:34] address - abits-bit address
// Total: 2 + 32 + abits bits

#define DMI_OP_NOP   0u
#define DMI_OP_READ  1u
#define DMI_OP_WRITE 2u
#define DMI_OP_MASK  3u

static bool jtag_dmi_op(uint32_t addr, uint32_t data_in, uint8_t op, uint32_t *data_out)
{
    // Select DMI
    uint8_t ir = JTAG_IR_DMI;
    jtag_write_ir(&ir, JTAG_IR_LEN);

    // Build DMI request: [op:2][data:32][addr:abits]
    // Total bits = 2 + 32 + abits
    uint32_t total_bits = 2u + 32u + g_dmi_abits;
    uint8_t tdi[8] = {0};
    uint8_t tdo[8] = {0};

    // Pack: op in bits [1:0], data in bits [33:2], addr in bits [33+abits:34]
    uint64_t request = ((uint64_t)op & 3u) |
                       ((uint64_t)data_in << 2) |
                       ((uint64_t)addr << 34);

    for (int i = 0; i < 8; i++) {
        tdi[i] = (uint8_t)(request >> (i * 8));
    }

    // Shift request, get response from previous operation
    jtag_shift_dr(tdi, tdo, total_bits);
    jtag_tms(1);  // Exit1-DR -> Update-DR
    jtag_tms(0);  // Update-DR -> Idle

    // For read operations, we need a second shift to get the data
    // (first shift sends request, second shift gets response)
    if (op == DMI_OP_READ) {
        // Send NOP to clock out the read response
        request = DMI_OP_NOP;
        for (int i = 0; i < 8; i++) {
            tdi[i] = (uint8_t)(request >> (i * 8));
        }

        jtag_shift_dr(tdi, tdo, total_bits);
        jtag_tms(1);
        jtag_tms(0);
    }

    // Parse response
    uint64_t response = 0;
    for (int i = 0; i < 8; i++) {
        response |= ((uint64_t)tdo[i]) << (i * 8);
    }

    uint8_t resp_op = response & 3u;
    uint32_t resp_data = (uint32_t)((response >> 2) & 0xFFFFFFFFu);

    if (data_out) {
        *data_out = resp_data;
    }

    // op field: 0=success, 2=failed, 3=busy
    return (resp_op == 0);
}

bool jtag_dmi_read(uint32_t addr, uint32_t *data)
{
    return jtag_dmi_op(addr, 0, DMI_OP_READ, data);
}

bool jtag_dmi_write(uint32_t addr, uint32_t data)
{
    return jtag_dmi_op(addr, data, DMI_OP_WRITE, NULL);
}

#endif // PROBE_ENABLE_JTAG
