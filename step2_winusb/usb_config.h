/* usb_config.h - step2: WinUSB (MS OS 2.0) + vendor control requests */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#define USB_VID         0x1209
#define USB_PID         0x0001
#define USB_BCD_DEVICE  0x0200      /* step2 */

#define USB_EP0_SIZE    64

/* bRequest Windows uses to fetch the MS OS 2.0 descriptor set (any value; we pick it) */
#define MS_VENDOR_CODE  0x20

/* DeviceInterfaceGUID (generated for this tutorial) */
#define WINUSB_GUID_STR "{1EE17A87-B766-4D91-861A-A1BA3D9E7170}"

/* PMA layout */
#define PMA_EP0_RX      0x040
#define PMA_EP0_TX      0x080

#define EP0_BUF_SIZE    256

#endif
