/*
 * stickui: menu and libretro frontend for the SEGAM M8 stick (kernel 3.0, fbdev).
 *
 *   stickui                           menu: systems -> games, settings
 *   stickui --play CORE ROM SYSTEM    runs a game (child process of the menu)
 *   stickui --options CORE FILE [ROM] writes the options a core declares to FILE
 *
 * Video: /dev/fb0 1280x720 RGB565, double buffer with FBIOPAN_DISPLAY (waits for vsync).
 * Audio: ALSA through direct ioctls, resampling with dynamic rate control.
 * Input: evdev; buttons configurable per joypad model (/sd/stickos/joypad.cfg).
 * Menu language: /sd/stickos/stickos.cfg, translations in lang.h.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <sound/asound.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "font.h"
#include "libretro.h"

#ifndef SD                                  /* redefinable for tests on a PC */
#define SD        "/sd"
#endif
#define CFGDIR    SD "/stickos"
#define ROMS      SD "/roms"
#define OLDSAVES  SD "/saves"             /* saves folder used up to 0.1 */
#define BIOS      SD "/bios"
#define COREDIR   "/usr/lib/libretro"
#define LOGDIR    CFGDIR "/logs"
#define VERSION   "0.1"

/* ================================================================ utilities */
static FILE *logf_;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logm(const char *fmt, ...)
{
    if (!logf_)
        return;
    va_list ap;
    fprintf(logf_, "[%8.2f %d] ", now(), (int)getpid());
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fputc('\n', logf_);
    fflush(logf_);
}

static void log_open(void)
{
    struct stat st;
    mkdir(CFGDIR, 0755);
    mkdir(LOGDIR, 0755);
    if (stat(LOGDIR "/stickui.log", &st) == 0 && st.st_size > 512 * 1024)
        rename(LOGDIR "/stickui.log", LOGDIR "/stickui.old.log");
    logf_ = fopen(LOGDIR "/stickui.log", "a");
}

static void mkdirs(const char *path)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *len = n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int ok = write(fd, data, len) == (ssize_t)len;
    fsync(fd);
    close(fd);
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    sync();
    return 0;
}

/* ================================================================ language */
enum { L_EN, L_IT, L_FR, L_DE, L_ES, NLANG };
static const char *lang_code[NLANG] = { "en", "it", "fr", "de", "es" };
static const char *lang_name[NLANG] = { "English", "Italiano", "Français", "Deutsch", "Español" };
static const unsigned lang_retro[NLANG] = { RETRO_LANGUAGE_ENGLISH, RETRO_LANGUAGE_ITALIAN, RETRO_LANGUAGE_FRENCH,
                                            RETRO_LANGUAGE_GERMAN, RETRO_LANGUAGE_SPANISH };
static int lang;                            /* L_EN unless stickos.cfg says otherwise */

#include "lang.h"

/* text in the current language (the English text is the key) */
static const char *T(const char *en)
{
    if (lang == L_EN)
        return en;
    for (size_t i = 0; i < NTR; i++)
        if (!strcmp(tr_table[i].s[L_EN], en))
            return tr_table[i].s[lang] ? tr_table[i].s[lang] : en;
    return en;
}

/* stickos.cfg: general StickOS settings, key=value (language, last game played) */
#define MAXSET 16
static struct { char key[32], val[256]; } sets[MAXSET];
static int nsets;

static const char *set_get(const char *key)
{
    for (int i = 0; i < nsets; i++)
        if (!strcmp(sets[i].key, key))
            return sets[i].val;
    return "";
}

static void set_put(const char *key, const char *val)
{
    int i;
    for (i = 0; i < nsets && strcmp(sets[i].key, key); i++)
        ;
    if (i == MAXSET)
        return;
    if (i == nsets)
        nsets++;
    snprintf(sets[i].key, sizeof(sets[i].key), "%s", key);
    snprintf(sets[i].val, sizeof(sets[i].val), "%s", val);
}

static void settings_load(void)
{
    FILE *f = fopen(CFGDIR "/stickos.cfg", "r");
    char line[320];
    nsets = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            set_put(line, eq + 1);
        }
        fclose(f);
    }
    lang = L_EN;
    for (int i = 0; i < NLANG; i++)
        if (!strcmp(set_get("language"), lang_code[i]))
            lang = i;
}

static void settings_save(void)
{
    char buf[MAXSET * 300];
    int n = snprintf(buf, sizeof(buf), "# StickOS settings\n# language: en it fr de es\n");
    for (int i = 0; i < nsets; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s=%s\n", sets[i].key, sets[i].val);
    if (write_file(CFGDIR "/stickos.cfg", buf, n))
        logm("stickos.cfg: write failed");
}

/* ================================================================ video */
static int fb = -1;
static uint8_t *fbmem;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static int XRES, YRES, STRIDE, back = 1, pages = 2;
static uint16_t *ui;                         /* menu buffer (RAM) */

static int video_init(void)
{
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0)
        fb = open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0) {
        logm("fb0: %s", strerror(errno));
        return -1;
    }
    /* text console in graphics mode: the kernel does not write over the picture any more */
    int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty >= 0) {
        if (ioctl(tty, KDSETMODE, KD_GRAPHICS))
            logm("KDSETMODE: %s", strerror(errno));
        close(tty);
    }
    ioctl(fb, FBIOGET_VSCREENINFO, &var);
    if (var.yres_virtual < 2 * var.yres || var.bits_per_pixel != 16) {
        var.yres_virtual = 2 * var.yres;
        var.bits_per_pixel = 16;
        var.activate = FB_ACTIVATE_NOW;
        if (ioctl(fb, FBIOPUT_VSCREENINFO, &var))
            logm("FBIOPUT_VSCREENINFO: %s", strerror(errno));
        ioctl(fb, FBIOGET_VSCREENINFO, &var);
    }
    ioctl(fb, FBIOGET_FSCREENINFO, &fix);
    XRES = var.xres;
    YRES = var.yres;
    STRIDE = fix.line_length / 2;
    fbmem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) {
        logm("mmap fb: %s", strerror(errno));
        return -1;
    }
    /* a single page (e.g. simple-framebuffer on the new kernel): draws on the shown page */
    if (var.yres_virtual < 2 * var.yres || fix.smem_len < 2 * (size_t)YRES * fix.line_length) {
        pages = 1;
        back = 0;
    }
    ui = calloc((size_t)XRES * YRES, 2);
    logm("video %dx%d bpp %d stride %d virt %d pages %d", XRES, YRES, var.bits_per_pixel, STRIDE,
         var.yres_virtual, pages);
    return 0;
}

static uint16_t *page(int p)
{
    return (uint16_t *)(fbmem + (size_t)p * YRES * fix.line_length);
}

static double vs_t;                         /* time of the last known vsync */
#define VS_PERIOD (1.0 / 60.1)

static void wait_vsync(void);

static void present(void)
{
    if (pages == 1) {                        /* no page flipping: keeps the pace only */
        wait_vsync();
        return;
    }
    var.yoffset = back * YRES;
    ioctl(fb, FBIOPAN_DISPLAY, &var);       /* returns at vsync */
    back ^= 1;
    vs_t = now();
}

/* Waits for the next vsync without changing the picture. FBIO_WAITFORVSYNC on this
 * kernel returns immediately if a vsync has just passed, so the time is computed
 * from the last real vsync (present) and we sleep until then. */
/* watchdog: if the game loop stops for 3 s, the log says where it is stuck
 * (inside the core or not) and the state of every thread of the process */
static volatile unsigned wd_count;
static volatile int wd_in_core;

static void wait_vsync(void)
{
    wd_count++;
    double t = vs_t + VS_PERIOD, n = now();
    if (t < n - VS_PERIOD) {                /* more than one frame late: realign */
        vs_t = n;
        return;
    }
    if (t > n) {
        struct timespec ts = { (time_t)t, (long)((t - (time_t)t) * 1e9) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
    vs_t = t;
}

static void clear_pages(void)
{
    for (int p = 0; p < pages; p++)
        for (int y = 0; y < YRES; y++)
            memset(page(p) + (size_t)y * STRIDE, 0, XRES * 2);
}

static void ui_show(void)
{
    uint16_t *d = page(back);
    for (int y = 0; y < YRES; y++)
        memcpy(d + (size_t)y * STRIDE, ui + (size_t)y * XRES, XRES * 2);
    present();
}

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BG     RGB(16, 24, 48)
#define C_BAR    RGB(32, 56, 110)
#define C_SEL    RGB(230, 140, 20)
#define C_TEXT   RGB(235, 235, 235)
#define C_DIM    RGB(140, 150, 170)
#define C_WARN   RGB(255, 200, 60)
#define C_OK     RGB(120, 230, 120)

static void fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > XRES) w = XRES - x;
    if (y + h > YRES) h = YRES - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = ui + (size_t)(y + j) * XRES + x;
        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

/* UTF-8 text (Latin-1 only), s = scale factor; returns the final x */
static int text(int x, int y, int s, uint16_t c, const char *str)
{
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        unsigned ch = *p++;
        if (ch >= 0xc0 && (*p & 0xc0) == 0x80) {
            ch = ((ch & 0x1f) << 6) | (*p++ & 0x3f);
            if (ch > 255)
                ch = '?';
        }
        if (x + 8 * s > XRES)
            break;
        const unsigned char *g = font8x16[ch];
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b))
                    for (int yy = 0; yy < s; yy++) {
                        int py = y + r * s + yy;
                        if (py < 0 || py >= YRES)
                            continue;
                        uint16_t *q = ui + (size_t)py * XRES + x + b * s;
                        for (int xx = 0; xx < s; xx++)
                            q[xx] = c;
                    }
        x += 8 * s;
    }
    return x;
}

static int utf8_len(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if ((*s & 0xc0) != 0x80)
            n++;
    return n;
}

static void text_center(int y, int s, uint16_t c, const char *str)
{
    text((XRES - utf8_len(str) * 8 * s) / 2, y, s, c, str);
}

static void screen(const char *title, const char *hint)
{
    fill(0, 0, XRES, YRES, C_BG);
    fill(0, 0, XRES, 64, C_BAR);
    text(32, 16, 2, C_TEXT, title);
    fill(0, YRES - 56, XRES, 56, C_BAR);
    if (hint)
        text(32, YRES - 44, 2, C_DIM, hint);
}

/* ================================================================ boot logo */
static unsigned rd32le(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* stickos/logo.bmp on the SD (uncompressed 24 or 32 bit BMP, ideally 1280x720), centered
 * on a black screen; 1 = shown */
static int logo_bmp(void)
{
    size_t len;
    unsigned char *b = read_file(CFGDIR "/logo.bmp", &len);
    if (!b)
        return 0;
    int ok = 0;
    if (len > 54 && b[0] == 'B' && b[1] == 'M') {
        unsigned off = rd32le(b + 10), bpp = b[28] | b[29] << 8, comp = rd32le(b + 30);
        int w = (int)rd32le(b + 18), h = (int)rd32le(b + 22), topdown = h < 0;
        if (topdown)
            h = -h;
        size_t stride = ((size_t)w * (bpp / 8) + 3) & ~(size_t)3;
        if ((bpp == 24 || bpp == 32) && (comp == 0 || comp == 3) && w > 0 && h > 0 &&
            off + stride * h <= len) {
            fill(0, 0, XRES, YRES, 0);
            int x0 = (XRES - w) / 2, y0 = (YRES - h) / 2;
            for (int y = 0; y < h; y++) {
                int sy = y0 + y;
                if (sy < 0 || sy >= YRES)
                    continue;
                const unsigned char *row = b + off + stride * (topdown ? y : h - 1 - y);
                for (int x = 0; x < w; x++) {
                    int sx = x0 + x;
                    if (sx < 0 || sx >= XRES)
                        continue;
                    const unsigned char *px = row + x * (bpp / 8);
                    ui[(size_t)sy * XRES + sx] = RGB(px[2], px[1], px[0]);
                }
            }
            ok = 1;
        }
    }
    if (!ok)
        logm("logo.bmp: unsupported format (use an uncompressed 24 or 32 bit BMP)");
    free(b);
    return ok;
}

/* built-in logo: name in large letters, with shadow and an orange bar */
static void logo_default(void)
{
    for (int y = 0; y < YRES; y++) {                /* vertical gradient */
        int k = y * 255 / YRES;
        fill(0, y, XRES, 1, RGB(16 + k / 12, 24 + k / 8, 48 + k / 4));
    }
    const char *name = "StickOS";
    int s = 12, w = (int)strlen(name) * 8 * s, x = (XRES - w) / 2, y = 200;
    text(x + 10, y + 10, s, RGB(6, 10, 24), name);
    text(x, y, s, C_TEXT, name);
    fill(x, y + 16 * s + 24, w, 12, C_SEL);
    text_center(y + 16 * s + 64, 2, C_DIM, "v" VERSION);
}

static void splash(void)
{
    if (!logo_bmp())
        logo_default();
    ui_show();
}

/* ================================================================ input */
/* buttons: same numbers as the libretro RetroPad (B=0 ... R3=15); after the buttons,
 * the 4 analog stick axes (bound to a whole axis, not to a direction) */
enum { B_B, B_Y, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_X, B_L, B_R,
       B_L2, B_R2, B_L3, B_R3, NBTN,
       S_LX = NBTN, S_LY, S_RX, S_RY, NIN };
/* names shown in the wizard (English, translated with T()) */
static const char *btn_name[NIN] = { "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
                                     "A", "X", "L", "R", "L2", "R2", "L3", "R3",
                                     "LEFT STICK horiz.", "LEFT STICK vert.", "RIGHT STICK horiz.",
                                     "RIGHT STICK vert." };
static const char *btn_key[NIN] = { "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
                                    "A", "X", "L", "R", "L2", "R2", "L3", "R3",
                                    "LSTICK_X", "LSTICK_Y", "RSTICK_X", "RSTICK_Y" };
/* what to ask in the wizard (default: "Press <name>") */
static const char *btn_prompt[NIN] = { [B_L3] = "Press L3 (push the left stick)",
                                       [B_R3] = "Press R3 (push the right stick)",
                                       [S_LX] = "Move the left stick RIGHT",
                                       [S_LY] = "Move the left stick DOWN",
                                       [S_RX] = "Move the right stick RIGHT",
                                       [S_RY] = "Move the right stick DOWN" };

struct bind {
    uint8_t type;      /* 0 = none, EV_KEY, EV_ABS */
    int8_t dir;        /* for EV_ABS: -1 / +1 */
    uint16_t code;
};

#define NALT 2
struct pad {
    int fd, idx;
    char name[80], id[128];
    struct bind map[NIN][NALT];
    int configured;                     /* map read from joypad.cfg */
    int asked;                          /* configuration already offered */
    unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1];
    int absval[ABS_CNT], absmin[ABS_CNT], absmax[ABS_CNT], absrest[ABS_CNT];
};

