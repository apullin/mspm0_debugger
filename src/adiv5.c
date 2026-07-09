// ADIv5/ADIv6 DP/AP access over SWD

#include "adiv5.h"

#include <stdint.h>

#include "hal.h"
#include "swd_bitbang.h"

#define DP_IDCODE    0x00u // DP register addr[3:2]=0
#define DP_ABORT     0x00u // write only (same addr)
#define DP_CTRL_STAT 0x04u // addr[3:2]=1
#define DP_SELECT    0x08u // addr[3:2]=2
#define DP_RDBUFF    0x0Cu // addr[3:2]=3

// ABORT clear bits: STKCMPCLR(1) STKERRCLR(2) WDERRCLR(3) ORUNERRCLR(4).
// Bit 0 is DAPABORT ("abort the current AP transaction") — NOT an error
// clear; it is only used when an AP is wedged in WAIT.
#define DP_ABORT_DAPABORT (1u << 0)
static const uint32_t DP_ABORT_CLEAR_ERRORS =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);

#define SWD_WAIT_RETRIES 100u

static uint32_t g_dp_select = 0;

// Retry the same transfer while the target answers WAIT (ADIv5 requires the
// host to retry; a busy AP is routine during posted reads and slow memories).
static swd_xfer_status_t swd_xfer_retry(bool ap, bool rnw, uint8_t addr2, uint32_t *v)
{
    swd_xfer_status_t st = SWD_XFER_WAIT;
    for (uint32_t i = 0; i < SWD_WAIT_RETRIES; i++) {
        st = swd_transfer(ap, rnw, addr2, v);
        if (st != SWD_XFER_WAIT) {
            break;
        }
    }
    return st;
}

static void resync_link(void)
{
    // Line reset leaves SELECT.DPBANKSEL unknown and the DP requires a DPIDR
    // read before any other request.
    swd_resync();
    uint32_t id = 0;
    (void) swd_transfer(false, true, 0, &id); // DPIDR
    g_dp_select = 0xFFFFFFFFu;
}

// One DP/AP transfer with WAIT retry and error recovery, so a single failed
// access degrades to one failed GDB operation instead of a dead session.
static bool xfer(bool ap, bool rnw, uint8_t addr2, uint32_t *v)
{
    swd_xfer_status_t st = swd_xfer_retry(ap, rnw, addr2, v);
    switch (st) {
    case SWD_XFER_OK:
        return true;
    case SWD_XFER_FAULT:
        // Sticky error set (e.g. GDB read a bad address): clear it so the
        // next access works.
        adiv5_clear_errors();
        return false;
    case SWD_XFER_PARITY:
    case SWD_XFER_PROTOCOL:
        resync_link();
        return false;
    case SWD_XFER_WAIT:
    default: {
        // WAIT persisted: abandon the stuck AP transaction.
        uint32_t abort_val = DP_ABORT_DAPABORT;
        (void) swd_transfer(false, false, 0, &abort_val);
        return false;
    }
    }
}

bool adiv5_dp_read(uint8_t addr, uint32_t *out)
{
    uint32_t v = 0;
    if (!xfer(false, true, (uint8_t) (addr >> 2), &v)) {
        return false;
    }
    *out = v;
    return true;
}

bool adiv5_dp_write(uint8_t addr, uint32_t v)
{
    return xfer(false, false, (uint8_t) (addr >> 2), &v);
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
    if (!xfer(true, false, (uint8_t) (addr >> 2), &v)) {
        return false;
    }

    // AP writes are buffered.  Reading RDBUFF is a stallable DP access that
    // waits for completion and surfaces any sticky error from the write.
    uint32_t completion = 0u;
    return adiv5_dp_read(DP_RDBUFF, &completion);
}

bool adiv5_ap_read(uint8_t ap_sel, uint8_t addr, uint32_t *out)
{
    uint8_t bank = (addr >> 4) & 0xF;
    if (!ap_select(ap_sel, bank)) {
        return false;
    }

    // AP reads are posted: first read starts, second read returns prior. Use RDBUFF to fetch.
    uint32_t dummy = 0;
    if (!xfer(true, true, (uint8_t) (addr >> 2), &dummy)) {
        return false;
    }

    return adiv5_dp_read(DP_RDBUFF, out);
}

bool adiv5_init(void)
{
    g_dp_select = 0xFFFFFFFFu;

    // ADIv6 dormant wake sequence (harmless on ADIv5 targets)
    swd_leave_dormant();
    swd_jtag_to_swd();

    // Try read IDCODE to confirm link
    uint32_t id = 0;
    if (!adiv5_dp_read(DP_IDCODE, &id)) {
        return false;
    }

    // A line reset does not define SELECT.DPBANKSEL.  Establish bank zero
    // before accessing CTRL/STAT at DP address 0x4.
    if (!adiv5_dp_write(DP_SELECT, 0u)) {
        return false;
    }
    g_dp_select = 0u;

    // Clear errors and request debug power-up
    // ABORT: clear STKERR/STKCMP/STKORUN + WDERR/ORUN
    (void) adiv5_dp_write(DP_ABORT, DP_ABORT_CLEAR_ERRORS);

    // CTRL/STAT: set CDBGPWRUPREQ + CSYSPWRUPREQ
    // Bits: CDBGPWRUPREQ(28), CSYSPWRUPREQ(30)
    uint32_t req = (1u << 28) | (1u << 30);
    if (!adiv5_dp_write(DP_CTRL_STAT, req)) {
        return false;
    }

    // Wait for ACK bits (CDBGPWRUPACK(29), CSYSPWRUPACK(31)); without them
    // the debug domain is unpowered and every AP access will FAULT.
    for (int i = 0; i < 200; i++) {
        uint32_t cs = 0;
        if (adiv5_dp_read(DP_CTRL_STAT, &cs)) {
            if ((cs & (1u << 29)) && (cs & (1u << 31))) {
                return true;
            }
        }
        delay_us(100);
    }

    return false;
}

void adiv5_clear_errors(void)
{
    // Raw transfer (not xfer()): must not recurse into FAULT handling, and
    // ABORT writes are always accepted even while sticky errors are set.
    uint32_t v = DP_ABORT_CLEAR_ERRORS;
    (void) swd_transfer(false, false, 0, &v);
}
