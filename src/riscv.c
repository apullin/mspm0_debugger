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
#define SBCS_SBACCESS32     (2u << 17)
#define SBCS_SBREADONADDR   (1u << 20)
#define SBCS_SBREADONDATA   (1u << 15)
#define SBCS_SBAUTOINCREMENT (1u << 16)
#define SBCS_SBBUSY         (1u << 21)
#define SBCS_SBERROR_MASK   (7u << 12)

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

// mcontrol (tdata1 type=2) bit fields for RV32
#define MCONTROL_TYPE_MCONTROL  (2u << 28)
#define MCONTROL_DMODE          (1u << 27)
#define MCONTROL_HIT            (1u << 20)
#define MCONTROL_ACTION_DEBUG   (1u << 12)
#define MCONTROL_M              (1u << 6)
#define MCONTROL_U              (1u << 3)
#define MCONTROL_EXECUTE        (1u << 2)
#define MCONTROL_STORE          (1u << 1)
#define MCONTROL_LOAD           (1u << 0)

// Timeout for operations (microseconds)
#define DM_TIMEOUT_US       100000u

// Maximum hardware triggers to probe
#define RISCV_MAX_TRIGGERS  4u

// Trigger slot tracking
typedef struct {
    uint32_t addr;
    uint8_t  type;   // 0=unused, 1=breakpoint, 2=watchpoint
    uint8_t  watch;  // TARGET_WATCH_WRITE/READ/ACCESS
    bool     used;
} riscv_trigger_t;

static riscv_trigger_t g_triggers[RISCV_MAX_TRIGGERS];
static uint8_t g_num_triggers = 0;
static bool g_triggers_probed = false;

static bool g_dm_active = false;
static uint8_t g_progbuf_size = 0;
static uint8_t g_data_count = 0;
static bool g_has_sba = false;  // System Bus Access

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

// Probe available triggers
static bool riscv_triggers_init(void)
{
    if (g_triggers_probed) return g_num_triggers > 0;
    g_triggers_probed = true;
    g_num_triggers = 0;

    // Clear all slots
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        g_triggers[i].used = false;
    }

    // Probe triggers by writing to tselect
    for (uint8_t i = 0; i < RISCV_MAX_TRIGGERS; i++) {
        if (!riscv_write_csr(CSR_TSELECT, i)) break;

        uint32_t sel;
        if (!riscv_read_csr(CSR_TSELECT, &sel)) break;
        if (sel != i) break;  // Not as many triggers

        uint32_t tdata1;
        if (!riscv_read_csr(CSR_TDATA1, &tdata1)) break;
        uint32_t type = (tdata1 >> 28) & 0xFu;
        if (type == 0) break;  // No trigger at this index

        g_num_triggers = i + 1;
    }

    return g_num_triggers > 0;
}

bool riscv_init(void)
{
    // Initialize JTAG
    jtag_init();

    // Read IDCODE to verify JTAG connection
    uint32_t idcode = jtag_read_idcode();
    if (idcode == 0 || idcode == 0xFFFFFFFFu) {
        return false;
    }

    // Read DTMCS to get DMI parameters
    uint32_t dtmcs = jtag_read_dtmcs();
    if ((dtmcs & 0x0Fu) == 0) {
        // Version 0 = no debug module
        return false;
    }

    // Activate the Debug Module
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE)) {
        return false;
    }

    // Read DMSTATUS to verify DM is responding
    uint32_t dmstatus;
    if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
        return false;
    }

    // Check version (should be 2 for 0.13, 3 for 1.0)
    uint32_t version = dmstatus & DMSTATUS_VERSION_MASK;
    if (version < 2) {
        return false;  // Too old or not present
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

    // Check for System Bus Access
    uint32_t sbcs;
    if (jtag_dmi_read(DM_SBCS, &sbcs)) {
        g_has_sba = (sbcs != 0);
    }

    g_dm_active = true;
    return true;
}

bool riscv_halt(void)
{
    if (!g_dm_active) return false;

    // Request halt
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ)) {
        return false;
    }

    // Wait for hart to halt
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        uint32_t dmstatus;
        if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
            return false;
        }
        if (dmstatus & DMSTATUS_ALLHALTED) {
            // Clear haltreq
            jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
            return true;
        }
    }

    // Timeout - clear haltreq anyway
    jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
    return false;
}

bool riscv_continue(void)
{
    if (!g_dm_active) return false;

    // Request resume
    if (!jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ)) {
        return false;
    }

    // Wait for resume acknowledgment
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        uint32_t dmstatus;
        if (!jtag_dmi_read(DM_DMSTATUS, &dmstatus)) {
            return false;
        }
        if (dmstatus & DMSTATUS_ALLRESUMEACK) {
            // Clear resumereq
            jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
            return true;
        }
    }

    jtag_dmi_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
    return false;
}

