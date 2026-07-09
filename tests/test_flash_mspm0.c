/* Register-level host tests for the real MSPM0 FLASHCTL driver.
 *
 * Standalone build (kept independent of the RSP mocks):
 *   cc -std=c11 -Wall -Wextra -Werror -g -O1 \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -DPROBE_ENABLE_CORTEXM=1 -DPROBE_ENABLE_FLASH_MSPM0=1 \
 *      -Iinclude -I. src/flash_mspm0.c tests/test_flash_mspm0.c \
 *      -o /tmp/flash_mspm0_tests && /tmp/flash_mspm0_tests
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_mspm0.h"

#define FLASHCTL_BASE       0x400CD000u
#define FLASHCTL_CMDEXEC    (FLASHCTL_BASE + 0x1100u)
#define FLASHCTL_CMDTYPE    (FLASHCTL_BASE + 0x1104u)
#define FLASHCTL_CMDCTL     (FLASHCTL_BASE + 0x1108u)
#define FLASHCTL_CMDADDR    (FLASHCTL_BASE + 0x1120u)
#define FLASHCTL_CMDBYTEN   (FLASHCTL_BASE + 0x1124u)
#define FLASHCTL_CMDDATA0   (FLASHCTL_BASE + 0x1130u)
#define FLASHCTL_CMDDATA1   (FLASHCTL_BASE + 0x1134u)
#define FLASHCTL_CMDDATA2   (FLASHCTL_BASE + 0x1138u)
#define FLASHCTL_CMDDATA3   (FLASHCTL_BASE + 0x113Cu)
#define FLASHCTL_CMDWEPROTA (FLASHCTL_BASE + 0x11D0u)
#define FLASHCTL_CMDWEPROTB (FLASHCTL_BASE + 0x11D4u)
#define FLASHCTL_CMDWEPROTC (FLASHCTL_BASE + 0x11D8u)
#define FLASHCTL_STATCMD    (FLASHCTL_BASE + 0x13D0u)
#define FLASHCTL_GBLINFO0   (FLASHCTL_BASE + 0x13F0u)
#define FLASHCTL_GBLINFO1   (FLASHCTL_BASE + 0x13F4u)
#define FLASHCTL_BANK0INFO0 (FLASHCTL_BASE + 0x1400u)

#define CMDTYPE_PROGRAM     0x01u
#define CMDTYPE_ERASE       0x02u
#define CMDTYPE_CLEARSTATUS 0x05u
#define CMDTYPE_SIZE_SECTOR 0x40u

#define STATCMD_CMDDONE       0x01u
#define STATCMD_CMDPASS       0x02u
#define STATCMD_CMDINPROGRESS 0x04u

#define CMDCTL_DATAVEREN          0x00200000u
#define CMDBYTEN_WORD64_WITH_ECC  0x000001FFu
#define CMDBYTEN_WORD128_WITH_ECC 0x0001FFFFu

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    uint32_t type;
    uint32_t ctl;
    uint32_t addr;
    uint32_t byten;
    uint32_t data0;
    uint32_t data1;
    uint32_t data2;
    uint32_t data3;
    uint32_t weprot_a;
    uint32_t weprot_b;
    uint32_t weprot_c;
} command_t;

static uint32_t mock_gblinfo0;
static uint32_t mock_gblinfo1;
static uint32_t mock_bankinfo[5];
static uint32_t mock_cmdtype;
static uint32_t mock_cmdctl;
static uint32_t mock_cmdaddr;
static uint32_t mock_byten;
static uint32_t mock_data0;
static uint32_t mock_data1;
static uint32_t mock_data2;
static uint32_t mock_data3;
static uint32_t mock_weprot_a;
static uint32_t mock_weprot_b;
static uint32_t mock_weprot_c;
static uint32_t mock_time;
static uint32_t mock_weprotc_writes;
static uint32_t mock_clear_execs;
static uint32_t mock_status_reads;
static unsigned mock_poll_kind;
static unsigned mock_polls_left;
static bool mock_fail_next_command;
static command_t mock_commands[32];
static size_t mock_command_count;

static int tests_run;
static int tests_failed;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                 \
                    __FILE__, __LINE__, #expr);                             \
            tests_failed++;                                                \
            return;                                                        \
        }                                                                   \
    } while (0)

static void begin_test(const char *name)
{
    tests_run++;
    printf("[TEST] %s\n", name);
}

static void mock_reset(void)
{
    memset(mock_bankinfo, 0, sizeof(mock_bankinfo));
    memset(mock_commands, 0, sizeof(mock_commands));
    mock_gblinfo0        = (1u << 16) | 1024u;
    mock_gblinfo1        = 64u;
    mock_bankinfo[0]     = 32u;
    mock_cmdtype         = 0u;
    mock_cmdctl          = 0u;
    mock_cmdaddr         = 0u;
    mock_byten           = 0u;
    mock_data0           = 0u;
    mock_data1           = 0u;
    mock_data2           = 0u;
    mock_data3           = 0u;
    mock_weprot_a        = UINT32_MAX;
    mock_weprot_b        = UINT32_MAX;
    mock_weprot_c        = UINT32_MAX;
    mock_time            = 0u;
    mock_weprotc_writes  = 0u;
    mock_clear_execs     = 0u;
    mock_status_reads    = 0u;
    mock_poll_kind       = 0u;
    mock_polls_left      = 0u;
    mock_fail_next_command = false;
    mock_command_count   = 0u;
    flash_mspm0_abort();
}

uint32_t hal_time_us(void)
{
    return mock_time++;
}

bool target_mem_read_word(uint32_t addr, uint32_t *out)
{
    if (!out) {
        return false;
    }
    if (addr == FLASHCTL_GBLINFO0) {
        *out = mock_gblinfo0;
        return true;
    }
    if (addr == FLASHCTL_GBLINFO1) {
        *out = mock_gblinfo1;
        return true;
    }
    if (addr >= FLASHCTL_BANK0INFO0 &&
        addr <= FLASHCTL_BANK0INFO0 + 4u * 0x10u &&
        ((addr - FLASHCTL_BANK0INFO0) % 0x10u) == 0u) {
        *out = mock_bankinfo[(addr - FLASHCTL_BANK0INFO0) / 0x10u];
        return true;
    }
    if (addr == FLASHCTL_STATCMD) {
        mock_status_reads++;
        if (mock_poll_kind == CMDTYPE_CLEARSTATUS) {
            if (mock_polls_left > 0u) {
                mock_polls_left--;
                *out = STATCMD_CMDINPROGRESS;
            } else {
                // TI semantics: CLEARSTATUS completes with STATCMD clear,
                // not with CMDDONE asserted.
                *out = 0u;
            }
            return true;
        }
        if (mock_poll_kind != 0u) {
            if (mock_polls_left > 0u) {
                mock_polls_left--;
                *out = STATCMD_CMDINPROGRESS;
            } else {
                *out = STATCMD_CMDDONE |
                       (mock_fail_next_command ? 0u : STATCMD_CMDPASS);
            }
            return true;
        }
        *out = 0u;
        return true;
    }
    return false;
}

static void mock_capture_command(void)
{
    if (mock_command_count >= ARRAY_LEN(mock_commands)) {
        fprintf(stderr, "mock command log overflow\n");
        abort();
    }
    command_t *cmd = &mock_commands[mock_command_count++];
    cmd->type      = mock_cmdtype;
    cmd->ctl       = mock_cmdctl;
    cmd->addr      = mock_cmdaddr;
    cmd->byten     = mock_byten;
    cmd->data0     = mock_data0;
    cmd->data1     = mock_data1;
    cmd->data2     = mock_data2;
    cmd->data3     = mock_data3;
    cmd->weprot_a  = mock_weprot_a;
    cmd->weprot_b  = mock_weprot_b;
    cmd->weprot_c  = mock_weprot_c;
}

bool target_mem_write_word(uint32_t addr, uint32_t value)
{
    switch (addr) {
    case FLASHCTL_CMDTYPE:    mock_cmdtype = value; return true;
    case FLASHCTL_CMDCTL:     mock_cmdctl = value; return true;
    case FLASHCTL_CMDADDR:    mock_cmdaddr = value; return true;
    case FLASHCTL_CMDBYTEN:   mock_byten = value; return true;
    case FLASHCTL_CMDDATA0:   mock_data0 = value; return true;
    case FLASHCTL_CMDDATA1:   mock_data1 = value; return true;
    case FLASHCTL_CMDDATA2:   mock_data2 = value; return true;
    case FLASHCTL_CMDDATA3:   mock_data3 = value; return true;
    case FLASHCTL_CMDWEPROTA: mock_weprot_a = value; return true;
    case FLASHCTL_CMDWEPROTB: mock_weprot_b = value; return true;
    case FLASHCTL_CMDWEPROTC:
        mock_weprot_c = value;
        mock_weprotc_writes++;
        return true;
    case FLASHCTL_CMDEXEC:
        if (value != 1u) {
            return false;
        }
        if ((mock_cmdtype & 0xFu) == CMDTYPE_CLEARSTATUS) {
            mock_clear_execs++;
            mock_poll_kind  = CMDTYPE_CLEARSTATUS;
            mock_polls_left = 2u;
            // CLEARSTATUS re-applies maximum write/erase protection.
            mock_weprot_a = UINT32_MAX;
            mock_weprot_b = UINT32_MAX;
            mock_weprot_c = UINT32_MAX;
        } else {
            mock_capture_command();
            mock_poll_kind  = mock_cmdtype & 0xFu;
            mock_polls_left = 2u;
        }
        return true;
    default:
        return false;
    }
}

static void test_geometry_uses_all_banks(void)
{
    begin_test("geometry sums every instantiated bank");
    mock_reset();
    mock_gblinfo0    = (3u << 16) | 1024u;
    mock_bankinfo[0] = 32u;
    mock_bankinfo[1] = 64u;
    mock_bankinfo[2] = 128u;

    uint32_t size = 0u, sector = 0u;
    CHECK(flash_mspm0_geometry(&size, &sector));
    CHECK(sector == 1024u);
    CHECK(size == (32u + 64u + 128u) * 1024u);

    mock_gblinfo0 = 1024u; // zero banks is invalid
    CHECK(!flash_mspm0_geometry(&size, &sector));
    mock_gblinfo0 = (1u << 16) | 768u; // masking relies on power-of-two size
    CHECK(!flash_mspm0_geometry(&size, &sector));
    mock_gblinfo0    = (2u << 16) | 1024u;
    mock_bankinfo[1] = 0u;
    CHECK(!flash_mspm0_geometry(&size, &sector));
}

static void test_erase_clear_status_and_protection_c(void)
{
    begin_test("erase follows CLEARSTATUS semantics and unlocks WEPROTC");
    mock_reset();
    mock_bankinfo[0] = 512u;

    CHECK(flash_mspm0_erase_range(256u * 1024u, 1024u));
    CHECK(mock_clear_execs == 1u);
    CHECK(mock_status_reads >= 6u);
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].type == (CMDTYPE_ERASE | CMDTYPE_SIZE_SECTOR));
    CHECK(mock_commands[0].addr == 256u * 1024u);
    CHECK(mock_commands[0].weprot_a == 0u);
    CHECK(mock_commands[0].weprot_b == 0u);
    CHECK(mock_commands[0].weprot_c == 0u);
    CHECK(mock_weprotc_writes == 1u);
}

static void test_erase_range_validation(void)
{
    begin_test("erase rejects out-of-range and wrapping requests");
    mock_reset();
    const uint32_t main_size = 32u * 1024u;

    CHECK(!flash_mspm0_erase_range(main_size, 1u));
    CHECK(mock_command_count == 0u);
    CHECK(!flash_mspm0_erase_range(main_size - 8u, 9u));
    CHECK(mock_command_count == 0u);
    CHECK(!flash_mspm0_erase_range(UINT32_MAX - 7u, 16u));
    CHECK(mock_command_count == 0u);

    CHECK(flash_mspm0_erase_range(1023u, 2u));
    CHECK(mock_command_count == 2u);
    CHECK(mock_commands[0].addr == 0u);
    CHECK(mock_commands[1].addr == 1024u);
}

static void test_partial_done_uses_one_ecc_program(void)
{
    begin_test("vFlashDone flushes one partial word with generated ECC");
    mock_reset();
    const uint8_t data[] = {0xAAu, 0xBBu, 0xCCu};

    CHECK(flash_mspm0_write(2u, data, sizeof(data)));
    CHECK(mock_command_count == 0u);
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].type == CMDTYPE_PROGRAM);
    CHECK(mock_commands[0].ctl == CMDCTL_DATAVEREN);
    CHECK(mock_commands[0].addr == 0u);
    CHECK(mock_commands[0].byten == CMDBYTEN_WORD64_WITH_ECC);
    CHECK(mock_commands[0].data0 == 0xBBAAFFFFu);
    CHECK(mock_commands[0].data1 == 0xFFFFFFCCu);
    CHECK(mock_commands[0].weprot_c == 0u);
    CHECK(mock_clear_execs == 1u);

    CHECK(flash_mspm0_done()); // idempotent
    CHECK(mock_command_count == 1u);
    CHECK(!flash_mspm0_write(8u, data, 1u)); // sealed until erase/abort
}

static void test_split_packets_program_once(void)
{
    begin_test("split packets coalesce into one flash-word command");
    mock_reset();
    const uint8_t first[]  = {0u, 1u, 2u};
    const uint8_t second[] = {3u, 4u, 5u, 6u, 7u};

    CHECK(flash_mspm0_write(0u, first, sizeof(first)));
    CHECK(flash_mspm0_write(3u, second, sizeof(second)));
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].data0 == 0x03020100u);
    CHECK(mock_commands[0].data1 == 0x07060504u);
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 1u);
}

static void test_out_of_order_and_overlap_rejected(void)
{
    begin_test("committed-word revisits and staged overlap are rejected");
    mock_reset();
    const uint8_t full[8] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    const uint8_t one = 0xA5u;

    CHECK(flash_mspm0_write(8u, full, sizeof(full)));
    CHECK(mock_command_count == 1u);
    CHECK(!flash_mspm0_write(4u, &one, 1u));
    CHECK(mock_command_count == 1u);

    flash_mspm0_abort();
    CHECK(flash_mspm0_write(2u, full, 2u));
    CHECK(!flash_mspm0_write(3u, &one, 1u));
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 1u); // overlapping staged word was discarded

    flash_mspm0_abort();
    CHECK(flash_mspm0_write(4u, &one, 1u));
    CHECK(!flash_mspm0_write(0u, &one, 1u));
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 1u); // out-of-order staged word was discarded
}

static void test_jump_flushes_each_word_once(void)
{
    begin_test("forward address jumps flush partial words exactly once");
    mock_reset();
    const uint8_t a = 0x11u, b = 0x22u;

    CHECK(flash_mspm0_write(0u, &a, 1u));
    CHECK(flash_mspm0_write(16u, &b, 1u));
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].addr == 0u);
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 2u);
    CHECK(mock_commands[1].addr == 16u);
}

static void test_write_range_validation(void)
{
    begin_test("write validates pointer, geometry, end, and overflow");
    mock_reset();
    const uint8_t data[16] = {0u};
    const uint32_t main_size = 32u * 1024u;

    CHECK(!flash_mspm0_write(0u, NULL, 1u));
    CHECK(!flash_mspm0_write(main_size, data, 1u));
    CHECK(!flash_mspm0_write(main_size - 4u, data, 5u));
    CHECK(!flash_mspm0_write(UINT32_MAX - 7u, data, 16u));
    CHECK(mock_command_count == 0u);
    CHECK(flash_mspm0_write(main_size, NULL, 0u));
}

static void test_multibank_upper_range_is_writable(void)
{
    begin_test("summed multi-bank geometry admits the upper bank");
    mock_reset();
    mock_gblinfo0    = (2u << 16) | 1024u;
    mock_bankinfo[0] = 32u;
    mock_bankinfo[1] = 32u;
    const uint8_t word[8] = {0u};

    CHECK(flash_mspm0_write(48u * 1024u, word, sizeof(word)));
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].addr == 48u * 1024u);
    CHECK(flash_mspm0_done());
}

static void test_128_bit_flash_word(void)
{
    begin_test("128-bit FLASHCTL target is staged and programmed atomically");
    mock_reset();
    mock_gblinfo1 = 128u;
    const uint8_t first[] = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u};
    const uint8_t last[]  = {0x16u, 0x17u, 0x18u, 0x19u, 0x1Au, 0x1Bu,
                             0x1Cu, 0x1Du, 0x1Eu, 0x1Fu};

    CHECK(flash_mspm0_write(0u, first, sizeof(first)));
    CHECK(mock_command_count == 0u);
    CHECK(flash_mspm0_write(6u, last, sizeof(last)));
    CHECK(mock_command_count == 1u);
    CHECK(mock_commands[0].type == 0x11u); // PROGRAM | SIZE_TWOWORD
    CHECK(mock_commands[0].byten == CMDBYTEN_WORD128_WITH_ECC);
    CHECK(mock_commands[0].data0 == 0x13121110u);
    CHECK(mock_commands[0].data1 == 0x17161514u);
    CHECK(mock_commands[0].data2 == 0x1B1A1918u);
    CHECK(mock_commands[0].data3 == 0x1F1E1D1Cu);
    CHECK(flash_mspm0_done());
}

static void test_command_failure_is_reported(void)
{
    begin_test("FLASHCTL CMDPASS failure propagates through vFlashDone");
    mock_reset();
    const uint8_t byte = 0x5Au;
    mock_fail_next_command = true;

    CHECK(flash_mspm0_write(0u, &byte, 1u));
    CHECK(!flash_mspm0_done());
    CHECK(mock_command_count == 1u);

    // Failure aborts the staged transaction; a later Done is empty rather
    // than issuing an unsafe retry of a command with unknown flash effects.
    mock_fail_next_command = false;
    CHECK(flash_mspm0_done());
    CHECK(mock_command_count == 1u);
}

int main(void)
{
    test_geometry_uses_all_banks();
    test_erase_clear_status_and_protection_c();
    test_erase_range_validation();
    test_partial_done_uses_one_ecc_program();
    test_split_packets_program_once();
    test_out_of_order_and_overlap_rejected();
    test_jump_flushes_each_word_once();
    test_write_range_validation();
    test_multibank_upper_range_is_writable();
    test_128_bit_flash_word();
    test_command_failure_is_reported();

    if (tests_failed != 0) {
        fprintf(stderr, "%d/%d flash tests failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("All %d flash tests passed\n", tests_run);
    return 0;
}
