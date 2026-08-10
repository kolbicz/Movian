#!/bin/bash

set -euo pipefail

ROOTDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="${ROOTDIR}/build.m7osx"
APP="${BUILDDIR}/dist/Movian.app"
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  "${APP}/Contents/Info.plist" 2>/dev/null || echo "7.0.272")
DMG=${DMG:-"${BUILDDIR}/Movian-macOS-${VERSION}-arm64.dmg"}
SIGN_IDENTITY=${SIGN_IDENTITY:-"Developer ID Application: Christoph Kolbicz (LPYXN7JAC8)"}
NOTARY_PROFILE=${NOTARY_PROFILE:-movian-notary}
DO_BUILD=1
DO_NOTARIZE=1

usage() {
  echo "Usage: $0 [--no-build] [--no-notarize]"
}

for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-notarize) DO_NOTARIZE=0 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

if [ "$DO_BUILD" -eq 1 ]; then
  make -C "$ROOTDIR" -j"$(sysctl -n hw.ncpu)" dist
fi

if [ ! -d "$APP" ]; then
  echo "Missing application bundle: $APP" >&2
  exit 1
fi

if ! security find-identity -v -p codesigning | grep -Fq "$SIGN_IDENTITY"; then
  echo "Signing identity is not available: $SIGN_IDENTITY" >&2
  exit 1
fi

# Build tools and copied test bundles can leave host-specific quarantine or
# App Sandbox provenance on individual bundle files. These attributes are not
# release content and can make hdiutil/Gatekeeper reject an otherwise valid app.
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
xattr -dr com.apple.provenance "$APP" 2>/dev/null || true

# Sign Mach-O contents from the inside out. Movian uses a launcher plus the
# separate movian.bin payload, so both must carry their own valid signature.
codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$APP/Contents/MacOS/movian.bin"
codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$APP/Contents/MacOS/movian"
codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$APP"

codesign --verify --deep --strict --verbose=2 "$APP"

STAGING=$(mktemp -d "${TMPDIR:-/tmp}/movian-dmg.XXXXXX")
RW_DMG="${STAGING}/Movian-rw.dmg"
MOUNTPOINT="${STAGING}/mount"
MOUNTED=0
cleanup() {
  if [ "$MOUNTED" -eq 1 ]; then
    hdiutil detach "$MOUNTPOINT" >/dev/null 2>&1 || true
  fi
  rm -rf "$STAGING"
}
trap cleanup EXIT

# On recent macOS releases, files created by a sandboxed build host receive an
# immutable provenance attribute. hdiutil refuses to create an image directly
# from such a source tree. Populate a writable image instead, then convert it
# to the compressed distribution image.
rm -f "$DMG"
mkdir "$MOUNTPOINT"
hdiutil create -size 128m -fs APFS -volname "Movian" "$RW_DMG"
hdiutil attach -nobrowse -mountpoint "$MOUNTPOINT" "$RW_DMG"
MOUNTED=1
ditto --noextattr --noacl "$APP" "$MOUNTPOINT/Movian.app"
ln -s /Applications "$MOUNTPOINT/Applications"
hdiutil detach "$MOUNTPOINT"
MOUNTED=0
hdiutil convert "$RW_DMG" -format UDZO -o "$DMG"
codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG"
codesign --verify --verbose=2 "$DMG"

if [ "$DO_NOTARIZE" -eq 1 ]; then
  xcrun notarytool submit "$DMG" \
    --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG"
  xcrun stapler validate "$DMG"
  spctl --assess --type open --context context:primary-signature \
    --verbose=4 "$DMG"
else
  echo "Created signed but non-notarized DMG: $DMG"
  exit 0
fi

shasum -a 256 "$DMG"
echo "Release DMG: $DMG"
