/*
 * dbglog.h - RAM ring-buffer log (read from the PC with wlink dump)
 *
 * The ISR may call it too. It takes only a few tens of cycles, so it barely
 * disturbs USB timing. Read and decode it from the PC with host/usblog.py.
 * usblog.py parses the enum below by regular expression to get the tag names.
 * Write one "LOG_xxx = number," entry per line.
 */
#ifndef DBGLOG_H
#define DBGLOG_H

#include <stdint.h>

enum {
    LOG_BOOT        = 1,   /* a=0 b=0                                    */
    LOG_BUS_RESET   = 2,   /* a=reset count                              */
    LOG_SETUP       = 3,   /* a=bmRequestType|bRequest<<8 b=wValue|wIndex<<16 */
    LOG_SETUP_EPR   = 4,   /* a=EPR0 at SETUP time (before touching it) b=wLength */
    LOG_STALL       = 5,   /* a=bmRequestType|bRequest<<8 b=wValue|wIndex<<16 */
    LOG_SET_ADDR    = 6,   /* a=address                                  */
    LOG_SET_CONFIG  = 7,   /* a=config value                             */
    LOG_EP0_OUT     = 8,   /* a=count b=state                            */
    LOG_EP0_IN      = 9,   /* a=remaining bytes b=state                  */
    LOG_EP0_UNEXP   = 10,  /* a=EPR0 b=state (OUT in an unexpected state)    */
    LOG_SUSPEND     = 11,
    LOG_WAKEUP      = 12,
    LOG_ERR         = 13,  /* a=ERR count                                */
    LOG_PMAOVR      = 14,
    LOG_EP0_EARLY   = 15,  /* a=EPR0 b=elapsed ticks (OUT that arrived early) */
    LOG_EP_OUT      = 16,  /* a=ep b=count                               */
    LOG_EP_IN       = 17,  /* a=ep b=count                               */
    LOG_CLR_HALT    = 18,  /* a=ep address                               */
    LOG_SET_HALT    = 19,  /* a=ep address                               */
    LOG_VENDOR      = 20,  /* a=bRequest b=wLength                       */
    LOG_EXPERIMENT  = 21,  /* a,b = experiment-specific values           */
    LOG_NOTIFY      = 22,  /* a=type b=seq                               */
    LOG_MARK        = 23,  /* a,b = anything                             */
    LOG_CLOCK_FIX   = 24,  /* a=repair count b=CFGR0 before repair       */
};

typedef struct {
    uint32_t t;     /* SysTick low 32 bits (18MHz) */
    uint16_t tag;
    uint16_t a;
    uint32_t b;
} dbglog_entry_t;

#define DBGLOG_N 256u   /* Must be a power of two */

typedef struct {
    uint32_t magic;         /* 0x474F4C44 'DLOG' */
    uint32_t head;          /* Total number of entries written */
    dbglog_entry_t e[DBGLOG_N];
} dbglog_t;

extern volatile dbglog_t g_log;

void dbglog_init(void);
void dbglog(uint16_t tag, uint16_t a, uint32_t b);

#endif
