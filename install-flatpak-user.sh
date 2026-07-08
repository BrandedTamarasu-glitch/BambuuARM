#!/usr/bin/env bash
set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_dir="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
plugin_dir="$config_dir/plugins"
conf="$config_dir/BambuStudio.conf"
stamp="$(date +%Y%m%d-%H%M%S-%N)"

network_src="$src_dir/build/libbambu_networking.so"
source_src="$src_dir/build/libBambuSource.so"
network_active="$plugin_dir/libbambu_networking.so"
source_active="$plugin_dir/libBambuSource.so"
network_backup="$network_active.backup-$stamp"
source_backup="$source_active.backup-$stamp"
network_new="$plugin_dir/.libbambu_networking.so.new-$stamp"
source_new="$plugin_dir/.libBambuSource.so.new-$stamp"

had_network=0
had_source=0
committed=0

die() {
  echo "error: $*" >&2
  exit 1
}

rollback() {
  local status=$?
  if [[ "$status" -eq 0 || "$committed" == "1" ]]; then
    return "$status"
  fi
  echo "Install failed; restoring previous plugin files." >&2
  rm -f "$network_new" "$source_new"
  if [[ "$had_network" == "1" && -f "$network_backup" ]]; then
    mv -f "$network_backup" "$network_active"
  else
    rm -f "$network_active"
  fi
  if [[ "$had_source" == "1" && -f "$source_backup" ]]; then
    mv -f "$source_backup" "$source_active"
  else
    rm -f "$source_active"
  fi
  return "$status"
}
trap rollback EXIT

command -v python3 >/dev/null 2>&1 || die "missing required command: python3"
command -v file >/dev/null 2>&1 || die "missing required command: file"

[[ -f "$network_src" ]] || die "missing build output: $network_src"
[[ -f "$source_src" ]] || die "missing build output: $source_src"
[[ -d "$config_dir" ]] || die "Bambu Studio config directory not found: $config_dir"
[[ -f "$conf" && -r "$conf" && -w "$conf" ]] || die "BambuStudio.conf must exist and be readable/writable: $conf"

python3 - "$conf" <<'PY'
import json
import sys
from pathlib import Path

conf = Path(sys.argv[1])
json.loads(conf.read_text())
PY

mkdir -p "$plugin_dir"
[[ ! -e "$network_backup" ]] || die "backup already exists: $network_backup"
[[ ! -e "$source_backup" ]] || die "backup already exists: $source_backup"

install -m 0755 "$network_src" "$network_new"
install -m 0755 "$source_src" "$source_new"

if [[ -e "$network_active" ]]; then
  had_network=1
  mv "$network_active" "$network_backup"
fi
if [[ -e "$source_active" ]]; then
  had_source=1
  mv "$source_active" "$source_backup"
fi

mv "$network_new" "$network_active"
mv "$source_new" "$source_active"

python3 - "$conf" <<'PY'
import json
import os
import sys
from pathlib import Path

conf = Path(sys.argv[1])
data = json.loads(conf.read_text())
data.setdefault("app", {})["installed_networking"] = "1"
tmp = conf.with_name(f".{conf.name}.tmp.{os.getpid()}")
tmp.write_text(json.dumps(data, indent=4) + "\n")
os.replace(tmp, conf)
PY

committed=1
trap - EXIT

file "$network_active"
file "$source_active"
