#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adiv5.h"
#include "swd_bitbang.h"
#include "target_mem.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            return false;                                                              \
        }                                                                              \
    } while (0)

enum {
    DP_ABORT_ADDR2     = 0u,
    DP_CTRL_STAT_ADDR2 = 1u,
    DP_SELECT_ADDR2    = 2u,
    DP_RDBUFF_ADDR2    = 3u,
    AP_CSW_ADDR2       = 0u,
    AP_TAR_ADDR2       = 1u,
    AP_DRW_ADDR2       = 3u,
};

#define DP_POWER_ACK ((1u << 29) | (1u << 31))
#define CSW_DEFAULT  0x23000000u
#define CSW_SIZE_8   0u

typedef struct {
    bool     ap;
    bool     rnw;
    uint8_t  addr2;
    uint32_t value;
    swd_xfer_status_t result;
} swd_event_t;

static swd_event_t g_events[256];
static size_t      g_event_count;
static uint32_t    g_rdbuff_value;
static bool        g_fail_next_rdbuff;
static unsigned    g_leave_dormant_calls;
static unsigned    g_jtag_to_swd_calls;

static void mock_reset(void)
{
    memset(g_events, 0, sizeof(g_events));
    g_event_count          = 0u;
    g_rdbuff_value         = 0u;
    g_fail_next_rdbuff     = false;
    g_leave_dormant_calls  = 0u;
    g_jtag_to_swd_calls    = 0u;
    target_mem_set_ap(0u);
}

void swd_leave_dormant(void) { g_leave_dormant_calls++; }
void swd_jtag_to_swd(void) { g_jtag_to_swd_calls++; }
void swd_resync(void) {}

swd_xfer_status_t swd_transfer(bool ap, bool rnw, uint8_t addr2, uint32_t *data_inout)
{
    swd_xfer_status_t result = SWD_XFER_OK;
    uint32_t value = *data_inout;

    if (!ap && rnw) {
        if (addr2 == DP_ABORT_ADDR2) {
            value = 0x2BA01477u;
        } else if (addr2 == DP_CTRL_STAT_ADDR2) {
            value = DP_POWER_ACK;
        } else if (addr2 == DP_RDBUFF_ADDR2) {
            if (g_fail_next_rdbuff) {
                g_fail_next_rdbuff = false;
                result = SWD_XFER_FAULT;
            } else {
                value = g_rdbuff_value;
            }
        }
    }

    if (result == SWD_XFER_OK && rnw) {
        *data_inout = value;
    }

    if (g_event_count < ARRAY_SIZE(g_events)) {
        g_events[g_event_count++] = (swd_event_t) {
            .ap = ap,
            .rnw = rnw,
            .addr2 = addr2,
            .value = rnw ? value : *data_inout,
            .result = result,
        };
    }
    return result;
}

void delay_us(uint32_t us) { (void) us; }

static bool event_is(size_t i, bool ap, bool rnw, uint8_t addr2)
{
    return i < g_event_count && g_events[i].ap == ap && g_events[i].rnw == rnw &&
           g_events[i].addr2 == addr2;
}

static size_t count_events(bool ap, bool rnw, uint8_t addr2)
{
    size_t count = 0u;
    for (size_t i = 0u; i < g_event_count; i++) {
        if (event_is(i, ap, rnw, addr2)) {
            count++;
        }
    }
    return count;
}

static bool init_link(void)
{
    CHECK(adiv5_init());
    CHECK(g_leave_dormant_calls == 1u);
    CHECK(g_jtag_to_swd_calls == 1u);
    return true;
}

static bool test_init_selects_dp_bank_zero_before_ctrl_stat(void)
{
    mock_reset();
    CHECK(init_link());

    size_t select_i = SIZE_MAX;
    size_t ctrl_i   = SIZE_MAX;
    for (size_t i = 0u; i < g_event_count; i++) {
        if (select_i == SIZE_MAX && event_is(i, false, false, DP_SELECT_ADDR2) &&
            g_events[i].value == 0u) {
            select_i = i;
        }
        if (ctrl_i == SIZE_MAX && !g_events[i].ap &&
            g_events[i].addr2 == DP_CTRL_STAT_ADDR2) {
            ctrl_i = i;
        }
    }

    CHECK(select_i != SIZE_MAX);
    CHECK(ctrl_i != SIZE_MAX);
    CHECK(select_i < ctrl_i);
    return true;
}

static bool test_ap_write_flushes_through_rdbuff(void)
{
    mock_reset();
    CHECK(init_link());
    g_event_count = 0u;

    CHECK(adiv5_ap_write(0u, 0x0Cu, 0xA5A55A5Au));
    CHECK(g_event_count == 2u);
    CHECK(event_is(0u, true, false, AP_DRW_ADDR2));
    CHECK(g_events[0].value == 0xA5A55A5Au);
    CHECK(event_is(1u, false, true, DP_RDBUFF_ADDR2));
    return true;
}

