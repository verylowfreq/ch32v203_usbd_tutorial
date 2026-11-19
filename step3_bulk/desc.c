/*
 * desc.c - step3 descriptors (step2 + Bulk EP1 IN/OUT)
 *   step1 + the BOS descriptor and the Microsoft OS 2.0 descriptor set (makes Windows load WinUSB automatically)
 */
#include <stdint.h>
#include "usb_std.h"
#include "usb_config.h"
#include "ch32v203_regs.h"
#include "desc.h"

const uint8_t desc_device[18] = {
    18,                     /* bLength                                    */
    USB_DESC_DEVICE,        /* bDescriptorType                            */
    U16_LE(0x0201),         /* bcdUSB: 2.01 or later -> Windows asks for the BOS */
    0x00,                   /* bDeviceClass: defined per interface        */
    0x00,                   /* bDeviceSubClass                            */
    0x00,                   /* bDeviceProtocol                            */
    USB_EP0_SIZE,           /* bMaxPacketSize0                            */
    U16_LE(USB_VID),        /* idVendor                                   */
    U16_LE(USB_PID),        /* idProduct                                  */
    U16_LE(USB_BCD_DEVICE), /* bcdDevice                                  */
    1,                      /* iManufacturer                              */
    2,                      /* iProduct                                   */
    3,                      /* iSerialNumber                              */
    1,                      /* bNumConfigurations                         */
};

#define CONFIG_TOTAL_LEN (9 + 9 + 7 + 7)

const uint8_t desc_config[CONFIG_TOTAL_LEN] = {
    9, USB_DESC_CONFIGURATION, U16_LE(CONFIG_TOTAL_LEN), 1, 1, 0, 0x80, 50,
    /* Interface 0: Vendor Specific, 2 endpoints */
    9, USB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    /* EP1 OUT: Bulk 64B */
    7, USB_DESC_ENDPOINT, EP_BULK_OUT, USB_EP_ATTR_BULK, U16_LE(BULK_MPS), 0,
    /* EP1 IN: Bulk 64B */
    7, USB_DESC_ENDPOINT, EP_BULK_IN, USB_EP_ATTR_BULK, U16_LE(BULK_MPS), 0,
};
const uint16_t desc_config_len = sizeof(desc_config);

/* ------------------------------------------------------------------ */
/* Microsoft OS 2.0 descriptor set                                     */
/*                                                                    */
/* For a non-composite device (single function) this is the minimal set: */
/*   Set header -> Compatible ID "WINUSB" -> registry property DeviceInterfaceGUIDs */
/* ------------------------------------------------------------------ */
#define W(c) (uint8_t)(c), 0      /* ASCII -> UTF-16LE */

#define MSOS20_SET_HEADER_LEN   10
#define MSOS20_COMPAT_ID_LEN    20
#define MSOS20_REG_PROP_LEN     (10 + 42 + 80)   /* = 132 */
#define MSOS20_TOTAL_LEN        (MSOS20_SET_HEADER_LEN + MSOS20_COMPAT_ID_LEN + MSOS20_REG_PROP_LEN)

