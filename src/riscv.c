// RISC-V Debug Module implementation.
// Implements RISC-V Debug Spec 0.13 via JTAG DMI.

#include "riscv.h"

#if defined(PROBE_ENABLE_RISCV) && (PROBE_ENABLE_RISCV)

#include <stddef.h>

#include "hal.h"
#include "jtag.h"
#include "target.h"  // for target_watch_t

// Debug Module registers (DMI addresses)
#define DM_DATA0        0x04u
#define DM_DATA1        0x05u
#define DM_DMCONTROL    0x10u
#define DM_DMSTATUS     0x11u
#define DM_HARTINFO     0x12u
#define DM_ABSTRACTCS   0x16u
#define DM_COMMAND      0x17u
#define DM_ABSTRACTAUTO 0x18u
#define DM_PROGBUF0     0x20u
#define DM_PROGBUF1     0x21u
#define DM_SBCS         0x38u
#define DM_SBADDRESS0   0x39u
#define DM_SBDATA0      0x3Cu

// DMCONTROL bits
#define DMCONTROL_DMACTIVE      (1u << 0)
#define DMCONTROL_NDMRESET      (1u << 1)
#define DMCONTROL_HALTREQ       (1u << 31)
#define DMCONTROL_RESUMEREQ     (1u << 30)
#define DMCONTROL_HARTRESET     (1u << 29)
#define DMCONTROL_ACKHAVERESET  (1u << 28)
#define DMCONTROL_SETRESETHALTREQ (1u << 3)

// DMSTATUS bits
#define DMSTATUS_VERSION_MASK   0x0Fu
#define DMSTATUS_ALLHALTED      (1u << 9)
#define DMSTATUS_ANYHALTED      (1u << 8)
#define DMSTATUS_ALLRUNNING     (1u << 11)
#define DMSTATUS_ANYRUNNING     (1u << 10)
#define DMSTATUS_ALLRESUMEACK   (1u << 17)
#define DMSTATUS_ANYRESUMEACK   (1u << 16)
#define DMSTATUS_AUTHENTICATED  (1u << 7)
#define DMSTATUS_HASRESETHALTREQ (1u << 5)

// ABSTRACTCS bits
#define ABSTRACTCS_DATACOUNT_MASK   0x0Fu
#define ABSTRACTCS_CMDERR_MASK      (7u << 8)
#define ABSTRACTCS_CMDERR_SHIFT     8
#define ABSTRACTCS_BUSY             (1u << 12)
#define ABSTRACTCS_PROGBUFSIZE_MASK (0x1Fu << 24)
#define ABSTRACTCS_PROGBUFSIZE_SHIFT 24

// Abstract command types
#define AC_ACCESS_REGISTER  0u
#define AC_QUICK_ACCESS     1u
#define AC_ACCESS_MEMORY    2u

// Access Register command fields
#define AC_AR_POSTEXEC      (1u << 18)
#define AC_AR_TRANSFER      (1u << 17)
#define AC_AR_WRITE         (1u << 16)
#define AC_AR_AARSIZE_32    (2u << 20)
#define AC_AR_REGNO(n)      ((n) & 0xFFFFu)

// Register numbers for abstract commands
#define REG_GPR_BASE        0x1000u  // x0-x31 = 0x1000-0x101F
#define REG_CSR_BASE        0x0000u  // CSRs = 0x0000-0x0FFF
#define REG_DPC             0x7B1u   // Debug PC (CSR)

// SBCS (System Bus Control and Status) bits
#define SBCS_SBACCESS8      (0u << 17)
#define SBCS_SBACCESS32     (2u << 17)
#define SBCS_SBREADONADDR   (1u << 20)
#define SBCS_SBREADONDATA   (1u << 15)
#define SBCS_SBAUTOINCREMENT (1u << 16)
#define SBCS_SBBUSY         (1u << 21)
#define SBCS_SBBUSYERROR    (1u << 22)
#define SBCS_SBERROR_MASK   (7u << 12)
#define SBCS_SBACCESS8_SUPPORTED  (1u << 0) // capability bits (read-only)
#define SBCS_SBACCESS32_SUPPORTED (1u << 2)
#define SBCS_SBASIZE_MASK   (0x7Fu << 5)
#define SBCS_SBASIZE_SHIFT  5
#define SBCS_SBVERSION_MASK (7u << 29)
#define SBCS_SBVERSION_SHIFT 29
#define SBCS_SBVERSION_1_0  1u

// Abstract command error codes
#define CMDERR_NONE         0u
#define CMDERR_BUSY         1u
#define CMDERR_NOT_SUPPORTED 2u
#define CMDERR_EXCEPTION    3u
#define CMDERR_HALT_RESUME  4u
#define CMDERR_BUS          5u
#define CMDERR_OTHER        7u

// Trigger Module CSR addresses
#define CSR_TSELECT     0x7A0u
#define CSR_TDATA1      0x7A1u
#define CSR_TDATA2      0x7A2u
#define CSR_TINFO       0x7A4u
#define CSR_DCSR        0x7B0u

