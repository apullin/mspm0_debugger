// Board initialization for MSPM0G5187 (USB variant)
// USB-CDC for GDB RSP and optional VCOM bridge

#include "board.h"

#include <stdint.h>
#include <stdbool.h>

#include <ti/devices/msp/msp.h>
#include <ti/driverlib/driverlib.h>
#include <ti/driverlib/m0p/dl_core.h>

#include "hal.h"
#include "tusb.h"

#ifndef PROBE_CORE_CLK_HZ
#define PROBE_CORE_CLK_HZ 80000000u
#endif

// Power startup delay cycles
#define POWER_STARTUP_DELAY 16

// SWD bitbang pins (adjust when schematic is finalized)
#define PROBE_SWD_PORT      GPIOA
#define PROBE_SWCLK_PIN     DL_GPIO_PIN_0
#define PROBE_SWDIO_PIN     DL_GPIO_PIN_1
#define PROBE_NRESET_PIN    DL_GPIO_PIN_2
#define PROBE_SWCLK_IOMUX   (IOMUX_PINCM1)
#define PROBE_SWDIO_IOMUX   (IOMUX_PINCM2)
#define PROBE_NRESET_IOMUX  (IOMUX_PINCM3)

#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
// Target UART for VCOM bridge (UC0 on PA20/PA21)
// MSPM0G518x uses UNICOMM interface, access via UC0->uart
#define VCOM_UC_INST        UC0
#define VCOM_UART_TX_IOMUX  (IOMUX_PINCM21)
#define VCOM_UART_RX_IOMUX  (IOMUX_PINCM22)
#define VCOM_UART_TX_FUNC   IOMUX_PINCM21_PF_UC0_TX
#define VCOM_UART_RX_FUNC   IOMUX_PINCM22_PF_UC0_RX
#define VCOM_UART_BAUD      115200u
#endif

// Forward declarations
static void clock_init(void);
static void gpio_init(void);
static void usb_init(void);
static void systick_init(void);
#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
static void vcom_uart_init(void);
#endif

// System tick counter for timing
static volatile uint32_t g_systick_ms = 0;

void board_init(void)
{
    // Initialize clocks first (80 MHz)
    clock_init();

    // Initialize GPIO for SWD/JTAG
    gpio_init();

    // Initialize USB peripheral
    usb_init();

#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
    // Initialize target UART for VCOM bridge
    vcom_uart_init();
#endif

    // Initialize SysTick for timing
    systick_init();

    // Initialize TinyUSB stack
    tusb_init();
}

static void clock_init(void)
{
    // MSPM0G5187 default: SYSOSC 32 MHz
    // For USB, we need 48 MHz USB clock which comes from USBFLL (locks to USB SOF)
    // CPU runs at 80 MHz from SYSPLL

    // Enable power to SYSCTL
    DL_SYSCTL_setPowerPolicyRUN0SLEEP0();

    // Configure SYSOSC to 32 MHz base frequency
    DL_SYSCTL_setSYSOSCFreq(DL_SYSCTL_SYSOSC_FREQ_BASE);

    // For simplicity, we run at SYSOSC (32 MHz) initially
    // USB will use its internal FLL locked to SOF for 48 MHz USB clock
    // TODO: Configure SYSPLL for 80 MHz if needed for faster SWD bitbang

    delay_cycles(POWER_STARTUP_DELAY);
}

static void gpio_init(void)
{
    // Reset and enable power to GPIO
    DL_GPIO_reset(GPIOA);
    DL_GPIO_enablePower(GPIOA);
    delay_cycles(POWER_STARTUP_DELAY);

    // SWD pins: SWCLK push-pull, SWDIO open-drain with pull-up, NRESET open-drain with pull-up
    DL_GPIO_initDigitalOutput(PROBE_SWCLK_IOMUX);
    DL_GPIO_initDigitalOutputFeatures(PROBE_SWDIO_IOMUX,
        DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP,
        DL_GPIO_DRIVE_STRENGTH_LOW,
        DL_GPIO_HIZ_ENABLE);
    DL_GPIO_initDigitalOutputFeatures(PROBE_NRESET_IOMUX,
        DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP,
        DL_GPIO_DRIVE_STRENGTH_LOW,
        DL_GPIO_HIZ_ENABLE);

    DL_GPIO_enableOutput(PROBE_SWD_PORT, PROBE_SWCLK_PIN | PROBE_SWDIO_PIN | PROBE_NRESET_PIN);

    // Idle levels: SWCLK low, SWDIO/NRESET high (released)
    DL_GPIO_clearPins(PROBE_SWD_PORT, PROBE_SWCLK_PIN);
    DL_GPIO_setPins(PROBE_SWD_PORT, PROBE_SWDIO_PIN | PROBE_NRESET_PIN);
}

