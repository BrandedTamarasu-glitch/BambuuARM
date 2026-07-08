#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="${BAMBU_STUDIO_SOURCE_DIR:-$HOME/Downloads/BambuStudio-source}"
assume_yes=0
do_build=1
do_install=1
do_seed=0
seed_discovery=0
seed_dev_id=""
seed_dev_ip=""
seed_access_code=""
seed_dev_name=""
mode="install"
restore_stamp=""
verbose_backups=0
run_diagnostics=0
force=0

usage() {
  cat <<'EOF'
Usage: ./guided-install.sh [options]

Options:
  --source-dir PATH   Bambu Studio source checkout used for ABI verification.
  --yes              Do not prompt before installing into the Flatpak config.
  --no-build         Skip ./build.sh and use existing build/ outputs.
  --no-install       Stop after build and export verification.
  --seed-lan         After install, offer to seed LAN printer config.
  --seed-dev-id ID   Device id or serial for --seed-lan.
  --seed-dev-ip IP   Printer LAN IPv4 address for --seed-lan.
  --access-code CODE LAN access code for --seed-lan. Prefer interactive entry.
  --discovery-seed   Also write arm64_discovery_devices.jsonl.
  --dev-name NAME    Printer name for --discovery-seed.
  --list-backups     List restorable plugin backup pairs and exit.
  --restore[=STAMP]  Restore a plugin backup pair. Defaults to newest pair.
  --doctor           Report environment, plugin, build, and backup status.
  --diagnostics      Run collect-diagnostics.sh after install or restore.
  --force            Allow install or restore while Bambu Studio appears open.
  --verbose          Show file details with --list-backups.
  -h, --help         Show this help.

This script builds and installs source-local ARM64 plugin shims into the current
user's Bambu Studio Flatpak config. It does not install Bambu Studio itself.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

confirm() {
  local prompt="$1"
  if [[ "$assume_yes" == "1" ]]; then
    return 0
  fi
  local answer
  read -r -p "$prompt [y/N] " answer
  case "$answer" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

need_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

have_command() {
  command -v "$1" >/dev/null 2>&1
}

valid_ipv4() {
  local ip="$1"
  local IFS=.
  local -a parts
  read -r -a parts <<< "$ip"
  [[ "${#parts[@]}" -eq 4 ]] || return 1
  local part
  for part in "${parts[@]}"; do
    [[ "$part" =~ ^[0-9]+$ ]] || return 1
    (( part >= 0 && part <= 255 )) || return 1
  done
}

show_build_id() {
  local path="$1"
  if [[ -f "$path" ]]; then
    local build_id
    build_id="$(readelf -n "$path" 2>/dev/null | awk '/Build ID:/ {print $3; exit}')"
    if [[ -n "$build_id" ]]; then
      echo "$path: $build_id"
    fi
  fi
}

print_file_summary() {
  local path="$1"
  if [[ -e "$path" ]]; then
    file "$path"
    show_build_id "$path"
  else
    echo "$path: missing"
  fi
}

maybe_run_diagnostics() {
  if [[ "$run_diagnostics" == "1" ]]; then
    BAMBU_STUDIO_SOURCE_DIR="$source_dir" "$project_dir/collect-diagnostics.sh"
    return
  fi
  if [[ -t 0 ]] && confirm "Collect a redacted diagnostics bundle now?"; then
    BAMBU_STUDIO_SOURCE_DIR="$source_dir" "$project_dir/collect-diagnostics.sh"
  fi
}

bambu_studio_processes() {
  ps -eo pid=,comm=,args= | awk '
    /(^|[[:space:]])\/app\/bin\/bambu-studio([[:space:]]|$)/ ||
    /(^|[[:space:]])bambu-studio([[:space:]]|$)/ ||
    /flatpak run .*com\.bambulab\.BambuStudio/ ||
    /--app-id[= ]com\.bambulab\.BambuStudio/ ||
    /(^|[[:space:]])app\/com\.bambulab\.BambuStudio\// {
      if ($0 !~ /awk / && $0 !~ /guided-install\.sh/)
        print
    }'
}

ensure_studio_not_running() {
  local matches
  matches="$(bambu_studio_processes || true)"
  if [[ -z "$matches" ]]; then
    return 0
  fi
  echo "Bambu Studio appears to be running:"
  echo "$matches" | sed 's/^/  /'
  if [[ "$force" == "1" ]]; then
    echo "warning: continuing because --force was provided"
    return 0
  fi
  die "close Bambu Studio before install/restore, or rerun with --force"
}

ensure_config_file_ready() {
  [[ -f "$config_file" && -r "$config_file" && -w "$config_file" ]] || die "BambuStudio.conf must exist and be readable/writable: $config_file"
  python3 - "$config_file" <<'PY'
import json
import sys
from pathlib import Path

json.loads(Path(sys.argv[1]).read_text())
PY
}

backup_stamps() {
  local network
  local stamp
  shopt -s nullglob
  for network in "$plugin_dir"/libbambu_networking.so.backup-*; do
    stamp="${network##*.backup-}"
    if [[ -f "$plugin_dir/libBambuSource.so.backup-$stamp" ]]; then
      echo "$stamp"
    fi
  done | sort -u
  shopt -u nullglob
}

list_backups() {
  local stamps
  mapfile -t stamps < <(backup_stamps)
  if [[ "${#stamps[@]}" -eq 0 ]]; then
    echo "No complete plugin backup pairs found in $plugin_dir"
    return 1
  fi
  echo "Complete plugin backup pairs:"
  local newest="${stamps[-1]}"
  local stamp
  for stamp in "${stamps[@]}"; do
    if [[ "$stamp" == "$newest" ]]; then
      echo "  $stamp (newest)"
    else
      echo "  $stamp"
    fi
    if [[ "$verbose_backups" == "1" ]]; then
      file "$plugin_dir/libbambu_networking.so.backup-$stamp" "$plugin_dir/libBambuSource.so.backup-$stamp" | sed 's/^/    /'
    fi
  done
}

newest_backup_stamp() {
  backup_stamps | tail -n 1
}

restore_backup() {
  local stamp="$1"
  if [[ -z "$stamp" ]]; then
    stamp="$(newest_backup_stamp)"
  fi
  [[ -n "$stamp" ]] || die "no complete plugin backup pairs found"
  local network_backup="$plugin_dir/libbambu_networking.so.backup-$stamp"
  local source_backup="$plugin_dir/libBambuSource.so.backup-$stamp"
  [[ -f "$network_backup" ]] || die "missing backup: $network_backup"
  [[ -f "$source_backup" ]] || die "missing backup: $source_backup"

  echo "Selected backup pair: $stamp"
  file "$network_backup" "$source_backup"
  echo
  ensure_studio_not_running
  confirm "Restore this backup pair into the active plugin files?" || die "restore cancelled"

  local rollback_stamp
  rollback_stamp="$(date +%Y%m%d-%H%M%S)"
  if [[ -e "$plugin_dir/libbambu_networking.so" ]]; then
    cp -a "$plugin_dir/libbambu_networking.so" "$plugin_dir/libbambu_networking.so.pre-restore-$rollback_stamp"
  fi
  if [[ -e "$plugin_dir/libBambuSource.so" ]]; then
    cp -a "$plugin_dir/libBambuSource.so" "$plugin_dir/libBambuSource.so.pre-restore-$rollback_stamp"
  fi
  cp -a "$network_backup" "$plugin_dir/libbambu_networking.so"
  cp -a "$source_backup" "$plugin_dir/libBambuSource.so"

  echo
  echo "Restored active plugin files:"
  file "$plugin_dir/libbambu_networking.so" "$plugin_dir/libBambuSource.so"
  echo
  echo "Restored Build IDs:"
  show_build_id "$plugin_dir/libbambu_networking.so"
  show_build_id "$plugin_dir/libBambuSource.so"
  echo
  echo "Previous active files were preserved as *.pre-restore-$rollback_stamp"
  maybe_run_diagnostics
}

doctor() {
  local failures=0
  echo "Doctor report"
  echo
  echo "Architecture: $(uname -m)"
  case "$(uname -m)" in
    aarch64|arm64) echo "  ok: ARM64 host" ;;
    *) echo "  problem: this project targets ARM64 Linux"; failures=$((failures + 1)) ;;
  esac

  echo
  if have_command flatpak; then
    echo "flatpak: $(command -v flatpak)"
    if flatpak info com.bambulab.BambuStudio >/dev/null 2>&1; then
      echo "  ok: Bambu Studio Flatpak is installed"
      flatpak info com.bambulab.BambuStudio | sed -n '1,8p' | sed 's/^/  /'
    else
      echo "  problem: Bambu Studio Flatpak was not found"
      failures=$((failures + 1))
    fi
  else
    echo "flatpak: missing"
    failures=$((failures + 1))
  fi

  echo
  if [[ -d "$config_dir" ]]; then
    echo "Config directory: $config_dir"
  else
    echo "Config directory missing: $config_dir"
    failures=$((failures + 1))
  fi
  if [[ -f "$config_file" && -r "$config_file" ]]; then
    echo "Config file: $config_file"
  else
    echo "Config file missing or unreadable: $config_file"
    failures=$((failures + 1))
  fi
  echo "Plugin directory: $plugin_dir"

  echo
  local running
  running="$(bambu_studio_processes || true)"
  if [[ -n "$running" ]]; then
    echo "Bambu Studio process: running"
    echo "$running" | sed 's/^/  /'
  else
    echo "Bambu Studio process: not running"
  fi

  echo
  echo "Bambu Studio source checkout:"
  if [[ -f "$source_dir/src/slic3r/Utils/NetworkAgent.cpp" ]]; then
    echo "  ok: $source_dir"
  else
    echo "  missing or incomplete: $source_dir"
    echo "  next: pass --source-dir PATH or set BAMBU_STUDIO_SOURCE_DIR"
  fi

  echo
  echo "Current installed plugins:"
  print_file_summary "$plugin_dir/libbambu_networking.so"
  print_file_summary "$plugin_dir/libBambuSource.so"

  echo
  echo "Current build outputs:"
  print_file_summary "$project_dir/build/libbambu_networking.so"
  print_file_summary "$project_dir/build/libBambuSource.so"

  echo
  echo "Export verification:"
  if [[ -f "$source_dir/src/slic3r/Utils/NetworkAgent.cpp" &&
        -f "$project_dir/build/libbambu_networking.so" &&
        -f "$project_dir/build/libBambuSource.so" ]]; then
    if BAMBU_STUDIO_SOURCE_DIR="$source_dir" "$project_dir/verify-exports.sh"; then
      echo "  ok"
    else
      echo "  problem: export verification failed"
      failures=$((failures + 1))
    fi
  else
    echo "  skipped: source checkout or build outputs are missing"
  fi

  echo
  local newest
  newest="$(newest_backup_stamp)"
  if [[ -n "$newest" ]]; then
    echo "Latest complete backup pair: $newest"
  else
    echo "Latest complete backup pair: none"
  fi

  echo
  if [[ "$failures" -eq 0 ]]; then
    echo "Doctor result: no blocking problems found."
    echo "Next: ./guided-install.sh"
  else
    echo "Doctor result: found $failures blocking problem(s)."
    echo "Fix the reported items, then rerun ./guided-install.sh --doctor."
  fi
  return "$failures"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-dir)
      [[ $# -ge 2 ]] || die "--source-dir requires a path"
      source_dir="$2"
      shift 2
      ;;
    --yes)
      assume_yes=1
      shift
      ;;
    --no-build)
      do_build=0
      shift
      ;;
    --no-install)
      do_install=0
      shift
      ;;
    --seed-lan)
      do_seed=1
      shift
      ;;
    --seed-dev-id)
      [[ $# -ge 2 ]] || die "--seed-dev-id requires a value"
      seed_dev_id="$2"
      do_seed=1
      shift 2
      ;;
    --seed-dev-ip)
      [[ $# -ge 2 ]] || die "--seed-dev-ip requires a value"
      seed_dev_ip="$2"
      do_seed=1
      shift 2
      ;;
    --access-code)
      [[ $# -ge 2 ]] || die "--access-code requires a value"
      seed_access_code="$2"
      do_seed=1
      shift 2
      ;;
    --discovery-seed)
      seed_discovery=1
      do_seed=1
      shift
      ;;
    --dev-name)
      [[ $# -ge 2 ]] || die "--dev-name requires a value"
      seed_dev_name="$2"
      shift 2
      ;;
    --list-backups)
      mode="list-backups"
      shift
      ;;
    --restore)
      mode="restore"
      shift
      ;;
    --restore=*)
      mode="restore"
      restore_stamp="${1#--restore=}"
      shift
      ;;
    --doctor)
      mode="doctor"
      shift
      ;;
    --diagnostics)
      run_diagnostics=1
      shift
      ;;
    --force)
      force=1
      shift
      ;;
    --verbose)
      verbose_backups=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