// mcontrol (type 2) and mcontrol6 (type 6) fields shared on RV32.
#define TRIGGER_TYPE_SHIFT       28
#define TRIGGER_TYPE_MASK        (0xFu << TRIGGER_TYPE_SHIFT)
#define TRIGGER_TYPE_MCONTROL    2u
#define TRIGGER_TYPE_MCONTROL6   6u
#define TRIGGER_TYPE_DISABLED    15u
#define TRIGGER_DMODE            (1u << 27)
#define TRIGGER_SELECT_DATA      (1u << 19)
#define TRIGGER_ACTION_MASK      (0xFu << 12)
#define TRIGGER_ACTION_DEBUG     (1u << 12)
#define TRIGGER_CHAIN            (1u << 11)
#define TRIGGER_MATCH_SHIFT      7
#define TRIGGER_MATCH_MASK       (0xFu << TRIGGER_MATCH_SHIFT)
#define TRIGGER_MATCH_EQUAL      0u
#define TRIGGER_MATCH_NAPOT      1u
#define TRIGGER_M                (1u << 6)
#define TRIGGER_S                (1u << 4)
#define TRIGGER_U                (1u << 3)
#define TRIGGER_EXECUTE          (1u << 2)
#define TRIGGER_STORE            (1u << 1)
#define TRIGGER_LOAD             (1u << 0)
#define TRIGGER_OP_MASK          (TRIGGER_EXECUTE | TRIGGER_STORE | TRIGGER_LOAD)
#define MCONTROL_HIT             (1u << 20)
#define MCONTROL6_HIT0           (1u << 22)
#define MCONTROL6_VU             (1u << 23)
#define MCONTROL6_VS             (1u << 24)
#define MCONTROL6_HIT1           (1u << 25)
#define MCONTROL6_SELECT_DATA     (1u << 21)

#define DCSR_STEP                (1u << 2)
#define DCSR_PRV_MASK            3u
#define DCSR_V                   (1u << 5)

// Timeout for operations (microseconds)
#define DM_TIMEOUT_US       100000u

// Maximum hardware triggers to probe
#define RISCV_MAX_TRIGGERS  4u

// Trigger slot tracking
typedef struct {
    uint32_t addr;
    uint32_t len;
    uint8_t  type;   // 0=unused, 1=breakpoint, 2=watchpoint
    uint8_t  watch;  // TARGET_WATCH_WRITE/READ/ACCESS
    uint8_t  hw_type; // TRIGGER_TYPE_MCONTROL or TRIGGER_TYPE_MCONTROL6
    bool     used;
} riscv_trigger_t;

static riscv_trigger_t g_triggers[RISCV_MAX_TRIGGERS];
static uint8_t g_num_triggers = 0;
static uint8_t g_num_usable_triggers = 0;
static bool g_triggers_probed = false;

static bool g_dm_active = false;
static uint8_t g_progbuf_size = 0;
static uint8_t g_data_count = 0;
static bool g_has_sba = false;  // SBA transport is usable this session
static bool g_has_sba8 = false;
static bool g_has_sba32 = false;
static bool g_has_abstract_mem = false;
static bool g_step_cleanup_pending = false;

static void riscv_transport_state_reset(void)
{
    g_dm_active = false;
    g_progbuf_size = 0;
    g_data_count = 0;
    g_has_sba = false;
    g_has_sba8 = false;
    g_has_sba32 = false;
    g_has_abstract_mem = false;
}

static void riscv_trigger_state_reset(void)
{
    g_triggers_probed = false;
    g_num_triggers = 0;
    g_num_usable_triggers = 0;
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        g_triggers[i] = (riscv_trigger_t){0};
    }
}

static bool range_is_valid(uint32_t addr, uint32_t len)
{
    return len == 0 || (len - 1u) <= (UINT32_MAX - addr);
}

static bool dm_wait_not_busy(void)
{
    uint32_t start = hal_time_us();
    uint32_t acs;

    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        if (!jtag_dmi_read(DM_ABSTRACTCS, &acs)) {
            return false;
        }
        if (!(acs & ABSTRACTCS_BUSY)) {
            return true;
        }
    }
    return false;
}

static bool dm_clear_cmderr(void)
{
    // Write 1s to cmderr field to clear it
    return jtag_dmi_write(DM_ABSTRACTCS, ABSTRACTCS_CMDERR_MASK);
}

static bool dm_exec_abstract(uint32_t cmd, uint32_t *data0_out)
{
    // Clear any previous error
    if (!dm_clear_cmderr()) return false;

    // Execute command
    if (!jtag_dmi_write(DM_COMMAND, cmd)) return false;

    // Wait for completion
    if (!dm_wait_not_busy()) return false;

    // Check for errors
    uint32_t acs;
    if (!jtag_dmi_read(DM_ABSTRACTCS, &acs)) return false;

    uint32_t cmderr = (acs & ABSTRACTCS_CMDERR_MASK) >> ABSTRACTCS_CMDERR_SHIFT;
    if (cmderr != CMDERR_NONE) {
        dm_clear_cmderr();
        return false;
    }

    // Read result if requested
    if (data0_out) {
        if (!jtag_dmi_read(DM_DATA0, data0_out)) return false;
    }

    return true;
}

// CSR read/write via Abstract Commands
static bool riscv_read_csr(uint32_t csr, uint32_t *val)
{
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 |
                   AC_AR_TRANSFER | AC_AR_REGNO(csr);
    return dm_exec_abstract(cmd, val);
}

static bool riscv_write_csr(uint32_t csr, uint32_t val)
{
    if (!jtag_dmi_write(DM_DATA0, val)) return false;
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 |
                   AC_AR_TRANSFER | AC_AR_WRITE | AC_AR_REGNO(csr);
    return dm_exec_abstract(cmd, NULL);
}

static bool trigger_disable_selected(void);
static bool riscv_clear_step(void);

static uint8_t riscv_owned_trigger_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        if (g_triggers[i].used) mask |= (uint8_t)(1u << i);
    }
    return mask;
}

