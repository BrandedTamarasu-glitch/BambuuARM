#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"
out_dir="$project_dir/diagnostics/bambuuarm-$stamp"
archive="$out_dir.tar.gz"
config_dir="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
source_dir="${BAMBU_STUDIO_SOURCE_DIR:-$HOME/Downloads/BambuStudio-source}"

mkdir -p "$out_dir"

redact_file() {
  local src="$1"
  local dest="$2"
  if [[ ! -f "$src" ]]; then
    return 0
  fi
  local escaped_home
  escaped_home="$(printf '%s\n' "$HOME" | sed 's/[\/&]/\\&/g')"
  sed -E \
    -e "s/${escaped_home}/<home>/g" \
    -e 's/(passwd|password|access[_-]?code|token|authkey)(["=:_ ]+)[^", &]+/\1\2<redacted>/Ig' \
    -e 's/("dev_id"[[:space:]]*:[[:space:]]*")[^"]+/\1<redacted-dev-id>/g' \
    -e 's/("dev_name"[[:space:]]*:[[:space:]]*")[^"]+/\1<redacted-printer-name>/g' \
    -e 's/(BAMBU_ACCESS_CODE=)[^[:space:]]+/\1<redacted>/g' \
    -e 's#(bambu://[^?[:space:]]+\?[^[:space:]]*(passwd|password|authkey)=)[^&[:space:]]+#\1<redacted>#Ig' \
    -e 's/\b(10|127|169\.254|172\.(1[6-9]|2[0-9]|3[0-1])|192\.168)\.[0-9]{1,3}\.[0-9]{1,3}\b/<redacted-ip>/g' \
    "$src" > "$dest"
}

write_command() {
  local name="$1"
  shift
  {
    echo "\$ $*"
    "$@" 2>&1 || true
  } > "$out_dir/$name.txt"
}

{
  echo "BambuuARM diagnostics"
  echo "generated=$stamp"
  echo "project_dir=$project_dir"
  echo "config_dir=$config_dir"
  echo "source_dir=$source_dir"
} > "$out_dir/summary.txt"

write_command git-status git -C "$project_dir" status --short
write_command git-head git -C "$project_dir" log -1 --oneline
write_command git-tags git -C "$project_dir" tag --points-at HEAD
write_command uname uname -a
write_command flatpak-info flatpak info com.bambulab.BambuStudio
write_command verify-exports "$project_dir/verify-exports.sh" "$source_dir"
write_command build-files file "$project_dir/build/libbambu_networking.so" "$project_dir/build/libBambuSource.so"
write_command buildids readelf -n "$project_dir/build/libbambu_networking.so" "$project_dir/build/libBambuSource.so"

python3 - "$config_dir/BambuStudio.conf" > "$out_dir/config-summary.txt" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.exists():
    print("BambuStudio.conf not found")
    raise SystemExit(0)

data = json.loads(path.read_text())
app = data.get("app", {})
print(f"studio_version={app.get('version', '')}")
print(f"installed_networking={app.get('installed_networking', '')}")
selected = app.get("user_last_selected_machine", "")
print(f"user_last_selected_machine_len={len(selected)}")
print(f"enable_ssl_for_ftp={app.get('enable_ssl_for_ftp', '')}")
print(f"enable_ssl_for_mqtt={app.get('enable_ssl_for_mqtt', '')}")
print(f"language={app.get('language', '')}")
print(f"region={app.get('region', '')}")
print(f"user_access_dev_ip_entries={len(data.get('user_access_dev_ip', {}))}")
PY

if [[ -f "$config_dir/arm64_network_stub.log" ]]; then
  tail -n 300 "$config_dir/arm64_network_stub.log" > "$out_dir/arm64_network_stub.tail.raw"
  redact_file "$out_dir/arm64_network_stub.tail.raw" "$out_dir/arm64_network_stub.tail.txt"
  rm -f "$out_dir/arm64_network_stub.tail.raw"
fi

if [[ -f "$config_dir/arm64_bambu_source.log" ]]; then
  tail -n 300 "$config_dir/arm64_bambu_source.log" > "$out_dir/arm64_bambu_source.tail.raw"
  redact_file "$out_dir/arm64_bambu_source.tail.raw" "$out_dir/arm64_bambu_source.tail.txt"
  rm -f "$out_dir/arm64_bambu_source.tail.raw"
fi

for file in arm64_discovery_devices.jsonl arm64_trusted_tls_pins.txt; do
  if [[ -f "$config_dir/$file" ]]; then
    redact_file "$config_dir/$file" "$out_dir/$file.txt"
  fi
done

{
  echo "Review every file in this bundle before sharing publicly."
  echo "The script redacts likely secrets and local IP addresses, but it cannot"
  echo "guarantee that all sensitive local details were removed."
} > "$out_dir/REVIEW_BEFORE_SHARING.txt"

for file in "$out_dir"/*.txt; do
  tmp="$file.tmp"
  redact_file "$file" "$tmp"
  mv "$tmp" "$file"
done

tar -czf "$archive" -C "$project_dir/diagnostics" "bambuuarm-$stamp"

echo "Wrote diagnostics bundle:"
echo "$archive"
echo
echo "Review the extracted files before sharing publicly."
