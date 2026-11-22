#!/bin/busybox sh
# /init of the bring-up initrd: tries the SD step by step with the results on the screen,
# then starts the real init (stickinit). If a step hangs, after 20 s prints what is blocked.
export PATH=/bin
/bin/busybox mkdir -p /bin /proc /sys /dev /m
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev
mount -t debugfs debug /sys/kernel/debug
say() { echo "StickOS: $(cut -d' ' -f1 /proc/uptime) $*"; }
diag() {
    say "---- BLOCKED at: $(cat /step) ----"
    echo w > /proc/sysrq-trigger
    sleep 1
    grep -E 'mmc|dma|timer' /proc/interrupts
    echo $(cat /sys/kernel/debug/mmc0/regs) 2>&1
    # DMA controller of the SD: interrupts, channel status and program counter, addresses
    for a in 0x20078020 0x20078024 0x20078028 0x20078030 0x20078034 0x20078040 0x20078100 0x20078104 0x20078400 0x20078404 0x20078408; do
        [ "$(uname -n)" = qemu ] || echo -n "$a=$(devmem $a 32) "
    done
    echo
    say "---- end: please take a photo ----"
}
step() { echo "$*" > /step; say "$*"; }
(
    last= n=0
    while [ ! -e /finished ]; do
        sleep 1
        s=$(cat /step 2>/dev/null)
        [ "$s" = "$last" ] && n=$((n + 1)) || n=0
        last=$s
        [ $n = 20 ] && diag
    done
) &
step "waiting for the SD"
d=
for i in $(seq 100); do
    for x in /dev/mmcblk0p1 /dev/mmcblk0; do
        [ -b $x ] && { [ $x = /dev/mmcblk0p1 ] || [ $i -gt 20 ]; } && d=$x && break 2
    done
    sleep 0.1
done
step "SD is $d, reading 4 MB"
dd if=$d of=/dev/null bs=64k count=64 2>&1 | tail -n1
step "mounting read-only"
mount -t vfat -o ro $d /m || say "mount failed"
step "reading system.sqfs"
sha256sum /m/stickos/system.sqfs
umount /m
step "mounting read-write"
mount -t vfat -o rw $d /m || say "mount failed"
step "writing a file"
mkdir -p /m/stickos/logs
L=/m/stickos/logs/initrd.log
date > $L
sync
step "writing 4 MB"
dd if=/dev/zero of=/m/stickos/logs/test.bin bs=64k count=64 conv=fsync 2>&1 | tail -n1
rm /m/stickos/logs/test.bin
step "writing the log"
{ cat /proc/interrupts /sys/kernel/debug/mmc0/ios; dmesg; } >> $L
sync
umount /m
touch /finished
say "SD OK, starting the system"
umount /sys/kernel/debug
exec /stickinit
