# Changelog

All notable release changes for this project are documented here.

## Unreleased

### Added

- Release testing checklist for outside ARM64 LAN validation.
- Redacted local diagnostics collection helper for bug reports.
- Guided install script and implementation plan.

## v0.1.1-arm64-lan - 2026-07-08

Follow-up source-only ARM64 LAN support release focused on sleep/reconnect
behavior, lower logging overhead, and local print/upload performance.

### Changed

- Hardened MQTT connect/reconnect lifecycle handling so reconnect workers are
  owned and serialized instead of detached.
- Improved post-sleep LAN reconnect behavior so Studio can keep or quickly
  restore the selected printer without requiring a full application restart.
- Reduced default hot-path logging for login polling, discovery probes, MQTT
  receive/ping acknowledgements, and per-frame liveview reads. Set
  `BAMBU_ARM_VERBOSE_LOG=1` to enable those verbose diagnostics.
- Optimized 3MF G-code parameter detection from repeated full-file scans to a
  single streaming scan, while preserving `Metadata/plate_N.gcode` and hidden
  Studio temp-export fallback behavior.
- Changed explicit connection refreshes to send MQTT `pushall` when already
  connected, falling back to reconnect only when the MQTT session is unusable.
- Added immediate status refresh after successful printer connect and local
  `project_file` publish.
- Cached FTPS TLS pin preflight results for repeated uploads to the same
  selected printer endpoint in a session.

### Validation

- Rebuilt and installed the ARM64 shims locally after each phase.
- Verified required exported symbols.
- Smoke-tested Bambu Studio Flatpak startup after the final phase.
- Confirmed one successful full local print and a second clean local print start
  before the follow-up performance phases.

## v0.1.0-arm64-lan - 2026-07-07

Initial source-only ARM64 LAN support release.

### Added

- ARM64 `libbambu_networking.so` replacement exporting the Bambu Studio
  networking ABI used by Bambu Studio 2.7.1.62.
- ARM64 `libBambuSource.so` replacement exporting the local media ABI needed by
  Studio's device page.
- LAN discovery and configured-printer restore paths for LAN-mode printers.
- TLS MQTT status monitoring on port `8883`.
- FTPS upload support on port `990`, with the tested A1 `sdcard/` path mapped
  to the writable `cache/` directory.
- Local `project_file` print start after FTPS upload.
- Local LAN liveview over the observed port-`6000` TLS/MJPEG tunnel.
- First-use TLS certificate/public-key pinning for local printer services.
- Redacted diagnostic logging for networking and media paths.
- User Flatpak install helper, export verification helper, LAN config seed
  helper, and developer smoke/probe tools.

### Security

- Runtime diagnostics avoid raw LAN access codes, raw MQTT payloads, and raw
  discovery payloads.
- Printer TLS peers are pinned on first use. First connection should happen only
  on a trusted LAN.

### Known Limitations

- Validated by the maintainer on ARM64 Linux with a Bambu Lab A1 in LAN mode.
  Additional printer, firmware, and distro validation is still needed.
- Source-only release. Users must build locally.
- Cloud login, cloud binding, account-backed remote access, Agora/cloud video,
  and direct RTSP are intentionally unsupported.
- Future Bambu Studio or printer firmware changes may require compatibility
  updates.
