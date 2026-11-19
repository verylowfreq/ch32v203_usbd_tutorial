#!/usr/bin/env python3
"""
step2_test.py - Check WinUSB + vendor control requests

  python host/step2_test.py              # basic checks + 500 SET_BUF/GET_BUF round trips
  python host/step2_test.py --delay 500  # experiment: wait 500us before arming the control OUT data stage
"""
import argparse
import os
import random
import time

from ch32vendor import (find_device, get_info, set_buf, get_buf, get_bos, get_ms_os_20,
                        set_exp_out_delay)


def hexdump(b):
    return " ".join(f"{x:02X}" for x in b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=500, help="number of SET_BUF/GET_BUF round trips")
    ap.add_argument("--delay", type=int, default=0, help="experiment: data-stage arming delay [us]")
    args = ap.parse_args()

    dev = find_device()
    print(f"found: {dev.manufacturer} / {dev.product} / serial {dev.serial_number}")
    print(f"bcdUSB=0x{dev.bcdUSB:04X} bcdDevice=0x{dev.bcdDevice:04X}")

    # BOS and the MS OS 2.0 descriptor set
    bos = get_bos(dev)
    print(f"BOS ({len(bos)} bytes): {hexdump(bos)}")
    msos_len = bos[5 + 24] | (bos[5 + 25] << 8)
    vendor_code = bos[5 + 26]
    msos = get_ms_os_20(dev, msos_len)
    print(f"MS OS 2.0 set: {len(msos)} bytes (vendor code 0x{vendor_code:02X}), "
          f"compatible ID = {msos[14:20].decode()}")

    info = get_info(dev)
    print(f"GET_INFO: {info}")

    set_exp_out_delay(dev, args.delay)
    if args.delay:
        print(f"experiment: OUT data-stage arming delay = {args.delay} us")

    # Round trips with random lengths (0..256). Mix in the 64-byte boundaries on purpose
    rnd = random.Random(1234)
    lengths = [0, 1, 63, 64, 65, 127, 128, 129, 255, 256]
    lengths += [rnd.randint(0, 256) for _ in range(max(0, args.n - len(lengths)))]
    errors = 0
    t0 = time.perf_counter()
    for i, n in enumerate(lengths[:args.n]):
        data = os.urandom(n)
        set_buf(dev, data)
        back = get_buf(dev, 256)
        if back != data:
            errors += 1
            print(f"  MISMATCH #{i} len={n}: got {len(back)} bytes")
            if errors > 10:
                break
    dt = time.perf_counter() - t0
    print(f"SET_BUF/GET_BUF x{len(lengths[:args.n])}: errors={errors}, "
          f"{dt * 1000 / max(1, args.n):.2f} ms/roundtrip")

    set_exp_out_delay(dev, 0)
    info2 = get_info(dev)
    print(f"GET_INFO: {info2}")
    print("OK" if errors == 0 else "FAILED")


if __name__ == "__main__":
    main()
