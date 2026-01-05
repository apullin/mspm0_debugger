#pragma once

#include <stdint.h>

// Time
void delay_us(uint32_t us);
uint32_t hal_time_us(void);     // monotonic time in microseconds (wraps at ~71 min @ 1MHz)

// UART
int  uart_getc(void);          // returns 0..255, or -1 if no data available
void uart_putc(uint8_t c);

// GPIO for SWD
void swclk_write(int level);   // 0/1
void swdio_write(int level);   // when configured as output
int  swdio_read(void);         // returns 0/1 when configured as input
void swdio_dir_out(void);
void swdio_dir_in(void);

// Optional but strongly recommended
void nreset_write(int level);  // target reset pin, 0/1

// GPIO for JTAG (optional, for RISC-V targets)
// Note: TCK can share with SWCLK, TMS can share with SWDIO in some configurations.
#if defined(PROBE_ENABLE_JTAG) && (PROBE_ENABLE_JTAG)
void jtag_tck_write(int level);   // clock, 0/1
void jtag_tms_write(int level);   // mode select, 0/1
void jtag_tdi_write(int level);   // data to target, 0/1
int  jtag_tdo_read(void);         // data from target, returns 0/1
#endif
