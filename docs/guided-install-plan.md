# Guided Install Script Plan

This plan describes the guided installer that wraps the existing build and
Flatpak install flow without changing the working shims.

## Goals

- Help a new user install from source with fewer manual steps.
- Verify obvious prerequisites before modifying the Bambu Studio user config.
- Preserve backups of existing plugin files.
- Keep all printer credentials local.
- Avoid shipping binary artifacts until compatibility is broader.

## Non-Goals

- No cloud login or binding support.
- No automatic download of Bambu Studio source without a clear prompt.
- No replacement for Bambu Studio installation.
- No privileged system install path by default.

## Proposed Command

```sh
./guided-install.sh
```

Supported options:

```sh
./guided-install.sh --source-dir /path/to/BambuStudio-source --seed-lan
./guided-install.sh --no-install
./guided-install.sh --no-build
./guided-install.sh --yes
```

## Phase 1: Preflight

- Confirm the host architecture is `aarch64` or `arm64`.
- Confirm `flatpak` is installed.
- Confirm Bambu Studio Flatpak config exists.
- Confirm the Bambu Studio source checkout exists or ask for its path.
- Confirm required build tools are available through the existing `build.sh`
  flow.
- Confirm the working tree is clean or warn the user that local changes are
  being built. Implemented.

## Phase 2: Build And Verify

- Run `./build.sh`. Implemented.
- Run `./verify-exports.sh`. Implemented.
- Print BuildIDs for `libbambu_networking.so` and `libBambuSource.so`.
  Implemented.
- Stop if either step fails. Implemented.

## Phase 3: Backup And Install

- Show the target Flatpak plugin directory. Implemented.
- Show existing plugin files that will be backed up. Implemented.
- Ask for confirmation before modifying the config directory. Implemented.
- Run the same backup/install behavior as `install-flatpak-user.sh`.
  Implemented.
- Confirm installed file types and BuildIDs. Implemented.

## Phase 4: Optional LAN Config Seed

- Ask whether Studio already sees the printer. Implemented with `--seed-lan`.
- If not, prompt for device id, LAN IP, and LAN access code. Implemented.
- Run `seed-lan-config.py`. Implemented.
- Optionally run `seed-discovered-a1-lan.sh`. Implemented.
- Never echo or store the access code outside Bambu Studio's expected local
  config file.

## Phase 5: Smoke And Report

- Prompt the user to restart Bambu Studio. Implemented.
- Offer to run `./collect-diagnostics.sh` after first startup. Not yet
  implemented as a prompt; the script prints the command.
- Print the testing checklist path: `docs/testing.md`. Implemented.
- Print troubleshooting pointers for logs and TLS pin reset. Partially
  implemented through README/testing links.

## Safety Checks

- Refuse to proceed if built libraries are not ARM64 ELF shared objects.
  Implemented.
- Refuse to install if required exported symbols are missing. Implemented.
- Back up existing plugin files with timestamps. Implemented through
  `install-flatpak-user.sh`.
- Do not delete old backups automatically. Implemented.
- Do not send diagnostics anywhere. Implemented.

## Open Questions

- Whether to support non-Flatpak installs later.
- Whether the script should detect the exact Bambu Studio version from the
  Flatpak metadata or from the app itself.
- Whether to include an uninstall/restore command in the same script or as a
  separate helper.
- Whether to launch Bambu Studio automatically after install.
