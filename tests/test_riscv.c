#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hal.h"
#include "jtag.h"
#include "riscv.h"

static void check_impl(bool condition, const char *expression, int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
        abort();
    }
}

#define CHECK(expr) check_impl((expr), #expr, __LINE__)

enum {
    DMI_DATA0 = 0x04u,
    DMI_DATA1 = 0x05u,
    DMI_DMCONTROL = 0x10u,
    DMI_DMSTATUS = 0x11u,
    DMI_ABSTRACTCS = 0x16u,
    DMI_COMMAND = 0x17u,
    DMI_SBCS = 0x38u,
    DMI_SBADDRESS0 = 0x39u,
    DMI_SBDATA0 = 0x3cu,
};

#define DMCONTROL_DMACTIVE       (1u << 0)
#define DMCONTROL_HALTREQ        (1u << 31)
#define DMCONTROL_RESUMEREQ      (1u << 30)
#define DMSTATUS_AUTHENTICATED   (1u << 7)
#define DMSTATUS_ALLHALTED       (1u << 9)
#define DMSTATUS_ALLRUNNING      (1u << 11)
#define DMSTATUS_ALLRESUMEACK    (1u << 17)
#define SBCS_CAP_ACCESS8         (1u << 0)
#define SBCS_CAP_ACCESS32        (1u << 2)
#define SBCS_ACCESS_SHIFT        17
#define SBCS_ACCESS8             0u
#define SBCS_ACCESS32            2u
#define SBCS_READONADDR          (1u << 20)
#define SBCS_SBBUSY              (1u << 21)
#define CSR_MISA                 0x301u
#define CSR_TSELECT              0x7a0u
#define CSR_TDATA1               0x7a1u
#define CSR_TDATA2               0x7a2u
#define CSR_TINFO                0x7a4u
#define CSR_DCSR                 0x7b0u
#define DCSR_STEP                (1u << 2)
#define DCSR_PRV_MASK            3u
#define MISA_EXT_S               (1u << 18)
#define MISA_EXT_U               (1u << 20)
#define TRIGGER_TYPE_SHIFT       28
#define TRIGGER_TYPE_MCONTROL6   6u
#define TRIGGER_TYPE_DISABLED    15u
#define TRIGGER_MATCH_SHIFT      7
#define TRIGGER_MATCH_MASK       (0xfu << TRIGGER_MATCH_SHIFT)
#define TRIGGER_MATCH_NAPOT      1u
#define TRIGGER_M                (1u << 6)
#define TRIGGER_S                (1u << 4)
#define TRIGGER_U                (1u << 3)
#define TRIGGER_STORE            (1u << 1)
#define TRIGGER_OP_MASK          7u
#define ABSTRACT_WRITE           (1u << 16)

static uint32_t mock_time;
static uint32_t mock_idcode;
static uint32_t mock_dtmcs;
static uint32_t mock_dm_version;
static uint32_t mock_dmcontrol;
static uint32_t mock_data0;
static uint32_t mock_data1;
static uint32_t mock_sbcs;
static uint32_t mock_sbaddress;
static uint32_t mock_sbdata;
static uint32_t mock_sba_caps;
static uint8_t mock_datacount;
static uint8_t mock_memory[32];
static uint32_t mock_dcsr;
static uint32_t mock_misa;
static uint32_t mock_tselect;
static uint32_t mock_tdata1[2];
static uint32_t mock_tdata2[2];
static bool mock_halted;
static bool mock_resume_ack;
static bool mock_sbcs_busy;
static bool fail_dmstatus_read_once;
static bool fail_resume_write_after_apply_once;
static unsigned fail_dmcontrol_clear_writes;
static bool fail_disable_command;
static unsigned fail_dcsr_clear_commands;
static bool strip_s_mode_on_trigger_write;
static unsigned dmi_ops;
static unsigned sbcs_writes;
static unsigned sba8_reads;
static unsigned sba8_writes;
static unsigned sba32_reads;
static unsigned sba32_writes;
static unsigned abstract_byte_ops;
static unsigned resume_with_step_count;

void delay_us(uint32_t us)
{
    mock_time += us;
}

