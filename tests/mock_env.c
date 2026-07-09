#include "mock_env.h"

#include <string.h>

#include "flash_mspm0.h"
#include "hal.h"
#include "probe.h"
#include "target.h"

#ifndef TEST_LEGACY
#define TEST_LEGACY 0
#endif

char   mock_tx[16384];
size_t mock_tx_len;

mock_arch_t mock_arch;
bool        mock_halted;
bool        mock_fail_halt;
bool        mock_fail_continue;
bool        mock_continue_failure_runs;
bool        mock_fail_halt_status;
uint32_t    mock_regs[42];
uint8_t     mock_mem[MOCK_MEM_SIZE];
int         mock_halt_calls, mock_continue_calls, mock_step_calls;
int         mock_disconnect_calls, mock_debug_clear_calls;

uint32_t mock_bp[8];
int      mock_bp_count;

bool     mock_watch_hit;
uint32_t mock_watch_addr;

mock_arch_t mock_attach_result;
int         mock_attach_calls;

bool     mock_flash_geometry_ok;
uint32_t mock_flash_size, mock_flash_sector;
int      mock_flash_erase_calls;
uint32_t mock_flash_erase_addr, mock_flash_erase_len;
uint8_t  mock_flash[MOCK_FLASH_SIZE];
uint32_t mock_flash_written;
int      mock_flash_done_calls;
int      mock_flash_abort_calls;

void mock_reset(void)
{
    mock_tx_len = 0;
    mock_arch   = MOCK_ARCH_CM;
    mock_halted = false;
    mock_fail_halt = false;
    mock_fail_continue = false;
    mock_continue_failure_runs = false;
    mock_fail_halt_status = false;
    for (int i = 0; i < 42; i++) {
        mock_regs[i] = 0x11110000u + (uint32_t) i;
    }
    memset(mock_mem, 0, sizeof(mock_mem));
    mock_halt_calls = mock_continue_calls = mock_step_calls = 0;
    mock_disconnect_calls = mock_debug_clear_calls = 0;
    mock_bp_count = 0;
    mock_watch_hit  = false;
    mock_watch_addr = 0;
    mock_attach_result = MOCK_ARCH_CM;
    mock_attach_calls  = 0;
    mock_flash_geometry_ok = true;
    mock_flash_size   = 0x8000u; // 32 KB
    mock_flash_sector = 0x400u;  // 1 KB
    mock_flash_erase_calls = 0;
    mock_flash_erase_addr = mock_flash_erase_len = 0;
    memset(mock_flash, 0xFF, sizeof(mock_flash));
    mock_flash_written = 0;
    mock_flash_done_calls = 0;
    mock_flash_abort_calls = 0;
}

// ---------------- HAL ----------------

void uart_putc(uint8_t c)
{
    if (mock_tx_len < sizeof(mock_tx)) {
        mock_tx[mock_tx_len++] = (char) c;
    }
}

int uart_getc(void) { return -1; }

void delay_us(uint32_t us) { (void) us; }

uint32_t hal_time_us(void)
{
    static uint32_t t;
    t += 100;
    return t;
}

// ---------------- probe ----------------

bool probe_attach(void)
{
    mock_attach_calls++;
    mock_arch = mock_attach_result;
    return mock_arch != MOCK_ARCH_NONE;
}

// ---------------- target ----------------

bool target_attached(void) { return mock_arch != MOCK_ARCH_NONE; }

void target_disconnect(void)
{
    mock_disconnect_calls++;
    mock_arch = MOCK_ARCH_NONE;
}

static uint32_t arch_regs(void)
{
    switch (mock_arch) {
    case MOCK_ARCH_CM:
#if defined(TEST_LEGACY) && TEST_LEGACY
        return 42u;
#else
        return 17u;
#endif
    case MOCK_ARCH_RV: return 33u;
    default:           return 0u;
    }
}

bool target_halt(void)
{
    mock_halt_calls++;
    if (mock_fail_halt || mock_arch == MOCK_ARCH_NONE) {
        return false;
    }
    mock_halted = true;
    return true;
}

bool target_continue(void)
{
    mock_continue_calls++;
    if (mock_arch == MOCK_ARCH_NONE) return false;
    if (mock_fail_continue) {
        mock_halted = !mock_continue_failure_runs;
        return false;
    }
    mock_halted = false;
    return true;
}

bool target_step(void)
{
    mock_step_calls++;
    if (mock_arch == MOCK_ARCH_NONE) return false;
    mock_halted = true;
    return true;
}

bool target_is_halted(bool *halted)
{
    if (mock_arch == MOCK_ARCH_NONE || mock_fail_halt_status) return false;
    *halted = mock_halted;
    return true;
}

uint32_t target_gdb_reg_count(void) { return arch_regs(); }

bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count)
{
    uint32_t n = arch_regs();
    if (n == 0 || max_count < n) return false;
#if defined(TEST_LEGACY) && TEST_LEGACY
    if (mock_arch == MOCK_ARCH_CM) {
        memset(regs, 0, n * sizeof(*regs));
        memcpy(regs, mock_regs, 16u * sizeof(*regs));
        regs[41] = mock_regs[16];
        return true;
    }
#endif
    memcpy(regs, mock_regs, n * 4u);
    return true;
}

