/*
 * ch32v203_regs.h - Minimal register definitions for the CH32V203 (D6: F8/G8/K8/C8)
 *
 * Only the registers this tutorial uses. Addresses and bits are from
 * CH32FV2x_V3xRM (V2.5) and the CH32V203 datasheet.
 * We deliberately do not use the WCH SDK's ch32v20x.h, so that every
 * register touched appears in this file.
 */
#ifndef CH32V203_REGS_H
#define CH32V203_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))

/* ------------------------------------------------------------------ */
/* RCC (reset and clock control)  base 0x40021000                     */
/* ------------------------------------------------------------------ */
#define RCC_BASE        0x40021000u
#define RCC_CTLR        REG32(RCC_BASE + 0x00)
#define RCC_CFGR0       REG32(RCC_BASE + 0x04)
#define RCC_INTR        REG32(RCC_BASE + 0x08)
#define RCC_APB2PRSTR   REG32(RCC_BASE + 0x0C)
#define RCC_APB1PRSTR   REG32(RCC_BASE + 0x10)
#define RCC_AHBPCENR    REG32(RCC_BASE + 0x14)
#define RCC_APB2PCENR   REG32(RCC_BASE + 0x18)
#define RCC_APB1PCENR   REG32(RCC_BASE + 0x1C)

/* RCC_CTLR */
#define RCC_HSION       (1u << 0)
#define RCC_HSIRDY      (1u << 1)
#define RCC_PLLON       (1u << 24)
#define RCC_PLLRDY      (1u << 25)

/* RCC_CFGR0 */
#define RCC_SW_MASK     (3u << 0)
#define RCC_SW_PLL      (2u << 0)
#define RCC_SWS_MASK    (3u << 2)
#define RCC_SWS_PLL     (2u << 2)
#define RCC_HPRE_MASK   (0xFu << 4)    /* AHB prescaler. 0 = /1          */
#define RCC_PPRE1_MASK  (7u << 8)      /* APB1 prescaler                 */
#define RCC_PPRE1_DIV2  (4u << 8)
#define RCC_PPRE2_MASK  (7u << 11)     /* APB2 prescaler. 0 = /1         */
#define RCC_PLLSRC      (1u << 16)     /* 0: HSI side, 1: HSE            */
#define RCC_PLLXTPRE    (1u << 17)
#define RCC_PLLMUL_MASK (0xFu << 18)
#define RCC_PLLMUL_18   (0xFu << 18)   /* On CH32V20x, 1111 = x18 (STM32 uses it for x16) */
#define RCC_USBPRE_MASK (3u << 22)
#define RCC_USBPRE_DIV1 (0u << 22)     /* PLL=48MHz  -> USBCLK 48MHz */
#define RCC_USBPRE_DIV2 (1u << 22)     /* PLL=96MHz  -> USBCLK 48MHz */
#define RCC_USBPRE_DIV3 (2u << 22)     /* PLL=144MHz -> USBCLK 48MHz */

/* RCC_APB2PCENR */
#define RCC_AFIOEN      (1u << 0)
#define RCC_IOPAEN      (1u << 2)
/* RCC_APB1PCENR / RCC_APB1PRSTR */
#define RCC_USBEN       (1u << 23)     /* USBD (FSDEV) */
#define RCC_USBRST      (1u << 23)

/* ------------------------------------------------------------------ */
/* EXTEN (extended configuration)  base 0x40023800                     */
/* ------------------------------------------------------------------ */
#define EXTEN_BASE      0x40023800u
#define EXTEN_CTR       REG32(EXTEN_BASE + 0x00)
#define EXTEN_USBD_LS   (1u << 0)      /* 1: low-speed mode / 0: full-speed mode */
#define EXTEN_USBD_PU   (1u << 1)      /* Internal 1.5k pull-up (D+ in full-speed mode) */
#define EXTEN_HSIPRE    (1u << 4)      /* 1: feed HSI to the PLL undivided (8MHz) */

