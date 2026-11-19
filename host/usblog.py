#!/usr/bin/env python3
"""
usblog.py - Read the RAM log (g_log) with wlink dump and decode it

  python host/usblog.py build/step1/fw.elf          # show the whole log
  python host/usblog.py build/step2/fw.elf --last 40
  python host/usblog.py build/step1/fw.elf --fault  # show g_fault (exception info)

Note: when wlink attaches, the WCH-LinkE (v2.16) rewrites RCC_CFGR0 (clears the PLLMUL bit19).
      The firmware's main loop calls system_clock_guard(), which detects this and redoes the
      PLL setup, so USB recovers after a brief glitch (chapter 09).
      Use --reset to reset the MCU after reading.

How it works:
  1. Get the addresses of g_log / g_fault from the ELF with riscv-none-elf-nm
  2. Read RAM with wlink dump <addr> <len> --out file
  3. Turn tag numbers into names using the enum in common/dbglog.h
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WLINK = r"C:\DATA-0\bin\wlink.exe"
NM = "riscv-none-elf-nm"
TICK_HZ = 18_000_000          # SysTick = HCLK/8
ENTRY_SIZE = 12
DBGLOG_N = 256


def symbol_addr(elf, name):
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"symbol {name} not found in {elf}")


def wlink_dump(addr, length):
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "dump.bin")
        r = subprocess.run([WLINK, "-q", "dump", hex(addr), str(length), "--out", path],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(path):
            raise SystemExit("wlink dump failed:\n" + r.stdout + r.stderr)
        with open(path, "rb") as f:
            return f.read()


def load_tag_names():
    names = {}
    with open(os.path.join(ROOT, "common", "dbglog.h"), encoding="utf-8") as f:
        for m in re.finditer(r"\b(LOG_\w+)\s*=\s*(\d+)", f.read()):
            names[int(m.group(2))] = m.group(1)[4:]
    return names


EP_STAT = ["DIS", "STALL", "NAK", "VALID"]
EP_TYPE = ["BULK", "CTRL", "ISO", "INT"]


def fmt_epr(v):
    """Turn an EPRn value into human-readable form"""
    s = []
    if v & 0x8000: s.append("CTR_RX")
    s.append(f"DTOG_RX={(v >> 14) & 1}")
    s.append(f"RX={EP_STAT[(v >> 12) & 3]}")
    if v & 0x0800: s.append("SETUP")
    s.append(EP_TYPE[(v >> 9) & 3])
    if v & 0x0100: s.append("KIND")
    if v & 0x0080: s.append("CTR_TX")
    s.append(f"DTOG_TX={(v >> 6) & 1}")
    s.append(f"TX={EP_STAT[(v >> 4) & 3]}")
    s.append(f"EA={v & 0xF}")
    return f"0x{v:04X}[" + " ".join(s) + "]"


REQ_NAMES = {0: "GET_STATUS", 1: "CLEAR_FEATURE", 3: "SET_FEATURE", 5: "SET_ADDRESS",
             6: "GET_DESCRIPTOR", 8: "GET_CONFIGURATION", 9: "SET_CONFIGURATION",
             10: "GET_INTERFACE", 11: "SET_INTERFACE"}
DESC_NAMES = {1: "DEVICE", 2: "CONFIG", 3: "STRING", 6: "DEV_QUALIFIER", 15: "BOS"}


def fmt_setup(a, b):
    bm, breq = a & 0xFF, a >> 8
    wval, widx = b & 0xFFFF, b >> 16
    kind = (bm >> 5) & 3
    if kind == 0:
        name = REQ_NAMES.get(breq, f"STD_{breq}")
        if breq == 6:
            name += f"({DESC_NAMES.get(wval >> 8, wval >> 8)},{wval & 0xFF})"
    else:
        name = ["STD", "CLASS", "VENDOR", "RSV"][kind] + f"_0x{breq:02X}"
    return f"{'IN ' if bm & 0x80 else 'OUT'} bm=0x{bm:02X} {name} wValue=0x{wval:04X} wIndex=0x{widx:04X}"


EP0_STATES = ["IDLE", "DATA_IN", "DATA_OUT", "STATUS_IN", "STATUS_OUT", "STALLED"]


def describe(name, a, b):
    if name in ("SETUP", "STALL"):
        return fmt_setup(a, b)
    if name == "SETUP_EPR":
        return f"EPR0={fmt_epr(a)} wLength={b}"
    if name in ("EP0_UNEXP", "EP0_EARLY"):
        return f"EPR0={fmt_epr(a)} b={b}"
    if name in ("EP0_OUT",):
        return f"count={a} state={EP0_STATES[b] if b < len(EP0_STATES) else b}"
    if name in ("EP0_IN",):
        return f"left={a} state={EP0_STATES[b] if b < len(EP0_STATES) else b}"
    return f"a=0x{a:04X} b=0x{b:08X} ({b})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--last", type=int, default=0, help="show only the last N entries")
    ap.add_argument("--fault", action="store_true", help="show g_fault")
    ap.add_argument("--reset", action="store_true", help="reset the MCU after reading")
    args = ap.parse_args()

    try:
        if args.fault:
            addr = symbol_addr(args.elf, "g_fault")
            mcause, mepc, mtval, magic = struct.unpack("<4I", wlink_dump(addr, 16))
            print(f"mcause=0x{mcause:08X} mepc=0x{mepc:08X} mtval=0x{mtval:08X} magic=0x{magic:08X}")
            return
        addr = symbol_addr(args.elf, "g_log")
        raw = wlink_dump(addr, 8 + ENTRY_SIZE * DBGLOG_N)
        decode(raw, args)
    finally:
        if args.reset:
            subprocess.run([WLINK, "-q", "reset"], capture_output=True)
            print("-- MCU reset (USB will re-enumerate)")


def decode(raw, args):
    magic, head = struct.unpack_from("<II", raw, 0)
    if magic != 0x474F4C44:
        sys.exit(f"bad magic 0x{magic:08X} (firmware not running?)")
    names = load_tag_names()

    count = min(head, DBGLOG_N)
    first = head - count
    if args.last:
        first = max(first, head - args.last)
    t0 = None
    prev = None
    for seq in range(first, head):
        i = seq % DBGLOG_N
        t, tag, a, b = struct.unpack_from("<IHHI", raw, 8 + i * ENTRY_SIZE)
        if t0 is None:
            t0 = t
            prev = t
        dt_us = ((t - prev) & 0xFFFFFFFF) / (TICK_HZ / 1e6)
        ts_ms = ((t - t0) & 0xFFFFFFFF) / (TICK_HZ / 1e3)
        prev = t
        name = names.get(tag, f"TAG{tag}")
        print(f"{seq:6d} {ts_ms:10.3f}ms (+{dt_us:9.1f}us) {name:<12} {describe(name, a, b)}")
    print(f"-- {head} entries total, {count} shown in buffer")


if __name__ == "__main__":
    main()
