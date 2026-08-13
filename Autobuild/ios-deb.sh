#!/bin/sh

set -eu

ROOTDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="${ROOTDIR}/build.ios"
VERSION=${VERSION:-7.0.273}
IPA=${IPA:-"${BUILDDIR}/Movian-iOS-${VERSION}-unsigned.ipa"}
PACKAGE_REVISION=${PACKAGE_REVISION-}
ROOTLESS_MINIMUM_IOS=${ROOTLESS_MINIMUM_IOS:-15.0}
ROOTHIDE_MINIMUM_IOS=${ROOTHIDE_MINIMUM_IOS:-15.0}
ROOTLESS_SUFFIX=${ROOTLESS_SUFFIX:-rootless}
BUILD_ROOTLESS_ONLY=${BUILD_ROOTLESS_ONLY:-0}
PACKAGE_ID=${PACKAGE_ID:-tv.movian.m7}
LDID=${LDID:-ldid}
DPKG_DEB=${DPKG_DEB:-dpkg-deb}
DEB_COMPRESSION=${DEB_COMPRESSION:-xz}

if [ ! -f "$IPA" ]; then
  echo "Missing IPA: $IPA" >&2
  echo "Build it first with ./Autobuild/ios.sh" >&2
  exit 1
fi

if ! command -v "$LDID" >/dev/null 2>&1; then
  echo "ldid is required (Homebrew: brew install ldid)." >&2
  exit 1
fi

if ! command -v "$DPKG_DEB" >/dev/null 2>&1; then
  echo "dpkg-deb is required (Homebrew: brew install dpkg)." >&2
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

make_package() {
  SCHEME=$1
  ARCH=$2
  PREFIX=$3
  SUFFIX=$4
  MINIMUM_IOS=$5
  ENTITLEMENTS=${6:-}
  if [ -n "$PACKAGE_REVISION" ]; then
    CONTROL_VERSION="${VERSION}-${PACKAGE_REVISION}"
  else
    CONTROL_VERSION="$VERSION"
  fi
  PKGROOT="$WORKDIR/$SCHEME"
  APPDIR="$PKGROOT$PREFIX/Applications"
  CONTROL="$PKGROOT/DEBIAN"
  OUTPUT="${BUILDDIR}/Movian-iOS-${VERSION}-${SUFFIX}.deb"

  mkdir -p "$APPDIR" "$CONTROL"
  cp -R "$SOURCE_APP" "$APPDIR/Movian.app"
  if [ -n "$ENTITLEMENTS" ]; then
    "$LDID" -S"$ENTITLEMENTS" "$APPDIR/Movian.app/$EXECUTABLE"
  else
    "$LDID" -S "$APPDIR/Movian.app/$EXECUTABLE"
  fi

  SIZE=$(du -sk "$PKGROOT" | awk '{print $1}')
  cat > "$CONTROL/control" <<EOF
Package: $PACKAGE_ID
Name: Movian M7
Version: $CONTROL_VERSION
Architecture: $ARCH
Description: Media center with modern HLS, plugins and SMB2/SMB3 support.
Section: Multimedia
Priority: optional
Installed-Size: $SIZE
Depends: firmware (>= $MINIMUM_IOS)
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

  rm -f "$OUTPUT"
  "$DPKG_DEB" --build --root-owner-group -Z"$DEB_COMPRESSION" \
    "$PKGROOT" "$OUTPUT"
  echo "Jailbreak package: $OUTPUT"
}

# Rootless jailbreaks install third-party content below /var/jb and use the
# iphoneos-arm64 Debian architecture. Like RootHide platform apps, Movian must
# explicitly receive the GPU/IOSurface sandbox extension used by OpenGL ES.
ROOTLESS_ENTITLEMENTS="$WORKDIR/rootless-entitlements.plist"
cat > "$ROOTLESS_ENTITLEMENTS" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>platform-application</key>
  <true/>
  <key>com.apple.private.security.no-container</key>
  <true/>
  <key>com.apple.private.security.no-sandbox</key>
  <true/>
  <key>com.apple.private.security.storage.AppBundles</key>
  <true/>
  <key>com.apple.private.security.storage.AppDataContainers</key>
  <true/>
  <key>com.apple.security.exception.iokit-user-client-class</key>
  <array>
    <string>AGXCommandQueue</string>
    <string>AGXDevice</string>
    <string>AGXDeviceUserClient</string>
    <string>AGXSharedUserClient</string>
    <string>IOGPUDeviceUserClient</string>
    <string>IOAccelContext</string>
    <string>IOAccelContext2</string>
    <string>IOAccelDevice</string>
    <string>IOAccelDevice2</string>
    <string>IOAccelSharedUserClient</string>
    <string>IOAccelSharedUserClient2</string>
    <string>IOAccelSubmitter2</string>
    <string>IOSurfaceAcceleratorClient</string>
    <string>IOSurfaceRootUserClient</string>
    <string>IOMobileFramebufferUserClient</string>
  </array>
</dict>
</plist>
EOF
make_package rootless iphoneos-arm64 /var/jb "$ROOTLESS_SUFFIX" \
  "$ROOTLESS_MINIMUM_IOS" \
  "$ROOTLESS_ENTITLEMENTS"

if [ "$BUILD_ROOTLESS_ONLY" = 1 ]; then
  exit 0
fi

# RootHide is a distinct scheme. Its package manager maps /Applications into
# the randomized jailbreak root and identifies packages as iphoneos-arm64e.
# RootHide's documented app entitlements allow LaunchServices and
# application-container access from that environment.
ROOTHIDE_ENTITLEMENTS="$WORKDIR/roothide-entitlements.plist"
cat > "$ROOTHIDE_ENTITLEMENTS" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>platform-application</key>
  <true/>
  <key>com.apple.private.security.no-sandbox</key>
  <true/>
  <key>com.apple.private.security.storage.AppBundles</key>
  <true/>
  <key>com.apple.private.security.storage.AppDataContainers</key>
  <true/>
  <!-- RootHide platform applications do not inherit the ordinary third-party
       app sandbox's OpenGL ES allowance. Movian needs these GPU and IOSurface
       user clients to create its EAGLContext. -->
  <key>com.apple.security.exception.iokit-user-client-class</key>
  <array>
    <string>AGXCommandQueue</string>
    <string>AGXDevice</string>
    <string>AGXDeviceUserClient</string>
    <string>AGXSharedUserClient</string>
    <string>IOGPUDeviceUserClient</string>
    <string>IOAccelContext</string>
    <string>IOAccelContext2</string>
    <string>IOAccelDevice</string>
    <string>IOAccelDevice2</string>
    <string>IOAccelSharedUserClient</string>
    <string>IOAccelSharedUserClient2</string>
    <string>IOAccelSubmitter2</string>
    <string>IOSurfaceAcceleratorClient</string>
    <string>IOSurfaceRootUserClient</string>
    <string>IOMobileFramebufferUserClient</string>
  </array>
</dict>
</plist>
EOF
make_package roothide iphoneos-arm64e "" roothide "$ROOTHIDE_MINIMUM_IOS" \
  "$ROOTHIDE_ENTITLEMENTS"
