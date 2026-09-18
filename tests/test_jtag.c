// Wire-level TAP/DTM model: production jtag_bitbang.c drives only HAL pins.
// In particular, dmireset clears sticky status without cancelling a request.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hal.h"
#include "jtag.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)

enum tap_state {
    RESET, IDLE, SELECT_DR, CAPTURE_DR, SHIFT_DR, EXIT1_DR, PAUSE_DR,
    EXIT2_DR, UPDATE_DR, SELECT_IR, CAPTURE_IR, SHIFT_IR, EXIT1_IR,
    PAUSE_IR, EXIT2_IR, UPDATE_IR,
};
// Independent target-side IEEE 1149.1 state machine.
static const enum tap_state next_state[][2] = {
    {IDLE, RESET}, {IDLE, SELECT_DR}, {CAPTURE_DR, SELECT_IR},
    {SHIFT_DR, EXIT1_DR}, {SHIFT_DR, EXIT1_DR}, {PAUSE_DR, UPDATE_DR},
    {PAUSE_DR, EXIT2_DR}, {SHIFT_DR, UPDATE_DR}, {IDLE, SELECT_DR},
    {CAPTURE_IR, RESET}, {SHIFT_IR, EXIT1_IR}, {SHIFT_IR, EXIT1_IR},
    {PAUSE_IR, UPDATE_IR}, {PAUSE_IR, EXIT2_IR}, {SHIFT_IR, UPDATE_IR},
    {IDLE, SELECT_DR},
};

static enum tap_state state;
static int tms, tdi, tdo, last_clock;
static uint32_t instruction, dtmcs;
static uint64_t input_bits, output_bits;
static unsigned bit_index, resets, dmi_scans, clocks;
static bool pending, complete_while_sticky, fail_next_request, pending_failure;
static unsigned busy_remaining, busy_next_request;
static uint8_t sticky, pending_op;
static uint32_t pending_data, result_data, device_value, last_addr;
static unsigned accepted_reads, accepted_writes, completed_reads, completed_writes;

static void complete_request(void)
{
    CHECK(pending);
    pending = false;
    if (pending_failure) {
        sticky = 2u;
    } else if (pending_op == 1u) {
        result_data = device_value++; // A side-effectful read, like a FIFO.
        completed_reads++;
    } else {
        device_value = pending_data;
        completed_writes++;
    }
}

static void capture_dr(void)
{
    if (instruction == JTAG_IR_DTMCS) output_bits = dtmcs;
    else if (instruction == JTAG_IR_IDCODE) output_bits = 0x12345679u;
    else {
        CHECK(instruction == JTAG_IR_DMI);
        dmi_scans++;
        if (!sticky && pending) {
            if (busy_remaining) {
                busy_remaining--;
                sticky = 3u;
                if (!busy_remaining && complete_while_sticky) complete_request();
            } else complete_request();
        }
        output_bits = ((uint64_t) result_data << 2) | sticky;
    }
}

static void update_dr(void)
{
    if (instruction != JTAG_IR_DMI) {
        CHECK(bit_index == 32u);
        if (instruction == JTAG_IR_DTMCS && (input_bits & (1u << 16))) {
            sticky = 0u;
            resets++;
        }
        return;
    }
    CHECK(bit_index == 34u + ((dtmcs >> 4) & 0x3Fu));
    uint8_t op = input_bits & 3u;
    if (sticky || op == 0u) return;
    CHECK(op == 1u || op == 2u);
    CHECK(!pending); // A new request may not replace an unfinished one.
    pending = true;
    pending_op = op;
    pending_data = (uint32_t) (input_bits >> 2);
    last_addr = (uint32_t) (input_bits >> 34);
    pending_failure = fail_next_request;
    fail_next_request = false;
    busy_remaining = busy_next_request;
    busy_next_request = 0u;
    if (op == 1u) accepted_reads++;
    else accepted_writes++;
}

void jtag_tms_write(int v) { tms = !!v; }
void jtag_tdi_write(int v) { tdi = !!v; }
int jtag_tdo_read(void) { return tdo; }
void delay_us(uint32_t us) { (void) us; }

void jtag_tck_write(int v)
{
    if (v && !last_clock) {
        clocks++;
        if (state == SHIFT_IR || state == SHIFT_DR) {
            CHECK(bit_index < 64u);
            tdo = (int) ((output_bits >> bit_index) & 1u);
            input_bits |= (uint64_t) tdi << bit_index++;
        }
        state = next_state[state][tms];
        if (state == RESET) instruction = JTAG_IR_IDCODE;
        if (state == CAPTURE_IR || state == CAPTURE_DR) {
            input_bits = 0u;
            bit_index = 0u;
            if (state == CAPTURE_IR) output_bits = 1u;
            else capture_dr();
        }
        if (state == UPDATE_IR) {
            CHECK(bit_index == PROBE_RISCV_JTAG_IR_LEN);
            CHECK(input_bits <= 0x1Fu); // Upper IR bits must be zero-extended.
            instruction = (uint32_t) input_bits;
        }
        if (state == UPDATE_DR) update_dr();
    }
    last_clock = v;
}

