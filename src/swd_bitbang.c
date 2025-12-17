// SWD wire (bit-bang)
// SWD bit order: LSB-first for requests/data.
// Sampling: SWD samples on rising edge of SWCLK (common probe behavior).

#include "swd_bitbang.h"

#include <stdint.h>

#include "hal.h"

#ifndef SWD_DELAY_US
#define SWD_DELAY_US 0u
#endif

static inline void swd_delay(void)
{
    if (SWD_DELAY_US) {
        delay_us(SWD_DELAY_US);
    }
}

static void swd_clk_cycle(void)
{
    swclk_write(0);
    swd_delay();
    swclk_write(1);
    swd_delay();
}

static void swd_write_bit(int bit)
{
    swdio_write(bit ? 1 : 0);
    swd_clk_cycle();
}

static int swd_read_bit(void)
{
    swclk_write(0);
    swd_delay();
    swclk_write(1);
    int b = swdio_read() ? 1 : 0;
    swd_delay();
    return b;
}

static void swd_line_reset(void)
{
    // At least 50 cycles with SWDIO high
    swdio_dir_out();
    swdio_write(1);
    for (int i = 0; i < 60; i++) {
        swd_clk_cycle();
    }
}

void swd_jtag_to_swd(void)
{
    // Standard JTAG->SWD 16-bit sequence: 0xE79E (LSB-first transmit)
    // Followed by line reset and idle cycles.
    swd_line_reset();

    const uint16_t seq = 0xE79E;
    swdio_dir_out();
    for (int i = 0; i < 16; i++) {
        swd_write_bit((seq >> i) & 1u);
    }

    swd_line_reset();

    // Idle (at least 2 cycles)
    swdio_write(1);
    swd_clk_cycle();
    swd_clk_cycle();
}

static uint8_t parity_u32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xF;
    return (uint8_t) ((0x6996 >> v) & 1u);
}

static uint8_t parity_u8_4(uint8_t v)
{
    // parity of low 4 bits (for request header parity)
    v &= 0xF;
    v ^= v >> 2;
    v ^= v >> 1;
    return (uint8_t) (v & 1u);
}

typedef enum {
    SWD_ACK_OK    = 0b001,
    SWD_ACK_WAIT  = 0b010,
    SWD_ACK_FAULT = 0b100,
} swd_ack_t;

static swd_ack_t swd_read_ack(void)
{
    int a0 = swd_read_bit();
    int a1 = swd_read_bit();
    int a2 = swd_read_bit();
    return (swd_ack_t) ((a2 << 2) | (a1 << 1) | a0);
}

static void swd_turnaround_to_read(void)
{
    // 1 turnaround cycle where master releases SWDIO
    swdio_dir_in();
    swd_clk_cycle();
}

static void swd_turnaround_to_write(void)
{
    // 1 turnaround cycle (target releases, master takes)
    swd_clk_cycle();
    swdio_dir_out();
}

static bool swd_read_u32(uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) {
        v |= ((uint32_t) swd_read_bit() << i);
    }
    int p = swd_read_bit();
    if ((parity_u32(v) & 1u) != (uint8_t) p) {
        return false;
    }

    // Idle cycle (master drives 1)
    swd_turnaround_to_write();
    swdio_write(1);
    swd_clk_cycle();

    *out = v;
    return true;
}

static void swd_write_u32(uint32_t v)
{
    for (int i = 0; i < 32; i++) {
        swd_write_bit((v >> i) & 1u);
    }
    swd_write_bit(parity_u32(v) & 1u);

    // Idle cycle
    swdio_write(1);
    swd_clk_cycle();
}

bool swd_transfer(bool ap, bool rnw, uint8_t addr2, uint32_t *data_inout)
{
    // Build 8-bit request (LSB-first):
    // start(1), APnDP, RnW, A2, A3, parity, stop(0), park(1)
    // addr2 is bits [3:2] of the register address (i.e., A[3:2])
    uint8_t a2 = (addr2 >> 0) & 1u;
    uint8_t a3 = (addr2 >> 1) & 1u;
    uint8_t hdr = (1u << 0) | ((ap ? 1u : 0u) << 1) | ((rnw ? 1u : 0u) << 2) |
                  (a2 << 3) | (a3 << 4);
    uint8_t p = parity_u8_4((hdr >> 1) & 0xF); // parity over APnDP,RnW,A2,A3

    // Assemble full request bits
    uint8_t req[8] = {1, ap ? 1 : 0, rnw ? 1 : 0, a2, a3, p, 0, 1};

    // Send request
    swdio_dir_out();
    for (int i = 0; i < 8; i++) {
        swd_write_bit(req[i]);
    }

    // Turnaround + read ACK
    swd_turnaround_to_read();
    swd_ack_t ack = swd_read_ack();

    if (ack == SWD_ACK_WAIT) {
        // Leave bus idle cleanly (turnaround back to write + idle)
        swd_turnaround_to_write();
        swdio_write(1);
        swd_clk_cycle();
        return false;
    }
    if (ack != SWD_ACK_OK) {
        // Fault or protocol error
        swd_turnaround_to_write();
        swdio_write(1);
        swd_clk_cycle();
        return false;
    }

    if (rnw) {
        // Read data phase (target drives)
        uint32_t v;
        bool ok = swd_read_u32(&v);
        if (!ok) {
            return false;
        }
        *data_inout = v;
        return true;
    }

    // Turnaround to write then write data
    swd_turnaround_to_write();
    swd_write_u32(*data_inout);
    return true;
}

