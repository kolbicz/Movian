#!/bin/sh

set -eu

ROOTDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="${ROOTDIR}/build.ios"
VERSION=${VERSION:-7.0.272}
IPA=${IPA:-"${BUILDDIR}/Movian-iOS-${VERSION}-unsigned.ipa"}
PACKAGE_ID=${PACKAGE_ID:-tv.movian.m7}
LDID=${LDID:-ldid}

if [ ! -f "$IPA" ]; then
  echo "Missing IPA: $IPA" >&2
  echo "Build it first with ./Autobuild/ios.sh" >&2
  exit 1
fi

if ! command -v "$LDID" >/dev/null 2>&1; then
  echo "ldid is required (Homebrew: brew install ldid)." >&2
  exit 1
fi

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/movian-ios-deb.XXXXXX")
cleanup() {
  rm -rf "$WORKDIR"
}
trap cleanup EXIT HUP INT TERM

unzip -q "$IPA" -d "$WORKDIR/ipa"
SOURCE_APP="$WORKDIR/ipa/Payload/Movian-iOS.app"
EXECUTABLE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
  "$SOURCE_APP/Info.plist")

if [ ! -f "$SOURCE_APP/$EXECUTABLE" ]; then
  echo "Invalid IPA: app executable is missing." >&2
  exit 1
fi

# Jailbreaks trust an ad-hoc signed executable. Movian does not require private
# or platform entitlements, so do not add privileges that the application does
# not use.
"$LDID" -S "$SOURCE_APP/$EXECUTABLE"

make_package() {
  SCHEME=$1
  ARCH=$2
  PREFIX=$3
  SUFFIX=$4
  PKGROOT="$WORKDIR/$SCHEME"
  APPDIR="$PKGROOT$PREFIX/Applications"
  CONTROL="$WORKDIR/control-$SCHEME"
  OUTPUT="${BUILDDIR}/Movian-iOS-${VERSION}-${SUFFIX}.deb"

  mkdir -p "$APPDIR" "$CONTROL"
  cp -R "$SOURCE_APP" "$APPDIR/Movian.app"

  SIZE=$(du -sk "$PKGROOT" | awk '{print $1}')
  cat > "$CONTROL/control" <<EOF
Package: $PACKAGE_ID
Name: Movian M7
Version: $VERSION
Architecture: $ARCH
Description: Media center with modern HLS, plugins and SMB2/SMB3 support.
Section: Multimedia
Priority: optional
Installed-Size: $SIZE
Depends: firmware (>= 16.0)
Maintainer: Christoph Kolbicz
Author: Movian contributors and Dean Kasabow
Homepage: https://github.com/kolbicz/Movian
EOF

  cat > "$CONTROL/postinst" <<EOF
#!/bin/sh
if command -v uicache >/dev/null 2>&1; then
  uicache -p "$PREFIX/Applications/Movian.app" >/dev/null 2>&1 || \
    uicache -a >/dev/null 2>&1 || true
fi
exit 0
EOF

  cat > "$CONTROL/prerm" <<EOF
#!/bin/sh
if command -v uicache >/dev/null 2>&1; then
  uicache -u "$PREFIX/Applications/Movian.app" >/dev/null 2>&1 || true
fi
exit 0
EOF
  chmod 0755 "$CONTROL/postinst" "$CONTROL/prerm"

  COPYFILE_DISABLE=1 tar --format=ustar --uid 0 --gid 0 \
    -C "$CONTROL" -czf "$WORKDIR/control.tar.gz" .
  COPYFILE_DISABLE=1 tar --format=ustar --uid 0 --gid 0 \
    -C "$PKGROOT" -czf "$WORKDIR/data.tar.gz" .
  printf '2.0\n' > "$WORKDIR/debian-binary"

  rm -f "$OUTPUT"
  (cd "$WORKDIR" && ar -q "$OUTPUT" \
    debian-binary control.tar.gz data.tar.gz)
  echo "Jailbreak package: $OUTPUT"
}

# Rootless jailbreaks install third-party content below /var/jb and use the
# iphoneos-arm64 Debian architecture. Rootful packages retain /Applications
# and the historical iphoneos-arm package architecture.
make_package rootless iphoneos-arm64 /var/jb rootless
make_package rootful iphoneos-arm "" rootful