arch="$(uname -m)"
case "$arch" in
  aarch64|arm64) ;;
  *) die "unsupported architecture '$arch'; this project targets ARM64 Linux" ;;
esac

need_command flatpak
need_command file
need_command readelf
need_command git
need_command rg
need_command python3

config_dir="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
plugin_dir="$config_dir/plugins"
config_file="$config_dir/BambuStudio.conf"

[[ -d "$config_dir" ]] || die "Bambu Studio Flatpak config not found: $config_dir. Launch Bambu Studio once first."

echo "BambuuARM guided installer"
echo
echo "Project: $project_dir"
echo "Flatpak config: $config_dir"
echo "Plugin target: $plugin_dir"
echo

if [[ "$mode" == "list-backups" ]]; then
  list_backups
  exit 0
fi

if [[ "$mode" == "restore" ]]; then
  restore_backup "$restore_stamp"
  exit 0
fi

if [[ "$mode" == "doctor" ]]; then
  doctor
  exit $?
fi

echo "Bambu Studio source: $source_dir"
echo

if [[ "$do_install" == "1" ]]; then
  ensure_config_file_ready
fi

[[ -d "$source_dir" ]] || die "Bambu Studio source checkout not found: $source_dir. Pass --source-dir PATH or set BAMBU_STUDIO_SOURCE_DIR."
[[ -f "$source_dir/src/slic3r/Utils/NetworkAgent.cpp" ]] || die "source checkout does not look like Bambu Studio source: $source_dir"

