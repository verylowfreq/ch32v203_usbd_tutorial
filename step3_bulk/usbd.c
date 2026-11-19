/*
 * usbd.c - step3: step2 + non-control endpoints (Bulk EP1 IN/OUT)
 *
 *  - SET_CONFIGURATION sets up EP1
 *  - CLEAR_FEATURE / SET_FEATURE (ENDPOINT_HALT), GET_STATUS(endpoint)
 *  - Endpoint API: usbd_ep_write / usbd_ep_read / usbd_ep_rx_enable
 *
 * All USB processing runs inside the USB_LP interrupt.
 * (The step2 EP0 experiment code is removed. Its conclusion: EP0 OUT data is read
 *  from the PMA immediately inside the ISR.)
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

/* ------------------------------------------------------------------ */
/* Endpoint definitions (EPRn index = endpoint number)                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  num;       /* Endpoint number (= EPR index)       */
    uint16_t type;      /* EP_TYPE_BULK / EP_TYPE_INTERRUPT     */
    uint16_t rx_pma;    /* OUT buffer (0 = no OUT direction)    */
    uint16_t tx_pma;    /* IN buffer (0 = no IN direction)      */
    uint16_t mps;
} ep_def_t;

static const ep_def_t ep_defs[] = {
    { 1, EP_TYPE_BULK, PMA_EP1_RX, PMA_EP1_TX, BULK_MPS },
};
#define EP_DEF_COUNT (sizeof(ep_defs) / sizeof(ep_defs[0]))

/* What the application wants (used to restore after a halt is cleared) */
static bool ep_rx_armed[8];
static bool ep_tx_armed[8];
static bool ep_halted[8][2];   /* [num][0=OUT,1=IN] */

