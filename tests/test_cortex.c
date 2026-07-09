#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cortex.h"
#include "target_mem.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            return false;                                                              \
        }                                                                              \
    } while (0)

#define DHCSR     0xE000EDF0u
#define CPUID     0xE000ED00u
#define DEMCR     0xE000EDFCu
#define FPB_CTRL  0xE0002000u
#define FPB_COMP0 0xE0002008u
#define DWT_CTRL  0xE0001000u
#define DWT_COMP0 0xE0001020u
#define DWT_MASK0 0xE0001024u
#define DWT_FUNC0 0xE0001028u

#define DHCSR_DBGKEY    (0xA05Fu << 16)
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT    (1u << 1)
#define DHCSR_C_STEP    (1u << 2)
#define DHCSR_C_MASKINTS (1u << 3)
#define DHCSR_S_HALT    (1u << 17)

typedef struct {
    uint32_t addr;
    uint32_t value;
    bool     succeeded;
} write_event_t;

static write_event_t g_writes[256];
static size_t        g_write_count;
static uint8_t       g_mem_ap;
static uint32_t      g_time_us;
static uint32_t      g_time_step_us;
static unsigned      g_dhcsr_reads;
static unsigned      g_halt_after_reads;
static unsigned      g_clear_halt_after_reads;
static bool          g_fail_dhcsr_read;
static uint32_t      g_fail_write_addr;
static uint32_t      g_fail_write_value;
static unsigned      g_fail_write_skip;
static unsigned      g_fail_write_count;
static unsigned      g_clear_error_calls;
static uint32_t      g_cpuid;

static void mock_reset(void)
{
    memset(g_writes, 0, sizeof(g_writes));
    g_write_count       = 0u;
    g_mem_ap            = 0u;
    g_time_us           = 0u;
    g_time_step_us      = 100u;
    g_dhcsr_reads       = 0u;
    g_halt_after_reads  = UINT32_MAX;
    g_clear_halt_after_reads = UINT32_MAX;
    g_fail_dhcsr_read   = false;
    g_fail_write_addr   = UINT32_MAX;
    g_fail_write_value  = UINT32_MAX;
    g_fail_write_skip   = 0u;
    g_fail_write_count  = 0u;
    g_clear_error_calls = 0u;
    g_cpuid             = (0x41u << 24) | (0xC60u << 4);
}

uint32_t hal_time_us(void)
{
    g_time_us += g_time_step_us;
    return g_time_us;
}

void adiv5_clear_errors(void) { g_clear_error_calls++; }

void target_mem_set_ap(uint8_t ap_sel) { g_mem_ap = ap_sel; }
uint8_t target_mem_get_ap(void) { return g_mem_ap; }

static uint32_t mock_cpuid(void)
{
    return g_cpuid;
}

bool target_mem_read_word_ap(uint8_t ap_sel, uint32_t addr, uint32_t *out)
{
    (void) ap_sel;
    if (addr != CPUID || !out) {
        return false;
    }
    *out = mock_cpuid();
    return true;
}

bool target_mem_read_word(uint32_t addr, uint32_t *out)
{
    if (!out) {
        return false;
    }
    switch (addr) {
    case CPUID:
        *out = mock_cpuid();
        return true;
    case DHCSR:
        g_dhcsr_reads++;
        if (g_fail_dhcsr_read) {
            return false;
        }
        if (g_clear_halt_after_reads != UINT32_MAX) {
            *out = (g_dhcsr_reads < g_clear_halt_after_reads) ? DHCSR_S_HALT : 0u;
        } else {
            *out = (g_dhcsr_reads >= g_halt_after_reads) ? DHCSR_S_HALT : 0u;
        }
        return true;
    case FPB_CTRL:
        *out = 1u << 4; // One FPB v1 code comparator.
        return true;
    case DEMCR:
        *out = 0u;
        return true;
    case DWT_CTRL:
        *out = 1u << 28; // One DWT comparator.
        return true;
    default:
        *out = 0u;
        return true;
    }
}

bool target_mem_write_word(uint32_t addr, uint32_t value)
{
    bool succeed = true;
    if (g_fail_write_count != 0u && addr == g_fail_write_addr &&
        value == g_fail_write_value) {
        if (g_fail_write_skip != 0u) {
            g_fail_write_skip--;
        } else {
            g_fail_write_count--;
            succeed = false;
        }
    }
    if (g_write_count < ARRAY_SIZE(g_writes)) {
        g_writes[g_write_count++] = (write_event_t) {
            .addr = addr,
            .value = value,
            .succeeded = succeed,
        };
    }
    return succeed;
}

