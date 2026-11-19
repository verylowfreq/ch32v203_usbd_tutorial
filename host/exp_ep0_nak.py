#!/usr/bin/env python3
"""
exp_ep0_nak.py - Measure whether CH32V203 USBD keeps STAT_RX=NAK on EP0 (needs step2 firmware)

For each condition:
  1. Set the experiment mode/wait time with a vendor request
  2. Do SET_BUF(200 bytes = 4 packets) -> GET_BUF round trips N times and count mismatches
  3. Read the RAM log over wlink and count the "NAK was set, yet the OUT got accepted" events
     (reading resets the MCU, so wait for it to re-enumerate afterwards)

  python host/exp_ep0_nak.py
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ch32vendor import (find_device, set_buf, get_buf, set_exp_out_delay,  # noqa: E402
                        EXP_AT_SETUP, EXP_IN_DATA, EXP_GATE_BULK, EXP_IN_SETUP)
import usblog  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "step2", "fw.elf")
LOG_EXPERIMENT = 21


def wait_device(timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            dev = find_device()
            dev.ctrl_transfer(0xC0, 0x01, 0, 0, 64)   # Check that WinUSB responds
            return dev
        except BaseException:
            time.sleep(0.5)
    raise SystemExit("device did not come back")


def read_experiment_log():
    addr = usblog.symbol_addr(ELF, "g_log")
    raw = usblog.wlink_dump(addr, 8 + usblog.ENTRY_SIZE * usblog.DBGLOG_N)
    import subprocess
    subprocess.run([usblog.WLINK, "-q", "reset"], capture_output=True)
    magic, head = struct.unpack_from("<II", raw, 0)
    entries = []
    for seq in range(max(0, head - usblog.DBGLOG_N), head):
        i = seq % usblog.DBGLOG_N
        t, tag, a, b = struct.unpack_from("<IHHI", raw, 8 + i * usblog.ENTRY_SIZE)
        if tag == LOG_EXPERIMENT:
            entries.append((a, b >> 16, (b >> 8) & 0xFF))
    return entries


def run(mode, delay, n=8):
    dev = wait_device()
    set_exp_out_delay(dev, delay, mode)
    errors = 0
    for _ in range(n):
        data = os.urandom(200)
        set_buf(dev, data)
        if get_buf(dev, 256) != data:
            errors += 1
    set_exp_out_delay(dev, 0, 0)
    ev = read_experiment_log()
    accepted = 0
    for after, before, where in ev:
        # CTR_RX newly set, or DTOG_RX moved, while we waited = an OUT was accepted under NAK
        if where == 1 and (after & 0x8000):
            accepted += 1
        if where == 2 and ((after ^ before) & 0x4000):
            accepted += 1
        # IN direction: CTR_TX set or DTOG_TX moved while waiting = an IN went out under NAK
        if where == 3 and ((after & 0x0080) or ((after ^ before) & 0x0040)):
            accepted += 1
    return errors, len(ev), accepted


def main():
    cases = [
        ("At SETUP, CONTROL type", EXP_AT_SETUP, 3000),
        ("At SETUP, BULK gate", EXP_AT_SETUP | EXP_GATE_BULK, 3000),
        ("In data stage, CONTROL type", EXP_IN_DATA, 3000),
        ("In data stage, BULK gate", EXP_IN_DATA | EXP_GATE_BULK, 3000),
        ("Control IN, CONTROL type", EXP_IN_SETUP, 3000),
    ]
    print(f"{'condition':<28} {'wait':>6} {'mismatch':>9} {'waits':>6} {'accepted under NAK':>18}")
    for name, mode, delay in cases:
        errors, waits, accepted = run(mode, delay)
        print(f"{name:<28} {delay:>5}us {errors:>9} {waits:>6} {accepted:>18}")
        time.sleep(1.0)


if __name__ == "__main__":
    main()
