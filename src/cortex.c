// Cortex-M debug via CoreSight memory-mapped registers in SCS

#include "cortex.h"

#include <stddef.h>

#include "adiv5.h"
#include "target_mem.h"

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

static uint32_t dwt_comp_reg(uint8_t slot) { return DWT_COMP0 + 0x20u * (uint32_t) slot; }
static uint32_t dwt_mask_reg(uint8_t slot) { return DWT_MASK0 + 0x20u * (uint32_t) slot; }
static uint32_t dwt_func_reg(uint8_t slot) { return DWT_FUNC0 + 0x20u * (uint32_t) slot; }

typedef struct {
    uint32_t addr;
    bool     used;
} fpb_slot_t;

static bool     g_fpb_inited   = false;
static uint8_t  g_fpb_num_code = 0;
static fpb_slot_t g_fpb_slots[8];

static cortexm_target_t g_target = CORTEXM_TARGET_UNKNOWN;

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
    "  <architecture>armv7-m</architecture>\n"
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

static bool cortex_target_is_v8m(void)
{
    return g_target == CORTEXM_TARGET_M23 || g_target == CORTEXM_TARGET_M33 || g_target == CORTEXM_TARGET_M55;
}

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
    const uint16_t PARTNO_CM0  = 0xC20u;
    const uint16_t PARTNO_CM0P = 0xC60u;
    const uint16_t PARTNO_CM3  = 0xC23u;
    const uint16_t PARTNO_CM4  = 0xC24u;
    const uint16_t PARTNO_CM7  = 0xC27u;
    const uint16_t PARTNO_CM23 = 0xD20u;
    const uint16_t PARTNO_CM33 = 0xD21u;
    const uint16_t PARTNO_CM55 = 0xD22u;

    g_target = CORTEXM_TARGET_UNKNOWN;

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

bool cortex_halt(void)
{
    // Enable debug + halt
    return cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_HALT);
}

bool cortex_continue(void)
{
    // Debug enable, clear halt/step
    return cortex_write_dhcsr(DHCSR_C_DEBUGEN);
}

bool cortex_step(void)
{
    // Halt first, then pulse step while debug enabled.
    if (!cortex_halt()) {
        return false;
    }
    if (!cortex_write_dhcsr(DHCSR_C_DEBUGEN | DHCSR_C_STEP)) {
        return false;
    }
    // Return to halt after step
    return cortex_halt();
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
    for (int i = 0; i < 10000; i++) {
        uint32_t dh = 0;
        if (!cortex_read_dhcsr(&dh)) {
            return false;
        }
        if (dh & DHCSR_S_REGRDY) {
            break;
        }
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
    for (int i = 0; i < 10000; i++) {
        uint32_t dh = 0;
        if (!cortex_read_dhcsr(&dh)) {
            return false;
        }
        if (dh & DHCSR_S_REGRDY) {
            break;
        }
    }
    return true;
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

static uint32_t fpb_comp_value(uint32_t addr)
{
    uint32_t replace = (addr & 2u) ? (2u << 30) : (1u << 30);
    return (addr & 0x1FFFFFFCu) | replace | 1u;
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
    g_dwt_inited = true;
    g_dwt_ok     = false;

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
        (void) target_mem_write_word(dwt_func_reg(i), 0u);
    }

    g_dwt_ok = true;
    return true;
}
#endif

void cortex_breakpoints_init(void)
{
    if (g_fpb_inited) {
        return;
    }
    g_fpb_inited = true;

    uint32_t ctrl = 0;
    if (!target_mem_read_word(FPB_CTRL, &ctrl)) {
        g_fpb_num_code = 0;
        return;
    }

    uint8_t num_code = (uint8_t) ((ctrl >> 4) & 0x0Fu);
    if (num_code > (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0]))) {
        num_code = (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0]));
    }
    g_fpb_num_code = num_code;

    for (uint8_t i = 0; i < (uint8_t) (sizeof(g_fpb_slots) / sizeof(g_fpb_slots[0])); i++) {
        g_fpb_slots[i].used = false;
        g_fpb_slots[i].addr = 0;
    }

    if (g_fpb_num_code == 0) {
        return;
    }

    // Enable FPB
    (void) target_mem_write_word(FPB_CTRL, ctrl | 1u);

    // Clear any stale comparators
    for (uint8_t i = 0; i < g_fpb_num_code; i++) {
        (void) target_mem_write_word(FPB_COMP0 + 4u * (uint32_t) i, 0u);
    }

#if defined(PROBE_ENABLE_DWT_WATCHPOINTS) && (PROBE_ENABLE_DWT_WATCHPOINTS)
    (void) cortex_dwt_init();
#endif
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
            uint32_t comp = fpb_comp_value(addr);
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

    uint32_t func = 0;
    if (cortex_target_is_v8m()) {
        func = dwt_v2_func(type, len);
    } else {
        if (!is_power_of_two_u32(len)) {
            return false;
        }
        func = dwt_v1_func(type, len);
    }
    if (func == 0u) {
        return false;
    }

    uint32_t comp = addr;
    if (len >= 2u && is_power_of_two_u32(len)) {
        comp = addr & ~(len - 1u);
    }

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
            (void) target_mem_write_word(dwt_func_reg(slot), 0u);
            (void) target_mem_write_word(dwt_mask_reg(slot), 0u);
            (void) target_mem_write_word(dwt_comp_reg(slot), 0u);
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
        if ((func & DWT_FUNC_MATCHED) && g_dwt_slots[i].used) {
            if (out_type) {
                *out_type = g_dwt_slots[i].type;
            }
            if (out_addr) {
                *out_addr = g_dwt_slots[i].addr;
            }
            found = true;
            break;
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
