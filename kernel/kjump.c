/*
 * kjump: starts a new Linux kernel from the running stock kernel (3.0.36, no kexec),
 * without writing to the flash. If anything goes wrong, power cycle: the stick starts
 * as before.
 *
 *   kjump [-w] ZIMAGE DTB INITRD    -w: start the watchdog (resets if the kernel hangs)
 *   kjump -t          only test that code can run in kernel mode
 *   kjump -d FILE     save the log left in RAM by the new kernel, if any
 *
 * How it works:
 *  - the stock framebuffer reserves 12 MB of physically contiguous memory; only the first
 *    3.6 MB are shown. The new kernel, its device tree and initrd go in the rest.
 *  - the memory is mapped at a virtual address equal to its physical address, so a small
 *    piece of code there keeps running when the MMU is switched off.
 *  - one unused entry of the kernel's system call table is replaced through /dev/mem and
 *    the system call is invoked: the code runs in kernel mode, turns off interrupts, caches
 *    and MMU, quiesces USB DMA, copies the images to their final place and jumps.
 *  - with -c the code fills the screen with a colour (and waits a second): a blocked start
 *    shows how far it got. red = kernel mode reached, green = MMU and caches off,
 *    blue = images copied.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef KJ_QEMU                         /* the stick */
#define TABLE   0x60433104ul            /* sys_call_table (physical) of stock kernel #211 */
#define NI      0xc0484a70ul            /* sys_ni_syscall: what unused entries hold */
#define MEMDEV  "/dev/fb0"
#define AREA    0x6f400000ul            /* reserved memory: physical = virtual address */
#define AREAOFF 0                       /* offset of AREA in MEMDEV */
#define AREALEN 0xc00000ul
#define SHOWN   0x400000ul              /* visible part, not used for the images */
#define PL310   0x10138000ul
#define FBSHOW  AREA                    /* screen to fill with colours */
#else                                   /* QEMU vexpress-a9, see kernel/test */
#include "kjump_qemu.h"
#endif
#define NRSYS   223                     /* unused on every ARM kernel */
#define OOPSOFF (AREALEN - 0x200000ul)  /* ramoops of the new kernel, 1 MB */
#define MARK    0x4b4a4d50              /* at the end of AREA: tells if it survives a reset */
#define STUBOFF (AREALEN - 0x220000ul)  /* code and parameters, before ramoops (0x6fe00000) */

/* where the images go */
#define Z_DST   0x62000000ul
#define D_DST   0x64000000ul
#define I_DST   0x64100000ul

/* parameters for the code in kernel mode (offsets used in the assembly below) */
struct params {
    uint32_t pl310;                     /* 0 */
    uint32_t fb;                        /* 4: where to paint, 0 = nothing */
    uint32_t pokes;                     /* 8: list of address, value pairs; address 0 ends */
    uint32_t copies;                    /* 12: list of src, dst, words; words 0 ends */
    uint32_t entry;                     /* 16 */
    uint32_t dtb;                       /* 20 */
    uint32_t list[160];
};

