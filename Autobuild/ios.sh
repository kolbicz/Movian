#!/bin/sh

set -eu

ROOTDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="${ROOTDIR}/build.ios"
DERIVED_DATA="${BUILDDIR}/DerivedData"
APP="${DERIVED_DATA}/Build/Products/Release-iphoneos/Movian-iOS.app"
IOS_DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET:-15.0}
IPA=${IPA:-"${BUILDDIR}/Movian-iOS-7.0.272.4-unsigned.ipa"}

rm -rf "${BUILDDIR}"
mkdir -p "${BUILDDIR}/Payload"

IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
    sh "${ROOTDIR}/ios/build_libsmb2.sh" "${BUILDDIR}/libsmb2"

xcodebuild \
    -project "${ROOTDIR}/ios/Movian.xcodeproj" \
    -scheme Movian-iOS \
    -configuration Release \
    -sdk iphoneos \
    -destination "generic/platform=iOS" \
    -derivedDataPath "${DERIVED_DATA}" \
    IPHONEOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
    CODE_SIGNING_ALLOWED=NO \
    build

cp -R "${APP}" "${BUILDDIR}/Payload/"
(cd "${BUILDDIR}" && zip -qry "${IPA}" Payload)
rm -rf "${BUILDDIR}/Payload" "${DERIVED_DATA}"

echo "Unsigned IPA: ${IPA}"

if command -v artifact >/dev/null 2>&1; then
    artifact "${IPA}" ipa application/octet-stream "$(basename "${IPA}")"
fi