static void usb_init(void)
{
    // Reset USB peripheral
    USBFS0->GPRCM.RSTCTL = (USB_RSTCTL_KEY_UNLOCK_W | USB_RSTCTL_RESETSTKYCLR_CLR |
                            USB_RSTCTL_RESETASSERT_ASSERT);

    // Enable power to USB
    USBFS0->GPRCM.PWREN = (USB_PWREN_ENABLE_ENABLE | USB_PWREN_KEY_UNLOCK_W);

    // Wait for USB power to stabilize
    while ((USBFS0->GPRCM.PWREN & USB_PWREN_ENABLE_ENABLE) == 0)
        ;

    // Wait for USB ready in SYSCTL
    while ((SYSCTL->SOCLOCK.SYSSTATUS & SYSCTL_SYSSTATUS_USBFS0READY_MASK) !=
            SYSCTL_SYSSTATUS_USBFS0READY_TRUE)
        ;

    // Configure USB as device with internal PHY
    USBFS0->USBMODE |= (USB_USBMODE_DEVICEONLY_ENABLE | USB_USBMODE_PHYMODE_USB);

    // Clear any pending USB interrupts
    NVIC_ClearPendingIRQ(USBFS0_INT_IRQn);
    USBFS0->CPU_INT.ICLR = (USB_ICLR_INTRUSB_CLR | USB_ICLR_VUSBPWRDN_CLR);
    (void)USBFS0->REGISTERS.USBIS;  // Read to clear

    // Enable USB interrupts
    NVIC_EnableIRQ(USBFS0_INT_IRQn);
}

#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
// IBRD/FBRD values for 115200 baud at 32 MHz clock
#define UNICOMMUART_IBRD_32_MHZ_115200_BAUD    (21)
#define UNICOMMUART_FBRD_32_MHZ_115200_BAUD    (45)

static void vcom_uart_init(void)
{
    // Reset UNICOMM instance
    VCOM_UC_INST->inst->GPRCM.RSTCTL = (UNICOMM_RSTCTL_KEY_UNLOCK_W |
                                         UNICOMM_RSTCTL_RESETSTKYCLR_CLR |
                                         UNICOMM_RSTCTL_RESETASSERT_ASSERT);

    // Enable power to UNICOMM instance
    VCOM_UC_INST->inst->GPRCM.PWREN = (UNICOMM_PWREN_ENABLE_ENABLE |
                                        UNICOMM_PWREN_KEY_UNLOCK_W);
    delay_cycles(POWER_STARTUP_DELAY);

    // Configure as UART mode
    VCOM_UC_INST->inst->IPMODE = UNICOMM_IPMODE_SELECT_UART;

    // Configure UART pins
    IOMUX->SECCFG.PINCM[VCOM_UART_TX_IOMUX] = (VCOM_UART_TX_FUNC | IOMUX_PINCM_PC_CONNECTED);
    IOMUX->SECCFG.PINCM[VCOM_UART_RX_IOMUX] = (VCOM_UART_RX_FUNC | IOMUX_PINCM_PC_CONNECTED |
                                                IOMUX_PINCM_INENA_ENABLE);

    // Configure UART clock to use BUSCLK 32 MHz
    VCOM_UC_INST->uart->CLKSEL = UNICOMMUART_CLKSEL_BUSCLK_SEL_ENABLE;
    VCOM_UC_INST->uart->CLKDIV = UNICOMMUART_CLKDIV_RATIO_DIV_BY_1;

    // Disable UART before configuration
    VCOM_UC_INST->uart->CTL0 &= ~UNICOMMUART_CTL0_ENABLE_ENABLE;

    // Configure: NORMAL mode, TX+RX enabled, no flow control
    DL_Common_updateReg(&VCOM_UC_INST->uart->CTL0,
        (UNICOMMUART_CTL0_MODE_UART | UNICOMMUART_CTL0_RXE_ENABLE |
         UNICOMMUART_CTL0_TXE_ENABLE | UNICOMMUART_CTL0_CTSEN_DISABLE |
         UNICOMMUART_CTL0_RTSEN_DISABLE),
        (UNICOMMUART_CTL0_RXE_MASK | UNICOMMUART_CTL0_TXE_MASK |
         UNICOMMUART_CTL0_MODE_MASK | UNICOMMUART_CTL0_RTSEN_MASK |
         UNICOMMUART_CTL0_CTSEN_MASK));

    // Line control: 8 data bits, no parity, 1 stop bit
    DL_Common_updateReg(&VCOM_UC_INST->uart->LCRH,
        (UNICOMMUART_LCRH_PEN_DISABLE | UNICOMMUART_LCRH_WLEN_DATABIT8 |
         UNICOMMUART_LCRH_STP2_DISABLE),
        (UNICOMMUART_LCRH_PEN_MASK | UNICOMMUART_LCRH_EPS_MASK |
         UNICOMMUART_LCRH_SPS_MASK | UNICOMMUART_LCRH_WLEN_MASK |
         UNICOMMUART_LCRH_STP2_MASK));

    // 16x oversampling
    DL_Common_updateReg(&VCOM_UC_INST->uart->CTL0, UNICOMMUART_CTL0_HSE_OVS16,
                        UNICOMMUART_CTL0_HSE_MASK);

    // Baud rate divisors for 115200
    DL_Common_updateReg(&VCOM_UC_INST->uart->IBRD, UNICOMMUART_IBRD_32_MHZ_115200_BAUD,
                        UNICOMMUART_IBRD_DIVINT_MASK);
    DL_Common_updateReg(&VCOM_UC_INST->uart->FBRD, UNICOMMUART_FBRD_32_MHZ_115200_BAUD,
                        UNICOMMUART_FBRD_DIVFRAC_MASK);

    // When configuring baud-rate divisor the LCRH must also be written
    DL_Common_updateReg(&VCOM_UC_INST->uart->LCRH,
                        (VCOM_UC_INST->uart->LCRH & UNICOMMUART_LCRH_BRK_MASK),
                        UNICOMMUART_LCRH_BRK_MASK);

    // Enable UART
    VCOM_UC_INST->uart->CTL0 |= UNICOMMUART_CTL0_ENABLE_ENABLE;
}
#endif

