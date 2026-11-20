#!/bin/bash
# Builds the libretro cores from the official sources (github.com/libretro) on the ARM
# build host (run it there; ideally a Cortex-A9 like the stick). Usage:  ./cores.sh [name...]
# Results in ~/cores/out/*.so, logs in ~/cores/log/
set -u
B=~/cores; mkdir -p $B/src $B/out $B/log; cd $B/src
J=${J:-3}
OPT="-O2 -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard"
export CFLAGS="$OPT" CXXFLAGS="$OPT"
get() { [ -d "$1" ] || git clone -q --depth 1 --recursive --shallow-submodules "https://github.com/libretro/$2" "$1"; }
build() { # name repo subdir file.so command...
  local n=$1 repo=$2 dir=$3 so=$4; shift 4
  echo "[$(date +%T)] $n: start"
  get $n $repo || { echo "$n: download FAILED"; return; }
  ( cd $n/$dir && "$@" ) > $B/log/$n.log 2>&1
  if [ -f "$n/$dir/$so" ]; then cp "$n/$dir/$so" $B/out/; strip $B/out/$so; echo "[$(date +%T)] $n: OK $(du -h $B/out/$so | cut -f1)";
  else echo "[$(date +%T)] $n: FAILED (see log/$n.log)"; tail -5 $B/log/$n.log; fi
}
all="snes9x2010 picodrive gpsp pcsx_rearmed stella2014 atari800 prosystem fbalpha2012 fbalpha mame2003"
for c in ${@:-$all}; do case $c in
  snes9x2010)   build $c snes9x2010 . snes9x2010_libretro.so make -j$J -f Makefile.libretro platform=unix LIBM=-lm ;;
  picodrive)    build $c picodrive . picodrive_libretro.so make -j$J -f Makefile.libretro platform=armv ARCH=arm ;;
  gpsp)         build $c gpsp . gpsp_libretro.so make -j$J platform=unix ARCH=arm HAVE_DYNAREC=1 CPU_ARCH=arm ;;
  pcsx_rearmed) build $c pcsx_rearmed . pcsx_rearmed_libretro.so make -j$J -f Makefile.libretro ARCH=arm HAVE_NEON=1 BUILTIN_GPU=neon DYNAREC=ari64 ;;
  stella2014)   build $c stella2014-libretro . stella2014_libretro.so make -j$J platform=unix ;;
  atari800)     build $c libretro-atari800 . atari800_libretro.so make -j$J platform=unix ;;
  prosystem)    build $c prosystem-libretro . prosystem_libretro.so make -j$J platform=unix ;;
  fbalpha2012)  build $c fbalpha2012 svn-current/trunk fbalpha2012_libretro.so make -j$J -f makefile.libretro platform=unix ;;
  fbalpha)      build $c fbalpha . fbalpha_libretro.so make -j$J -f makefile.libretro platform=unix ;;
  mame2003)     build $c mame2003-libretro . mame2003_libretro.so make -j$J platform=unix ARCH=arm ;;
esac; done
echo "[$(date +%T)] DONE"; ls -la $B/out
