/*
 * vendor.c - step3: vendor requests + Bulk EP1 loopback / sink / source
 *
 * Everything runs in the USB interrupt (callbacks from usbd.c), so no locking is needed.
 *
 *  Loopback flow control:
 *    OUT packet arrives -> store in FIFO -> if FIFO has room, accept the next OUT (STAT_RX=VALID)
 *                                         if FIFO is full, leave STAT_RX=NAK (host is NAKed and waits)
 *    IN idle and FIFO not empty -> send the head packet
 *    IN finished -> free the FIFO slot -> re-enable OUT if it was paused
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
    char     magic[4];
    uint8_t  step;
    uint8_t  reserved[3];
    uint32_t uptime_ms;
    uint32_t set_buf_count;
} vendor_info_t;

typedef struct __attribute__((packed)) {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_paused;      /* Times OUT was held off because the FIFO was full */
    uint8_t  mode;
    uint8_t  fifo_max;       /* Highest FIFO occupancy seen */
    uint8_t  reserved[2];
} bulk_stats_t;

static vendor_info_t info;
static uint8_t  store[256];
static uint16_t store_len;
static uint32_t set_buf_count;

/* ---- Bulk ---- */
#define FIFO_N 8
static uint8_t  fifo_data[FIFO_N][BULK_MPS];
static uint8_t  fifo_len[FIFO_N];
static uint8_t  fifo_head, fifo_tail, fifo_count;
static bool     in_busy;
static bool     out_paused;
static uint8_t  in_len;           /* Length of the packet being sent */
static uint8_t  mode;
static uint8_t  src_seq;          /* Source-mode pattern counter */
static bulk_stats_t stats;

static void kick_in(void)
{
    if (in_busy) return;
    if (mode == BULK_MODE_LOOPBACK) {
        if (fifo_count == 0) return;
        in_len = fifo_len[fifo_tail];
        usbd_ep_write(EP_BULK_IN, fifo_data[fifo_tail], in_len);
        in_busy = true;
    } else if (mode == BULK_MODE_SOURCE) {
        /* Pattern: running counter. The host can check continuity */
        static uint8_t pkt[BULK_MPS];
        for (int i = 0; i < BULK_MPS; i++) pkt[i] = src_seq++;
        in_len = BULK_MPS;
        usbd_ep_write(EP_BULK_IN, pkt, in_len);
        in_busy = true;
    }
}

static void bulk_reset(void)
{
    fifo_head = fifo_tail = fifo_count = 0;
    in_busy = false;
    out_paused = false;
    src_seq = 0;
}

void vendor_configured(uint8_t config)
{
    bulk_reset();
    if (config) {
        usbd_ep_rx_enable(EP_BULK_OUT);   /* Start accepting OUT */
        kick_in();                        /* Source mode starts sending right away */
    }
}

void vendor_ep_out(uint8_t ep_addr)
{
    if (ep_addr != EP_BULK_OUT) return;

    if (mode == BULK_MODE_LOOPBACK) {
        uint16_t n = usbd_ep_read(EP_BULK_OUT, fifo_data[fifo_head], BULK_MPS);
        fifo_len[fifo_head] = (uint8_t)n;
        fifo_head = (fifo_head + 1) % FIFO_N;
        fifo_count++;
        if (fifo_count > stats.fifo_max) stats.fifo_max = fifo_count;
        stats.rx_bytes += n;
        stats.rx_packets++;
        kick_in();
        if (fifo_count < FIFO_N) {
            usbd_ep_rx_enable(EP_BULK_OUT);
        } else {
            out_paused = true;            /* Full: leave STAT_RX=NAK */
            stats.rx_paused++;
        }
    } else {
        /* Sink (and stray OUTs during source): count only, discard the data */
        uint16_t n = usbd_ep_rx_count(EP_BULK_OUT);
        stats.rx_bytes += n;
        stats.rx_packets++;
        usbd_ep_rx_enable(EP_BULK_OUT);
    }
}

void vendor_ep_in_done(uint8_t ep_addr)
{
    if (ep_addr != EP_BULK_IN || !in_busy) return;
    in_busy = false;
    stats.tx_bytes += in_len;
    stats.tx_packets++;

    if (mode == BULK_MODE_LOOPBACK) {
        fifo_tail = (fifo_tail + 1) % FIFO_N;
        fifo_count--;
        if (out_paused) {
            out_paused = false;
            usbd_ep_rx_enable(EP_BULK_OUT);
        }
    }
    kick_in();
}

static bool set_buf_done(const usb_setup_t *req, uint16_t received)
{
    store_len = received;
    set_buf_count++;
    return true;
}

bool vendor_control_request(const usb_setup_t *req)
{
    dbglog(LOG_VENDOR, req->bRequest, req->wLength);

    if (req->bRequest == MS_VENDOR_CODE) {
        if (req->wIndex == MS_OS_20_DESCRIPTOR_INDEX && (req->bmRequestType & USB_REQ_DIR_IN)) {
            usbd_ep0_reply(desc_ms_os_20, desc_ms_os_20_len);
            return true;
        }
        return false;
    }

    bool dir_in = (req->bmRequestType & USB_REQ_DIR_IN) != 0;

    switch (req->bRequest) {
    case VREQ_GET_INFO:
        if (!dir_in) return false;
        info.magic[0] = 'V'; info.magic[1] = '2'; info.magic[2] = '0'; info.magic[3] = '3';
        info.step = VENDOR_STEP;
        info.uptime_ms = millis();
        info.set_buf_count = set_buf_count;
        usbd_ep0_reply(&info, sizeof(info));
        return true;

    case VREQ_SET_BUF:
        if (dir_in || req->wLength > sizeof(store)) return false;
        if (req->wLength == 0) {
            store_len = 0;
            set_buf_count++;
            return true;
        }
        usbd_ep0_receive(store, req->wLength, set_buf_done);
        return true;

    case VREQ_GET_BUF:
        if (!dir_in) return false;
        usbd_ep0_reply(store, store_len);
        return true;

    case VREQ_SET_MODE:
        if (dir_in || req->wValue > BULK_MODE_SOURCE) return false;
        /* Cancel the pending IN and empty the FIFO before switching */
        usbd_ep_tx_cancel(EP_BULK_IN);
        mode = (uint8_t)req->wValue;
        bulk_reset();
        stats = (bulk_stats_t){ 0 };
        stats.mode = mode;
        if (usbd_configured()) {
            usbd_ep_rx_enable(EP_BULK_OUT);
            kick_in();
        }
        return true;

    case VREQ_GET_STATS:
        if (!dir_in) return false;
        stats.mode = mode;
        usbd_ep0_reply(&stats, sizeof(stats));
        return true;

    default:
        return false;
    }
}
