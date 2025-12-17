// Cortex-M debug via CoreSight memory-mapped registers in SCS

#include "cortex.h"

#include "target_mem.h"

// Core debug regs
#define DHCSR 0xE000EDF0u
#define DCRSR 0xE000EDF4u
#define DCRDR 0xE000EDF8u

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
