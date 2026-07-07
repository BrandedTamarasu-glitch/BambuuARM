# ARM64 Bambu Network Plugin Stub

This is an experimental ARM64 replacement for Bambu Studio's
`libbambu_networking.so` on ARM64 Linux.

It exports the symbols that Bambu Studio 2.7.1.62 resolves at startup and
implements enough local/LAN behavior for an A1 in LAN mode to be discovered,
restored from config, connected over TLS MQTT, monitored through printer status
reports, uploaded over FTPS, started with a local `project_file` print command,
and viewed through local LAN liveview. Cloud account and binding flows are still
intentionally unsupported.

Build:

```sh
./build.sh
```

By default, `build.sh` expects a Bambu Studio source checkout at
`$HOME/Downloads/BambuStudio-source`. To use a different checkout:

```sh
BAMBU_STUDIO_SOURCE_DIR=/path/to/BambuStudio-source ./build.sh
```

`./build.sh` produces:

```text
build/libbambu_networking.so
build/libBambuSource.so
build/smoke-upload
build/smoke-bambu-source
build/probe-local-tunnel
```

Quick start:

1. Install Bambu Studio as a Flatpak and launch it once so its user config
   directory exists.
2. Put the printer in LAN mode and make sure this ARM device can reach the
   printer on the same network.
3. Build the ARM64 plugin binaries:

   ```sh
   ./build.sh
   ```

4. Install the rebuilt plugins into Bambu Studio's user Flatpak config:

   ```sh
   ./install-flatpak-user.sh
   ```

5. Seed or restore the LAN printer entry if Studio does not discover it
   automatically. Use the printer device id, printer LAN IP, and LAN access
   code:

   ```sh
   ./seed-lan-config.py SERIAL_OR_DEVICE_ID 192.0.2.50 ACCESS_CODE
   ```

   If discovery also needs a manual seed, write one with the same id and IP:

   ```sh
   BAMBU_DEV_ID=SERIAL_OR_DEVICE_ID BAMBU_DEV_IP=192.0.2.50 ./seed-discovered-a1-lan.sh
   ```

6. Restart Bambu Studio. The printer should appear as a LAN printer. Local
   status, FTPS upload, local print start, and LAN liveview are the intended
   working paths.

Install into the user Flatpak config:

```sh
./install-flatpak-user.sh
```

Verify the exported ABI symbols:

```sh
./verify-exports.sh
```

Verify the media shim load behavior:

```sh
./build/smoke-bambu-source ./build/libBambuSource.so
```

Runtime diagnostics are written to:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_network_stub.log
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_bambu_source.log
```

LAN TLS peers are authenticated with first-use certificate/public-key pins
stored in:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_trusted_tls_pins.txt
```

If a printer mainboard or certificate changes, remove the matching pin entry and
reconnect on a trusted LAN.

Supported local behavior:

- LAN discovery and configured-printer restore.
- TLS MQTT status monitoring on port `8883`.
- FTPS file upload on port `990`.
- Local `project_file` print start using the uploaded 3MF.
- LAN liveview through the local port-`6000` tunnel. The implemented stream path
  matches the x86 plugin's combined `0x3000` TLS write and advertises the
  printer's MJPEG frames to Studio.

Unsupported behavior:

- Bambu cloud login, cloud binding, and account-backed remote access.
- Agora/cloud video.
- Direct RTSP unless a printer advertises a usable `ipcam.rtsp_url`.

Known limitations:

- Tested with a Bambu Lab A1 in LAN mode on ARM64 Linux.
- First connection to each LAN TLS service must happen on a trusted LAN because
  the plugin stores a first-use certificate/public-key pin.
- If the printer certificate changes, connections will fail until the matching
  entry is removed from `arm64_trusted_tls_pins.txt`.
- The `build/smoke-upload`, `build/probe-local-tunnel`, and
  `build/official-live-probe` binaries are developer diagnostics. They are not
  intended as end-user release artifacts.
- Runtime logs are diagnostic only and intentionally avoid raw access codes,
  raw MQTT payloads, and raw discovery payloads.

License:

This project is released under the MIT License. See [LICENSE](LICENSE).

Troubleshooting:

- If Studio does not load the plugin, run `./verify-exports.sh` and check the
  two `arm64_*` log files listed above.
- If liveview opens but stays blank, inspect `arm64_bambu_source.log` for
  `prefetch_stream_info_sample codec=mjpeg size=...`.
- If a connection fails after a printer certificate change, remove the matching
  entry from `arm64_trusted_tls_pins.txt` while on a trusted LAN.
- If upload or local print fails, inspect `arm64_network_stub.log` for the FTPS
  path and MQTT command result.
- Re-run `./install-flatpak-user.sh` after every rebuild; Bambu Studio loads the
  installed copies from its Flatpak config directory.