uint32_t hal_time_us(void)
{
    return ++mock_time;
}

void jtag_init(void) {}

uint32_t jtag_read_idcode(void)
{
    return mock_idcode;
}

uint32_t jtag_read_dtmcs(void)
{
    return mock_dtmcs;
}

void jtag_dmi_reset(void) {}

static uint32_t memory_read_word(uint32_t addr)
{
    uint32_t index = addr & 31u;
    return (uint32_t)mock_memory[index] |
           ((uint32_t)mock_memory[(index + 1u) & 31u] << 8) |
           ((uint32_t)mock_memory[(index + 2u) & 31u] << 16) |
           ((uint32_t)mock_memory[(index + 3u) & 31u] << 24);
}

static void memory_write_word(uint32_t addr, uint32_t value)
{
    uint32_t index = addr & 31u;
    mock_memory[index] = (uint8_t)value;
    mock_memory[(index + 1u) & 31u] = (uint8_t)(value >> 8);
    mock_memory[(index + 2u) & 31u] = (uint8_t)(value >> 16);
    mock_memory[(index + 3u) & 31u] = (uint8_t)(value >> 24);
}

static uint32_t csr_read(uint32_t csr)
{
    switch (csr) {
        case CSR_DCSR: return mock_dcsr;
        case CSR_MISA: return mock_misa;
        case CSR_TSELECT: return mock_tselect;
        case CSR_TDATA1: return mock_tdata1[mock_tselect];
        case CSR_TDATA2: return mock_tdata2[mock_tselect];
        case CSR_TINFO: return 1u << TRIGGER_TYPE_MCONTROL6;
        default: return 0;
    }
}

static void csr_write(uint32_t csr, uint32_t value)
{
    switch (csr) {
        case CSR_DCSR:
            mock_dcsr = value;
            break;
        case CSR_TSELECT:
            mock_tselect = value < 2u ? value : 1u;
            break;
        case CSR_TDATA1:
            if (strip_s_mode_on_trigger_write) value &= ~TRIGGER_S;
            mock_tdata1[mock_tselect] = value ? value :
                (TRIGGER_TYPE_DISABLED << TRIGGER_TYPE_SHIFT);
            break;
        case CSR_TDATA2:
            mock_tdata2[mock_tselect] = value;
            break;
        default:
            break;
    }
}

bool jtag_dmi_read(uint32_t addr, uint32_t *data)
{
    dmi_ops++;
    if (addr == DMI_DMSTATUS && fail_dmstatus_read_once) {
        fail_dmstatus_read_once = false;
        return false;
    }

    switch (addr) {
        case DMI_DMCONTROL:
            *data = mock_dmcontrol;
            break;
        case DMI_DMSTATUS:
            *data = mock_dm_version | DMSTATUS_AUTHENTICATED |
                    (mock_halted ? DMSTATUS_ALLHALTED : DMSTATUS_ALLRUNNING) |
                    (mock_resume_ack ? DMSTATUS_ALLRESUMEACK : 0u);
            break;
        case DMI_ABSTRACTCS:
            *data = mock_datacount; // not busy
            break;
        case DMI_DATA0:
            *data = mock_data0;
            break;
        case DMI_DATA1:
            *data = mock_data1;
            break;
        case DMI_SBDATA0:
            *data = mock_sbdata;
            break;
        case DMI_SBCS:
            *data = (1u << 29) | (32u << 5) | mock_sba_caps |
                    mock_sbcs |
                    (mock_sbcs_busy ? SBCS_SBBUSY : 0u);
            break;
        default:
            *data = 0;
            break;
    }
    return true;
}

