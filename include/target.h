#pragma once

// Target abstraction layer for Cortex-M/SWD and RV32/RISC-V JTAG, with
// compile-time selection and runtime detection when both are enabled.

#include <stdbool.h>
#include <stdint.h>

// Watchpoint types (common across architectures)
typedef enum {
    TARGET_WATCH_WRITE = 0,
    TARGET_WATCH_READ,
    TARGET_WATCH_ACCESS,
} target_watch_t;

// Initialize target detection and state. Call after SWD/JTAG link is up.
void target_init(void);

// Forget the currently selected target after a detach or failed attach.
// The next session must call target_init() again before accessing it.
void target_disconnect(void);

// True once target_init() has detected an architecture.
bool target_attached(void);

// Execution control
bool target_halt(void);
bool target_continue(void);
bool target_step(void);
bool target_is_halted(bool *halted);

// Register access (architecture-specific register numbering)
// For Cortex-M: regnum 0-15 = r0-r15, 16 = xPSR
// For RISC-V:   regnum 0-31 = x0-x31, 32 = pc
bool target_read_reg(uint32_t regnum, uint32_t *out);
bool target_write_reg(uint32_t regnum, uint32_t val);

// The core regnum holding the PC (15 for Cortex-M, 32 for RISC-V).
uint32_t target_pc_regnum(void);

// Map a GDB p/P packet register number onto the core numbering above.
// Returns false for registers this architecture doesn't expose.
bool target_map_gdb_regnum(uint32_t gdb_regno, uint32_t *core_regno);

// GDB register block access (all GPRs + status in one call)
// Returns number of 32-bit registers in the block.
uint32_t target_gdb_reg_count(void);
bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count);
bool target_write_gdb_regs(const uint32_t *regs, uint32_t count);

// Breakpoints
void target_breakpoints_init(void);
// Disable all breakpoint/watchpoint resources owned by this debug session.
// Software bookkeeping is retained when a hardware clear fails so callers
// can retry instead of silently losing track of a live comparator.
bool target_debug_resources_clear(void);
bool target_breakpoint_insert(uint32_t addr);
bool target_breakpoint_remove(uint32_t addr);

// Watchpoints
bool target_watchpoints_supported(void);
bool target_watchpoint_insert(target_watch_t type, uint32_t addr, uint32_t len);
bool target_watchpoint_remove(target_watch_t type, uint32_t addr, uint32_t len);
bool target_watchpoint_hit(target_watch_t *out_type, uint32_t *out_addr);

// Memory access
bool target_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len);
bool target_mem_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len);

// Optional: GDB target description XML (qXfer:features:read)
// Returns false if not supported or disabled at build time.
bool target_xml_get(const char **out_xml, uint32_t *out_len);
