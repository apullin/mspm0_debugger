#pragma once

#include <stdint.h>

void rsp_init(void);
void rsp_process_byte(uint8_t c);
void rsp_poll(void);

