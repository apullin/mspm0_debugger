#include "board.h"

#include <stddef.h>
#include <stdint.h>

#include <ti/devices/msp/msp.h>
#include <ti/driverlib/driverlib.h>
#include <ti/driverlib/m0p/dl_core.h>

#include "hal.h"
#include "intmath.h"

#ifndef PROBE_CORE_CLK_HZ
#define PROBE_CORE_CLK_HZ 32000000u
#endif

#ifndef PROBE_UART_BAUD
#define PROBE_UART_BAUD 115200u
#endif

// UART0 on GPIOB.6/.7 (LP_MSPM0C1106 syscfg defaults; easy to change later)
#define PROBE_UART_INST            UART0
#define PROBE_UART_TX_IOMUX        (IOMUX_PINCM17)
#define PROBE_UART_RX_IOMUX        (IOMUX_PINCM18)
#define PROBE_UART_TX_IOMUX_FUNC   IOMUX_PINCM17_PF_UART0_TX
#define PROBE_UART_RX_IOMUX_FUNC   IOMUX_PINCM18_PF_UART0_RX

// SWD bitbang pins (arbitrary defaults; adjust when schematic is set)
#define PROBE_SWD_PORT             GPIOA
#define PROBE_SWCLK_PIN            DL_GPIO_PIN_0
#define PROBE_SWDIO_PIN            DL_GPIO_PIN_1
#define PROBE_NRESET_PIN           DL_GPIO_PIN_2
#define PROBE_SWCLK_IOMUX          (IOMUX_PINCM1)
#define PROBE_SWDIO_IOMUX          (IOMUX_PINCM2)
// PINCM numbering is device-specific: PA2 is PINCM5 on C1105/C1106.
#define PROBE_NRESET_IOMUX         (IOMUX_PINCM5)

#if defined(PROBE_USE_HFXT) && (PROBE_USE_HFXT)
// HFXT crystal pins: PA3/PINCM6 and PA4/PINCM7 (adjust when schematic is set)
#define PROBE_HFXIN_IOMUX          (IOMUX_PINCM6)
#define PROBE_HFXOUT_IOMUX         (IOMUX_PINCM7)
#endif

#if defined(PROBE_ENABLE_JTAG) && (PROBE_ENABLE_JTAG)
// JTAG data pins: TDI = PA3 (PINCM6), TDO = PA4 (PINCM7); see
// IOMUX_PINCMn_PF_GPIOA_DIOxx in mspm0c1105_c1106.h. These PINCMs are also
// the HFXT crystal pins, so JTAG and HFXT are mutually exclusive here.
#if defined(PROBE_USE_HFXT) && (PROBE_USE_HFXT)
#error "PROBE_ENABLE_JTAG conflicts with PROBE_USE_HFXT on C1105: PA3/PA4 (PINCM6/7) are the HFXT pins"
#endif
#define PROBE_JTAG_TDI_PIN_DEF     DL_GPIO_PIN_3
#define PROBE_JTAG_TDO_PIN_DEF     DL_GPIO_PIN_4
#define PROBE_JTAG_TDI_IOMUX       (IOMUX_PINCM6)
#define PROBE_JTAG_TDO_IOMUX       (IOMUX_PINCM7)
#endif

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
    DL_GPIO_reset(GPIOB);
    DL_UART_Main_reset(PROBE_UART_INST);

    DL_GPIO_enablePower(GPIOA);
    DL_GPIO_enablePower(GPIOB);
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

#if defined(PROBE_USE_HFXT) && (PROBE_USE_HFXT)
#if PROBE_HFXT_FREQ_HZ < 4000000
#error "HFXT below 4 MHz is outside the supported C1105 HFXT range"
#elif PROBE_HFXT_FREQ_HZ > 32000000
#error "HFXT above 32 MHz exceeds the C1105 datasheet maximum"
#endif
    // Configure HFXT crystal pins (disable digital IO on HFXIN/HFXOUT)
    IOMUX->SECCFG.PINCM[PROBE_HFXIN_IOMUX] = IOMUX_PINCM_PC_UNCONNECTED;
    IOMUX->SECCFG.PINCM[PROBE_HFXOUT_IOMUX] = IOMUX_PINCM_PC_UNCONNECTED;

    // Select HFXT frequency range based on crystal frequency
#if (PROBE_HFXT_FREQ_HZ <= 8000000)
    DL_SYSCTL_setHFCLKSourceHFXT(DL_SYSCTL_HFXT_RANGE_4_8_MHZ);
#elif (PROBE_HFXT_FREQ_HZ <= 16000000)
    DL_SYSCTL_setHFCLKSourceHFXT(DL_SYSCTL_HFXT_RANGE_8_16_MHZ);
#else
    DL_SYSCTL_setHFCLKSourceHFXT(DL_SYSCTL_HFXT_RANGE_16_32_MHZ);
#endif

    // Switch MCLK from SYSOSC to HSCLK (HFXT)
    DL_SYSCTL_switchMCLKfromSYSOSCtoHSCLK();
