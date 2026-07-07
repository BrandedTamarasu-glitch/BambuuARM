#!/usr/bin/env bash
set -euo pipefail

src_root="${1:-${BAMBU_STUDIO_SOURCE_DIR:-$HOME/Downloads/BambuStudio-source}}"
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
network_lib="${2:-$project_dir/build/libbambu_networking.so}"
source_lib="${3:-$project_dir/build/libBambuSource.so}"

required="$(mktemp)"
exported="$(mktemp)"
trap 'rm -f "$required" "$exported"' EXIT

rg -o 'get_network_function\("[^"]+"\)' "$src_root/src/slic3r/Utils/NetworkAgent.cpp" \
  | sed 's/.*"\([^"]*\)".*/\1/' \
  | sort -u > "$required"

nm -D --defined-only "$network_lib" \
  | awk '{print $3}' \
  | sort -u > "$exported"

missing="$(comm -23 "$required" "$exported")"
if [[ -n "$missing" ]]; then
  echo "Missing required Bambu network symbols:"
  echo "$missing"
  exit 1
fi

for sym in \
  ft_abi_version \
  ft_free \
  ft_job_cancel \
  ft_job_create \
  ft_job_get_msg \
  ft_job_get_result \
  ft_job_msg_destroy \
  ft_job_release \
  ft_job_result_destroy \
  ft_job_retain \
  ft_job_set_msg_cb \
  ft_job_set_result_cb \
  ft_job_try_get_msg \
  ft_tunnel_create \
  ft_tunnel_release \
  ft_tunnel_retain \
  ft_tunnel_set_status_cb \
  ft_tunnel_shutdown \
  ft_tunnel_start_connect \
  ft_tunnel_start_job \
  ft_tunnel_sync_connect
do
  if ! rg -qx "$sym" "$exported"; then
    echo "Missing required file-transfer symbol: $sym"
    exit 1
  fi
done

nm -D --defined-only "$source_lib" \
  | awk '{print $3}' \
  | sort -u > "$exported"

for sym in \
  Bambu_Create \
  Bambu_SetLogger \
  Bambu_Open \
  Bambu_StartStream \
  Bambu_StartStreamEx \
  Bambu_GetStreamCount \
  Bambu_GetStreamInfo \
  Bambu_GetDuration \
  Bambu_Seek \
  Bambu_ReadSample \
  Bambu_SendMessage \
  Bambu_RecvMessage \
  Bambu_Close \
  Bambu_Destroy \
  Bambu_Init \
  Bambu_Deinit \
  Bambu_GetLastErrorMsg \
  Bambu_FreeLogMsg
do
  if ! rg -qx "$sym" "$exported"; then
    echo "Missing required BambuSource symbol: $sym"
    exit 1
  fi
done

echo "All required network, file-transfer, and BambuSource symbols are exported."
