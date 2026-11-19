/* vendor.h - Vendor request definitions (shared with the PC side: host/ch32vendor.py) */
#ifndef VENDOR_H
#define VENDOR_H

#define VREQ_GET_INFO       0x01  /* IN : device info                          */
#define VREQ_SET_BUF        0x02  /* OUT: store up to 256 bytes                */
#define VREQ_GET_BUF        0x03  /* IN : return the stored bytes              */
#define VREQ_SET_MODE       0x10  /* No data: wValue = BULK_MODE_xxx           */
#define VREQ_GET_STATS      0x11  /* IN : bulk_stats_t                         */

#define BULK_MODE_LOOPBACK  0     /* Echo EP1 OUT back to EP1 IN unchanged     */
#define BULK_MODE_SINK      1     /* Discard EP1 OUT (for receive throughput)  */
#define BULK_MODE_SOURCE    2     /* Keep sending a pattern on EP1 IN (for send throughput) */

#define VENDOR_STEP         3

#endif
