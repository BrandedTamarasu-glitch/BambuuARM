#!/usr/bin/env bash
set -euo pipefail

seed_file="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_discovery_devices.jsonl"
dev_name="${BAMBU_DEV_NAME:-Bambu Lab A1}"
dev_id="${BAMBU_DEV_ID:?set BAMBU_DEV_ID}"
dev_ip="${BAMBU_DEV_IP:?set BAMBU_DEV_IP}"
dev_signal="${BAMBU_DEV_SIGNAL:--50}"
dev_version="${BAMBU_DEV_VERSION:-01.08.01.00}"

mkdir -p "$(dirname "$seed_file")"
printf '{"dev_name":"%s","dev_id":"%s","dev_ip":"%s","dev_type":"N2S","dev_signal":"%s","connect_type":"lan","bind_state":"free","sec_link":"secure","ssdp_version":"%s"}\n' \
  "$dev_name" "$dev_id" "$dev_ip" "$dev_signal" "$dev_version" > "$seed_file"

echo "Wrote LAN discovery seed:"
echo "$seed_file"
