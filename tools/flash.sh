#!/bin/sh
# StickOS flashing helper for the SEGAM M8 stick (Rockchip loader mode + rkdeveloptool).
#
#   tools/flash.sh backup  DIR        dump boot, kernel and the whole SPI flash into DIR
#   tools/flash.sh install IMG BACKUP write IMG (built by mkboot.py) to the boot partition
#   tools/flash.sh restore BACKUP     write the stock boot partition back
#
# Only the boot partition (LBA 0x1800, 0x400 sectors = 512 KB) is ever written.
# The IDB, PARM and kernel partitions are never touched.
# Set RKDEV to the rkdeveloptool command if it is not in PATH (e.g. RKDEV="sudo rkdeveloptool").
set -e
RK=${RKDEV:-rkdeveloptool}
BOOT_LBA=0x1800
BOOT_LEN=0x400
KERNEL_LBA=0x80
KERNEL_LEN=0x1780
FLASH_LEN=0x8000                     # 16 MB of SPI NOR, in 512-byte sectors

die() { echo "ERROR: $*" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)

device() {
    $RK ld | tee /dev/stderr | grep -q "Loader" ||
        die "no stick in loader mode (hold the reset button while plugging it into USB)"
}

# 0 if FILE is a boot image whose ramdisk already contains init.stock (i.e. StickOS)
is_stickos() {
    python3 -c "
import gzip, sys
sys.path.insert(0, '$here')
from mkboot import unpack_krnl, cpio_read
e = cpio_read(gzip.decompress(unpack_krnl(open('$1', 'rb').read())))
sys.exit(0 if any(x['name'] == 'init.stock' for x in e) else 1)" 2>/dev/null
}

case "$1" in
backup)
    [ -n "$2" ] || die "usage: $0 backup DIR"
    mkdir -p "$2"
    device
    $RK rl $BOOT_LBA $BOOT_LEN "$2/boot.img"
    $RK rl $KERNEL_LBA $KERNEL_LEN "$2/kernel.img"
    $RK rl 0 $FLASH_LEN "$2/flash_16MB.bin"
    $RK rl $BOOT_LBA $BOOT_LEN "$2/boot_check.img"
    cmp "$2/boot.img" "$2/boot_check.img" || die "two reads of the boot partition differ"
    rm "$2/boot_check.img"
    if is_stickos "$2/boot.img"; then
        echo "WARNING: this boot partition already contains StickOS: it is not a stock backup"
    fi
    (cd "$2" && sha256sum boot.img kernel.img flash_16MB.bin | tee SHA256SUMS)
    echo "Backup done in $2. Keep it safe: it is the only way back to the stock system."
    ;;
install)
    [ -f "$2" ] && [ -f "$3" ] || die "usage: $0 install IMG BACKUP_BOOT.img"
    [ "$(stat -c %s "$2")" = 524288 ] || die "$2 is not 512 KB: build it with tools/mkboot.py"
    is_stickos "$2" || die "$2 is not a StickOS boot image"
    device
    tmp=$(mktemp -d)
    $RK rl $BOOT_LBA $BOOT_LEN "$tmp/current.img"
    if cmp -s "$tmp/current.img" "$3"; then
        echo "current boot partition == your backup (stock)"
    elif is_stickos "$tmp/current.img"; then
        echo "current boot partition is an older StickOS: it will be updated"
    else
        die "the current boot partition is neither your backup nor StickOS: stopping"
    fi
    $RK wl $BOOT_LBA "$2"
    $RK rl $BOOT_LBA $BOOT_LEN "$tmp/check.img"
    cmp "$tmp/check.img" "$2" || die "read-back differs! Do not unplug: run '$0 restore $3'"
    rm -rf "$tmp"
    echo "BOOT VERIFIED: StickOS installed. Unplug the stick and connect it to the TV."
    ;;
restore)
    [ -f "$2" ] || die "usage: $0 restore BACKUP_BOOT.img"
    [ "$(stat -c %s "$2")" = 524288 ] || die "$2 is not 512 KB"
    device
    $RK wl $BOOT_LBA "$2"
    tmp=$(mktemp -d)
    $RK rl $BOOT_LBA $BOOT_LEN "$tmp/check.img"
    cmp "$tmp/check.img" "$2" || die "read-back differs, try again"
    rm -rf "$tmp"
    echo "STOCK BOOT RESTORED AND VERIFIED"
    ;;
*)
    sed -n '2,12p' "$0"
    exit 1
    ;;
esac
