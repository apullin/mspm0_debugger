#include "probe.h"
#include "board.h"

int main(void)
{
    board_init();
    probe_init();
    while (1) {
        probe_poll();
        // optionally low-power wait, etc.
    }
}
