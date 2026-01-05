#pragma once

#include <stdbool.h>
#include <stdint.h>

// Select which MEM-AP/APSEL to use for subsequent target_mem_* operations.
// Default after boot is APSEL=0.
void target_mem_set_ap(uint8_t ap_sel);
uint8_t target_mem_get_ap(void);

// Direct read/write via a specific APSEL (used for discovery/probing).
bool target_mem_read_word_ap(uint8_t ap_sel, uint32_t addr, uint32_t *out);
bool target_mem_write_word_ap(uint8_t ap_sel, uint32_t addr, uint32_t v);

bool target_mem_read_word(uint32_t addr, uint32_t *out);
bool target_mem_write_word(uint32_t addr, uint32_t v);

bool target_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len);
bool target_mem_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len);
