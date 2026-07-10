# ARM64 Bambu Studio LAN Plugin Shim

This project is an experimental ARM64 replacement for Bambu Studio's local
networking and media plugins on ARM64 Linux.

It targets Bambu Studio 2.7.1.62 and implements the local/LAN paths needed for
a LAN-mode Bambu Lab A1 to be discovered or restored from config, connected over
TLS MQTT, monitored through printer status reports, uploaded over FTPS,
started with a local `project_file` print command, and viewed through local LAN
liveview.

Cloud account features remain intentionally unsupported.

![Fedora Asahi ARM64 system running Bambu Studio with LAN liveview playing](docs/assets/arm64-liveview-working.png)

## Current Scope

Working local paths:

- LAN discovery and configured-printer restore.
- TLS MQTT status monitoring on port `8883`.
- FTPS file upload on port `990`.
- Local `project_file` print start using the uploaded 3MF.
- LAN liveview through the local port-`6000` TLS/MJPEG tunnel.
- Sleep/resume-oriented MQTT reconnect handling with direct `pushall` status
  refreshes when the selected printer is already connected.
- First-use TLS certificate/public-key pinning for local printer services.
- Redacted runtime diagnostics, with verbose hot-path logging available by
  launching Studio with `BAMBU_ARM_VERBOSE_LOG=1`.

Unsupported paths:

- Bambu cloud login, cloud binding, and account-backed remote access.
- Agora/cloud video.
- Direct RTSP unless a printer advertises a usable `ipcam.rtsp_url`.

## Requirements

- ARM64 Linux.
- Bambu Studio installed as a Flatpak and launched once so its user config
  directory exists.
- A Bambu Studio source checkout. By default scripts look for:

  ```text
  ~/Downloads/BambuStudio-source
  ```

  To use another checkout, pass `--source-dir` to the guided installer or set
  `BAMBU_STUDIO_SOURCE_DIR`.
- A LAN-mode printer reachable from the ARM64 device on the same network.

This release is source-only. Build locally on the ARM64 machine where Bambu
Studio will run.

## Quick Start

1. Close Bambu Studio.
2. Run the guided installer:

   ```sh
   ./guided-install.sh
   ```

3. Restart Bambu Studio.
4. Select the LAN printer and verify status, upload, print start, and liveview.

The guided installer checks prerequisites, builds the ARM64 shims, verifies the
exported ABI symbols, shows Build IDs, backs up existing plugin files, asks
before installing into the user Flatpak config, and can optionally seed LAN
printer config.

Run a non-mutating environment check at any time:

```sh
./guided-install.sh --doctor
```

If Studio does not discover the printer automatically, use the guided LAN seed
flow:

```sh
./guided-install.sh --seed-lan
```

For unattended setup:

```sh
BAMBU_ACCESS_CODE=ACCESS_CODE ./guided-install.sh --yes --seed-lan \
  --seed-dev-id SERIAL_OR_DEVICE_ID \
  --seed-dev-ip 192.0.2.50 \
  --discovery-seed
```

For normal interactive use, omit `--access-code` and enter it at the hidden
prompt. Avoid passing the LAN access code on the command line on shared systems;
command-line arguments can be visible through process listings and shell
history.

## Installer Safety And Recovery

Close Bambu Studio before install or restore. The guided installer refuses to
replace active plugin files while Studio appears to be running unless `--force`
is provided.

List restorable backup pairs:

```sh
./guided-install.sh --list-backups
```

Restore the newest complete backup pair:

```sh
./guided-install.sh --restore
```

Restore a specific backup pair:

```sh
./guided-install.sh --restore=YYYYMMDD-HHMMSS
```

Restore mode copies the selected backup pair back to the active plugin files and
preserves the current active files as `*.pre-restore-<timestamp>`.

## Diagnostics

Collect a local, redacted diagnostic bundle:

```sh
./collect-diagnostics.sh
```

Or collect diagnostics immediately after an install or restore:

```sh
./guided-install.sh --diagnostics
```

Review generated diagnostics before sharing them publicly.

Runtime logs are written to:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_network_stub.log
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_bambu_source.log
```

Runtime logs are diagnostic only and intentionally avoid raw access codes, raw
MQTT payloads, and raw discovery payloads.

LAN TLS pins are stored in:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_trusted_tls_pins.txt
```

If a printer mainboard or certificate changes, remove the matching pin entry and
reconnect on a trusted LAN.

