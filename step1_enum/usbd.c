/*
 * usbd.c - step1: USBD initialization, bus reset, EP0 control transfers, standard requests
 *
 * All USB processing runs inside the USB_LP interrupt.
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
/* EP0 control transfer state                                          */
/* ------------------------------------------------------------------ */
enum ep0_state {
    EP0_IDLE,        /* Waiting for SETUP                         */
    EP0_DATA_IN,     /* Data stage (device -> host)               */
    EP0_DATA_OUT,    /* Data stage (host -> device)               */
    EP0_STATUS_IN,   /* Status stage: device sends ZLP            */
    EP0_STATUS_OUT,  /* Status stage: host sends ZLP              */
    EP0_STALLED,
};

static struct {
    uint8_t        state;
    usb_setup_t    req;
    /* IN data stage */
    const uint8_t *in_ptr;
    uint16_t       in_left;
    bool           in_zlp;     /* Need to send a ZLP at the end              */
    bool           has_in;     /* Request handler set up IN data             */
    /* OUT data stage */
    uint8_t       *out_ptr;
    uint16_t       out_left;
    bool           has_out;
    /* Apply SET_ADDRESS after the status stage completes */
    uint8_t        pending_addr;
} ep0;

static uint8_t ep0_buf[EP0_BUF_SIZE];   /* Work area for building descriptors etc. */

static volatile uint8_t usb_configuration;
static volatile uint32_t usb_reset_count;

