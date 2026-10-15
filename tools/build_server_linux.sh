#!/bin/bash
# The portable Linux DEDICATED SERVER, start to finish. Run inside the Debian 11 chroot
# (tools/linux_chroot.sh):   build_server_linux.sh <project dir> <game name> [scene]
#
# Portable means two things:
#   glibc 2.31 (the chroot)        -> runs on Debian 11+ / Ubuntu 20.04+
#   x86-64 BASELINE instructions   -> runs on any 64-bit x86 CPU, including the generic
#                                     virtual CPUs many VPS hosts expose (no AVX / FMA / F16C /
#                                     BMI / LZCNT - the engine's defaults crash there with
#                                     SIGILL before the first log line). A server does not
#                                     need the extra SIMD speed.
# The result: <project>/build/linux_server/ and <project>/build/<game>_server_linux_x86-64.tar.gz
set -e
PROJECT="$1"; GAME="$2"; SCENE="${3:-vehicletest}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=/root/serverbuild-$GAME
LOWER=$(echo "$GAME" | tr '[:upper:]' '[:lower:]')

cd "$PROJECT"
cmake --preset linux_x86-64 -B "$BUILD" -DSIMTARY_NO_SHADERS=ON \
    -DUSE_SSE4_1=OFF -DUSE_SSE4_2=OFF -DUSE_AVX=OFF -DUSE_AVX2=OFF -DUSE_AVX512=OFF \
    -DUSE_F16C=OFF -DUSE_FMADD=OFF -DUSE_LZCNT=OFF -DUSE_TZCNT=OFF
cmake --build "$BUILD" --config Release -j"$(nproc)"

bash "$HERE/package_server_linux.sh" "$BUILD/Release" /root/linux_server-$GAME "$GAME" "$SCENE"
rm -rf "$PROJECT/build/linux_server"
cp -a /root/linux_server-$GAME "$PROJECT/build/linux_server"
tar -C "$PROJECT/build" -czf "$PROJECT/build/${LOWER}_server_linux_x86-64.tar.gz" linux_server
echo "done: $PROJECT/build/${LOWER}_server_linux_x86-64.tar.gz"