// Clear comparators retained from a prior connection after DMI has been
// re-established. Invalid old indices imply a replacement/reset target and
// cannot still contain the old comparator; uncertain DMI failures retain
// ownership and make reattachment fail so cleanup can be retried.
static bool riscv_clear_reconnected_triggers(uint8_t owned_mask)
{
    if (owned_mask == 0) return true;

    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) return false;

    bool success = true;
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        if (!(owned_mask & (uint8_t)(1u << i))) continue;

        uint32_t selected;
        if (!riscv_write_csr(CSR_TSELECT, i) ||
            !riscv_read_csr(CSR_TSELECT, &selected)) {
            success = false;
            continue;
        }
        if (selected != i) {
            // This target has no such slot, so the old trigger cannot exist.
            g_triggers[i].used = false;
            continue;
        }
        if (trigger_disable_selected()) {
            g_triggers[i].used = false;
        } else {
            success = false;
        }
    }

    if (!riscv_write_csr(CSR_TSELECT, saved_tselect)) success = false;
    return success;
}

// Probe available triggers
static bool riscv_triggers_init(void)
{
    if (g_triggers_probed) return g_num_usable_triggers > 0;
    g_num_triggers = 0;
    g_num_usable_triggers = 0;

    // Clear all slots
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        g_triggers[i] = (riscv_trigger_t){0};
    }

    // Preserve the program's tselect value. Trigger CSRs are shared with
    // machine-mode software and an external debugger must not leave tselect
    // changed merely because it enumerated capabilities.
    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) {
        return false;
    }

    bool probe_ok = true;
    uint8_t usable = 0;

    // Probe the contiguous trigger indices. A nonzero tdata1 type does not
    // imply an address-match trigger: icount/itrigger/etc. are unusable for
    // GDB Z packets. Prefer mcontrol6, falling back to legacy mcontrol.
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        if (!riscv_write_csr(CSR_TSELECT, i)) {
            probe_ok = false;
            break;
        }

        uint32_t sel;
        if (!riscv_read_csr(CSR_TSELECT, &sel)) {
            probe_ok = false;
            break;
        }
        if (sel != i) break;  // Not as many triggers

        uint32_t tdata1;
        if (!riscv_read_csr(CSR_TDATA1, &tdata1)) {
            probe_ok = false;
            break;
        }
        uint32_t type = (tdata1 & TRIGGER_TYPE_MASK) >> TRIGGER_TYPE_SHIFT;
        if (type == 0) break;  // No trigger at this index

        uint32_t supported_types = 0;
        uint32_t tinfo;
        if (riscv_read_csr(CSR_TINFO, &tinfo)) {
            supported_types = tinfo & 0xFFFFu;
        } else if (type == TRIGGER_TYPE_MCONTROL ||
                   type == TRIGGER_TYPE_MCONTROL6) {
            // tinfo is optional for legacy, fixed-type mcontrol slots.
            supported_types = 1u << type;
        }

        if (supported_types & (1u << TRIGGER_TYPE_MCONTROL6)) {
            g_triggers[i].hw_type = TRIGGER_TYPE_MCONTROL6;
            usable++;
        } else if (supported_types & (1u << TRIGGER_TYPE_MCONTROL)) {
            g_triggers[i].hw_type = TRIGGER_TYPE_MCONTROL;
            usable++;
        }
        g_num_triggers = i + 1;
    }

    if (!riscv_write_csr(CSR_TSELECT, saved_tselect)) {
        probe_ok = false;
    }

    if (!probe_ok) {
        g_num_triggers = 0;
        return false;
    }

    g_num_usable_triggers = usable;
    g_triggers_probed = true;
    return usable > 0;
}

