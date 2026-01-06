// USB descriptors for MSPM0G5187 debugger probe
// Dual CDC: Port 0 = GDB RSP, Port 1 = Target VCOM

#include "tusb.h"
#include <string.h>

// Use TI's test VID with a unique PID for this debugger probe
// For production, obtain a proper VID/PID allocation
#define USB_VID   0x2047  // TI USB VID
#define USB_PID   0x0EDB  // Custom PID for MSPM0 Debugger
#define USB_BCD   0x0200  // USB 2.0

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    // Use Interface Association Descriptor (IAD) for CDC
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum {
    ITF_NUM_CDC_0 = 0,       // GDB RSP port
    ITF_NUM_CDC_0_DATA,
    ITF_NUM_CDC_1,           // Target VCOM port
    ITF_NUM_CDC_1_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + (2 * TUD_CDC_DESC_LEN))

// Endpoint numbers (IN endpoints have 0x80 bit set)
#define EPNUM_CDC_0_NOTIF   0x84
#define EPNUM_CDC_0_IN      0x83
#define EPNUM_CDC_0_OUT     0x02
#define EPNUM_CDC_1_NOTIF   0x82
#define EPNUM_CDC_1_IN      0x81
#define EPNUM_CDC_1_OUT     0x01

uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // CDC 0: GDB RSP port
    // Interface number, string index, EP notification address and size, EP data address (out, in) and size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0, 4, EPNUM_CDC_0_NOTIF, 8, EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

    // CDC 1: Target VCOM port
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_1, 5, EPNUM_CDC_1_NOTIF, 8, EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_0,
    STRID_CDC_1,
};

char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: Language ID (English US)
    "MSPM0 Debugger",            // 1: Manufacturer
    "GDB RSP Probe",             // 2: Product
    NULL,                        // 3: Serial (use chip unique ID)
    "GDB RSP",                   // 4: CDC 0 interface name
    "Target VCOM",               // 5: CDC 1 interface name
};

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL: {
            // Use chip unique ID as serial number (48 bits)
            // Read from SYSCTL UNIQUEID registers
            extern uint32_t SystemCoreClock;  // Placeholder - implement unique ID read
            // For now, use a fixed serial. In production, read from device.
            const char *serial = "MSPM0-0001";
            chr_count = strlen(serial);
            if (chr_count > 31) chr_count = 31;
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = serial[i];
            }
            break;
        }

        default:
            if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
                return NULL;
            }

            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            if (chr_count > 31) chr_count = 31;

            // Convert ASCII string into UTF-16
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    // First byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
