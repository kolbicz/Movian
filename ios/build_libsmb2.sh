#!/bin/sh

set -eu

ROOTDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUTDIR=${1:-"${ROOTDIR}/build.ios/libsmb2"}
OBJDIR="${OUTDIR}/obj"
LIBDIR="${OUTDIR}/lib"
SDKROOT=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
IOS_DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET:-16.0}

rm -rf "${OUTDIR}"
mkdir -p "${OBJDIR}" "${LIBDIR}"

SOURCES="
aes.c aes_reference.c aes_apple.c aes128ccm.c alloc.c asn1-ber.c compat.c
dcerpc.c dcerpc-lsa.c dcerpc-srvsvc.c errors.c hmac.c hmac-md5.c init.c
libsmb2.c md4c.c md5.c ntlmssp.c pdu.c sha1.c sha224-256.c sha384-512.c
smb2-cmd-close.c smb2-cmd-create.c smb2-cmd-echo.c smb2-cmd-error.c
smb2-cmd-flush.c smb2-cmd-ioctl.c smb2-cmd-lock.c smb2-cmd-logoff.c
smb2-cmd-negotiate.c smb2-cmd-notify-change.c smb2-cmd-oplock-break.c
smb2-cmd-query-directory.c smb2-cmd-query-info.c smb2-cmd-read.c
smb2-cmd-session-setup.c smb2-cmd-set-info.c smb2-cmd-tree-connect.c
smb2-cmd-tree-disconnect.c smb2-cmd-write.c smb2-data-file-info.c
smb2-data-filesystem-info.c smb2-data-security-descriptor.c
smb2-data-reparse-point.c smb2-share-enum.c smb3-seal.c smb2-signing.c
socket.c spnego-wrapper.c sync.c timestamps.c unicode.c usha.c
"

for SOURCE in ${SOURCES}; do
  "${CC}" \
    -arch arm64 \
    -isysroot "${SDKROOT}" \
    -miphoneos-version-min="${IOS_DEPLOYMENT_TARGET}" \
    -O2 \
    -fPIC \
    -std=gnu99 \
    -include stdint.h \
    -include stddef.h \
    -include time.h \
    -include stdlib.h \
    -include string.h \
    -include stdio.h \
    -include unistd.h \
    -include fcntl.h \
    -include poll.h \
    -include netdb.h \
    -include sys/socket.h \
    -include sys/uio.h \
    -include netinet/tcp.h \
    -D_U_='__attribute__((unused))' \
    -DHAVE_LINGER=1 \
    -DHAVE_SOCKADDR_STORAGE=1 \
    -I"${ROOTDIR}/ext/libsmb2/include" \
    -I"${ROOTDIR}/ext/libsmb2/include/smb2" \
    -I"${ROOTDIR}/ext/libsmb2/lib" \
    -c "${ROOTDIR}/ext/libsmb2/lib/${SOURCE}" \
    -o "${OBJDIR}/${SOURCE%.c}.o"
done

xcrun libtool -static -o "${LIBDIR}/libsmb2.a" "${OBJDIR}"/*.o

echo "libsmb2 archive: ${LIBDIR}/libsmb2.a"