bool riscv_init(void)
{
    // Do not discard owned comparator or pending-step state on a reconnect.
    // Both live in the hart, not in dmactive-reset Debug Module state.
    uint8_t owned_triggers = riscv_owned_trigger_mask();
    riscv_transport_state_reset();

    // Initialize JTAG
    jtag_init();

    // Read IDCODE to verify JTAG connection
    uint32_t idcode = jtag_read_idcode();
    if (idcode == 0 || idcode == 0xFFFFFFFFu || !(idcode & 1u)) {
        return false;
    }

    // Read DTMCS to get DMI parameters
    uint32_t dtmcs = jtag_read_dtmcs();
    if (dtmcs == 0xFFFFFFFFu) {
        return false; // line stuck high
    }
    // This implementation speaks the JTAG DTM defined for Debug 0.13/1.0
    // only. Version 0 is the incompatible 0.11 transport, 15 is custom, and
    // the remaining values are reserved. The 64-bit scan buffer can hold at
    // most 30 address bits; six are required for the registers used here.
    uint32_t dtm_version = dtmcs & 0x0Fu;
    uint32_t dmi_abits = (dtmcs >> 4) & 0x3Fu;
    if (dtm_version != 1u || dmi_abits < 6u || dmi_abits > 30u) {
        return false;
    }
    // Clear any sticky DMI state left from a previous session
    jtag_dmi_reset();

    // Reset the Debug Module to a known state (dmactive 0 -> 1), clearing
    // stale haltreq/resumereq/abstractauto from a previous session.
    if (!jtag_dmi_write(DM_DMCONTROL, 0)) {
        return false;
    }
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE)) {
        return false;
    }
    // dmactive may take time to assert; poll it.
    {
        uint32_t start = hal_time_us();
        uint32_t dmc = 0;
        do {
            if (!jtag_dmi_read(DM_DMCONTROL, &dmc)) {
                return false;
            }
            if (dmc & DMCONTROL_DMACTIVE) {
                break;
            }
        } while ((hal_time_us() - start) < DM_TIMEOUT_US);
        if (!(dmc & DMCONTROL_DMACTIVE)) {
            return false;
        }
    }

    // Read DMSTATUS to verify DM is responding
    uint32_t dmstatus;
    if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
        return false;
    }

    // Only the standardized 0.13 and 1.0 Debug Module layouts are supported.
    // Do not interpret reserved/custom versions as a newer compatible DM.
    uint32_t version = dmstatus & DMSTATUS_VERSION_MASK;
    if (version != 2u && version != 3u) {
        return false;
    }

    // Check authentication
    if (!(dmstatus & DMSTATUS_AUTHENTICATED)) {
        return false;  // Need to authenticate first (not supported)
    }

    // Read ABSTRACTCS to get capabilities
    uint32_t acs;
    if (!jtag_dmi_read(DM_ABSTRACTCS, &acs)) {
        return false;
    }

    g_data_count = acs & ABSTRACTCS_DATACOUNT_MASK;
    g_progbuf_size = (acs & ABSTRACTCS_PROGBUFSIZE_MASK) >> ABSTRACTCS_PROGBUFSIZE_SHIFT;
    if (g_data_count < 1u) {
        return false; // RV32 Access Register requires data0.
    }
    g_has_abstract_mem = g_data_count >= 2u;

    // Check for standardized System Bus Access. Byte SBA avoids widening
    // MMIO accesses; 32-bit SBA is used only for naturally aligned words.
    uint32_t sbcs;
    if (jtag_dmi_read(DM_SBCS, &sbcs)) {
        uint32_t sbversion = (sbcs & SBCS_SBVERSION_MASK) >> SBCS_SBVERSION_SHIFT;
        uint32_t sbasize = (sbcs & SBCS_SBASIZE_MASK) >> SBCS_SBASIZE_SHIFT;
        bool valid_sba = sbversion == SBCS_SBVERSION_1_0 && sbasize >= 32u;
        g_has_sba8 = valid_sba && (sbcs & SBCS_SBACCESS8_SUPPORTED) != 0;
        g_has_sba32 = valid_sba && (sbcs & SBCS_SBACCESS32_SUPPORTED) != 0;
        g_has_sba = g_has_sba8 || g_has_sba32;
    }

    g_dm_active = true;
    if (owned_triggers != 0) {
        bool halted;
        if (!riscv_is_halted(&halted) || (!halted && !riscv_halt())) {
            return false;
        }
    }
    if (!riscv_clear_reconnected_triggers(owned_triggers)) {
        return false;
    }
    riscv_trigger_state_reset();
    if (g_step_cleanup_pending && !riscv_clear_step()) {
        return false;
    }
    return true;
}

bool riscv_halt(void)
{
    if (!g_dm_active) return false;

    // Request halt
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ)) {
        (void)jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
        return false;
    }

    // Wait for hart to halt. haltreq is level-sensitive, so every exit after
    // the request is issued must try to deassert it, including DMI failures.
    bool halted = false;
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        uint32_t dmstatus;
        if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
            break;
        }
        if (dmstatus & DMSTATUS_ALLHALTED) {
            halted = true;
            break;
        }
    }

    bool request_cleared = jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
    return halted && request_cleared;
}

static bool riscv_continue_raw(void)
{
    if (!g_dm_active) return false;

    // Request resume
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ)) {
        (void)jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
        return false;
    }

    // Wait for resume acknowledgment, then deassert resumereq on every exit.
    bool resumed = false;
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        uint32_t dmstatus;
        if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
            break;
        }
        if (dmstatus & DMSTATUS_ALLRESUMEACK) {
            resumed = true;
            break;
        }
    }

    bool request_cleared = jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
    return resumed && request_cleared;
}

bool riscv_continue(void)
{
    if (!g_dm_active) return false;

    // A prior cleanup failure means DCSR.STEP may still be live. Never issue
    // an ordinary resume until a read/modify/write/read cycle confirms that
    // the debugger-owned bit is clear.
    if (g_step_cleanup_pending && !riscv_clear_step()) {
        return false;
    }
    if (riscv_continue_raw()) {
        return true;
    }

    // A failed resume acknowledgment or resumereq cleanup does not prove the
    // hart stayed halted. Restore a known stop before reporting failure.
    (void)riscv_halt();
    return false;
}

static bool riscv_clear_step(void)
{
    bool halted;
    if (!riscv_is_halted(&halted) || !halted) {
        if (!riscv_halt()) {
            return false;
        }
    }

    // Read dcsr again after the step. Entry into Debug Mode updates prv/v;
    // writing the pre-step snapshot would undo a privilege transition such
    // as mret/sret. Only clear the debugger-owned step bit in the new value.
    uint32_t current_dcsr;
    if (!riscv_read_csr(CSR_DCSR, &current_dcsr)) {
        return false;
    }
    if (!(current_dcsr & DCSR_STEP)) {
        g_step_cleanup_pending = false;
        return true;
    }
    if (!riscv_write_csr(CSR_DCSR, current_dcsr & ~DCSR_STEP)) {
        return false;
    }
    if (!riscv_read_csr(CSR_DCSR, &current_dcsr) ||
        (current_dcsr & DCSR_STEP)) {
        return false;
    }
    g_step_cleanup_pending = false;
    return true;
}