static const ep_def_t *find_ep(uint8_t num)
{
    for (unsigned i = 0; i < EP_DEF_COUNT; i++) {
        if (ep_defs[i].num == num) return &ep_defs[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* EP0 control transfer state                                          */
/* ------------------------------------------------------------------ */
enum ep0_state {
    EP0_IDLE,
    EP0_DATA_IN,
    EP0_DATA_OUT,
    EP0_STATUS_IN,
    EP0_STATUS_OUT,
    EP0_STALLED,
};

static struct {
    uint8_t        state;
    usb_setup_t    req;
    const uint8_t *in_ptr;
    uint16_t       in_left;
    bool           in_zlp;
    bool           has_in;
    uint8_t       *out_ptr;
    uint16_t       out_left;
    uint16_t       out_received;
    bool           has_out;
    usbd_ep0_out_done_t out_done;
    uint8_t        pending_addr;
} ep0;

static uint8_t ep0_buf[EP0_BUF_SIZE];

static volatile uint8_t usb_configuration;
static volatile uint32_t usb_reset_count;

/* ------------------------------------------------------------------ */
/* Non-control endpoint API                                            */
/* ------------------------------------------------------------------ */
void usbd_ep_write(uint8_t ep_addr, const void *data, uint16_t len)
{
    uint8_t n = ep_addr & 0x0F;
    const ep_def_t *d = find_ep(n);
    if (!d || !d->tx_pma) return;
    if (len > d->mps) len = d->mps;
    pma_write(d->tx_pma, (const uint8_t *)data, len);
    BT_COUNT_TX(n) = len;
    ep_tx_armed[n] = true;
    if (!ep_halted[n][1]) {
        ep_set_stat_tx(n, EP_STAT_VALID);
    }
}

uint16_t usbd_ep_read(uint8_t ep_addr, void *dst, uint16_t maxlen)
{
    uint8_t n = ep_addr & 0x0F;
    const ep_def_t *d = find_ep(n);
    if (!d || !d->rx_pma) return 0;
    uint16_t count = BT_COUNT_RX(n) & BT_COUNT_MASK;
    if (count > maxlen) count = maxlen;
    pma_read(d->rx_pma, (uint8_t *)dst, count);
    return count;
}

uint16_t usbd_ep_rx_count(uint8_t ep_addr)
{
    return BT_COUNT_RX(ep_addr & 0x0F) & BT_COUNT_MASK;
}

void usbd_ep_rx_enable(uint8_t ep_addr)
{
    uint8_t n = ep_addr & 0x0F;
    ep_rx_armed[n] = true;
    if (!ep_halted[n][0]) {
        ep_set_stat_rx(n, EP_STAT_VALID);
    }
}

void usbd_ep_tx_cancel(uint8_t ep_addr)
{
    uint8_t n = ep_addr & 0x0F;
    ep_tx_armed[n] = false;
    if (!ep_halted[n][1]) {
        ep_set_stat_tx(n, EP_STAT_NAK);
    }
}

/* Configure EP1.. (SET_CONFIGURATION) / disable them (config 0, bus reset) */
static void configure_endpoints(bool enable)
{
    for (unsigned i = 0; i < EP_DEF_COUNT; i++) {
        const ep_def_t *d = &ep_defs[i];
        uint8_t n = d->num;
        ep_rx_armed[n] = false;
        ep_tx_armed[n] = false;
        ep_halted[n][0] = false;
        ep_halted[n][1] = false;
        if (enable) {
            BT_ADDR_TX(n) = d->tx_pma;
            BT_COUNT_TX(n) = 0;
            BT_ADDR_RX(n) = d->rx_pma;
            BT_COUNT_RX(n) = d->rx_pma ? bt_rx_size(d->mps) : 0;
            /* Toggle = DATA0. Both directions start NAKed; the app arms them */
            ep_init(n, d->type | n,
                    d->rx_pma ? EP_STAT_NAK : EP_STAT_DISABLED,
                    d->tx_pma ? EP_STAT_NAK : EP_STAT_DISABLED);
        } else {
            ep_init(n, n, EP_STAT_DISABLED, EP_STAT_DISABLED);
        }
    }
}

/* ENDPOINT_HALT */
static bool ep_halt_feature(uint8_t ep_addr, bool set)
{
    uint8_t n = ep_addr & 0x0F;
    bool in = (ep_addr & 0x80) != 0;
    if (n == 0) return true;                       /* EP0: no-op */
    const ep_def_t *d = find_ep(n);
    if (!d || usb_configuration == 0) return false;
    if (in ? !d->tx_pma : !d->rx_pma) return false;

    if (set) {
        ep_halted[n][in] = true;
        if (in) ep_set_stat_tx(n, EP_STAT_STALL);
        else    ep_set_stat_rx(n, EP_STAT_STALL);
        dbglog(LOG_SET_HALT, ep_addr, 0);
    } else {
        /* CLEAR_FEATURE(HALT) always resets the toggle to DATA0 (even if not halted) */
        ep_halted[n][in] = false;
        if (in) {
            ep_reset_dtog_tx(n);
            ep_set_stat_tx(n, ep_tx_armed[n] ? EP_STAT_VALID : EP_STAT_NAK);
        } else {
            ep_reset_dtog_rx(n);
            ep_set_stat_rx(n, ep_rx_armed[n] ? EP_STAT_VALID : EP_STAT_NAK);
        }
        dbglog(LOG_CLR_HALT, ep_addr, 0);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* API used by request handlers                                        */
/* ------------------------------------------------------------------ */
void usbd_ep0_reply(const void *data, uint16_t len)
{
    if (len > ep0.req.wLength) {
        len = ep0.req.wLength;
    }
    ep0.in_ptr = (const uint8_t *)data;
    ep0.in_left = len;
    ep0.in_zlp = (len < ep0.req.wLength) && ((len % USB_EP0_SIZE) == 0);
    ep0.has_in = true;
}

void usbd_ep0_receive(void *buf, uint16_t len, usbd_ep0_out_done_t done)
{
    ep0.out_ptr = (uint8_t *)buf;
    ep0.out_left = len < ep0.req.wLength ? len : ep0.req.wLength;
    ep0.out_received = 0;
    ep0.out_done = done;
    ep0.has_out = true;
}

/* ------------------------------------------------------------------ */
/* EP0 low-level operations                                            */
/* ------------------------------------------------------------------ */
static void ep0_send_next_packet(void)
{
    uint16_t n = ep0.in_left > USB_EP0_SIZE ? USB_EP0_SIZE : ep0.in_left;
    pma_write(PMA_EP0_TX, ep0.in_ptr, n);
    BT_COUNT_TX(0) = n;
    ep0.in_ptr += n;
    ep0.in_left -= n;
    if (n < USB_EP0_SIZE) {
        ep0.in_zlp = false;
    }
    ep_set_stat_tx(0, EP_STAT_VALID);
}

static void ep0_send_status_in(void)
{
    BT_COUNT_TX(0) = 0;
    ep0.state = EP0_STATUS_IN;
    ep_set_stat_tx(0, EP_STAT_VALID);
}

static void ep0_stall(void)
{
    ep0.state = EP0_STALLED;
    ep_set_stat_rxtx(0, EP_STAT_STALL, EP_STAT_STALL);
    dbglog(LOG_STALL, (uint16_t)(ep0.req.bmRequestType | (ep0.req.bRequest << 8)),
           (uint32_t)ep0.req.wValue | ((uint32_t)ep0.req.wIndex << 16));
}

static void ep0_go_idle(void)
{
    ep0.state = EP0_IDLE;
    ep_set_stat_rxtx(0, EP_STAT_VALID, EP_STAT_NAK);
}

/* ------------------------------------------------------------------ */
/* Standard requests                                                   */
/* ------------------------------------------------------------------ */
static bool std_get_descriptor(const usb_setup_t *req)
{
    uint8_t type = (uint8_t)(req->wValue >> 8);
    uint8_t index = (uint8_t)(req->wValue & 0xFF);

    switch (type) {
    case USB_DESC_DEVICE:
        usbd_ep0_reply(desc_device, sizeof(desc_device));
        return true;
    case USB_DESC_CONFIGURATION:
        if (index != 0) return false;
        usbd_ep0_reply(desc_config, desc_config_len);
        return true;
    case USB_DESC_STRING: {
        uint16_t n = desc_get_string(index, ep0_buf, sizeof(ep0_buf));
        if (n == 0) return false;
        usbd_ep0_reply(ep0_buf, n);
        return true;
    }
    case USB_DESC_BOS:
        usbd_ep0_reply(desc_bos, desc_bos_len);
        return true;
    default:
        return false;
    }
}

static bool std_request(const usb_setup_t *req)
{
    uint8_t rcpt = req->bmRequestType & USB_REQ_RCPT_MASK;

    switch (req->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
        return std_get_descriptor(req);

    case USB_REQ_SET_ADDRESS:
        ep0.pending_addr = (uint8_t)(req->wValue & 0x7F);
        dbglog(LOG_SET_ADDR, ep0.pending_addr, 0);
        return true;

    case USB_REQ_SET_CONFIGURATION:
        if (req->wValue > 1) return false;
        usb_configuration = (uint8_t)req->wValue;
        configure_endpoints(usb_configuration != 0);     /* step3 */
        vendor_configured(usb_configuration);
        dbglog(LOG_SET_CONFIG, usb_configuration, 0);
        return true;

    case USB_REQ_GET_CONFIGURATION:
        ep0_buf[0] = usb_configuration;
        usbd_ep0_reply(ep0_buf, 1);
        return true;

    case USB_REQ_GET_STATUS:
        ep0_buf[0] = 0;
        ep0_buf[1] = 0;
        if (rcpt == USB_REQ_RCPT_ENDPOINT) {
            uint8_t n = req->wIndex & 0x0F;
            bool in = (req->wIndex & 0x80) != 0;
            if (n != 0) {
                if (!find_ep(n) || usb_configuration == 0) return false;
                ep0_buf[0] = ep_halted[n][in] ? 1 : 0;   /* bit0 = Halt */
            }
        }
        usbd_ep0_reply(ep0_buf, 2);
        return true;

    case USB_REQ_GET_INTERFACE:
        if (usb_configuration == 0 || req->wIndex != 0) return false;
        ep0_buf[0] = 0;
        usbd_ep0_reply(ep0_buf, 1);
        return true;

    case USB_REQ_SET_INTERFACE:
        return usb_configuration != 0 && req->wIndex == 0 && req->wValue == 0;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        if (rcpt == USB_REQ_RCPT_ENDPOINT && req->wValue == USB_FEATURE_ENDPOINT_HALT) {
            return ep_halt_feature((uint8_t)req->wIndex, req->bRequest == USB_REQ_SET_FEATURE);
        }
        return false;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* EP0 event handling                                                  */
/* ------------------------------------------------------------------ */
static void ep0_on_setup(uint32_t epr_at_entry)
{
    uint16_t count = BT_COUNT_RX(0) & BT_COUNT_MASK;
    pma_read(PMA_EP0_RX, (uint8_t *)&ep0.req, 8);
    ep_clear_ctr_rx(0);

    dbglog(LOG_SETUP_EPR, (uint16_t)epr_at_entry, ep0.req.wLength);
    dbglog(LOG_SETUP, (uint16_t)(ep0.req.bmRequestType | (ep0.req.bRequest << 8)),
           (uint32_t)ep0.req.wValue | ((uint32_t)ep0.req.wIndex << 16));

    ep0.has_in = false;
    ep0.has_out = false;
    ep0.pending_addr = 0;
    ep_set_stat_rxtx(0, EP_STAT_NAK, EP_STAT_NAK);

    if (count != 8) {
        ep0_stall();
        return;
    }

    bool ok = false;
    switch (ep0.req.bmRequestType & USB_REQ_TYPE_MASK) {
    case USB_REQ_TYPE_STANDARD:
        ok = std_request(&ep0.req);
        break;
    case USB_REQ_TYPE_VENDOR:
        ok = vendor_control_request(&ep0.req);
        break;
    default:
        break;
    }

    if (!ok) {
        ep0_stall();
        return;
    }

    if (ep0.req.wLength == 0) {
        ep0_send_status_in();
    } else if (ep0.req.bmRequestType & USB_REQ_DIR_IN) {
        if (!ep0.has_in) { ep0_stall(); return; }
        ep0.state = EP0_DATA_IN;
        ep0_send_next_packet();
    } else {
        if (!ep0.has_out) { ep0_stall(); return; }
        ep0.state = EP0_DATA_OUT;
        /* Note: on CH32, a CONTROL-type EP accepts OUT even with STAT_RX=NAK (chapter 09).
         * Every received packet must be read from the PMA right away in the ISR. */
        ep_set_stat_rx(0, EP_STAT_VALID);
    }
}

static void ep0_on_out(void)
{
    uint16_t count = BT_COUNT_RX(0) & BT_COUNT_MASK;
    dbglog(LOG_EP0_OUT, count, ep0.state);

    switch (ep0.state) {
    case EP0_DATA_OUT: {
        uint16_t n = count > ep0.out_left ? ep0.out_left : count;
        pma_read(PMA_EP0_RX, ep0.out_ptr, n);
        ep0.out_ptr += n;
        ep0.out_left -= n;
        ep0.out_received += n;
        ep_clear_ctr_rx(0);
        if (ep0.out_left == 0 || count < USB_EP0_SIZE) {
            bool ok = ep0.out_done ? ep0.out_done(&ep0.req, ep0.out_received) : true;
            if (ok) {
                ep0_send_status_in();
            } else {
                ep0_stall();
            }
        } else {
            ep_set_stat_rx(0, EP_STAT_VALID);
        }
        break;
    }
    case EP0_STATUS_OUT:
        ep_clear_ctr_rx(0);
        ep0_go_idle();
        break;
    default:
        dbglog(LOG_EP0_UNEXP, (uint16_t)USBD_EPR(0), ep0.state);
        ep_clear_ctr_rx(0);
        ep0_go_idle();
        break;
    }
}

static void ep0_on_in(void)
{
    ep_clear_ctr_tx(0);
    dbglog(LOG_EP0_IN, ep0.in_left, ep0.state);

    switch (ep0.state) {
    case EP0_DATA_IN:
        if (ep0.in_left > 0 || ep0.in_zlp) {
            ep0_send_next_packet();
        } else {
            ep0.state = EP0_STATUS_OUT;
            ep_set_stat_rx(0, EP_STAT_VALID);
        }
        break;
    case EP0_STATUS_IN:
        if (ep0.pending_addr) {
            USBD_DADDR = DADDR_EF | ep0.pending_addr;
            ep0.pending_addr = 0;
        }
        ep0_go_idle();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bus reset                                                           */
/* ------------------------------------------------------------------ */
static void usbd_bus_reset(void)
{
    usb_reset_count++;
    dbglog(LOG_BUS_RESET, (uint16_t)usb_reset_count, 0);

    USBD_BTABLE = BTABLE_OFFSET;
    BT_ADDR_TX(0) = PMA_EP0_TX;
    BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0) = PMA_EP0_RX;
    BT_COUNT_RX(0) = bt_rx_size(USB_EP0_SIZE);

    ep_init(0, EP_TYPE_CONTROL | 0, EP_STAT_VALID, EP_STAT_NAK);
    for (unsigned i = 1; i < 8; i++) {
        ep_init(i, i, EP_STAT_DISABLED, EP_STAT_DISABLED);
    }

    ep0.state = EP0_IDLE;
    ep0.pending_addr = 0;
    if (usb_configuration) {
        usb_configuration = 0;
        configure_endpoints(false);
        vendor_configured(0);
    }

    USBD_DADDR = DADDR_EF;
    USBD_CNTR &= ~CNTR_FSUSP;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                   */
/* ------------------------------------------------------------------ */
void __attribute__((interrupt)) USB_LP_CAN1_RX0_IRQHandler(void)
{
    for (;;) {
        uint32_t istr = USBD_ISTR;

        if (istr & ISTR_RESET) {
            ISTR_CLEAR(ISTR_RESET);
            usbd_bus_reset();
            continue;
        }
        if (istr & ISTR_CTR) {
            unsigned ep = istr & ISTR_EP_ID;
            uint32_t epr = USBD_EPR(ep);
            if (ep == 0) {
                if ((epr & EP_CTR_RX) && (epr & EP_SETUP)) {
                    if (epr & EP_CTR_TX) ep_clear_ctr_tx(0);
                    ep0_on_setup(epr);
                    continue;
                }
                if (epr & EP_CTR_TX) ep0_on_in();
                if (epr & EP_CTR_RX) ep0_on_out();
            } else {
                /* step3: non-control endpoints. Clear CTR, then hand off to the app.
                 * Once CTR_RX is set, hardware has made STAT_RX=NAK, so the data stays in the PMA
                 * (BULK/INTERRUPT types honor NAK, unlike CONTROL). */
                if (epr & EP_CTR_RX) {
                    ep_clear_ctr_rx(ep);
                    ep_rx_armed[ep] = false;
                    vendor_ep_out((uint8_t)ep);
                }
                if (epr & EP_CTR_TX) {
                    ep_clear_ctr_tx(ep);
                    ep_tx_armed[ep] = false;
                    vendor_ep_in_done((uint8_t)(0x80 | ep));
                }
            }
            continue;
        }
        if (istr & ISTR_SUSP) {
            USBD_CNTR |= CNTR_FSUSP;
            ISTR_CLEAR(ISTR_SUSP);
            dbglog(LOG_SUSPEND, 0, 0);
            continue;
        }
        if (istr & ISTR_WKUP) {
            USBD_CNTR &= ~CNTR_FSUSP;
            ISTR_CLEAR(ISTR_WKUP);
            dbglog(LOG_WAKEUP, 0, 0);
            continue;
        }
        if (istr & ISTR_ERR) {
            ISTR_CLEAR(ISTR_ERR);
            continue;
        }
        if (istr & ISTR_PMAOVR) {
            ISTR_CLEAR(ISTR_PMAOVR);
            dbglog(LOG_PMAOVR, 0, 0);
            continue;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */
void usbd_init(void)
{
    RCC_APB2PCENR |= RCC_IOPAEN;
    GPIO_OUTDR(GPIOA_BASE) &= ~((1u << 11) | (1u << 12));
    uint32_t cfg = GPIO_CFGHR(GPIOA_BASE);
    cfg &= ~((0xFu << ((11 - 8) * 4)) | (0xFu << ((12 - 8) * 4)));
    cfg |= (GPIO_CFG_OUT_PP_50M << ((11 - 8) * 4)) | (GPIO_CFG_OUT_PP_50M << ((12 - 8) * 4));
    GPIO_CFGHR(GPIOA_BASE) = cfg;

    EXTEN_CTR &= ~(EXTEN_USBD_PU | EXTEN_USBD_LS);
    delay_ms(20);

    RCC_APB1PCENR |= RCC_USBEN;
    RCC_APB1PRSTR |= RCC_USBRST;
    RCC_APB1PRSTR &= ~RCC_USBRST;

    USBD_CNTR = CNTR_FRES;
    delay_us(10);

    USBD_BTABLE = BTABLE_OFFSET;
    USBD_DADDR = 0;

    USBD_CNTR = 0;
    USBD_ISTR = 0;
    USBD_CNTR = CNTR_CTRM | CNTR_RESETM | CNTR_SUSPM | CNTR_WKUPM | CNTR_ERRM | CNTR_PMAOVRM;

    pfic_enable_irq(IRQN_USB_LP_CAN1_RX0);

    EXTEN_CTR |= EXTEN_USBD_PU;
}

uint8_t usbd_configured(void)
{
    return usb_configuration;
}
