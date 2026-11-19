#!/usr/bin/env python3
"""
step3_test.py - Bulk EP1 checks

  1. Loopback (synchronous, <=512 bytes): write then read back immediately and compare
  2. Flow control: write more than the device FIFO (8 x 64B) without reading -> the device NAKs
  3. Loopback (writer and reader threads in parallel, 1 MiB): integrity + throughput
  4. Sink throughput (PC -> device)
  5. Source throughput (device -> PC) + pattern check
"""
import os
import random
import threading
import time

import usb.core

from ch32vendor import (find_device, set_mode, get_stats, BULK_MODE_LOOPBACK,
                        BULK_MODE_SINK, BULK_MODE_SOURCE, EP_BULK_OUT, EP_BULK_IN)


def test_loopback_sync(dev, n=300):
    rnd = random.Random(1)
    sizes = [1, 63, 64, 65, 128, 512] + [rnd.randint(1, 512) for _ in range(n)]
    for i, size in enumerate(sizes):
        data = os.urandom(size)
        dev.write(EP_BULK_OUT, data, timeout=1000)
        back = bytes(dev.read(EP_BULK_IN, size, timeout=1000))
        if back != data:
            raise SystemExit(f"loopback mismatch at #{i} size={size}")
    print(f"[1] loopback sync: {len(sizes)} transfers (1..512 B) OK")


def test_flow_control(dev):
    """
    Write more than the device FIFO holds without reading.
    The device leaves STAT_RX=NAK, so the host write stalls and times out.
    (pyusb + libusb1 does not raise on timeout; it returns the number of bytes transferred.)

    Note: in this setup (FS device behind a USB2.0 hub), cancelling a NAKed bulk OUT
    by timeout stops control transfers from reaching the device afterwards.
    Draining IN and then letting one OUT on the same EP complete normally recovers it (chapter 09).
    """
    data = os.urandom(2048)
    t0 = time.perf_counter()
    try:
        n = dev.write(EP_BULK_OUT, data, timeout=300)
    except usb.core.USBTimeoutError:
        n = -1
    dt = (time.perf_counter() - t0) * 1000
    back = bytes(dev.read(EP_BULK_IN, 4096, timeout=1000))
    print(f"[2] flow control: write(2048 B) returned {n} after {dt:.0f} ms "
          f"(device FIFO = 8 x 64 B), read back {len(back)} B, "
          f"head match={back == data[:len(back)]}")
    # Recovery: let one OUT on the same EP complete normally (clears the host side's aborted state)
    dev.write(EP_BULK_OUT, b"R" * 64, timeout=1000)
    assert bytes(dev.read(EP_BULK_IN, 64, timeout=1000)) == b"R" * 64
    st = get_stats(dev)
    print(f"    device stats: rx={st['rx_bytes']} B, fifo_max={st['fifo_max']}, "
          f"rx_paused={st['rx_paused']}")


def test_loopback_threaded(dev, total=1 << 20):
    data = os.urandom(total)
    result = {}

    def reader():
        buf = bytearray()
        while len(buf) < total:
            buf += dev.read(EP_BULK_IN, min(16384, total - len(buf)), timeout=3000)
        result["data"] = bytes(buf)

    th = threading.Thread(target=reader)
    t0 = time.perf_counter()
    th.start()
    for off in range(0, total, 16384):
        dev.write(EP_BULK_OUT, data[off:off + 16384], timeout=3000)
    th.join()
    dt = time.perf_counter() - t0
    ok = result.get("data") == data
    print(f"[3] loopback threaded: {total} B {'OK' if ok else 'MISMATCH'}, "
          f"{total / dt / 1024:.0f} KiB/s each way")
    if not ok:
        raise SystemExit(1)


def test_sink(dev, total=2 << 20):
    set_mode(dev, BULK_MODE_SINK)
    data = os.urandom(65536)
    t0 = time.perf_counter()
    for _ in range(total // len(data)):
        dev.write(EP_BULK_OUT, data, timeout=3000)
    dt = time.perf_counter() - t0
    st = get_stats(dev)
    print(f"[4] sink  (PC->dev): {total / dt / 1024:.0f} KiB/s, device counted {st['rx_bytes']} B")


def test_source(dev, total=2 << 20):
    set_mode(dev, BULK_MODE_SOURCE)
    buf = bytearray()
    t0 = time.perf_counter()
    while len(buf) < total:
        buf += dev.read(EP_BULK_IN, 65536, timeout=3000)
    dt = time.perf_counter() - t0
    # Pattern: running counter (mod 256). Check continuity
    errors = sum(1 for i in range(1, len(buf)) if buf[i] != (buf[i - 1] + 1) & 0xFF)
    print(f"[5] source (dev->PC): {len(buf) / dt / 1024:.0f} KiB/s, pattern errors={errors}")
    set_mode(dev, BULK_MODE_LOOPBACK)


def main():
    dev = find_device()
    print(f"found: {dev.product} bcdDevice=0x{dev.bcdDevice:04X}")
    # SET_MODE empties the device FIFO and cancels any pending IN (a device-side "flush").
    # Reading on the host until a timeout (drain) cancels a transfer, which blocks control
    # transfers in this setup, so we do not do that (chapter 09).
    set_mode(dev, BULK_MODE_LOOPBACK)
    test_loopback_sync(dev)
    set_mode(dev, BULK_MODE_LOOPBACK)
    test_flow_control(dev)
    set_mode(dev, BULK_MODE_LOOPBACK)
    test_loopback_threaded(dev)
    test_sink(dev)
    test_source(dev)
    print("OK")


if __name__ == "__main__":
    main()
