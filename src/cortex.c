// Cortex-M debug via CoreSight memory-mapped registers in SCS

#include "cortex.h"

#include <stddef.h>

#include "adiv5.h"
#include "hal.h"
#include "target_mem.h"

// Timeout for register access operations (microseconds)
#define REG_ACCESS_TIMEOUT_US 10000u  // 10ms

// Core debug regs
#define DHCSR 0xE000EDF0u
#define DCRSR 0xE000EDF4u
#define DCRDR 0xE000EDF8u
#define CPUID 0xE000ED00u
#define DFSR  0xE000ED30u
#define DEMCR 0xE000EDFCu

#define DEMCR_TRCENA   (1u << 24)
#define DFSR_DWTTRAP   (1u << 2)

#define DHCSR_DBGKEY     (0xA05Fu << 16)
#define DHCSR_C_DEBUGEN  (1u << 0)
#define DHCSR_C_HALT     (1u << 1)
#define DHCSR_C_STEP     (1u << 2)
#define DHCSR_C_MASKINTS (1u << 3)
#define DHCSR_S_REGRDY   (1u << 16)
#define DHCSR_S_HALT     (1u << 17)

// FPB (Flash Patch and Breakpoint) unit (if present)
#define FPB_CTRL  0xE0002000u
#define FPB_COMP0 0xE0002008u

#define DWT_CTRL 0xE0001000u
#define DWT_COMP0 0xE0001020u
#define DWT_MASK0 0xE0001024u
#define DWT_FUNC0 0xE0001028u

#define DWT_FUNC_MATCHED (1u << 24)

#define DWT_FUNC_V1_DATAVSIZE_SHIFT 10u
#define DWT_FUNC_V1_DATAVSIZE_WORD  (2u << DWT_FUNC_V1_DATAVSIZE_SHIFT)
#define DWT_FUNC_V1_READ            (5u << 0)
#define DWT_FUNC_V1_WRITE           (6u << 0)
#define DWT_FUNC_V1_ACCESS          (7u << 0)

#define DWT_FUNC_V2_MATCH_ACCESS    (4u << 0)
#define DWT_FUNC_V2_MATCH_WRITE     (5u << 0)
#define DWT_FUNC_V2_MATCH_READ      (6u << 0)
#define DWT_FUNC_V2_ACTION_DBG_EVENT (1u << 4)
#define DWT_FUNC_V2_LEN_VALUE(len)  (((len) >> 1) << 10)

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
// DWT_COMPn/MASKn/FUNCTIONn are architecturally spaced 0x10 apart.
static uint32_t dwt_comp_reg(uint8_t slot) { return DWT_COMP0 + 0x10u * (uint32_t) slot; }
static uint32_t dwt_mask_reg(uint8_t slot) { return DWT_MASK0 + 0x10u * (uint32_t) slot; }
static uint32_t dwt_func_reg(uint8_t slot) { return DWT_FUNC0 + 0x10u * (uint32_t) slot; }
#endif

typedef struct {
    uint32_t addr;
    bool     used;
} fpb_slot_t;

static bool     g_fpb_inited   = false;
static uint8_t  g_fpb_num_code = 0;
static uint8_t  g_fpb_rev      = 0; // FP_CTRL[31:28]: 0 = FPB v1, 1 = FPB v2
static fpb_slot_t g_fpb_slots[8];

static cortexm_target_t g_target = CORTEXM_TARGET_UNKNOWN;
// Once C_MASKINTS may have been set for a step, do not resume the core until
// a read-back-confirmed halt has allowed us to clear it again. Keep this
// sticky across transient transport failures so a later command can retry.
static bool g_step_maskints_cleanup_pending = false;

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
#define DWT_MAX_SLOTS 4u

typedef struct {
    uint32_t         addr;
    uint32_t         len;
    cortexm_watch_t  type;
    bool             used;
    uint8_t          slot;
} dwt_slot_t;