#define MAXPAD 8
static struct pad pads[MAXPAD];
static int npads;

/* saved maps: device id -> buttons */
struct savedmap {
    char id[128];
    struct bind map[NIN][NALT];
};
static struct savedmap saved[32];
static int nsaved;

#define TBIT(a, b) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static void cfg_load(void)
{
    FILE *f = fopen(CFGDIR "/joypad.cfg", "r");
    char line[256];
    struct savedmap *cur = NULL;
    nsaved = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            char *e = strrchr(line, ']');
            if (!e || nsaved >= 32)
                continue;
            *e = 0;
            cur = &saved[nsaved++];
            memset(cur, 0, sizeof(*cur));
            snprintf(cur->id, sizeof(cur->id), "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!cur || !eq)
            continue;
        *eq = 0;
        int b;
        for (b = 0; b < NIN && strcmp(line, btn_key[b]); b++)
            ;
        if (b == NIN)
            continue;
        char *tok = strtok(eq + 1, ",");
        for (int a = 0; tok && a < NALT; a++, tok = strtok(NULL, ",")) {
            struct bind *bd = &cur->map[b][a];
            if (tok[0] == 'K') {
                bd->type = EV_KEY;
                bd->code = atoi(tok + 1);
            } else if (tok[0] == 'A') {
                bd->type = EV_ABS;
                bd->code = atoi(tok + 1);
                bd->dir = strchr(tok, '-') ? -1 : 1;
            }
        }
    }
    fclose(f);
    logm("joypad.cfg: %d models", nsaved);
}

static void cfg_save(void)
{
    char buf[16384];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "# Joypad buttons, one section per model. Written by \"Settings > Joypad\".\n"
                  "# K<number> = button, A<axis>+ / A<axis>- = axis or d-pad direction.\n"
                  "# Sticks (xSTICK_X/Y): A<axis>+ = normal, A<axis>- = inverted.\n");
    for (int i = 0; i < nsaved; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "\n[%s]\n", saved[i].id);
        for (int b = 0; b < NIN; b++) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s=", btn_key[b]);
            for (int a = 0; a < NALT; a++) {
                struct bind *bd = &saved[i].map[b][a];
                if (!bd->type)
                    continue;
                if (a)
                    n += snprintf(buf + n, sizeof(buf) - n, ",");
                if (bd->type == EV_KEY)
                    n += snprintf(buf + n, sizeof(buf) - n, "K%u", bd->code);
                else
                    n += snprintf(buf + n, sizeof(buf) - n, "A%u%c", bd->code, bd->dir < 0 ? '-' : '+');
            }
            n += snprintf(buf + n, sizeof(buf) - n, "\n");
        }
    }
    if (write_file(CFGDIR "/joypad.cfg", buf, n))
        logm("joypad.cfg: write failed");
}

static void set_bind(struct pad *p, int b, int a, int type, int code, int dir)
{
    p->map[b][a].type = type;
    p->map[b][a].code = code;
    p->map[b][a].dir = dir;
}

/* default map, based on the buttons the device declares */
static void default_map(struct pad *p, const unsigned long *kb, const unsigned long *ab)
{
    memset(p->map, 0, sizeof(p->map));
    if (TBIT(ab, ABS_HAT0X)) {
        set_bind(p, B_LEFT, 0, EV_ABS, ABS_HAT0X, -1);
        set_bind(p, B_RIGHT, 0, EV_ABS, ABS_HAT0X, 1);
        set_bind(p, B_UP, 0, EV_ABS, ABS_HAT0Y, -1);
        set_bind(p, B_DOWN, 0, EV_ABS, ABS_HAT0Y, 1);
    }
    if (TBIT(ab, ABS_X)) {
        int a = p->map[B_LEFT][0].type ? 1 : 0;
        set_bind(p, B_LEFT, a, EV_ABS, ABS_X, -1);
        set_bind(p, B_RIGHT, a, EV_ABS, ABS_X, 1);
        set_bind(p, B_UP, a, EV_ABS, ABS_Y, -1);
        set_bind(p, B_DOWN, a, EV_ABS, ABS_Y, 1);
    }
    if (TBIT(kb, BTN_SOUTH)) {                          /* standard gamepad */
        set_bind(p, B_A, 0, EV_KEY, BTN_EAST, 0);
        set_bind(p, B_B, 0, EV_KEY, BTN_SOUTH, 0);
        set_bind(p, B_X, 0, EV_KEY, BTN_NORTH, 0);
        set_bind(p, B_Y, 0, EV_KEY, BTN_WEST, 0);
        set_bind(p, B_L, 0, EV_KEY, BTN_TL, 0);
        set_bind(p, B_R, 0, EV_KEY, BTN_TR, 0);
        set_bind(p, B_SELECT, 0, EV_KEY, BTN_SELECT, 0);
        set_bind(p, B_START, 0, EV_KEY, BTN_START, 0);
        set_bind(p, B_L3, 0, EV_KEY, BTN_THUMBL, 0);
        set_bind(p, B_R3, 0, EV_KEY, BTN_THUMBR, 0);
        if (TBIT(kb, BTN_TL2)) {
            set_bind(p, B_L2, 0, EV_KEY, BTN_TL2, 0);
            set_bind(p, B_R2, 0, EV_KEY, BTN_TR2, 0);
        } else if (TBIT(ab, ABS_Z) && TBIT(ab, ABS_RZ) && TBIT(ab, ABS_RX)) {  /* analog triggers */
            set_bind(p, B_L2, 0, EV_ABS, ABS_Z, 1);
            set_bind(p, B_R2, 0, EV_ABS, ABS_RZ, 1);
        }
        if (TBIT(kb, BTN_DPAD_UP)) {
            set_bind(p, B_UP, 0, EV_KEY, BTN_DPAD_UP, 0);
            set_bind(p, B_DOWN, 0, EV_KEY, BTN_DPAD_DOWN, 0);
            set_bind(p, B_LEFT, 0, EV_KEY, BTN_DPAD_LEFT, 0);
            set_bind(p, B_RIGHT, 0, EV_KEY, BTN_DPAD_RIGHT, 0);
        }
    } else if (TBIT(kb, BTN_TRIGGER)) {                 /* generic joystick */
        set_bind(p, B_A, 0, EV_KEY, BTN_THUMB, 0);
        set_bind(p, B_B, 0, EV_KEY, BTN_TRIGGER, 0);
        set_bind(p, B_X, 0, EV_KEY, BTN_THUMB2, 0);
        set_bind(p, B_Y, 0, EV_KEY, BTN_TOP, 0);
        set_bind(p, B_L, 0, EV_KEY, BTN_TOP2, 0);
        set_bind(p, B_R, 0, EV_KEY, BTN_PINKIE, 0);
        set_bind(p, B_SELECT, 0, EV_KEY, BTN_BASE3, 0);
        set_bind(p, B_START, 0, EV_KEY, BTN_BASE4, 0);
        set_bind(p, B_L2, 0, EV_KEY, BTN_BASE, 0);
        set_bind(p, B_R2, 0, EV_KEY, BTN_BASE2, 0);
        set_bind(p, B_L3, 0, EV_KEY, BTN_BASE5, 0);
        set_bind(p, B_R3, 0, EV_KEY, BTN_BASE6, 0);
    }
    /* analog sticks */
    if (TBIT(ab, ABS_X) && TBIT(ab, ABS_Y)) {
        set_bind(p, S_LX, 0, EV_ABS, ABS_X, 1);
        set_bind(p, S_LY, 0, EV_ABS, ABS_Y, 1);
    }
    if (TBIT(ab, ABS_RX) && TBIT(ab, ABS_RY)) {
        set_bind(p, S_RX, 0, EV_ABS, ABS_RX, 1);
        set_bind(p, S_RY, 0, EV_ABS, ABS_RY, 1);
    } else if (TBIT(ab, ABS_Z) && TBIT(ab, ABS_RZ)) {   /* many HID joypads: right stick on Z/RZ */
        set_bind(p, S_RX, 0, EV_ABS, ABS_Z, 1);
        set_bind(p, S_RY, 0, EV_ABS, ABS_RZ, 1);
    }
    if (TBIT(kb, KEY_UP)) {                             /* keyboard, or joypad that reports keys */
        int a = p->map[B_UP][0].type ? 1 : 0;
        set_bind(p, B_UP, a, EV_KEY, KEY_UP, 0);
        set_bind(p, B_DOWN, a, EV_KEY, KEY_DOWN, 0);
        set_bind(p, B_LEFT, a, EV_KEY, KEY_LEFT, 0);
        set_bind(p, B_RIGHT, a, EV_KEY, KEY_RIGHT, 0);
        if (!p->map[B_A][0].type) {
            set_bind(p, B_A, 0, EV_KEY, KEY_A, 0);
            set_bind(p, B_B, 0, EV_KEY, KEY_B, 0);
            set_bind(p, B_X, 0, EV_KEY, KEY_X, 0);
            set_bind(p, B_Y, 0, EV_KEY, KEY_Y, 0);
            set_bind(p, B_L, 0, EV_KEY, KEY_L, 0);
            set_bind(p, B_R, 0, EV_KEY, KEY_R, 0);
            set_bind(p, B_START, 0, EV_KEY, KEY_ENTER, 0);
            set_bind(p, B_SELECT, 0, EV_KEY, KEY_RIGHTSHIFT, 0);
            set_bind(p, B_A, 1, EV_KEY, KEY_ENTER, 0);
            set_bind(p, B_B, 1, EV_KEY, KEY_ESC, 0);
        }
    }
}

static void pad_apply_saved(struct pad *p)
{
    p->configured = 0;
    for (int i = 0; i < nsaved; i++)
        if (!strcmp(saved[i].id, p->id)) {
            memcpy(p->map, saved[i].map, sizeof(p->map));
            p->configured = 1;
        }
}

static int pad_cmp(const void *a, const void *b)
{
    return ((const struct pad *)a)->idx - ((const struct pad *)b)->idx;
}

