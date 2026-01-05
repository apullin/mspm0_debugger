// ADIv5 DP/AP access over SWD

#include "adiv5.h"

#include <stdint.h>

#include "hal.h"
#include "swd_bitbang.h"

#define DP_IDCODE    0x00u // DP register addr[3:2]=0
#define DP_ABORT     0x00u // write only (same addr)
#define DP_CTRL_STAT 0x04u // addr[3:2]=1
#define DP_SELECT    0x08u // addr[3:2]=2
#define DP_RDBUFF    0x0Cu // addr[3:2]=3

static const uint32_t DP_ABORT_CLEAR_ERRORS =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);

static uint32_t g_dp_select = 0;

bool adiv5_dp_read(uint8_t addr, uint32_t *out)
{
    uint32_t v = 0;
    if (!swd_transfer(false, true, (uint8_t) (addr >> 2), &v)) {
        return false;
    }
    *out = v;
    return true;
}

bool adiv5_dp_write(uint8_t addr, uint32_t v)
{
    return swd_transfer(false, false, (uint8_t) (addr >> 2), &v);
}

static bool ap_select(uint8_t ap_sel, uint8_t bank_sel)
{
    uint32_t sel = ((uint32_t) ap_sel << 24) | ((uint32_t) bank_sel << 4);
    if (sel == g_dp_select) {
        return true;
    }
    if (!adiv5_dp_write(DP_SELECT, sel)) {
        return false;
    }
    g_dp_select = sel;
    return true;
}

bool adiv5_ap_write(uint8_t ap_sel, uint8_t addr, uint32_t v)
{
    uint8_t bank = (addr >> 4) & 0xF; // bank is A[7:4]
    if (!ap_select(ap_sel, bank)) {
        return false;
    }
    return swd_transfer(true, false, (uint8_t) (addr >> 2), &v);
}

bool adiv5_ap_read(uint8_t ap_sel, uint8_t addr, uint32_t *out)
{
    uint8_t bank = (addr >> 4) & 0xF;
    if (!ap_select(ap_sel, bank)) {
        return false;
    }

    // AP reads are posted: first read starts, second read returns prior. Use RDBUFF to fetch.
    uint32_t dummy = 0;
    if (!swd_transfer(true, true, (uint8_t) (addr >> 2), &dummy)) {
        return false;
    }

    return adiv5_dp_read(DP_RDBUFF, out);
}

bool adiv5_init(void)
{
    g_dp_select = 0xFFFFFFFFu;

    swd_jtag_to_swd();

    // Try read IDCODE to confirm link
    uint32_t id = 0;
    if (!adiv5_dp_read(DP_IDCODE, &id)) {
        return false;
    }

    // Clear errors and request debug power-up
    // ABORT: clear STKERR/STKCMP/STKORUN + WDERR/ORUN
    (void) adiv5_dp_write(DP_ABORT, DP_ABORT_CLEAR_ERRORS);

    // CTRL/STAT: set CDBGPWRUPREQ + CSYSPWRUPREQ
    // Bits: CDBGPWRUPREQ(28), CSYSPWRUPREQ(30)
    uint32_t req = (1u << 28) | (1u << 30);
    if (!adiv5_dp_write(DP_CTRL_STAT, req)) {
        return false;
    }

    // Optionally wait for ACK bits (CDBGPWRUPACK(29), CSYSPWRUPACK(31))
    for (int i = 0; i < 200; i++) {
        uint32_t cs = 0;
        if (adiv5_dp_read(DP_CTRL_STAT, &cs)) {
            if ((cs & (1u << 29)) && (cs & (1u << 31))) {
                break;
            }
        }
        delay_us(100);
    }

    return true;
}

void adiv5_clear_errors(void)
{
    (void) adiv5_dp_write(DP_ABORT, DP_ABORT_CLEAR_ERRORS);
}
