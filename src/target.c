// Target abstraction implementation.
// Compile-time selection between Cortex-M (SWD) and RISC-V (JTAG).

#include "target.h"

#if defined(PROBE_ENABLE_RISCV) && (PROBE_ENABLE_RISCV)
#include "riscv.h"
#define TARGET_IS_RISCV 1
#else
#include "cortex.h"
#define TARGET_IS_RISCV 0
#endif

void target_init(void)
{
#if TARGET_IS_RISCV
    riscv_init();
#else
    cortex_target_init();
#endif
}

bool target_halt(void)
{
#if TARGET_IS_RISCV
    return riscv_halt();
#else
    return cortex_halt();
#endif
}

bool target_continue(void)
{
#if TARGET_IS_RISCV
    return riscv_continue();
#else
    return cortex_continue();
#endif
}

bool target_step(void)
{
#if TARGET_IS_RISCV
    return riscv_step();
#else
    return cortex_step();
#endif
}

bool target_is_halted(bool *halted)
{
#if TARGET_IS_RISCV
    return riscv_is_halted(halted);
#else
    return cortex_is_halted(halted);
#endif
}

bool target_read_reg(uint32_t regnum, uint32_t *out)
{
#if TARGET_IS_RISCV
    return riscv_read_reg(regnum, out);
#else
    return cortex_read_core_reg(regnum, out);
#endif
}

bool target_write_reg(uint32_t regnum, uint32_t val)
{
#if TARGET_IS_RISCV
    return riscv_write_reg(regnum, val);
#else
    return cortex_write_core_reg(regnum, val);
#endif
}

uint32_t target_gdb_reg_count(void)
{
#if TARGET_IS_RISCV
    return riscv_gdb_reg_count();
#else
    // Cortex-M: r0-r15 (16) + xPSR (1) = 17 registers
    return 17u;
#endif
}

bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count)
{
#if TARGET_IS_RISCV
    uint32_t count = riscv_gdb_reg_count();
    if (max_count < count) return false;
    return riscv_read_gdb_regs(regs, max_count);
#else
    if (max_count < 17u) return false;
    return cortex_read_gdb_regs(regs);
#endif
}

bool target_write_gdb_regs(const uint32_t *regs, uint32_t count)
{
#if TARGET_IS_RISCV
    return riscv_write_gdb_regs(regs, count);
#else
    if (count < 17u) return false;
    return cortex_write_gdb_regs(regs);
#endif
}

void target_breakpoints_init(void)
{
#if TARGET_IS_RISCV
    // RISC-V uses trigger module - initialization handled in riscv_init()
#else
    cortex_breakpoints_init();
#endif
}

bool target_breakpoint_insert(uint32_t addr)
{
#if TARGET_IS_RISCV
    // TODO: Implement RISC-V trigger module for breakpoints
    (void)addr;
    return false;
#else
    return cortex_breakpoint_insert(addr);
#endif
}

bool target_breakpoint_remove(uint32_t addr)
{
#if TARGET_IS_RISCV
    (void)addr;
    return false;
#else
    return cortex_breakpoint_remove(addr);
#endif
}

bool target_watchpoints_supported(void)
{
#if TARGET_IS_RISCV
    // TODO: Check trigger module capabilities
    return false;
#else
    return cortex_watchpoints_supported();
#endif
}

bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
#if TARGET_IS_RISCV
    (void)type; (void)addr; (void)len;
    return false;
#else
    return cortex_watchpoint_insert((cortexm_watch_t)type, addr, len);
#endif
}

bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
#if TARGET_IS_RISCV
    (void)type; (void)addr; (void)len;
    return false;
#else
    return cortex_watchpoint_remove((cortexm_watch_t)type, addr, len);
#endif
}

bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
#if TARGET_IS_RISCV
    (void)out_type; (void)out_addr;
    return false;
#else
    cortexm_watch_t wt;
    if (!cortex_watchpoint_hit(&wt, out_addr)) {
        return false;
    }
    if (out_type) {
        *out_type = (target_watch_t)wt;
    }
    return true;
#endif
}

bool target_xml_get(const char **out_xml, uint32_t *out_len)
{
#if TARGET_IS_RISCV
    // RISC-V target XML (minimal for RV32)
    static const char riscv_xml[] =
        "<?xml version=\"1.0\"?>"
        "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
        "<target version=\"1.0\">"
        "<architecture>riscv:rv32</architecture>"
        "</target>";
    *out_xml = riscv_xml;
    *out_len = sizeof(riscv_xml) - 1;
    return true;
#else
    return cortex_target_xml_get(out_xml, out_len);
#endif
}