static void input_scan(void)
{
    for (int i = 0; i < 32 && npads < MAXPAD; i++) {
        int j, open_ = 0;
        for (j = 0; j < npads; j++)
            if (pads[j].idx == i)
                open_ = 1;
        if (open_)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long kb[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 }, ab[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(kb)), kb);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(ab)), ab);
        struct pad *p = &pads[npads];
        memset(p, 0, sizeof(*p));
        p->fd = fd;
        p->idx = i;
        ioctl(fd, EVIOCGNAME(sizeof(p->name) - 1), p->name);
        struct input_id iid = { 0 };
        ioctl(fd, EVIOCGID, &iid);
        snprintf(p->id, sizeof(p->id), "%04x:%04x %s", iid.vendor, iid.product, p->name);
        for (int a = 0; a < ABS_CNT; a++)
            if (TBIT(ab, a)) {
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(a), &ai) == 0) {
                    p->absmin[a] = ai.minimum;
                    p->absmax[a] = ai.maximum;
                    p->absval[a] = p->absrest[a] = ai.value;
                }
            }
        ioctl(fd, EVIOCGKEY(sizeof(p->keys)), p->keys);
        default_map(p, kb, ab);
        pad_apply_saved(p);
        logm("joypad event%d: \"%s\" %s", i, p->id, p->configured ? "configured" : "default buttons");
        npads++;
    }
    qsort(pads, npads, sizeof(pads[0]), pad_cmp);
}

static void pad_close(int j)
{
    logm("joypad event%d disconnected", pads[j].idx);
    close(pads[j].fd);
    memmove(&pads[j], &pads[j + 1], (npads - j - 1) * sizeof(pads[0]));
    npads--;
}

static void input_close_all(void)
{
    while (npads)
        pad_close(npads - 1);
}

static int abs_side(struct pad *p, int code, int v)
{
    int mn = p->absmin[code], mx = p->absmax[code];
    if (mx <= mn)
        return 0;
    int mid = (mn + mx) / 2, q = (mx - mn) / 4;
    if (q < 1)
        q = 1;
    if (v < mid - q || (mx - mn <= 2 && v < mid))
        return -1;
    if (v > mid + q || (mx - mn <= 2 && v > mid))
        return 1;
    return 0;
}

static int bind_on(struct pad *p, const struct bind *b)
{
    if (b->type == EV_KEY)
        return b->code < KEY_CNT && TBIT(p->keys, b->code);
    if (b->type == EV_ABS)
        return b->code < ABS_CNT && abs_side(p, b->code, p->absval[b->code]) == b->dir;
    return 0;
}

/* stick position: -32767..32767, with a 12% dead zone */
static int stick_val(struct pad *p, const struct bind *b)
{
    if (b->type != EV_ABS || b->code >= ABS_CNT)
        return 0;
    int mn = p->absmin[b->code], mx = p->absmax[b->code];
    if (mx <= mn)
        return 0;
    double v = (p->absval[b->code] - (mn + mx) / 2.0) / ((mx - mn) / 2.0);
    if (b->dir < 0)
        v = -v;
    const double dz = 0.12;
    if (v > -dz && v < dz)
        return 0;
    v = v > 0 ? (v - dz) / (1 - dz) : (v + dz) / (1 - dz);
    if (v > 1)
        v = 1;
    if (v < -1)
        v = -1;
    return (int)(v * 32767);
}

static unsigned pad_state(struct pad *p)
{
    unsigned m = 0;
    for (int b = 0; b < NBTN; b++)
        for (int a = 0; a < NALT; a++)
            if (bind_on(p, &p->map[b][a]))
                m |= 1u << b;
    return m;
}

/* raw event, for the wizard */
struct rawev {
    int pad, type, code, value;
};
static struct rawev rawq[64];
static int nraw;

/* reads all queued events; updates the joypad state */
static void input_poll(void)
{
    static double last_scan;
    for (int j = 0; j < npads; j++) {
        struct input_event ev[32];
        int n;
        while ((n = read(pads[j].fd, ev, sizeof(ev))) > 0) {
            for (int k = 0; k < n / (int)sizeof(ev[0]); k++) {
                struct pad *p = &pads[j];
                if (ev[k].type == EV_KEY && ev[k].code < KEY_CNT) {
                    unsigned long bit = 1ul << (ev[k].code % (8 * sizeof(long)));
                    if (ev[k].value)
                        p->keys[ev[k].code / (8 * sizeof(long))] |= bit;
                    else
                        p->keys[ev[k].code / (8 * sizeof(long))] &= ~bit;
                } else if (ev[k].type == EV_ABS && ev[k].code < ABS_CNT) {
                    p->absval[ev[k].code] = ev[k].value;
                } else {
                    continue;
                }
                if (nraw < 64)
                    rawq[nraw++] = (struct rawev){ j, ev[k].type, ev[k].code, ev[k].value };
            }
        }
        if (n < 0 && errno == ENODEV) {
            pad_close(j);
            j--;
        }
    }
    if (now() - last_scan > 2) {
        last_scan = now();
        input_scan();
    }
}

static void input_wait(int ms)
{
    wd_count++;
    struct pollfd pf[MAXPAD];
    for (int j = 0; j < npads; j++) {
        pf[j].fd = pads[j].fd;
        pf[j].events = POLLIN;
    }
    poll(pf, npads, ms);
}

/* menu buttons: all joypads together, with auto-repeat */
static unsigned menu_held, menu_prev;
static double rep_t;

static unsigned menu_buttons(void)
{
    input_poll();
    unsigned m = 0;
    for (int j = 0; j < npads; j++)
        m |= pad_state(&pads[j]);
    nraw = 0;
    unsigned pressed = m & ~menu_prev;
    unsigned dirs = (1u << B_UP) | (1u << B_DOWN) | (1u << B_LEFT) | (1u << B_RIGHT);
    if (pressed & dirs)
        rep_t = now() + 0.4;
    else if ((m & dirs) && now() > rep_t) {
        pressed |= m & dirs;
        rep_t = now() + 0.07;
    }
    menu_prev = m;
    menu_held = m;
    return pressed;
}

static void menu_release_all(void)
{
    double t0 = now();
    do {
        input_wait(20);
        menu_buttons();
    } while (menu_held && now() - t0 < 3);
}

/* ================================================================ joypad wizard */
static const int wiz_order[NIN] = { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_X, B_Y, B_L, B_R,
                                    B_L2, B_R2, B_L3, B_R3, B_SELECT, B_START, S_LX, S_LY, S_RX, S_RY };

static int bind_eq(const struct bind *a, const struct bind *b)
{
    return a->type == b->type && a->code == b->code && (a->type != EV_ABS || a->dir == b->dir);
}

/* waits for a button/axis on joypad 'target' (any if -1); 1 = got one, 0 = timeout */
static int capture_abs_only;                /* 1 = accept axis movements only (sticks) */

static int capture(int *target, struct bind *out, double timeout)
{
    double t0 = now();
    while (now() - t0 < timeout) {
        input_wait(50);
        nraw = 0;
        input_poll();
        for (int i = 0; i < nraw; i++) {
            struct rawev *r = &rawq[i];
            if (*target >= 0 && r->pad != *target)
                continue;
            struct pad *p = &pads[r->pad];
            if (r->type == EV_KEY && r->value == 1 && !capture_abs_only) {
                *out = (struct bind){ EV_KEY, 0, (uint16_t)r->code };
            } else if (r->type == EV_ABS) {
                int s = abs_side(p, r->code, r->value);
                if (!s || s == abs_side(p, r->code, p->absrest[r->code]))
                    continue;
                *out = (struct bind){ EV_ABS, (int8_t)s, (uint16_t)r->code };
            } else {
                continue;
            }
            *target = r->pad;
            nraw = 0;
            /* wait for release */
            double t1 = now();
            while (now() - t1 < 2 && *target < npads && bind_on(&pads[*target], out)) {
                input_wait(20);
                input_poll();
            }
            nraw = 0;
            return 1;
        }
        nraw = 0;
    }
    return 0;
}

static void wizard_draw(const char *padname, int step, const char *msg, int secs, struct bind map[NIN][NALT])
{
    screen(T("Configure joypad"), T("If a button is missing, wait: it will be skipped"));
    text(32, 76, 2, C_DIM, padname);
    for (int i = 0; i < NIN; i++) {
        int b = wiz_order[i], x = i < 10 ? 60 : 660, y = 116 + (i % 10) * 42;
        uint16_t c = i == step ? C_SEL : i < step ? C_TEXT : C_DIM;
        text(x, y, 2, c, T(btn_name[b]));
        if (i < step)
            text(x + 340, y, 2, map[b][0].type ? C_OK : C_DIM, map[b][0].type ? "ok" : T("(skipped)"));
    }
    if (msg) {
        int sc = utf8_len(msg) * 24 < XRES - 40 ? 3 : 2;
        text_center(548, sc, C_SEL, msg);
        if (secs > 0) {
            char s[64];
            snprintf(s, sizeof(s), "(%d s)", secs);
            text_center(608, 2, C_DIM, s);
        }
    }
    ui_show();
}

/* do two bindings conflict? (for sticks the axis counts, not the direction) */
static int bind_conflict(int ba, const struct bind *a, int bb, const struct bind *b)
{
    if (!a->type || !b->type)
        return 0;
    if (ba >= NBTN || bb >= NBTN)
        return a->type == EV_ABS && b->type == EV_ABS && a->code == b->code;
    return bind_eq(a, b);
}

/* target = joypad index, -1 = the first one that presses a button */
static void wizard(int target)
{
    struct bind map[NIN][NALT];
    memset(map, 0, sizeof(map));
    nraw = 0;
    menu_release_all();
    for (int j = 0; j < npads; j++)                 /* axes rest position */
        memcpy(pads[j].absrest, pads[j].absval, sizeof(pads[j].absval));
    if (target < 0) {
        struct bind dummy;
        wizard_draw("", -1, T("Press a button on the joypad"), 0, map);
        if (!capture(&target, &dummy, 30))
            return;
    }
    char name[100];
    snprintf(name, sizeof(name), "%s", pads[target].name);
    for (int i = 0; i < NIN; i++) {
        int b = wiz_order[i];
        int must = b == B_UP || b == B_DOWN || b == B_A || b == B_B;
        double limit = must ? 1e9 : 6;
        double t0 = now();
        char msg[100];
        if (btn_prompt[b])
            snprintf(msg, sizeof(msg), "%s", T(btn_prompt[b]));
        else
            snprintf(msg, sizeof(msg), T("Press %s"), T(btn_name[b]));
        capture_abs_only = b >= NBTN;
        for (;;) {
            int left = must ? 0 : (int)(limit - (now() - t0) + 0.99);
            if (!must && left <= 0)
                break;
            wizard_draw(name, i, msg, left, map);
            struct bind got;
            int tg = target;
            if (!capture(&tg, &got, 1))
                continue;
            if (target >= npads)
                return;                                 /* joypad disconnected */
            int dup = 0;
            for (int k = 0; k < i; k++)
                if (bind_conflict(wiz_order[k], &map[wiz_order[k]][0], b, &got))
                    dup = 1;
            if (dup) {
                snprintf(msg, sizeof(msg), T("Already used! %s"), T(btn_prompt[b] ? btn_prompt[b] : btn_name[b]));
                continue;
            }
            map[b][0] = got;
            break;
        }
    }
    capture_abs_only = 0;
    /* the left stick also drives the d-pad (menus and games without analog support) */
    if (map[S_LX][0].type && map[S_LY][0].type) {
        const struct { int b, axis, dir; } d[4] = { { B_LEFT, S_LX, -1 }, { B_RIGHT, S_LX, 1 },
                                                     { B_UP, S_LY, -1 }, { B_DOWN, S_LY, 1 } };
        for (int i = 0; i < 4; i++) {
            struct bind a = map[d[i].axis][0];
            a.dir = a.dir * d[i].dir;
            if (!map[d[i].b][1].type && !bind_eq(&map[d[i].b][0], &a))
                map[d[i].b][1] = a;
        }
    }
    /* save for this model */
    struct pad *p = &pads[target];
    int k;
    for (k = 0; k < nsaved && strcmp(saved[k].id, p->id); k++)
        ;
    if (k == nsaved && nsaved < 32)
        nsaved++;
    if (k < 32) {
        snprintf(saved[k].id, sizeof(saved[k].id), "%s", p->id);
        memcpy(saved[k].map, map, sizeof(map));
    }
    cfg_save();
    for (int j = 0; j < npads; j++) {
        pad_apply_saved(&pads[j]);
        pads[j].asked = 1;
    }
    logm("joypad configured: %s", p->id);
    wizard_draw(name, NIN, T("Saved!"), 0, map);
    sleep(1);
    menu_release_all();
}

/* new joypad (no saved configuration): offer the wizard */
static void offer_config(void)
{
    for (int j = 0; j < npads; j++) {
        if (pads[j].configured || pads[j].asked)
            continue;
        for (int k = 0; k < npads; k++)
            if (!strcmp(pads[k].id, pads[j].id))
                pads[k].asked = 1;
        screen(T("New joypad"), NULL);
        text_center(220, 2, C_TEXT, pads[j].name);
        text_center(300, 2, C_TEXT, T("Press any button on this joypad"));
        text_center(340, 2, C_TEXT, T("to configure its buttons."));
        text_center(420, 2, C_DIM, T("If you press nothing for 10 seconds"));
        text_center(456, 2, C_DIM, T("the default buttons are used."));
        ui_show();
        int tg = j;
        struct bind dummy;
        nraw = 0;
        if (capture(&tg, &dummy, 10))
            wizard(j < npads ? j : -1);
        menu_release_all();
        return;
    }
}