bool riscv_step(void)
{
    if (!g_dm_active) return false;

    // Ensure halted first
    bool halted;
    if (!riscv_is_halted(&halted) || !halted) {
        if (!riscv_halt()) return false;
    }

    // Set step bit in dcsr (Debug Control and Status Register)
    // dcsr.step = 1 (bit 2)
    uint32_t dcsr;
    uint32_t cmd_read = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 | AC_AR_TRANSFER | AC_AR_REGNO(0x7B0);
    if (!dm_exec_abstract(cmd_read, &dcsr)) {
        return false;
    }

    dcsr |= (1u << 2);  // Set step bit

    // Write dcsr with step bit set
    if (!jtag_dmi_write(DM_DATA0, dcsr)) return false;
    uint32_t cmd_write = (AC_ACCESS_REGISTER << 24) | AC_AR_AARSIZE_32 | AC_AR_TRANSFER | AC_AR_WRITE | AC_AR_REGNO(0x7B0);
    if (!dm_exec_abstract(cmd_write, NULL)) {
        return false;
    }

    // Resume to execute one instruction
    if (!riscv_continue()) return false;

    // Wait for halt (step complete)
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < DM_TIMEOUT_US) {
        if (riscv_is_halted(&halted) && halted) {
            // Clear step bit in dcsr
            dcsr &= ~(1u << 2);
            if (!jtag_dmi_write(DM_DATA0, dcsr)) return false;
            dm_exec_abstract(cmd_write, NULL);
            return true;
        }
    }

    return false;
}

bool riscv_is_halted(bool *halted)
{
    if (!g_dm_active) return false;

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

// Memory access via System Bus Access (if available) or Abstract Memory commands
bool riscv_mem_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!g_dm_active) return false;

    if (g_has_sba) {
        // Use System Bus Access for memory reads
        // Configure SBCS for 32-bit access
        uint32_t sbcs = SBCS_SBACCESS32 | SBCS_SBREADONADDR | SBCS_SBAUTOINCREMENT;
        if (!jtag_dmi_write(DM_SBCS, sbcs)) return false;

        uint32_t aligned = addr & ~3u;
        uint32_t offset = addr & 3u;

        while (len > 0) {
            // Write address to trigger read
            if (!jtag_dmi_write(DM_SBADDRESS0, aligned)) return false;

            // Wait for read to complete
            uint32_t start = hal_time_us();
            while ((hal_time_us() - start) < DM_TIMEOUT_US) {
                uint32_t status;
                if (!jtag_dmi_read(DM_SBCS, &status)) return false;
                if (!(status & SBCS_SBBUSY)) {
                    if (status & SBCS_SBERROR_MASK) {
                        // Clear error and fail
                        jtag_dmi_write(DM_SBCS, status);
                        return false;
                    }
                    break;
                }
            }

            // Read data
            uint32_t word;
            if (!jtag_dmi_read(DM_SBDATA0, &word)) return false;

            // Extract bytes
            while (offset < 4 && len > 0) {
                *buf++ = (uint8_t)(word >> (offset * 8));
                offset++;
                len--;
            }

            aligned += 4;
            offset = 0;
        }
        return true;
    }

    // Fallback: use Abstract Memory Access command (if supported)
    // This is simpler but may not be available on all targets
    for (uint32_t i = 0; i < len; i++) {
        // Access Memory command: read byte
        uint32_t cmd = (AC_ACCESS_MEMORY << 24) | (0 << 20) | (addr + i);  // aamsize=0 (8-bit)
        uint32_t data;
        if (!dm_exec_abstract(cmd, &data)) {
            return false;
        }
        buf[i] = (uint8_t)data;
    }
    return true;
}