## Validation

Release validation checklist:

```text
docs/testing.md
```

The `v0.1.3-arm64-lan` release was validated by:

- Running the guided installer end to end on the maintainer ARM64 system.
- Verifying a fresh clone can build, verify exports, run doctor mode, and run
  the documented dry installer path.
- Confirming restore refuses to continue while Bambu Studio appears to be
  running.
- Passing the GitHub Actions syntax workflow.

Current `main` also includes post-v0.1.3 liveview, reconnect, and FTPS upload
latency work. When validating those changes, record the timing fields described
in `docs/testing.md`, especially `connect ok ... elapsed_ms=...` and
`ftps upload ok ... total_ms=...`.

## Manual And Developer Commands

Build manually:

```sh
./build.sh
```

Use a non-default Bambu Studio source checkout:

```sh
BAMBU_STUDIO_SOURCE_DIR=/path/to/BambuStudio-source ./build.sh
```

Manual build outputs:

```text
build/libbambu_networking.so
build/libBambuSource.so
build/smoke-upload
build/smoke-bambu-source
build/probe-local-tunnel
```

Install manually into the user Flatpak config after a build:

```sh
./install-flatpak-user.sh
```

Verify exported ABI symbols:

```sh
./verify-exports.sh
```

Verify media shim load behavior:

```sh
./build/smoke-bambu-source ./build/libBambuSource.so
```

Direct LAN config seed helper:

```sh
BAMBU_DEV_ID=SERIAL_OR_DEVICE_ID \
BAMBU_DEV_IP=192.0.2.50 \
BAMBU_ACCESS_CODE=ACCESS_CODE \
./seed-lan-config.py
```

The helper still accepts positional arguments for automation, but prefer
environment variables or the guided installer's hidden prompt when command
history or process listings are a concern.

Optional manual discovery seed helper:

```sh
BAMBU_DEV_ID=SERIAL_OR_DEVICE_ID BAMBU_DEV_IP=192.0.2.50 ./seed-discovered-a1-lan.sh
```

The discovery seed file is:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_discovery_devices.jsonl
```

Each non-comment line must be one full discovery JSON object:

```json
{"dev_name":"Bambu Lab A1","dev_id":"SERIAL_OR_DEVICE_ID","dev_ip":"192.0.2.50","dev_type":"N2S","dev_signal":"-50","connect_type":"lan","bind_state":"free","sec_link":"secure"}
```

The `build/smoke-upload`, `build/probe-local-tunnel`, and
`build/official-live-probe` binaries are developer diagnostics. They are not
end-user release artifacts.

## Troubleshooting

- If Studio does not load the plugin, run `./guided-install.sh --doctor`, then
  `./verify-exports.sh`.
- If liveview opens but stays blank, inspect `arm64_bambu_source.log` for
  `Bambu_StartStream`, `Bambu_GetStreamInfo`, `Bambu_ReadSample waiting`, and
  read-frame error lines. Enable `BAMBU_ARM_VERBOSE_LOG=1` only when detailed
  liveview read diagnostics are needed.
- If a connection fails after a printer certificate change, remove the matching
  entry from `arm64_trusted_tls_pins.txt` while on a trusted LAN.
- If upload or local print fails, inspect `arm64_network_stub.log` for the FTPS
  path, upload timing fields, and MQTT command result.
- Re-run `./guided-install.sh` after rebuilding; Bambu Studio loads the
  installed plugin copies from its Flatpak config directory.

## License, Releases, And Security

This project is released under the MIT License. See [LICENSE](LICENSE).

- [CHANGELOG.md](CHANGELOG.md) documents tagged release history.
- [SECURITY.md](SECURITY.md) documents supported versions, vulnerability
  reporting, and local TLS pinning guidance.
- [docs/testing.md](docs/testing.md) documents the release validation checklist.
- [docs/guided-install-plan.md](docs/guided-install-plan.md) documents the
  completed guided installer phases.

## Known Limitations

- Validated by the maintainer on ARM64 Linux with a Bambu Lab A1 in LAN mode.
  Additional printer, firmware, and distro validation is still needed.
- First connection to each LAN TLS service must happen on a trusted LAN because
  the plugin stores a first-use certificate/public-key pin. LAN discovery is not
  authenticated, so verify the selected printer/IP before first connection.
- Future Bambu Studio or printer firmware changes may require compatibility
  updates.
