#!/usr/bin/env python3
"""Builds the StickOS "boot" partition image (ramdisk) for the SPI flash.

Starts from the stick's own stock boot partition: renames /init to /init.stock
and adds /init (stickinit, static ARM binary). Then gzips the cpio and wraps it
in the Rockchip format: "KRNL" + size + data + rkcrc32.
Nothing from the stock firmware is distributed: you dump your own boot.img first.

usage: mkboot.py STOCK_BOOT.img STICKINIT OUT.img
"""
import gzip, struct, sys

PART_SIZE = 0x400 * 512          # boot partition: 0x400 sectors (see PARM)


def rkcrc(data):
    tab = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04c10db7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
        tab.append(c)
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xffffffff) ^ tab[((crc >> 24) ^ b) & 0xff]
    return crc


def unpack_krnl(img):
    tag, size = struct.unpack_from('<4sI', img, 0)
    assert tag == b'KRNL', 'not a KRNL image'
    body = img[8:8 + size]
    assert struct.unpack_from('<I', img, 8 + size)[0] == rkcrc(body), 'bad CRC in the original image'
    return body


def cpio_read(data):
    out, off = [], 0
    while True:
        assert data[off:off + 6] == b'070701', 'not a newc cpio at %d' % off
        f = [int(data[off + 6 + i * 8: off + 14 + i * 8], 16) for i in range(13)]
        ino, mode, uid, gid, nlink, mtime, fsize, dmaj, dmin, rmaj, rmin, nsize, chk = f
        name = data[off + 110: off + 110 + nsize - 1].decode()
        off = (off + 110 + nsize + 3) & ~3
        body = data[off: off + fsize]
        off = (off + fsize + 3) & ~3
        if name == 'TRAILER!!!':
            return out
        out.append(dict(name=name, mode=mode, uid=uid, gid=gid, nlink=nlink, mtime=mtime,
                        rmaj=rmaj, rmin=rmin, body=body))


def cpio_write(entries):
    out = bytearray()
    for ino, e in enumerate(entries + [dict(name='TRAILER!!!', mode=0, uid=0, gid=0, nlink=1,
                                            mtime=0, rmaj=0, rmin=0, body=b'')]):
        name = e['name'].encode() + b'\0'
        hdr = '070701' + ''.join('%08X' % v for v in (
            ino + 1 if e['name'] != 'TRAILER!!!' else 0, e['mode'], e['uid'], e['gid'], e['nlink'],
            e['mtime'], len(e['body']), 0, 0, e['rmaj'], e['rmin'], len(name), 0))
        out += hdr.encode() + name
        out += b'\0' * (-len(out) % 4)
        out += e['body']
        out += b'\0' * (-len(out) % 4)
    out += b'\0' * (-len(out) % 512)
    return bytes(out)


def main():
    orig, initbin, out = sys.argv[1:4]
    entries = cpio_read(gzip.decompress(unpack_krnl(open(orig, 'rb').read())))
    names = [e['name'] for e in entries]
    assert 'init' in names and 'init.stock' not in names
    for e in entries:
        if e['name'] == 'init':
            e['name'] = 'init.stock'
        # sbin/ueventd and sbin/watchdogd are links to ../init: they must keep pointing to the stock init
        if e['mode'] >> 12 == 0o12 and e['body'] in (b'../init', b'/init', b'init'):
            e['body'] = e['body'].replace(b'init', b'init.stock')
            print('link %s -> %s' % (e['name'], e['body'].decode()))
    entries.append(dict(name='init', mode=0o100750, uid=0, gid=0, nlink=1, mtime=0,
                        rmaj=0, rmin=0, body=open(initbin, 'rb').read()))
    body = gzip.compress(cpio_write(entries), 9, mtime=0)
    img = b'KRNL' + struct.pack('<I', len(body)) + body + struct.pack('<I', rkcrc(body))
    print('cpio entries: %d, gzip: %d bytes, image: %d bytes, partition: %d bytes (free %d)'
          % (len(entries), len(body), len(img), PART_SIZE, PART_SIZE - len(img)))
    if len(img) > PART_SIZE:
        sys.exit('ERROR: does not fit in the boot partition')
    img += b'\0' * (PART_SIZE - len(img))
    open(out, 'wb').write(img)

    # check: read it back and verify
    chk = cpio_read(gzip.decompress(unpack_krnl(img)))
    d = {e['name']: e for e in chk}
    assert d['init']['body'] == open(initbin, 'rb').read()
    orig_init = [e for e in cpio_read(gzip.decompress(unpack_krnl(open(orig, 'rb').read())))
                 if e['name'] == 'init'][0]
    assert d['init.stock']['body'] == orig_init['body'] and d['init.stock']['mode'] == orig_init['mode']
    print('check OK: new init, init.stock identical to the original, %d entries' % len(chk))


if __name__ == '__main__':
    main()