bool target_write_gdb_regs(const uint32_t *regs, uint32_t count)
{
    uint32_t n = arch_regs();
    if (n == 0 || count < n) return false;
#if defined(TEST_LEGACY) && TEST_LEGACY
    if (mock_arch == MOCK_ARCH_CM) {
        memcpy(mock_regs, regs, 16u * sizeof(*regs));
        mock_regs[16] = regs[41];
        return true;
    }
#endif
    memcpy(mock_regs, regs, n * 4u);
    return true;
}

bool target_read_reg(uint32_t regnum, uint32_t *out)
{
    uint32_t core_count = (mock_arch == MOCK_ARCH_CM) ? 17u : 33u;
    if (mock_arch == MOCK_ARCH_NONE || regnum >= core_count) return false;
    *out = mock_regs[regnum];
    return true;
}

bool target_write_reg(uint32_t regnum, uint32_t val)
{
    uint32_t core_count = (mock_arch == MOCK_ARCH_CM) ? 17u : 33u;
    if (mock_arch == MOCK_ARCH_NONE || regnum >= core_count) return false;
    mock_regs[regnum] = val;
    return true;
}

uint32_t target_pc_regnum(void)
{
    return (mock_arch == MOCK_ARCH_RV) ? 32u : 15u;
}

bool target_map_gdb_regnum(uint32_t gdb_regno, uint32_t *core_regno)
{
    switch (mock_arch) {
    case MOCK_ARCH_CM:
        if (gdb_regno <= 15u) { *core_regno = gdb_regno; return true; }
        if (((!TEST_LEGACY) && gdb_regno == 16u) || gdb_regno == 25u) {
            *core_regno = 16u;
            return true;
        }
        return false;
    case MOCK_ARCH_RV:
        if (gdb_regno <= 32u) { *core_regno = gdb_regno; return true; }
        return false;
    default:
        return false;
    }
}

void target_breakpoints_init(void) {}

bool target_debug_resources_clear(void)
{
    mock_debug_clear_calls++;
    mock_bp_count = 0;
    return true;
}

bool target_breakpoint_insert(uint32_t addr)
{
    if (mock_bp_count >= 8) return false;
    mock_bp[mock_bp_count++] = addr;
    return true;
}

bool target_breakpoint_remove(uint32_t addr)
{
    for (int i = 0; i < mock_bp_count; i++) {
        if (mock_bp[i] == addr) {
            mock_bp[i] = mock_bp[--mock_bp_count];
            return true;
        }
    }
    return true;
}

bool target_watchpoints_supported(void) { return true; }

bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    (void) type; (void) addr; (void) len;
    return true;
}

bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    (void) type; (void) addr; (void) len;
    return true;
}

bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    if (!mock_watch_hit) return false;
    if (out_type) *out_type = TARGET_WATCH_WRITE;
    if (out_addr) *out_addr = mock_watch_addr;
    return true;
}

static bool mem_range_ok(uint32_t addr, uint32_t len)
{
    return addr >= MOCK_MEM_BASE && (addr - MOCK_MEM_BASE) + len <= MOCK_MEM_SIZE &&
           (addr + len) >= addr;
}

bool target_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (mock_arch == MOCK_ARCH_NONE || !mem_range_ok(addr, len)) return false;
    memcpy(buf, &mock_mem[addr - MOCK_MEM_BASE], len);
    return true;
}

bool target_mem_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (mock_arch == MOCK_ARCH_NONE || !mem_range_ok(addr, len)) return false;
    memcpy(&mock_mem[addr - MOCK_MEM_BASE], buf, len);
    return true;
}

bool target_xml_get(const char **out_xml, uint32_t *out_len)
{
#if defined(PROBE_ENABLE_RISCV_MINIMAL_XML) && PROBE_ENABLE_RISCV_MINIMAL_XML && \
    (!defined(PROBE_ENABLE_QXFER_TARGET_XML) || !PROBE_ENABLE_QXFER_TARGET_XML)
    static const char xml[] =
        "<target>"
        "<architecture>riscv:rv32</architecture></target>";
    if (mock_arch != MOCK_ARCH_RV) return false;
#else
    static const char xml[] =
        "<?xml version=\"1.0\"?><target version=\"1.0\">"
        "<architecture>armv6-m</architecture></target>";
    if (mock_arch == MOCK_ARCH_NONE) return false;
#endif
    *out_xml = xml;
    *out_len = (uint32_t) (sizeof(xml) - 1u);
    return true;
}

// ---------------- flash driver ----------------

bool flash_mspm0_geometry(uint32_t *main_size_bytes, uint32_t *sector_size_bytes)
{
    if (!mock_flash_geometry_ok) return false;
    if (main_size_bytes) *main_size_bytes = mock_flash_size;
    if (sector_size_bytes) *sector_size_bytes = mock_flash_sector;
    return true;
}

bool flash_mspm0_erase_range(uint32_t addr, uint32_t len)
{
    mock_flash_erase_calls++;
    mock_flash_erase_addr = addr;
    mock_flash_erase_len  = len;
    return true;
}

bool flash_mspm0_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (addr + len > MOCK_FLASH_SIZE) return false;
    memcpy(&mock_flash[addr], data, len);
    mock_flash_written += len;
    return true;
}

bool flash_mspm0_done(void)
{
    mock_flash_done_calls++;
    return true;
}

void flash_mspm0_abort(void)
{
    mock_flash_abort_calls++;
}
