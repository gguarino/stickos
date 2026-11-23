# Bring-up on the new kernel: run by /sbin/init from the SD (stickos/kernel/new.sh).
# Every step is written to the SD before doing it, so a hang shows where it happened.
K=/sd/stickos/kernel D=/sd/stickos/logs L=/sd/stickos/logs/new.log
: > $L
step() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $L; sync; echo "StickOS: $*" > /dev/tty0; }
run() { step "$*"; "$@" >> $L 2>&1; echo "  rc $?" >> $L; sync; }
rd() { step "read $1 ($2)"; devmem $1 32 >> $L 2>&1; sync; }

step "start, kernel $(uname -a)"
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
run sha256sum /sd/stickos/system.sqfs
step "SD speed, reading 32 MB"
t0=$(cut -d' ' -f1 /proc/uptime)
echo 3 > /proc/sys/vm/drop_caches
dd if=/dev/mmcblk0 of=/dev/null bs=1M count=32 2>> $L
echo "  from $t0 to $(cut -d' ' -f1 /proc/uptime)" >> $L
run cat /proc/cpuinfo
run cat /proc/meminfo
run cat /proc/iomem
run cat /proc/interrupts
run cat /sys/kernel/debug/mmc0/ios
cat /sys/kernel/debug/clk/clk_summary > $D/clk-new.log 2>&1
dmesg > $D/dmesg-new.log
sync

step "USB registers"
rd 0x200000e4 "CRU clock gates 5"
rd 0x2000817c "GRF phy0 con0"
rd 0x20008180 "GRF phy0 con1"
rd 0x20008184 "GRF phy0 con2"
rd 0x20008188 "GRF phy1 con0"
rd 0x2000815c "GRF soc status"
rd 0x101c0040 "host controller id"
rd 0x10180040 "otg controller id"
echo 0 > /sys/bus/platform/drivers_autoprobe
run insmod $K/dwc2.ko
for d in 101c0000 10180000; do
    step "starting USB $d"
    echo $d.usb > /sys/bus/platform/drivers/dwc2/bind 2>> $L
    step "USB $d: bind finished"
    sleep 3
    dmesg > $D/dmesg-new.log
    step "USB $d: 3 s later"
done
echo 1 > /sys/bus/platform/drivers_autoprobe
run cat /proc/bus/input/devices
run ls -l /sys/bus/hid/devices/
# joypad: what arrives from USB (hidraw) and from the input layer (event), while buttons are pressed
joy() {
    for i in 1 2 3; do echo "StickOS: >>>>> PRESS THE JOYPAD BUTTONS NOW / PREMI I TASTI DEL JOYPAD ORA <<<<<" > /dev/tty0; done
    step "joypad test $1: 10 s"
    grep usb /proc/interrupts >> $L
    for d in /dev/hidraw0 /dev/hidraw1 /dev/input/event0 /dev/input/event1; do
        [ -e $d ] && timeout 10 cat $d > /tmp/$(basename $d) 2>> $L &
    done
    sleep 11
    grep usb /proc/interrupts >> $L
    for d in hidraw0 hidraw1 event0 event1; do
        [ -e /tmp/$d ] && echo "  $d: $(wc -c < /tmp/$d) bytes, first: $(head -c 32 /tmp/$d | od -An -tx1 | tr -d '\n')" >> $L
    done
    sync
    step "joypad test $1 done: $(cat /tmp/hidraw0 /tmp/hidraw1 2>/dev/null | wc -c) bytes from USB, $(cat /tmp/event0 /tmp/event1 2>/dev/null | wc -c) bytes of events"
    rm -f /tmp/hidraw? /tmp/event?
}
joy "1 (driver $(basename $(readlink /sys/bus/hid/devices/*:20BC:5500.0001/driver 2>/dev/null)))"
if [ -e /sys/bus/hid/drivers/betop ]; then
    # same test with the generic HID driver instead of the force feedback one
    echo 1 > /sys/module/hid/parameters/ignore_special_drivers 2>> $L
    for h in /sys/bus/hid/drivers/betop/0003:*; do
        [ -e "$h" ] && n=$(basename $h) && echo $n > /sys/bus/hid/drivers/betop/unbind && echo $n > /sys/bus/hid/drivers_probe
    done
    sleep 2
    run ls -l /sys/bus/hid/devices/*/driver
    joy "2 (driver $(basename $(readlink /sys/bus/hid/devices/*:20BC:5500.0001/driver 2>/dev/null)))"
fi
run cat /proc/interrupts
dmesg > $D/dmesg-new.log
step "end"