/* ================================================================ systems and games */
struct sys {
    const char *dir, *name, *core, *exts;
    int arcade; /* 1 = zips are romsets, passed to the core as they are */
    const char *extra;  /* second ROM folder shown in the same list (or NULL) */
};
static const struct sys systems[] = {
    { "nes", "Nintendo NES", "nestopia_libretro.so", "nes fds unf unif" },
    { "snes", "Super Nintendo", "snes9x2010_libretro.so", "sfc smc fig swc" },
    { "megadrive", "Sega Mega Drive", "picodrive_libretro.so", "md gen smd bin" },
    { "mastersystem", "Sega Master System", "genesis_plus_gx_libretro.so", "sms" },
    { "gamegear", "Sega Game Gear", "genesis_plus_gx_libretro.so", "gg" },
    { "gb", "Game Boy", "gambatte_libretro.so", "gb" },
    { "gbc", "Game Boy Color", "gambatte_libretro.so", "gbc gb" },
    { "gba", "Game Boy Advance", "gpsp_libretro.so", "gba" },
    { "pcengine", "PC Engine", "mednafen_pce_fast_libretro.so", "pce cue" },
    { "psx", "PlayStation", "pcsx_rearmed_libretro.so", "cue chd pbp m3u iso img" },
    { "atari2600", "Atari 2600", "stella2014_libretro.so", "a26 bin" },
    { "atari5200", "Atari 5200", "atari800_libretro.so", "a52 bin" },
    { "atari7800", "Atari 7800", "prosystem_libretro.so", "a78 bin" },
    { "arcade", "Arcade (FB Alpha 2012)", "fbalpha2012_libretro.so", "zip", 1 },
    { "fbalpha", "Arcade (FB Alpha)", "fbalpha_libretro.so", "zip", 1, "neogeo" },
    { "mame2003", "Arcade (MAME 2003)", "mame2003_libretro.so", "zip", 1 },
};
#define NSYS (int)(sizeof(systems) / sizeof(systems[0]))

static int ext_ok(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1])
        return 0;
    size_t n = strlen(dot + 1);
    const char *e = exts;
    while (*e) {
        size_t l = strcspn(e, " ");
        if (l == n && !strncasecmp(e, dot + 1, n))
            return 1;
        e += l;
        while (*e == ' ')
            e++;
    }
    return 0;
}

/* ================================================================ zip archives */
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (unsigned)rd16(p + 2) << 16; }

/* If buf is a zip, extracts the first file with one of the system's extensions (or the
 * largest one if none matches). Returns the data (malloc) and the inner file name. */
static void *unzip_rom(const unsigned char *buf, size_t len, const char *exts, size_t *outlen, char *name,
                       size_t namelen)
{
    if (len < 22 || rd32(buf) != 0x04034b50)
        return NULL;
    size_t i = len - 22, stop = len > 22 + 65535 ? len - 22 - 65535 : 0;
    for (;; i--) {
        if (rd32(buf + i) == 0x06054b50)
            break;
        if (i == stop) {
            logm("zip: central directory not found");
            return NULL;
        }
    }
    unsigned cnt = rd16(buf + i + 10);
    size_t cd = rd32(buf + i + 16), best = 0, bestsize = 0;
    int bestmatch = -1;
    for (unsigned k = 0; k < cnt; k++) {
        if (cd + 46 > len || rd32(buf + cd) != 0x02014b50)
            break;
        unsigned nl = rd16(buf + cd + 28), xl = rd16(buf + cd + 30), cl = rd16(buf + cd + 32);
        size_t usize = rd32(buf + cd + 24);
        if (cd + 46 + nl > len)
            break;
        char n[256];
        snprintf(n, sizeof(n), "%.*s", (int)nl, buf + cd + 46);
        int match = ext_ok(n, exts);
        if (nl && n[nl - 1] != '/' && (match > bestmatch || (match == bestmatch && usize > bestsize))) {
            best = cd;
            bestsize = usize;
            bestmatch = match;
        }
        cd += 46 + nl + xl + cl;
    }
    if (bestmatch < 0) {
        logm("zip: no files in the archive");
        return NULL;
    }
    unsigned method = rd16(buf + best + 10), nl = rd16(buf + best + 28);
    size_t csize = rd32(buf + best + 20), usize = rd32(buf + best + 24), lh = rd32(buf + best + 42);
    snprintf(name, namelen, "%.*s", (int)nl, buf + best + 46);
    char *sl = strrchr(name, '/');
    if (sl)
        memmove(name, sl + 1, strlen(sl));
    if (lh + 30 > len || rd32(buf + lh) != 0x04034b50)
        return NULL;
    size_t data = lh + 30 + rd16(buf + lh + 26) + rd16(buf + lh + 28);
    if (data + csize > len || usize > 512u << 20)
        return NULL;
    unsigned char *out = malloc(usize ? usize : 1);
    if (!out)
        return NULL;
    if (method == 0 && csize == usize) {
        memcpy(out, buf + data, usize);
    } else if (method == 8) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        z.next_in = (unsigned char *)buf + data;
        z.avail_in = csize;
        z.next_out = out;
        z.avail_out = usize;
        int r = inflateInit2(&z, -MAX_WBITS);
        if (r == Z_OK)
            r = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (r != Z_STREAM_END || z.total_out != usize) {
            logm("zip: decompression error (%d)", r);
            free(out);
            return NULL;
        }
    } else {
        logm("zip: unsupported compression method %u", method);
        free(out);
        return NULL;
    }
    if (crc32(0, out, usize) != rd32(buf + best + 16)) {
        logm("zip: bad CRC, damaged archive");
        free(out);
        return NULL;
    }
    *outlen = usize;
    return out;
}

static int core_exists(const struct sys *s)
{
    char p[256];
    snprintf(p, sizeof(p), COREDIR "/%s", s->core);
    return access(p, R_OK) == 0;
}

/* ---- full titles of arcade romsets: /usr/share/stickos/names/<core>.txt, made at build
 * time from the core sources (tools/arcadenames.py). Lines "zip TAB flag TAB title",
 * sorted by zip name; flag "b" = BIOS, not a game (hidden from the list). */
#ifndef NAMEDIR
#define NAMEDIR "/usr/share/stickos/names"
#endif
struct aname {
    const char *zip, *title;
    char bios;
};
static struct names {
    char core[64];
    char *buf;
    struct aname *v;
    int n;
} ntab[4];

static void core_base(char *dst, size_t n, const char *corefile)
{
    const char *suf = strstr(corefile, "_libretro");
    snprintf(dst, n, "%.*s", suf ? (int)(suf - corefile) : (int)strcspn(corefile, "."), corefile);
}

/* table of a core (loaded once, then kept in memory); NULL if there is none */
static struct names *names_for(const char *corefile)
{
    char base[64];
    core_base(base, sizeof(base), corefile);
    int i;
    for (i = 0; i < 4 && ntab[i].core[0]; i++)
        if (!strcmp(ntab[i].core, base))
            return ntab[i].v ? &ntab[i] : NULL;
    if (i == 4)
        return NULL;
    struct names *t = &ntab[i];
    snprintf(t->core, sizeof(t->core), "%s", base);
    char path[160];
    size_t len;
    snprintf(path, sizeof(path), NAMEDIR "/%s.txt", base);
    t->buf = read_file(path, &len);
    if (!t->buf)
        return NULL;
    t->buf = realloc(t->buf, len + 1);
    t->buf[len] = 0;
    int cap = 0;
    for (size_t k = 0; k < len; k++)
        cap += t->buf[k] == '\n';
    t->v = malloc((cap + 1) * sizeof(*t->v));
    for (char *l = t->buf; *l;) {
        char *nl = strchr(l, '\n'), *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (nl)
            *nl = 0;
        if (t2 && (!nl || t2 < nl)) {
            *t1 = *t2 = 0;
            t->v[t->n++] = (struct aname){ l, t2 + 1, t1[1] == 'b' };
        }
        if (!nl)
            break;
        l = nl + 1;
    }
    logm("%s: %d arcade titles", path, t->n);
    return t;
}

static const struct aname *name_find(const struct names *t, const char *file)
{
    char key[64];
    snprintf(key, sizeof(key), "%.*s", (int)strcspn(file, "."), file);
    for (char *c = key; *c; c++)
        *c = tolower((unsigned char)*c);
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(key, t->v[mid].zip);
        if (!c)
            return &t->v[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

static void display_name(char *dst, size_t len, const char *file)
{
    snprintf(dst, len, "%s", file);
    char *dot = strrchr(dst, '.');
    if (dot)
        *dot = 0;
}

struct game {
    char *file, *name;
};

static int cmp_game(const void *a, const void *b)
{
    const struct game *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->file, y->file);
}

/* games of a system, sorted by the name shown: file names (relative to roms/<dir>)
 * in *files, names to show in *names (if names != NULL). Arcade: full titles, BIOS
 * zips left out. Games of the extra folder are listed as "../<extra>/<file>". */
static int list_games(const struct sys *s, char ***files, char ***names)
{
    int n = 0, cap = 0;
    struct game *v = NULL;
    struct dirent *e;
    *files = NULL;
    if (names)
        *names = NULL;
    struct names *t = s->arcade ? names_for(s->core) : NULL;
    for (int k = 0; k < 2; k++) {
    const char *dir = k ? s->extra : s->dir;
    if (!dir)
        continue;
    char p[256];
    snprintf(p, sizeof(p), ROMS "/%s", dir);
    DIR *d = opendir(p);
    if (!d) {
        mkdirs(p);
        continue;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !(ext_ok(e->d_name, s->exts) || ext_ok(e->d_name, "zip")))
            continue;
        const struct aname *a = t ? name_find(t, e->d_name) : NULL;
        if (a && a->bios)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            v = realloc(v, cap * sizeof(*v));
        }
        char dn[256];
        if (a)
            snprintf(dn, sizeof(dn), "%s", a->title);
        else
            display_name(dn, sizeof(dn), e->d_name);
        char fn[512];
        snprintf(fn, sizeof(fn), k ? "../%s/%s" : "%.0s%s", dir, e->d_name);
        v[n].file = strdup(fn);
        v[n].name = strdup(dn);
        n++;
    }
    closedir(d);
    }
    if (!n)
        return 0;
    qsort(v, n, sizeof(*v), cmp_game);
    *files = malloc(n * sizeof(char *));
    if (names)
        *names = malloc(n * sizeof(char *));
    for (int i = 0; i < n; i++) {
        (*files)[i] = v[i].file;
        if (names)
            (*names)[i] = v[i].name;
        else
            free(v[i].name);
    }
    free(v);
    return n;
}

