// Target abstraction implementation.
// Runtime selection between Cortex-M (SWD) and RISC-V (JTAG).
// When both are compiled in, tries SWD first, falls back to JTAG.

#include "target.h"

#if defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM)
#include "cortex.h"
#define HAVE_CORTEXM 1
#else
#define HAVE_CORTEXM 0
#endif

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

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
#define TARGET_HAVE_XML 1
#else
#define TARGET_HAVE_XML 0
#endif

void target_init(void)
{
    g_target_arch = TARGET_ARCH_NONE;

#if HAVE_CORTEXM
    // Try Cortex-M (SWD) first - it's more common
    cortex_target_init();
    if (cortex_is_connected()) {
        g_target_arch = TARGET_ARCH_CORTEX_M;
        return;
    }
#endif

#if HAVE_RISCV
    // Try RISC-V (JTAG) as fallback (or primary if Cortex-M disabled)
    if (riscv_init()) {
        g_target_arch = TARGET_ARCH_RISCV;
        return;
    }
#endif
}

void target_disconnect(void)
{
    g_target_arch = TARGET_ARCH_NONE;
}

bool target_attached(void)
{
    return g_target_arch != TARGET_ARCH_NONE;
}

uint32_t target_pc_regnum(void)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return 15u;
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return 32u;
#endif
        default:
            return 0u;
    }
}

bool target_map_gdb_regnum(uint32_t gdb_regno, uint32_t *core_regno)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            if (gdb_regno <= 15u) {
                *core_regno = gdb_regno;
                return true;
            }
            // XML descriptions number xPSR sequentially as remote register
            // 16.  Register 25 is the historical no-XML ARM CPSR slot; keep
            // accepting it for clients that issue legacy p/P packets.
            if ((TARGET_HAVE_XML && gdb_regno == 16u) ||
                gdb_regno == 25u) {
                *core_regno = 16u;
                return true;
            }
            return false;
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            if (gdb_regno <= 32u) {
                *core_regno = gdb_regno; // x0-x31, 32 = pc
                return true;
            }
            return false;
#endif
        default:
            (void) gdb_regno;
            (void) core_regno;
            return false;
    }
}

bool target_halt(void)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_halt();
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_continue();
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_step();
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_is_halted(halted);
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_read_core_reg(regnum, out);
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_write_core_reg(regnum, val);
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
#if TARGET_HAVE_XML
            // XML describes r0-r15 + xPSR as 17 consecutive registers.
            return 17u;
#else
            // Legacy ARM remote layout is 168 bytes: r0-r15, eight 96-bit
            // FPA registers, FPS, and CPSR/xPSR.  Retain it when XML is
            // explicitly disabled so stock GDB does not reject a short g.
            return 42u;
#endif
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
#if TARGET_HAVE_XML
            if (max_count < 17u) return false;
            return cortex_read_gdb_regs(regs);
#else
            if (max_count < 42u || !cortex_read_gdb_regs(regs)) return false;
            {
                uint32_t xpsr = regs[16];
                for (uint32_t i = 16u; i < 41u; i++) {
                    regs[i] = 0u; // unsupported legacy FPA/FPS slots
                }
                regs[41] = xpsr;
            }
            return true;
#endif
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
#if TARGET_HAVE_XML
            if (count != 17u) return false;
            return cortex_write_gdb_regs(regs);
#else
            if (count != 42u) return false;
            for (uint32_t i = 0; i < 16u; i++) {
                if (!cortex_write_core_reg(i, regs[i])) return false;
            }
            return cortex_write_core_reg(16u, regs[41]);
#endif
#endif
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
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            cortex_breakpoints_init();
            break;
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            // RISC-V uses trigger module - initialization handled in riscv_init()
            break;
#endif
        default:
            break;
    }
}

bool target_debug_resources_clear(void)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_debug_resources_clear();
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_debug_resources_clear();
#endif
        default:
            return true;
    }
}

bool target_breakpoint_insert(uint32_t addr)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_breakpoint_insert(addr);
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_breakpoint_insert(addr);
#endif
        default:
            (void)addr;
            return false;
    }
}

bool target_breakpoint_remove(uint32_t addr)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_breakpoint_remove(addr);
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_breakpoint_remove(addr);
#endif
        default:
            (void)addr;
            return false;
    }
}

bool target_watchpoints_supported(void)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoints_supported();
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_watchpoints_supported();
#endif
        default:
            return false;
    }
}

bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoint_insert((cortexm_watch_t)type, addr, len);
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_watchpoint_insert(type, addr, len);
#endif
        default:
            (void)type; (void)addr; (void)len;
            return false;
    }
}

bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_watchpoint_remove((cortexm_watch_t)type, addr, len);
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_watchpoint_remove(type, addr, len);
#endif
        default:
            (void)type; (void)addr; (void)len;
            return false;
    }
}

bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
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
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_watchpoint_hit(out_type, out_addr);
#endif
        default:
            (void)out_type; (void)out_addr;
            return false;
    }
}

bool target_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if ((len != 0u && (!buf || addr > UINT32_MAX - (len - 1u)))) {
        return false;
    }
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M: {
            // Use target_mem module for Cortex-M
            extern bool target_mem_read_bytes_impl(uint32_t addr, uint8_t *buf, uint32_t len);
            return target_mem_read_bytes_impl(addr, buf, len);
        }
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_mem_read(addr, buf, len);
#endif
        default:
            (void)addr; (void)buf; (void)len;
            return false;
    }
}

bool target_mem_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if ((len != 0u && (!buf || addr > UINT32_MAX - (len - 1u)))) {
        return false;
    }
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M: {
            extern bool target_mem_write_bytes_impl(uint32_t addr, const uint8_t *buf, uint32_t len);
            return target_mem_write_bytes_impl(addr, buf, len);
        }
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV:
            return riscv_mem_write(addr, buf, len);
#endif
        default:
            (void)addr; (void)buf; (void)len;
            return false;
    }
}

bool target_xml_get(const char **out_xml, uint32_t *out_len)
{
    switch (g_target_arch) {
#if HAVE_CORTEXM
        case TARGET_ARCH_CORTEX_M:
            return cortex_target_xml_get(out_xml, out_len);
#endif
#if HAVE_RISCV
        case TARGET_ARCH_RISCV: {
            // RISC-V target XML (minimal for RV32)
            static const char riscv_xml[] =
                "<target>"
                "<architecture>riscv:rv32</architecture>"
                "</target>";
            if (!out_xml || !out_len) return false;
            *out_xml = riscv_xml;
            *out_len = sizeof(riscv_xml) - 1;
            return true;
        }
#endif
        default:
            return false;
    }
}
