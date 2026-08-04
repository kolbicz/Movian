# M7 source audit

Compared against Movian 5.2.0 using the M7 7.0.264 and updated M7 7.0.271
source archives supplied by Dean Kasabow.

## Archive scope

The archive is a partial source overlay, not a Git checkout. It contains the
shared `src`, `res`, and `glwskins` trees plus `ext/libav` and `configure.inc`.
It does not contain commit history, Android packaging/build files, the M7
libsmb2 source or build recipe, or the complete macOS application sources.
Its FFmpeg headers identify a substantially newer API generation
(`libavcodec` 58.134 and `libavformat` 58.76) than the current tree
(`libavcodec` 57.13 and `libavformat` 57.3), but the archive's `.git` directory
is empty, so its exact FFmpeg revision cannot be recovered.

Of 935 files inspected in `src`, `res`, and `glwskins`, 750 are byte-identical
to blobs already present in Movian's Git history. The remaining 185 files are
M7 changes or local/generated assets. The integration therefore uses M7 as a
shared-source overlay while retaining the newer Apple platform and SMB2 code.

## Changes ported

- SMB2 authentication now defaults to an empty domain instead of `WORKGROUP`.
  Empty means a local account on the target and is required by macOS file
  sharing. Windows also accepts local accounts with an empty domain.
- Android may show the credential prompt when a directory scan is running on
  Movian's `asyncio` worker, matching the existing macOS exception.
- Native SMB error `0xc000000d` is displayed as `Invalid parameters` instead
  of an untranslated numeric NT status.

## Android SMB2 failure against macOS

M7's shared SMB2 adapter explicitly initialized both anonymous attempts and
the login dialog with `WORKGROUP`. A macOS local account is not a Workgroup
domain account, so macOS returns `STATUS_LOGON_FAILURE` (`0xc000006d`). The
successful iOS test after clearing Workgroup is direct confirmation of this
path.

There is a second Android-specific problem in the shared adapter: credential
queries were disabled whenever the calling thread was named `asyncio`.
Directory scans on Android can use that worker, causing the first failed login
to be returned without offering a corrected login. Both behaviors are fixed
in the port.

The archive contains no separate Android SMB2 implementation. It also omits
M7's libsmb2 binary/source, so library-version or Android ABI differences
cannot be verified from this archive alone.

## Full integration status

The M7 HLS/fMP4 and `EXT-X-MAP` implementation, mp4dash module, ECMAScript
modules, WebP/thumbnail changes, subtitle features, torrent changes, UI and
resources have been imported. FFmpeg 4.4.4 is built statically with M7's
feature flags. The plugin repository is `https://repo.movian.eu/plugins-v1.json`.

The macOS integration retains the modern OpenGL startup fixes, native text
editor, overlay handling and pooled libsmb2 backend from Movian 5.2.0. Small
compatibility changes were needed for current Duktape, Apple Clang and static
iconv linking. The resulting ARM64 application builds, launches and is
packaged as version 7.0.272.

The published M7 branch is deliberately Apple-only. Android, Linux/X11, NaCl,
PS3, Raspberry Pi, Sunxi/STOS and their packaging/build surfaces are excluded.
The canonical upstream for Dean Kasabow's published M7 applications is
[apps.movian.eu](https://apps.movian.eu/).
