# ARM64 Bambu Network Plugin Stub

This is an experimental ARM64 replacement for Bambu Studio's
`libbambu_networking.so` on ARM64 Linux.

It exports the symbols that Bambu Studio 2.7.1.62 resolves at startup and
implements enough local/LAN behavior for an A1 in LAN mode to be discovered,
restored from config, connected over TLS MQTT, monitored through printer status
reports, uploaded over FTPS, and started with a local `project_file` print
command. Cloud account, binding, and live camera/video are still intentionally
unsupported.

Build:

```sh
./build.sh
```

`./build.sh` produces:

```text
build/libbambu_networking.so
build/libBambuSource.so
build/smoke-upload
build/smoke-bambu-source
build/probe-local-tunnel
```

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

Optional manual LAN discovery seed file:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_discovery_devices.jsonl
```

Each non-comment line must be one full discovery JSON object:

```json
{"dev_name":"Bambu Lab A1","dev_id":"SERIAL_OR_DEVICE_ID","dev_ip":"192.168.1.50","dev_type":"N2S","dev_signal":"-50","connect_type":"lan","bind_state":"free","sec_link":"secure"}
```

For the printer discovered during Phase 2, this helper writes a LAN-mode seed:

```sh
./seed-discovered-a1-lan.sh
```

For the current test printer, this helper writes Bambu Studio's saved LAN access
state, including the encoded `user_access_dev_ip` value required by Studio's
local-printer restore path:

```sh
./seed-lan-config.py
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
  `bambu_network_start_send_gcode_to_sdcard` export. The latest smoke test
  uploaded `/etc/hostname` to `cache/hostname` and returned `rc=0`.
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
  Studio on ARM64. It creates/destroys tunnels, advertises one provisional H.264
  stream for local liveview, implements both `Bambu_StartStream(true)` and
  `Bambu_StartStreamEx(0x3000)`, and logs a controlled unsupported status for
  non-local media instead of causing Studio's media code to report a
  plugin-library/DLL load failure.
- The A1 status payload advertises an `ipcam` object with
  `ipcam_dev:"1"`, `resolution:"1080p"`, `tutk_server:"disable"`, and
  `mode_bits:3`, but it does not currently include `ipcam.liveview` or
  `ipcam.rtsp_url`. Bambu Studio's LAN video path therefore depends on the
  `bambu:///local/<ip>.?port=6000&user=bblp&passwd=<access>` local tunnel.
- `build/probe-local-tunnel` implements the currently reverse-engineered port
  6000 TLS frame protocol. It matches the x86 plugin's observed credential
  slots, random initial sequence, cipher list, and optional no-SNI behavior.
  The printer negotiates `TLSv1.2` with `ECDHE-RSA-AES256-GCM-SHA384`, but it
  still times out without sending a local-tunnel frame. The likely remaining gap
  is in Studio's local liveview state machine, not raw TLS connectivity.
- The live-video path has now been matched more closely to the x86 control
  flow: Studio calls `Bambu_Open`, then `Bambu_StartStream(true)`, then
  `Bambu_GetStreamInfo`, then `Bambu_ReadSample`. Studio reaches "playing", but
  `Bambu_ReadSample` still receives no bytes from the printer over the TLS
  tunnel when using the older ARM payload layout.
- The x86 plugin was inspected on a working Bambu Studio session. Its
  `BambuTunnelLocal::start(0x3000)` copies `passwd` into the first 32-byte slot
  and `authkey` into the second 32-byte slot before sending the 64-byte live
  control frame. The ARM shim previously sent the common LAN access code in the
  second slot with the first slot zeroed; this has been corrected. The separate
  16-byte auth frame also uses `authkey[8] + passwd[8]`, not `user[8] +
  passwd[8]`.
- A standalone `ctrl3000-long` probe sent the corrected x86-style 64-byte
  control frame after stopping Studio's active camera connection. `ss -tnp`
  showed no active `:6000` socket to the printer, but both SNI and no-SNI probe
  runs still timed out through 60 read windows with no returned frames. Direct
  RTSP-style ports 554, 8554, 322, and 8080 were refused; port 6000 is the only
  open local-video candidate.
- `tools/official_live_probe.cpp` loads the installed x86 `libBambuSource.so`
  and calls the official ABI outside Studio. It can `Bambu_Init`,
  `Bambu_Create`, and `Bambu_Open` the same LAN URL, but standalone
  `Bambu_StartStream(true)` remains in would-block/error state and never reaches
  the `start_stream ok` transition seen in Studio's own log. That suggests
  Studio is providing an additional playback/session prerequisite or exact call
  sequence that the raw probe and standalone ABI harness do not yet reproduce.

Next phase:

- Investigate whether Studio's "unable to parse" UI message can be stale from a
  previous failed attempt or whether an additional printer acknowledgement should
  be surfaced differently to Studio.
- Improve remote filename generation so uploaded files are not hidden dot names
  such as `.2.0.3mf`.
- Continue camera/video as a separate phase. The ARM64 `libBambuSource.so` ABI
  shim should now load cleanly, but it does not yet provide frames.
- Camera phase scope:
  - Re-test Studio camera preview and inspect `arm64_bambu_source.log` to
    confirm whether the patched `Bambu_StartStreamEx`/stream-count behavior
    changes the call order or whether it still reaches "playing" with repeated
    `Bambu_ReadSample waiting count=N` entries.
  - Continue reverse engineering the x86 `BambuTunnelLocal` implementation,
    especially any live-control payload field not yet represented by
    `probe-local-tunnel`. Current probes show that both `user/passwd` auth JSON
    variants and the obvious x86-style `0x3000` frame credential variants
    produce no response from the A1 on port 6000.
  - Before more local-tunnel work, verify the printer-side LAN liveview setting
    on the A1. The current MQTT `ipcam` payload advertises a camera but omits
    both `ipcam.liveview` and `ipcam.rtsp_url`; the printer may be accepting TLS
    on port 6000 while refusing to start liveview.
  - Compare Studio's exact playback/session setup against
    `tools/official_live_probe.cpp`. The installed x86 plugin works when driven
    by Studio, but the same plugin does not reach `start_stream ok` from the
    standalone harness, so the missing piece is above the raw TLS frame send.
  - Keep the direct RTSP path on hold unless the printer starts advertising
    `ipcam.rtsp_url`; probes showed 554/322 closed and only 6000 open for local
    video.
  - Once port 6000 returns media frames, implement the minimal local camera
    behavior in `libBambuSource_stub.cpp`: parse the `bambu:///local/` URL,
    establish the TLS tunnel, start stream type 1, expose one H.264 stream, and
    feed `Bambu_ReadSample` with framed video payloads.
  - Keep Agora/cloud video unsupported unless a specific local-only requirement
    is found.
- Keep cloud login/binding unsupported unless there is a clear local-only reason
  to add a specific safe behavior.
