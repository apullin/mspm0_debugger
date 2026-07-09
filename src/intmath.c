#include "intmath.h"

#include <stdint.h>

uint64_t probe_u64_div_u32(
    uint64_t dividend, uint32_t divisor, uint32_t *remainder_out)
{
    if (divisor == 0u) {
        if (remainder_out) {
            *remainder_out = 0u;
        }
        return UINT64_MAX;
    }

    uint32_t dividend_hi = (uint32_t) (dividend >> 32);
    uint32_t dividend_lo = (uint32_t) dividend;
    uint32_t quotient_hi = dividend_hi / divisor;
    uint32_t quotient_lo = 0u;
    uint32_t remainder = dividend_hi % divisor;

    /* Divide the remaining base-2^32 digit with restoring division. The
     * carry records the conceptual 33rd remainder bit, so every operation in
     * the loop stays 32-bit on Armv6-M. */
    for (uint32_t bit = 0u; bit < 32u; bit++) {
        uint32_t carry = remainder >> 31;
        remainder = (remainder << 1) | (dividend_lo >> 31);
        dividend_lo <<= 1;
        quotient_lo <<= 1;

        if (carry || remainder >= divisor) {
            remainder -= divisor;
            quotient_lo |= 1u;
        }
    }

    if (remainder_out) {
        *remainder_out = remainder;
    }
    return ((uint64_t) quotient_hi << 32) | (uint64_t) quotient_lo;
}
