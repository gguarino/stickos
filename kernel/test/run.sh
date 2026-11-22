#!/bin/sh
# Tests kjump on QEMU vexpress-a9 (two Cortex-A9, RAM at 0x60000000 like the stick).
#   kernel/test/run.sh BUILD_DIR DTB BUSYBOX WORKDIR
# BUILD_DIR: 6.18 build of stick.config plus kernel/test/qemu.config.
# DTB: vexpress-v2p-ca9.dtb with 256 MB of RAM, a no-map reserved region at 0x6f400000
#      (12 MB), chosen/linux,initrd-start and -end, bootargs with maxcpus=1 second=1.
# BUSYBOX: static ARM busybox. Success prints "SECOND KERNEL OK".
set -e
here=$(cd "$(dirname "$0")" && pwd)
b=$1 dtb=$2 bb=$3 w=$4
mkdir -p "$w/fs/bin" "$w/fs/proc" "$w/fs/sys" "$w/fs/dev" "$w/fs/d"
t=$(sed -n 's/ T sys_call_table$//p' "$b/System.map")
n=$(sed -n 's/ T sys_ni_syscall$//p' "$b/System.map")
${CROSS_COMPILE:-arm-linux-gnueabihf-}gcc -O2 -Wall -marm -march=armv7-a -mfpu=vfpv3-d16 -D_FILE_OFFSET_BITS=64 \
    -static -DKJ_QEMU -DTABLE="$(printf '0x%xul' $((0x$t - 0x60000000)))" -DNI="0x${n}ul" \
    -I"$here" -o "$w/fs/bin/kjump" "$here/../kjump.c"
cp "$bb" "$w/fs/bin/busybox"
cat > "$w/fs/init" <<'I'
#!/bin/busybox sh
b=/bin/busybox
$b mount -t proc proc /proc; $b mount -t sysfs sysfs /sys; $b mount -t devtmpfs d /dev
if [ ! -e /d/zImage ]; then
    echo "SECOND KERNEL OK"; $b uname -a
else
    echo "FIRST KERNEL"; /bin/kjump -t && /bin/kjump /d/zImage /d/dtb /d/initrd; echo "RETURNED $?"
fi
$b poweroff -f
I
chmod 755 "$w/fs/init"
pack() { (cd "$w/fs" && find . | cpio -o -H newc --quiet -R 0:0 | gzip -1) > "$1"; }
rm -f "$w"/fs/d/*
pack "$w/second"
cp "$b/arch/arm/boot/zImage" "$w/fs/d/zImage"; cp "$dtb" "$w/fs/d/dtb"; cp "$w/second" "$w/fs/d/initrd"
pack "$w/first"
timeout 120 ${QEMU:-qemu-system-arm} -M vexpress-a9 -smp 2 -m 256M -nographic -no-reboot \
    -kernel "$b/arch/arm/boot/zImage" -dtb "$dtb" -initrd "$w/first" \
    -append "console=ttyAMA0 rdinit=/init first=1" < /dev/null | tee "$w/log"
grep -q "SECOND KERNEL OK" "$w/log"
