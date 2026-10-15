#!/bin/bash
# Linux counterpart of build_gns.ps1: GameNetworkingSockets into Simtary/deps/gns-install-linux.
# Run INSIDE the portable build chroot (tools/linux_chroot.sh) so the .so needs nothing newer
# than glibc 2.31. protobuf (static, PIC) and OpenSSL's libcrypto (static) are linked into
# libGameNetworkingSockets.so, so a server machine needs no extra packages.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="$HERE/../deps"
INSTALL="$DEPS/gns-install-linux"
WORK=/root/gnsbuild
mkdir -p "$WORK"
[ -d "$DEPS/protobuf" ] || git clone --depth 1 --branch v21.12 https://github.com/protocolbuffers/protobuf.git "$DEPS/protobuf"
[ -d "$DEPS/gns" ] || git clone --depth 1 --branch v1.6.1 https://github.com/ValveSoftware/GameNetworkingSockets.git "$DEPS/gns"

cmake -S "$DEPS/protobuf" -B "$WORK/protobuf" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_WITH_ZLIB=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$WORK/prefix"
cmake --build "$WORK/protobuf" --target install

cmake -S "$DEPS/gns" -B "$WORK/gns" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON -DBUILD_STATIC_LIB=OFF -DENABLE_ICE=OFF -DUSE_CRYPTO=OpenSSL \
    -DOPENSSL_USE_STATIC_LIBS=ON -DProtobuf_USE_STATIC_LIBS=ON \
    -DCMAKE_PREFIX_PATH="$WORK/prefix" -DCMAKE_INSTALL_PREFIX="$INSTALL"
cmake --build "$WORK/gns" --target install
echo "GNS installed to $INSTALL"
