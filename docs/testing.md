# Release Testing Checklist

Use this checklist when validating a tagged source release on an ARM64 Linux
system with a LAN-mode Bambu printer.

## Test Environment

Record these details in the issue or report:

- Release tag tested, for example `v0.1.1-arm64-lan`.
- ARM64 device and distro.
- Bambu Studio version.
- Printer model and firmware version.
- Whether Bambu Studio is installed as Flatpak.
- Whether the printer was discovered automatically or seeded manually.
- Approximate LAN type, for example wired, Wi-Fi 5, Wi-Fi 6, or unknown.

Do not post LAN access codes, raw config files, or unredacted logs.

## Build And Install

From a fresh checkout of the release tag:

```sh
./guided-install.sh --doctor
./build.sh
./verify-exports.sh
./install-flatpak-user.sh
```

Expected result:

- `build/libbambu_networking.so` is an ARM64 shared object.
- `build/libBambuSource.so` is an ARM64 shared object.
- `./verify-exports.sh` reports all required symbols are exported.
- The install script prints the installed plugin file types.
- `./guided-install.sh --doctor` reports no blocking problems, or reports the
  missing prerequisite to fix before testing.

## Startup And Discovery

1. Start Bambu Studio.
2. Confirm the LAN printer appears.
3. Select the printer.
4. Open the device/status page.

Expected result:

- Studio starts without plugin load errors.
- The printer can be selected.
- Printer status updates over LAN MQTT.
- `arm64_network_stub.log` shows `connect ok ... elapsed_ms=...` and a
  `pushall` refresh.

Record:

- `connect ok` `elapsed_ms`.
- Whether the first status update arrived without reselecting the printer.

Close Bambu Studio before running install or restore commands. The guided
installer should refuse to replace active plugin files while Studio is running
unless `--force` is provided.

## Upload And Local Print

Use a small, low-risk print file.

1. Send the print from Bambu Studio using LAN/local print.
2. Confirm upload completes.
3. Confirm the printer accepts the `project_file` command.
4. Confirm the printer reaches `PREPARE` or `RUNNING`.

Expected result:

- FTPS upload succeeds.
- The remote filename is readable.
- The `gcode_param` points at an embedded 3MF metadata G-code path.
- Studio does not report that the printer cannot parse the uploaded file.
- `arm64_network_stub.log` shows
  `ftps upload ok ... total_ms=... connect_ms=... tls_ms=... speed_Bps=...`.

Record:

- First upload `total_ms`, `connect_ms`, `tls_ms`, and `speed_Bps` after
  launching Studio.
- Repeat upload `total_ms`, `connect_ms`, `tls_ms`, and `speed_Bps` in the same
  Studio session, if a second low-risk send is practical.

## Liveview

1. Open the printer liveview in Studio.
2. Wait for video to start.
3. Leave it open for at least 60 seconds.

Expected result:

- Studio transitions to playing.
- Frames appear.
- `arm64_bambu_source.log` does not repeatedly report no frames.

Record:

- Approximate time from opening liveview to first visible frame.
- Whether playback remains smooth enough to inspect the print during the
  60-second window.
- Any repeated `Bambu_ReadSample waiting` or read-frame error lines.

## Sleep And Resume

1. Leave Studio open with the printer selected.
2. Put the ARM64 system to sleep.
3. Resume and wait up to 60 seconds.
4. Open the printer/device page if it is not already visible.
5. Use Studio's refresh or reselect the printer if needed.

Expected result:

- Studio does not require a full restart.
- Status refreshes after reconnect.
- Selecting the printer reconnects promptly if the UI lost selection.

Record:

- Whether the selected printer remains selected after resume.
- Time from resume or reselect to fresh status, using
  `connect ok ... elapsed_ms=...` when a reconnect occurs.

## Diagnostic Bundle

If something fails, run:

```sh
./collect-diagnostics.sh
```

Attach the generated tarball from `diagnostics/` only after reviewing it. The
script redacts likely secrets and local IP addresses, but manual review is still
required before sharing diagnostics publicly.

## Cleanup Or Restore

To list plugin backups created by installer runs:

```sh
./guided-install.sh --list-backups
```

To restore the newest complete backup pair:

```sh
./guided-install.sh --restore
```

To restore a specific pair:

```sh
./guided-install.sh --restore=YYYYMMDD-HHMMSS
```

After restoring, restart Bambu Studio.