/* test: returns the processor mode (0x13 = supervisor) + 0x5a00 */
extern const char kj_test[], kj_test_end[], kj_go[], kj_go_end[];
__asm__(
"   .arm\n   .syntax unified\n   .balign 4\n"
"kj_test:\n"
"   mrs r0, cpsr\n"
"   and r0, r0, #0x1f\n"
"   orr r0, r0, #0x5a00\n"
"   bx lr\n"
"kj_test_end:\n"
"kj_go:\n"
"   mov r11, r0\n"                      /* params */
"   cpsid aif\n"
"   ldr r10, [r11, #4]\n"               /* fb */
"   movw r1, #0xf800\n"
"   bl 90f\n"
/* data cache off, then clean and invalidate L1 by set/way (32 KB, 4 ways, 32 bytes) */
"   dsb\n"
"   mrc p15, 0, r0, c1, c0, 0\n"
"   bic r0, r0, #4\n"
"   mcr p15, 0, r0, c1, c0, 0\n"
"   isb\n"
"   mov r1, #0\n"                       /* way << 30 */
"1: mov r2, #0\n"                       /* set << 5 */
"2: orr r3, r1, r2\n"
"   mcr p15, 0, r3, c7, c14, 2\n"
"   add r2, r2, #32\n"
"   cmp r2, #8192\n"
"   blo 2b\n"
"   adds r1, r1, #0x40000000\n"
"   bne 1b\n"
"   dsb\n"
"   mrc p15, 0, r0, c1, c0, 1\n"        /* leave SMP coherency */
"   bic r0, r0, #0x41\n"
"   mcr p15, 0, r0, c1, c0, 1\n"
"   isb\n"
/* MMU, instruction cache, branch prediction off: we are at virtual = physical */
"   mrc p15, 0, r0, c1, c0, 0\n"
"   bic r0, r0, #0x1800\n"
"   bic r0, r0, #1\n"
"   mcr p15, 0, r0, c1, c0, 0\n"
"   isb\n"
"   mov r0, #0\n"
"   mcr p15, 0, r0, c8, c7, 0\n"
"   mcr p15, 0, r0, c7, c5, 0\n"
"   mcr p15, 0, r0, c7, c5, 6\n"
"   dsb\n"
"   isb\n"
/* L2: clean and invalidate all ways, sync, off */
"   ldr r1, [r11, #0]\n"
"   cmp r1, #0\n"
"   beq 4f\n"
"   movw r2, #0xffff\n"
"   str r2, [r1, #0x7fc]\n"
"3: ldr r3, [r1, #0x7fc]\n"
"   tst r3, r2\n"
"   bne 3b\n"
"   mov r3, #0\n"
"   str r3, [r1, #0x730]\n"
"   dsb\n"
"   str r3, [r1, #0x100]\n"
"   dsb\n"
"4: movw r1, #0x07e0\n"
"   bl 90f\n"
/* device registers */
"   ldr r4, [r11, #8]\n"
"5: ldr r0, [r4], #4\n"
"   cmp r0, #0\n"
"   beq 6f\n"
"   ldr r1, [r4], #4\n"
"   str r1, [r0]\n"
"   dsb\n"
"   b 5b\n"
/* copies */
"6: ldr r4, [r11, #12]\n"
"7: ldmia r4!, {r0, r1, r2}\n"
"   cmp r2, #0\n"
"   beq 9f\n"
"8: ldr r3, [r0], #4\n"
"   str r3, [r1], #4\n"
"   subs r2, r2, #1\n"
"   bne 8b\n"
"   b 7b\n"
"9: dsb\n"
"   movw r1, #0x001f\n"
"   bl 90f\n"
"   ldr r4, [r11, #16]\n"
"   ldr r2, [r11, #20]\n"
"   mov r0, #0\n"
"   mvn r1, #0\n"
"   bx r4\n"
/* fills the screen (1280x720, 16 bit, at r10) with the colour r1, then waits about a second */
"90: cmp r10, #0\n"
"   bxeq lr\n"
"   orr r1, r1, r1, lsl #16\n"
"   mov r3, r10\n"
"   movw r2, #0x0800\n"                /* 460800 words */
"   movt r2, #7\n"
"91: str r1, [r3], #4\n"
"   subs r2, r2, #1\n"
"   bne 91b\n"
"   dsb\n"
"   mov r2, #0x400000\n"
"92: subs r2, r2, #1\n"
"   bne 92b\n"
"   bx lr\n"
"   .ltorg\n"
"kj_go_end:\n");

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("kjump: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static void die(const char *what)
{
    say("%s: %s", what, strerror(errno));
    exit(1);
}

static unsigned char *load(const char *path, size_t *len)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st))
        die(path);
    unsigned char *p = calloc(1, st.st_size + 8);
    size_t n = 0;
    while (p && n < (size_t)st.st_size) {
        ssize_t r = read(fd, p + n, st.st_size - n);
        if (r <= 0)
            die(path);
        n += r;
    }
    if (!p)
        die("memory");
    close(fd);
    *len = n;
    return p;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* sets the u32 property linux,initrd-end of the device tree */
static int fdt_set_initrd_end(unsigned char *d, size_t len, uint32_t v)
{
    if (len < 40 || be32(d) != 0xd00dfeed || be32(d + 4) > len)
        return -1;
    uint32_t off = be32(d + 8), str = be32(d + 12), end = off + be32(d + 36);
    if (end > len || str >= len)
        return -1;
    while (off + 4 <= end) {
        uint32_t tok = be32(d + off);
        off += 4;
        if (tok == 1) {                         /* node: name */
            off += (strlen((char *)d + off) + 4) & ~3u;
        } else if (tok == 3) {                  /* property: length, name offset, value */
            uint32_t l = be32(d + off), nm = be32(d + off + 4);
            off += 8;
            if (str + nm < len && !strcmp((char *)d + str + nm, "linux,initrd-end") && l == 4) {
                d[off] = v >> 24; d[off + 1] = v >> 16; d[off + 2] = v >> 8; d[off + 3] = v;
                return 0;
            }
            off += (l + 3) & ~3u;
        } else if (tok == 9) {
            break;
        } else if (tok != 2 && tok != 4) {
            return -1;
        }
    }
    return -1;
}

