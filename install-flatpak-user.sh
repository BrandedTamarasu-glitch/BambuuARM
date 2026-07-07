#!/usr/bin/env bash
set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio/plugins"
stamp="$(date +%Y%m%d-%H%M%S)"

mkdir -p "$plugin_dir"

if [[ -e "$plugin_dir/libbambu_networking.so" ]]; then
  mv "$plugin_dir/libbambu_networking.so" "$plugin_dir/libbambu_networking.so.backup-$stamp"
fi
if [[ -e "$plugin_dir/libBambuSource.so" ]]; then
  mv "$plugin_dir/libBambuSource.so" "$plugin_dir/libBambuSource.so.backup-$stamp"
fi

install -m 0755 "$src_dir/build/libbambu_networking.so" "$plugin_dir/libbambu_networking.so"
install -m 0755 "$src_dir/build/libBambuSource.so" "$plugin_dir/libBambuSource.so"

python3 - <<'PY'
import json
from pathlib import Path

conf = Path.home() / ".var/app/com.bambulab.BambuStudio/config/BambuStudio/BambuStudio.conf"
data = json.loads(conf.read_text())
data.setdefault("app", {})["installed_networking"] = "1"
conf.write_text(json.dumps(data, indent=4) + "\n")
PY

file "$plugin_dir/libbambu_networking.so"
file "$plugin_dir/libBambuSource.so"
