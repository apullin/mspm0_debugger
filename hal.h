#pragma once

#include <stdint.h>

// Time
void delay_us(uint32_t us);

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
