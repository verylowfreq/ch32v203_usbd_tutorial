#!/usr/bin/env python3
"""
int_async.py - Receive Interrupt IN with N transfers kept pending at all times (python-libusb1, async)

  pip install libusb1 libusb-package
  python host/int_async.py --pending 1    # Equivalent to a synchronous read (a gap after every completion)
  python host/int_async.py --pending 4    # Re-submit inside the callback, always keep N pending

Purpose: in this setup (FS device behind a USB2.0 hub), the IN transactions that happen while
no read request is pending get ACKed on the host side and then discarded (chapter 09).
Keeping several transfers pending at all times closes that gap.
"""
import argparse
import ctypes
import struct
import time

import libusb_package
import usb1

VID, PID = 0x1209, 0x0001
EP_INT_IN = 0x82
VREQ_SET_NOTIFY = 0x12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pending", type=int, default=4)
    ap.add_argument("--period", type=int, default=1, help="TICK period [ms]")
    ap.add_argument("-n", type=int, default=3000)
    args = ap.parse_args()

    usb1.loadLibrary(ctypes.CDLL(str(libusb_package.get_library_path())))
    with usb1.USBContext() as ctx:
        handle = ctx.openByVendorIDAndProductID(VID, PID)
        if handle is None:
            raise SystemExit("device not found")
        handle.claimInterface(0)
        handle.controlWrite(0x40, VREQ_SET_NOTIFY, args.period, 0, b"")

        seqs = []
        transfers = []

        def on_done(tr):
            if tr.getStatus() == usb1.TRANSFER_COMPLETED:
                buf = tr.getBuffer()[:tr.getActualLength()]
                for i in range(0, len(buf), 16):
                    seqs.append(struct.unpack_from("<BB", buf, i)[1])
            if len(seqs) < args.n:
                tr.submit()      # Re-submit immediately (keeps N pending)

        for _ in range(args.pending):
            tr = handle.getTransfer()
            tr.setInterrupt(EP_INT_IN, 16, callback=on_done, timeout=0)
            tr.submit()
            transfers.append(tr)

        t0 = time.time()
        while len(seqs) < args.n and time.time() - t0 < 30:
            ctx.handleEventsTimeout(0.1)

        # Shutting down: cancel the pending transfers.
        # (A cancelled transfer leaves this setup in a state that blocks control transfers,
        #  so restore with "keep TICKs running and let one transfer complete".)
        for tr in transfers:
            try:
                tr.cancel()
            except usb1.USBErrorNotFound:
                pass
        t1 = time.time()
        while any(tr.isSubmitted() for tr in transfers) and time.time() - t1 < 2:
            ctx.handleEventsTimeout(0.1)
        handle.interruptRead(EP_INT_IN, 16, timeout=1000)   # Let one transfer complete (recovery)
        handle.controlWrite(0x40, VREQ_SET_NOTIFY, 50, 0, b"")

        miss = sum(((b - a) & 0xFF) - 1 for a, b in zip(seqs, seqs[1:]))
        print(f"pending={args.pending} period={args.period}ms: received {len(seqs)}, "
              f"missing {miss} ({100 * miss / (len(seqs) + miss):.2f}%)")


if __name__ == "__main__":
    main()