Optional manual LAN discovery seed file:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_discovery_devices.jsonl
```

Each non-comment line must be one full discovery JSON object:

```json
{"dev_name":"Bambu Lab A1","dev_id":"SERIAL_OR_DEVICE_ID","dev_ip":"192.0.2.50","dev_type":"N2S","dev_signal":"-50","connect_type":"lan","bind_state":"free","sec_link":"secure"}
```

For the printer discovered during Phase 2, this helper writes a LAN-mode seed:

```sh
BAMBU_DEV_ID=SERIAL_OR_DEVICE_ID BAMBU_DEV_IP=192.0.2.50 ./seed-discovered-a1-lan.sh
```

This helper writes Bambu Studio's saved LAN access state, including the encoded
`user_access_dev_ip` value required by Studio's local-printer restore path:

```sh
./seed-lan-config.py SERIAL_OR_DEVICE_ID 192.0.2.50 ACCESS_CODE
```

Current verified state:

- Bambu Studio loads the ARM64 plugin and sees the A1 over SSDP.
- `bind_detect` restores the configured device id from the seed file before
  discovery has fully started.
- TLS MQTT to the configured printer IP on port `8883` authenticates with
  `connack rc=0`.
- The plugin subscribes to `device/<device-id>/report` and receives SUBACK.
- Studio publishes `pushall` and `get_version`; the printer replies with version
  and live `push_status` JSON.
- `bambu_network_install_device_cert` emits `device_cert_installed` once per
  device so Studio's security-ready state is satisfied without log spam.
- The A1 exposes FTPS on port `990` and the newer transfer service on port
  `6000`; plain FTP port `21` is closed.
- `bambu_network_start_send_gcode_to_sdcard` now uploads the requested local file
  over FTPS using libcurl. The A1 profile's `sdcard/` folder is mapped to
  `cache/`, which is the writable FTPS directory seen on this printer.
- `build/smoke-upload` loads the plugin with `dlopen` and verifies the real
  `bambu_network_start_send_gcode_to_sdcard` export against a caller-supplied
  local file.
- `bambu_network_start_local_print_with_record` and
  `bambu_network_start_local_print` now upload the file over FTPS and publish a
  `project_file` MQTT command pointing at `file:///sdcard/cache/<file>`.
- The first live local-print test initially failed because the payload pointed
  at `Metadata/plate_2.gcode` while the uploaded 3MF contained
  `Metadata/plate_1.gcode`. The plugin now scans the 3MF and uses the embedded
  gcode path.
- After that fix, the printer acknowledged `project_file` with
  `result:"success"` and reported `gcode_state:"PREPARE"` followed by
  `gcode_state:"RUNNING"`. The printer also generated `_plate_1.gcode` and
  `1_.2.0.bbl` in `cache/`.
- `libBambuSource.so` now exports the complete `Bambu_*` media ABI expected by
  Studio on ARM64. It creates/destroys tunnels, opens local port-6000 TLS
  liveview, advertises the printer's MJPEG stream, implements both
  `Bambu_StartStream(true)` and `Bambu_StartStreamEx(0x3000)`, and logs a
  controlled unsupported status for non-local media instead of causing Studio's
  media code to report a plugin-library/DLL load failure.
- The A1 status payload advertises an `ipcam` object with
  `ipcam_dev:"1"`, `resolution:"1080p"`, `tutk_server:"disable"`, and
  `mode_bits:3`, but it does not currently include `ipcam.liveview` or
  `ipcam.rtsp_url`. Bambu Studio's LAN video path therefore depends on the
  `bambu:///local/<ip>.?port=6000&user=bblp&passwd=<access>` local tunnel.
- `build/probe-local-tunnel` implements the currently reverse-engineered port
  6000 TLS frame protocol. It matches the x86 plugin's observed credential
  slots, random initial sequence, cipher list, and optional no-SNI behavior.
  The printer negotiates `TLSv1.2` with `ECDHE-RSA-AES256-GCM-SHA384`.
- The live-video path has now been matched more closely to the x86 control
  flow: Studio calls `Bambu_Open`, then `Bambu_StartStream(true)`, then
  `Bambu_GetStreamInfo`, then `Bambu_ReadSample`.
- The x86 plugin was inspected on a working Bambu Studio session. Its
  `BambuTunnelLocal::start(0x3000)` copies `user` into the first 32-byte slot
  and `passwd` into the second 32-byte slot before sending the 64-byte live
  control frame. A follow-up disassembly pass corrected an earlier mistaken
  `passwd`/`authkey` interpretation: the referenced string was `"user"`, not an
  authkey field. The separate non-video 16-byte auth frame uses the same
  `user`-then-`passwd` slot order.
- A follow-up disassembly pass found the critical wire-level difference:
  official `LocalTunnel_Write` sends the 16-byte `0x3000` header and 64-byte
  control payload as one contiguous `SSL_write`. Earlier ARM probes and the ARM
  shim sent header and payload as separate TLS writes; that negotiated TLS but
  produced no media frames.
