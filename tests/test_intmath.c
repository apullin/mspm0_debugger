#include "intmath.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static int check_division(uint64_t dividend, uint32_t divisor)
{
    uint32_t remainder = UINT32_MAX;
    uint64_t quotient =
        probe_u64_div_u32(dividend, divisor, &remainder);
    uint64_t expected_quotient = dividend / divisor;
    uint32_t expected_remainder = (uint32_t) (dividend % divisor);

    if (quotient != expected_quotient || remainder != expected_remainder) {
        fprintf(stderr,
            "division mismatch: %" PRIu64 " / %" PRIu32
            " = %" PRIu64 " r %" PRIu32
            ", expected %" PRIu64 " r %" PRIu32 "\n",
            dividend, divisor, quotient, remainder,
            expected_quotient, expected_remainder);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const uint64_t dividends[] = {
        0u,
        1u,
        UINT32_MAX,
        (uint64_t) UINT32_MAX + 1u,
        UINT64_C(0x0123456789ABCDEF),
        UINT64_MAX - 1u,
        UINT64_MAX,
    };
    static const uint32_t divisors[] = {
        1u, 2u, 3u, 10u, 115200u, 1000000u, UINT32_MAX,
    };

    for (uint32_t i = 0u; i < sizeof(dividends) / sizeof(dividends[0]); i++) {
        for (uint32_t j = 0u; j < sizeof(divisors) / sizeof(divisors[0]); j++) {
            if (!check_division(dividends[i], divisors[j])) {
                return 1;
            }
        }
    }

    uint64_t state = UINT64_C(0xD1B54A32D192ED03);
    for (uint32_t i = 0u; i < 10000u; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t dividend = state;
        uint32_t divisor = (uint32_t) (state >> 32) | 1u;
        if (!check_division(dividend, divisor)) {
            return 1;
        }
    }

    uint32_t remainder = 123u;
    if (probe_u64_div_u32(42u, 0u, &remainder) != UINT64_MAX ||
        remainder != 0u) {
        fputs("division-by-zero sentinel mismatch\n", stderr);
        return 1;
    }

    return 0;
}
