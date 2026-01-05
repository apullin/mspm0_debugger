#pragma once

// RISC-V Debug Module interface for GDB RSP.
// Implements halt/resume/step, register access, and memory access via
// RISC-V Debug Spec 0.13 Abstract Commands.

#include <stdbool.h>
#include <stdint.h>

// Initialize RISC-V debug (JTAG init, read IDCODE/DTMCS, probe DM).
// Returns true if a valid RISC-V debug target is found.
bool riscv_init(void);

// Halt the hart.
bool riscv_halt(void);

// Resume execution.
bool riscv_continue(void);

// Single-step one instruction.
bool riscv_step(void);

// Check if hart is halted.
bool riscv_is_halted(bool *halted);

// Read a GPR (0-31) or PC (32).
bool riscv_read_reg(uint32_t regnum, uint32_t *out);

// Write a GPR (0-31) or PC (32).
bool riscv_write_reg(uint32_t regnum, uint32_t val);

// Number of GDB registers (32 GPRs + PC = 33 for RV32).
uint32_t riscv_gdb_reg_count(void);

// Read all GDB registers into buffer.
bool riscv_read_gdb_regs(uint32_t *regs, uint32_t max_count);

// Write all GDB registers from buffer.
bool riscv_write_gdb_regs(const uint32_t *regs, uint32_t count);

// Memory access (via Program Buffer or System Bus Access).
bool riscv_mem_read(uint32_t addr, uint8_t *buf, uint32_t len);
bool riscv_mem_write(uint32_t addr, const uint8_t *buf, uint32_t len);

// Get stop reason after halt.
// Returns GDB signal number (e.g., 5 for SIGTRAP).
uint8_t riscv_stop_reason(void);
