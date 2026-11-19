/*
 * vendor.c - step2: vendor control requests
 *
 *  - Returns the MS OS 2.0 descriptor set (bRequest = MS_VENDOR_CODE, wIndex = 7)
 *  - GET_INFO / SET_BUF / GET_BUF
 *  - Experiment request (arming delay for the control OUT data stage)
 *
 * Called from the USB interrupt.
 */
#include <stdint.h>
#include <stdbool.h>
#include "usb_std.h"
#include "usb_config.h"
#include "system.h"
#include "dbglog.h"
#include "desc.h"
#include "usbd.h"
#include "vendor.h"

typedef struct __attribute__((packed)) {
    char     magic[4];        /* "V203"                      */
    uint8_t  step;            /* Tutorial step number        */
    uint8_t  reserved[3];
    uint32_t uptime_ms;
    uint32_t set_buf_count;   /* Number of SET_BUF requests completed */
} vendor_info_t;

static vendor_info_t info;
static uint8_t  store[256];
static uint16_t store_len;
static uint32_t set_buf_count;

/* OUT data received -> commit it */
static bool set_buf_done(const usb_setup_t *req, uint16_t received)
{
    store_len = received;
    set_buf_count++;
    return true;
}

bool vendor_control_request(const usb_setup_t *req)
{
    dbglog(LOG_VENDOR, req->bRequest, req->wLength);

    /* Microsoft OS 2.0 descriptor set */
    if (req->bRequest == MS_VENDOR_CODE) {
        if (req->wIndex == MS_OS_20_DESCRIPTOR_INDEX && (req->bmRequestType & USB_REQ_DIR_IN)) {
            usbd_ep0_reply(desc_ms_os_20, desc_ms_os_20_len);
            return true;
        }
        return false;
    }

    switch (req->bRequest) {
    case VREQ_GET_INFO:
        if (!(req->bmRequestType & USB_REQ_DIR_IN)) return false;
        info.magic[0] = 'V'; info.magic[1] = '2'; info.magic[2] = '0'; info.magic[3] = '3';
        info.step = VENDOR_STEP;
        info.uptime_ms = millis();
        info.set_buf_count = set_buf_count;
        usbd_ep0_reply(&info, sizeof(info));
        return true;

    case VREQ_SET_BUF:
        if (req->bmRequestType & USB_REQ_DIR_IN) return false;
        if (req->wLength > sizeof(store)) return false;
        if (req->wLength == 0) {
            store_len = 0;
            set_buf_count++;
            return true;
        }
        usbd_ep0_receive(store, req->wLength, set_buf_done);
        return true;

    case VREQ_GET_BUF:
        if (!(req->bmRequestType & USB_REQ_DIR_IN)) return false;
        usbd_ep0_reply(store, store_len);
        return true;

    case VREQ_EXP_OUT_DELAY:
        if (req->bmRequestType & USB_REQ_DIR_IN) return false;
        g_exp_delay_us = req->wValue;
        g_exp_mode = req->wIndex;
        return true;

    default:
        return false;
    }
}
