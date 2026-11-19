/*
 * usbd_hw.h - Register-level helpers for the CH32V203 USBD (FSDEV-compatible)
 *
 * The helpers in this file are covered in tutorial chapter 02.
 *  - Register map (CNTR / ISTR / FNR / DADDR / BTABLE / EPRn)
 *  - Writing EPRn safely (bits are read-write, write-0-to-clear, or write-1-to-toggle)
 *  - Accessing PMA (the 512-byte packet buffer). It is a 2x16-bit scheme:
 *    a 16-bit word at PMA offset off lives at CPU address 0x40006000 + off*2.
 */
#ifndef USBD_HW_H
#define USBD_HW_H

#include <stdint.h>
#include "ch32v203_regs.h"

/* ------------------------------------------------------------------ */
/* Registers                                                          */
/* ------------------------------------------------------------------ */
#define USBD_BASE       0x40005C00u
#define USBD_EPR(n)     REG32(USBD_BASE + 4u * (n))   /* n = 0..7 */
#define USBD_CNTR       REG32(USBD_BASE + 0x40)
#define USBD_ISTR       REG32(USBD_BASE + 0x44)
#define USBD_FNR        REG32(USBD_BASE + 0x48)
#define USBD_DADDR      REG32(USBD_BASE + 0x4C)
#define USBD_BTABLE     REG32(USBD_BASE + 0x50)

/* CNTR */
#define CNTR_CTRM       (1u << 15)
#define CNTR_PMAOVRM    (1u << 14)
#define CNTR_ERRM       (1u << 13)
#define CNTR_WKUPM      (1u << 12)
#define CNTR_SUSPM      (1u << 11)
#define CNTR_RESETM     (1u << 10)
#define CNTR_SOFM       (1u << 9)
#define CNTR_ESOFM      (1u << 8)
#define CNTR_RESUME     (1u << 4)
#define CNTR_FSUSP      (1u << 3)
#define CNTR_LPMODE     (1u << 2)
#define CNTR_PDWN       (1u << 1)
#define CNTR_FRES       (1u << 0)

/* ISTR (CTR/DIR/EP_ID are read-only; the rest are write-0-to-clear) */
#define ISTR_CTR        (1u << 15)
#define ISTR_PMAOVR     (1u << 14)
#define ISTR_ERR        (1u << 13)
#define ISTR_WKUP       (1u << 12)
#define ISTR_SUSP       (1u << 11)
#define ISTR_RESET      (1u << 10)
#define ISTR_SOF        (1u << 9)
#define ISTR_ESOF       (1u << 8)
#define ISTR_DIR        (1u << 4)
#define ISTR_EP_ID      (0xFu)

/* To clear one ISTR flag, write 0 to that bit only. All other bits must be 1, or they get cleared too. */
#define ISTR_CLEAR(flag) (USBD_ISTR = (uint16_t)~(flag))

/* DADDR */
#define DADDR_EF        (1u << 7)

/* FNR */
#define FNR_FN_MASK     0x7FFu
#define FNR_LCK         (1u << 13)
#define FNR_RXDM        (1u << 14)
#define FNR_RXDP        (1u << 15)

/* ------------------------------------------------------------------ */
/* EPRn bits                                                          */
/*                                                                    */
/*  15      14      13:12    11    10:9   8      7      6      5:4  3:0 */
/*  CTR_RX  DTOG_RX STAT_RX SETUP EPTYPE KIND CTR_TX DTOG_TX STAT_TX EA */
/*  rc_w0   t       t       r     rw     rw   rc_w0  t      t      rw   */
/*                                                                    */
/*  rc_w0 : write 0 clears, write 1 has no effect                      */
/*  t     : write 1 toggles, write 0 has no effect                     */
/* ------------------------------------------------------------------ */
#define EP_CTR_RX       0x8000u
#define EP_DTOG_RX      0x4000u
#define EP_STAT_RX      0x3000u
#define EP_SETUP        0x0800u
#define EP_TYPE_MASK    0x0600u
#define EP_KIND         0x0100u
#define EP_CTR_TX       0x0080u
#define EP_DTOG_TX      0x0040u
#define EP_STAT_TX      0x0030u
#define EP_EA_MASK      0x000Fu

#define EP_TYPE_BULK      0x0000u
#define EP_TYPE_CONTROL   0x0200u
#define EP_TYPE_ISO       0x0400u
#define EP_TYPE_INTERRUPT 0x0600u

/* STAT_RX / STAT_TX values (before shifting) */
#define EP_STAT_DISABLED 0u
#define EP_STAT_STALL    1u
#define EP_STAT_NAK      2u
#define EP_STAT_VALID    3u

#define EP_STAT_RX_SHIFT 12
#define EP_STAT_TX_SHIFT 4

/* Bits written as-is (plain read/write bits) */
#define EP_RW_BITS      (EP_TYPE_MASK | EP_KIND | EP_EA_MASK)
/* Bits that toggle when written as 1 */
#define EP_TOGGLE_BITS  (EP_DTOG_RX | EP_STAT_RX | EP_DTOG_TX | EP_STAT_TX)

/*
 * Basic EPR write pattern:
 *   w = (r & EP_RW_BITS)         keep the plain RW bits as they are
 *     | EP_CTR_RX | EP_CTR_TX    write 1 to the CTR bits = leave them unchanged
 *     | (toggle bits to flip)    write 1 only where we want a change
 */