- After changing `probe-local-tunnel` to send the combined 80-byte control
  record, the standalone aarch64 Flatpak probe received a media frame
  immediately. The payload starts with `ff d8 ff e0 ... AVI1`, so this printer's
  local liveview stream is JPEG/MJPEG, not H.264.
- `libBambuSource.so` now matches the combined `0x3000` write, prefetches the
  first media frame during `Bambu_GetStreamInfo`, advertises `MJPG` /
  `video_jpeg` when the first sample is JPEG, and raises `max_frame_size` from
  the old provisional 48 KB value to 1 MB. A Studio liveview test on ARM
  confirmed video playback after these changes.
- `tools/official_live_probe.cpp` loads the installed x86 `libBambuSource.so`
  and calls the official ABI outside Studio. It can `Bambu_Init`,
  `Bambu_Create`, and `Bambu_Open` the same LAN URL, but standalone
  `Bambu_StartStream(true)` remains in would-block/error state and never reaches
  the `start_stream ok` transition seen in Studio's own log. That suggests
  Studio is providing an additional playback/session prerequisite or exact call
  sequence that the raw probe and standalone ABI harness do not yet reproduce.
- Current ARM probing through the aarch64 Flatpak runtime requires
  `--share=network`; without it, the local probe cannot connect even though the
  host can reach the printer. With network sharing enabled, port 6000 negotiates
  `TLSv1.2` / `ECDHE-RSA-AES256-GCM-SHA384`. Earlier empty/default-authkey
  control-frame probes timed out, but those probes used the now-obsolete
  `passwd`/`authkey` slot interpretation.
- The ARM `libBambuSource.so` now logs local URL credential lengths and the
  `0x3000` control-frame credential-slot lengths so the next Studio camera
  preview test can confirm whether Studio provides an `authkey` or only the LAN
  access code.
- A follow-up x86 disassembly pass confirmed `Bambu_StartStream(true)` maps to
  `Bambu_StartStreamEx(0x3000)`. On successful local liveview start, the common
  x86 wrapper can return `Bambu_would_block` while samples are pending. Returning
  `Bambu_would_block` directly from the ARM shim's `start_local_live` caused
  Studio to repeatedly call `Bambu_StartStream(true)` and never advance to
  `Bambu_ReadSample`, so the ARM shim keeps returning `Bambu_success` from
  stream start and records concrete SSL read failure details in the sample path.
- The ARM networking shim now normalizes full `push_status` payloads that
  advertise `ipcam_dev:"1"` but omit `ipcam.liveview`/`ipcam.rtsp_url`, adding
  `ipcam.liveview.local:"local"` and `remote:"none"` before handing the status
  to Studio. A clean Studio restart on 2026-07-07 confirmed the forwarded full
  status contains the injected liveview object. The timed launch did not open
  the camera panel, so `libBambuSource.so` was not invoked during that run.
- A 2026-07-07 Studio camera-panel test confirmed the current call order is
  now correct (`Bambu_Open` -> `Bambu_StartStream(true)` -> `Bambu_GetStreamInfo`
  -> repeated `Bambu_ReadSample`) and that the x86-style `user`/`passwd`
  `0x3000` payload is sent. No media bytes arrived; reads stayed at
  `SSL_ERROR_WANT_READ` until close. The combined-write probe result supersedes
  that finding.
- Direct RTSP-style ports 554, 8554, 322, and 8080 were refused; port 6000 is
  the only open local-video candidate seen so far.
- `tools/official_live_probe.cpp` now supports `video`, `ex`, `audio`, and
  `studio` call-order modes plus optional extra query parameters. `build.sh`
  builds it when a host C++ compiler is installed; this machine currently has no
  host compiler, so only the ARM/aarch64 artifacts were rebuilt here.
- `libBambuSource.so` now masks `passwd` and `authkey` query values in new
  `Bambu_Create` log lines and treats `SSL_ERROR_ZERO_RETURN` as stream end
  rather than continuing to report "playing" with no frames.

Next phase:

- Investigate whether Studio's "unable to parse" UI message can be stale from a
  previous failed attempt or whether an additional printer acknowledgement should
  be surfaced differently to Studio.
- Improve remote filename generation so uploaded files are not hidden dot names
  such as `.2.0.3mf`.
- Camera follow-up scope:
  - Keep `arm64_bambu_source.log` diagnostics around
    `prefetch_stream_info_sample codec=mjpeg size=...` and `Bambu_ReadSample`
    until the liveview path has had more soak time on ARM.
  - Compare Studio's exact playback/session setup against
    `tools/official_live_probe.cpp` only if standalone x86-vs-ARM probe parity
    becomes useful again; Studio-driven ARM liveview is working with the current
    combined-write shim.
  - Keep the direct RTSP path on hold unless the printer starts advertising
    `ipcam.rtsp_url`; probes showed 554/322 closed and only 6000 open for local
    video.
  - Keep Agora/cloud video unsupported unless a specific local-only requirement
    is found.
- Keep cloud login/binding unsupported unless there is a clear local-only reason
  to add a specific safe behavior.