#endif

    // UART pins
    DL_GPIO_initPeripheralOutputFunction(PROBE_UART_TX_IOMUX, PROBE_UART_TX_IOMUX_FUNC);
    DL_GPIO_initPeripheralInputFunction(PROBE_UART_RX_IOMUX, PROBE_UART_RX_IOMUX_FUNC);

    // SWD pins
    // SWCLK: standard push-pull output
    DL_GPIO_initDigitalOutput(PROBE_SWCLK_IOMUX);
    // SWDIO: push-pull while driving (output enabled), Hi-Z with pull-up
    // while listening (output disabled via swdio_dir_in). Open-drain would
    // make every high bit an RC rise against the pull-up - too slow for SWD.
    DL_GPIO_initDigitalOutputFeatures(PROBE_SWDIO_IOMUX,
        DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP,
        DL_GPIO_DRIVE_STRENGTH_LOW,
        DL_GPIO_HIZ_DISABLE);
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

#if defined(PROBE_ENABLE_JTAG) && (PROBE_ENABLE_JTAG)
    // JTAG data pins (TCK/TMS reuse the SWD pins configured above).
    // TDI: push-pull output; TDO: input. Without IOMUX configuration these
    // pins are disconnected and every TDO read returns 0.
    DL_GPIO_initDigitalOutput(PROBE_JTAG_TDI_IOMUX);
    DL_GPIO_initDigitalInput(PROBE_JTAG_TDO_IOMUX);
    DL_GPIO_enableOutput(GPIOA, PROBE_JTAG_TDI_PIN_DEF);
    DL_GPIO_clearPins(GPIOA, PROBE_JTAG_TDI_PIN_DEF);
#endif

    systick_init_free_running();
}

// ---------------- HAL ----------------

void delay_us(uint32_t us)
{
    uint64_t ticks_total = probe_u64_div_u32(
        (uint64_t) PROBE_CORE_CLK_HZ * (uint64_t) us, 1000000u, NULL);

    // Chunk at half the counter range: a full-range chunk leaves only a
    // one-tick exit window that the polling loop can straddle forever.
    while (ticks_total) {
        uint32_t chunk =
            (ticks_total > 0x00800000u) ? 0x00800000u : (uint32_t) ticks_total;
        uint32_t start = SysTick->VAL & 0x00FFFFFFu;
        while (((start - (SysTick->VAL & 0x00FFFFFFu)) & 0x00FFFFFFu) < chunk) {
        }
        ticks_total -= chunk;
    }
}

uint32_t hal_time_us(void)
{
    // Monotonic microsecond counter using free-running SysTick (24-bit down-counter).
    // Must be called at least once per ~700ms to avoid missing wrap-around.
    static uint32_t last_val   = 0;
    static uint32_t us_counter = 0;
    uint32_t cur = SysTick->VAL & 0x00FFFFFFu;
    // Down-counter: modular subtraction covers the wrap case too.
    uint32_t elapsed_ticks = (last_val - cur) & 0x00FFFFFFu;

#if (PROBE_CORE_CLK_HZ % 1000000u) == 0
    const uint32_t ticks_per_us = PROBE_CORE_CLK_HZ / 1000000u;
    // Consume only whole microseconds and leave the remainder ticks in
    // place, so rapid calls (< 1us apart) don't silently discard time.
    uint32_t whole_us = elapsed_ticks / ticks_per_us;
    us_counter += whole_us;
    last_val = (last_val - whole_us * ticks_per_us) & 0x00FFFFFFu;
#else
    // At fractional-MHz clocks, consume all elapsed ticks and carry the
    // sub-microsecond numerator remainder across calls.
    static uint32_t time_remainder = 0u;
    uint64_t scaled_ticks =
        (uint64_t) elapsed_ticks * 1000000u + (uint64_t) time_remainder;
    uint64_t whole_us = probe_u64_div_u32(
        scaled_ticks, PROBE_CORE_CLK_HZ, &time_remainder);
    us_counter += (uint32_t) whole_us;
    last_val = cur;
#endif

    return us_counter;
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

// ---------------- JTAG HAL (optional) ----------------
#if defined(PROBE_ENABLE_JTAG) && (PROBE_ENABLE_JTAG)

// JTAG pins (adjust when schematic is set)
// For now, reuse SWCLK as TCK and SWDIO as TMS; TDI/TDO defined at the top
// of this file (and IOMUX-configured in board_init).
#define PROBE_JTAG_PORT            GPIOA
#define PROBE_JTAG_TCK_PIN         DL_GPIO_PIN_0   // same as SWCLK
#define PROBE_JTAG_TMS_PIN         DL_GPIO_PIN_1   // same as SWDIO
#define PROBE_JTAG_TDI_PIN         PROBE_JTAG_TDI_PIN_DEF
#define PROBE_JTAG_TDO_PIN         PROBE_JTAG_TDO_PIN_DEF

void jtag_tck_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_JTAG_PORT, PROBE_JTAG_TCK_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_JTAG_PORT, PROBE_JTAG_TCK_PIN);
    }
}

void jtag_tms_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_JTAG_PORT, PROBE_JTAG_TMS_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_JTAG_PORT, PROBE_JTAG_TMS_PIN);
    }
}

void jtag_tdi_write(int level)
{
    if (level) {
        DL_GPIO_setPins(PROBE_JTAG_PORT, PROBE_JTAG_TDI_PIN);
    } else {
        DL_GPIO_clearPins(PROBE_JTAG_PORT, PROBE_JTAG_TDI_PIN);
    }
}

int jtag_tdo_read(void)
{
    return (DL_GPIO_readPins(PROBE_JTAG_PORT, PROBE_JTAG_TDO_PIN) ? 1 : 0);
}

#endif // PROBE_ENABLE_JTAG
