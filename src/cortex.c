// Cortex-M debug via CoreSight memory-mapped registers in SCS

#include "cortex.h"

#include <stddef.h>

#include "target_mem.h"

// Core debug regs
#define DHCSR 0xE000EDF0u
#define DCRSR 0xE000EDF4u
#define DCRDR 0xE000EDF8u
#define CPUID 0xE000ED00u

#define DHCSR_DBGKEY     (0xA05Fu << 16)
#define DHCSR_C_DEBUGEN  (1u << 0)
#define DHCSR_C_HALT     (1u << 1)
#define DHCSR_C_STEP     (1u << 2)
#define DHCSR_S_REGRDY   (1u << 16)
#define DHCSR_S_HALT     (1u << 17)

// FPB (Flash Patch and Breakpoint) unit (if present)
#define FPB_CTRL  0xE0002000u
#define FPB_COMP0 0xE0002008u

typedef struct {
    uint32_t addr;
    bool     used;
} fpb_slot_t;

static bool     g_fpb_inited   = false;
static uint8_t  g_fpb_num_code = 0;
static fpb_slot_t g_fpb_slots[8];

static cortexm_target_t g_target = CORTEXM_TARGET_UNKNOWN;

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

void cortex_target_init(void)
{
    // CPUID partno values (bits [15:4])
    // See ARM Cortex-M TRMs / ARM ARM.
    const uint16_t PARTNO_CM0  = 0xC20u;
    const uint16_t PARTNO_CM0P = 0xC60u;
    const uint16_t PARTNO_CM3  = 0xC23u;
    const uint16_t PARTNO_CM4  = 0xC24u;
    const uint16_t PARTNO_CM7  = 0xC27u;
    const uint16_t PARTNO_CM33 = 0xD21u;
    const uint16_t PARTNO_CM55 = 0xD22u;

    g_target = CORTEXM_TARGET_UNKNOWN;

    uint32_t cpuid = 0;
    if (!target_mem_read_word(CPUID, &cpuid)) {
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
