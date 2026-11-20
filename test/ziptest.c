/* prova: estrazione zip + load_game del core su tutte le ROM date */
#define main stickui_main
#include "../ui/stickui.c"
#undef main
int main(int argc, char **argv)
{
    if (core_load(argv[1]))
        return 2;
    core.set_environment(env_cb);
    core.init();
    int ok = 0, ko = 0;
    for (int i = 2; i < argc; i++) {
        size_t len, ulen;
        unsigned char *d = read_file(argv[i], &len);
        char inner[256] = "";
        void *u = d ? unzip_rom(d, len, systems[0].exts, &ulen, inner, sizeof(inner)) : NULL;
        struct retro_game_info gi = { .path = inner, .data = u, .size = ulen };
        if (u && core.load_game(&gi)) {
            ok++;
            core.unload_game();
        } else {
            ko++;
            printf("KO %s [%s]\n", argv[i], inner);
        }
        free(u);
        free(d);
    }
    printf("caricati %d, falliti %d\n", ok, ko);
    return 0;
}