if [[ -n "$(git -C "$project_dir" status --short)" ]]; then
  echo "warning: working tree has local changes; this installer will build the current checkout."
  git -C "$project_dir" status --short
  echo
fi

if [[ "$do_seed" == "1" && ! -t 0 && ( -z "$seed_dev_id" || -z "$seed_dev_ip" || -z "$seed_access_code" ) ]]; then
  die "--seed-lan in a non-interactive shell requires --seed-dev-id, --seed-dev-ip, and --access-code"
fi

if [[ "$do_build" == "1" ]]; then
  echo "Building ARM64 shims..."
  BAMBU_STUDIO_SOURCE_DIR="$source_dir" "$project_dir/build.sh"
else
  echo "Skipping build; using existing build outputs."
fi

network_lib="$project_dir/build/libbambu_networking.so"
source_lib="$project_dir/build/libBambuSource.so"
[[ -f "$network_lib" ]] || die "missing build output: $network_lib"
[[ -f "$source_lib" ]] || die "missing build output: $source_lib"

file "$network_lib" "$source_lib"
file "$network_lib" | rg -q 'ARM aarch64|ARM64|aarch64' || die "networking shim is not ARM64"
file "$source_lib" | rg -q 'ARM aarch64|ARM64|aarch64' || die "BambuSource shim is not ARM64"

