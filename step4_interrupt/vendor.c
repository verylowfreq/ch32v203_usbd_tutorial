/*
 * vendor.c - step4: vendor requests + Bulk EP1 (same as step3) + Interrupt EP2 notifications
 *
 * Interrupt EP2:
 *   IN : 16-byte notifications sent from a queue (NOTIFY_TICK / NOTIFY_ECHO)
 *   OUT: 16-byte commands from the host. The reply (NOTIFY_ECHO) goes onto the IN queue
 *
 * Execution contexts:
 *   - Most of the code runs in the USB interrupt (callbacks from usbd.c)
 *   - Only vendor_poll() runs in the main loop. It pushes the periodic TICK onto the queue
 *     The queue and EP2 IN are shared with the interrupt, so that part runs with interrupts disabled
 */
#include <stdint.h>
#include <stdbool.h>
#include "ch32v203_regs.h"
#include "usbd_hw.h"
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
    uint32_t rx_paused;
    uint8_t  mode;
    uint8_t  fifo_max;
    uint8_t  reserved[2];
} bulk_stats_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;        /* NOTIFY_xxx                         */
    uint8_t  seq;         /* Sequence number (gaps reveal drops) */
    uint16_t frame;       /* USB frame number at creation (FNR)  */
    uint32_t uptime_ms;
    uint32_t arg0;
    uint32_t arg1;
} notify_t;
_Static_assert(sizeof(notify_t) == INT_MPS, "notify size");

static vendor_info_t info;
static uint8_t  store[256];
static uint16_t store_len;
static uint32_t set_buf_count;

/* ================================================================== */
/* Bulk EP1 (same as step3)                                           */
/* ================================================================== */
#define FIFO_N 8
static uint8_t  fifo_data[FIFO_N][BULK_MPS];
static uint8_t  fifo_len[FIFO_N];
static uint8_t  fifo_head, fifo_tail, fifo_count;
static bool     bulk_in_busy;
static bool     bulk_out_paused;
static uint8_t  bulk_in_len;
static uint8_t  mode;
static uint8_t  src_seq;
static bulk_stats_t stats;

static void bulk_kick_in(void)
{
    if (bulk_in_busy) return;
    if (mode == BULK_MODE_LOOPBACK) {
        if (fifo_count == 0) return;
        bulk_in_len = fifo_len[fifo_tail];
        usbd_ep_write(EP_BULK_IN, fifo_data[fifo_tail], bulk_in_len);
        bulk_in_busy = true;
    } else if (mode == BULK_MODE_SOURCE) {
        static uint8_t pkt[BULK_MPS];
        for (int i = 0; i < BULK_MPS; i++) pkt[i] = src_seq++;
        bulk_in_len = BULK_MPS;
        usbd_ep_write(EP_BULK_IN, pkt, bulk_in_len);
        bulk_in_busy = true;
    }
}

static void bulk_reset(void)
{
    fifo_head = fifo_tail = fifo_count = 0;
    bulk_in_busy = false;
    bulk_out_paused = false;
    src_seq = 0;
}

static void bulk_out(void)
{
    if (mode == BULK_MODE_LOOPBACK) {
        uint16_t n = usbd_ep_read(EP_BULK_OUT, fifo_data[fifo_head], BULK_MPS);
        fifo_len[fifo_head] = (uint8_t)n;
        fifo_head = (fifo_head + 1) % FIFO_N;
        fifo_count++;
        if (fifo_count > stats.fifo_max) stats.fifo_max = fifo_count;
        stats.rx_bytes += n;
        stats.rx_packets++;
        bulk_kick_in();
        if (fifo_count < FIFO_N) {
            usbd_ep_rx_enable(EP_BULK_OUT);
        } else {
            bulk_out_paused = true;
            stats.rx_paused++;
        }
    } else {
        stats.rx_bytes += usbd_ep_rx_count(EP_BULK_OUT);
        stats.rx_packets++;
        usbd_ep_rx_enable(EP_BULK_OUT);
    }
}

static void bulk_in_done(void)
{
    if (!bulk_in_busy) return;
    bulk_in_busy = false;
    stats.tx_bytes += bulk_in_len;
    stats.tx_packets++;
    if (mode == BULK_MODE_LOOPBACK) {
        fifo_tail = (fifo_tail + 1) % FIFO_N;
        fifo_count--;
        if (bulk_out_paused) {
            bulk_out_paused = false;
            usbd_ep_rx_enable(EP_BULK_OUT);
        }
    }
    bulk_kick_in();
}

