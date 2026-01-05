// Target abstraction implementation.
// Runtime selection between Cortex-M (SWD) and RISC-V (JTAG).
// When both are compiled in, tries SWD first, falls back to JTAG.

#include "target.h"
#include "cortex.h"

#if defined(PROBE_ENABLE_RISCV) && (PROBE_ENABLE_RISCV)
#include "riscv.h"
#define HAVE_RISCV 1
#else
#define HAVE_RISCV 0
#endif

typedef enum {
    TARGET_ARCH_NONE = 0,
    TARGET_ARCH_CORTEX_M,
    TARGET_ARCH_RISCV,
} target_arch_t;

static target_arch_t g_target_arch = TARGET_ARCH_NONE;

void target_init(void)
{
    g_target_arch = TARGET_ARCH_NONE;

    // Try Cortex-M (SWD) first - it's more common
    cortex_target_init();
    if (cortex_is_connected()) {
        g_target_arch = TARGET_ARCH_CORTEX_M;
        return;
    }

#if HAVE_RISCV
    // Try RISC-V (JTAG) as fallback
    if (riscv_init()) {
        g_target_arch = TARGET_ARCH_RISCV;
        return;
    }
#endif
}

bool target_halt(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_halt();
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_halt();
#endif
        default:
            return false;
    }
}

bool target_continue(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_continue();
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_continue();
#endif
        default:
            return false;
    }
}

bool target_step(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_step();
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_step();
#endif
        default:
            return false;
    }
}

bool target_is_halted(bool *halted)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_is_halted(halted);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_is_halted(halted);
#endif
        default:
            return false;
    }
}

bool target_read_reg(uint32_t regnum, uint32_t *out)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_read_core_reg(regnum, out);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_read_reg(regnum, out);
#endif
        default:
            return false;
    }
}

bool target_write_reg(uint32_t regnum, uint32_t val)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_write_core_reg(regnum, val);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_write_reg(regnum, val);
#endif
        default:
            return false;
    }
}

uint32_t target_gdb_reg_count(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            // Cortex-M: r0-r15 (16) + xPSR (1) = 17 registers
            return 17u;
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_gdb_reg_count();
#endif
        default:
            return 0;
    }
}

bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            if (max_count < 17u) return false;
            return cortex_read_gdb_regs(regs);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV: {
            uint32_t count = riscv_gdb_reg_count();
            if (max_count < count) return false;
            return riscv_read_gdb_regs(regs, max_count);
        }
#endif
        default:
            return false;
    }
}

bool target_write_gdb_regs(const uint32_t *regs, uint32_t count)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            if (count < 17u) return false;
            return cortex_write_gdb_regs(regs);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_write_gdb_regs(regs, count);
#endif
        default:
            return false;
    }
}

void target_breakpoints_init(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            cortex_breakpoints_init();
            break;
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            // RISC-V uses trigger module - initialization handled in riscv_init()
            break;
#endif
        default:
            break;
    }
}

bool target_breakpoint_insert(uint32_t addr)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_breakpoint_insert(addr);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            // TODO: Implement RISC-V trigger module for breakpoints
            (void)addr;
            return false;
#endif
        default:
            return false;
    }
}

bool target_breakpoint_remove(uint32_t addr)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_breakpoint_remove(addr);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            (void)addr;
            return false;
#endif
        default:
            return false;
    }
}

bool target_watchpoints_supported(void)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoints_supported();
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            // TODO: Check trigger module capabilities
            return false;
#endif
        default:
            return false;
    }
}

bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoint_insert((cortexm_watch_t)type, addr, len);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            (void)type; (void)addr; (void)len;
            return false;
#endif
        default:
            return false;
    }
}

bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoint_remove((cortexm_watch_t)type, addr, len);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            (void)type; (void)addr; (void)len;
            return false;
#endif
        default:
            return false;
    }
}

bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M: {
            cortexm_watch_t wt;
            if (!cortex_watchpoint_hit(&wt, out_addr)) {
                return false;
            }
            if (out_type) {
                *out_type = (target_watch_t)wt;
            }
            return true;
        }
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            (void)out_type; (void)out_addr;
            return false;
#endif
        default:
            return false;
    }
}

bool target_xml_get(const char **out_xml, uint32_t *out_len)
{
    switch (g_target_arch) {
        case TARGET_ARCH_CORTEX_M:
            return cortex_target_xml_get(out_xml, out_len);
#if HAVE_RISCV
        case TARGET_ARCH_RISCV: {
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
        }
#endif
        default:
            return false;
    }
}
