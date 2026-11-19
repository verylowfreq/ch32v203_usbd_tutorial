"""
ch32vendor.py - Helpers for talking to the tutorial device (pyusb + libusb)

  pip install pyusb libusb-package

libusb on Windows can open devices that use the WinUSB driver.
libusb-package bundles libusb-1.0.dll, so it needs no separate install.
"""
import struct

import usb.core
import usb.util
import libusb_package

VID = 0x1209
PID = 0x0001

# Vendor requests (match vendor.h on the firmware side)
VREQ_GET_INFO = 0x01
VREQ_SET_BUF = 0x02
VREQ_GET_BUF = 0x03
VREQ_SET_MODE = 0x10        # step3+
VREQ_GET_STATS = 0x11       # step3+
VREQ_SET_NOTIFY = 0x12      # step4
VREQ_EXP_OUT_DELAY = 0x7E
MS_VENDOR_CODE = 0x20

# bmRequestType
VENDOR_IN = 0xC0    # Device-to-host | Vendor | Device
VENDOR_OUT = 0x40   # Host-to-device | Vendor | Device


def find_device():
    backend = libusb_package.get_libusb1_backend()
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
    if dev is None:
        raise SystemExit("device not found (VID=%04X PID=%04X)" % (VID, PID))
    return dev


def get_info(dev):
    raw = bytes(dev.ctrl_transfer(VENDOR_IN, VREQ_GET_INFO, 0, 0, 64))
    magic, step, uptime, set_buf_count = struct.unpack_from("<4sB3xII", raw)
    return {"magic": magic.decode(), "step": step, "uptime_ms": uptime,
            "set_buf_count": set_buf_count}


def set_buf(dev, data):
    return dev.ctrl_transfer(VENDOR_OUT, VREQ_SET_BUF, 0, 0, data)


def get_buf(dev, length=256):
    return bytes(dev.ctrl_transfer(VENDOR_IN, VREQ_GET_BUF, 0, 0, length))


# Experiment modes (match usbd.h)
EXP_AT_SETUP = 0x01
EXP_IN_DATA = 0x02
EXP_GATE_BULK = 0x04
EXP_IN_SETUP = 0x08


def set_exp_out_delay(dev, us, mode=EXP_AT_SETUP):
    dev.ctrl_transfer(VENDOR_OUT, VREQ_EXP_OUT_DELAY, us, mode, None)


BULK_MODE_LOOPBACK = 0
BULK_MODE_SINK = 1
BULK_MODE_SOURCE = 2
EP_BULK_OUT = 0x01
EP_BULK_IN = 0x81
EP_INT_OUT = 0x02
EP_INT_IN = 0x82


def set_mode(dev, mode):
    dev.ctrl_transfer(VENDOR_OUT, VREQ_SET_MODE, mode, 0, None)


def get_stats(dev):
    raw = bytes(dev.ctrl_transfer(VENDOR_IN, VREQ_GET_STATS, 0, 0, 64))
    rxb, txb, rxp, txp, paused, mode, fifo_max = struct.unpack_from("<5IBB", raw)
    return {"rx_bytes": rxb, "tx_bytes": txb, "rx_packets": rxp, "tx_packets": txp,
            "rx_paused": paused, "mode": mode, "fifo_max": fifo_max}


NOTIFY_TICK = 1
NOTIFY_ECHO = 2


def set_notify(dev, period_ms):
    dev.ctrl_transfer(VENDOR_OUT, VREQ_SET_NOTIFY, period_ms, 0, None)


def parse_notify(raw):
    typ, seq, frame, uptime, arg0, arg1 = struct.unpack("<BBHIII", bytes(raw))
    return {"type": typ, "seq": seq, "frame": frame, "uptime_ms": uptime,
            "arg0": arg0, "arg1": arg1}


def get_bos(dev):
    # GET_DESCRIPTOR(BOS): first the 5-byte header, then the full length
    hdr = bytes(dev.ctrl_transfer(0x80, 6, 0x0F00, 0, 5))
    total = hdr[2] | (hdr[3] << 8)
    return bytes(dev.ctrl_transfer(0x80, 6, 0x0F00, 0, total))


def get_ms_os_20(dev, length):
    return bytes(dev.ctrl_transfer(VENDOR_IN, MS_VENDOR_CODE, 0, 7, length))
