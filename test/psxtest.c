/* headless test of a CD game: psxtest CORE ROM FRAMES OUTDIR
 * runs FRAMES frames with the StickOS callbacks (options from SD/stickos/options),
 * every 50 frames prints how many real frames the core produced and how long it took,
 * and every 250 frames writes the last real frame as OUTDIR/fNNNNN.ppm.
 * A 5th argument simulates an audio buffer always about to run dry (frameskip test). */
#define main stickui_main
#include "../ui/stickui.c"
#undef main
static const void *t_data;
static unsigned t_w, t_h, t_real;
static size_t t_pitch;
static uint16_t t_copy[1024 * 1024];
static void t_video(const void *d, unsigned w, unsigned h, size_t p)
{
    if (!d)
        return;
    t_real++;
    t_w = w; t_h = h; t_pitch = p;
    for (unsigned y = 0; y < h; y++)
        memcpy(t_copy + y * w, (const uint8_t *)d + y * p, w * 2);
    t_data = t_copy;
}
static void t_dump(const char *dir, int n)
{
    if (!t_data)
        return;
    char p[512];
    snprintf(p, sizeof(p), "%s/f%05d.ppm", dir, n);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6 %u %u 255\n", t_w, t_h);
    for (unsigned i = 0; i < t_w * t_h; i++) {
        uint16_t c = t_copy[i];
        if (pixfmt != RETRO_PIXEL_FORMAT_RGB565)
            c = px1555(c);
        unsigned char rgb[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
int main(int argc, char **argv)
{
    log_open();
    if (core_load(argv[1]))
        return 2;
    snprintf(sysdir, sizeof(sysdir), BIOS);
    snprintf(savedir, sizeof(savedir), "/tmp");
    core.set_environment(env_cb);
    core.init();
    core.set_video_refresh(t_video);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    struct retro_game_info gi = { .path = argv[2] };
    if (!core.load_game(&gi)) { printf("load_game failed\n"); return 3; }
    for (int p = 0; p < 2; p++) core.set_controller_port_device(p, controller_for(p));
    int frames = atoi(argv[3]);
    static double t; t = now();     /* static: the core does not preserve the NEON registers d8-d15 */
    static unsigned r0;
    for (int i = 1; i <= frames; i++) {
        if (argc > 5 && audio_status_cb)    /* simulate an audio buffer about to run dry */
            vfp_call((void *)audio_status_cb, 1, 5, 1);
        else
            audio_status();
        core_run_safe();
        audio_flush();                      /* no audio device: drops the samples */
        if (i % 50 == 0) {
            printf("frame %5d: %2u real frames, %.0f ms per frame\n", i, t_real - r0, (now() - t) * 1000 / 50);
            fflush(stdout);
            r0 = t_real;
            t = now();
        }
        if (i % 250 == 0)
            t_dump(argv[4], i);
    }
    return 0;
}