bool riscv_step(void)
{
    if (!g_dm_active) return false;
    if (g_step_cleanup_pending && !riscv_clear_step()) return false;

    // Ensure halted first
    bool halted;
    if (!riscv_is_halted(&halted) || !halted) {
        if (!riscv_halt()) return false;
    }

    // Set step bit in dcsr (Debug Control and Status Register)
    // dcsr.step = 1 (bit 2)
    uint32_t dcsr;
    if (!riscv_read_csr(CSR_DCSR, &dcsr)) {
        return false;
    }

    dcsr |= DCSR_STEP;

    // Mark cleanup pending before the write: a failed transport response does
    // not prove the hart rejected the write.
    g_step_cleanup_pending = true;
    if (!riscv_write_csr(CSR_DCSR, dcsr)) {
        (void)riscv_clear_step();
        return false;
    }

    // Resume to execute one instruction
    if (!riscv_continue_raw()) {
        (void)riscv_clear_step();
        return false;
    }

    // Wait for halt (step complete)
    bool step_done = false;
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        if (riscv_is_halted(&halted) && halted) {
            step_done = true;
            break;
        }
    }

    // Never leave dcsr.step set. Cleanup failure is an operation failure even
    // if the single step itself completed, because a later continue would
    // otherwise unexpectedly execute only one instruction.
    bool cleaned = riscv_clear_step();
    return step_done && cleaned;
}

bool riscv_is_halted(bool *halted)
{
    if (!g_dm_active || !halted) return false;

    uint32_t dmstatus;
    if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
        return false;
    }

    *halted = (dmstatus & DMSTATUS_ALLHALTED) != 0;
    return true;
}

bool riscv_read_reg(uint32_t regnum, uint32_t *out)
{
    if (!g_dm_active) return false;

    uint16_t regno;
    if (regnum < 32) {
        // GPR x0-x31
        regno = REG_GPR_BASE + regnum;
    } else if (regnum == 32) {
        // PC -> dpc CSR
        regno = REG_DPC;
    } else {
        return false;
    }

    uint32_t cmd = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 | AC_AR_TRANSFER | AC_AR_REGNO(regno);
    return dm_exec_abstract(cmd, out);
}

bool riscv_write_reg(uint32_t regnum, uint32_t val)
{
    if (!g_dm_active) return false;

    uint16_t regno;
    if (regnum < 32) {
        regno = REG_GPR_BASE + regnum;
    } else if (regnum == 32) {
        regno = REG_DPC;
    } else {
        return false;
    }

    // Write data0 first
    if (!jtag_dmi_write(DM_DATA0, val)) return false;

    uint32_t cmd = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 | AC_AR_TRANSFER | AC_AR_WRITE | AC_AR_REGNO(regno);
    return dm_exec_abstract(cmd, NULL);
}

uint32_t riscv_gdb_reg_count(void)
{
    // RV32: 32 GPRs + PC = 33
    return 33;
}

bool riscv_read_gdb_regs(uint32_t *regs, uint32_t max_count)
{
    if (!g_dm_active) return false;

    uint32_t count = riscv_gdb_reg_count();
    if (max_count < count) count = max_count;

    for (uint32_t i = 0; i < count; i++) {
        if (!riscv_read_reg(i, &regs[i])) {
            return false;
        }
    }
    return true;
}

bool riscv_write_gdb_regs(const uint32_t *regs, uint32_t count)
{
    if (!g_dm_active) return false;

    uint32_t max = riscv_gdb_reg_count();
    if (count > max) count = max;

    // Skip x0 (always zero)
    for (uint32_t i = 1; i < count; i++) {
        if (!riscv_write_reg(i, regs[i])) {
            return false;
        }
    }
    return true;
}

// Wait for the system bus to go idle. On bus error, sticky-busy error or
// timeout, clear the write-1-to-clear bits and fail — never fall through
// and treat stale sbdata0 contents as valid data.
static bool sba_wait_idle(void)
{
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        uint32_t status;
        if (!jtag_dmi_read(DM_SBCS, &status)) {
            // Bus state is unknown; do not risk a later sbcs write while the
            // manager may still be busy.
            g_has_sba = false;
            return false;
        }
        if (status & SBCS_SBBUSY) {
            continue;
        }
        if (status & (SBCS_SBERROR_MASK | SBCS_SBBUSYERROR)) {
            // Clear only the W1C error bits (writing the config fields
            // back would re-program them).
            if (!jtag_dmi_write(DM_SBCS,
                                status & (SBCS_SBERROR_MASK | SBCS_SBBUSYERROR))) {
                g_has_sba = false;
            }
            return false;
        }
        return true;
    }
    // Writes to sbcs while sbbusy is set have undefined behavior and SBA has
    // no cancellation command. Poison SBA for this session; a later request
    // can use Abstract Memory Access instead.
    g_has_sba = false;
    return false;
}

static bool sba_configure(uint32_t value)
{
    if (!sba_wait_idle()) {
        return false;
    }
    if (!jtag_dmi_write(DM_SBCS, value)) {
        g_has_sba = false;
        return false;
    }
    return true;
}

static bool sba_dmi_write(uint32_t addr, uint32_t value)
{
    if (!jtag_dmi_write(addr, value)) {
        g_has_sba = false;
        return false;
    }
    return true;
}

static bool sba_dmi_read(uint32_t addr, uint32_t *value)
{
    if (!jtag_dmi_read(addr, value)) {
        g_has_sba = false;
        return false;
    }
    return true;
}

static bool sba_read_unit(uint32_t addr, uint32_t access, uint32_t *value)
{
    if (!sba_configure(access | SBCS_SBREADONADDR)) return false;
    if (!sba_dmi_write(DM_SBADDRESS0, addr)) return false;
    if (!sba_wait_idle()) return false;
    return sba_dmi_read(DM_SBDATA0, value);
}

static bool sba_write_unit(uint32_t addr, uint32_t access, uint32_t value)
{
    if (!sba_configure(access)) return false;
    if (!sba_dmi_write(DM_SBADDRESS0, addr)) return false;
    if (!sba_dmi_write(DM_SBDATA0, value)) return false;
    return sba_wait_idle();
}