bool riscv_mem_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (!g_dm_active) return false;

    if (g_has_sba) {
        // Use System Bus Access for memory writes
        uint32_t sbcs = SBCS_SBACCESS32 | SBCS_SBAUTOINCREMENT;
        if (!jtag_dmi_write(DM_SBCS, sbcs)) return false;

        // Set starting address
        if (!jtag_dmi_write(DM_SBADDRESS0, addr & ~3u)) return false;

        uint32_t aligned = addr & ~3u;
        uint32_t offset = addr & 3u;

        while (len > 0) {
            uint32_t word = 0;

            // For partial words, we need read-modify-write
            if (offset != 0 || len < 4) {
                // Read current word
                if (!jtag_dmi_write(DM_SBADDRESS0, aligned)) return false;
                // Trigger read
                sbcs = SBCS_SBACCESS32 | SBCS_SBREADONADDR;
                if (!jtag_dmi_write(DM_SBCS, sbcs)) return false;
                if (!jtag_dmi_read(DM_SBDATA0, &word)) return false;
            }

            // Modify bytes
            while (offset < 4 && len > 0) {
                word &= ~(0xFFu << (offset * 8));
                word |= ((uint32_t)*buf++) << (offset * 8);
                offset++;
                len--;
            }

            // Write word
            if (!jtag_dmi_write(DM_SBADDRESS0, aligned)) return false;
            if (!jtag_dmi_write(DM_SBDATA0, word)) return false;

            // Wait for write to complete
            uint32_t start = hal_time_us();
            while ((hal_time_us() - start) < DM_TIMEOUT_US) {
                uint32_t status;
                if (!jtag_dmi_read(DM_SBCS, &status)) return false;
                if (!(status & SBCS_SBBUSY)) {
                    if (status & SBCS_SBERROR_MASK) {
                        jtag_dmi_write(DM_SBCS, status);
                        return false;
                    }
                    break;
                }
            }

            aligned += 4;
            offset = 0;
        }
        return true;
    }

    // Fallback: Abstract Memory Access (byte-by-byte)
    for (uint32_t i = 0; i < len; i++) {
        if (!jtag_dmi_write(DM_DATA0, buf[i])) return false;
        uint32_t cmd = (AC_ACCESS_MEMORY << 24) | (1u << 16) | (0 << 20) | (addr + i);  // write, 8-bit
        if (!dm_exec_abstract(cmd, NULL)) {
            return false;
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

// Breakpoint support via trigger module
bool riscv_breakpoint_insert(uint32_t addr)
{
    if (!g_dm_active) return false;
    if (!riscv_triggers_init()) return false;

    // Check if already installed
    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used && g_triggers[i].type == 1 &&
            g_triggers[i].addr == addr) {
            return true;  // Already set
        }
    }

    // Find free slot
    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (!g_triggers[i].used) {
            // Configure trigger: select, disable, set addr, enable
            if (!riscv_write_csr(CSR_TSELECT, i)) return false;
            if (!riscv_write_csr(CSR_TDATA1, 0)) return false;  // Disable first
            if (!riscv_write_csr(CSR_TDATA2, addr)) return false;

            uint32_t cfg = MCONTROL_TYPE_MCONTROL | MCONTROL_DMODE |
                           MCONTROL_ACTION_DEBUG | MCONTROL_M |
                           MCONTROL_U | MCONTROL_EXECUTE;
            if (!riscv_write_csr(CSR_TDATA1, cfg)) return false;

            g_triggers[i].addr = addr;
            g_triggers[i].type = 1;  // breakpoint
            g_triggers[i].used = true;
            return true;
        }
    }
    return false;  // No free slots
}

bool riscv_breakpoint_remove(uint32_t addr)
{
    if (!g_dm_active) return true;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used && g_triggers[i].type == 1 &&
            g_triggers[i].addr == addr) {
            riscv_write_csr(CSR_TSELECT, i);
            riscv_write_csr(CSR_TDATA1, 0);  // Disable
            g_triggers[i].used = false;
            return true;
        }
    }
    return true;  // Safe to remove non-existent
}

// Watchpoint support via trigger module
bool riscv_watchpoints_supported(void)
{
    if (!g_dm_active) return false;
    return riscv_triggers_init() && g_num_triggers > 0;
}

bool riscv_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    (void)len;  // RISC-V triggers don't support length directly
    if (!g_dm_active) return false;
    if (!riscv_triggers_init()) return false;

    // Find free slot
    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (!g_triggers[i].used) {
            if (!riscv_write_csr(CSR_TSELECT, i)) return false;
            if (!riscv_write_csr(CSR_TDATA1, 0)) return false;
            if (!riscv_write_csr(CSR_TDATA2, addr)) return false;

            uint32_t cfg = MCONTROL_TYPE_MCONTROL | MCONTROL_DMODE |
                           MCONTROL_ACTION_DEBUG | MCONTROL_M | MCONTROL_U;

            if (type == TARGET_WATCH_WRITE) {
                cfg |= MCONTROL_STORE;
            } else if (type == TARGET_WATCH_READ) {
                cfg |= MCONTROL_LOAD;
            } else {  // ACCESS
                cfg |= MCONTROL_LOAD | MCONTROL_STORE;
            }

            if (!riscv_write_csr(CSR_TDATA1, cfg)) return false;

            g_triggers[i].addr = addr;
            g_triggers[i].type = 2;  // watchpoint
            g_triggers[i].watch = (uint8_t)type;
            g_triggers[i].used = true;
            return true;
        }
    }
    return false;  // No free slots
}

bool riscv_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    (void)len;
    if (!g_dm_active) return true;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used && g_triggers[i].type == 2 &&
            g_triggers[i].addr == addr && g_triggers[i].watch == (uint8_t)type) {
            riscv_write_csr(CSR_TSELECT, i);
            riscv_write_csr(CSR_TDATA1, 0);
            g_triggers[i].used = false;
            return true;
        }
    }
    return true;  // Safe to remove non-existent
}

bool riscv_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    if (!g_dm_active) return false;

    for (uint8_t i = 0; i < g_num_triggers; i++) {
        if (g_triggers[i].used && g_triggers[i].type == 2) {
            if (!riscv_write_csr(CSR_TSELECT, i)) continue;

            uint32_t tdata1;
            if (riscv_read_csr(CSR_TDATA1, &tdata1) && (tdata1 & MCONTROL_HIT)) {
                if (out_type) *out_type = (target_watch_t)g_triggers[i].watch;
                if (out_addr) *out_addr = g_triggers[i].addr;
                // Clear hit bit by writing back without it
                riscv_write_csr(CSR_TDATA1, tdata1 & ~MCONTROL_HIT);
                return true;
            }
        }
    }
    return false;
}

#endif // PROBE_ENABLE_RISCV
