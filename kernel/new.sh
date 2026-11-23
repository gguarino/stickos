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
run cat /proc/interrupts
step "display and audio"
run ls -l /dev/dri /dev/snd /dev/fb0
for c in /sys/class/drm/card*-*; do
    [ -e $c/status ] && echo "  $c: $(cat $c/status), modes: $(cat $c/modes | tr '\n' ' ')" >> $L
done
run cat /proc/asound/cards /proc/asound/pcm
cat /sys/kernel/debug/dri/0/state > $D/drm-state.log 2>&1
dmesg > $D/dmesg-new.log
step "end"
