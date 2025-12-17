#pragma once

#include <stdbool.h>
#include <stdint.h>

void swd_jtag_to_swd(void);
bool swd_transfer(bool ap, bool rnw, uint8_t addr2, uint32_t *data_inout);

