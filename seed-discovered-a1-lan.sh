#!/usr/bin/env bash
set -euo pipefail

seed_file="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_discovery_devices.jsonl"
dev_name="${BAMBU_DEV_NAME:-Bambu Lab A1}"
dev_id="${BAMBU_DEV_ID:?set BAMBU_DEV_ID}"
dev_ip="${BAMBU_DEV_IP:?set BAMBU_DEV_IP}"
dev_signal="${BAMBU_DEV_SIGNAL:--50}"
dev_version="${BAMBU_DEV_VERSION:-01.08.01.00}"

mkdir -p "$(dirname "$seed_file")"
python3 - "$seed_file" "$dev_name" "$dev_id" "$dev_ip" "$dev_signal" "$dev_version" <<'PY'
import json
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
device = {
    "dev_name": sys.argv[2],
    "dev_id": sys.argv[3],
    "dev_ip": sys.argv[4],
    "dev_type": "N2S",
    "dev_signal": sys.argv[5],
    "connect_type": "lan",
    "bind_state": "free",
    "sec_link": "secure",
    "ssdp_version": sys.argv[6],
}

lines = []
if path.exists():
    for raw in path.read_text().splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            lines.append(raw)
            continue
        try:
            current = json.loads(stripped)
        except json.JSONDecodeError:
            lines.append(raw)
            continue
        if current.get("dev_id") != device["dev_id"]:
            lines.append(json.dumps(current, separators=(",", ":")))

lines.append(json.dumps(device, separators=(",", ":")))
tmp = path.with_name(f".{path.name}.tmp.{os.getpid()}")
tmp.write_text("\n".join(lines) + "\n")
os.replace(tmp, path)
PY

echo "Wrote LAN discovery seed:"
echo "$seed_file"