static void free_list(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

/* generic scrolling list; returns the chosen index, -1 = back */
static int list_menu(const char *title, char **items, int n, int *sel, const char *empty_msg)
{
    const int rows = (YRES - 64 - 56 - 24) / 40;
    int top = 0, dirty = 1;
    menu_release_all();
    for (;;) {
        if (n > 0) {
            if (*sel < 0)
                *sel = 0;
            if (*sel >= n)
                *sel = n - 1;
            if (*sel < top)
                top = *sel;
            if (*sel >= top + rows)
                top = *sel - rows + 1;
        }
        if (dirty) {
            screen(title, T("A: select   B: back   </>: page"));
            if (n == 0 && empty_msg) {
                int y = 160;
                for (const char *l = empty_msg; *l; y += 40) {
                    char line[160];
                    size_t k = strcspn(l, "\n");
                    snprintf(line, sizeof(line), "%.*s", (int)k, l);
                    text_center(y, 2, C_TEXT, line);
                    l += k + (l[k] == '\n');
                }
            }
            for (int i = 0; i < rows && top + i < n; i++) {
                int y = 76 + i * 40;
                if (top + i == *sel)
                    fill(16, y - 2, XRES - 32, 38, C_SEL);
                text(40, y + 2, 2, top + i == *sel ? RGB(0, 0, 0) : C_TEXT, items[top + i]);
            }
            if (n > rows) {
                char pos[32];
                snprintf(pos, sizeof(pos), "%d/%d", *sel + 1, n);
                text(XRES - 32 - (int)strlen(pos) * 16, 16, 2, C_DIM, pos);
            }
            ui_show();
            dirty = 0;
        }
        input_wait(30);
        unsigned k = menu_buttons();
        int old = *sel;
        if (k & (1u << B_UP))
            *sel = *sel > 0 ? *sel - 1 : n - 1;
        if (k & (1u << B_DOWN))
            *sel = *sel < n - 1 ? *sel + 1 : 0;
        if (k & (1u << B_LEFT))
            *sel = *sel - rows < 0 ? 0 : *sel - rows;
        if (k & (1u << B_RIGHT))
            *sel = *sel + rows >= n ? n - 1 : *sel + rows;
        if (*sel != old)
            dirty = 1;
        if ((k & (1u << B_A)) && n > 0)
            return *sel;
        if (k & (1u << B_B))
            return -1;
        static int lastn = -1;
        if (npads != lastn) {                   /* joypad connected/disconnected */
            lastn = npads;
            offer_config();
            dirty = 1;
        }
    }
}

static int run_game(const struct sys *s, const char *file, const char *title)
{
    char core[256], rom[512];
    snprintf(core, sizeof(core), COREDIR "/%s", s->core);
    snprintf(rom, sizeof(rom), ROMS "/%s/%s", s->dir, file);
    screen(T("Starting..."), NULL);
    text_center(330, 2, C_TEXT, title);
    ui_show();
    if (strcmp(set_get("last_system"), s->dir) || strcmp(set_get("last_game"), file)) {
        set_put("last_system", s->dir);         /* the menus start from here next time */
        set_put("last_game", file);
        settings_save();
    }
    logm("starting %s with %s", rom, core);
    input_close_all();
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "stickui", "--play", core, rom, s->dir, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    logm("game ended: status %d", st);
    var.yoffset = 0;
    back = pages - 1;
    input_scan();
    menu_release_all();
    return st;
}

static void games_menu(const struct sys *s)
{
    char **files, **names;
    int n = list_games(s, &files, &names), sel = 0;
    if (!strcmp(set_get("last_system"), s->dir))      /* cursor on the last game played */
        for (int i = 0; i < n; i++)
            if (!strcmp(files[i], set_get("last_game")))
                sel = i;
    for (;;) {
        int k = list_menu(s->name, names, n, &sel, NULL);
        if (k < 0)
            break;
        int st = run_game(s, files[k], names[k]);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            char det[160];
            snprintf(det, sizeof(det), T("Details in %s on the SD card."), "stickos/logs/stickui.log");
            screen(T("Error"), T("B: back"));
            text_center(300, 2, C_WARN, T("The game did not start or closed with an error."));
            text_center(350, 2, C_DIM, det);
            ui_show();
            menu_release_all();
            for (;;) {
                input_wait(50);
                if (menu_buttons() & ((1u << B_B) | (1u << B_A)))
                    break;
            }
        }
    }
    free_list(names, n);
    free_list(files, n);
}

static void info_screen(void)
{
    screen(T("Information"), T("B: back"));
    char l[200];
    snprintf(l, sizeof(l), T("StickOS %s - menu and libretro emulators"), VERSION);
    text(40, 100, 2, C_TEXT, l);
    text(40, 160, 2, C_DIM, T("ROMs: folder roms/<system> on the SD card"));
    text(40, 200, 2, C_DIM, T("In game: SELECT + START = pause menu"));
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(l, sizeof(l), "%s Linux %s %s", T("Kernel:"), u.release, u.version);
        l[74] = 0;                          /* fits on one line */
        text(40, 240, 2, C_DIM, l);
    }
    int y = 300;
    text(40, y, 2, C_TEXT, T("Connected joypads:"));
    for (int j = 0; j < npads; j++) {
        y += 40;
        snprintf(l, sizeof(l), "%d) %s%s%s", j + 1, pads[j].name, pads[j].configured ? "" : "  ",
                 pads[j].configured ? "" : T("(default buttons)"));
        text(60, y, 2, C_TEXT, l);
    }
    ui_show();
    menu_release_all();
    for (;;) {
        input_wait(50);
        if (menu_buttons() & ((1u << B_B) | (1u << B_A)))
            break;
    }
}

static void settings_menu(void);

static int main_menu(void)
{
    int sel = 0, first = 1;
    for (;;) {
        char *items[NSYS + 2];
        int map[NSYS + 2], n = 0;
        for (int i = 0; i < NSYS; i++) {
            if (!core_exists(&systems[i]))
                continue;
            char **g;
            int c = list_games(&systems[i], &g, NULL);
            free_list(g, c);
            if (!c)
                continue;
            char buf[128];
            snprintf(buf, sizeof(buf), "%s  (%d)", systems[i].name, c);
            if (first && !strcmp(systems[i].dir, set_get("last_system")))
                sel = n;                            /* at startup: cursor on the last system played */
            items[n] = strdup(buf);
            map[n++] = i;
        }
        first = 0;
        if (!n)
            for (int i = 0; i < NSYS; i++)
                if (core_exists(&systems[i]) && n < NSYS) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), T("(no games: copy ROMs into roms/%s)"), systems[i].dir);
                    items[n] = strdup(buf);
                    map[n++] = -3;
                }
        items[n] = strdup(T("Settings"));
        map[n++] = -1;
        items[n] = strdup(T("Information"));
        map[n++] = -2;
        int k = list_menu("StickOS", items, n, &sel, NULL);
        for (int i = 0; i < n; i++)
            free(items[i]);
        if (k < 0)
            continue;
        if (map[k] == -1)
            settings_menu();
        else if (map[k] == -2)
            info_screen();
        else if (map[k] == -3)
            continue;
        else
            games_menu(&systems[map[k]]);
    }
}

/* ================================================================ audio */
static int pcm = -1;
static unsigned out_rate;
static double in_rate, rs_pos;
static int16_t rs_prev[2];
static int16_t *abuf;
static int anframes, acap;
#define A_PERIOD 512
#define A_PERIODS 8
#define A_TARGET (A_PERIOD * A_PERIODS / 2)

static void hw_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void hw_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->integer = 1;
}

static int audio_open(unsigned rate)
{
    pcm = open("/dev/snd/pcmC0D0p", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (pcm < 0) {
        logm("audio: %s", strerror(errno));
        return -1;
    }
    struct snd_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(&hw.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
        hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
    hw.rmask = ~0u;
    hw.info = ~0u;
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, A_PERIOD);
    hw_int(&hw, SNDRV_PCM_HW_PARAM_PERIODS, A_PERIODS);
    if (ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &hw)) {
        logm("audio hw_params %u: %s", rate, strerror(errno));
        close(pcm);
        pcm = -1;
        return -1;
    }
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.period_step = 1;
    sw.avail_min = A_PERIOD;
    sw.start_threshold = A_TARGET;
    sw.stop_threshold = A_PERIOD * A_PERIODS;
    sw.xfer_align = 1;
    sw.boundary = A_PERIOD * A_PERIODS;
    while (sw.boundary * 2 <= 0x7fffffffu - A_PERIOD * A_PERIODS)
        sw.boundary *= 2;
    if (ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sw))
        logm("audio sw_params: %s", strerror(errno));
    ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
    out_rate = rate;
    logm("audio opened at %u Hz", rate);
    return 0;
}

static void audio_push(const int16_t *data, int frames)
{
    if (anframes + frames > acap) {
        acap = (anframes + frames) * 2;
        abuf = realloc(abuf, acap * 4);
    }
    memcpy(abuf + anframes * 2, data, frames * 4);
    anframes += frames;
}

static unsigned underruns;
static retro_audio_buffer_status_callback_t audio_status_cb;
static uintptr_t vfp_call(void *fn, uintptr_t a, uintptr_t b, uintptr_t c);

/* tells the core how full the audio buffer is, before each frame: when it is
 * about to run dry the core can skip drawing a frame (frameskip "auto") */
static void audio_status(void)
{
    if (!audio_status_cb)
        return;
    snd_pcm_sframes_t delay = 0;
    if (pcm < 0 || ioctl(pcm, SNDRV_PCM_IOCTL_DELAY, &delay))
        delay = A_TARGET;
    if (delay < 0)
        delay = 0;
    unsigned occ = (unsigned)(delay * 100 / (A_PERIOD * A_PERIODS));
    vfp_call((void *)audio_status_cb, 1, occ > 100 ? 100 : occ, delay < A_TARGET / 2);
}

/* resamples the frame's samples and writes them, never blocking */
static void audio_flush(void)
{
    if (pcm < 0 || !anframes) {
        anframes = 0;
        return;
    }
    snd_pcm_sframes_t delay = 0;
    if (ioctl(pcm, SNDRV_PCM_IOCTL_DELAY, &delay))
        delay = 0;
    double err = (double)(A_TARGET - delay) / A_TARGET;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    double ratio = out_rate / in_rate * (1.0 + 0.005 * err);
    double step = 1.0 / ratio;
    static int16_t *obuf;
    static int ocap;
    int maxout = (int)(anframes * ratio) + 4;
    if (maxout > ocap) {
        ocap = maxout * 2;
        obuf = realloc(obuf, ocap * 4);
    }
    int no = 0;
    double t = rs_pos;
    while (t < anframes && no < maxout) {
        int i = (int)t;
        double f = t - i;
        const int16_t *a = i == 0 ? rs_prev : abuf + (i - 1) * 2;
        const int16_t *b = abuf + i * 2;
        obuf[no * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f);
        obuf[no * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        no++;
        t += step;
    }
    rs_pos = t - anframes;
    rs_prev[0] = abuf[(anframes - 1) * 2];
    rs_prev[1] = abuf[(anframes - 1) * 2 + 1];
    anframes = 0;
    int done = 0;
    while (done < no) {
        struct snd_xferi x = { .buf = obuf + done * 2, .frames = no - done };
        if (ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
            if (x.result <= 0)
                break;
            done += x.result;
        } else if (errno == EPIPE) {
            underruns++;
            ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
        } else {
            break;                                      /* EAGAIN: buffer full */
        }
    }
}

/* ================================================================ libretro */
static struct {
    void *h;
    void (*init)(void);
    void (*deinit)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} core;

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char sysdir[256], savedir[256];
static unsigned pad_bits[MAXPAD];
static int16_t pad_axes[MAXPAD][4];         /* LX, LY, RX, RY */

/* core options (SET_VARIABLES): value saved in stickos/options/<core>.cfg,
 * otherwise the default (the first one in the list) */
#define MAXOPT 256
struct opt {
    char key[64], val[64], def[64];
    char *desc, *vals;                      /* description, values separated by '|' */
};
static struct opt opts[MAXOPT];
static int nopts;
static char opt_cfg[256];                   /* settings file of the current core */
static struct { char key[64], val[64]; } osaved[MAXOPT];
static int nosaved;

static void opt_cfg_path(char *dst, size_t n, const char *corefile)
{
    const char *b = strrchr(corefile, '/');
    b = b ? b + 1 : corefile;
    int l = (int)strcspn(b, ".");
    const char *suf = strstr(b, "_libretro");
    if (suf && suf - b < l)
        l = suf - b;
    snprintf(dst, n, CFGDIR "/options/%.*s.cfg", l, b);
}

static void opt_cfg_load(const char *corefile)
{
    opt_cfg_path(opt_cfg, sizeof(opt_cfg), corefile);
    nosaved = 0;
    FILE *f = fopen(opt_cfg, "r");
    if (!f)
        return;
    char line[256];
    while (nosaved < MAXOPT && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        snprintf(osaved[nosaved].key, sizeof(osaved[0].key), "%s", line);
        snprintf(osaved[nosaved].val, sizeof(osaved[0].val), "%s", eq + 1);
        nosaved++;
    }
    fclose(f);
}

/* is v one of the allowed values? */
static int opt_valid(const struct opt *o, const char *v)
{
    const char *p = o->vals;
    size_t n = strlen(v);
    while (p && *p) {
        size_t l = strcspn(p, "|");
        if (l == n && !strncmp(p, v, n))
            return 1;
        p += l + (p[l] == '|');
    }
    return 0;
}

/* StickOS defaults that replace the core's own (still changeable from Settings) */
static const char *const opt_stick_def[][2] = {
    /* skip drawing a frame when the audio is about to run dry, instead of slowing
     * down (Crash Bandicoot's intro had audio gaps on the stick without it) */
    { "pcsx_rearmed_frameskip_type", "auto" },
};

static void opt_add(const char *key, const char *desc, const char *vals)
{
    struct opt *o = NULL;
    for (int i = 0; i < nopts; i++)         /* a core may declare its options again */
        if (!strcmp(opts[i].key, key))
            o = &opts[i];
    if (!o) {
        if (nopts >= MAXOPT)
            return;
        o = &opts[nopts++];
    } else {
        free(o->desc);
        free(o->vals);
    }
    snprintf(o->key, sizeof(o->key), "%s", key);
    o->desc = strdup(desc);
    o->vals = strdup(vals);
    snprintf(o->def, sizeof(o->def), "%.*s", (int)strcspn(vals, "|"), vals);
    for (size_t i = 0; i < sizeof(opt_stick_def) / sizeof(opt_stick_def[0]); i++)
        if (!strcmp(opt_stick_def[i][0], key) && opt_valid(o, opt_stick_def[i][1]))
            snprintf(o->def, sizeof(o->def), "%s", opt_stick_def[i][1]);
    snprintf(o->val, sizeof(o->val), "%s", o->def);
    for (int i = 0; i < nosaved; i++)
        if (!strcmp(osaved[i].key, key) && opt_valid(o, osaved[i].val))
            snprintf(o->val, sizeof(o->val), "%s", osaved[i].val);
}

static void opt_clear(void)
{
    for (int i = 0; i < nopts; i++) {
        free(opts[i].desc);
        free(opts[i].vals);
    }
    nopts = 0;
}

/* saves only the values that differ from the default */
static int opt_cfg_save(void)
{
    char buf[MAXOPT * 140];
    int n = snprintf(buf, sizeof(buf), "# StickOS settings: key=value (changed values only)\n");
    for (int i = 0; i < nopts && n < (int)sizeof(buf) - 140; i++)
        if (strcmp(opts[i].val, opts[i].def))
            n += snprintf(buf + n, sizeof(buf) - n, "%s=%s\n", opts[i].key, opts[i].val);
    char dir[256];
    snprintf(dir, sizeof(dir), CFGDIR "/options");
    mkdirs(dir);
    return write_file(opt_cfg, buf, n);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (!logf_ || level < RETRO_LOG_WARN)
        return;
    va_list ap;
    fprintf(logf_, "[core] ");
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fflush(logf_);
}

/* game timing (globals: a core that clobbers registers cannot alter them) */
#define DISP_HZ 60.0
static double core_fps = 60.0, pace_credit;
static double t_log, t_save, t_combo, frames0;
static int vsync_done;                      /* the core showed a frame in this iteration */

/* Some cores leave the FPU control register (FPSCR) modified:
 * it is saved and restored around every call into the core. */
#ifdef __arm__
static inline unsigned fpscr_get(void) { unsigned v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }
static inline void fpscr_set(unsigned v) { __asm__ volatile("vmsr fpscr, %0" : : "r"(v)); }
#else
static inline unsigned fpscr_get(void) { return 0; }
static inline void fpscr_set(unsigned v) { (void)v; }
#endif
/* Calls into the core through vfp_call(): some cores (PCSX-ReARMed) do not preserve
 * the registers d8-d15, which the ARM calling convention requires. Without this the
 * values the compiler keeps there (e.g. the 50 Hz frame pacing) get corrupted and
 * the game stops. */
#if defined(__arm__)
__attribute__((naked, noinline)) static uintptr_t vfp_call(void *fn, uintptr_t a, uintptr_t b, uintptr_t c)
{
    __asm__ volatile(
        "push {r4, lr}\n"
        "vpush {d8-d15}\n"
        "mov r4, r0\n"
        "mov r0, r1\n"
        "mov r1, r2\n"
        "mov r2, r3\n"
        "blx r4\n"
        "vpop {d8-d15}\n"
        "pop {r4, pc}\n");
}
#else
static uintptr_t vfp_call(void *fn, uintptr_t a, uintptr_t b, uintptr_t c)
{
    return ((uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t))fn)(a, b, c);
}
#endif
#define CORE0(f)       ((void)vfp_call((void *)core.f, 0, 0, 0))
#define CORE1(f, a)    vfp_call((void *)core.f, (uintptr_t)(a), 0, 0)
#define CORE2(f, a, b) vfp_call((void *)core.f, (uintptr_t)(a), (uintptr_t)(b), 0)