echo
echo "Verifying exported ABI symbols..."
BAMBU_STUDIO_SOURCE_DIR="$source_dir" "$project_dir/verify-exports.sh"

echo
echo "Build IDs:"
show_build_id "$network_lib"
show_build_id "$source_lib"
echo

if [[ "$do_install" != "1" ]]; then
  echo "Install skipped by --no-install."
  echo "Next: run ./install-flatpak-user.sh or rerun this script without --no-install."
  exit 0
fi

echo "Existing plugin files that will be backed up if present:"
for file in "$plugin_dir/libbambu_networking.so" "$plugin_dir/libBambuSource.so"; do
  if [[ -e "$file" ]]; then
    file "$file"
  else
    echo "$file: not present"
  fi
done
echo

ensure_studio_not_running
confirm "Install rebuilt plugins into the Bambu Studio Flatpak user config?" || die "install cancelled"

"$project_dir/install-flatpak-user.sh"

echo
echo "Installed Build IDs:"
show_build_id "$plugin_dir/libbambu_networking.so"
show_build_id "$plugin_dir/libBambuSource.so"

if [[ "$do_seed" == "1" ]]; then
  echo
  if confirm "Seed LAN printer config now? This will prompt for local printer details."; then
    dev_id="$seed_dev_id"
    dev_ip="$seed_dev_ip"
    access_code="$seed_access_code"
    if [[ -z "$dev_id" ]]; then
      read -r -p "Printer device id or serial: " dev_id
    fi
    if [[ -z "$dev_ip" ]]; then
      read -r -p "Printer LAN IPv4 address: " dev_ip
    fi
    if [[ -z "$access_code" ]]; then
      read -r -s -p "Printer LAN access code: " access_code
      echo
    fi
    echo
    [[ -n "$dev_id" && -n "$dev_ip" && -n "$access_code" ]] || die "device id, IP, and access code are required"
    valid_ipv4 "$dev_ip" || die "invalid IPv4 address: $dev_ip"
    BAMBU_DEV_ID="$dev_id" BAMBU_DEV_IP="$dev_ip" BAMBU_ACCESS_CODE="$access_code" "$project_dir/seed-lan-config.py"
    if [[ "$seed_discovery" == "1" ]] || confirm "Also write a manual discovery seed?"; then
      if [[ -n "$seed_dev_name" ]]; then
        BAMBU_DEV_NAME="$seed_dev_name" BAMBU_DEV_ID="$dev_id" BAMBU_DEV_IP="$dev_ip" "$project_dir/seed-discovered-a1-lan.sh"
      else
        BAMBU_DEV_ID="$dev_id" BAMBU_DEV_IP="$dev_ip" "$project_dir/seed-discovered-a1-lan.sh"
      fi
    fi
  fi
fi

maybe_run_diagnostics

cat <<EOF

Done.

Restart Bambu Studio, select the LAN printer, and follow:
  docs/testing.md

If something fails, collect a local redacted bundle with:
  ./collect-diagnostics.sh

Review diagnostics before sharing publicly.
EOF
