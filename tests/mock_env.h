#pragma once

// Host-side mock of everything rsp.c links against: the HAL UART, the
// target abstraction, probe_attach, and the MSPM0 flash driver.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MOCK_ARCH_NONE = 0,
    MOCK_ARCH_CM,  // Cortex-M: 17 XML regs, PC=15, xPSR remote 16 (legacy alias 25)
    MOCK_ARCH_RV,  // RV32: 33 GDB regs, PC = reg 32
} mock_arch_t;

// Captured UART (probe -> GDB) bytes
extern char   mock_tx[16384];
extern size_t mock_tx_len;

// Target model
#define MOCK_MEM_BASE 0x20000000u
#define MOCK_MEM_SIZE 4096u
extern mock_arch_t mock_arch;
extern bool        mock_halted;
extern bool        mock_fail_halt;
extern bool        mock_fail_continue;
extern bool        mock_continue_failure_runs;
extern bool        mock_fail_halt_status;
extern uint32_t    mock_regs[42];
extern uint8_t     mock_mem[MOCK_MEM_SIZE];
extern int         mock_halt_calls, mock_continue_calls, mock_step_calls;
extern int         mock_disconnect_calls, mock_debug_clear_calls;

// Breakpoints
extern uint32_t mock_bp[8];
extern int      mock_bp_count;

// Watchpoint-hit injection for rsp_poll stop replies
extern bool     mock_watch_hit;
extern uint32_t mock_watch_addr;

// probe_attach behavior
extern mock_arch_t mock_attach_result;
extern int         mock_attach_calls;

// Flash driver mock
extern bool     mock_flash_geometry_ok;
extern uint32_t mock_flash_size, mock_flash_sector;
extern int      mock_flash_erase_calls;
extern uint32_t mock_flash_erase_addr, mock_flash_erase_len;
#define MOCK_FLASH_SIZE 4096u
extern uint8_t  mock_flash[MOCK_FLASH_SIZE];
extern uint32_t mock_flash_written; // bytes written so far
extern int      mock_flash_done_calls;
extern int      mock_flash_abort_calls;

void mock_reset(void);
