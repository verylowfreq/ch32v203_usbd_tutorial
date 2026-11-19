#include "dbglog.h"
#include "ch32v203_regs.h"
#include "system.h"

volatile dbglog_t g_log;

void dbglog_init(void)
{
    g_log.head = 0;
    g_log.magic = 0x474F4C44u;
}

void dbglog(uint16_t tag, uint16_t a, uint32_t b)
{
    uint32_t m = irq_save();
    uint32_t i = g_log.head & (DBGLOG_N - 1u);
    g_log.e[i].t = ticks_now();
    g_log.e[i].tag = tag;
    g_log.e[i].a = a;
    g_log.e[i].b = b;
    g_log.head++;
    irq_restore(m);
}
