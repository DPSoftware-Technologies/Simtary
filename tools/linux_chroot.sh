#!/bin/bash
# Enter (or run a command in) the Debian 11 build chroot used for portable Linux builds.
# Old glibc (2.31) on purpose: a binary built here runs on Debian 11+ / Ubuntu 20.04+.
# Made once with: debootstrap --variant=minbase bullseye /opt/sbx  (see MULTIPLAYER.md)
#   sudo Simtary/tools/linux_chroot.sh                 # shell
#   sudo Simtary/tools/linux_chroot.sh <command...>    # one command
set -e
ROOT=/opt/sbx
mountpoint -q $ROOT/proc || mount -t proc proc $ROOT/proc
mountpoint -q $ROOT/dev  || mount --bind /dev $ROOT/dev
mountpoint -q $ROOT/sys  || mount --bind /sys $ROOT/sys
mkdir -p $ROOT/mnt/e
mountpoint -q $ROOT/mnt/e || mount --bind /mnt/e $ROOT/mnt/e
cp /etc/resolv.conf $ROOT/etc/resolv.conf
if [ $# -eq 0 ]; then exec chroot $ROOT /bin/bash -l; fi
exec chroot $ROOT /bin/bash -lc "$*"
