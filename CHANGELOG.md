# Changelog

## 5.2.0 - 2026-08-03

### Added

- Added SMB2 and SMB3 browsing and playback through the bundled `libsmb2`
  backend on iOS and macOS.
- Added an unsigned iOS 16+ IPA build workflow.
- Added a self-contained Apple Silicon macOS application build.
- Added native iOS and macOS text editors with cursor placement, selection,
  password-field support, and proper focus handling.

### Fixed

- Updated the iOS interface for notches, rounded displays, and safe areas.
- Fixed startup crashes on current iOS and macOS versions.
- Fixed information popups that could not be dismissed on iOS.
- Fixed touches passing through iOS popup overlays to controls behind them.
- Fixed duplicated iOS edit controls and stale blinking cursors.
- Fixed SMB credential prompts on macOS and authentication with both Windows
  and macOS SMB shares.
- Fixed the macOS OpenGL initialization race that crashed the display-link
  rendering thread.
- Updated legacy build flags and compatibility fixes for current Apple Clang.

### Compatibility

- iOS/iPadOS 16 or newer, arm64; unsigned IPA for sideloading.
- macOS 15 or newer, Apple Silicon; ad-hoc signed application ZIP.

### Credits

SMB2/SMB3 support is adapted from
[Buksa/movian](https://github.com/Buksa/movian), including work by its
contributors such as `uzver`. The bundled client is based on
[Buksa/libsmb2](https://github.com/Buksa/libsmb2) and Ronnie Sahlberg's
[libsmb2](https://github.com/sahlberg/libsmb2). Their original license and
copyright notices are retained.