bool jtag_dmi_write(uint32_t addr, uint32_t data)
{
    dmi_ops++;
    switch (addr) {
        case DMI_DMCONTROL:
            mock_dmcontrol = data;
            if (data & DMCONTROL_HALTREQ) mock_halted = true;
            if (data & DMCONTROL_RESUMEREQ) {
                mock_resume_ack = true;
                if (mock_dcsr & DCSR_STEP) {
                    resume_with_step_count++;
                    // Model stepping an mret: the new Debug Mode entry came
                    // from S-mode, while the debugger-owned step bit remains.
                    mock_dcsr = (mock_dcsr & ~DCSR_PRV_MASK) | 1u;
                    mock_halted = true;
                } else {
                    mock_halted = false;
                }
            }
            if ((data & DMCONTROL_RESUMEREQ) &&
                fail_resume_write_after_apply_once) {
                fail_resume_write_after_apply_once = false;
                return false;
            }
            if (data == DMCONTROL_DMACTIVE &&
                fail_dmcontrol_clear_writes != 0u) {
                fail_dmcontrol_clear_writes--;
                return false;
            }
            return true;
        case DMI_ABSTRACTCS:
            return true;
        case DMI_DATA0:
            mock_data0 = data;
            return true;
        case DMI_DATA1:
            mock_data1 = data;
            return true;
        case DMI_COMMAND: {
            uint32_t cmdtype = data >> 24;
            if (cmdtype == 2u) {
                uint32_t index = mock_data1 & 31u;
                if (data & ABSTRACT_WRITE) mock_memory[index] = (uint8_t)mock_data0;
                else mock_data0 = mock_memory[index];
                abstract_byte_ops++;
                return true;
            }
            uint32_t csr = data & 0xffffu;
            bool write = (data & ABSTRACT_WRITE) != 0;
            if (write && csr == CSR_DCSR && !(mock_data0 & DCSR_STEP) &&
                fail_dcsr_clear_commands > 0) {
                fail_dcsr_clear_commands--;
                return false;
            }
            if (write && csr == CSR_TDATA1 && mock_data0 == 0 &&
                fail_disable_command) {
                return false;
            }
            if (write) csr_write(csr, mock_data0);
            else mock_data0 = csr_read(csr);
            return true;
        }
        case DMI_SBCS:
            sbcs_writes++;
            mock_sbcs = data & (SBCS_READONADDR | (7u << SBCS_ACCESS_SHIFT));
            return true;
        case DMI_SBADDRESS0: {
            mock_sbaddress = data;
            uint32_t access = (mock_sbcs >> SBCS_ACCESS_SHIFT) & 7u;
            if (mock_sbcs & SBCS_READONADDR) {
                if (access == SBCS_ACCESS8) {
                    mock_sbdata = mock_memory[data & 31u];
                    sba8_reads++;
                } else if (access == SBCS_ACCESS32) {
                    mock_sbdata = memory_read_word(data);
                    sba32_reads++;
                }
            }
            return true;
        }
        case DMI_SBDATA0: {
            uint32_t access = (mock_sbcs >> SBCS_ACCESS_SHIFT) & 7u;
            if (access == SBCS_ACCESS8) {
                mock_memory[mock_sbaddress & 31u] = (uint8_t)data;
                sba8_writes++;
            } else if (access == SBCS_ACCESS32) {
                memory_write_word(mock_sbaddress, data);
                sba32_writes++;
            }
            return true;
        }
        default:
            return true;
    }
}

static void mock_reset(void)
{
    mock_time = 0;
    mock_idcode = 0x12345679u;
    mock_dtmcs = 1u | (7u << 4);
    mock_dm_version = 3u;
    mock_dmcontrol = 0;
    mock_data0 = 0;
    mock_data1 = 0;
    mock_sbcs = 0;
    mock_sbaddress = 0;
    mock_sbdata = 0;
    mock_sba_caps = SBCS_CAP_ACCESS8 | SBCS_CAP_ACCESS32;
    mock_datacount = 2u;
    for (uint32_t i = 0; i < 32u; i++) mock_memory[i] = (uint8_t)(0xa0u + i);
    mock_dcsr = 3u;
    mock_misa = MISA_EXT_S | MISA_EXT_U;
    mock_tselect = 0;
    mock_tdata1[0] = TRIGGER_TYPE_DISABLED << TRIGGER_TYPE_SHIFT;
    mock_tdata1[1] = TRIGGER_TYPE_DISABLED << TRIGGER_TYPE_SHIFT;
    mock_tdata2[0] = 0;
    mock_tdata2[1] = 0;
    mock_halted = true;
    mock_resume_ack = false;
    mock_sbcs_busy = false;
    fail_dmstatus_read_once = false;
    fail_resume_write_after_apply_once = false;
    fail_dmcontrol_clear_writes = 0u;
    fail_disable_command = false;
    fail_dcsr_clear_commands = 0;
    strip_s_mode_on_trigger_write = false;
    dmi_ops = 0;
    sbcs_writes = 0;
    sba8_reads = 0;
    sba8_writes = 0;
    sba32_reads = 0;
    sba32_writes = 0;
    abstract_byte_ops = 0;
    resume_with_step_count = 0;
}