static bool abstract_mem_read_byte(uint32_t addr, uint8_t *value)
{
    if (!jtag_dmi_write(DM_DATA1, addr)) return false;
    uint32_t data;
    uint32_t cmd = (AC_ACCESS_MEMORY << 24); // aamsize=0 (8-bit), read
    if (!dm_exec_abstract(cmd, &data)) return false;
    *value = (uint8_t)data;
    return true;
}

static bool abstract_mem_write_byte(uint32_t addr, uint8_t value)
{
    if (!jtag_dmi_write(DM_DATA1, addr)) return false;
    if (!jtag_dmi_write(DM_DATA0, value)) return false;
    uint32_t cmd = (AC_ACCESS_MEMORY << 24) | AC_AR_WRITE;
    return dm_exec_abstract(cmd, NULL);
}

static bool byte_memory_access_available(void)
{
    return (g_has_sba && g_has_sba8) || g_has_abstract_mem;
}

// Memory access via System Bus Access (if available) or Abstract Memory commands
bool riscv_mem_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!g_dm_active || (len > 0 && !buf) || !range_is_valid(addr, len)) {
        return false;
    }

    bool can_word = g_has_sba && g_has_sba32;
    bool can_byte = byte_memory_access_available();
    if (len > 0 && !can_word && !can_byte) return false;
    if (len > 0 && !can_byte && ((addr & 3u) != 0 || (len & 3u) != 0)) {
        return false;
    }

    while (len > 0) {
        if (can_word && (addr & 3u) == 0 && len >= 4u) {
            uint32_t word;
            if (!sba_read_unit(addr, SBCS_SBACCESS32, &word)) return false;
            buf[0] = (uint8_t)word;
            buf[1] = (uint8_t)(word >> 8);
            buf[2] = (uint8_t)(word >> 16);
            buf[3] = (uint8_t)(word >> 24);
            addr += 4u;
            buf += 4;
            len -= 4u;
        } else {
            if (g_has_sba && g_has_sba8) {
                uint32_t data;
                if (!sba_read_unit(addr, SBCS_SBACCESS8, &data)) return false;
                *buf = (uint8_t)data;
            } else if (!g_has_abstract_mem ||
                       !abstract_mem_read_byte(addr, buf)) {
                return false;
            }
            addr++;
            buf++;
            len--;
        }
    }
    return true;
}

bool riscv_mem_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (!g_dm_active || (len > 0 && !buf) || !range_is_valid(addr, len)) {
        return false;
    }

    bool can_word = g_has_sba && g_has_sba32;
    bool can_byte = byte_memory_access_available();
    if (len > 0 && !can_word && !can_byte) return false;
    if (len > 0 && !can_byte && ((addr & 3u) != 0 || (len & 3u) != 0)) {
        return false;
    }

    while (len > 0) {
        if (can_word && (addr & 3u) == 0 && len >= 4u) {
            uint32_t word = (uint32_t)buf[0] |
                            ((uint32_t)buf[1] << 8) |
                            ((uint32_t)buf[2] << 16) |
                            ((uint32_t)buf[3] << 24);
            if (!sba_write_unit(addr, SBCS_SBACCESS32, word)) return false;
            addr += 4u;
            buf += 4;
            len -= 4u;
        } else {
            if (g_has_sba && g_has_sba8) {
                if (!sba_write_unit(addr, SBCS_SBACCESS8, *buf)) return false;
            } else if (!g_has_abstract_mem ||
                       !abstract_mem_write_byte(addr, *buf)) {
                return false;
            }
            addr++;
            buf++;
            len--;
        }
    }
    return true;
}

uint8_t riscv_stop_reason(void)
{
    // Read dcsr to get cause
    uint32_t dcsr;
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 | AC_AR_TRANSFER | AC_AR_REGNO(0x7B0);
    if (!dm_exec_abstract(cmd, &dcsr)) {
        return 5;  // SIGTRAP default
    }

    uint32_t cause = (dcsr >> 6) & 0x7u;
    switch (cause) {
        case 1:  // ebreak
        case 2:  // trigger (breakpoint)
            return 5;  // SIGTRAP
        case 3:  // haltreq
            return 17;  // SIGSTOP
        case 4:  // step
            return 5;  // SIGTRAP
        default:
            return 5;  // SIGTRAP
    }
}

enum {
    RISCV_TRIGGER_BREAKPOINT = 1,
    RISCV_TRIGGER_WATCHPOINT = 2,
};

#define CSR_MISA          0x301u
#define MISA_EXT_H        (1u << 7)
#define MISA_EXT_S        (1u << 18)
#define MISA_EXT_U        (1u << 20)
#define MCONTROL_SIZE_MASK  (3u << 16)
#define MCONTROL6_SIZE_MASK (7u << 16)

static bool trigger_disable_selected(void)
{
    if (!riscv_write_csr(CSR_TDATA1, 0)) {
        return false;
    }

    uint32_t value;
    if (!riscv_read_csr(CSR_TDATA1, &value)) {
        return false;
    }

    uint32_t type = (value & TRIGGER_TYPE_MASK) >> TRIGGER_TYPE_SHIFT;
    if (type == 0u || type == TRIGGER_TYPE_DISABLED) {
        return true;
    }
    // A fixed-type WARL implementation may retain its type. With every
    // execute/load/store control clear it cannot match an address access.
    return (value & TRIGGER_OP_MASK) == 0;
}

