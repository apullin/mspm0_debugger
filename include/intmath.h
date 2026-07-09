#ifndef INTMATH_H
#define INTMATH_H

#include <stdint.h>

/*
 * Small project-owned unsigned 64-by-32 divider. Cortex-M0+ has no hardware
 * divide, and the generic libgcc 64-bit ABI path costs more than 1 KB in the
 * C1104 image. The optional remainder is always smaller than divisor.
 * Division by zero returns UINT64_MAX and stores zero as the remainder.
 */
uint64_t probe_u64_div_u32(
    uint64_t dividend, uint32_t divisor, uint32_t *remainder_out);

#endif
