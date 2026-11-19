#!/usr/bin/env python3
"""
step4_test.py - Interrupt EP2 checks

  1. TICK notifications: receive at 10ms / 1ms periods and measure seq gaps (loss) and the interval
  2. ECHO: send 16 bytes on EP2 OUT -> get the reply on EP2 IN (length, sum). Measure round-trip latency
  3. Bulk loopback (1 MiB) and 10ms TICK at the same time

Notes (chapter 09):
  - In this setup (FS device behind a USB2.0 hub), cancelling a transfer by timeout blocks control
    transfers afterwards. TICKs are kept running so that every read always completes.
  - Interrupt IN arriving in back-to-back frames loses some packets on the host side
    (the device gets an ACK for every one). The notifications carry seq, so the host detects loss.
"""
import os
import statistics
import threading
import time

from ch32vendor import (find_device, set_mode, set_notify, parse_notify, NOTIFY_TICK,
                        NOTIFY_ECHO, BULK_MODE_LOOPBACK, EP_BULK_OUT, EP_BULK_IN,
                        EP_INT_OUT, EP_INT_IN)


def read_notify(dev):
    return parse_notify(dev.read(EP_INT_IN, 16, timeout=2000))


def seq_missing(notes):
    return sum(((b["seq"] - a["seq"]) & 0xFF) - 1 for a, b in zip(notes, notes[1:]))


def test_tick(dev, period, count):
    set_notify(dev, period)
    ticks = []
    while len(ticks) < count:
        n = read_notify(dev)
        if n["type"] == NOTIFY_TICK:
            ticks.append(n)
    miss = seq_missing(ticks)
    dt = [(b["uptime_ms"] - a["uptime_ms"]) / (((b["seq"] - a["seq"]) & 0xFF) or 1)
          for a, b in zip(ticks, ticks[1:])]
    print(f"[1] TICK {period:2d}ms: received {len(ticks)}, missing {miss} "
          f"({100 * miss / (len(ticks) + miss):.1f}%), device-side drops={ticks[-1]['arg1']}, "
          f"interval mean {statistics.mean(dt):.2f} ms")


def test_echo(dev, count=200):
    set_notify(dev, 20)      # Background TICK (keeps reads completing even when an ECHO is lost)
    lat, lost = [], 0
    for i in range(count):
        cmd = os.urandom(16)
        t0 = time.perf_counter()
        dev.write(EP_INT_OUT, cmd, timeout=1000)
        ticks_seen = 0
        while True:
            n = read_notify(dev)
            if n["type"] == NOTIFY_ECHO:
                if n["arg0"] != 16 or n["arg1"] != sum(cmd):
                    raise SystemExit(f"echo mismatch at #{i}: {n}")
                lat.append((time.perf_counter() - t0) * 1000)
                break
            ticks_seen += 1
            if ticks_seen > 3:   # 3 TICKs (60ms) with no ECHO -> treat it as lost on the host side
                lost += 1
                break
    lat.sort()
    print(f"[2] ECHO x{count}: lost {lost}, round-trip median {statistics.median(lat):.2f} ms, "
          f"p95 {lat[int(len(lat) * 0.95)]:.2f} ms, max {lat[-1]:.2f} ms")


def test_concurrent(dev, total=1 << 20):
    set_mode(dev, BULK_MODE_LOOPBACK)
    set_notify(dev, 10)
    data = os.urandom(total)
    result = {"ticks": []}
    stop = threading.Event()

    def bulk_reader():
        buf = bytearray()
        while len(buf) < total:
            buf += dev.read(EP_BULK_IN, min(16384, total - len(buf)), timeout=3000)
        result["data"] = bytes(buf)

    def int_reader():
        while not stop.is_set():
            n = read_notify(dev)
            if n["type"] == NOTIFY_TICK:
                result["ticks"].append(n)

    tr = threading.Thread(target=bulk_reader)
    ti = threading.Thread(target=int_reader)
    t0 = time.perf_counter()
    ti.start()
    tr.start()
    for off in range(0, total, 16384):
        dev.write(EP_BULK_OUT, data[off:off + 16384], timeout=3000)
    tr.join()
    dt = time.perf_counter() - t0
    stop.set()
    ti.join()
    ticks = result["ticks"]
    ok = result.get("data") == data
    print(f"[3] bulk loopback {total} B {'OK' if ok else 'MISMATCH'} at "
          f"{total / dt / 1024:.0f} KiB/s, TICK(10ms) received {len(ticks)} "
          f"missing {seq_missing(ticks)}")
    if not ok:
        raise SystemExit(1)


def main():
    dev = find_device()
    print(f"found: {dev.product} bcdDevice=0x{dev.bcdDevice:04X}")
    cfg = dev.get_active_configuration()
    for ep in cfg[(0, 0)]:
        print(f"  EP 0x{ep.bEndpointAddress:02X} attr={ep.bmAttributes} "
              f"mps={ep.wMaxPacketSize} interval={ep.bInterval}")
    test_tick(dev, 10, 200)
    test_tick(dev, 1, 600)
    test_echo(dev)
    test_concurrent(dev)
    set_notify(dev, 100)     # Finish with a slow TICK left running
    print("OK")


if __name__ == "__main__":
    main()