static bool      g_dwt_inited   = false;
static bool      g_dwt_ok       = false;
static uint8_t   g_dwt_num_comp = 0;
static dwt_slot_t g_dwt_slots[DWT_MAX_SLOTS];
#endif

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
static const char g_target_xml_v6m[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>armv6-m</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.m-profile\">\n"
    "    <reg name=\"r0\" bitsize=\"32\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\"/>\n"
    "    <reg name=\"xpsr\" bitsize=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char g_target_xml_v7m[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>armv7</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.m-profile\">\n"
    "    <reg name=\"r0\" bitsize=\"32\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\"/>\n"
    "    <reg name=\"xpsr\" bitsize=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char g_target_xml_v7em[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>armv7e-m</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.m-profile\">\n"
    "    <reg name=\"r0\" bitsize=\"32\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\"/>\n"
    "    <reg name=\"xpsr\" bitsize=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char g_target_xml_v8m_base[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>armv8-m.base</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.m-profile\">\n"
    "    <reg name=\"r0\" bitsize=\"32\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\"/>\n"
    "    <reg name=\"xpsr\" bitsize=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char g_target_xml_v8m_main[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>armv8-m.main</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.m-profile\">\n"
    "    <reg name=\"r0\" bitsize=\"32\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\"/>\n"
    "    <reg name=\"xpsr\" bitsize=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char *g_target_xml     = NULL;
static uint32_t    g_target_xml_len = 0;
#endif

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
static bool cortex_target_is_v8m(void)
{
    return g_target == CORTEXM_TARGET_M23 || g_target == CORTEXM_TARGET_M33 || g_target == CORTEXM_TARGET_M55;
}
#endif

static bool is_valid_arm_cpuid(uint32_t cpuid)
{
    // Implementer is bits [31:24]. ARM is 0x41.
    return ((cpuid >> 24) & 0xFFu) == 0x41u;
}

static bool select_memap_by_cpuid(uint32_t *out_cpuid)
{
    const uint8_t max_aps = 16u;

    // Try current selection first.
    uint8_t ap0 = target_mem_get_ap();
    uint32_t cpuid = 0;
    adiv5_clear_errors();
    if (target_mem_read_word_ap(ap0, CPUID, &cpuid) && is_valid_arm_cpuid(cpuid)) {
        if (out_cpuid) {
            *out_cpuid = cpuid;
        }
        return true;
    }

    // Scan APSEL values for a MEM-AP that can read CPUID.
    for (uint8_t ap = 0; ap < max_aps; ap++) {
        adiv5_clear_errors();
        cpuid = 0;
        if (!target_mem_read_word_ap(ap, CPUID, &cpuid)) {
            continue;
        }
        if (!is_valid_arm_cpuid(cpuid)) {
            continue;
        }
        target_mem_set_ap(ap);
        if (out_cpuid) {
            *out_cpuid = cpuid;
        }
        return true;
    }

    return false;
}

void cortex_target_init(void)
{
    // CPUID partno values (bits [15:4])
    // See ARM Cortex-M TRMs / ARM ARM.
    enum {
        PARTNO_CM0  = 0xC20,
        PARTNO_CM0P = 0xC60,
        PARTNO_CM3  = 0xC23,
        PARTNO_CM4  = 0xC24,
        PARTNO_CM7  = 0xC27,
        PARTNO_CM23 = 0xD20,
        PARTNO_CM33 = 0xD21,
        PARTNO_CM55 = 0xD22,
    };

    // A new attach may be a reset or an entirely different target.  Never
    // carry comparator counts/ownership or a cached XML selection across it.
    g_target       = CORTEXM_TARGET_UNKNOWN;
    g_fpb_inited   = false;
    g_fpb_num_code = 0u;
    g_fpb_rev      = 0u;
    for (uint8_t i = 0; i < (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0])); i++) {
        g_fpb_slots[i].used = false;
        g_fpb_slots[i].addr = 0u;
    }
