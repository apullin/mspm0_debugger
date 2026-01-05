#pragma once

#include <stdbool.h>
#include <stdint.h>

bool adiv5_init(void);

bool adiv5_dp_read(uint8_t addr, uint32_t *out);
bool adiv5_dp_write(uint8_t addr, uint32_t v);

bool adiv5_ap_read(uint8_t ap_sel, uint8_t addr, uint32_t *out);
bool adiv5_ap_write(uint8_t ap_sel, uint8_t addr, uint32_t v);

// Clear sticky errors (STKERR/STKCMP/STKORUN/WDERR/ORUN).
void adiv5_clear_errors(void);
