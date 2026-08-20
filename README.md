# Movian M7 for Apple platforms

This branch contains Movian **7.0.273.1** for:

- iOS and iPadOS 15 or newer (`arm64`), distributed as an unsigned IPA for
  TrollStore/sideloading and as rootless and RootHide jailbreak packages. See
  the [iOS installation guide](docs/ios-installation.md).
- Apple Silicon macOS 15 or newer, distributed as a Developer ID signed,
  notarized and stapled DMG.

It combines the modern Apple platform, touch UI and SMB2/SMB3 work from this
repository with the M7 media-engine additions and enhancements provided by
**Dean Kasabow**. Those M7 changes include HLS/fMP4, mp4dash, ECMAScript and UI
updates, plugin compatibility, FFmpeg 4.4.4, and newer network discovery.
Dean's published M7 applications and source reference are available at
[apps.movian.eu](https://apps.movian.eu/).

SMB2/SMB3 support also retains work adapted from
[Buksa/movian](https://github.com/Buksa/movian) and
[libsmb2](https://github.com/sahlberg/libsmb2).

## Repository scope

The M7 branch is intentionally Apple-only. Shared application/media code is
kept, while build trees and platform implementations for Android, Linux/X11,
NaCl, PS3, Raspberry Pi and Sunxi/STOS are excluded. FFmpeg 4.4.4 is vendored
under `ext/libav` so a fresh checkout does not depend on an obsolete libav
submodule or contain old overlaid source snapshots.

## Builds

```sh
./Autobuild/ios.sh
./Autobuild/ios-deb.sh
./configure.osx --build=m7osx --version=7.0.273.1
make -j8 dist
./Autobuild/macos-release.sh --no-build
```

The macOS release script requires a Developer ID Application certificate and
saved `notarytool` credentials. See `CHANGELOG.md` for the complete change and
credit history.
