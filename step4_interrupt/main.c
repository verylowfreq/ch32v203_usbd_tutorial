/* main.c - step4: WinUSB + Bulk + Interrupt (the main loop sends periodic notifications) */
#include "ch32v203_regs.h"
#include "system.h"
#include "dbglog.h"
#include "usbd.h"
#include "vendor.h"

int main(void)
{
    dbglog_init();
    dbglog(LOG_BOOT, 0, 0);

    usbd_init();
    irq_enable_global();

    for (;;) {
        vendor_poll();    /* Push a TICK onto the Interrupt IN queue every period */
        /* Detect clock settings rewritten by the debugger (wlink) and repair them (chapter 09) */
        uint32_t cfgr0 = REG32(0x40021004u);
        if (system_clock_guard()) dbglog(LOG_CLOCK_FIX, (uint16_t)g_clock_repairs, cfgr0);
    }
}