static void put(const char *path, const char *v)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t r = write(fd, v, strlen(v));
    say("%s = %s%s", path, v, r < 0 ? " (failed)" : "");
    close(fd);
}

/* ramoops zones start with "DBGC" (one every 128 KB): saves the area if there is one */
static int dump(const char *path)
{
    int fb = open(MEMDEV, O_RDWR | O_SYNC);
    if (fb < 0)
        die(MEMDEV);
    unsigned char *m = mmap(NULL, AREALEN, PROT_READ, MAP_SHARED, fb, AREAOFF);
    if (m == MAP_FAILED)
        die("mmap");
    int found = 0;
    uint32_t first = *(uint32_t *)(m + OOPSOFF);
    for (unsigned long o = 0; o < 0x100000; o += 0x10000)
        if (!memcmp(m + OOPSOFF + o, "DBGC", 4))
            found++;
    uint32_t mark = *(uint32_t *)(m + AREALEN - 4);
    say("ramoops zones found: %d (first word 0x%08x), mark 0x%08x: memory %s", found, first, mark,
        mark == MARK ? "kept" : "lost or cleared");
    if (!found)
        return 1;
    FILE *f = fopen(path, "wb");
    if (!f)
        die(path);
    fwrite(m + OOPSOFF, 1, 0x100000, f);
    fclose(f);
    return 0;
}

static int memfd;

static uint32_t table_get(unsigned nr)
{
    uint32_t v = 0;
    if (pread64(memfd, &v, 4, TABLE + nr * 4) != 4)
        die("read /dev/mem");
    return v;
}

