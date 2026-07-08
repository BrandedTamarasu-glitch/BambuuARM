#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path


CONFIG = Path.home() / ".var/app/com.bambulab.BambuStudio/config/BambuStudio/BambuStudio.conf"


class MT19937:
    def __init__(self, seed):
        self.mt = [0] * 624
        self.index = 624
        self.mt[0] = seed & 0xFFFFFFFF
        for i in range(1, 624):
            self.mt[i] = (1812433253 * (self.mt[i - 1] ^ (self.mt[i - 1] >> 30)) + i) & 0xFFFFFFFF

    def extract(self):
        if self.index >= 624:
            self.twist()
        y = self.mt[self.index]
        self.index += 1
        y ^= y >> 11
        y ^= (y << 7) & 0x9D2C5680
        y ^= (y << 15) & 0xEFC60000
        y ^= y >> 18
        return y & 0xFFFFFFFF

    def twist(self):
        for i in range(624):
            y = (self.mt[i] & 0x80000000) + (self.mt[(i + 1) % 624] & 0x7FFFFFFF)
            self.mt[i] = self.mt[(i + 397) % 624] ^ (y >> 1)
            if y & 1:
                self.mt[i] ^= 0x9908B0DF
        self.index = 0


def fnv1a_32(text):
    seed = 2166136261
    for byte in text.encode():
        seed ^= byte
        seed = (seed * 16777619) & 0xFFFFFFFF
    return seed


def encode_dev_ip(ip, slicer_uuid):
    if not ip or not slicer_uuid:
        return ip
    rng = MT19937(fnv1a_32(slicer_uuid))
    return "".join(f"{ord(ch) ^ (rng.extract() & 0xFF):02x}" for ch in ip)


def main():
    dev_id = os.environ.get("BAMBU_DEV_ID") or (sys.argv[1] if len(sys.argv) > 1 else "")
    dev_ip = os.environ.get("BAMBU_DEV_IP") or (sys.argv[2] if len(sys.argv) > 2 else "")
    access_code = os.environ.get("BAMBU_ACCESS_CODE") or (sys.argv[3] if len(sys.argv) > 3 else "")
    if not dev_id or not dev_ip or not access_code:
        print("usage: seed-lan-config.py <dev-id> <dev-ip> <access-code>", file=sys.stderr)
        print("or set BAMBU_DEV_ID, BAMBU_DEV_IP, and BAMBU_ACCESS_CODE", file=sys.stderr)
        return 2

    data = json.loads(CONFIG.read_text())
    app = data.setdefault("app", {})
    slicer_uuid = app.get("slicer_uuid", "")
    data.setdefault("access_code", {})[dev_id] = access_code
    data.setdefault("user_access_code", {})[dev_id] = access_code
    data.setdefault("user_access_dev_ip", {})[dev_id] = encode_dev_ip(dev_ip, slicer_uuid)
    app["user_last_selected_machine"] = dev_id
    app["enable_ssl_for_mqtt"] = True
    app["enable_ssl_for_ftp"] = True
    tmp = CONFIG.with_name(f".{CONFIG.name}.tmp.{os.getpid()}")
    tmp.write_text(json.dumps(data, indent=4) + "\n")
    os.replace(tmp, CONFIG)
    print(f"seeded {dev_id} at {dev_ip}")


if __name__ == "__main__":
    raise SystemExit(main())