#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    g_dwt_inited   = false;
    g_dwt_ok       = false;
    g_dwt_num_comp = 0u;
    for (uint8_t i = 0; i < DWT_MAX_SLOTS; i++) {
        g_dwt_slots[i].used = false;
        g_dwt_slots[i].addr = 0u;
        g_dwt_slots[i].len  = 0u;
        g_dwt_slots[i].type = CORTEXM_WATCH_ACCESS;
        g_dwt_slots[i].slot = i;
    }
#endif
#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
    g_target_xml     = NULL;
    g_target_xml_len = 0u;
#endif

    uint32_t cpuid = 0;
    if (!select_memap_by_cpuid(&cpuid)) {
        return;
    }

    uint16_t partno = (uint16_t) ((cpuid >> 4) & 0x0FFFu);
    switch (partno) {
    case PARTNO_CM0:
#if defined(PROBE_TARGET_M0)
        g_target = CORTEXM_TARGET_M0;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM0P:
        // Always supported (baseline target profile).
        g_target = CORTEXM_TARGET_M0P;
        break;
    case PARTNO_CM3:
#if defined(PROBE_TARGET_M3)
        g_target = CORTEXM_TARGET_M3;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM4:
#if defined(PROBE_TARGET_M4)
        g_target = CORTEXM_TARGET_M4;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM7:
#if defined(PROBE_TARGET_M7)
        g_target = CORTEXM_TARGET_M7;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM23:
#if defined(PROBE_TARGET_M23)
        g_target = CORTEXM_TARGET_M23;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM33:
#if defined(PROBE_TARGET_M33)
        g_target = CORTEXM_TARGET_M33;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    case PARTNO_CM55:
#if defined(PROBE_TARGET_M55)
        g_target = CORTEXM_TARGET_M55;
#else
        g_target = CORTEXM_TARGET_UNKNOWN;
#endif
        break;
    default:
        g_target = CORTEXM_TARGET_UNKNOWN;
        break;
    }

#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
    switch (g_target) {
    case CORTEXM_TARGET_M0:
    case CORTEXM_TARGET_M0P:
        g_target_xml     = g_target_xml_v6m;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v6m) - 1u);
        break;
    case CORTEXM_TARGET_M3:
        g_target_xml     = g_target_xml_v7m;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v7m) - 1u);
        break;
    case CORTEXM_TARGET_M4:
    case CORTEXM_TARGET_M7:
        g_target_xml     = g_target_xml_v7em;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v7em) - 1u);
        break;
    case CORTEXM_TARGET_M23:
        g_target_xml     = g_target_xml_v8m_base;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v8m_base) - 1u);
        break;
    case CORTEXM_TARGET_M33:
    case CORTEXM_TARGET_M55:
        g_target_xml     = g_target_xml_v8m_main;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v8m_main) - 1u);
        break;
    default:
        // Unknown Cortex-M: fall back to v6-M-ish reg model (works for basic debug on all M-profile cores).
        g_target_xml     = g_target_xml_v6m;
        g_target_xml_len = (uint32_t) (sizeof(g_target_xml_v6m) - 1u);
        break;
    }
#endif
}

cortexm_target_t cortex_target_get(void) { return g_target; }

bool cortex_is_connected(void) { return g_target != CORTEXM_TARGET_UNKNOWN; }

bool cortex_target_xml_get(const char **out_xml, uint32_t *out_len)
{
#if defined(PROBE_ENABLE_QXFER_TARGET_XML) && (PROBE_ENABLE_QXFER_TARGET_XML)
    if (!out_xml || !out_len) {
        return false;
    }
    if (!g_target_xml) {
        return false;
    }
    *out_xml = g_target_xml;
    *out_len = g_target_xml_len;
    return true;
#else
    (void) out_xml;
    (void) out_len;
    return false;
#endif
}

static bool cortex_write_dhcsr(uint32_t v)
{
    return target_mem_write_word(DHCSR, DHCSR_DBGKEY | v);
}

