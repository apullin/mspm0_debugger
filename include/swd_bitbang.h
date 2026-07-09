#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SWD_XFER_OK = 0,
    SWD_XFER_WAIT,     // ACK=WAIT: retry the same transfer
    SWD_XFER_FAULT,    // ACK=FAULT: sticky error set, clear via DP ABORT
    SWD_XFER_PARITY,   // read data parity mismatch
    SWD_XFER_PROTOCOL, // ACK not OK/WAIT/FAULT: line dead or desynced
} swd_xfer_status_t;

void swd_leave_dormant(void);
void swd_jtag_to_swd(void);
// Line reset + idle-low. Caller must read DPIDR before any other request.
void swd_resync(void);
swd_xfer_status_t swd_transfer(bool ap, bool rnw, uint8_t addr2, uint32_t *data_inout);
