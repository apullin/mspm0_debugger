#include "board.h"

#include <stdint.h>

#include <ti/devices/msp/msp.h>
#include <ti/driverlib/driverlib.h>
#include <ti/driverlib/m0p/dl_core.h>

#include "hal.h"

#ifndef PROBE_CORE_CLK_HZ
// MSPM0C110x SYSOSC is a 24MHz internal oscillator (per device datasheet/DFP metadata).
#define PROBE_CORE_CLK_HZ 24000000u
#endif

#ifndef PROBE_UART_BAUD
#define PROBE_UART_BAUD 115200u
#endif

// UART0 on GPIOA.26/.27 (LP_MSPM0C1104 syscfg defaults; easy to change later)
#define PROBE_UART_INST            UART0
#define PROBE_UART_TX_IOMUX        (IOMUX_PINCM28)
#define PROBE_UART_RX_IOMUX        (IOMUX_PINCM27)
#define PROBE_UART_TX_IOMUX_FUNC   IOMUX_PINCM28_PF_UART0_TX
#define PROBE_UART_RX_IOMUX_FUNC   IOMUX_PINCM27_PF_UART0_RX

// SWD bitbang pins (arbitrary defaults; adjust when schematic is set)
#define PROBE_SWD_PORT             GPIOA
#define PROBE_SWCLK_PIN            DL_GPIO_PIN_0
#define PROBE_SWDIO_PIN            DL_GPIO_PIN_1
#define PROBE_NRESET_PIN           DL_GPIO_PIN_2
#define PROBE_SWCLK_IOMUX          (IOMUX_PINCM1)
#define PROBE_SWDIO_IOMUX          (IOMUX_PINCM2)
#define PROBE_NRESET_IOMUX         (IOMUX_PINCM3)