static bool trigger_privilege_config(uint8_t hw_type, uint32_t *requested,
                                     uint32_t *required)
{
    uint32_t request = TRIGGER_M | TRIGGER_S | TRIGGER_U;
    uint32_t require = TRIGGER_M;
    uint32_t misa;
    bool has_h = false;

    if (riscv_read_csr(CSR_MISA, &misa)) {
        if (misa & MISA_EXT_S) require |= TRIGGER_S;
        if (misa & MISA_EXT_U) require |= TRIGGER_U;
        has_h = (misa & MISA_EXT_H) != 0;
    }

    // Always include the actual resume privilege in the required readback.
    // misa may legally read as zero even when privilege modes exist.
    uint32_t dcsr;
    if (!riscv_read_csr(CSR_DCSR, &dcsr)) return false;
    uint32_t prv = dcsr & DCSR_PRV_MASK;
    if ((dcsr & DCSR_V) || has_h) {
        // This implementation knows the virtualization enable fields for
        // mcontrol6 only. Do not claim that legacy mcontrol covers VS/VU.
        if (hw_type != TRIGGER_TYPE_MCONTROL6) return false;
        request |= MCONTROL6_VS | MCONTROL6_VU;
        if (has_h) {
            if (misa & MISA_EXT_S) require |= MCONTROL6_VS;
            if (misa & MISA_EXT_U) require |= MCONTROL6_VU;
        }
    }
    if (dcsr & DCSR_V) {
        if (prv == 1u) {
            require |= MCONTROL6_VS;
        } else if (prv == 0u) {
            require |= MCONTROL6_VU;
        } else {
            return false; // M-mode cannot have dcsr.v set.
        }
    } else if (prv == 0u) {
        require |= TRIGGER_U;
    } else if (prv == 1u) {
        require |= TRIGGER_S;
    } else if (prv != 3u) {
        return false; // Reserved privilege encoding.
    }

    *requested = request;
    *required = require;
    return true;
}

static bool trigger_config_readback_ok(uint8_t hw_type, uint32_t value,
                                       uint32_t op, uint32_t match,
                                       uint32_t required_priv)
{
    uint32_t type = (value & TRIGGER_TYPE_MASK) >> TRIGGER_TYPE_SHIFT;
    uint32_t select_mask = hw_type == TRIGGER_TYPE_MCONTROL6 ?
                           MCONTROL6_SELECT_DATA : TRIGGER_SELECT_DATA;
    uint32_t size_mask = hw_type == TRIGGER_TYPE_MCONTROL6 ?
                         MCONTROL6_SIZE_MASK : MCONTROL_SIZE_MASK;

    return type == hw_type &&
           (value & TRIGGER_DMODE) != 0 &&
           (value & TRIGGER_ACTION_MASK) == TRIGGER_ACTION_DEBUG &&
           (value & TRIGGER_CHAIN) == 0 &&
           ((value & TRIGGER_MATCH_MASK) >> TRIGGER_MATCH_SHIFT) == match &&
           (value & select_mask) == 0 &&
           (value & size_mask) == 0 &&
           (value & TRIGGER_OP_MASK) == op &&
           (value & required_priv) == required_priv;
}

static bool trigger_program(uint8_t index, uint8_t kind, target_watch_t watch,
                            uint32_t addr, uint32_t len, uint32_t match,
                            uint32_t compare, uint32_t op)
{
    riscv_trigger_t *slot = &g_triggers[index];
    // Defensive initialization also keeps older GCC dataflow analysis from
    // treating the out-parameters as potentially uninitialized.
    uint32_t requested_priv = 0u;
    uint32_t required_priv  = 0u;
    if (!trigger_privilege_config(slot->hw_type, &requested_priv,
                                  &required_priv)) {
        return false;
    }

    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) return false;

    bool selected = riscv_write_csr(CSR_TSELECT, index);
    bool success = selected && trigger_disable_selected();
    if (success) success = riscv_write_csr(CSR_TDATA2, compare);

    uint32_t compare_readback;
    if (success) {
        success = riscv_read_csr(CSR_TDATA2, &compare_readback) &&
                  compare_readback == compare;
    }

    uint32_t cfg = ((uint32_t)slot->hw_type << TRIGGER_TYPE_SHIFT) |
                   TRIGGER_DMODE | TRIGGER_ACTION_DEBUG | requested_priv |
                   (match << TRIGGER_MATCH_SHIFT) | op;

    // Track ownership before the enabling write. A failed DMI response can
    // leave completion uncertain; retaining the slot lets detach retry the
    // disable rather than forgetting a potentially live trigger.
    if (success) {
        slot->addr = addr;
        slot->len = len;
        slot->type = kind;
        slot->watch = (uint8_t)watch;
        slot->used = true;
        success = riscv_write_csr(CSR_TDATA1, cfg);
    }

    uint32_t cfg_readback;
    if (success) {
        success = riscv_read_csr(CSR_TDATA1, &cfg_readback) &&
                  trigger_config_readback_ok(slot->hw_type, cfg_readback, op,
                                             match, required_priv);
    }

    if (!success && slot->used && selected) {
        if (riscv_write_csr(CSR_TSELECT, index) &&
            trigger_disable_selected()) {
            slot->used = false;
        }
    }

    bool restored = riscv_write_csr(CSR_TSELECT, saved_tselect);
    if (!restored && success) {
        // Do not report a clean insertion while leaving shared tselect
        // corrupted. Best-effort rollback keeps an unsuccessful Z packet
        // from leaving an unrequested trigger active.
        if (riscv_write_csr(CSR_TSELECT, index) &&
            trigger_disable_selected()) {
            slot->used = false;
        }
        (void)riscv_write_csr(CSR_TSELECT, saved_tselect);
        success = false;
    }

    return success;
}

