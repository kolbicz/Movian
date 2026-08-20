# Movian M7 7.0.273.1

This Apple maintenance release remains based on the M7 7.0.273 core. The
plugin-facing HTTP user agent therefore remains `Movian Apple 7.0.273`.

## Highlights

- Adds safer network-service controls with `Localhost only` and `All
  interfaces` binding choices, independent web, FTP and Movian remote-control
  toggles, and configurable web and FTP ports.
- Fresh installations use localhost-only network access. Existing
  installations retain their earlier network accessibility during migration.
- Adds native EDR presentation for H.264 BT.2020 HLG, including affected Sony
  XAVC files, and improves Dolby Vision, HDR10+, HDR/HLG tone mapping and
  unsupported-device fallback behavior.
- Supports HEVC MP4 files whose VPS/SPS/PPS configuration is supplied in-band,
  improving startup and seeking for affected media.
- Includes ZIP/plugin, media, subtitle, torrent, SMB, JSON, JPEG, audio and
  binary-parser security and stability hardening.
- Removes repetitive preview diagnostics while retaining concise renderer,
  fallback and error information in the system log.

## Assets prepared

- Unsigned arm64 IPA for iOS/iPadOS 15 or newer.
- Rootless jailbreak package for iOS/iPadOS 15 or newer.
- RootHide jailbreak package for iOS/iPadOS 15 or newer.
- Developer ID signed, notarized and stapled Apple Silicon macOS DMG.