/* ------------------------------------------------------------------ */
/* GPIO  base GPIOA = 0x40010800                                       */
/* ------------------------------------------------------------------ */
#define GPIOA_BASE      0x40010800u
#define GPIO_CFGLR(b)   REG32((b) + 0x00)   /* Pins 0-7, 4 bits per pin  */
#define GPIO_CFGHR(b)   REG32((b) + 0x04)   /* Pins 8-15, 4 bits per pin */
#define GPIO_INDR(b)    REG32((b) + 0x08)
#define GPIO_OUTDR(b)   REG32((b) + 0x0C)
#define GPIO_BSHR(b)    REG32((b) + 0x10)
#define GPIO_BCR(b)     REG32((b) + 0x14)
/* Per-pin 4-bit value: CNF[1:0]:MODE[1:0] */
#define GPIO_CFG_IN_FLOAT   0x4u
#define GPIO_CFG_OUT_PP_50M 0x3u

/* ------------------------------------------------------------------ */
/* PFIC (QingKe V4 interrupt controller)  base 0xE000E000              */
/* ------------------------------------------------------------------ */
#define PFIC_BASE       0xE000E000u
#define PFIC_ISR(n)     REG32(PFIC_BASE + 0x000 + 4 * (n))  /* Interrupt enable status */
#define PFIC_IPR(n)     REG32(PFIC_BASE + 0x020 + 4 * (n))  /* Interrupt pending status */
#define PFIC_IENR(n)    REG32(PFIC_BASE + 0x100 + 4 * (n))  /* Write 1 to enable  */
#define PFIC_IRER(n)    REG32(PFIC_BASE + 0x180 + 4 * (n))  /* Write 1 to disable */
#define PFIC_IPRIOR(n)  (*(volatile uint8_t *)(uintptr_t)(PFIC_BASE + 0x400 + (n)))
#define PFIC_SCTLR      REG32(PFIC_BASE + 0xD10)

static inline void pfic_enable_irq(unsigned irqn)  { PFIC_IENR(irqn >> 5) = 1u << (irqn & 31); }
static inline void pfic_disable_irq(unsigned irqn) { PFIC_IRER(irqn >> 5) = 1u << (irqn & 31); }

/* Interrupt numbers (= vector table index) */
#define IRQN_SYSTICK            12
#define IRQN_USB_HP_CAN1_TX     35
#define IRQN_USB_LP_CAN1_RX0    36
#define IRQN_USBWAKEUP          58

/* ------------------------------------------------------------------ */
/* SysTick (QingKe V4 64-bit counter)  base 0xE000F000                 */
/* ------------------------------------------------------------------ */
#define STK_BASE        0xE000F000u
#define STK_CTLR        REG32(STK_BASE + 0x00)
#define STK_SR          REG32(STK_BASE + 0x04)
#define STK_CNTL        REG32(STK_BASE + 0x08)
#define STK_CNTH        REG32(STK_BASE + 0x0C)
#define STK_CMPL        REG32(STK_BASE + 0x10)
#define STK_CMPH        REG32(STK_BASE + 0x14)
#define STK_CTLR_STE    (1u << 0)   /* Counter enable                 */
#define STK_CTLR_STIE   (1u << 1)   /* Interrupt enable               */
#define STK_CTLR_STCLK  (1u << 2)   /* 0: HCLK/8, 1: HCLK             */
#define STK_CTLR_STRE   (1u << 3)   /* Auto reload                    */

/* ------------------------------------------------------------------ */
/* Electronic signature (chip-unique ID)                               */
/* ------------------------------------------------------------------ */
#define ESIG_FLACAP     REG16(0x1FFFF7E0u)  /* Flash size in KB  */
#define ESIG_UNIID1     REG32(0x1FFFF7E8u)
#define ESIG_UNIID2     REG32(0x1FFFF7ECu)
#define ESIG_UNIID3     REG32(0x1FFFF7F0u)

/* ------------------------------------------------------------------ */
/* Interrupt enable/disable (machine mode: mstatus.MIE)                */
/* ------------------------------------------------------------------ */
static inline uint32_t irq_save(void)
{
    uint32_t m;
    __asm volatile("csrrci %0, mstatus, 8" : "=r"(m) :: "memory");
    return m;
}
static inline void irq_restore(uint32_t m)
{
    if (m & 8u) __asm volatile("csrsi mstatus, 8" ::: "memory");
}
static inline void irq_enable_global(void) { __asm volatile("csrsi mstatus, 8" ::: "memory"); }

#endif /* CH32V203_REGS_H */