static bool cortex_read_dhcsr(uint32_t *out)
{
    return target_mem_read_word(DHCSR, out);
}

static bool cortex_wait_halted(void)
{
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < REG_ACCESS_TIMEOUT_US) {
        uint32_t dhcsr = 0u;
        if (!cortex_read_dhcsr(&dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_S_HALT) != 0u) {
            return true;
        }
    }
    return false;
}

static bool cortex_step_maskints_clear(bool halt_confirmed)
{
    if (!g_step_maskints_cleanup_pending) {
        return true;
    }

    // If a STEP release or poll failed, the core's state is ambiguous. Halt
    // it while preserving C_MASKINTS, then confirm S_HALT before changing the
    // mask bit (an architectural requirement).
    if (!halt_confirmed) {
        if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_HALT |
                                DHCSR_C_MASKINTS) ||
            !cortex_wait_halted()) {
            return false;
        }
    }

    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_HALT)) {
        return false;
    }
    g_step_maskints_cleanup_pending = false;
    return true;
}

bool cortex_halt(void)
{
    if (g_step_maskints_cleanup_pending) {
        return cortex_step_maskints_clear(false);
    }
    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_HALT)) {
        return false;
    }

    // The DHCSR write requests a halt; it does not prove that the core has
    // observed it.  Do not let register/flash operations race a running core.
    return cortex_wait_halted();
}

bool cortex_continue(void)
{
    if (!cortex_step_maskints_clear(false)) {
        return false;
    }
    // Request resume, then wait until the core has actually left Debug state.
    // Otherwise the next RSP poll can observe the old S_HALT and report an
    // immediate false stop without executing an instruction.
    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN)) {
        // AP-write failure is ambiguous: the resume may have reached DHCSR
        // even if its posted completion failed. Restore a known halted state.
        (void) cortex_halt();
        return false;
    }
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < REG_ACCESS_TIMEOUT_US) {
        uint32_t dhcsr = 0u;
        if (!cortex_read_dhcsr(&dhcsr)) {
            break;
        }
        if ((dhcsr & DHCSR_S_HALT) == 0u) {
            return true;
        }
    }
    // A timeout/read fault after the resume request may leave the core
    // running. Best-effort re-halt so an E01 never silently loses ownership.
    (void) cortex_halt();
    return false;
}

bool cortex_step(void)
{
    if (!cortex_step_maskints_clear(false)) {
        return false;
    }

    // Halt first to ensure known state.
    if (!cortex_halt()) {
        return false;
    }

    // Mask interrupts while stepping so the step lands on the next user
    // instruction instead of the first instruction of a pending ISR.
    // C_MASKINTS may only be changed while C_HALT is written as 1.
    // Treat even a failed transport write as potentially applied; cleanup is
    // sticky until a later read-back-confirmed halt lets us clear MASKINTS.
    g_step_maskints_cleanup_pending = true;
    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_HALT | DHCSR_C_MASKINTS)) {
        (void) cortex_step_maskints_clear(false);
        return false;
    }

    // Release halt with step requested; C_MASKINTS is held at its current
    // value so this write is architecturally allowed.
    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_MASKINTS | DHCSR_C_STEP)) {
        (void) cortex_step_maskints_clear(false);
        return false;
    }

    // Wait for step to complete (S_HALT set) with timeout.
    bool halted = false;
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < REG_ACCESS_TIMEOUT_US) {
        uint32_t dh = 0;
        if (!cortex_read_dhcsr(&dh)) {
            break;
        }
        if (dh & DHCSR_S_HALT) {
            halted = true;
            break;
        }
    }

    // On timeout/read failure, first force and confirm a halt while retaining
    // MASKINTS. A failed final clear is a failed step, never a false S05.
    bool cleaned = cortex_step_maskints_clear(halted);
    return halted && cleaned;
}

