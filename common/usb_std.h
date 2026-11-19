/* usb_std.h - USB 2.0 specification constants (Chapter 9) and Microsoft OS 2.0 descriptor constants */
#ifndef USB_STD_H
#define USB_STD_H

#include <stdint.h>

/* SETUP packet (8 bytes, little-endian) */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* bmRequestType */
#define USB_REQ_DIR_IN          0x80u
#define USB_REQ_TYPE_MASK       0x60u
#define USB_REQ_TYPE_STANDARD   0x00u
#define USB_REQ_TYPE_CLASS      0x20u
#define USB_REQ_TYPE_VENDOR     0x40u
#define USB_REQ_RCPT_MASK       0x1Fu
#define USB_REQ_RCPT_DEVICE     0x00u
#define USB_REQ_RCPT_INTERFACE  0x01u
#define USB_REQ_RCPT_ENDPOINT   0x02u

/* bRequest (standard) */
#define USB_REQ_GET_STATUS          0
#define USB_REQ_CLEAR_FEATURE       1
#define USB_REQ_SET_FEATURE         3
#define USB_REQ_SET_ADDRESS         5
#define USB_REQ_GET_DESCRIPTOR      6
#define USB_REQ_SET_DESCRIPTOR      7
#define USB_REQ_GET_CONFIGURATION   8
#define USB_REQ_SET_CONFIGURATION   9
#define USB_REQ_GET_INTERFACE       10
#define USB_REQ_SET_INTERFACE       11
#define USB_REQ_SYNCH_FRAME         12

/* Feature selectors */
#define USB_FEATURE_ENDPOINT_HALT   0
#define USB_FEATURE_REMOTE_WAKEUP   1

/* Descriptor types */
#define USB_DESC_DEVICE             1
#define USB_DESC_CONFIGURATION      2
#define USB_DESC_STRING             3
#define USB_DESC_INTERFACE          4
#define USB_DESC_ENDPOINT           5
#define USB_DESC_DEVICE_QUALIFIER   6
#define USB_DESC_BOS                15
#define USB_DESC_DEVICE_CAPABILITY  16

/* Endpoint attributes */
#define USB_EP_ATTR_CONTROL     0x00
#define USB_EP_ATTR_ISO         0x01
#define USB_EP_ATTR_BULK        0x02
#define USB_EP_ATTR_INTERRUPT   0x03

/* Device capability types */
#define USB_DEVCAP_PLATFORM     0x05

/* Microsoft OS 2.0 descriptors */
#define MS_OS_20_DESCRIPTOR_INDEX       7       /* wIndex: get the descriptor set */
#define MS_OS_20_SET_HEADER_DESCRIPTOR  0x00
#define MS_OS_20_SUBSET_HEADER_CONFIGURATION 0x01
#define MS_OS_20_SUBSET_HEADER_FUNCTION 0x02
#define MS_OS_20_FEATURE_COMPATIBLE_ID  0x03
#define MS_OS_20_FEATURE_REG_PROPERTY   0x04
#define MS_OS_20_WINDOWS_VERSION_8_1    0x06030000u

/* Helpers for writing 16/32-bit values into a descriptor byte array */
#define U16_LE(x)  (uint8_t)((x) & 0xFF), (uint8_t)(((x) >> 8) & 0xFF)
#define U32_LE(x)  (uint8_t)((x) & 0xFF), (uint8_t)(((x) >> 8) & 0xFF), \
                   (uint8_t)(((x) >> 16) & 0xFF), (uint8_t)(((x) >> 24) & 0xFF)

#endif
