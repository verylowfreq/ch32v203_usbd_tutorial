#ifndef USBD_H
#define USBD_H
#include <stdint.h>
#include <stdbool.h>
#include "usb_std.h"

void usbd_init(void);
uint8_t usbd_configured(void);

/* ---- EP0 API for request handlers (call only during SETUP handling) ---- */
/* IN data stage: send data (truncated to wLength). data must stay valid until sending finishes. */
void usbd_ep0_reply(const void *data, uint16_t len);
/* OUT data stage: receive up to len bytes into buf, then call done(). If done returns false, STALL. */
typedef bool (*usbd_ep0_out_done_t)(const usb_setup_t *req, uint16_t received);
void usbd_ep0_receive(void *buf, uint16_t len, usbd_ep0_out_done_t done);

/* ---- Experiment (verifying CH32 EP0 behavior, chapter 09) ---- */
#define EXP_AT_SETUP   0x01  /* After SETUP: wait with RX=NAK before arming the data stage */
#define EXP_IN_DATA    0x02  /* Data stage: wait before reading each received packet      */
#define EXP_GATE_BULK  0x04  /* Switch EP0 to BULK type only while waiting               */
#define EXP_IN_SETUP   0x08  /* Control IN: wait with TX=NAK before arming the first packet */
extern volatile uint16_t g_exp_delay_us;
extern volatile uint16_t g_exp_mode;

/* Vendor request handler (vendor.c). Return false -> STALL */
bool vendor_control_request(const usb_setup_t *req);

#endif
