/*
 * desc.c - step1 descriptors
 *   Device / Configuration (one interface with no endpoints) / String
 */
#include <stdint.h>
#include "usb_std.h"
#include "usb_config.h"
#include "ch32v203_regs.h"
#include "desc.h"

const uint8_t desc_device[18] = {
    18,                     /* bLength                                    */
    USB_DESC_DEVICE,        /* bDescriptorType                            */
    U16_LE(0x0200),         /* bcdUSB: USB 2.0                            */
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

#define CONFIG_TOTAL_LEN (9 + 9)

const uint8_t desc_config[CONFIG_TOTAL_LEN] = {
    /* Configuration descriptor */
    9,                      /* bLength                                    */
    USB_DESC_CONFIGURATION, /* bDescriptorType                            */
    U16_LE(CONFIG_TOTAL_LEN), /* wTotalLength                             */
    1,                      /* bNumInterfaces                             */
    1,                      /* bConfigurationValue                        */
    0,                      /* iConfiguration                             */
    0x80,                   /* bmAttributes: bus-powered                  */
    50,                     /* bMaxPower: 100mA (in 2mA units)            */

    /* Interface descriptor */
    9,                      /* bLength                                    */
    USB_DESC_INTERFACE,     /* bDescriptorType                            */
    0,                      /* bInterfaceNumber                           */
    0,                      /* bAlternateSetting                          */
    0,                      /* bNumEndpoints: none in step1               */
    0xFF,                   /* bInterfaceClass: Vendor Specific           */
    0x00,                   /* bInterfaceSubClass                         */
    0x00,                   /* bInterfaceProtocol                         */
    0,                      /* iInterface                                 */
};
const uint16_t desc_config_len = sizeof(desc_config);

static const char *const str_table[] = {
    [1] = "usb-stack-tutorial",
    [2] = "CH32V203 Vendor Step1",
};

/* Build an ASCII string as a USB string descriptor (UTF-16LE) */
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
        /* LANGID list: 0x0409 (English - US) */
        buf[0] = 4;
        buf[1] = USB_DESC_STRING;
        buf[2] = 0x09;
        buf[3] = 0x04;
        return 4;
    }
    if (index == 3) {
        /* Serial number: 96-bit chip-unique ID as 24 hex digits */
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
    return 0; /* Not found -> STALL */
}
