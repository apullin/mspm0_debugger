#pragma once

#include <stdbool.h>
#include <stdint.h>

bool cortex_halt(void);
bool cortex_continue(void);
bool cortex_step(void);
bool cortex_is_halted(bool *halted);

bool cortex_read_gdb_regs(uint32_t regs[17]);
bool cortex_write_gdb_regs(const uint32_t regs[17]);

bool cortex_read_core_reg(uint32_t regnum, uint32_t *out);
bool cortex_write_core_reg(uint32_t regnum, uint32_t v);

void cortex_breakpoints_init(void);
bool cortex_breakpoint_insert(uint32_t addr);
bool cortex_breakpoint_remove(uint32_t addr);