static size_t count_successful_writes(uint32_t addr, uint32_t value)
{
    size_t count = 0u;
    for (size_t i = 0u; i < g_write_count; i++) {
        if (g_writes[i].succeeded && g_writes[i].addr == addr &&
            g_writes[i].value == value) {
            count++;
        }
    }
    return count;
}

static bool test_halt_waits_until_s_halt(void)
{
    mock_reset();
    g_halt_after_reads = 3u;

    CHECK(cortex_halt());
    CHECK(g_dhcsr_reads == 3u);
    CHECK(g_write_count == 1u);
    CHECK(g_writes[0].addr == DHCSR);
    CHECK(g_writes[0].value == (DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT));
    return true;
}

static bool test_halt_times_out_if_core_never_stops(void)
{
    mock_reset();
    g_time_step_us = 1000u;

    CHECK(!cortex_halt());
    CHECK(g_dhcsr_reads > 0u);
    CHECK(g_time_us >= 10000u);
    return true;
}

static bool test_halt_propagates_poll_transport_failure(void)
{
    mock_reset();
    g_fail_dhcsr_read = true;

    CHECK(!cortex_halt());
    CHECK(g_dhcsr_reads == 1u);
    return true;
}

static bool test_continue_waits_for_s_halt_to_clear(void)
{
    mock_reset();
    g_clear_halt_after_reads = 3u;
    CHECK(cortex_continue());
    CHECK(g_dhcsr_reads == 3u);
    CHECK(g_writes[0].addr == DHCSR);
    CHECK(g_writes[0].value == (DHCSR_DBGKEY | DHCSR_C_DEBUGEN));
    return true;
}

static bool test_continue_poll_failure_attempts_rehalt(void)
{
    mock_reset();
    g_fail_dhcsr_read = true;

    CHECK(!cortex_continue());
    CHECK(g_write_count == 2u);
    CHECK(g_writes[0].value == (DHCSR_DBGKEY | DHCSR_C_DEBUGEN));
    CHECK(g_writes[1].value ==
          (DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT));
    return true;
}

static bool test_continue_write_failure_attempts_rehalt(void)
{
    mock_reset();
    g_halt_after_reads = 1u;
    g_fail_write_addr  = DHCSR;
    g_fail_write_value = DHCSR_DBGKEY | DHCSR_C_DEBUGEN;
    g_fail_write_count = 1u;

    CHECK(!cortex_continue());
    CHECK(g_write_count == 2u);
    CHECK(!g_writes[0].succeeded);
    CHECK(g_writes[1].succeeded);
    CHECK(g_writes[1].value ==
          (DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT));
    return true;
}

static bool test_step_release_failure_still_clears_maskints(void)
{
    mock_reset();
    g_halt_after_reads = 1u;
    g_fail_write_addr  = DHCSR;
    g_fail_write_value = DHCSR_DBGKEY | DHCSR_C_DEBUGEN |
                         DHCSR_C_MASKINTS | DHCSR_C_STEP;
    g_fail_write_count = 1u;

    CHECK(!cortex_step());
    CHECK(count_successful_writes(
              DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) >= 2u);
    return true;
}

static bool test_step_reports_final_maskints_clear_failure(void)
{
    mock_reset();
    g_halt_after_reads = 1u;
    g_fail_write_addr  = DHCSR;
    g_fail_write_value = DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT;
    g_fail_write_skip  = 1u; // Initial halt succeeds; final clear fails.
    g_fail_write_count = 1u;

    CHECK(!cortex_step());

    // The failed clear remains sticky and is retried before a later halt or
    // resume. This retry succeeds and leaves the core safely halted.
    CHECK(cortex_halt());
    CHECK(g_fail_write_count == 0u);
    return true;
}

static bool initialize_target_and_resources(void)
{
    cortex_target_init();
    CHECK(cortex_target_get() == CORTEXM_TARGET_M0P);
    cortex_breakpoints_init();
    return true;
}

