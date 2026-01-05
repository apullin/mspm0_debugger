// Probe entry points: init + poll loop

#include "probe.h"

#include <stdint.h>

#include "adiv5.h"
#include "hal.h"
#include "rsp.h"
#include "target.h"

static bool g_link_up = false;

bool probe_init(void)
{
    rsp_init();

    // Optional: hold target in reset briefly
    nreset_write(0);
    delay_us(1000);
    nreset_write(1);
    delay_us(1000);

    g_link_up = adiv5_init();
    if (g_link_up) {
        target_init();
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