static void test_version_validation(void)
{
    mock_dtmcs = 7u << 4;
    CHECK(!riscv_init()); // incompatible DTM 0.11
    mock_dtmcs = 2u | (7u << 4);
    CHECK(!riscv_init()); // reserved DTM version
    mock_dtmcs = 1u | (5u << 4);
    CHECK(!riscv_init()); // cannot address registers through 0x3c
    mock_dtmcs = 1u | (31u << 4);
    CHECK(!riscv_init()); // exceeds the local DMI scan buffer
    mock_dtmcs = 1u | (7u << 4);
    mock_dm_version = 15u;
    CHECK(!riscv_init()); // custom DM layout
    mock_dm_version = 3u;
    CHECK(riscv_init());
}

int main(void)
{
    mock_reset();
    test_version_validation();

    uint8_t byte = 0;
    unsigned before = dmi_ops;
    CHECK(!riscv_mem_read(UINT32_MAX, &byte, 2));
    CHECK(!riscv_mem_write(UINT32_MAX, &byte, 2));
    CHECK(dmi_ops == before);

    // Sub-word MMIO requests must stay sub-word: SBA8 is preferred and no
    // adjacent byte may be touched by a write.
    uint8_t neighbor0 = mock_memory[0];
    uint8_t neighbor2 = mock_memory[2];
    CHECK(riscv_mem_read(1u, &byte, 1u));
    CHECK(byte == 0xa1u);
    CHECK(sba8_reads == 1u && sba32_reads == 0u && abstract_byte_ops == 0u);
    byte = 0x5au;
    CHECK(riscv_mem_write(1u, &byte, 1u));
    CHECK(mock_memory[1] == 0x5au);
    CHECK(mock_memory[0] == neighbor0 && mock_memory[2] == neighbor2);
    CHECK(sba8_writes == 1u && sba32_writes == 0u);

    // Naturally aligned complete words use SBA32 exactly once.
    uint8_t word[4];
    CHECK(riscv_mem_read(4u, word, sizeof(word)));
    CHECK(sba32_reads == 1u);
    const uint8_t replacement[4] = {1u, 2u, 3u, 4u};
    CHECK(riscv_mem_write(4u, replacement, sizeof(replacement)));
    CHECK(sba32_writes == 1u);
    CHECK(mock_memory[4] == 1u && mock_memory[7] == 4u);

    // A stuck SBA is never "recovered" by writing sbcs while busy; it is
    // poisoned and the following request uses Abstract Memory Access.
    mock_sbcs_busy = true;
    before = sbcs_writes;
    CHECK(!riscv_mem_read(8u, word, sizeof(word)));
    CHECK(sbcs_writes == before); // never write sbcs while busy
    mock_sbcs_busy = false;
    before = abstract_byte_ops;
    CHECK(riscv_mem_read(8u, &byte, 1u));
    CHECK(abstract_byte_ops == before + 1u);

    // If SBA8 is absent, partial requests use abstract byte commands. If
    // neither byte mechanism exists, reject before touching the target.
    mock_sba_caps = SBCS_CAP_ACCESS32;
    mock_datacount = 2u;
    CHECK(riscv_init());
    before = abstract_byte_ops;
    byte = 0x66u;
    CHECK(riscv_mem_write(3u, &byte, 1u));
    CHECK(abstract_byte_ops == before + 1u && mock_memory[3] == 0x66u);

    mock_datacount = 1u;
    CHECK(riscv_init());
    before = dmi_ops;
    byte = 0x77u;
    CHECK(!riscv_mem_write(3u, &byte, 1u));
    CHECK(dmi_ops == before && mock_memory[3] == 0x66u);

    fail_dmstatus_read_once = true;
    CHECK(!riscv_halt());
    CHECK(mock_dmcontrol == DMCONTROL_DMACTIVE);

    // Resume may have reached the hart even when acknowledgment polling fails;
    // ordinary continue must restore a known halted state before returning E01.
    mock_halted = true;
    mock_resume_ack = false;
    fail_dmstatus_read_once = true;
    CHECK(!riscv_continue());
    CHECK(mock_halted);
    CHECK(mock_dmcontrol == DMCONTROL_DMACTIVE);

    mock_halted = true;
    mock_resume_ack = false;
    fail_resume_write_after_apply_once = true;
    CHECK(!riscv_continue());
    CHECK(mock_halted);
    CHECK(mock_dmcontrol == DMCONTROL_DMACTIVE);

    mock_halted = true;
    mock_resume_ack = false;
    fail_dmcontrol_clear_writes = 1u;
    CHECK(!riscv_continue());
    CHECK(mock_halted);
    CHECK(mock_dmcontrol == DMCONTROL_DMACTIVE);

    mock_dcsr = 3u;
    mock_halted = true;
    mock_resume_ack = false;
    CHECK(riscv_step());
    CHECK((mock_dcsr & DCSR_PRV_MASK) == 1u);
    CHECK((mock_dcsr & DCSR_STEP) == 0);

    // A transient cleanup failure stays sticky. The next ordinary continue
    // first confirms STEP clear and must not accidentally perform a step.
    mock_dcsr = 3u;
    mock_halted = true;
    mock_resume_ack = false;
    fail_dcsr_clear_commands = 1u;
    unsigned stepped_resumes = resume_with_step_count;
    CHECK(!riscv_step());
    CHECK((mock_dcsr & DCSR_STEP) != 0);
    CHECK(resume_with_step_count == stepped_resumes + 1u);
    mock_resume_ack = false;
    CHECK(riscv_continue());
    CHECK((mock_dcsr & DCSR_STEP) == 0);
    CHECK(resume_with_step_count == stepped_resumes + 1u);

    CHECK(riscv_halt());

    CHECK(riscv_watchpoint_insert(TARGET_WATCH_WRITE, 0x1000u, 4u));
    CHECK(mock_tdata2[0] == 0x1001u);
    CHECK(((mock_tdata1[0] & TRIGGER_MATCH_MASK) >>
            TRIGGER_MATCH_SHIFT) == TRIGGER_MATCH_NAPOT);
    CHECK((mock_tdata1[0] & (TRIGGER_M | TRIGGER_S | TRIGGER_U)) ==
           (TRIGGER_M | TRIGGER_S | TRIGGER_U));
    CHECK((mock_tdata1[0] & TRIGGER_OP_MASK) == TRIGGER_STORE);

    // Reattach without D/k must clear tracked hart trigger CSRs before it
    // resets the software capability cache.
    mock_halted = false;
    CHECK(riscv_init());
    CHECK(mock_halted);
    CHECK((mock_tdata1[0] >> TRIGGER_TYPE_SHIFT) == TRIGGER_TYPE_DISABLED);
    CHECK(riscv_watchpoint_insert(TARGET_WATCH_WRITE, 0x1000u, 4u));

    CHECK(!riscv_watchpoint_insert(TARGET_WATCH_WRITE, 0x1002u, 4u));
    CHECK(!riscv_watchpoint_insert(TARGET_WATCH_WRITE, 0x2000u, 3u));

    fail_disable_command = true;
    CHECK(!riscv_watchpoint_remove(TARGET_WATCH_WRITE, 0x1000u, 4u));
    fail_disable_command = false;
    CHECK(riscv_debug_resources_clear());
    CHECK((mock_tdata1[0] >> TRIGGER_TYPE_SHIFT) == TRIGGER_TYPE_DISABLED);

    strip_s_mode_on_trigger_write = true;
    CHECK(!riscv_breakpoint_insert(0x3000u));
    CHECK((mock_tdata1[0] >> TRIGGER_TYPE_SHIFT) == TRIGGER_TYPE_DISABLED);

    puts("riscv tests passed");
    return 0;
}
