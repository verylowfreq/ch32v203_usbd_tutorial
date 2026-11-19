/* system.h - Clock initialization and time base */
#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>

#define SYSCLK_HZ      144000000u
#define STK_TICK_HZ    (SYSCLK_HZ / 8u)          /* SysTick = HCLK/8 = 18MHz */
#define STK_TICKS_PER_US (STK_TICK_HZ / 1000000u)
#define STK_TICKS_PER_MS (STK_TICK_HZ / 1000u)

#include <stdbool.h>

void system_init(void);                /* Called from startup.S */

/* Detect clock settings rewritten by the debugger and set them up again (call from the main loop).
 * Returns true if it repaired something. */
bool system_clock_guard(void);
extern volatile uint32_t g_clock_repairs;

/* Low 32 bits of SysTick (18MHz, wraps every ~238s) */
static inline uint32_t ticks_now(void) { return *(volatile uint32_t *)0xE000F008u; }

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
uint32_t millis(void);                 /* 32-bit ms counter, updated by polling */

/* Exception info saved by Default_Handler */
extern volatile uint32_t g_fault[4];   /* mcause, mepc, mtval, magic */

void *memcpy(void *dst, const void *src, unsigned n);
void *memset(void *dst, int c, unsigned n);

#endif