static bool test_ap_write_propagates_completion_fault(void)
{
    mock_reset();
    CHECK(init_link());
    g_event_count = 0u;
    g_fail_next_rdbuff = true;

    CHECK(!adiv5_ap_write(0u, 0x0Cu, 0x11223344u));
    CHECK(g_event_count == 3u);
    CHECK(event_is(0u, true, false, AP_DRW_ADDR2));
    CHECK(event_is(1u, false, true, DP_RDBUFF_ADDR2));
    CHECK(g_events[1].result == SWD_XFER_FAULT);
    CHECK(event_is(2u, false, false, DP_ABORT_ADDR2));
    CHECK(g_events[2].value == 0x1Eu);
    return true;
}

static bool test_byte_read_uses_native_size_and_lane(void)
{
    mock_reset();
    CHECK(init_link());
    g_event_count  = 0u;
    g_rdbuff_value = 0x44332211u;

    uint8_t value = 0u;
    CHECK(target_mem_read_bytes_impl(0x20000002u, &value, 1u));
    CHECK(value == 0x33u);

    CHECK(count_events(true, true, AP_DRW_ADDR2) == 1u);
    CHECK(count_events(true, true, AP_CSW_ADDR2) == 0u);
    CHECK(count_events(true, true, AP_TAR_ADDR2) == 0u);

    bool saw_csw = false;
    bool saw_tar = false;
    for (size_t i = 0u; i < g_event_count; i++) {
        if (event_is(i, true, false, AP_CSW_ADDR2)) {
            CHECK(g_events[i].value == (CSW_DEFAULT | CSW_SIZE_8));
            saw_csw = true;
        } else if (event_is(i, true, false, AP_TAR_ADDR2)) {
            CHECK(g_events[i].value == 0x20000002u);
            saw_tar = true;
        }
    }
    CHECK(saw_csw);
    CHECK(saw_tar);
    return true;
}

static bool test_byte_write_uses_lane_without_read_modify_write(void)
{
    mock_reset();
    CHECK(init_link());
    g_event_count = 0u;

    const uint8_t value = 0x5Au;
    CHECK(target_mem_write_bytes_impl(0x40000003u, &value, 1u));
    CHECK(count_events(true, true, AP_DRW_ADDR2) == 0u);

    bool saw_csw = false;
    bool saw_tar = false;
    bool saw_drw = false;
    for (size_t i = 0u; i < g_event_count; i++) {
        if (event_is(i, true, false, AP_CSW_ADDR2)) {
            CHECK(g_events[i].value == (CSW_DEFAULT | CSW_SIZE_8));
            saw_csw = true;
        } else if (event_is(i, true, false, AP_TAR_ADDR2)) {
            CHECK(g_events[i].value == 0x40000003u);
            saw_tar = true;
        } else if (event_is(i, true, false, AP_DRW_ADDR2)) {
            CHECK(g_events[i].value == 0x5A000000u);
            saw_drw = true;
        }
    }
    CHECK(saw_csw);
    CHECK(saw_tar);
    CHECK(saw_drw);
    return true;
}

static bool test_byte_ranges_reject_address_wrap_and_null_buffers(void)
{
    mock_reset();
    uint8_t bytes[2] = {0u, 0u};

    CHECK(!target_mem_read_bytes_impl(UINT32_MAX, bytes, 2u));
    CHECK(!target_mem_write_bytes_impl(UINT32_MAX, bytes, 2u));
    CHECK(!target_mem_read_bytes_impl(0u, NULL, 1u));
    CHECK(!target_mem_write_bytes_impl(0u, NULL, 1u));
    CHECK(target_mem_read_bytes_impl(UINT32_MAX, NULL, 0u));
    CHECK(target_mem_write_bytes_impl(UINT32_MAX, NULL, 0u));
    CHECK(g_event_count == 0u);
    return true;
}

typedef bool (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t   fn;
} test_case_t;

int main(void)
{
    static const test_case_t tests[] = {
        {"init selects DP bank zero", test_init_selects_dp_bank_zero_before_ctrl_stat},
        {"AP write completion", test_ap_write_flushes_through_rdbuff},
        {"AP write completion error", test_ap_write_propagates_completion_fault},
        {"native byte read", test_byte_read_uses_native_size_and_lane},
        {"native byte write", test_byte_write_uses_lane_without_read_modify_write},
        {"byte range validation", test_byte_ranges_reject_address_wrap_and_null_buffers},
    };

    for (size_t i = 0u; i < ARRAY_SIZE(tests); i++) {
        if (!tests[i].fn()) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return 1;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return 0;
}
