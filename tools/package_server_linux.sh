#!/bin/bash
# Package a Linux DEDICATED SERVER folder from a portable build (made in the Debian 11 chroot,
# tools/linux_chroot.sh, with -DSIMTARY_NO_SHADERS=ON). Run inside the chroot:
#   package_server_linux.sh <build>/Release <out-dir> <game-name> [scene]
# The result runs on any x86-64 Linux with glibc >= 2.31 (Debian 11+, Ubuntu 20.04+) and
# needs no packages: every non-system library it links is copied beside it ($ORIGIN rpath).
set -e
SRC="$1"; OUT="$2"; GAME="$3"; SCENE="${4:-vehicletest}"
[ -x "$SRC/$GAME" ] || { echo "no $SRC/$GAME"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"

cp -a "$SRC/$GAME" "$SRC/$GAME"-* "$OUT/" 2>/dev/null || cp -a "$SRC/$GAME" "$OUT/"
cp "$SRC"/*.so* "$OUT/"
cp "$SRC/crashpad_handler" "$OUT/" 2>/dev/null || true
cp -r "$SRC/assets" "$OUT/"

# Everything the binaries load that is not glibc / the C++ runtime (always present, and a
# copy of an OLDER libstdc++ would only shadow the system's newer, compatible one).
SYSTEM='^(linux-vdso|libc\.|libm\.|libdl\.|libpthread\.|librt\.|ld-linux|libgcc_s\.|libstdc\+\+\.)'
for bin in "$OUT/$GAME" "$OUT"/*.so*; do
    ldd "$bin" 2>/dev/null | awk '/=> \//{print $1, $3}' | while read name path; do
        if echo "$name" | grep -qE "$SYSTEM"; then continue; fi
        [ -e "$OUT/$name" ] || cp -L "$path" "$OUT/$name"
    done
done

cat > "$OUT/run_server.sh" <<RUN
#!/bin/sh
# Dedicated server: headless, no GPU. Extra arguments pass through, e.g.
#   ./run_server.sh --server=27020 --transport=tcp --server-config=server.cfg
cd "\$(dirname "\$0")"
exec ./$GAME --scene=$SCENE --server --server-config=server.cfg "\$@"
RUN
chmod +x "$OUT/run_server.sh"

cat > "$OUT/server.cfg" <<CFG
# $GAME dedicated server. Command-line arguments override these.
port = 27015
max_players = 16
name = $GAME server
# gns (UDP, default) or tcp (for networks that block UDP - clients must match)
transport = gns
CFG
echo "server packaged in $OUT ($(du -sh "$OUT" | cut -f1))"
ls "$OUT"