static bool trigger_remove_index(uint8_t index)
{
    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) return false;

    bool disabled = riscv_write_csr(CSR_TSELECT, index) &&
                    trigger_disable_selected();
    bool restored = riscv_write_csr(CSR_TSELECT, saved_tselect);
    if (disabled) {
        // Clear only after the hardware write and verification succeeded.
        g_triggers[index].used = false;
    }
    return disabled && restored;
}

// Breakpoint support via trigger module
bool riscv_breakpoint_insert(uint32_t addr)
{
    if (!g_dm_active || !riscv_triggers_init()) return false;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used &&
            g_triggers[i].type == RISCV_TRIGGER_BREAKPOINT &&
            g_triggers[i].addr == addr) {
            return true;
        }
    }

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (!g_triggers[i].used && g_triggers[i].hw_type != 0) {
            if (trigger_program(i, RISCV_TRIGGER_BREAKPOINT,
                                TARGET_WATCH_ACCESS, addr, 1u,
                                TRIGGER_MATCH_EQUAL, addr,
                                TRIGGER_EXECUTE)) {
                return true;
            }
        }
    }
    return false;
}

bool riscv_breakpoint_remove(uint32_t addr)
{
    if (!g_dm_active) return true;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used &&
            g_triggers[i].type == RISCV_TRIGGER_BREAKPOINT &&
            g_triggers[i].addr == addr) {
            return trigger_remove_index(i);
        }
    }
    return true;
}

// Watchpoint support via trigger module
bool riscv_watchpoints_supported(void)
{
    return g_dm_active && riscv_triggers_init() &&
           g_num_usable_triggers > 0;
}

static bool watch_range_encode(uint32_t addr, uint32_t len, uint32_t *match,
                               uint32_t *compare)
{
    if (len == 0 || !range_is_valid(addr, len)) return false;
    if (len == 1u) {
        *match = TRIGGER_MATCH_EQUAL;
        *compare = addr;
        return true;
    }

    // Sdtrig NAPOT represents one naturally aligned power-of-two region.
    // Reject arbitrary GDB ranges instead of silently watching only addr.
    if ((len & (len - 1u)) != 0 || (addr & (len - 1u)) != 0) {
        return false;
    }
    *match = TRIGGER_MATCH_NAPOT;
    *compare = addr | ((len >> 1) - 1u);
    return true;
}

bool riscv_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    if (!g_dm_active || (unsigned)type > (unsigned)TARGET_WATCH_ACCESS ||
        !riscv_triggers_init()) {
        return false;
    }

    uint32_t match;
    uint32_t compare;
    if (!watch_range_encode(addr, len, &match, &compare)) return false;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used &&
            g_triggers[i].type == RISCV_TRIGGER_WATCHPOINT &&
            g_triggers[i].addr == addr && g_triggers[i].len == len &&
            g_triggers[i].watch == (uint8_t)type) {
            return true;
        }
    }

    uint32_t op;
    if (type == TARGET_WATCH_WRITE) {
        op = TRIGGER_STORE;
    } else if (type == TARGET_WATCH_READ) {
        op = TRIGGER_LOAD;
    } else {
        op = TRIGGER_LOAD | TRIGGER_STORE;
    }

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (!g_triggers[i].used && g_triggers[i].hw_type != 0) {
            if (trigger_program(i, RISCV_TRIGGER_WATCHPOINT, type,
                                addr, len, match, compare, op)) {
                return true;
            }
        }
    }
    return false;
}

bool riscv_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    if (!g_dm_active) return true;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used &&
            g_triggers[i].type == RISCV_TRIGGER_WATCHPOINT &&
            g_triggers[i].addr == addr && g_triggers[i].len == len &&
            g_triggers[i].watch == (uint8_t)type) {
            return trigger_remove_index(i);
        }
    }
    return true;
}

bool riscv_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    if (!g_dm_active) return false;

    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) return false;
    bool found = false;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (!g_triggers[i].used ||
            g_triggers[i].type != RISCV_TRIGGER_WATCHPOINT) {
            continue;
        }
        if (!riscv_write_csr(CSR_TSELECT, i)) continue;

        uint32_t tdata1;
        uint32_t hit_mask = g_triggers[i].hw_type == TRIGGER_TYPE_MCONTROL6 ?
                            (MCONTROL6_HIT0 | MCONTROL6_HIT1) : MCONTROL_HIT;
        if (riscv_read_csr(CSR_TDATA1, &tdata1) && (tdata1 & hit_mask)) {
            if (out_type) *out_type = (target_watch_t)g_triggers[i].watch;
            if (out_addr) *out_addr = g_triggers[i].addr;
            (void)riscv_write_csr(CSR_TDATA1, tdata1 & ~hit_mask);
            found = true;
            break;
        }
    }

    if (!riscv_write_csr(CSR_TSELECT, saved_tselect)) return false;
    return found;
}

bool riscv_debug_resources_clear(void)
{
    if (!g_dm_active) return riscv_owned_trigger_mask() == 0;

    uint32_t saved_tselect;
    if (!riscv_read_csr(CSR_TSELECT, &saved_tselect)) return false;

    bool success = true;
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        if (!g_triggers[i].used) continue;
        if (riscv_write_csr(CSR_TSELECT, i) && trigger_disable_selected()) {
            g_triggers[i].used = false;
        } else {
            success = false;
        }
    }

    if (!riscv_write_csr(CSR_TSELECT, saved_tselect)) success = false;
    return success;
}

#endif // PROBE_ENABLE_RISCV