const uint8_t desc_ms_os_20[MSOS20_TOTAL_LEN] = {
    /* Microsoft OS 2.0 descriptor set header */
    U16_LE(MSOS20_SET_HEADER_LEN),            /* wLength                     */
    U16_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),   /* wDescriptorType             */
    U32_LE(MS_OS_20_WINDOWS_VERSION_8_1),     /* dwWindowsVersion            */
    U16_LE(MSOS20_TOTAL_LEN),                 /* wTotalLength                */

    /* Compatible ID descriptor: "WINUSB" -> winusb.inf matches */
    U16_LE(MSOS20_COMPAT_ID_LEN),             /* wLength                     */
    U16_LE(MS_OS_20_FEATURE_COMPATIBLE_ID),   /* wDescriptorType             */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,       /* CompatibleID (8 bytes)      */
    0, 0, 0, 0, 0, 0, 0, 0,                   /* SubCompatibleID (8 bytes)   */

    /* Registry property descriptor: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
    U16_LE(MSOS20_REG_PROP_LEN),              /* wLength                     */
    U16_LE(MS_OS_20_FEATURE_REG_PROPERTY),    /* wDescriptorType             */
    U16_LE(7),                                /* wPropertyDataType: REG_MULTI_SZ */
    U16_LE(42),                               /* wPropertyNameLength         */
    W('D'), W('e'), W('v'), W('i'), W('c'), W('e'), W('I'), W('n'), W('t'), W('e'), W('r'),
    W('f'), W('a'), W('c'), W('e'), W('G'), W('U'), W('I'), W('D'), W('s'), W(0),
    U16_LE(80),                               /* wPropertyDataLength         */
    W('{'), W('1'), W('E'), W('E'), W('1'), W('7'), W('A'), W('8'), W('7'), W('-'), W('B'),
    W('7'), W('6'), W('6'), W('-'), W('4'), W('D'), W('9'), W('1'), W('-'), W('8'), W('6'),
    W('1'), W('A'), W('-'), W('A'), W('1'), W('B'), W('A'), W('3'), W('D'), W('9'), W('E'),
    W('7'), W('1'), W('7'), W('0'), W('}'),
    W(0), W(0),                               /* Terminator of the string, then of the MULTI_SZ list */
};
_Static_assert(sizeof(desc_ms_os_20) == 0xA2, "MS OS 2.0 descriptor set size");

/* ------------------------------------------------------------------ */
/* BOS descriptor: holds the platform capability (MS OS 2.0)           */
/* ------------------------------------------------------------------ */
#define BOS_TOTAL_LEN (5 + 28)

const uint8_t desc_bos[BOS_TOTAL_LEN] = {
    /* BOS header */
    5,                                        /* bLength                     */
    USB_DESC_BOS,                             /* bDescriptorType             */
    U16_LE(BOS_TOTAL_LEN),                    /* wTotalLength                */
    1,                                        /* bNumDeviceCaps              */

    /* Platform capability descriptor (Microsoft OS 2.0) */
    28,                                       /* bLength                     */
    USB_DESC_DEVICE_CAPABILITY,               /* bDescriptorType             */
    USB_DEVCAP_PLATFORM,                      /* bDevCapabilityType          */
    0,                                        /* bReserved                   */
    /* PlatformCapabilityUUID: {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} (mixed-endian) */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    U32_LE(MS_OS_20_WINDOWS_VERSION_8_1),     /* dwWindowsVersion            */
    U16_LE(MSOS20_TOTAL_LEN),                 /* wMSOSDescriptorSetTotalLength */
    MS_VENDOR_CODE,                           /* bMS_VendorCode              */
    0,                                        /* bAltEnumCode                */
};
const uint16_t desc_bos_len = sizeof(desc_bos);
const uint16_t desc_ms_os_20_len = sizeof(desc_ms_os_20);

static const char *const str_table[] = {
    [1] = "usb-stack-tutorial",
    [2] = "CH32V203 Vendor Step3",
};

static uint16_t make_string(uint8_t *buf, uint16_t bufsize, const char *s)
{
    uint16_t n = 2;
    while (*s && n + 2u <= bufsize && n + 2u <= 254u) {
        buf[n++] = (uint8_t)*s++;
        buf[n++] = 0;
    }
    buf[0] = (uint8_t)n;
    buf[1] = USB_DESC_STRING;
    return n;
}

uint16_t desc_get_string(uint8_t index, uint8_t *buf, uint16_t bufsize)
{
    if (index == 0) {
        buf[0] = 4;
        buf[1] = USB_DESC_STRING;
        buf[2] = 0x09;
        buf[3] = 0x04;
        return 4;
    }
    if (index == 3) {
        static const char hex[] = "0123456789ABCDEF";
        char s[25];
        uint32_t id[3] = { ESIG_UNIID1, ESIG_UNIID2, ESIG_UNIID3 };
        for (int w = 0; w < 3; w++) {
            for (int i = 0; i < 8; i++) {
                s[w * 8 + i] = hex[(id[w] >> (28 - 4 * i)) & 0xF];
            }
        }
        s[24] = 0;
        return make_string(buf, bufsize, s);
    }
    if (index < sizeof(str_table) / sizeof(str_table[0]) && str_table[index]) {
        return make_string(buf, bufsize, str_table[index]);
    }
    return 0;
}
