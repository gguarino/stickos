# New kernel for StickOS (work in progress)

Linux **6.18 LTS** for the SEGAM M8 stick (Rockchip RK3066), started from the SD card by
the stock 3.0.36 kernel. **Nothing is written to the flash.**

Status: builds; the jump is tested only in QEMU. **Not yet started on the real stick.**

| File | |
|---|---|
| `stick.config` | options on top of `allnoconfig` |
| `rk3066a-segam-m8.dtsi` | the stick's hardware |
| `rk3066a-segam-m8-simplefb.dts` | first bring-up: keeps the picture of the stock kernel as console, one CPU, no audio |
| `rk3066a-segam-m8.dts` | full version: KMS display, HDMI audio, GPU |
| `patches/` | HDMI audio for `rk3066_hdmi` (not in mainline) |
| `kjump.c` | starts the new kernel from the stock one |
| `test/` | test of `kjump` in QEMU |
| `build.sh` | builds everything into `out/kernel` |

## Build

```sh
./build.sh                                    # out/system.sqfs (with kjump), out/stickinit
kernel/build.sh ~/linux-6.18.64               # out/kernel: zImage, *.dtb, initrd
```

Needs `gcc-arm-linux-gnueabihf`, `bison`, `flex`, `bc`, `cpio`.

## How the stick starts it

The stock kernel has no kexec, but `/dev/mem` gives access to the RAM and the framebuffer
reserves 12 MB of contiguous memory. `kjump` puts kernel, device tree and initrd there,
then runs a small piece of code in kernel mode through an unused system call: it turns
off interrupts, caches and MMU, stops the USB DMA and jumps to the new kernel. It refuses
to run on any other kernel than the stock one (it checks the version and the system call
table).

On the SD, next to `system.sqfs`:

```
stickos/kernel/zImage
stickos/kernel/stick.dtb      one of the two .dtb files
stickos/kernel/initrd
stickos/kernel/try            empty file: try once at the next start
stickos/kernel/always         empty file: use the new kernel at every start
```

`try` is renamed to `tried` before jumping: if the new kernel does not work, switch the
stick off and on and the stock kernel is back.
The watchdog resets the stick after about 28 s if the new kernel hangs; the log it left
in RAM is then saved as `stickos/logs/ramoops.bin` (`tried` becomes `done`).
Logs: `stickos/logs/kjump.log`, `dmesg-stock.log`, `dmesg-new.log`.

The whole screen turns into a colour to show how far the jump got: red = kernel mode,
green = caches and MMU off, blue = images copied, kernel about to start.

## Known limits

- CPU clock, DDR clock, SD and USB power are left as the stock kernel set them.
- The second CPU is off in the simplefb version.
- In the simplefb version StickOS has no audio and no page flipping.
