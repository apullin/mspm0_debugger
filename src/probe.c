// Probe entry points: init + poll loop

#include "probe.h"

#include <stdint.h>

#include "hal.h"
#include "rsp.h"
#include "target.h"

#if defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM)
#include "adiv5.h"
#endif

static bool g_link_up = false;

bool probe_init(void)
{
    rsp_init();

    // Optional: hold target in reset briefly
    nreset_write(0);
    delay_us(1000);
    nreset_write(1);
    delay_us(1000);

#if defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM)
    // Try SWD/ADIv5 for Cortex-M targets
    g_link_up = adiv5_init();
#endif

    // target_init() handles architecture detection/fallback
    // (tries SWD first if Cortex-M enabled, then JTAG if RISC-V enabled)
    target_init();

    // Check if any target was detected
    // For now, assume link is up if we get here (target_init sets internal state)
#if !defined(PROBE_ENABLE_CORTEXM) || !(PROBE_ENABLE_CORTEXM)
    g_link_up = true;  // RISC-V only: JTAG init happens in target_init
#endif

    if (g_link_up) {
        (void) target_halt();
        target_breakpoints_init();
    }
    return g_link_up;
}

void probe_poll(void)
{
    int ch;
    while ((ch = uart_getc()) >= 0) {
        rsp_process_byte((uint8_t) ch);
    }
    rsp_poll();
}
