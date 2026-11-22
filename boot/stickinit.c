/*
 * /init of the "boot" partition (stock kernel 3.0.36).
 *
 * If a FAT32/exFAT partition of the microSD holds stickos/system.sqfs, it is mounted
 * (loop + squashfs) and StickOS is started (/sbin/init of the image).
 * Otherwise, or if anything goes wrong, the stock system is started
 * (/init.stock), as if this program were not there.
 * To force the stock system with the SD inserted: create the file
 * stickos/disabled on the SD.
 * Log: stickos/logs/boot.log on the SD.
 *
 * Build (ARM, static):  gcc -static -Os -s -o init stickinit.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static int logfd = -1;

static void say(const char *fmt, ...)
{
    char buf[512];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(buf, sizeof(buf), "[%5ld.%02ld] ", (long)ts.tv_sec, ts.tv_nsec / 10000000);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    if (access("/init.stock", F_OK) && write(2, buf, n) < 0) {   /* initrd of the new kernel */
    }
    if (logfd >= 0) {
        if (write(logfd, buf, n) < 0) {
        }
        fsync(logfd);
    }
}

static int node_from_sysfs(const char *sysdev, const char *node)
{
    char buf[32];
    int fd = open(sysdev, O_RDONLY), ma, mi;
    if (fd < 0)
        return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    if (sscanf(buf, "%d:%d", &ma, &mi) != 2)
        return -1;
    unlink(node);
    return mknod(node, S_IFBLK | 0600, makedev(ma, mi));
}

static void stock(char **argv)
{
    say("starting the stock system");
    if (logfd >= 0)
        close(logfd);
    logfd = -1;
    umount("/sb/sd");
    umount("/sb/proc");
    umount("/sb/sys");
    execv("/init.stock", argv);
    say("no /init.stock: nothing else to start");
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    (void)argc;
    if (getpid() != 1) {
        execv("/init.stock", argv);
        return 127;
    }
    mkdir("/sb", 0755);
    mkdir("/sb/sys", 0755);
    mkdir("/sb/proc", 0755);
    mkdir("/sb/dev", 0755);
    mkdir("/sb/sd", 0755);
    mkdir("/sb/root", 0755);
    mount("sysfs", "/sb/sys", "sysfs", 0, NULL);
    mount("proc", "/sb/proc", "proc", 0, NULL);

    /* Looks on every partition of the microSD (or on the whole card if it has no
     * partitions) for a FAT32/exFAT filesystem containing stickos/system.sqfs.
     * Waits up to 5 s for the card. */
    int i, ok = -1;
    char found[16] = "";
    for (i = 0; i < 50 && ok; i++) {
        for (int part = 1; part <= 17 && ok; part++) {
            char sysdev[96];
            if (part <= 16)
                snprintf(sysdev, sizeof(sysdev), "/sb/sys/block/mmcblk0/mmcblk0p%d/dev", part);
            else
                snprintf(sysdev, sizeof(sysdev), "/sb/sys/block/mmcblk0/dev");   /* card without partitions */
            if (node_from_sysfs(sysdev, "/sb/dev/sd"))
                continue;
            static const char *fs[][2] = { { "vfat", "utf8,shortname=mixed" }, { "vfat", NULL },
                                           { "exfat", NULL } };
            int m = -1;
            for (unsigned k = 0; k < 3 && m; k++)
                m = mount("/sb/dev/sd", "/sb/sd", fs[k][0], MS_NOATIME, fs[k][1]);
            if (m)
                continue;
            if (access("/sb/sd/stickos/system.sqfs", F_OK) == 0 ||
                access("/sb/sd/stickos/disabled", F_OK) == 0) {
                ok = 0;
                snprintf(found, sizeof(found), part <= 16 ? "p%d" : "whole card", part);
            } else {
                umount("/sb/sd");
            }
        }
        if (ok)
            usleep(100000);
    }
    if (ok)
        stock(argv);
    mkdir("/sb/sd/stickos", 0755);
    mkdir("/sb/sd/stickos/logs", 0755);
    logfd = open("/sb/sd/stickos/logs/boot.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    say("stickinit: StickOS found on the SD (partition %s) after %d attempts", found, i);
    if (access("/sb/sd/stickos/disabled", F_OK) == 0) {
        say("found stickos/disabled");
        stock(argv);
    }
    int img = open("/sb/sd/stickos/system.sqfs", O_RDONLY);
    if (img < 0) {
        say("stickos/system.sqfs: %s", strerror(errno));
        stock(argv);
    }
    mknod("/sb/dev/loop0", S_IFBLK | 0600, makedev(7, 0));
    int loop = open("/sb/dev/loop0", O_RDWR);
    if (loop < 0)
        loop = open("/sb/dev/loop0", O_RDONLY);
    if (loop < 0 || ioctl(loop, LOOP_SET_FD, img)) {
        say("loop0: %s", strerror(errno));
        close(img);
        stock(argv);
    }
    struct loop_info64 li;
    memset(&li, 0, sizeof(li));
    li.lo_flags = LO_FLAGS_READ_ONLY;
    strncpy((char *)li.lo_file_name, "system.sqfs", LO_NAME_SIZE - 1);
    ioctl(loop, LOOP_SET_STATUS64, &li);
    close(img);
    if (mount("/sb/dev/loop0", "/sb/root", "squashfs", MS_RDONLY, NULL)) {
        say("mount squashfs: %s", strerror(errno));
        ioctl(loop, LOOP_CLR_FD, 0);
        stock(argv);
    }
    if (access("/sb/root/sbin/init", X_OK)) {
        say("/sbin/init missing from the image");
        umount("/sb/root");
        ioctl(loop, LOOP_CLR_FD, 0);
        stock(argv);
    }
    say("starting StickOS");
    close(logfd);
    logfd = -1;
    if (mount("/sb/sd", "/sb/root/sd", NULL, MS_MOVE, NULL)) {
        umount("/sb/sd");
    }
    umount("/sb/proc");
    umount("/sb/sys");
    if (chdir("/sb/root") == 0 && mount(".", "/", NULL, MS_MOVE, NULL) == 0 && chroot(".") == 0 &&
        chdir("/") == 0) {
        char *a[] = { "/sbin/init", NULL };
        execv("/sbin/init", a);
    }
    /* unrecoverable: nothing left to do but a manual reboot */
    for (;;)
        pause();
}