/* watchdog (see wd_main) */

static void wd_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[128], st[256] = "", wc[64] = "";
        snprintf(p, sizeof(p), "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) {
            if (!fgets(st, sizeof(st), f))
                st[0] = 0;
            fclose(f);
        }
        snprintf(p, sizeof(p), "/proc/self/task/%s/wchan", e->d_name);
        f = fopen(p, "r");
        if (f) {
            if (!fgets(wc, sizeof(wc), f))
                wc[0] = 0;
            fclose(f);
        }
        char *c = strrchr(st, ')');           /* "pid (name) S ..." */
        st[strcspn(st, "\n")] = 0;
        logm("  thread %s %.*s state %c wchan %s", e->d_name, c ? (int)(c - st + 1) : 0, st,
             c && c[1] ? c[2] : '?', wc[0] ? wc : "-");
    }
    if (d)
        closedir(d);
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    for (int i = 0; f && i < 4 && fgets(l, sizeof(l), f); i++) {
        l[strcspn(l, "\n")] = 0;
        logm("  %s", l);
    }
    if (f)
        fclose(f);
}

static void *wd_main(void *arg)
{
    (void)arg;
    unsigned last = wd_count;
    int still = 0, reported = 0;
    for (;;) {
        sleep(1);
        unsigned c = wd_count;
        if (c != last) {
            if (reported)
                logm("watchdog: the game loop is running again after %d s", still);
            last = c;
            still = reported = 0;
            continue;
        }
        if (++still == 3 || (reported && still % 30 == 0)) {
            logm("watchdog: game loop stopped for %d s, %s", still,
                 wd_in_core ? "inside the core" : "outside the core");
            wd_threads();
            sync();
            reported = 1;
        }
    }
    return NULL;
}

static void core_run_safe(void)
{
    static int warned;
    unsigned f = fpscr_get();
    wd_in_core = 1;
    CORE0(run);
    wd_in_core = 0;
    wd_count++;
    unsigned g = fpscr_get();
    if ((g ^ f) & ~0xf800009fu) {           /* ignore NZCV, QC and cumulative exception flags */
        if (!warned++)
            logm("the core changed FPSCR from %08x to %08x: restored", f, g);
        fpscr_set(f);
    }
}

/* controller types declared by the core (SET_CONTROLLER_INFO), ports 1 and 2:
 * they become the "Player N controller" setting */
static struct {
    int n;
    unsigned id[32];
    char desc[32][64];
} ctl[2];

static void controller_info(const struct retro_controller_info *ci)
{
    for (int port = 0; port < 2 && ci && ci[port].types; port++) {
        ctl[port].n = 0;
        char vals[2048] = "";
        int first = -1;
        for (unsigned t = 0; t < ci[port].num_types && ctl[port].n < 32; t++)
            if ((ci[port].types[t].id & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD && ci[port].types[t].id ==
                RETRO_DEVICE_JOYPAD)
                first = t;
        for (int pass = 0; pass < 2; pass++)            /* the standard RetroPad first (default) */
            for (unsigned t = 0; t < ci[port].num_types && ctl[port].n < 32; t++) {
                if ((pass == 0) != ((int)t == first))
                    continue;
                const char *d = ci[port].types[t].desc ? ci[port].types[t].desc : "?";
                int k = ctl[port].n;
                snprintf(ctl[port].desc[k], sizeof(ctl[port].desc[k]), "%s", d);
                for (char *c = ctl[port].desc[k]; *c; c++)
                    if (*c == '|' || *c == '\t' || *c == '\n')
                        *c = '/';
                ctl[port].id[k] = ci[port].types[t].id;
                ctl[port].n++;
                snprintf(vals + strlen(vals), sizeof(vals) - strlen(vals), "%s%s", k ? "|" : "",
                         ctl[port].desc[k]);
            }
        if (ctl[port].n > 1) {
            char key[32], desc[64];
            snprintf(key, sizeof(key), "stickos_controller_p%d", port + 1);
            snprintf(desc, sizeof(desc), T("Player %d controller"), port + 1);
            opt_add(key, desc, vals);
        }
    }
}

/* controller type chosen for the port (default: RetroPad) */
static unsigned controller_for(int port)
{
    char key[32];
    snprintf(key, sizeof(key), "stickos_controller_p%d", port + 1);
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].key, key))
            for (int k = 0; k < ctl[port].n; k++)
                if (!strcmp(ctl[port].desc[k], opts[i].val))
                    return ctl[port].id[k];
    return RETRO_DEVICE_JOYPAD;
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = sysdir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = savedir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f > RETRO_PIXEL_FORMAT_RGB565)
            return false;
        pixfmt = f;
        logm("pixel format %d", f);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const struct retro_variable *v = data;
        for (; v && v->key; v++) {
            if (!v->value)
                continue;
            const char *semi = strchr(v->value, ';');
            char desc[160];
            if (!semi) {
                opt_add(v->key, v->key, "");
                continue;
            }
            snprintf(desc, sizeof(desc), "%.*s", (int)(semi - v->value), v->value);
            semi++;
            while (*semi == ' ')
                semi++;
            opt_add(v->key, desc, semi);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (int i = 0; i < nopts; i++)
            if (!strcmp(opts[i].key, v->key)) {
                v->value = opts[i].val;
                return true;
            }
        v->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;              /* the core will use SET_VARIABLES */
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = lang_retro[lang];
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 3;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        if (av->timing.fps > 1)
            core_fps = av->timing.fps;
        if (av->timing.sample_rate > 1000)
            in_rate = av->timing.sample_rate;
        logm("core changes timing: fps %.3f, audio %.0f Hz", av->timing.fps, av->timing.sample_rate);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        controller_info(data);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        logm("core message: %s", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        /* used by the cores' automatic frameskip */
        const struct retro_audio_buffer_status_callback *c = data;
        audio_status_cb = c ? c->callback : NULL;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;                        /* the buffer is fixed (85 ms) */
    default:
        return false;
    }
}

/* ---- game video: integer scale, centered, converted to RGB565 */
static int gw, gh, gclear;
static double frames_shown;

static inline uint16_t px1555(uint16_t c)
{
    return ((c & 0x7fe0) << 1) | ((c & 0x0200) >> 4) | (c & 0x1f);
}

/* time measurements for the performance log (seconds, summed over 10 s) */
static double pf_run, pf_run_max, pf_draw, pf_wait;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    double t0 = now();
    vsync_done = 1;
    if (!data || !w || !h) {                /* repeated frame */
        wait_vsync();
        pf_wait += now() - t0;
        return;
    }
    int s = YRES / (int)h;
    if (XRES / (int)w < s)
        s = XRES / (int)w;
    if (s < 1)
        s = 1;
    int ow = w * s, oh = h * s;
    if (ow > XRES)
        ow = XRES, w = XRES / s;
    if (oh > YRES)
        oh = YRES, h = YRES / s;
    if ((int)w != gw || (int)h != gh) {
        gw = w;
        gh = h;
        gclear = 2;
        logm("game video %ux%u, scale %d", w, h, s);
    }
    uint16_t *dst = page(back);
    if (gclear) {
        for (int y = 0; y < YRES; y++)
            memset(dst + (size_t)y * STRIDE, 0, XRES * 2);
        gclear--;
    }
    int x0 = (XRES - ow) / 2, y0 = (YRES - oh) / 2;
    static uint16_t line[2048], conv[2048];
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)data + y * pitch;
        const uint16_t *row;
        if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
            row = (const uint16_t *)src;
        } else if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *p = (const uint32_t *)src;
            for (unsigned x = 0; x < w; x++)
                conv[x] = ((p[x] >> 8) & 0xf800) | ((p[x] >> 5) & 0x07e0) | ((p[x] >> 3) & 0x1f);
            row = conv;
        } else {
            const uint16_t *p = (const uint16_t *)src;
            for (unsigned x = 0; x < w; x++)
                conv[x] = px1555(p[x]);
            row = conv;
        }
        const uint16_t *out = row;
        if (s > 1) {
            uint16_t *l = line;
            if (s == 2)
                for (unsigned x = 0; x < w; x++, l += 2)
                    l[0] = l[1] = row[x];
            else if (s == 3)
                for (unsigned x = 0; x < w; x++, l += 3)
                    l[0] = l[1] = l[2] = row[x];
            else
                for (unsigned x = 0; x < w; x++)
                    for (int k = 0; k < s; k++)
                        *l++ = row[x];
            out = line;
        }
        uint16_t *d = dst + (size_t)(y0 + y * s) * STRIDE + x0;
        for (int k = 0; k < s; k++, d += STRIDE)
            memcpy(d, out, ow * 2);
    }
    double t1 = now();
    present();
    pf_draw += t1 - t0;
    pf_wait += now() - t1;
    frames_shown++;
}

/* cpu time of the whole system from /proc/stat: user+nice, system+irq, iowait, idle */
static void cpu_times(unsigned long long v[4])
{
    unsigned long long u = 0, n = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq) != 7)
            u = n = sy = id = io = irq = sirq = 0;
        fclose(f);
    }
    v[0] = u + n;
    v[1] = sy + irq + sirq;
    v[2] = io;
    v[3] = id;
}

static void audio_sample_cb(int16_t l, int16_t r)
{
    int16_t f[2] = { l, r };
    audio_push(f, 1);
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    audio_push(data, frames);
    return frames;
}