bool cortex_is_halted(bool *halted)
{
    uint32_t v = 0;
    if (!cortex_read_dhcsr(&v)) {
        return false;
    }
    *halted = (v & DHCSR_S_HALT) ? true : false;
    return true;
}

bool cortex_read_core_reg(uint32_t regnum, uint32_t *out)
{
    // Write reg selector, read transfer
    if (!target_mem_write_word(DCRSR, regnum & 0x1Fu)) {
        return false;
    }

    // Wait for S_REGRDY with timeout
    uint32_t start = hal_time_us();
    bool ready = false;
    while ((hal_time_us() - start) < REG_ACCESS_TIMEOUT_US) {
        uint32_t dh = 0;
        if (!cortex_read_dhcsr(&dh)) {
            return false;
        }
        if (dh & DHCSR_S_REGRDY) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        return false;  // Timeout waiting for register ready
    }

    return target_mem_read_word(DCRDR, out);
}

bool cortex_write_core_reg(uint32_t regnum, uint32_t v)
{
    if (!target_mem_write_word(DCRDR, v)) {
        return false;
    }
    if (!target_mem_write_word(DCRSR, (regnum & 0x1Fu) | (1u << 16))) {
        return false;
    }

    // Wait for S_REGRDY with timeout
    uint32_t start = hal_time_us();
    while ((hal_time_us() - start) < REG_ACCESS_TIMEOUT_US) {
        uint32_t dh = 0;
        if (!cortex_read_dhcsr(&dh)) {
            return false;
        }
        if (dh & DHCSR_S_REGRDY) {
            return true;  // Success
        }
    }
    return false;  // Timeout waiting for register ready
}

bool cortex_read_gdb_regs(uint32_t regs[17])
{
    // r0-r15
    for (uint32_t i = 0; i <= 15; i++) {
        if (!cortex_read_core_reg(i, &regs[i])) {
            return false;
        }
    }
    // xPSR is regnum 16 in the Core Debug scheme for v7-M/v8-M
    return cortex_read_core_reg(16, &regs[16]);
}

bool cortex_write_gdb_regs(const uint32_t regs[17])
{
    for (uint32_t i = 0; i <= 15; i++) {
        if (!cortex_write_core_reg(i, regs[i])) {
            return false;
        }
    }
    return cortex_write_core_reg(16, regs[16]);
}

static bool fpb_comp_value(uint32_t addr, uint32_t *out)
{
    if (g_fpb_rev >= 1u) {
        // FPB v2 (Cortex-M7 and v8-M): FP_COMPn = BPADDR[31:1], bit0 = BE.
        *out = (addr & 0xFFFFFFFEu) | 1u;
        return true;
    }
    // FPB v1 (M0/M0+/M3/M4): COMP[28:2] + REPLACE halfword select. The
    // comparator can only match code addresses below 0x20000000.
    if (addr >= 0x20000000u) {
        return false;
    }
    uint32_t replace = (addr & 2u) ? (2u << 30) : (1u << 30);
    *out = (addr & 0x1FFFFFFCu) | replace | 1u;
    return true;
}

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
static bool is_power_of_two_u32(uint32_t v)
{
    return v && ((v & (v - 1u)) == 0u);
}

static uint8_t ilog2_u32(uint32_t v)
{
    uint8_t n = 0;
    while (v > 1u) {
        v >>= 1u;
        n++;
    }
    return n;
}

static uint32_t dwt_v1_func(cortexm_watch_t type, uint32_t len)
{
    uint32_t datavsize = 0;
    if (g_target != CORTEXM_TARGET_M0 && g_target != CORTEXM_TARGET_M0P) {
        if (len <= 1u) {
            datavsize = 0u << DWT_FUNC_V1_DATAVSIZE_SHIFT;
        } else if (len == 2u) {
            datavsize = 1u << DWT_FUNC_V1_DATAVSIZE_SHIFT;
        } else {
            datavsize = DWT_FUNC_V1_DATAVSIZE_WORD;
        }
    }

    switch (type) {
    case CORTEXM_WATCH_WRITE:
        return DWT_FUNC_V1_WRITE | datavsize;
    case CORTEXM_WATCH_READ:
        return DWT_FUNC_V1_READ | datavsize;
    case CORTEXM_WATCH_ACCESS:
        return DWT_FUNC_V1_ACCESS | datavsize;
    default:
        return 0;
    }
}

