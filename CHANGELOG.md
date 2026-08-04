# Changelog

## 7.0.272 - 2026-08-03

### Added

- Added documented TrollStore, Apple-ID sideloading and jailbreak installation
  methods, plus reproducible rootless (`iphoneos-arm64`) and RootHide/Relaxin
  (`iphoneos-arm64e`) Debian package generation from the unsigned IPA.
- Added the GPU/IOSurface user-client entitlements required for Movian's
  OpenGL ES renderer in RootHide and conventional rootless environments.
- Fixed jailbreak installs starting without an Apple application container,
  which caused `(null)/kvstore` and `(null)/bc2` errors and a black screen.
  Movian now verifies its data directories, uses a writable mobile-library
  fallback, and gives conventional rootless builds the required platform-app
  entitlements.
- Integrated M7 7.0.271 network updates: WS-Discovery for modern Windows SMB
  hosts and Network settings for SMB2 large reads and per-protocol extended
  attributes, while retaining SMB2 administrative shares and EA support.

- Added a reproducible Developer ID signing, DMG creation, notarization,
  stapling and Gatekeeper-validation workflow for direct macOS distribution.
- Integrated the M7 7.0 media engine changes, including HLS fMP4 and
  `EXT-X-MAP` playback, the mp4dash module, updated ECMAScript modules,
  WebP artwork, subtitle improvements, torrent changes and updated UI assets.
- Updated the bundled media stack to FFmpeg 4.4.4 using M7's static feature
  configuration.
- Aligned the JavaScript runtime with M7 by bundling Duktape 1.8.0, restoring
  compatibility with plugins that do not work correctly on Duktape 2.x.
- Switched the default plugin repository to `repo.movian.eu`.
- Enabled the M7 JavaScript plugin manager on iOS, including repository
  browsing, installation, persistent storage, updates and uninstallation.

### Preserved

- Retained the modern macOS build, OpenGL startup fixes, native text editor,
  overlay handling and pooled SMB2/SMB3 implementation from Movian 5.2.0.
- Retained the iOS 16+ safe-area layout, dismissible overlays, native text
  editor and the working Buksa-derived SMB2/SMB3 authentication behavior.

### Fixed

- Removed the obsolete Movian 3/4 `glwskins/old` skin and unused MP3 speaker
  samples; current builds use the flat skin and WAV speaker-position samples.
- Replaced the hand-built Debian archive with canonical `dpkg-deb` packaging,
  normalized root ownership and gzip members for Sileo compatibility.
- Standardized the default HTTP user agent on macOS and iOS as
  `Movian Apple 7.0.272`.
- Fixed the macOS launcher on case-sensitive filesystems by using the exact
  lowercase `movian.bin` name, and removed its obsolete preference for an old
  user-writable `~/.hts/showtime/movian-upgrade.bin` executable.
- Restored Apple update checks with server version and changelog information;
  installation opens the latest signed
  GitHub release without replacing code inside the notarized app bundle.
- Added compatibility parsing for malformed legacy update manifests containing
  literal line breaks in changelog strings or trailing commas.
- Enabled the General update section on iOS, where no standalone executable
  upgrade path exists, while retaining signed-release-only installation.
- Added a shared touch back control to expanded settings selectors, covering
  language, video format and all other multi-option settings on iOS.
- Added macOS Tab/Shift-Tab traversal between native edit fields, corrected
  windowed pointer hit-testing, added a mouse-visible playback back button and
  prevented macOS display dimming while video is playing.
- Added native macOS cursor placement, double-click selection, automatic
  editor teardown and reliable keyboard-first-responder restoration after SMB
  authentication dialogs.
- Fixed mixed mouse/keyboard list navigation on macOS: stale hover highlights
  are suppressed in keyboard mode, focus resumes from the item under the
  pointer, and the first arrow press advances immediately.
- Fixed playback-menu slider hover regions spilling into separators and
  unrelated sidebar rows.
- Fixed the macOS video/audio settings back arrow so it consumes mouse clicks
  instead of passing them through to the first menu item.
- Hardened SMB share enumeration against malformed entries while retaining
  administrative shares and remote extended-attribute metadata support.
- Added a readable message for native SMB `STATUS_INVALID_PARAMETER` errors.
- Updated M7's ECMAScript console integration for the bundled Duktape API.
- Added the static iconv linkage required by FFmpeg 4.4.4 on Apple platforms.
- Updated the iOS build bridge for FFmpeg 4.4.4's avfilter, avresample and
  swresample libraries.
- Restored the iOS media-information popup and its full-screen tap-to-dismiss
  behavior, and restored tap dismissal for the M7 system-information popup.
- Made the iOS system-log overlay consume touches and dismiss on a tap instead
  of passing touches through to playback controls.
- Made the shared playback settings and sidebar overlays consume touches;
  outside taps now dismiss Video, Audio and Subtitle settings consistently.
- Corrected the playback menu's top-left back control so it dismisses the
  menu and returns to the playing movie instead of closing the video page.
- Increased the playback menu dismiss control to solid white for visibility
  over video content.
- Moved the iOS playback back control into the foreground OSD panel; it closes
  the main menu and returns submenus to the main menu without stopping video.
- Close the originating sidebar when opening media info, system info or the
  system log, leaving the foreground overlay directly over playback.
- Removed the hard-coded Windows WSL label from the shared About screen.

### Compatibility

- iOS/iPadOS 16 or newer, arm64; unsigned IPA for sideloading.
- macOS 15 or newer, Apple Silicon; Developer ID signed and notarized DMG.

### Credits

The M7 additions and enhancements—including the media, HLS/fMP4, UI,
ECMAScript and FFmpeg 4.4.4 integration—are based on M7 7.0.271 source
provided by **Dean Kasabow**. See [apps.movian.eu](https://apps.movian.eu/).

This M7 branch is intentionally scoped to iOS/iPadOS and Apple Silicon macOS.
Obsolete alternate `plugins_2xx.c` snapshots, embedded source archives,
generated Xcode metadata and non-Apple platform/build trees are excluded.

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
