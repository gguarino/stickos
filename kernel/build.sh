#!/bin/sh
# Builds the StickOS kernel (cross-compiled): out/kernel/zImage and the .dtb files.
#   kernel/build.sh LINUX_SOURCE_DIR [BUILD_DIR]
# LINUX_SOURCE_DIR: unpacked linux-6.18.x. The patches in kernel/patches are applied once.
# CROSS_COMPILE defaults to arm-linux-gnueabihf-
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$1" && pwd)
b=${2:-$src/../build}
mkdir -p "$b" && b=$(cd "$b" && pwd)
export KBUILD_BUILD_USER=stickos KBUILD_BUILD_HOST=stickos
export ARCH=arm CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabihf-}
for p in "$here"/patches/*.patch; do
    n=$src/.applied-$(basename "$p")
    [ -e "$n" ] || { patch -d "$src" -p1 < "$p" && touch "$n"; }
done
d=$src/arch/arm/boot/dts/rockchip
cp "$here"/rk3066a-segam-m8*.dts* "$d"/
KCONFIG_ALLCONFIG="$here/stick.config" make -s -C "$src" O="$b" allnoconfig
make -s -C "$src" O="$b" -j"$(nproc)" zImage modules rockchip/rk3066a-segam-m8.dtb rockchip/rk3066a-segam-m8-simplefb.dtb \
    rockchip/rk3066a-segam-m8-simplefb-dma25.dtb rockchip/rk3066a-segam-m8-smp.dtb
mkdir -p "$here/../out/kernel"
cp "$b"/drivers/usb/dwc2/dwc2.ko "$here/../out/kernel/"
cp "$b"/arch/arm/boot/zImage "$b"/arch/arm/boot/dts/rockchip/rk3066a-segam-m8*.dtb "$here/../out/kernel/"
# initrd: kernel/initrd.sh (SD tests), then the same program as in the boot partition
# (stickinit: finds and mounts the SD image)
if [ -f "$here/../out/stickinit" ]; then
    t=$(mktemp -d)
    mkdir "$t/dev" "$t/proc" "$t/sys" "$t/bin"
    cp "$here/initrd.sh" "$t/init"
    cp "$here/../out/stickinit" "$t/stickinit"
    cp "$here/../out/root/bin/busybox" "$t/bin/"
    chmod 755 "$t/init" "$t/stickinit" "$t/bin/busybox"
    (cd "$t" && find . | LC_ALL=C sort | cpio -o -H newc --quiet -R 0:0 | gzip -9n) > "$here/../out/kernel/initrd"
    rm -r "$t"
else
    echo "out/stickinit is missing: run ./build.sh first, then build the kernel again for the initrd"
fi
ls -l "$here/../out/kernel"
