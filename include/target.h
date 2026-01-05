#pragma once

// Target abstraction layer for multi-architecture support.
// Currently supports Cortex-M; RISC-V support can be added by implementing
// the same interface in src/riscv.c and selecting via compile-time option.

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

// GDB register block access (all GPRs + status in one call)
// Returns number of 32-bit registers in the block.
uint32_t target_gdb_reg_count(void);
bool target_read_gdb_regs(uint32_t *regs, uint32_t max_count);
bool target_write_gdb_regs(const uint32_t *regs, uint32_t count);

// Breakpoints
void target_breakpoints_init(void);
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