static uint32_t dwt_v2_func(cortexm_watch_t type, uint32_t len)
{
    uint32_t match = 0;
    switch (type) {
    case CORTEXM_WATCH_WRITE:
        match = DWT_FUNC_V2_MATCH_WRITE;
        break;
    case CORTEXM_WATCH_READ:
        match = DWT_FUNC_V2_MATCH_READ;
        break;
    case CORTEXM_WATCH_ACCESS:
        match = DWT_FUNC_V2_MATCH_ACCESS;
        break;
    default:
        return 0;
    }

    if (len == 0u) {
        len = 1u;
    }
    return DWT_FUNC_V2_ACTION_DBG_EVENT | match | DWT_FUNC_V2_LEN_VALUE(len);
}

static bool cortex_dwt_init(void)
{
    if (g_dwt_inited) {
        return g_dwt_ok;
    }
    g_dwt_ok = false;

    g_dwt_num_comp = 0;
    for (uint8_t i = 0; i < DWT_MAX_SLOTS; i++) {
        g_dwt_slots[i].used = false;
        g_dwt_slots[i].addr = 0;
        g_dwt_slots[i].len  = 0;
        g_dwt_slots[i].type = CORTEXM_WATCH_ACCESS;
        g_dwt_slots[i].slot = i;
    }

    uint32_t demcr = 0;
    if (!target_mem_read_word(DEMCR, &demcr)) {
        return false;
    }
    if (!(demcr & DEMCR_TRCENA)) {
        if (!target_mem_write_word(DEMCR, demcr | DEMCR_TRCENA)) {
            return false;
        }
    }

    uint32_t ctrl = 0;
    if (!target_mem_read_word(DWT_CTRL, &ctrl)) {
        return false;
    }
    uint8_t num = (uint8_t) ((ctrl >> 28) & 0xFu);
    if (num > DWT_MAX_SLOTS) {
        num = DWT_MAX_SLOTS;
    }
    g_dwt_num_comp = num;

    for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
        if (!target_mem_write_word(dwt_func_reg(i), 0u)) {
            g_dwt_num_comp = 0u;
            return false;
        }
    }

    g_dwt_inited = true;
    g_dwt_ok = true;
    return true;
}
#endif

void cortex_breakpoints_init(void)
{
    if (g_fpb_inited) {
        return;
    }
    uint32_t ctrl = 0;
    if (!target_mem_read_word(FPB_CTRL, &ctrl)) {
        g_fpb_num_code = 0;
        return;
    }

    g_fpb_rev = (uint8_t) ((ctrl >> 28) & 0x0Fu);

    uint8_t num_code = (uint8_t) (((ctrl >> 4) & 0x0Fu) |
                                  ((ctrl >> 8) & 0x70u));
    if (num_code > (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0]))) {
        num_code = (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0]));
    }
    g_fpb_num_code = num_code;

    for (uint8_t i = 0; i < (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0])); i++) {
        g_fpb_slots[i].used = false;
        g_fpb_slots[i].addr = 0;
    }

    if (g_fpb_num_code == 0) {
        g_fpb_inited = true;
        return;
    }

    // Enable FPB. Writes to FP_CTRL are ignored unless KEY (bit 1) is
    // written as 1 (KEY reads as zero), so ENABLE must go in with KEY set.
    if (!target_mem_write_word(FPB_CTRL, ctrl | 3u)) {
        g_fpb_num_code = 0u;
        return;
    }

    // Clear any stale comparators
    for (uint8_t i = 0; i < g_fpb_num_code; i++) {
        if (!target_mem_write_word(FPB_COMP0 + 4u * (uint32_t) i, 0u)) {
            g_fpb_num_code = 0u;
            return;
        }
    }

    g_fpb_inited = true;

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    (void) cortex_dwt_init();
#endif
}

