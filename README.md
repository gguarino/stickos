# StickOS

An open, minimal retro-gaming OS for the **SEGAM M8 V5.0** HDMI game stick
(the cheap "4K game stick" sold with two wireless joypads).

It replaces the stock game menu with a fast, folder-based frontend running
[libretro](https://www.libretro.com/) emulators: put your ROMs in
`roms/<system>/` on the microSD and they show up in the menu. No game database,
no hidden folders, no stock-firmware hacks.

The stock system is **not removed**: without the SD card (or with a marker file on it) the
stick boots exactly as before, and a single command restores the original boot partition.

> ⚠️ Flashing firmware always carries a risk. Read the whole guide before starting, make the
> backup, and use this project at your own risk. This project has no affiliation with the
> stick's manufacturer.

---

## Contents

- [Features](#features)
- [Hardware](#hardware)
- [How it works](#how-it-works)
- [Supported systems](#supported-systems)
- [Install with the ready images](#install-with-the-ready-images)
- [Preparing the microSD card](#preparing-the-microsd-card)
- [Using StickOS](#using-stickos)
- [Going back to the stock system](#going-back-to-the-stock-system)
- [Building everything from source](#building-everything-from-source)
- [Repository layout](#repository-layout)
- [Troubleshooting](#troubleshooting)
- [Technical notes](#technical-notes)
- [Licenses](#licenses)

---

## Features

- Boots straight into the menu in about 3 seconds after the kernel.
- One folder per system; zipped console ROMs are supported.
- Arcade games are listed by their full title (`sf2.zip` → *Street Fighter II: The World
  Warrior*), and BIOS zips such as `neogeo.zip` are hidden. The titles come from each
  emulator's own driver list.
- The menu remembers the last system and game played.
- Pause menu in game (**SELECT + START**): resume, save state, load state, restart, quit.
- Game saves (`.srm`) are written automatically and kept next to the games in
  `roms/<system>/save/`.
- Joypad configuration wizard with per-model profiles, including analog sticks and L2/R2/L3/R3.
  Both wireless pads work as player 1 and player 2.
- Every emulator option can be changed from **Settings**, including the controller type
  (e.g. DualShock for the PlayStation).
- Menu in English, Italian, French, German and Spanish.
- Built-in boot logo, or your own `stickos/logo.bmp`.
- 50 Hz (PAL) games are paced correctly on the 60 Hz HDMI output; audio is resampled
  to the HDMI rate.
- Detailed logs on the SD (`stickos/logs/`) for troubleshooting.

## Hardware

| | |
|---|---|
| Device | SEGAM M8 V5.0 HDMI stick (board labelled *M8*) |
| SoC | Rockchip **RK3066** (register level, see below), 2× Cortex-A9 up to 912 MHz, Mali-400 |
| RAM | 256 MB (244 MB visible to Linux) |
| Boot flash | 16 MB SPI NOR (PUYA PY25Q128HA) |
| Video | HDMI 1280×720, framebuffer RGB565 |
| Audio | HDMI (ALSA "ROCKCHIP HDMI") |
| Input | "ShanWan Twin USB Joystick" 2.4 GHz receiver (two evdev devices) |
| Storage | microSD (games, saves, the StickOS system image) |
| Stock software | Linux 3.0.36 + Android-style init + a proprietary game menu |

Despite what the vendor firmware says, the SoC is not an RK3032/RK3036 (those are Cortex-A7):
the running kernel reports two Cortex-A9 cores (MIDR `0x413fc090`). The chip marking is
under the heatsink, but everything visible from software is an **RK3066**: USB id
`2207:300a`, six GPIO banks, 512 KB of L2 cache, the `mach-rk30` / `rk30-hdmi` stock
drivers, and a peripheral map (LCDC, HDMI, I2S, USB, SD, SPI) that matches the mainline
`rk3066a.dtsi`. The boot loader still passes the RK2928 machine id (2928).
Earlier versions of this page wrongly said RK3026/RK3028A.

## How it works

```
 SPI flash (16 MB)                                microSD (FAT32 or exFAT)
 ┌──────────────────────────┐                     ┌────────────────────────────┐
 │ IDB / loader   (stock)   │                     │ stickos/system.sqfs        │
 │ PARM           (stock)   │                     │ stickos/joypad.cfg, logs/… │
 │ kernel 3.0.36  (stock)   │                     │ roms/<system>/…            │
 │ boot (ramdisk) ──────────┼── /init = stickinit │ bios/…                     │
 │   /init.stock  (stock)   │        │            └────────────────────────────┘
 └──────────────────────────┘        │
                                     ▼
       stickinit looks for stickos/system.sqfs on every SD partition
            │ found                                │ not found / stickos/disabled / error
            ▼                                      ▼
   loop-mount the squashfs, move the SD        exec /init.stock
   to /sd, chroot, exec /sbin/init             (the stock system starts as usual)
            │
            ▼
   busybox init script → stickui (menu + libretro frontend, one process per game)
```

- Only the **boot** partition of the SPI flash is replaced: its ramdisk gets a new `/init`
  (`stickinit`, a small static C program). The original `/init` is kept as `/init.stock`.
  The bootloader, partition table and kernel stay stock.
- The whole OS lives in `stickos/system.sqfs` on the SD card (about 32 MB). It contains
  busybox, glibc, `stickui` and the emulator cores. Updating StickOS means copying one
  file to the SD.
- `stickui` is a single C program with no dependencies beyond libc and zlib. It drives the
  framebuffer, ALSA (raw ioctls) and evdev directly and loads the libretro cores with `dlopen`.

## Supported systems

| Menu entry | SD folder | Emulator (libretro core) | Extensions |
|---|---|---|---|
| Nintendo NES | `roms/nes` | Nestopia | nes fds unf unif |
| Super Nintendo | `roms/snes` | Snes9x 2010 | sfc smc fig swc |
| Sega Mega Drive | `roms/megadrive` | PicoDrive | md gen smd bin |
| Sega Master System | `roms/mastersystem` | Genesis Plus GX | sms |
| Sega Game Gear | `roms/gamegear` | Genesis Plus GX | gg |
| Game Boy | `roms/gb` | Gambatte | gb |
| Game Boy Color | `roms/gbc` | Gambatte | gbc gb |
| Game Boy Advance | `roms/gba` | gpSP | gba |
| PC Engine | `roms/pcengine` | Beetle PCE Fast | pce cue |
| PlayStation | `roms/psx` | PCSX-ReARMed | cue chd pbp m3u iso img |
| Atari 2600 | `roms/atari2600` | Stella 2014 | a26 bin |
| Atari 5200 | `roms/atari5200` | Atari800 | a52 bin |
| Atari 7800 | `roms/atari7800` | ProSystem | a78 bin |
| Arcade (FB Alpha 2012) | `roms/arcade` | FB Alpha 2012 | zip |
| Arcade (FB Alpha) | `roms/fbalpha` + `roms/neogeo` | FB Alpha | zip |
| Arcade (MAME 2003) | `roms/mame2003` | MAME 2003 (0.78 romsets) | zip |

Console ROMs (not CD images) can also be zipped. Arcade romsets must match the emulator
version. A system only appears in the menu when its folder contains at least one game.

Tested on the stick at full speed: NES, SNES, Master System, PlayStation (Tekken 3,
Crash Bandicoot 1–3, Radikal Bikers), Neo Geo and MAME 2003. MAME 2016 was tried and
dropped: it is too heavy for this hardware and would make the image 80 MB.

## Install with the ready images

The ready images live in [`images/`](images/):

| File | What it is |
|---|---|
| `images/system.sqfs` | the OS: copy it to the SD as `stickos/system.sqfs` |
| `images/stickinit` | the new `/init` for the boot partition (static ARM binary) |
| `images/kernel/` | Linux 6.18 for the stick, started from the SD (branch `kernel-6.18`, see [`kernel/README.md`](kernel/README.md)) |
| `images/SHA256SUMS` | checksums |

The boot partition image is **not** distributed, because it contains the manufacturer's
ramdisk. You build it in one second from the backup of **your own** stick with
`tools/mkboot.py`. That backup is also your way back to the stock system.

### What you need

- A Linux PC with a free **USB 2.0** port (see [Troubleshooting](#troubleshooting) for
  USB 3/xHCI issues), `python3` and `git`.
- [`rkdeveloptool`](https://github.com/rockchip-linux/rkdeveloptool) (build it from
  source; on Debian/Ubuntu: `sudo apt install libudev-dev libusb-1.0-0-dev dh-autoreconf
  pkg-config`, then `autoreconf -i && ./configure && make`).
- A USB cable from the stick's micro-USB port to the PC.
- A microSD card (any size; FAT32 or exFAT).

### 1. Put the stick in loader mode

The stock loader switches to its USB download mode (rockusb, shown as `Loader`) when the
stick is powered from a PC's USB port instead of a charger or the TV.

1. Unplug the stick from the TV and from any power source.
2. Connect the stick's micro-USB port to the PC. If it is not detected, unplug it, hold
   the **reset button** (in the small hole) and plug it in again while holding it.
3. Check:

   ```sh
   sudo rkdeveloptool ld
   # DevNo=1 Vid=0x2207,Pid=...,LocationID=... Loader
   ```

   You must see **`Loader`**. If you see `Maskrom` or nothing at all, unplug the stick
   and try again.

All the commands below use `tools/flash.sh`. It writes **only** the boot partition
(LBA `0x1800`, 512 KB), never the bootloader, partition table or kernel, and it reads
back and compares everything it writes. If `rkdeveloptool` needs root, prefix the
commands with `RKDEV="sudo rkdeveloptool"`.

### 2. Back up the stock flash (mandatory)

```sh
git clone https://github.com/gguarino/stickos.git && cd stickos
RKDEV="sudo rkdeveloptool" tools/flash.sh backup ~/m8-backup
```

This saves `boot.img` (boot partition), `kernel.img` and `flash_16MB.bin` (the whole
16 MB SPI flash), plus `SHA256SUMS`. **Keep this folder safe** (copy it elsewhere too).

For reference, the stock boot partition of the stick this project was developed on has
SHA-256 `d74e3d960f845321aa07899120ba6a7b90977dbbe4b0f1a39d5028262e6c80da`. Yours may
differ if your firmware is a different version. That is fine, because mkboot.py works on
any stock image that has an `/init`.

### 3. Build your StickOS boot image

```sh
sha256sum -c images/SHA256SUMS
python3 tools/mkboot.py ~/m8-backup/boot.img images/stickinit boot_stickos.img
```

It prints `check OK: new init, init.stock identical to the original`. With the reference
stock image above, the result has SHA-256
`676595eeeb234fa9a3fa418677de0d602f72a116783762bf35266365c925c3cf`, which is the exact
image tested on the hardware.

### 4. Write it

```sh
RKDEV="sudo rkdeveloptool" tools/flash.sh install boot_stickos.img ~/m8-backup/boot.img
```

The script first checks that the current boot partition is your stock backup (or an
older StickOS), then writes it, reads it back and compares. You must see
`BOOT VERIFIED`.

Unplug the stick. Without an SD card prepared for StickOS it still boots the stock
system as before.

### 5. Prepare the SD card

See the next section, insert the card, connect the stick to the TV, and the menu
appears.

## Preparing the microSD card

1. Format the card as **FAT32** or **exFAT**. A single partition is fine; other
   partitions are also scanned, so the stock card layout works too.
2. Copy the SD template from this repository and the system image:

   ```sh
   cp -r sd/. /media/SDCARD/
   cp images/system.sqfs /media/SDCARD/stickos/
   sync
   ```

3. Copy your games into `roms/<system>/` (each folder has a `_README.txt` that lists the
   accepted extensions) and any BIOS files into `bios/` (see `bios/_README.txt`).

The resulting layout:

```
/stickos/system.sqfs        the OS (replace this file to update StickOS)
/stickos/README.txt         user guide on the card
/stickos/stickos.cfg        language, last game played          (created by StickOS)
/stickos/joypad.cfg         joypad button profiles               (created by StickOS)
/stickos/options/<core>.cfg emulator options changed from Settings (created by StickOS)
/stickos/logs/*.log         boot.log, init.log, stickui.log, dmesg.log
/stickos/logo.bmp           optional custom boot logo (24/32-bit BMP, ideally 1280x720)
/stickos/disabled           optional: if present, the stock system boots
/roms/<system>/             your games
/roms/<system>/save/        saves and save states                (created by StickOS)
/bios/                      BIOS files
```

**BIOS files and games are not included** and never will be. Use dumps of hardware
and games you own.

| BIOS file | System | |
|---|---|---|
| `scph1001.bin` | PlayStation | recommended (otherwise the less compatible built-in HLE BIOS is used) |
| `gba_bios.bin` | Game Boy Advance | recommended |
| `gb_bios.bin`, `gbc_bios.bin` | Game Boy / Color | optional |
| `disksys.rom` | Famicom Disk System | needed for `.fds` games |
| `syscard3.pce` | PC Engine CD | needed for CD games in `.cue` format |
| `5200.rom` | Atari 5200 | required |
| `7800 BIOS (U).rom` | Atari 7800 | optional |
| `neogeo.zip` | Neo Geo | required; put it in `roms/neogeo/` (or `roms/fbalpha/`) |

PlayStation tips:
- Prefer `.chd`, because the images are smaller and there is one file per disc.
- European (PAL) games protected by LibCrypt need a `.sbi` file with the same name next
  to the image (e.g. `Radikal Bikers.sbi`). Without it they freeze after a few seconds.

## Using StickOS

| Where | Button | Action |
|---|---|---|
| Menus | D-pad | move |
| Menus | A | select |
| Menus | B | back |
| Menus | Left/Right | page up/down |
| In game | **SELECT + START** (hold) | pause menu |
| In game, pad without SELECT | START held 2 s | pause menu |

- **First start / new joypad**: StickOS offers the button wizard. Press each requested
  button. If a button does not exist on your pad, wait and it is skipped. If you press
  nothing for 10 seconds the default mapping is used. You can redo the wizard any time
  from **Settings → Joypad**.
- **Settings → Language**: English, Italiano, Français, Deutsch, Español.
- **Settings → \<emulator\>**: every core option. Only the values you change are saved,
  in `stickos/options/<core>.cfg`.
- **Information**: version and connected joypads.

Updating StickOS: copy the new `system.sqfs` over `stickos/system.sqfs`. Everything
else (settings, saves) is kept.

## Going back to the stock system

- **Temporarily**: remove the SD card, or create an empty file `stickos/disabled` on it.
  `stickinit` then starts the stock system unchanged.
- **Permanently**: put the stick in loader mode and write back your backup:

  ```sh
  RKDEV="sudo rkdeveloptool" tools/flash.sh restore ~/m8-backup/boot.img
  ```

## Building everything from source

### Build machine

The build is split in two:

- **Host PC** (any Linux, x86 is fine): runs `build.sh`, needs `python3`, `ssh`, `scp`
  and `squashfs-tools` (`mksquashfs`).
- **ARM build host**: an armhf Debian/Ubuntu machine reachable by `ssh` (the project was
  built on a Cubox-i, also a Cortex-A9). It needs `gcc`, `make`, `git`, `zlib1g-dev`,
  `apt-get download` access to the distribution's armhf packages, and passwordless
  `sudo` for `chroot` (used only by the final load check). All binaries and libraries in
  the image come from this machine's distribution: the image ships its own glibc, so
  the distribution version does not matter as long as it is armhf. A glibc built for
  kernel 3.2+ is known to work on the stick's 3.0.36 kernel.

Set `BUILD_HOST` to the ssh name of the ARM machine (default `cuboxi`).

### 1. Emulator cores

Four cores (Nestopia, Genesis Plus GX, Gambatte, Beetle PCE Fast) are taken from the
distribution's `libretro-*` packages. The others are built from the official libretro
sources by `cores.sh`, which you run **on the ARM build host**:

```sh
scp cores.sh armhost:
ssh armhost ./cores.sh                 # all cores (several hours on a Cortex-A9)
ssh armhost ./cores.sh pcsx_rearmed    # or only some of them
```

The results go to `~/cores/out/*.so` on the ARM host, and `build.sh` picks them up from
there. Flags: `-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard`. Do not use cores built
for Cortex-A7 boards (VFPv4/IDIV instructions), because they crash on this Cortex-A9.

Versions in the published image:

| Core | Source | Version |
|---|---|---|
| snes9x2010 | libretro/snes9x2010 | git 82904a9 |
| picodrive | libretro/picodrive | git 1890c29 |
| gpsp | libretro/gpsp | git 5819380 |
| pcsx_rearmed | libretro/pcsx_rearmed | git 7b0e8f8 |
| stella2014 | libretro/stella2014-libretro | git 7d1361e |
| atari800 | libretro/libretro-atari800 | git 4e7fbc7 |
| prosystem | libretro/prosystem-libretro | git 8a88014 |
| fbalpha2012 | libretro/fbalpha2012 | git 0ce3153 |
| fbalpha | libretro/fbalpha | git b25b724 |
| mame2003 | libretro/mame2003-libretro | git 07f262e |
| nestopia | Ubuntu `libretro-nestopia` | 1.53.2+20260228 |
| genesis_plus_gx | Ubuntu `libretro-genesisplusgx` | 1.7.4+git20221128 |
| gambatte | Ubuntu `libretro-gambatte` | 0.5.0+git20160522 |
| beetle-pce-fast | Ubuntu `libretro-beetle-pce-fast` | 0.9.38.7+git20160609 |

### 2. The system image

```sh
BUILD_HOST=armhost ./build.sh
```

`build.sh` does the following:
1. Checks the translations (`ui/checklang.py`).
2. Compiles `stickui` and `stickinit` on the ARM host.
3. Assembles the root filesystem: busybox, glibc, the cores and every library they need,
   resolved transitively.
4. Checks inside a `chroot` that every binary and core loads, as on the stick.
5. Creates `out/system.sqfs` and `out/stickinit` on the PC.

Then:

```sh
python3 tools/mkboot.py ~/m8-backup/boot.img out/stickinit boot_stickos.img   # boot image
cp out/system.sqfs /media/SDCARD/stickos/                                       # OS image
```

To publish new ready images, copy `out/system.sqfs` and `out/stickinit` into `images/`
and regenerate `images/SHA256SUMS`.

### 3. Arcade titles (optional)

`stickos/names/<core>.txt` (`zip<TAB>flag<TAB>title`, where flag `b` marks a BIOS) is
generated from each core's driver sources:

```sh
python3 tools/arcadenames.py fbalpha2012 ~/cores/src/fbalpha2012/svn-current/trunk/src/burn names/fbalpha2012.txt
python3 tools/arcadenames.py fbalpha     ~/cores/src/fbalpha/src/burn                    names/fbalpha.txt
python3 tools/arcadenames.py mame2003    ~/cores/src/mame2003/src/drivers                names/mame2003.txt
```

### 4. Translations

The menu strings are in `ui/lang.h`. The English text is the key, followed by Italian,
French, German and Spanish. Only Latin-1 characters can be used (the font is Terminus
8×16 Latin-1). `ui/checklang.py` checks that every row is present, that the format
specifiers match and that the text fits on screen. It runs automatically in `build.sh`.
To add a language, add a column to every row and an entry to `lang_code`, `lang_name`
and `lang_retro` in `stickui.c`.

### Tests without the stick

`test/` contains small headless programs that include `stickui.c` and run a core with the
same callbacks, on the ARM build host:

- `runtest.c`: loads a core and a ROM and runs 300 frames, with a crash handler.
- `psxtest.c`: runs a CD game, counts real frames and dumps screenshots (`.ppm`).
- `ziptest.c`: the built-in zip reader.

Example:

```sh
gcc -O2 -DSD=\"$HOME/sdtest\" -o psxtest test/psxtest.c -ldl -lz -lm
./psxtest pcsx_rearmed_libretro.so game.chd 1500 /tmp
```

## Repository layout

```
boot/stickinit.c      /init of the boot partition: finds the SD image, else starts the stock init
ui/stickui.c          menu + libretro frontend (framebuffer, ALSA, evdev, zip, options, saves)
ui/lang.h             menu translations
ui/font.h             Terminus 8x16 font (Latin-1)
ui/libretro.h         libretro API header
ui/checklang.py       translation checker (run by build.sh)
rootfs/sbin/init      init script of the image (mounts, logs, runs stickui in a loop)
build.sh              builds out/system.sqfs and out/stickinit (via BUILD_HOST)
cores.sh              builds the libretro cores from source (run on the ARM host)
names/                arcade title lists (generated by tools/arcadenames.py)
sd/                   SD card template (folders, README files)
images/               ready images: system.sqfs, stickinit, kernel/, SHA256SUMS
tools/mkboot.py       builds the boot partition image from your stock boot.img
tools/flash.sh        backup / install / restore with rkdeveloptool
tools/arcadenames.py  extracts arcade titles from the emulators' sources
test/                 headless test programs
```

## Troubleshooting

**`rkdeveloptool ld` finds nothing, or `dmesg` shows `error -71`**
: The stick's USB in loader mode is picky. Use a USB 2.0 port or a USB 2.0 hub, try
  another port (on the development machine only one of two identical ports worked),
  and use a short cable. Re-enter loader mode after each failed attempt.

**`rkdeveloptool` works only as root**
: Add a udev rule for vendor `2207`, or use `RKDEV="sudo rkdeveloptool"`.

**The stick boots the stock menu instead of StickOS**
: Check `stickos/logs/boot.log` on the SD. It shows which partitions were scanned and
  why the image was not used. The file must be exactly `stickos/system.sqfs`, and the
  partition must be FAT32 or exFAT. Also check that `stickos/disabled` does not exist.

**A game does not start**
: The message on screen points to `stickos/logs/stickui.log`. The core's own messages
  are logged there too. Arcade romsets must match the emulator version.

**A PlayStation game freezes after a few seconds**
: If the log says `LibCrypt protected game with missing SBI`, add the `.sbi` file
  (see above).

**Slowdowns**
: Every 10 s the log contains a line like
  `performance: game 50.0 fps (expected 50.0), screen 25.0 fps, audio underruns 0`.
  `game` should stay close to `expected`. `screen` is the number of different frames
  the game drew, so 25 or 30 is normal for many 3D games. The next line
  (`per frame: core … ms, drawing … ms; cpu: …`) shows where the time goes: emulation,
  drawing, or waiting for the SD (`wait-io`).
  PlayStation games use PCSX-ReARMed's automatic frameskip by default: a frame's drawing is
  skipped when the audio is about to run dry. Change it in Settings → PCSX-ReARMed.

**Something is stuck**
: A watchdog writes `watchdog: game loop stopped …` to the log, with the state of every
  thread, if a game stops for more than 3 seconds.

## Technical notes

Things learned while porting to this hardware. They may be useful to anyone working with
the RK3066 or old Rockchip kernels.

- **Boot.** The stick boots from the internal SPI NOR only; the bootrom does not boot the
  microSD. The stock loader's rockusb mode (`Loader`) supports reading and writing LBA
  sectors, and that is all StickOS needs. Mask ROM recovery works (short SPI pin 6 CLK
  to pin 4 GND at power-on), but loading a RAM loader in Mask ROM mode was never
  completed. That is why the bootloader area is never written.
- **Boot partition format.** `KRNL` + little-endian size + gzip'd newc cpio + Rockchip
  CRC32 (polynomial `0x04c10db7`), padded to 0x400 sectors. `tools/mkboot.py`
  implements it and verifies its own output.
- **Kernel 3.0.36.** It has devtmpfs, loop, squashfs, vfat and exfat (no ext4), and
  framebuffer page flipping with `FBIO_WAITFORVSYNC` at 60 Hz. A current armhf glibc
  runs fine on it.
- **Only 244 MB of RAM.** CD images are never read into memory; the core reads them
  itself (`need_fullpath`).
- **Callee-saved VFP registers.** PCSX-ReARMed does not preserve `d8-d15` across
  `retro_run()`, which the ARM AAPCS requires. Floating-point state kept in those
  registers by the frontend (e.g. the 50 Hz pacing accumulator) got corrupted, and PAL
  games froze on the first screen. Every call into a core now goes through a small
  assembly trampoline (`vfp_call` in `stickui.c`) that saves and restores `d8-d15`. The
  frontend also restores FPSCR if a core changes it.
- **Pacing.** The display is always 60 Hz. A 50 Hz core skips one tick in six (the TV
  repeats a frame), and the audio is resampled with a small feedback on the ALSA delay,
  so there is no drift and no underruns.

## Licenses

- StickOS code (`boot/`, `ui/stickui.c`, `ui/lang.h`, `ui/checklang.py`, `tools/`,
  `test/`, scripts): **MIT**, see [`LICENSE`](LICENSE).
- `ui/libretro.h`: MIT, © The RetroArch team.
- `ui/font.h`: Terminus font, SIL Open Font License 1.1, © Dimitar Toshkov Zhekov.
- `images/system.sqfs` contains third-party software under its own licenses: busybox
  (GPL-2.0), glibc (LGPL-2.1), zlib, and the libretro cores. Several cores (Snes9x,
  FB Alpha, MAME 2003, PicoDrive, among others) allow **non-commercial use only**, so
  the image must not be sold or bundled with commercial products. The corresponding
  source code is available from the upstream repositories at the versions listed above
  and from the Ubuntu archive for the packaged components.
- No BIOS, firmware or game is included in this repository.
