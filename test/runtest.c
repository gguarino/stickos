/* headless test: loads core + ROM and runs 300 frames with the same callbacks as StickOS */
#define main stickui_main
#include "../ui/stickui.c"
#undef main
#include <ucontext.h>
static void segv(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    void *pc = (void *)uc->uc_mcontext.arm_pc, *lr = (void *)uc->uc_mcontext.arm_lr;
    Dl_info a = { 0 }, b = { 0 };
    dladdr(pc, &a);
    dladdr(lr, &b);
    fprintf(stderr, "CRASH sig %d addr %p\n pc %p in %s (+0x%lx) %s\n lr %p in %s (+0x%lx) %s\n", sig, si->si_addr,
            pc, a.dli_fname, (unsigned long)((char *)pc - (char *)a.dli_fbase), a.dli_sname ? a.dli_sname : "",
            lr, b.dli_fname, (unsigned long)((char *)lr - (char *)b.dli_fbase), b.dli_sname ? b.dli_sname : "");
    _exit(1);
}
static void t_video(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; frames_shown++; }
int main(int argc, char **argv)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    if (core_load(argv[1]))
        return 2;
    snprintf(sysdir, sizeof(sysdir), "/tmp");
    snprintf(savedir, sizeof(savedir), "/tmp");
    core.set_environment(env_cb);
    core.init();
    core.set_video_refresh(t_video);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    size_t len;
    void *d = read_file(argv[2], &len);
    struct retro_game_info gi = { .path = argv[2], .data = d, .size = len };
    if (!core.load_game(&gi)) { printf("load_game failed\n"); return 3; }
    for (int p = 0; p < 2; p++) core.set_controller_port_device(p, controller_for(p));
    printf("loaded, controllers %u %u\n", controller_for(0), controller_for(1));
    for (int i = 0; i < 300; i++) core_run_safe();
    printf("300 frames OK (%.0f shown)\n", frames_shown);
    return 0;
}