static void input_poll_cb(void)
{
    input_poll();
    nraw = 0;
    for (int j = 0; j < MAXPAD; j++) {
        pad_bits[j] = j < npads ? pad_state(&pads[j]) : 0;
        for (int a = 0; a < 4; a++)
            pad_axes[j][a] = j < npads ? stick_val(&pads[j], &pads[j].map[S_LX + a][0]) : 0;
    }
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port >= MAXPAD)
        return 0;
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG) {
        if (index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y)
            return pad_axes[port][index * 2 + id];
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON && id < NBTN)   /* "analog" buttons (L2/R2) */
            return ((pad_bits[port] >> id) & 1) ? 32767 : 0;
        return 0;
    }
    if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return pad_bits[port];
    return id < NBTN ? (pad_bits[port] >> id) & 1 : 0;
}

#define SYM(n)                                                     \
    do {                                                           \
        *(void **)&core.n = dlsym(core.h, "retro_" #n);            \
        if (!core.n) {                                             \
            logm("missing symbol retro_%s", #n);                   \
            return -1;                                             \
        }                                                          \
    } while (0)

static int core_load(const char *path)
{
    opt_cfg_load(path);
    core.h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!core.h) {
        logm("dlopen: %s", dlerror());
        return -1;
    }
    SYM(init); SYM(deinit); SYM(get_system_info); SYM(get_system_av_info); SYM(set_environment);
    SYM(set_video_refresh); SYM(set_audio_sample); SYM(set_audio_sample_batch); SYM(set_input_poll);
    SYM(set_input_state); SYM(set_controller_port_device); SYM(reset); SYM(run); SYM(serialize_size);
    SYM(serialize); SYM(unserialize); SYM(load_game); SYM(unload_game); SYM(get_memory_data);
    SYM(get_memory_size);
    return 0;
}

/* ================================================================ Settings */
/* stickui --options CORE FILE [ROM]: loads the core, collects the options it declares
 * and writes them to FILE (separate process: the core may even crash) */
static void nop_video(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void nop_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t nop_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void nop_poll(void) {}
static int16_t nop_state(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

static int dump_options(const char *corepath, const char *out, const char *rom)
{
    if (core_load(corepath))
        return 1;
    snprintf(sysdir, sizeof(sysdir), BIOS);
    snprintf(savedir, sizeof(savedir), "/tmp");
    core.set_environment(env_cb);
    CORE0(init);
    if (!nopts && rom) {                    /* arcade cores declare their options only with a game loaded */
        core.set_video_refresh(nop_video);
        core.set_audio_sample(nop_audio);
        core.set_audio_sample_batch(nop_batch);
        core.set_input_poll(nop_poll);
        core.set_input_state(nop_state);
        struct retro_game_info gi = { .path = rom };
        logm("options: loading %s to read them", rom);
        CORE1(load_game, &gi);
    }
    FILE *f = fopen(out, "w");
    if (!f)
        return 1;
    for (int i = 0; i < nopts; i++)
        fprintf(f, "%s\t%s\t%s\n", opts[i].key, opts[i].desc, opts[i].vals);
    fclose(f);
    _exit(0);                               /* no deinit: some cores are slow to shut down */
}

/* loads a core's options into the menu (through a child process) */
static int load_core_options(const char *corefile, const char *rom)
{
    char corepath[256];
    const char *tmp = "/tmp/options.txt";
    snprintf(corepath, sizeof(corepath), COREDIR "/%s", corefile);
    unlink(tmp);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "stickui", "--options", corepath, tmp, rom, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (pid > 0)
        waitpid(pid, &st, 0);
    opt_clear();
    opt_cfg_load(corepath);
    FILE *f = fopen(tmp, "r");
    if (!f) {
        logm("options of %s not read (status %d)", corefile, st);
        return -1;
    }
    static char line[8192];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2)
            continue;
        *t1 = *t2 = 0;
        opt_add(line, t1 + 1, t2 + 1);
    }
    fclose(f);
    logm("options of %s: %d", corefile, nopts);
    return 0;
}

/* moves to the next (dir=1) or previous (dir=-1) value */
static void opt_step(struct opt *o, int dir)
{
    const char *v[128];
    int lens[128], n = 0, cur = 0;
    for (const char *p = o->vals; p && *p && n < 128;) {
        size_t l = strcspn(p, "|");
        v[n] = p;
        lens[n] = (int)l;
        if (l == strlen(o->val) && !strncmp(p, o->val, l))
            cur = n;
        n++;
        p += l + (p[l] == '|');
    }
    if (n < 2)
        return;
    cur = (cur + dir + n) % n;
    snprintf(o->val, sizeof(o->val), "%.*s", lens[cur], v[cur]);
}

/* copies at most max characters (UTF-8), adding "..." if cut */
static void clip(char *dst, size_t n, const char *src, int max)
{
    const char *p = src;
    int c = 0;
    if (utf8_len(src) > max)
        max -= 3;
    while (*p && c < max) {
        p++;
        while ((*p & 0xC0) == 0x80)
            p++;
        c++;
    }
    snprintf(dst, n, "%.*s%s", (int)(p - src), src, *p ? "..." : "");
}

static void options_menu(const char *title, const struct sys *sy)
{
    const char *corefile = sy->core;
    char rom[512], *romp = NULL;
    if (sy->arcade) {                       /* a game is needed to read the options */
        char **g;
        int c = list_games(sy, &g, NULL);
        if (c) {
            snprintf(rom, sizeof(rom), ROMS "/%s/%s", sy->dir, g[0]);
            romp = rom;
        }
        free_list(g, c);
    }
    screen(title, NULL);
    text_center(320, 2, C_TEXT, T("Reading settings..."));
    ui_show();
    if (sy->arcade && !romp) {
        char m[200];
        screen(title, T("B: back"));
        text_center(280, 2, C_TEXT, T("This emulator reads its settings from a game:"));
        snprintf(m, sizeof(m), T("copy at least one game into roms/%s"), sy->dir);
        text_center(330, 2, C_TEXT, m);
        ui_show();
        menu_release_all();
        for (;;) {
            input_wait(100);
            if (menu_buttons() & ((1u << B_B) | (1u << B_A)))
                return;
        }
    }
    if (load_core_options(corefile, romp)) {
        screen(title, T("B: back"));
        text_center(320, 2, C_WARN, T("Unable to read this emulator's settings"));
        ui_show();
        for (;;) {
            input_wait(100);
            if (menu_buttons() & ((1u << B_B) | (1u << B_A)))
                return;
        }
    }
    const int rows = (YRES - 64 - 56 - 24) / 40;
    int sel = 0, top = 0, dirty = 1, changed = 0, n = nopts + 1;  /* +1: restore defaults */
    menu_release_all();
    for (;;) {
        if (sel < top)
            top = sel;
        if (sel >= top + rows)
            top = sel - rows + 1;
        if (dirty) {
            screen(title, nopts ? T("</>: change   B: save and exit") : T("B: back"));
            if (!nopts)
                text_center(200, 2, C_DIM, T("This emulator has no settings"));
            for (int i = 0; i < rows && top + i < n; i++) {
                int y = 76 + i * 40, k = top + i;
                uint16_t c = k == sel ? RGB(0, 0, 0) : C_TEXT;
                if (k == sel)
                    fill(16, y - 2, XRES - 32, 38, C_SEL);
                char a[200], b[100];
                if (k == nopts) {
                    if (nopts)
                        text(40, y + 2, 2, k == sel ? c : C_DIM, T("Restore all default values"));
                    continue;
                }
                struct opt *o = &opts[k];
                clip(a, sizeof(a), o->desc, 44);
                clip(b, sizeof(b), o->val, 26);
                text(40, y + 2, 2, c, a);
                int changed_v = strcmp(o->val, o->def) != 0;
                int w = utf8_len(b);
                text(XRES - 48 - w * 16, y + 2, 2, k == sel ? c : (changed_v ? C_SEL : C_OK), b);
            }
            if (n > rows) {
                char pos[32];
                snprintf(pos, sizeof(pos), "%d/%d", sel + 1, n);
                text(XRES - 32 - (int)strlen(pos) * 16, 16, 2, C_DIM, pos);
            }
            ui_show();
            dirty = 0;
        }
        input_wait(30);
        unsigned k = menu_buttons();
        if (k & (1u << B_UP)) {
            sel = sel > 0 ? sel - 1 : n - 1;
            dirty = 1;
        }
        if (k & (1u << B_DOWN)) {
            sel = sel < n - 1 ? sel + 1 : 0;
            dirty = 1;
        }
        if (sel < nopts && (k & ((1u << B_RIGHT) | (1u << B_A) | (1u << B_LEFT)))) {
            opt_step(&opts[sel], (k & (1u << B_LEFT)) ? -1 : 1);
            changed = dirty = 1;
        }
        if (sel == nopts && nopts && (k & (1u << B_A))) {
            for (int i = 0; i < nopts; i++)
                snprintf(opts[i].val, sizeof(opts[i].val), "%s", opts[i].def);
            changed = dirty = 1;
        }
        if (k & (1u << B_B)) {
            if (changed) {
                int r = opt_cfg_save();
                logm("settings saved to %s (%s)", opt_cfg, r ? "ERROR" : "ok");
                screen(title, NULL);
                text_center(320, 2, r ? C_WARN : C_OK, r ? T("Error while saving!") : T("Saved!"));
                ui_show();
                usleep(700000);
            }
            opt_clear();
            return;
        }
    }
}

static void language_menu(void)
{
    char *items[NLANG];
    int sel = lang;
    for (int i = 0; i < NLANG; i++)
        items[i] = (char *)lang_name[i];
    int k = list_menu(T("Language"), items, NLANG, &sel, NULL);
    if (k >= 0 && k != lang) {
        lang = k;
        set_put("language", lang_code[lang]);
        settings_save();
        logm("language: %s", lang_code[lang]);
    }
}

static void settings_menu(void)
{
    int sel = 0;
    for (;;) {
        char *items[NSYS + 2];
        const char *cores[NSYS + 2];
        const struct sys *sy[NSYS + 2];
        int n = 0;
        char lbuf[128];
        /* "Language" is also shown in English, so it can be found in any language */
        snprintf(lbuf, sizeof(lbuf), "%s%s: %s", T("Language"), lang == L_EN ? "" : " (Language)",
                 lang_name[lang]);
        items[n] = strdup(lbuf);
        sy[n] = NULL;
        cores[n++] = NULL;
        items[n] = strdup(T("Joypad: configure buttons"));
        sy[n] = NULL;
        cores[n++] = NULL;
        for (int i = 0; i < NSYS; i++) {
            if (!core_exists(&systems[i]))
                continue;
            int dup = 0;
            for (int j = 2; j < n; j++)
                if (!strcmp(cores[j], systems[i].core))
                    dup = 1;
            if (dup)
                continue;
            char names[200] = "";
            for (int j = i; j < NSYS; j++)
                if (!strcmp(systems[j].core, systems[i].core))
                    snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s",
                             names[0] ? " / " : "", systems[j].name);
            char core[64], buf[300];
            snprintf(core, sizeof(core), "%.*s", (int)(strstr(systems[i].core, "_libretro") ?
                     strstr(systems[i].core, "_libretro") - systems[i].core : 63), systems[i].core);
            snprintf(buf, sizeof(buf), "%s  (%s)", names, core);
            items[n] = strdup(buf);
            sy[n] = &systems[i];
            cores[n++] = systems[i].core;
        }
        int k = list_menu(T("Settings"), items, n, &sel, NULL);
        char title[300];
        if (k > 1)
            snprintf(title, sizeof(title), "%s", items[k]);
        for (int i = 0; i < n; i++)
            free(items[i]);
        if (k < 0)
            return;
        if (k == 0)
            language_menu();
        else if (k == 1)
            wizard(-1);
        else
            options_menu(title, sy[k]);
    }
}

static char srm_path[512], state_path[512];
static uint8_t *srm_copy;
static size_t srm_len;

static void sram_load(void)
{
    void *d = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!d || !n)
        return;
    size_t len;
    void *f = read_file(srm_path, &len);
    if (f) {
        memcpy(d, f, len < n ? len : n);
        free(f);
        logm("save loaded: %s", srm_path);
    }
    srm_copy = malloc(n);
    memcpy(srm_copy, d, n);
    srm_len = n;
}

static void sram_save(int force)
{
    void *d = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!d || !n || n != srm_len)
        return;
    if (!force && !memcmp(d, srm_copy, n))
        return;
    if (memcmp(d, srm_copy, n) == 0 && force && access(srm_path, F_OK) != 0)
        return;                                 /* never modified: no file */
    memcpy(srm_copy, d, n);
    if (write_file(srm_path, d, n) == 0)
        logm("save written: %s", srm_path);
}

