#ifndef USBD_H
#define USBD_H
#include <stdint.h>
#include <stdbool.h>
#include "usb_std.h"

void usbd_init(void);
uint8_t usbd_configured(void);

/* ---- EP0 API for request handlers (call only during SETUP handling) ---- */
void usbd_ep0_reply(const void *data, uint16_t len);
typedef bool (*usbd_ep0_out_done_t)(const usb_setup_t *req, uint16_t received);
void usbd_ep0_receive(void *buf, uint16_t len, usbd_ep0_out_done_t done);

/* ---- Non-control endpoint API (step3) ----
 * ep_addr is the USB endpoint address (0x01, 0x81 ...).
 * Call from inside the USB interrupt, or with interrupts disabled.
 */
/* IN: copy one packet (<= MPS) into the PMA and arm it (STAT_TX=VALID) */
void usbd_ep_write(uint8_t ep_addr, const void *data, uint16_t len);
/* OUT: copy the received packet from PMA to dst, return its length (only valid in the OUT callback) */
uint16_t usbd_ep_read(uint8_t ep_addr, void *dst, uint16_t maxlen);
/* OUT: return only the length of the received packet */
uint16_t usbd_ep_rx_count(uint8_t ep_addr);
/* OUT: allow the next packet to be received (STAT_RX=VALID). Until then the host is NAKed */
void usbd_ep_rx_enable(uint8_t ep_addr);
/* Cancel a pending IN (STAT_TX=NAK) */
void usbd_ep_tx_cancel(uint8_t ep_addr);

/* ---- Callbacks implemented by the application (vendor.c), called from the USB interrupt ---- */
bool vendor_control_request(const usb_setup_t *req);  /* Vendor requests. false -> STALL */
void vendor_configured(uint8_t config);               /* SET_CONFIGURATION / bus reset */
void vendor_ep_out(uint8_t ep_addr);                  /* An OUT packet arrived (STAT_RX is NAK) */
void vendor_ep_in_done(uint8_t ep_addr);              /* An IN packet was ACKed */

#endif
