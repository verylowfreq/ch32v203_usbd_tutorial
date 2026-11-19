/*
 * system.c - Clock initialization (HSI -> PLL 144MHz, USBCLK 48MHz) and time base
 *
 * The board has no crystal, so the internal HSI (8MHz) is the clock source.
 *   HSI 8MHz --(EXTEN.HSIPRE=1: undivided)--> PLL x18 = 144MHz = SYSCLK
 *   USBCLK   = PLLCLK / 3 = 48MHz
 *   HCLK     = SYSCLK / 1 = 144MHz
 *   PCLK2    = HCLK / 1, PCLK1 = HCLK / 2 = 72MHz (USBD sits on APB1)
 */
#include "ch32v203_regs.h"
#include "system.h"

volatile uint32_t g_fault[4];

/* Expected values of the CFGR0 fields we set up (checked by system_clock_guard) */
#define CFGR0_CHECK_MASK (RCC_SW_MASK | RCC_PLLSRC | RCC_PLLXTPRE | RCC_PLLMUL_MASK | RCC_USBPRE_MASK)
#define CFGR0_EXPECTED   (RCC_SW_PLL | RCC_PLLMUL_18 | RCC_USBPRE_DIV3)

volatile uint32_t g_clock_repairs;

static void pll_setup(void)
{
    /* 1. Feed HSI to the PLL undivided (HSIPRE can only be written while the PLL is off) */
    EXTEN_CTR |= EXTEN_HSIPRE;

    /* 2. Bus prescalers, PLL source/multiplier, USB prescaler */
    uint32_t cfg = RCC_CFGR0;
    cfg &= ~(RCC_HPRE_MASK | RCC_PPRE1_MASK | RCC_PPRE2_MASK |
             RCC_PLLSRC | RCC_PLLXTPRE | RCC_PLLMUL_MASK | RCC_USBPRE_MASK);
    cfg |= RCC_PPRE1_DIV2;      /* APB1 = HCLK/2                  */
    cfg |= RCC_PLLMUL_18;       /* PLL = 8MHz x 18 = 144MHz       */
    cfg |= RCC_USBPRE_DIV3;     /* USBCLK = 144 / 3 = 48MHz       */
    RCC_CFGR0 = cfg;

    /* 3. Start the PLL */
    RCC_CTLR |= RCC_PLLON;
    while (!(RCC_CTLR & RCC_PLLRDY)) {
    }

    /* 4. Switch SYSCLK to the PLL */
    RCC_CFGR0 = (RCC_CFGR0 & ~RCC_SW_MASK) | RCC_SW_PLL;
    while ((RCC_CFGR0 & RCC_SWS_MASK) != RCC_SWS_PLL) {
    }
}

/*
 * Clock guard (development aid).
 * In this environment, when wlink (WCH-LinkE v2.16) attaches, it rewrites RCC_CFGR0
 * (clears the PLLMUL bit19). SYSCLK/USBCLK go off frequency and USB stops working.
 * Calling this from the main loop detects the change and redoes the PLL setup
 * (USB glitches briefly, but the host retries and communication continues).
 * Background: tutorial chapter 09.
 */
bool system_clock_guard(void)
{
    if ((RCC_CFGR0 & CFGR0_CHECK_MASK) == CFGR0_EXPECTED) {
        return false;
    }
    uint32_t m = irq_save();
    /* Move SYSCLK to HSI -> stop the PLL -> set it up again */
    RCC_CFGR0 &= ~RCC_SW_MASK;
    while ((RCC_CFGR0 & RCC_SWS_MASK) != 0) {
    }
    RCC_CTLR &= ~RCC_PLLON;
    while (RCC_CTLR & RCC_PLLRDY) {
    }
    pll_setup();
    g_clock_repairs++;
    irq_restore(m);
    return true;
}

void system_init(void)
{
    /* 0. Make sure HSI is running (it is on after reset) */
    RCC_CTLR |= RCC_HSION;
    while (!(RCC_CTLR & RCC_HSIRDY)) {
    }

    /* 1-4. PLL 144MHz, USBCLK 48MHz */
    pll_setup();

    /* 5. SysTick: free-running up counter at HCLK/8 = 18MHz (no interrupt) */
    STK_CTLR = 0;
    STK_CNTL = 0;
    STK_CNTH = 0;
    STK_CTLR = STK_CTLR_STE;
}

void delay_us(uint32_t us)
{
    uint32_t start = ticks_now();
    uint32_t wait = us * STK_TICKS_PER_US;
    while ((uint32_t)(ticks_now() - start) < wait) {
    }
}

void delay_ms(uint32_t ms)
{
    while (ms--) {
        delay_us(1000);
    }
}

/* Accumulate elapsed time from the 32-bit tick difference (call at least every ~238s) */
uint32_t millis(void)
{
    static uint32_t last_tick, ms, frac;
    uint32_t m = irq_save();
    uint32_t now = ticks_now();
    frac += now - last_tick;
    last_tick = now;
    ms += frac / STK_TICKS_PER_MS;
    frac %= STK_TICKS_PER_MS;
    uint32_t r = ms;
    irq_restore(m);
    return r;
}

/* The compiler may emit calls to memcpy/memset (e.g. for struct copies), so provide them here */
void *memcpy(void *dst, const void *src, unsigned n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}
