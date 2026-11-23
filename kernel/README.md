# Linux 6.18 for StickOS

Linux **6.18 LTS** for the SEGAM M8 stick (Rockchip RK3066), started from the SD card by
the stock 3.0.36 kernel. **Nothing is written to the flash**: without the files on the SD
(or without `stickos/kernel/always`) the stick starts exactly as before.

Status: works on the real stick. Both CPUs, HDMI picture through the display driver with
page flipping, HDMI audio, SD card on 4 data lines (about 18 MB/s), USB joypads.

| File | |
|---|---|
| `stick.config` | options on top of `allnoconfig` |
| `rk3066a-segam-m8.dtsi` | the stick's hardware |
| `rk3066a-segam-m8.dts` | the version in use: display driver (VOP0 + HDMI), HDMI audio, both CPUs |
| `rk3066a-segam-m8-simplefb.dts` | fallback: keeps the picture of the stock kernel (no display driver, no audio, one CPU) |
| `rk3066a-segam-m8-simplefb-dma25.dts` | experiment: SD with DMA at 25 MHz (never tried) |
| `patches/` | kernel patches, see below |
| `kjump.c` | starts the new kernel from the stock one |
| `initrd.sh` | `/init` of the initrd: optional SD tests, then `stickinit` |
| `new.sh` | optional diagnostics, written to `stickos/logs/new.log` |
| `test/` | test of `kjump` in QEMU |
| `build.sh` | builds everything into `out/kernel` |

## Build

```sh
./build.sh                                    # out/system.sqfs (with kjump), out/stickinit
kernel/build.sh ~/linux-6.18.64               # out/kernel: zImage, *.dtb, initrd
```

Needs `gcc-arm-linux-gnueabihf`, `bison`, `flex`, `bc`, `cpio`. `./build.sh` must run
first: the initrd contains `out/stickinit` and the busybox of the image.

## Install

Copy to the SD, next to `system.sqfs` (ready files in `images/kernel/`):

```
stickos/kernel/zImage
stickos/kernel/stick.dtb      rk3066a-segam-m8.dtb
stickos/kernel/initrd
stickos/kernel/always         empty file: the new kernel at every start
```

To go back to the stock kernel, delete `stickos/kernel/always` from the PC.

Other files in `stickos/kernel/`:

| File | |
|---|---|
| `try` | the new kernel at the next start only. Empty: `stick.dtb`; otherwise names of `.dtb` files, one per line, one for each start. A line is removed (and written to `tried`) before jumping; the list ends when a start reaches StickOS. After a failed start, switch the stick off and on: the next line, or the stock kernel, starts. |
| `debug` | empty file: full-screen colours during the jump, SD tests in the initrd (reading only, then writing) and diagnostics in `new.sh` |
| `watchdog` | empty file: the watchdog resets the stick about 28 s after the jump if the new kernel hangs; its log left in RAM is saved as `stickos/logs/ramoops.bin` |

Logs in `stickos/logs/`: `kjump.log`, `dmesg-stock.log`, `dmesg-new.log`, and with `debug`
`initrd.log`, `new.log`, `clk-new.log`, `drm-state.log` (after a `try`, renamed with the name
of the `.dtb` at the next start of the stock kernel).

## How the stick starts it

The stock kernel has no kexec, but `/dev/mem` gives access to the RAM and the framebuffer
reserves 12 MB of contiguous memory. `kjump` puts kernel, device tree and initrd there,
then runs a small piece of code in kernel mode through an unused system call: it turns
off interrupts, caches and MMU, stops the USB DMA, puts the interrupt controller back to
its reset state and jumps to the new kernel. It refuses to run on any other kernel than
the stock one (it checks the version and the system call table). With `debug` (`kjump -c`)
the whole screen turns into a colour to show how far the jump got: red = kernel mode,
green = caches and MMU off, blue = images copied, kernel about to start.

The start takes the time of the stock kernel plus a few seconds.

## Patches

| Patch | |
|---|---|
| `0001` | HDMI audio for `rk3066_hdmi` (never merged upstream), ported to the DRM bridge HDMI audio operations |
| `0002` | GIC: all interrupts in group 0. The stock kernel (FIQ debugger) leaves them in group 1, which mainline never enables: no interrupt at all, not even the timer |
| `0003` | dwc2: no connector ID interrupt on host-only ports (it re-initialised the core in a loop) |
| `0004` | `rk3066_hdmi`: RGB → YCbCr converter off (left on by the stock kernel: purple picture); bridge allocated with `devm_drm_bridge_alloc` (reference count warning) |

## What was learned

- **No interrupts after the jump**: see patch 0002. `kjump` also resets the GIC.
- **`allnoconfig` turns off `COMPAT_32BIT_TIME`**: `sleep`/`usleep` of a 32-bit userland
  fail with ENOSYS. Also `BLK_DEV_WRITE_MOUNTED` is needed to mount the image with a loop
  device opened read-write.
- **SD with DMA** (pl330, external DMA of `dw_mmc`) hangs on 1 MB reads ("Card stuck being
  busy"): the SD is used without DMA. 4 data lines: 18 MB/s, near the bus limit.
- **Clocks**: mainline's `assigned-clock-rates` of the CRU change the PLLs under the
  running display. The stock values are kept (GPLL 297 MHz gives exactly the 74.25 MHz
  of 720p), and the CPU stays at 888 MHz (the voltage regulator is unknown).
- **Joypads**: the stock kernel reports the ShanWan pad as a keyboard, Linux 6.18 as a
  gamepad (`BTN_*`, hat). `stickui` keeps one button map per kernel in `joypad.cfg`.
- **Display**: the DRM fbdev emulation of Rockchip is a shadow buffer copied about 20
  times a second, not usable for games. `stickui` uses KMS directly: three dumb buffers,
  page flips without waiting, one step per vsync counted with `DRM_IOCTL_WAIT_VBLANK`.
- **Second CPU**: mainline's RK3066 SMP code powers the core off through the PMU and starts
  it again from the internal SRAM, so the state left by the stock kernel does not matter.
