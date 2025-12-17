#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CORTEXM_TARGET_UNKNOWN = 0,
    CORTEXM_TARGET_M0,
    CORTEXM_TARGET_M0P,
    CORTEXM_TARGET_M3,
    CORTEXM_TARGET_M4,
    CORTEXM_TARGET_M7,
    CORTEXM_TARGET_M33,
    CORTEXM_TARGET_M55,
} cortexm_target_t;

// Detect target core/features (CPUID, etc.) and select the active target profile.
void cortex_target_init(void);
cortexm_target_t cortex_target_get(void);

// Optional: GDB target description (qXfer:features:read).
// Returns false if not supported/disabled at build-time.
bool cortex_target_xml_get(const char **out_xml, uint32_t *out_len);

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
