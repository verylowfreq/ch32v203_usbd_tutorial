/* usb_config.h - step3: WinUSB + Bulk IN/OUT (EP1) */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#define USB_VID         0x1209
#define USB_PID         0x0001
#define USB_BCD_DEVICE  0x0300      /* step3 */

#define USB_EP0_SIZE    64

#define MS_VENDOR_CODE  0x20
#define WINUSB_GUID_STR "{1EE17A87-B766-4D91-861A-A1BA3D9E7170}"

/* Endpoints */
#define EP_BULK_OUT     0x01
#define EP_BULK_IN      0x81
#define BULK_MPS        64

/*
 * PMA layout
 *   0x000-0x03F : BTABLE
 *   0x040       : EP0 OUT 64B
 *   0x080       : EP0 IN  64B
 *   0x0C0       : EP1 OUT 64B
 *   0x100       : EP1 IN  64B
 */
#define PMA_EP0_RX      0x040
#define PMA_EP0_TX      0x080
#define PMA_EP1_RX      0x0C0
#define PMA_EP1_TX      0x100

#define EP0_BUF_SIZE    256

#endif