bool cortex_debug_resources_clear(void)
{
    bool ok = true;

    if (!cortex_step_maskints_clear(false)) {
        ok = false;
    }

    if (g_fpb_inited) {
        for (uint8_t i = 0; i < g_fpb_num_code; i++) {
            if (g_fpb_slots[i].used) {
                if (target_mem_write_word(FPB_COMP0 + 4u * (uint32_t) i, 0u)) {
                    g_fpb_slots[i].used = false;
                    g_fpb_slots[i].addr = 0u;
                } else {
                    ok = false;
                }
            }
        }
    }

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    if (g_dwt_inited) {
        for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
            if (!g_dwt_slots[i].used) {
                continue;
            }
            uint8_t slot = g_dwt_slots[i].slot;
            bool cleared = target_mem_write_word(dwt_func_reg(slot), 0u);
            if (cleared && !cortex_target_is_v8m()) {
                cleared = target_mem_write_word(dwt_mask_reg(slot), 0u);
            }
            if (cleared) {
                cleared = target_mem_write_word(dwt_comp_reg(slot), 0u);
            }
            if (cleared) {
                g_dwt_slots[i].used = false;
                g_dwt_slots[i].addr = 0u;
                g_dwt_slots[i].len  = 0u;
            } else {
                ok = false;
            }
        }
    }
#endif

    return ok;
}

bool cortex_breakpoint_insert(uint32_t addr)
{
    if (!g_fpb_inited) {
        cortex_breakpoints_init();
    }
    if (g_fpb_num_code == 0) {
        return false;
    }

    // Already installed?
    for (uint8_t i = 0; i < g_fpb_num_code; i++) {
        if (g_fpb_slots[i].used && g_fpb_slots[i].addr == addr) {
            return true;
        }
    }

    for (uint8_t i = 0; i < g_fpb_num_code; i++) {
        if (!g_fpb_slots[i].used) {
            uint32_t comp = 0;
            if (!fpb_comp_value(addr, &comp)) {
                return false; // address not encodable on this FPB revision
            }
            if (!target_mem_write_word(FPB_COMP0 + 4u * (uint32_t) i, comp)) {
                return false;
            }
            g_fpb_slots[i].used = true;
            g_fpb_slots[i].addr = addr;
            return true;
        }
    }
    return false;
}

bool cortex_breakpoint_remove(uint32_t addr)
{
    if (!g_fpb_inited) {
        cortex_breakpoints_init();
    }
    if (g_fpb_num_code == 0) {
        return false;
    }

    for (uint8_t i = 0; i < g_fpb_num_code; i++) {
        if (g_fpb_slots[i].used && g_fpb_slots[i].addr == addr) {
            if (!target_mem_write_word(FPB_COMP0 + 4u * (uint32_t) i, 0u)) {
                return false;
            }
            g_fpb_slots[i].used = false;
            g_fpb_slots[i].addr = 0;
            return true;
        }
    }
    return true;
}

