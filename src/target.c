// Target abstraction implementation - currently wraps Cortex-M.
// Future: add compile-time or runtime selection for RISC-V.

#include "target.h"
#include "cortex.h"

// For now, we only support Cortex-M targets via SWD.
// RISC-V support would be selected at compile time or runtime.

void target_init(void)
{
    cortex_target_init();
}

bool target_halt(void)
{
    return cortex_halt();
}

bool target_continue(void)
{
    return cortex_continue();
}

bool target_step(void)
{
    return cortex_step();
}

bool target_is_halted(bool *halted)
{
    return cortex_is_halted(halted);
}

bool target_read_reg(uint32_t regnum, uint32_t *out)
{
    return cortex_read_core_reg(regnum, out);
}

bool target_write_reg(uint32_t regnum, uint32_t val)
{
    return cortex_write_core_reg(regnum, val);
}

uint32_t target_gdb_reg_count(void)
{
    // Cortex-M: r0-r15 (16) + xPSR (1) = 17 registers
    return 17u;
}

bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count)
{
    if (max_count < 17u) {
        return false;
    }
    return cortex_read_gdb_regs(regs);
}

bool target_write_gdb_regs(const uint32_t *regs, uint32_t count)
{
    if (count < 17u) {
        return false;
    }
    return cortex_write_gdb_regs(regs);
}

void target_breakpoints_init(void)
{
    cortex_breakpoints_init();
}

bool target_breakpoint_insert(uint32_t addr)
{
    return cortex_breakpoint_insert(addr);
}

bool target_breakpoint_remove(uint32_t addr)
{
    return cortex_breakpoint_remove(addr);
}

bool target_watchpoints_supported(void)
{
    return cortex_watchpoints_supported();
}

bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    // Map target_watch_t to cortexm_watch_t (same values for now)
    return cortex_watchpoint_insert((cortexm_watch_t)type, addr, len);
}

bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    return cortex_watchpoint_remove((cortexm_watch_t)type, addr, len);
}

bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    cortexm_watch_t wt;
    if (!cortex_watchpoint_hit(&wt, out_addr)) {
        return false;
    }
    if (out_type) {
        *out_type = (target_watch_t)wt;
    }
    return true;
}

bool target_xml_get(const char **out_xml, uint32_t *out_len)
{
    return cortex_target_xml_get(out_xml, out_len);
}