/* saves used to live in saves/<system>/: move them to roms/<system>/save/,
 * never overwriting a file that already exists there */
static void migrate_saves(void)
{
    DIR *top = opendir(OLDSAVES);
    if (!top)
        return;
    struct dirent *e, *f;
    while ((e = readdir(top))) {
        if (e->d_name[0] == '.')
            continue;
        char from[512], to[512];
        snprintf(from, sizeof(from), OLDSAVES "/%s", e->d_name);
        DIR *d = opendir(from);
        if (!d)
            continue;
        snprintf(to, sizeof(to), ROMS "/%s/save", e->d_name);
        mkdirs(to);
        int moved = 0, kept = 0;
        while ((f = readdir(d))) {
            if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, ".."))
                continue;
            char a[1024], b[1024];
            snprintf(a, sizeof(a), "%s/%s", from, f->d_name);
            snprintf(b, sizeof(b), "%s/%s", to, f->d_name);
            if (access(b, F_OK) != 0 && rename(a, b) == 0)
                moved++;
            else
                kept++;
        }
        closedir(d);
        rmdir(from);                        /* only succeeds if now empty */
        logm("saves: moved %d files from %s to %s%s", moved, from, to,
             kept ? " (some already existed, left in the old folder)" : "");
    }
    closedir(top);
    rmdir(OLDSAVES);
}

/* pause menu; returns 0 = resume, 1 = quit */
static int pause_menu(void)
{
    static const char *items[] = { "Resume", "Save state", "Load state", "Restart", "Quit game" };
    int sel = 0, n = 5;
    char msg[80] = "";
    menu_release_all();
    for (;;) {
        fill(0, 0, XRES, YRES, C_BG);
        fill(0, 0, XRES, 64, C_BAR);
        text(32, 16, 2, C_TEXT, T("Pause"));
        for (int i = 0; i < n; i++) {
            int y = 160 + i * 56;
            if (i == sel)
                fill(380, y - 6, 520, 48, C_SEL);
            text_center(y, 2, i == sel ? RGB(0, 0, 0) : C_TEXT, T(items[i]));
        }
        if (msg[0])
            text_center(520, 2, C_OK, msg);
        ui_show();
        unsigned k = 0;
        while (!k) {
            input_wait(50);
            k = menu_buttons();
        }
        msg[0] = 0;
        if (k & (1u << B_UP))
            sel = (sel + n - 1) % n;
        if (k & (1u << B_DOWN))
            sel = (sel + 1) % n;
        if (k & (1u << B_B))
            return 0;
        if (!(k & (1u << B_A)))
            continue;
        if (sel == 0)
            return 0;
        if (sel == 4)
            return 1;
        if (sel == 3) {
            CORE0(reset);
            return 0;
        }
        size_t sz = core.serialize_size();
        if (sel == 1) {
            void *b = malloc(sz ? sz : 1);
            if (sz && (CORE2(serialize, b, sz) & 0xff) && write_file(state_path, b, sz) == 0)
                snprintf(msg, sizeof(msg), "%s", T("State saved"));
            else
                snprintf(msg, sizeof(msg), "%s", T("Save failed"));
            free(b);
        } else {
            size_t len;
            void *b = read_file(state_path, &len);
            if (b && (CORE2(unserialize, b, len) & 0xff)) {
                free(b);
                return 0;
            }
            free(b);
            snprintf(msg, sizeof(msg), "%s", T("No saved state"));
        }
    }
}

static int play(const char *corepath, const char *rom, const char *sysname)
{
    if (core_load(corepath))
        return 1;
    snprintf(sysdir, sizeof(sysdir), BIOS);
    snprintf(savedir, sizeof(savedir), ROMS "/%s/save", sysname);
    mkdirs(sysdir);
    mkdirs(savedir);
    const char *base = strrchr(rom, '/');
    base = base ? base + 1 : rom;
    char stem[256];
    display_name(stem, sizeof(stem), base);
    snprintf(srm_path, sizeof(srm_path), "%s/%s.srm", savedir, stem);
    snprintf(state_path, sizeof(state_path), "%s/%s.state", savedir, stem);

    core.set_environment(env_cb);
    CORE0(init);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);

    struct retro_system_info si = { 0 };
    core.get_system_info(&si);
    logm("core %s %s, need_fullpath %d", si.library_name, si.library_version, si.need_fullpath);
    struct retro_game_info gi = { .path = rom };
    size_t len = 0;
    void *data = NULL;
    /* read only the first bytes: CD images (hundreds of MB) must not be loaded into
     * memory when the core reads the file by itself (need_fullpath) */
    uint8_t head[4] = { 0 };
    FILE *hf = fopen(rom, "rb");
    size_t hl = hf ? fread(head, 1, sizeof(head), hf) : 0;
    if (hf)
        fclose(hf);
    if (!hf) {
        logm("cannot open ROM: %s (%s)", rom, strerror(errno));
        return 1;
    }
    int is_zip = hl == 4 && rd32(head) == 0x04034b50;
    if (is_zip || !si.need_fullpath) {
        data = read_file(rom, &len);
        if (!data) {
            logm("cannot read ROM: %s (%s)", rom, strerror(errno));
            return 1;
        }
    }
    /* zip archive (detected by content, not by name): extract the ROM to /tmp */
    static char inner_path[512];
    if (is_zip) {
        const char *exts = "";
        int arcade = 0;
        for (int i = 0; i < NSYS; i++)
            if (!strcmp(systems[i].dir, sysname)) {
                exts = systems[i].exts;
                arcade = systems[i].arcade;
            }
        if (arcade)
            goto no_extract;
        char inner[256];
        size_t ulen = 0;
        void *u = unzip_rom(data, len, exts, &ulen, inner, sizeof(inner));
        free(data);
        if (!u) {
            logm("invalid zip: %s", rom);
            return 1;
        }
        mkdir("/tmp/rom", 0755);
        snprintf(inner_path, sizeof(inner_path), "/tmp/rom/%s", inner);
        if (write_file(inner_path, u, ulen)) {
            logm("cannot write %s", inner_path);
            free(u);
            return 1;
        }
        logm("zip: extracted %s (%zu bytes)", inner, ulen);
        data = u;
        len = ulen;
        gi.path = inner_path;
    }
no_extract:
    if (si.need_fullpath) {
        free(data);
        data = NULL;
        len = 0;
    }
    gi.data = data;
    gi.size = len;
    if (!(CORE1(load_game, &gi) & 0xff)) {
        logm("the core did not load the ROM");
        return 1;
    }
    for (int p = 0; p < 2; p++) {
        unsigned dev = controller_for(p);
        core.set_controller_port_device(p, dev);
        if (dev != RETRO_DEVICE_JOYPAD)
            logm("player %d: controller type %u", p + 1, dev);
    }
    struct retro_system_av_info av;
    core.get_system_av_info(&av);
    in_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100;
    core_fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
    logm("av: %ux%u fps %.3f audio %.0f Hz", av.geometry.base_width, av.geometry.base_height, av.timing.fps,
         in_rate);
    if (audio_open(48000) && audio_open(44100))
        logm("audio not available");
    sram_load();
    clear_pages();
    gclear = 2;
    for (int i = 0; i < nopts; i++)
        if (strcmp(opts[i].val, opts[i].def) || strstr(opts[i].key, "thread") || strstr(opts[i].key, "readahead") ||
            strstr(opts[i].key, "frameskip_type"))
            logm("option %s = %s", opts[i].key, opts[i].val);
    pthread_t wd;
    if (pthread_create(&wd, NULL, wd_main, NULL) == 0)
        pthread_detach(wd);

    static double t0;
    static unsigned under0;
    static unsigned long runs, runs0;
    t0 = t_log = t_save = now();
    t_combo = 0;
    frames0 = frames_shown;
    under0 = underruns;
    pace_credit = 0;
    for (;;) {
        /* Each iteration = one TV vsync (60 Hz). The core advances one frame only
         * when it has "credit": a 50 Hz game skips 1 iteration out of 6 and the TV
         * repeats the previous frame. Above 98.5% of 60 Hz it always runs. */
        int run = 1;
        if (core_fps < DISP_HZ * 0.985) {
            pace_credit += core_fps / DISP_HZ;
            if (pace_credit >= 1.0)
                pace_credit -= 1.0;
            else
                run = 0;
        }
        vsync_done = 0;
        if (run) {
            audio_status();
            double tr = now(), w0 = pf_wait;
            core_run_safe();
            double d = now() - tr - (pf_wait - w0);     /* without the vsync waits */
            pf_run += d;
            if (d > pf_run_max)
                pf_run_max = d;
            runs++;
            audio_flush();
        }
        if (!vsync_done)                    /* no frame shown: wait for the vsync anyway */
            wait_vsync();
        /* SELECT+START (or START held for 2 s if there is no SELECT) = pause */
        unsigned m = 0;
        for (int j = 0; j < npads; j++)
            m |= pad_state(&pads[j]);
        int has_select = 0;
        for (int j = 0; j < npads; j++)
            if (pads[j].map[B_SELECT][0].type)
                has_select = 1;
        int hot = has_select ? (m & (1u << B_SELECT)) && (m & (1u << B_START)) : (m & (1u << B_START)) != 0;
        if (hot) {
            if (!t_combo)
                t_combo = now();
            if (now() - t_combo > (has_select ? 0.3 : 2.0)) {
                t_combo = 0;
                sram_save(0);
                static double tp;
                tp = now();
                logm("pause");
                int quit = pause_menu();
                logm("pause ended after %.0f s%s", now() - tp, quit ? ", quit" : "");
                if (quit)
                    break;
                clear_pages();
                gclear = 2;
                gw = 0;
                if (pcm >= 0)
                    ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
                menu_release_all();
                /* the pause does not count in the statistics */
                t_log = now();
                frames0 = frames_shown;
                runs0 = runs;
                under0 = underruns;
            }
        } else {
            t_combo = 0;
        }
        if (now() - t_save > 30) {
            t_save = now();
            sram_save(0);
        }
        if (now() - t_log > 10) {
            double dt = now() - t_log;
            logm("performance: game %.1f fps (expected %.1f), screen %.1f fps, audio underruns %u",
                 (runs - runs0) / dt, core_fps, (frames_shown - frames0) / dt, underruns - under0);
            static unsigned long long c0[4];
            unsigned long long c1[4], tot = 0;
            cpu_times(c1);
            for (int i = 0; i < 4; i++)
                tot += c1[i] - c0[i];
            unsigned long nr = runs - runs0 ? runs - runs0 : 1;
            if (c0[3] && tot)
                logm("  per frame: core %.1f ms (max %.0f), drawing %.1f ms; cpu: user %llu%% sys %llu%% "
                     "wait-io %llu%% idle %llu%%", pf_run * 1000 / nr, pf_run_max * 1000,
                     pf_draw * 1000 / (frames_shown - frames0 ? frames_shown - frames0 : 1),
                     (c1[0] - c0[0]) * 100 / tot, (c1[1] - c0[1]) * 100 / tot, (c1[2] - c0[2]) * 100 / tot,
                     (c1[3] - c0[3]) * 100 / tot);
            memcpy(c0, c1, sizeof(c0));
            pf_run = pf_run_max = pf_draw = pf_wait = 0;
            frames0 = frames_shown;
            runs0 = runs;
            under0 = underruns;
            t_log = now();
        }
    }
    sram_save(0);
    CORE0(unload_game);
    if (gi.path == inner_path)
        unlink(inner_path);
    CORE0(deinit);
    logm("game quit after %.0f s", now() - t0);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    log_open();
    settings_load();
    if (argc >= 4 && !strcmp(argv[1], "--options"))
        return dump_options(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    if (video_init())
        return 1;
    int game = argc >= 5 && !strcmp(argv[1], "--play");
    double t_splash = now();
    if (!game)
        splash();
    cfg_load();
    input_scan();
    if (game)
        return play(argv[2], argv[3], argv[4]);
    logm("StickOS %s: menu, language %s", VERSION, lang_code[lang]);
    if (access(CFGDIR "/opzioni", F_OK) == 0 && access(CFGDIR "/options", F_OK) != 0)
        rename(CFGDIR "/opzioni", CFGDIR "/options");     /* folder name used up to 0.1 */
    mkdirs(ROMS);
    for (int i = 0; i < NSYS; i++)
        if (core_exists(&systems[i])) {
            char p[256];
            snprintf(p, sizeof(p), ROMS "/%s", systems[i].dir);
            mkdirs(p);
            if (systems[i].extra) {
                snprintf(p, sizeof(p), ROMS "/%s", systems[i].extra);
                mkdirs(p);
            }
        }
    migrate_saves();
    while (now() - t_splash < 1.5)                   /* the logo stays on screen at least 1.5 s */
        usleep(50000);
    clear_pages();
    offer_config();
    return main_menu();
}