bool cortex_watchpoint_insert(cortexm_watch_t type, uint32_t addr, uint32_t len)
{
#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    if (!cortex_dwt_init()) {
        return false;
    }
    if (g_dwt_num_comp == 0) {
        return false;
    }

    // Already installed?
    for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
        if (g_dwt_slots[i].used && g_dwt_slots[i].addr == addr && g_dwt_slots[i].len == len &&
            g_dwt_slots[i].type == type) {
            return true;
        }
    }

    uint8_t slot = 0xFFu;
    for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
        if (!g_dwt_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == 0xFFu) {
        return false;
    }

    if (len == 0u || !is_power_of_two_u32(len) ||
        (addr & (len - 1u)) != 0u) {
        return false;
    }

    uint32_t func = 0;
    if (cortex_target_is_v8m()) {
        // DATAVSIZE is 2 bits: byte/halfword/word only.
        if (len > 4u) {
            return false;
        }
        func = dwt_v2_func(type, len);
    } else {
        func = dwt_v1_func(type, len);
    }
    if (func == 0u) {
        return false;
    }

    uint32_t comp = addr;

    if (!target_mem_write_word(dwt_comp_reg(slot), comp)) {
        return false;
    }
    if (!cortex_target_is_v8m()) {
        uint32_t mask = ilog2_u32(len);
        if (!target_mem_write_word(dwt_mask_reg(slot), mask)) {
            return false;
        }
    }
    if (!target_mem_write_word(dwt_func_reg(slot), func)) {
        (void) target_mem_write_word(dwt_func_reg(slot), 0u);
        return false;
    }

    g_dwt_slots[slot].used = true;
    g_dwt_slots[slot].addr = addr;
    g_dwt_slots[slot].len  = len;
    g_dwt_slots[slot].type = type;
    g_dwt_slots[slot].slot = slot;
    return true;
#else
    (void) type;
    (void) addr;
    (void) len;
    return false;
#endif
}

bool cortex_watchpoints_supported(void)
{
#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    if (!cortex_dwt_init()) {
        return false;
    }
    return g_dwt_num_comp != 0;
#else
    return false;
#endif
}

bool cortex_watchpoint_remove(cortexm_watch_t type, uint32_t addr, uint32_t len)
{
#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    if (!g_dwt_inited) {
        return true;
    }

    for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
        if (g_dwt_slots[i].used && g_dwt_slots[i].addr == addr && g_dwt_slots[i].len == len &&
            g_dwt_slots[i].type == type) {
            uint8_t slot = g_dwt_slots[i].slot;
            if (!target_mem_write_word(dwt_func_reg(slot), 0u)) {
                return false;
            }
            if (!cortex_target_is_v8m() &&
                !target_mem_write_word(dwt_mask_reg(slot), 0u)) {
                return false;
            }
            if (!target_mem_write_word(dwt_comp_reg(slot), 0u)) {
                return false;
            }
            g_dwt_slots[i].used = false;
            g_dwt_slots[i].addr = 0;
            g_dwt_slots[i].len  = 0;
            return true;
        }
    }

    return true;
#else
    (void) type;
    (void) addr;
    (void) len;
    return false;
#endif
}

bool cortex_watchpoint_hit(cortexm_watch_t *out_type, uint32_t *out_addr)
{
#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    if (!g_dwt_inited || g_dwt_num_comp == 0) {
        return false;
    }

    uint32_t dfsr = 0;
    if (!target_mem_read_word(DFSR, &dfsr)) {
        return false;
    }
    if (!(dfsr & DFSR_DWTTRAP)) {
        return false;
    }

    bool found = false;
    if (out_type) {
        *out_type = CORTEXM_WATCH_ACCESS;
    }
    if (out_addr) {
        *out_addr = 0;
    }

    for (uint8_t i = 0; i < g_dwt_num_comp; i++) {
        uint32_t func = 0;
        if (!target_mem_read_word(dwt_func_reg(i), &func)) {
            continue;
        }
        // Reading FUNC clears MATCHED bit; continue loop to clear all.
        // Report only the first matched watchpoint to GDB.
        if ((func & DWT_FUNC_MATCHED) && g_dwt_slots[i].used && !found) {
            if (out_type) {
                *out_type = g_dwt_slots[i].type;
            }
            if (out_addr) {
                *out_addr = g_dwt_slots[i].addr;
            }
            found = true;
        }
    }

    (void) target_mem_write_word(DFSR, DFSR_DWTTRAP);
    return found;
#else
    (void) out_type;
    (void) out_addr;
    return false;
#endif
}
