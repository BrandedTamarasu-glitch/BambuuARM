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

config_dir="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
plugin_dir="$config_dir/plugins"

[[ -d "$config_dir" ]] || die "Bambu Studio Flatpak config not found: $config_dir. Launch Bambu Studio once first."
[[ -d "$source_dir" ]] || die "Bambu Studio source checkout not found: $source_dir. Pass --source-dir PATH or set BAMBU_STUDIO_SOURCE_DIR."
[[ -f "$source_dir/src/slic3r/Utils/NetworkAgent.cpp" ]] || die "source checkout does not look like Bambu Studio source: $source_dir"

echo "BambuuARM guided installer"
echo
echo "Project: $project_dir"
echo "Bambu Studio source: $source_dir"
echo "Flatpak config: $config_dir"
echo "Plugin target: $plugin_dir"
echo

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

cat <<EOF

Done.

Restart Bambu Studio, select the LAN printer, and follow:
  docs/testing.md

If something fails, collect a local redacted bundle with:
  ./collect-diagnostics.sh

Review diagnostics before sharing publicly.
EOF
