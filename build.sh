#!/usr/bin/env bash
set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="$src_dir/build"
mkdir -p "$out_dir"

flatpak run --branch=50 --arch=aarch64 \
  --filesystem="$src_dir" \
  --filesystem=/home/coryebert/Downloads/BambuStudio-source:ro \
  --command=g++ org.gnome.Sdk \
  -std=c++17 -fPIC -shared -O2 -Wall -Wextra \
  -I/home/coryebert/Downloads/BambuStudio-source/src/slic3r/Utils \
  "$src_dir/libbambu_networking_stub.cpp" \
  -lssl -lcrypto -lcurl \
  -o "$out_dir/libbambu_networking.so"

flatpak run --branch=50 --arch=aarch64 \
  --filesystem="$src_dir" \
  --filesystem=/home/coryebert/Downloads/BambuStudio-source:ro \
  --command=g++ org.gnome.Sdk \
  -std=c++17 -fPIC -shared -O2 -Wall -Wextra \
  -I/home/coryebert/Downloads/BambuStudio-source/src/slic3r/GUI \
  "$src_dir/libBambuSource_stub.cpp" \
  -lssl -lcrypto \
  -o "$out_dir/libBambuSource.so"

flatpak run --branch=50 --arch=aarch64 \
  --filesystem="$src_dir" \
  --filesystem=/home/coryebert/Downloads/BambuStudio-source:ro \
  --command=g++ org.gnome.Sdk \
  -std=c++17 -O2 -Wall -Wextra \
  -I/home/coryebert/Downloads/BambuStudio-source/src/slic3r/Utils \
  "$src_dir/tools/smoke_upload.cpp" \
  -ldl \
  -o "$out_dir/smoke-upload"

flatpak run --branch=50 --arch=aarch64 \
  --filesystem="$src_dir" \
  --command=g++ org.gnome.Sdk \
  -std=c++17 -O2 -Wall -Wextra \
  "$src_dir/tools/smoke_bambu_source.cpp" \
  -ldl \
  -o "$out_dir/smoke-bambu-source"

flatpak run --branch=50 --arch=aarch64 \
  --filesystem="$src_dir" \
  --command=g++ org.gnome.Sdk \
  -std=c++17 -O2 -Wall -Wextra \
  "$src_dir/tools/probe_local_tunnel.cpp" \
  -lssl -lcrypto \
  -o "$out_dir/probe-local-tunnel"

file "$out_dir/libbambu_networking.so"
file "$out_dir/libBambuSource.so"
file "$out_dir/smoke-upload"
file "$out_dir/smoke-bambu-source"
file "$out_dir/probe-local-tunnel"
