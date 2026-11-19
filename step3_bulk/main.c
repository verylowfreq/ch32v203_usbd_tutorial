/* main.c - step3: WinUSB + Bulk (all processing in the USB interrupt) */
#include "ch32v203_regs.h"
#include "system.h"
#include "dbglog.h"
#include "usbd.h"

int main(void)
{
    dbglog_init();
    dbglog(LOG_BOOT, 0, 0);

    usbd_init();
    irq_enable_global();

    for (;;) {
        (void)millis();   /* Keep the time counter moving (call at least every ~238s) */
        /* Detect clock settings rewritten by the debugger (wlink) and repair them (chapter 09) */
        uint32_t cfgr0 = REG32(0x40021004u);
        if (system_clock_guard()) dbglog(LOG_CLOCK_FIX, (uint16_t)g_clock_repairs, cfgr0);
    }
}
