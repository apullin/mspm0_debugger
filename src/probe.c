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

bool probe_attach(void)
{
#if defined(PROBE_ENABLE_CORTEXM) && (PROBE_ENABLE_CORTEXM)
    // Bring up SWD/ADIv5 for Cortex-M detection; RISC-V JTAG init happens
    // inside target_init() as needed.
    (void) adiv5_init();
#endif

    // target_init() handles architecture detection/fallback
    // (tries SWD first if Cortex-M enabled, then JTAG if RISC-V enabled)
    target_init();

    g_link_up = target_attached() && target_halt();
    if (!g_link_up) {
        target_disconnect();
        return false;
    }

    target_breakpoints_init();
    return true;
}

bool probe_init(void)
{
    rsp_init();

    // Optional: hold target in reset briefly
    nreset_write(0);
    delay_us(1000);
    nreset_write(1);
    delay_us(1000);

    return probe_attach();
}

void probe_poll(void)
{
#if defined(PROBE_HAS_USB) && PROBE_HAS_USB
    // Service USB stack (must be called frequently)
    extern void usb_poll(void);
    usb_poll();
#endif

    int ch;
    while ((ch = uart_getc()) >= 0) {
        rsp_process_byte((uint8_t) ch);
    }
    rsp_poll();

#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
    // Service VCOM bridge (USB CDC port 1 <-> target UART)
    extern void vcom_poll(void);
    vcom_poll();
#endif
}
