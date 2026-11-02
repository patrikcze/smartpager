#!/usr/bin/env python3
"""Pre-load server settings into a pager that is in setup mode, over USB serial.

    tools/provision.py --port /dev/cu.usbmodem1101 --url http://192.168.1.10:8080 \
        --device pager-1 --token spd_... [--ssid HomeNet --wifi-password ...] [--restart]

Secrets are read from SMARTPAGER_DEVICE_TOKEN / WIFI_PASSWORD when not given as options,
so they need not appear in shell history. Requires pyserial.
"""
import argparse
import json
import os
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--url")
    ap.add_argument("--device")
    ap.add_argument("--token", default=os.environ.get("SMARTPAGER_DEVICE_TOKEN"))
    ap.add_argument("--ssid")
    ap.add_argument("--wifi-password", default=os.environ.get("WIFI_PASSWORD"))
    ap.add_argument("--tz")
    ap.add_argument("--restart", action="store_true", help="restart into normal mode when complete")
    a = ap.parse_args()
    cfg = {k: v for k, v in {"url": a.url, "dev": a.device, "token": a.token, "ssid": a.ssid,
                             "wpass": a.wifi_password, "tz": a.tz}.items() if v}
    if a.restart:
        cfg["restart"] = True
    with serial.Serial(a.port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write(("provision " + json.dumps(cfg) + "\n").encode())
        deadline = time.time() + 5
        buf = b""
        while time.time() < deadline:
            buf += s.read(512)
            for line in buf.decode(errors="replace").splitlines():
                if line.startswith("[portal] provision"):
                    print(line)
                    return 0 if "saved" in line else 1
    print("no answer: is the pager in setup mode (setup screen visible)?", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