static void model_reset(void)
{
    state = RESET;
    tms = tdi = tdo = last_clock = 0;
    instruction = JTAG_IR_IDCODE;
    dtmcs = 1u | (7u << 4) | (2u << 12);
    input_bits = output_bits = 0u;
    bit_index = resets = dmi_scans = clocks = 0u;
    pending = complete_while_sticky = fail_next_request = pending_failure = false;
    busy_remaining = busy_next_request = 0u;
    sticky = pending_op = 0u;
    pending_data = result_data = last_addr = 0u;
    device_value = 0x89ABCDEFu;
    accepted_reads = accepted_writes = completed_reads = completed_writes = 0u;
    jtag_init();
    CHECK(jtag_read_idcode() == 0x12345679u);
    CHECK(jtag_read_dtmcs() == dtmcs);
}

static void test_busy_does_not_repeat_operation(void)
{
    for (unsigned completion = 0; completion < 2u; completion++) {
        for (unsigned busy = 0; busy < 6u; busy++) {
            model_reset();
            complete_while_sticky = completion != 0u;
            busy_next_request = busy;
            CHECK(jtag_dmi_write(0x3Cu, 0xFEDCBA98u));
            CHECK(accepted_writes == 1u && completed_writes == 1u);
            CHECK(device_value == 0xFEDCBA98u && last_addr == 0x3Cu);
            CHECK(resets == busy);

            busy_next_request = busy;
            uint32_t data = 0u;
            CHECK(jtag_dmi_read(0x3Cu, &data));
            CHECK(data == 0xFEDCBA98u);
            CHECK(accepted_reads == 1u && completed_reads == 1u);
            CHECK(device_value == 0xFEDCBA99u);
            CHECK(resets == 2u * busy);
        }
    }
}

static void test_timeout_and_next_request_admission(void)
{
    model_reset();
    busy_next_request = 100u;
    uint32_t data = 0x11223344u;
    CHECK(!jtag_dmi_read(0x3Cu, &data));
    CHECK(data == 0x11223344u); // No invalid output on failure.
    CHECK(accepted_reads == 1u && completed_reads == 0u && pending);
    CHECK(dmi_scans <= 16u); // Bounded retries.
    // The timed-out request can still finish; an admission scan encountering
    // its BUSY must discard the new request, not collect the old result as new.
    busy_remaining = 2u;
    CHECK(jtag_dmi_read(0x11u, &data));
    CHECK(data == 0x89ABCDF0u);
    CHECK(accepted_reads == 2u && completed_reads == 2u);
    CHECK(last_addr == 0x11u);
}

static void test_failed_and_invalid_requests(void)
{
    model_reset();
    fail_next_request = true;
    uint32_t data = 0x11223344u;
    CHECK(!jtag_dmi_read(0x3Cu, &data));
    CHECK(data == 0x11223344u);
    CHECK(accepted_reads == 1u && completed_reads == 0u);
    CHECK(resets == 1u && !sticky);
    CHECK(jtag_dmi_write(0x3Cu, 0x55AA55AAu));
    CHECK(completed_writes == 1u);

    unsigned before = clocks;
    CHECK(!jtag_dmi_read(0u, NULL));
    CHECK(!jtag_dmi_write(0x80u, 0u));
    CHECK(clocks == before);
    const uint32_t invalid[] = {7u << 4, 2u | (7u << 4), 15u | (7u << 4),
                                1u, 1u | (31u << 4)};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        dtmcs = invalid[i];
        CHECK(jtag_read_dtmcs() == dtmcs);
        before = clocks;
        CHECK(!jtag_dmi_read(0u, &data));
        CHECK(!jtag_dmi_write(0u, 0u));
        jtag_dmi_reset();
        CHECK(clocks == before);
    }
}

static void test_largest_dmi_scan(void)
{
    model_reset();
    dtmcs = 1u | (30u << 4);
    CHECK(jtag_read_dtmcs() == dtmcs);
    CHECK(jtag_dmi_write(0x3FFFFFFFu, 0xFFFFFFFFu));
    CHECK(last_addr == 0x3FFFFFFFu && device_value == 0xFFFFFFFFu);
    uint32_t data = 0u;
    CHECK(jtag_dmi_read(0x20000001u, &data));
    CHECK(last_addr == 0x20000001u && data == 0xFFFFFFFFu);
    CHECK(!jtag_dmi_write(0x40000000u, 0u));
}

int main(void)
{
    test_busy_does_not_repeat_operation();
    test_timeout_and_next_request_admission();
    test_failed_and_invalid_requests();
    test_largest_dmi_scan();
    puts("JTAG wire-level tests passed");
    return 0;
}