/* Set STAT_TX to stat (DTOG untouched) */
static inline void ep_set_stat_tx(unsigned ep, unsigned stat)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t w = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX;
    w |= (r ^ (stat << EP_STAT_TX_SHIFT)) & EP_STAT_TX;
    USBD_EPR(ep) = w;
}

/* Set STAT_RX to stat (DTOG untouched) */
static inline void ep_set_stat_rx(unsigned ep, unsigned stat)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t w = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX;
    w |= (r ^ (stat << EP_STAT_RX_SHIFT)) & EP_STAT_RX;
    USBD_EPR(ep) = w;
}

/* Set STAT_RX and STAT_TX in one write */
static inline void ep_set_stat_rxtx(unsigned ep, unsigned stat_rx, unsigned stat_tx)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t want = (stat_rx << EP_STAT_RX_SHIFT) | (stat_tx << EP_STAT_TX_SHIFT);
    uint32_t w = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX;
    w |= (r ^ want) & (EP_STAT_RX | EP_STAT_TX);
    USBD_EPR(ep) = w;
}

/* Clear CTR_RX (write 0 to it only) */
static inline void ep_clear_ctr_rx(unsigned ep)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = (r & EP_RW_BITS) | EP_CTR_TX;
}

/* Clear CTR_TX */
static inline void ep_clear_ctr_tx(unsigned ep)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = (r & EP_RW_BITS) | EP_CTR_RX;
}

/* Reset the data toggle to DATA0 (used for SET_CONFIGURATION / CLEAR_FEATURE(HALT)) */
static inline void ep_reset_dtog_rx(unsigned ep)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX | (r & EP_DTOG_RX);
}
static inline void ep_reset_dtog_tx(unsigned ep)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX | (r & EP_DTOG_TX);
}

/*
 * Initialize an endpoint register in one go.
 *   type_ea : EP_TYPE_xxx | (EP_KIND) | endpoint number
 *   DTOG_RX/DTOG_TX are set to DATA0, STAT_RX/STAT_TX to the given values.
 *   CTR_RX/CTR_TX are cleared.
 */
static inline void ep_init(unsigned ep, uint32_t type_ea, unsigned stat_rx, unsigned stat_tx)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t want = (stat_rx << EP_STAT_RX_SHIFT) | (stat_tx << EP_STAT_TX_SHIFT); /* DTOG = 0 */
    uint32_t w = type_ea;                       /* CTR_RX/CTR_TX = 0 -> cleared */
    w |= (r ^ want) & EP_TOGGLE_BITS;
    USBD_EPR(ep) = w;
}

/* Change only EP_TYPE (used for the CH32 EP0 experiment) */
static inline void ep_set_type(unsigned ep, uint32_t type)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = ((r & EP_RW_BITS) & ~EP_TYPE_MASK) | type | EP_CTR_RX | EP_CTR_TX;
}

/* ------------------------------------------------------------------ */
/* PMA (packet memory area), 512 bytes                                 */
/* ------------------------------------------------------------------ */
#define PMA_BASE        0x40006000u
#define PMA_SIZE        512u

/* 16-bit word at PMA offset off (off must be even) */
static inline volatile uint16_t *pma_word(uint16_t off)
{
    return (volatile uint16_t *)(uintptr_t)(PMA_BASE + (uint32_t)off * 2u);
}

/* Buffer description table (BTABLE). We set USBD_BTABLE = 0, so it starts at PMA offset 0. */
#define BTABLE_OFFSET   0u
#define BT_ADDR_TX(ep)  (*pma_word(BTABLE_OFFSET + (ep) * 8u + 0u))
#define BT_COUNT_TX(ep) (*pma_word(BTABLE_OFFSET + (ep) * 8u + 2u))
#define BT_ADDR_RX(ep)  (*pma_word(BTABLE_OFFSET + (ep) * 8u + 4u))
#define BT_COUNT_RX(ep) (*pma_word(BTABLE_OFFSET + (ep) * 8u + 6u))
#define BT_COUNT_MASK   0x03FFu

/* Encode a receive buffer size for COUNTn_RX (BL_SIZE / NUM_BLOCK) */
static inline uint16_t bt_rx_size(uint16_t size)
{
    if (size > 62u) {
        return (uint16_t)(0x8000u | (((size / 32u) - 1u) << 10)); /* 32-byte blocks */
    }
    return (uint16_t)(((size + 1u) / 2u) << 10);                /* 2-byte blocks */
}

/* CPU memory -> PMA */
static inline void pma_write(uint16_t off, const uint8_t *src, uint16_t len)
{
    volatile uint16_t *p = pma_word(off);
    uint16_t i;
    for (i = 0; i + 1u < len; i += 2u) {
        *p = (uint16_t)(src[i] | ((uint16_t)src[i + 1u] << 8));
        p += 2; /* next 16-bit word is 4 bytes further on the CPU side */
    }
    if (len & 1u) {
        *p = src[len - 1u];
    }
}

/* PMA -> CPU memory */
static inline void pma_read(uint16_t off, uint8_t *dst, uint16_t len)
{
    volatile uint16_t *p = pma_word(off);
    uint16_t i;
    for (i = 0; i + 1u < len; i += 2u) {
        uint16_t w = *p;
        dst[i] = (uint8_t)w;
        dst[i + 1u] = (uint8_t)(w >> 8);
        p += 2;
    }
    if (len & 1u) {
        dst[len - 1u] = (uint8_t)*p;
    }
}

#endif /* USBD_HW_H */