static void table_set(unsigned nr, uint32_t v)
{
    if (pwrite64(memfd, &v, 4, TABLE + nr * 4) != 4)
        die("write /dev/mem");
    if (table_get(nr) != v) {
        say("the system call table cannot be changed");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    int wd = 0, colours = 0;
    for (;;) {
        if (argc > 1 && !strcmp(argv[1], "-w"))
            wd = 1;
        else if (argc > 1 && !strcmp(argv[1], "-c"))
            colours = 1;
        else
            break;
        argv++;
        argc--;
    }
    (void)wd;
    int test = argc > 1 && !strcmp(argv[1], "-t");
    if (argc == 3 && !strcmp(argv[1], "-d"))
        return dump(argv[2]);
    if (!test && argc != 4) {
        fprintf(stderr, "usage: kjump [-w] [-c] ZIMAGE DTB INITRD | -t | -d FILE\n");
        return 2;
    }
    struct utsname u;
    uname(&u);
#ifndef KJ_QEMU
    if (strncmp(u.release, "3.0.36", 6) || !strstr(u.version, "#211 ")) {
        say("not the stock kernel: %s %s", u.release, u.version);
        return 3;
    }
#endif
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0)
        die("/dev/mem");
    /* the table must look exactly as expected */
    if (table_get(17) != NI || table_get(NRSYS) != NI || table_get(31) != NI ||
        table_get(1) == NI || table_get(3) == NI || table_get(4) == NI) {
        say("unexpected system call table, stopping");
        return 3;
    }
    cpu_set_t cpu0;
    CPU_ZERO(&cpu0);
    CPU_SET(0, &cpu0);
    if (sched_setaffinity(0, sizeof(cpu0), &cpu0))
        die("sched_setaffinity");

    int fb = open(MEMDEV, O_RDWR | O_SYNC);
    if (fb < 0)
        die(MEMDEV);
    unsigned char *area = mmap((void *)AREA, AREALEN, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_SHARED | MAP_FIXED, fb, AREAOFF);
    if (area != (void *)AREA)
        die("mmap");

    /* 1: can our code run in kernel mode? (harmless) */
    unsigned char *stub = area + STUBOFF;
    memcpy(stub, kj_test, kj_test_end - kj_test);
    __asm__ volatile("dsb");
    table_set(NRSYS, (uint32_t)(uintptr_t)stub);
    long r = syscall(NRSYS);
    table_set(NRSYS, NI);
    say("test: 0x%lx (expected 0x5a13)", r);
    if (r != 0x5a13)
        return 4;
    if (test)
        return 0;

#ifndef KJ_QEMU
    volatile uint32_t *gic = mmap(NULL, 0x2000, PROT_READ, MAP_SHARED, memfd, 0x1013c000);
    if (gic != MAP_FAILED)
        say("GIC: GICD_CTLR 0x%x IGROUPR 0x%x 0x%x 0x%x 0x%x GICC_CTLR 0x%x PMR 0x%x", gic[0x1000 / 4],
            gic[0x1080 / 4], gic[0x1084 / 4], gic[0x1088 / 4], gic[0x108c / 4], gic[0x100 / 4], gic[0x104 / 4]);
#endif
    /* 2: images */
    size_t zl, dl, il;
    unsigned char *z = load(argv[1], &zl), *d = load(argv[2], &dl), *in = load(argv[3], &il);
    if (zl < 0x40 || *(uint32_t *)(z + 0x24) != 0x016f2818) {
        say("%s is not a zImage", argv[1]);
        return 1;
    }
    if (fdt_set_initrd_end(d, dl, I_DST + il)) {
        say("%s: not a valid device tree", argv[2]);
        return 1;
    }
    size_t zw = (zl + 3) & ~3u, dw = (dl + 3) & ~3u, iw = (il + 3) & ~3u;
    if (SHOWN + zw + dw + iw > STUBOFF || D_DST + dw > I_DST) {
        say("images too big: %zu + %zu + %zu", zl, dl, il);
        return 1;
    }
    unsigned char *zs = area + SHOWN, *ds = zs + zw, *is = ds + dw;
    memset(area + OOPSOFF, 0, 0x100000);        /* no leftovers from earlier attempts */
    *(volatile uint32_t *)(area + AREALEN - 4) = MARK;
    memcpy(zs, z, zw);
    memcpy(ds, d, dw);
    memcpy(is, in, iw);
    __asm__ volatile("dsb");
    if (memcmp(zs, z, zw) || memcmp(ds, d, dw) || memcmp(is, in, iw)) {
        say("the images do not match in memory");
        return 1;
    }
    say("kernel %zu, device tree %zu, initrd %zu bytes ready", zl, dl, il);

    /* 3: code and parameters */
    unsigned char *pp = stub + 0x1000;
    struct params p = { 0 };
    uint32_t n = 0, base = (uint32_t)(uintptr_t)pp + offsetof(struct params, list);
    p.pl310 = PL310;
    p.fb = colours ? FBSHOW : 0;
    p.entry = Z_DST;
    p.dtb = D_DST;
    p.pokes = base;
#ifndef KJ_QEMU
    for (int i = 0; i < 10; i++) {              /* CRU: all the clock gates open */
        p.list[n++] = 0x200000d0 + i * 4;
        p.list[n++] = 0xffff0000;
    }
    /* both USB controllers: interrupts and DMA off, core reset */
    static const uint32_t usb[] = { 0x10180000, 0x101c0000 };
    for (int i = 0; i < 2; i++) {
        p.list[n++] = usb[i] + 0x008; p.list[n++] = 0;
        p.list[n++] = usb[i] + 0x010; p.list[n++] = 1;
    }
    /* GIC: the stock kernel (FIQ debugger) puts the interrupts in group 1, the new kernel
     * only enables group 0 and would get no interrupts at all */
    p.list[n++] = 0x1013c100; p.list[n++] = 0;          /* cpu interface off */
    p.list[n++] = 0x1013d000; p.list[n++] = 0;          /* distributor off */
    for (int i = 0; i < 8; i++) {
        p.list[n++] = 0x1013d180 + i * 4; p.list[n++] = 0xffffffff;     /* disable */
        p.list[n++] = 0x1013d280 + i * 4; p.list[n++] = 0xffffffff;     /* not pending */
        p.list[n++] = 0x1013d080 + i * 4; p.list[n++] = 0;              /* group 0 */
    }
    /* watchdog: resets the stick after about 28 s, unless the new kernel takes it over */
    if (wd) {
    p.list[n++] = 0x2004c004; p.list[n++] = 0xff;
    p.list[n++] = 0x2004c00c; p.list[n++] = 0x76;
    p.list[n++] = 0x2004c000; p.list[n++] = 0x1d;
    }
#endif
    p.list[n++] = 0;
    p.copies = base + n * 4;
    uint32_t c[] = { (uint32_t)(uintptr_t)zs, Z_DST, zw / 4, (uint32_t)(uintptr_t)ds, D_DST, dw / 4,
                     (uint32_t)(uintptr_t)is, I_DST, iw / 4, 0, 0, 0 };
    memcpy(p.list + n, c, sizeof(c));
    memcpy(pp, &p, sizeof(p));
    memcpy(stub, kj_go, kj_go_end - kj_go);
    __asm__ volatile("dsb");

    /* 4: quiet system */
    put("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance");
    put("/sys/bus/usb/devices/usb1/authorized", "0");
    put("/sys/bus/usb/devices/usb2/authorized", "0");
    put("/sys/devices/system/cpu/cpu1/online", "0");
    say("starting the new kernel");
    sync();
    sleep(1);
    table_set(NRSYS, (uint32_t)(uintptr_t)stub);
    r = syscall(NRSYS, pp);
    table_set(NRSYS, NI);
    say("came back?! 0x%lx", r);
    return 5;
}