/* ================================================================== */
/* Interrupt EP2: notification queue                                  */
/* ================================================================== */
#define NQ_N 8
static notify_t nq[NQ_N];
static uint8_t  nq_head, nq_tail, nq_count;
static bool     int_in_busy;
static bool     int_out_paused;    /* EP2 OUT is held off because the queue is full */
static uint8_t  notify_seq;
static uint32_t notify_dropped;
static volatile uint16_t notify_period_ms;
static uint32_t tick_count;

/* (Interrupts disabled / inside the interrupt) If IN is idle, send the head of the queue */
static void int_kick_in(void)
{
    if (int_in_busy || nq_count == 0 || !usbd_configured()) return;
    usbd_ep_write(EP_INT_IN, &nq[nq_tail], sizeof(notify_t));
    int_in_busy = true;
}

/* (Interrupts disabled / inside the interrupt) Queue a notification. Returns false if full */
static bool notify_push(uint8_t type, uint32_t arg0, uint32_t arg1)
{
    if (nq_count >= NQ_N) {
        notify_dropped++;
        return false;
    }
    notify_t *n = &nq[nq_head];
    n->type = type;
    n->seq = notify_seq++;
    n->frame = (uint16_t)(USBD_FNR & FNR_FN_MASK);
    n->uptime_ms = millis();
    n->arg0 = arg0;
    n->arg1 = arg1;
    nq_head = (nq_head + 1) % NQ_N;
    nq_count++;
    int_kick_in();
    return true;
}

static void int_reset(void)
{
    nq_head = nq_tail = nq_count = 0;
    int_in_busy = false;
    int_out_paused = false;
}

/* EP2 OUT: command from the host -> answer with an ECHO notification */
static void int_out(void)
{
    uint8_t buf[INT_MPS];
    uint16_t n = usbd_ep_read(EP_INT_OUT, buf, sizeof(buf));
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) sum += buf[i];
    notify_push(NOTIFY_ECHO, n, sum);
    /* Accept the next command only if the queue has room (otherwise stay NAK = flow control) */
    if (nq_count < NQ_N) {
        usbd_ep_rx_enable(EP_INT_OUT);
    } else {
        int_out_paused = true;
    }
}

static void int_in_done(void)
{
    if (!int_in_busy) return;
    int_in_busy = false;
    nq_tail = (nq_tail + 1) % NQ_N;
    nq_count--;
    if (int_out_paused) {
        int_out_paused = false;
        usbd_ep_rx_enable(EP_INT_OUT);
    }
    int_kick_in();
}

/* Main loop: push a TICK notification every period */
void vendor_poll(void)
{
    static uint32_t last;
    uint32_t now = millis();
    uint16_t period = notify_period_ms;
    if (period == 0 || !usbd_configured()) {
        last = now;
        return;
    }
    if ((uint32_t)(now - last) >= period) {
        last += period;
        if ((uint32_t)(now - last) >= period) last = now;   /* Fell far behind: resync */
        uint32_t m = irq_save();           /* Shared with the USB interrupt -> critical section */
        tick_count++;
        notify_push(NOTIFY_TICK, tick_count, notify_dropped);
        irq_restore(m);
    }
}

/* ================================================================== */
/* Callbacks from usbd.c (in the interrupt)                            */
/* ================================================================== */
void vendor_configured(uint8_t config)
{
    bulk_reset();
    int_reset();
    if (config) {
        usbd_ep_rx_enable(EP_BULK_OUT);
        usbd_ep_rx_enable(EP_INT_OUT);
        bulk_kick_in();
    }
}

void vendor_ep_out(uint8_t ep_addr)
{
    if (ep_addr == EP_BULK_OUT) bulk_out();
    else if (ep_addr == EP_INT_OUT) int_out();
}

void vendor_ep_in_done(uint8_t ep_addr)
{
    if (ep_addr == EP_BULK_IN) bulk_in_done();
    else if (ep_addr == EP_INT_IN) int_in_done();
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
        usbd_ep_tx_cancel(EP_BULK_IN);
        mode = (uint8_t)req->wValue;
        bulk_reset();
        stats = (bulk_stats_t){ 0 };
        stats.mode = mode;
        if (usbd_configured()) {
            usbd_ep_rx_enable(EP_BULK_OUT);
            bulk_kick_in();
        }
        return true;

    case VREQ_GET_STATS:
        if (!dir_in) return false;
        stats.mode = mode;
        usbd_ep0_reply(&stats, sizeof(stats));
        return true;

    case VREQ_SET_NOTIFY:
        if (dir_in) return false;
        /* Empty the queue (and cancel the pending IN) before applying the setting */
        usbd_ep_tx_cancel(EP_INT_IN);
        int_reset();
        if (usbd_configured()) usbd_ep_rx_enable(EP_INT_OUT);
        notify_period_ms = req->wValue;
        tick_count = 0;
        notify_dropped = 0;
        notify_seq = 0;
        return true;

    default:
        return false;
    }
}
