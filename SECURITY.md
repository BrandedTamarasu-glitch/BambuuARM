# Security Policy

## Supported Versions

This project is experimental. Security fixes are currently targeted at the
latest `main` branch and the latest tagged release.

| Version | Supported |
| --- | --- |
| `v0.1.2-arm64-lan` | Yes |
| `v0.1.1-arm64-lan` and older | No |

## Scope

This project is intended for local/LAN use with Bambu Studio on ARM64 Linux. It
does not implement cloud login, cloud binding, account-backed remote access, or
cloud video.

The plugin uses first-use TLS certificate/public-key pinning for local printer
services. LAN discovery itself is unauthenticated, so the first connection to a
printer must happen on a trusted LAN and users should verify they are selecting
the expected printer/IP before entering a LAN access code. If a printer
mainboard or certificate changes, remove the matching entry from:

```text
~/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_trusted_tls_pins.txt
```

Then reconnect only while on a trusted LAN.

## Reporting A Vulnerability

Please report suspected vulnerabilities through GitHub Security Advisories for
this repository when available. If advisories are unavailable, open a GitHub
issue with a minimal description and avoid posting secrets, printer access
codes, raw MQTT payloads, raw discovery payloads, or private network details.

Useful vulnerability reports include:

- Bambu Studio version.
- Printer model and firmware version.
- ARM64 Linux distribution and Flatpak runtime details.
- Exact local action taken.
- Sanitized logs from `arm64_network_stub.log` or `arm64_bambu_source.log`.

## Handling Secrets And Logs

Do not share LAN access codes, raw printer identifiers, public IP addresses, or
unredacted diagnostic logs in public issues. The runtime diagnostics are
designed to avoid raw access codes and raw printer payloads, but users should
still review logs before sharing them.

## Security Boundaries

- This project does not bypass Bambu cloud authorization, binding, or account
  controls.
- This project does not provide remote access outside the local network.
- This project assumes the LAN is trusted during first-use pin creation.
- This project is source-only; rebuild locally from trusted source.
