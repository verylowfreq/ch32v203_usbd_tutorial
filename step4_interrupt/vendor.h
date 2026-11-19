/* vendor.h - Vendor request definitions (shared with the PC side: host/ch32vendor.py) */
#ifndef VENDOR_H
#define VENDOR_H

#define VREQ_GET_INFO       0x01  /* IN : device info                          */
#define VREQ_SET_BUF        0x02  /* OUT: store up to 256 bytes                */
#define VREQ_GET_BUF        0x03  /* IN : return the stored bytes              */
#define VREQ_SET_MODE       0x10  /* No data: wValue = BULK_MODE_xxx           */
#define VREQ_GET_STATS      0x11  /* IN : bulk_stats_t                         */
#define VREQ_SET_NOTIFY     0x12  /* No data: wValue = TICK notification period (ms, 0 = off) */

#define BULK_MODE_LOOPBACK  0
#define BULK_MODE_SINK      1
#define BULK_MODE_SOURCE    2

/* Notification sent on Interrupt IN (16 bytes) */
#define NOTIFY_TICK         1     /* Periodic: arg0 = tick count, arg1 = dropped notifications */
#define NOTIFY_ECHO         2     /* Reply to EP2 OUT: arg0 = received length, arg1 = byte sum */

#define VENDOR_STEP         4

/* Called from the main loop (sends periodic notifications) */
void vendor_poll(void);

#endif
