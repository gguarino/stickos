#!/bin/sh
# Builds StickOS: compiles on an ARM build host (Debian/Ubuntu armhf, reached by ssh),
# creates the squashfs image here.
#   out/system.sqfs   -> copy to the SD as stickos/system.sqfs
#   out/stickinit     -> /init of the boot partition (see tools/mkboot.py)
# BUILD_HOST = ssh name of the ARM machine (default: cuboxi). Extra cores built with
# cores.sh on that machine are taken from its ~/cores/out.
set -e
cd "$(dirname "$0")"
python3 ui/checklang.py
H=${BUILD_HOST:-cuboxi}
R=stickos/build
CORES="libretro-nestopia libretro-genesisplusgx libretro-gambatte libretro-beetle-pce-fast"
# core libraries taken from the packages (not from what is installed on cuboxi)
LIBS="libsndfile1 libvorbis0a libvorbisenc2 libvorbisfile3 libogg0 libflac14 libmp3lame0 libmpg123-0t64 libopus0"
ssh $H "rm -rf $R && mkdir -p $R"
scp -q ui/stickui.c ui/font.h ui/lang.h ui/libretro.h boot/stickinit.c $H:$R/
scp -q -r names $H:$R/
ssh $H "set -e; cd $R
CF='-O2 -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wall'
gcc \$CF -o stickui stickui.c -ldl -lz -Wl,--no-as-needed -lm
strip stickui
gcc -static -Os -s -Wall -o stickinit stickinit.c
mkdir -p debs && cd debs && apt-get download $CORES $LIBS busybox-static >/dev/null && cd ..
mkdir -p x && for d in debs/*.deb; do dpkg -x \$d x; done
F=root; rm -rf \$F
mkdir -p \$F/bin \$F/usr/bin \$F/usr/lib/arm-linux-gnueabihf \$F/usr/lib/libretro \$F/proc \$F/sys \$F/dev \$F/tmp \$F/sd \$F/sbin
ln -s usr/lib \$F/lib
ln -s arm-linux-gnueabihf/ld-linux-armhf.so.3 \$F/usr/lib/ld-linux-armhf.so.3
cp x/usr/bin/busybox \$F/bin/
for a in \$(x/usr/bin/busybox --list); do [ \$a = busybox ] || ln -sf busybox \$F/bin/\$a; done
cp stickui \$F/usr/bin/
mkdir -p \$F/usr/share/stickos && cp -r names \$F/usr/share/stickos/
cp x/usr/lib/arm-linux-gnueabihf/libretro/*.so \$F/usr/lib/libretro/
[ -d ~/cores/out ] && { cp ~/cores/out/*.so \$F/usr/lib/libretro/ 2>/dev/null || true; }; echo cores: \$(ls \$F/usr/lib/libretro)
for l in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 libstdc++.so.6 libgcc_s.so.1; do
  cp -L /usr/lib/arm-linux-gnueabihf/\$l \$F/usr/lib/arm-linux-gnueabihf/
done
# transitive dependencies (libraries of libraries too)
while :; do
  for f in \$F/usr/bin/stickui \$F/usr/lib/libretro/*.so \$F/usr/lib/arm-linux-gnueabihf/*.so*; do
    readelf -d \$f | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'
  done | sort -u > needed.txt
  added=0
  for l in \$(cat needed.txt); do
    [ -e \$F/usr/lib/arm-linux-gnueabihf/\$l ] && continue
    if [ -e x/usr/lib/arm-linux-gnueabihf/\$l ]; then cp -L x/usr/lib/arm-linux-gnueabihf/\$l \$F/usr/lib/arm-linux-gnueabihf/
    else cp -L /usr/lib/arm-linux-gnueabihf/\$l \$F/usr/lib/arm-linux-gnueabihf/; fi
    added=1
  done
  [ \$added = 1 ] || break
done
echo libraries: \$(ls \$F/usr/lib/arm-linux-gnueabihf)
tar -C \$F -cf root.tar .
"
scp -q boot/../rootfs/sbin/init $H:$R/init.sh
ssh $H "cd $R && mkdir -p root/sbin && cp init.sh root/sbin/init && chmod 755 root/sbin/init && tar -C root -cf root.tar .
# check: every executable/core must load inside the image as on the stick (like 'ldd -r')
set -e
for f in root/usr/bin/stickui root/usr/lib/libretro/*.so; do
  f=\${f#root}
  out=\$(sudo chroot root /bin/sh -c \"LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf /lib/ld-linux-armhf.so.3 \$f\" 2>&1)
  if echo \"\$out\" | grep -qi 'not found\\|undefined\\|error'; then echo \"\$out\"; echo \"CHECK FAILED: \$f\"; exit 1; fi
  echo \"ok \$f\"
done
for p in /lib/ld-linux-armhf.so.3 /sbin/init /bin/sh; do sudo chroot root /bin/sh -c \"test -x $p\" || { echo \"missing $p\"; exit 1; }; done
echo CHECK OK"
rm -rf out/root && mkdir -p out/root
scp -q $H:$R/root.tar out/ && tar -C out/root -xf out/root.tar && rm out/root.tar
scp -q $H:$R/stickinit out/stickinit
rm -f out/system.sqfs
mksquashfs out/root out/system.sqfs -comp gzip -all-root -noappend -quiet
ls -l out/system.sqfs out/stickinit
sha256sum out/system.sqfs out/stickinit