static void systick_init_free_running(void)
{
    SysTick->CTRL = 0;
    SysTick->LOAD = 0x00FFFFFFu;
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

void board_init(void)
{
    // Reset/power up peripherals
    DL_GPIO_reset(GPIOA);
    DL_UART_Main_reset(PROBE_UART_INST);

    DL_GPIO_enablePower(GPIOA);
    DL_UART_Main_enablePower(PROBE_UART_INST);

    delay_cycles(16);

    // High-speed run mode (MCLK from SYSOSC)
    DL_SYSCTL_setPowerPolicyRUN0SLEEP0();
    DL_SYSCTL_setSYSOSCFreq(DL_SYSCTL_SYSOSC_FREQ_BASE);
#if defined(PROBE_ENABLE_SYSOSC_FCL) && (PROBE_ENABLE_SYSOSC_FCL)
    // SYSOSC Frequency Correction Loop (FCL). See `docs/slau893c.md` section 2.3.1.2.1.
    // Note: enabling FCL is sticky until BOOTRST; hardware may require an ROSC resistor depending on device.
    DL_SYSCTL_enableSYSOSCFCL();
#endif

    // UART pins
    DL_GPIO_initPeripheralOutputFunction(PROBE_UART_TX_IOMUX, PROBE_UART_TX_IOMUX_FUNC);
    DL_GPIO_initPeripheralInputFunction(PROBE_UART_RX_IOMUX, PROBE_UART_RX_IOMUX_FUNC);

    // SWD pins
    // SWCLK: standard push-pull output
    DL_GPIO_initDigitalOutput(PROBE_SWCLK_IOMUX);
    // SWDIO: open-drain (Hi-Z) with internal pull-up for bidirectional SWD
    DL_GPIO_initDigitalOutputFeatures(PROBE_SWDIO_IOMUX,
        DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP,
        DL_GPIO_DRIVE_STRENGTH_LOW,
        DL_GPIO_HIZ_ENABLE);
    // NRESET: open-drain with pull-up (active low reset)
    DL_GPIO_initDigitalOutputFeatures(PROBE_NRESET_IOMUX,
        DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP,
        DL_GPIO_DRIVE_STRENGTH_LOW,
        DL_GPIO_HIZ_ENABLE);

    DL_GPIO_enableOutput(PROBE_SWD_PORT, PROBE_SWCLK_PIN | PROBE_SWDIO_PIN | PROBE_NRESET_PIN);

    // Idle levels
    DL_GPIO_clearPins(PROBE_SWD_PORT, PROBE_SWCLK_PIN);
    DL_GPIO_setPins(PROBE_SWD_PORT, PROBE_SWDIO_PIN | PROBE_NRESET_PIN);

    // UART config
    static const DL_UART_Main_ClockConfig uart_clk = {
        .clockSel    = DL_UART_MAIN_CLOCK_BUSCLK,
        .divideRatio = DL_UART_MAIN_CLOCK_DIVIDE_RATIO_1,
    };

    static const DL_UART_Main_Config uart_cfg = {
        .mode        = DL_UART_MAIN_MODE_NORMAL,
        .direction   = DL_UART_MAIN_DIRECTION_TX_RX,
        .flowControl = DL_UART_MAIN_FLOW_CONTROL_NONE,
        .parity      = DL_UART_MAIN_PARITY_NONE,
        .wordLength  = DL_UART_MAIN_WORD_LENGTH_8_BITS,
        .stopBits    = DL_UART_MAIN_STOP_BITS_ONE,
    };

    DL_UART_Main_setClockConfig(PROBE_UART_INST, (DL_UART_Main_ClockConfig *) &uart_clk);
    DL_UART_Main_init(PROBE_UART_INST, (DL_UART_Main_Config *) &uart_cfg);
    DL_UART_Main_configBaudRate(PROBE_UART_INST, PROBE_CORE_CLK_HZ, PROBE_UART_BAUD);

    DL_UART_Main_enableFIFOs(PROBE_UART_INST);
    DL_UART_Main_setRXFIFOThreshold(PROBE_UART_INST, DL_UART_RX_FIFO_LEVEL_ONE_ENTRY);
    DL_UART_Main_setTXFIFOThreshold(PROBE_UART_INST, DL_UART_TX_FIFO_LEVEL_EMPTY);
    DL_UART_Main_enable(PROBE_UART_INST);

    systick_init_free_running();
}

// ---------------- HAL ----------------

void delay_us(uint32_t us)
{
    // SysTick is 24-bit, decrementing at core clock.
    uint64_t ticks_total = ((uint64_t) PROBE_CORE_CLK_HZ * (uint64_t) us) / 1000000ull;

    while (ticks_total) {
        uint32_t chunk = (ticks_total > 0x00FFFFFFu) ? 0x00FFFFFFu : (uint32_t) ticks_total;
        uint32_t start = SysTick->VAL & 0x00FFFFFFu;
        while (((start - (SysTick->VAL & 0x00FFFFFFu)) & 0x00FFFFFFu) < chunk) {
        }
        ticks_total -= chunk;
    }
}

int uart_getc(void)
{
    if (DL_UART_Main_isRXFIFOEmpty(PROBE_UART_INST)) {
        return -1;
    }
    return (int) (DL_UART_Main_receiveData(PROBE_UART_INST) & 0xFFu);
}

void uart_putc(uint8_t c)
{
    DL_UART_Main_transmitDataBlocking(PROBE_UART_INST, c);
}

void swclk_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_SWD_PORT, PROBE_SWCLK_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_SWD_PORT, PROBE_SWCLK_PIN);
    }
}

void swdio_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_SWD_PORT, PROBE_SWDIO_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_SWD_PORT, PROBE_SWDIO_PIN);
    }
}

int swdio_read(void)
{
    return (DL_GPIO_readPins(PROBE_SWD_PORT, PROBE_SWDIO_PIN) ? 1 : 0);
}

void swdio_dir_out(void)
{
    DL_GPIO_enableOutput(PROBE_SWD_PORT, PROBE_SWDIO_PIN);
}

void swdio_dir_in(void)
{
    DL_GPIO_disableOutput(PROBE_SWD_PORT, PROBE_SWDIO_PIN);
}

void nreset_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_SWD_PORT, PROBE_NRESET_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_SWD_PORT, PROBE_NRESET_PIN);
    }
}