static bool test_new_session_forgets_breakpoint_and_watchpoint_ownership(void)
{
    mock_reset();
    CHECK(initialize_target_and_resources());

    const uint32_t bp_addr = 0x00001000u;
    const uint32_t bp_comp = 0x40001001u;
    const uint32_t wp_addr = 0x20000000u;
    CHECK(cortex_breakpoint_insert(bp_addr));
    CHECK(cortex_watchpoint_insert(CORTEXM_WATCH_WRITE, wp_addr, 4u));
    CHECK(count_successful_writes(FPB_COMP0, bp_comp) == 1u);
    CHECK(count_successful_writes(DWT_FUNC0, 6u) == 1u);

    cortex_target_init();
    CHECK(cortex_target_get() == CORTEXM_TARGET_M0P);

    // If stale ownership survived, each duplicate insertion would return true
    // without touching the comparator a second time.
    CHECK(cortex_breakpoint_insert(bp_addr));
    CHECK(cortex_watchpoint_insert(CORTEXM_WATCH_WRITE, wp_addr, 4u));
    CHECK(count_successful_writes(FPB_COMP0, bp_comp) == 2u);
    CHECK(count_successful_writes(DWT_FUNC0, 6u) == 2u);
    return true;
}

static bool test_watchpoint_rejects_invalid_length_and_alignment(void)
{
    mock_reset();
    CHECK(initialize_target_and_resources());

    CHECK(!cortex_watchpoint_insert(CORTEXM_WATCH_ACCESS, 0x20000000u, 0u));
    CHECK(!cortex_watchpoint_insert(CORTEXM_WATCH_ACCESS, 0x20000000u, 3u));
    CHECK(!cortex_watchpoint_insert(CORTEXM_WATCH_ACCESS, 0x20000002u, 4u));
    CHECK(count_successful_writes(DWT_COMP0, 0x20000000u) == 0u);
    CHECK(count_successful_writes(DWT_COMP0, 0x20000002u) == 0u);

    // Invalid requests must not consume the sole comparator.
    CHECK(cortex_watchpoint_insert(CORTEXM_WATCH_ACCESS, 0x20000000u, 4u));
    return true;
}

static bool test_breakpoint_remove_retains_bookkeeping_on_write_failure(void)
{
    mock_reset();
    CHECK(initialize_target_and_resources());

    const uint32_t old_addr = 0x00001000u;
    const uint32_t new_addr = 0x00002000u;
    CHECK(cortex_breakpoint_insert(old_addr));

    g_fail_write_addr  = FPB_COMP0;
    g_fail_write_value = 0u;
    g_fail_write_count = 1u;
    CHECK(!cortex_breakpoint_remove(old_addr));

    // There is only one comparator. A failed removal must leave it owned.
    CHECK(!cortex_breakpoint_insert(new_addr));

    g_fail_write_count = 0u;
    CHECK(cortex_breakpoint_remove(old_addr));
    CHECK(cortex_breakpoint_insert(new_addr));
    return true;
}

static bool test_m3_xml_uses_gdb_architecture_name(void)
{
    mock_reset();
    g_cpuid = (0x41u << 24) | (0xC23u << 4); // Cortex-M3
    cortex_target_init();
    CHECK(cortex_target_get() == CORTEXM_TARGET_M3);

    const char *xml = NULL;
    uint32_t len = 0u;
    CHECK(cortex_target_xml_get(&xml, &len));
    CHECK(xml != NULL && len != 0u);
    CHECK(strstr(xml, "<architecture>armv7</architecture>") != NULL);
    CHECK(strstr(xml, "armv7-m") == NULL);
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
        {"halt polls S_HALT", test_halt_waits_until_s_halt},
        {"halt timeout", test_halt_times_out_if_core_never_stops},
        {"halt transport error", test_halt_propagates_poll_transport_failure},
        {"continue polls S_HALT clear", test_continue_waits_for_s_halt_to_clear},
        {"continue failure re-halts", test_continue_poll_failure_attempts_rehalt},
        {"continue write ambiguity re-halts", test_continue_write_failure_attempts_rehalt},
        {"step release cleanup", test_step_release_failure_still_clears_maskints},
        {"step final cleanup failure", test_step_reports_final_maskints_clear_failure},
        {"session state reset", test_new_session_forgets_breakpoint_and_watchpoint_ownership},
        {"watchpoint validation", test_watchpoint_rejects_invalid_length_and_alignment},
        {"breakpoint removal failure", test_breakpoint_remove_retains_bookkeeping_on_write_failure},
        {"Cortex-M3 target XML", test_m3_xml_uses_gdb_architecture_name},
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