static void systick_init(void)
{
    // Configure SysTick for 1ms interrupts at 32 MHz (SYSOSC)
    SysTick_Config(32000);
}

// USB interrupt handler - forward to TinyUSB
void USBFS0_IRQHandler(void)
{
    // Clear interrupt (read IIDX register)
    (void)USBFS0->CPU_INT.IIDX;
    tud_int_handler(0);
}

// SysTick interrupt handler
void SysTick_Handler(void)
{
    g_systick_ms++;
}

// ---------------- HAL Functions ----------------

void delay_us(uint32_t us)
{
    // Simple busy-wait delay at 32 MHz
    uint32_t cycles = (32u * us);  // 32 cycles per us at 32 MHz
    delay_cycles(cycles);
}

uint32_t hal_time_us(void)
{
    // Return time in microseconds (from ms counter)
    return g_systick_ms * 1000u;
}

// USB-CDC Port 0: GDB RSP communication
int uart_getc(void)
{
    if (!tud_cdc_n_available(0)) {
        return -1;
    }
    return tud_cdc_n_read_char(0);
}

void uart_putc(uint8_t c)
{
    tud_cdc_n_write_char(0, c);
    tud_cdc_n_write_flush(0);
}

// USB task - must be called from main loop
void usb_poll(void)
{
    tud_task();
}

#if defined(PROBE_ENABLE_VCOM) && PROBE_ENABLE_VCOM
// VCOM bridge: USB-CDC Port 1 <-> Target UART
void vcom_poll(void)
{
    // USB -> Target UART
    while (tud_cdc_n_available(1)) {
        int c = tud_cdc_n_read_char(1);
        if (c >= 0) {
            // Wait for TX FIFO not full, then transmit
            while (VCOM_UC_INST->uart->STAT & UNICOMMUART_STAT_TXFF_MASK)
                ;
            VCOM_UC_INST->uart->TXDATA = (uint8_t)c;
        }
    }

    // Target UART -> USB
    while (!(VCOM_UC_INST->uart->STAT & UNICOMMUART_STAT_RXFE_MASK)) {
        uint8_t c = (uint8_t)(VCOM_UC_INST->uart->RXDATA & UNICOMMUART_RXDATA_DATA_MASK);
        tud_cdc_n_write_char(1, c);
    }
    tud_cdc_n_write_flush(1);
}
#endif

// ---------------- SWD HAL ----------------

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

// JTAG pins (adjust when schematic is finalized)
#define PROBE_JTAG_PORT     GPIOA
#define PROBE_JTAG_TCK_PIN  DL_GPIO_PIN_0   // same as SWCLK
#define PROBE_JTAG_TMS_PIN  DL_GPIO_PIN_1   // same as SWDIO
#define PROBE_JTAG_TDI_PIN  DL_GPIO_PIN_3
#define PROBE_JTAG_TDO_PIN  DL_GPIO_PIN_4

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