/* ------------------------------------------------------------------ */
/* API used by request handlers                                        */
/* ------------------------------------------------------------------ */
/* Register IN data stage contents (truncated to wLength) */
static void ep0_reply(const void *data, uint16_t len)
{
    if (len > ep0.req.wLength) {
        len = ep0.req.wLength;
    }
    ep0.in_ptr = (const uint8_t *)data;
    ep0.in_left = len;
    /* If we send less than wLength and it is a multiple of 64, a ZLP must mark the end */
    ep0.in_zlp = (len < ep0.req.wLength) && ((len % USB_EP0_SIZE) == 0);
    ep0.has_in = true;
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
        ep0.in_zlp = false;    /* A short packet (including a ZLP) ends the transfer */
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

/* Idle EP0 state: wait for SETUP (RX=VALID, TX=NAK) */
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
        ep0_reply(desc_device, sizeof(desc_device));
        return true;
    case USB_DESC_CONFIGURATION:
        if (index != 0) return false;
        ep0_reply(desc_config, desc_config_len);
        return true;
    case USB_DESC_STRING: {
        uint16_t n = desc_get_string(index, ep0_buf, sizeof(ep0_buf));
        if (n == 0) return false;
        ep0_reply(ep0_buf, n);
        return true;
    }
    default:
        /* DEVICE_QUALIFIER etc.: a full-speed-only device answers with STALL */
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
        /* Do not change the address yet: the status stage still runs at address 0 */
        ep0.pending_addr = (uint8_t)(req->wValue & 0x7F);
        dbglog(LOG_SET_ADDR, ep0.pending_addr, 0);
        return true;

    case USB_REQ_SET_CONFIGURATION:
        if (req->wValue > 1) return false;
        usb_configuration = (uint8_t)req->wValue;
        dbglog(LOG_SET_CONFIG, usb_configuration, 0);
        return true;

    case USB_REQ_GET_CONFIGURATION:
        ep0_buf[0] = usb_configuration;
        ep0_reply(ep0_buf, 1);
        return true;

    case USB_REQ_GET_STATUS:
        ep0_buf[0] = 0;   /* Device: bus-powered, no remote wakeup / Interface: 0 / EP0: not halted */
        ep0_buf[1] = 0;
        if (rcpt == USB_REQ_RCPT_ENDPOINT && (req->wIndex & 0x0F) != 0) return false;
        ep0_reply(ep0_buf, 2);
        return true;

    case USB_REQ_GET_INTERFACE:
        if (usb_configuration == 0 || req->wIndex != 0) return false;
        ep0_buf[0] = 0;
        ep0_reply(ep0_buf, 1);
        return true;

    case USB_REQ_SET_INTERFACE:
        return usb_configuration != 0 && req->wIndex == 0 && req->wValue == 0;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        /* step1 has no endpoints, so only a halt on EP0 is accepted (no-op) */
        if (rcpt == USB_REQ_RCPT_ENDPOINT && req->wValue == USB_FEATURE_ENDPOINT_HALT &&
            (req->wIndex & 0x0F) == 0) {
            return true;
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

    /* Discard whatever was in progress. Set both directions to NAK explicitly */
    ep0.has_in = false;
    ep0.has_out = false;
    ep0.pending_addr = 0;
    ep_set_stat_rxtx(0, EP_STAT_NAK, EP_STAT_NAK);

    if (count != 8) {
        ep0_stall();
        return;
    }

    bool ok = false;
    if ((ep0.req.bmRequestType & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        ok = std_request(&ep0.req);
    }

    if (!ok) {
        ep0_stall();
        return;
    }

    if (ep0.req.wLength == 0) {
        ep0_send_status_in();                          /* No data stage */
    } else if (ep0.req.bmRequestType & USB_REQ_DIR_IN) {
        if (!ep0.has_in) { ep0_stall(); return; }
        ep0.state = EP0_DATA_IN;
        ep0_send_next_packet();
    } else {
        /* step1 has no requests with an OUT data stage */
        ep0_stall();
    }
}

/* CTR_RX (not SETUP) = an OUT transaction completed */
static void ep0_on_out(void)
{
    uint16_t count = BT_COUNT_RX(0) & BT_COUNT_MASK;
    dbglog(LOG_EP0_OUT, count, ep0.state);

    switch (ep0.state) {
    case EP0_STATUS_OUT:
        /* Status stage of a control read (ZLP) received -> done */
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

/* CTR_TX = an IN transaction completed (ACK received) */
static void ep0_on_in(void)
{
    ep_clear_ctr_tx(0);
    dbglog(LOG_EP0_IN, ep0.in_left, ep0.state);

    switch (ep0.state) {
    case EP0_DATA_IN:
        if (ep0.in_left > 0 || ep0.in_zlp) {
            ep0_send_next_packet();
        } else {
            /* All data sent -> wait for the status-stage ZLP from the host */
            ep0.state = EP0_STATUS_OUT;
            ep_set_stat_rx(0, EP_STAT_VALID);
        }
        break;
    case EP0_STATUS_IN:
        /* Our status-stage ZLP went out */
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

    /* EP0 buffer descriptors */
    BT_ADDR_TX(0) = PMA_EP0_TX;
    BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0) = PMA_EP0_RX;
    BT_COUNT_RX(0) = bt_rx_size(USB_EP0_SIZE);

    /* EP0 = CONTROL, RX=VALID (accept SETUP), TX=NAK */
    ep_init(0, EP_TYPE_CONTROL | 0, EP_STAT_VALID, EP_STAT_NAK);
    for (unsigned i = 1; i < 8; i++) {
        ep_init(i, i, EP_STAT_DISABLED, EP_STAT_DISABLED);
    }

    ep0.state = EP0_IDLE;
    ep0.pending_addr = 0;
    usb_configuration = 0;

    USBD_DADDR = DADDR_EF;      /* Address 0, function enabled */
    USBD_CNTR &= ~CNTR_FSUSP;   /* Clear suspend detection mask */
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
                    /* A SETUP supersedes everything; drop any pending IN completion */
                    if (epr & EP_CTR_TX) ep_clear_ctr_tx(0);
                    ep0_on_setup(epr);
                    continue;
                }
                if (epr & EP_CTR_TX) ep0_on_in();
                if (epr & EP_CTR_RX) ep0_on_out();
            } else {
                /* step1 has no endpoints besides EP0. Just clear the flags */
                if (epr & EP_CTR_RX) ep_clear_ctr_rx(ep);
                if (epr & EP_CTR_TX) ep_clear_ctr_tx(ep);
            }
            continue;
        }
        if (istr & ISTR_SUSP) {
            /* No traffic on the bus for 3ms. Mask detection with FSUSP so the interrupt does not keep firing */
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
    /* 1. PA11(D-)/PA12(D+): push-pull output low until USBD is enabled (RM recommendation) */
    RCC_APB2PCENR |= RCC_IOPAEN;
    GPIO_OUTDR(GPIOA_BASE) &= ~((1u << 11) | (1u << 12));
    uint32_t cfg = GPIO_CFGHR(GPIOA_BASE);
    cfg &= ~((0xFu << ((11 - 8) * 4)) | (0xFu << ((12 - 8) * 4)));
    cfg |= (GPIO_CFG_OUT_PP_50M << ((11 - 8) * 4)) | (GPIO_CFG_OUT_PP_50M << ((12 - 8) * 4));
    GPIO_CFGHR(GPIOA_BASE) = cfg;

    /* 2. Pull-up off (= disconnected). Gives the host time to see a detach */
    EXTEN_CTR &= ~(EXTEN_USBD_PU | EXTEN_USBD_LS);
    delay_ms(20);

    /* 3. USBD clock on (USBCLK=48MHz is already set up in system_init) + peripheral reset */
    RCC_APB1PCENR |= RCC_USBEN;
    RCC_APB1PRSTR |= RCC_USBRST;
    RCC_APB1PRSTR &= ~RCC_USBRST;

    /* 4. Leave power-down (PDWN=0) while still in forced reset (FRES=1) */
    USBD_CNTR = CNTR_FRES;
    delay_us(10);

    /* 5. BTABLE and address registers */
    USBD_BTABLE = BTABLE_OFFSET;
    USBD_DADDR = 0;

    /* 6. Release forced reset, clear pending flags, enable interrupts */
    USBD_CNTR = 0;
    USBD_ISTR = 0;
    USBD_CNTR = CNTR_CTRM | CNTR_RESETM | CNTR_SUSPM | CNTR_WKUPM | CNTR_ERRM | CNTR_PMAOVRM;

    /* 7. Enable the USB LP interrupt in the PFIC */
    pfic_enable_irq(IRQN_USB_LP_CAN1_RX0);

    /* 8. Pull-up on -> host detects the attach and starts a bus reset */
    EXTEN_CTR |= EXTEN_USBD_PU;
}

uint8_t usbd_configured(void)
{
    return usb_configuration;
}
