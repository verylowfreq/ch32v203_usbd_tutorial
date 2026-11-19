/* usb_config.h - step1: settings for enumeration only */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

/* pid.codes test VID/PID (for experiments only; do not ship products with this) */
#define USB_VID         0x1209
#define USB_PID         0x0001
/* bcdDevice is changed per step so that Windows does not reuse cached device information */
#define USB_BCD_DEVICE  0x0100

#define USB_EP0_SIZE    64

/*
 * PMA (512 bytes) layout. Offsets are byte offsets inside the PMA.
 *   0x000-0x03F : BTABLE (8 bytes x 8 entries)
 *   0x040-0x07F : EP0 OUT (SETUP/OUT) 64B
 *   0x080-0x0BF : EP0 IN 64B
 */
#define PMA_EP0_RX      0x040
#define PMA_EP0_TX      0x080

/* Largest data stage EP0 can handle (descriptors and control OUT data) */
#define EP0_BUF_SIZE    256

#endif
