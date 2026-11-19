/* vendor.h - Vendor request definitions (shared with the PC side: host/ch32vendor.py) */
#ifndef VENDOR_H
#define VENDOR_H

#define VREQ_GET_INFO       0x01  /* IN : device info (vendor_info_t)           */
#define VREQ_SET_BUF        0x02  /* OUT: store up to 256 bytes                 */
#define VREQ_GET_BUF        0x03  /* IN : return the stored bytes               */
#define VREQ_EXP_OUT_DELAY  0x7E  /* No data: wValue = wait (us), wIndex = experiment mode (EXP_xxx) */

#define VENDOR_STEP         2

#endif
