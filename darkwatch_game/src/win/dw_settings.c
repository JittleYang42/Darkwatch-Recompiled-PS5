/*
 * dw_settings.c -- darkwatch.ini: load, save, defaults, and the mapping from
 * settings to the runtime's environment switches.
 */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dw_settings.h"
#include "xinput_xbox.h"

DwSettings g_dw;

/* ── paths ───────────────────────────────────────────────────────── */

const char *dw_exe_dir(void)
{
    static char dir[DW_PATH_MAX];
    if (!dir[0]) {
        char *p;
        GetModuleFileNameA(NULL, dir, sizeof dir);
        p = strrchr(dir, '\\');
        if (p)
            *p = 0;
    }
    return dir;
}

const char *dw_settings_path(void)
{
    static char path[DW_PATH_MAX];
    if (!path[0])
        snprintf(path, sizeof path, "%s\\darkwatch.ini", dw_exe_dir());
    return path;
}

int dw_game_dir_ok(const char *dir)
{
    char p[DW_PATH_MAX + 16];
    DWORD a;
    if (!dir || !*dir)
        return 0;
    snprintf(p, sizeof p, "%s\\default.xbe", dir);
    a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

const char *dw_game_dir(void)
{
    static char found[DW_PATH_MAX];
    static const char *const rel[] = { "game", "..\\game", "..\\..\\game", "..\\..\\..\\game" };
    int i;
    if (g_dw.game_dir[0])
        return g_dw.game_dir;
    for (i = 0; i < 4; i++) {
        char p[DW_PATH_MAX];
        snprintf(p, sizeof p, "%s\\%s", dw_exe_dir(), rel[i]);
        if (dw_game_dir_ok(p)) {
            if (!GetFullPathNameA(p, sizeof found, found, NULL))
                snprintf(found, sizeof found, "%s", p);
            return found;
        }
    }
    snprintf(found, sizeof found, "%s\\game", dw_exe_dir());
    return found;
}

const char *dw_save_dir(void)
{
    static char def[DW_PATH_MAX];
    if (g_dw.save_dir[0])
        return g_dw.save_dir;
    snprintf(def, sizeof def, "%s\\saves", dw_exe_dir());
    return def;
}

/* ── defaults, load, save ────────────────────────────────────────── */

static const struct { const char *name; size_t off; int lo, hi; } s_ints[] = {
#define F(n, lo, hi) { #n, offsetof(DwSettings, n), lo, hi }
    F(skip_launcher, 0, 1), F(show_fps, 0, 1),
    F(fullscreen, 0, 1), F(win_w, 0, 16384), F(win_h, 0, 16384),
    F(res_scale, 0, 4), F(interp, 0, 1), F(vsync, 0, 1), F(fps_cap, 0, 1000),
    F(aspect, 0, 1), F(gpu, -1, 15),
    F(mkb, 0, 1), F(mouse_sens, 10, 500), F(invert_y, 0, 1), F(rumble, 0, 1),
    F(volume, 0, 100), F(mute_unfocused, 0, 1),
    F(kick_lag, 0, 4), F(xf_threads, -1, 16), F(log_console, 0, 1),
#undef F
};

static char *trim(char *p)
{
    char *e;
    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return p;
}

void dw_settings_load(void)
{
    FILE *f = fopen(dw_settings_path(), "r");
    char line[1024];
    size_t i;

    dw_settings_defaults(&g_dw);
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *k, *v;
        if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        *eq = 0;
        k = trim(line);
        v = trim(eq + 1);
        if (!strcmp(k, "game_dir")) {
            snprintf(g_dw.game_dir, sizeof g_dw.game_dir, "%s", v);
            continue;
        }
        if (!strcmp(k, "save_dir")) {
            snprintf(g_dw.save_dir, sizeof g_dw.save_dir, "%s", v);
            continue;
        }
        if (!strncmp(k, "key.", 4)) {
            int a;
            for (a = 0; a < dw_action_count; a++)
                if (!strcmp(k + 4, dw_actions[a].id)) {
                    char *comma = strchr(v, ',');
                    if (comma)
                        *comma = 0;
                    g_dw.keys[a][0] = (uint16_t)mkb_input_from_name(trim(v));
                    g_dw.keys[a][1] = comma ? (uint16_t)mkb_input_from_name(trim(comma + 1)) : 0;
                }
            continue;
        }
        if (!strncmp(k, "pad.", 4)) {
            int c = mkb_control_from_name(k + 4), p = mkb_control_from_name(v);
            if (c >= 0 && c < 16 && p >= 0 && p < 16)
                g_dw.pad_src[c] = (uint8_t)p;
            continue;
        }
        for (i = 0; i < sizeof s_ints / sizeof s_ints[0]; i++)
            if (!strcmp(k, s_ints[i].name)) {
                int x = atoi(v);
                if (x < s_ints[i].lo) x = s_ints[i].lo;
                if (x > s_ints[i].hi) x = s_ints[i].hi;
                *(int *)((char *)&g_dw + s_ints[i].off) = x;
            }
    }
    fclose(f);
}

void dw_settings_save(void)
{
    char tmp[DW_PATH_MAX + 8];
    FILE *f;
    size_t i;
    int a;

    snprintf(tmp, sizeof tmp, "%s.tmp", dw_settings_path());
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# Darkwatch PC settings. The launcher writes this file; edit it\n"
               "# by hand only while the game is closed.\n\n[general]\n");
    fprintf(f, "game_dir=%s\nsave_dir=%s\n", g_dw.game_dir, g_dw.save_dir);
    for (i = 0; i < sizeof s_ints / sizeof s_ints[0]; i++)
        fprintf(f, "%s=%d\n", s_ints[i].name,
                *(const int *)((const char *)&g_dw + s_ints[i].off));
    fprintf(f, "\n[keyboard_mouse]\n");
    for (a = 0; a < dw_action_count; a++) {
        const char *k0 = g_dw.keys[a][0] ? mkb_input_name(g_dw.keys[a][0]) : NULL;
        const char *k1 = g_dw.keys[a][1] ? mkb_input_name(g_dw.keys[a][1]) : NULL;
        fprintf(f, "key.%s=%s%s%s\n", dw_actions[a].id, k0 ? k0 : "",
                k1 ? "," : "", k1 ? k1 : "");
    }
    fprintf(f, "\n[controller]\n");
    for (i = 0; i < 16; i++)
        fprintf(f, "pad.%s=%s\n", mkb_control_name((int)i), mkb_control_name(g_dw.pad_src[i]));
    fclose(f);
    MoveFileExA(tmp, dw_settings_path(), MOVEFILE_REPLACE_EXISTING);
}

/* ── to the runtime ──────────────────────────────────────────────── */

static void env(const char *k, const char *v)
{
    _putenv_s(k, v);
}

static void envi(const char *k, int v)
{
    char b[32];
    snprintf(b, sizeof b, "%d", v);
    _putenv_s(k, b);
}

static void env_file(void)
{
    char path[DW_PATH_MAX + 16], line[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s\\env.txt", dw_exe_dir());
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *k, *v;
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        k = trim(line);
        v = trim(eq + 1);
        if (*k) {
            _putenv_s(k, v);
            fprintf(stderr, "[PC] env.txt: %s=%s\n", k, v);
        }
    }
    fclose(f);
}

void dw_settings_apply_env(int display_height)
{
    int scale = g_dw.res_scale;
    char b[32];

    if (!scale) {
        /* Enough rows to cover the display: 1080p and 1440p get 3x
         * (1920x1440), 4K gets 4x. */
        scale = (display_height + 479) / 480;
        if (scale < 2) scale = 2;
        if (scale > 4) scale = 4;
    }
    env("DW_GAME_DIR", dw_game_dir());
    env("DW_SAVE_DIR", dw_save_dir());
    /* What Darkwatch needs from the runtime (as the PS5 host sets it). */
    env("RECOMP_AC97_READY", "1");
    env("RECOMP_TITLE_KEVENTS", "1");
    env("RECOMP_PB_EXEC", "1");
    env("RECOMP_PB_WALK", "dma");
    env("RECOMP_HEAP_RECLAIM", "1");
    env("RECOMP_VBLANK", "1");
    env("RECOMP_FB_WINDOW", "1");
    env("RECOMP_STALL_SECS", "15");
    env("DW_RENDERER", "vulkan");
    envi("DW_RES_SCALE", scale);
    envi("DW_INTERP", g_dw.interp);
    envi("DW_KICK_LAG", g_dw.kick_lag);
    if (g_dw.xf_threads >= 0)
        envi("RECOMP_XF_THREADS", g_dw.xf_threads);
    if (g_dw.gpu >= 0)
        envi("DW_VK_DEVICE", g_dw.gpu);
    /* Input: keyboard and mouse through the binding table; the generic key
     * map stays off so the two do not fight over the same keys. */
    env("RECOMP_KEYBOARD", "0");
    envi("RECOMP_MKB", g_dw.mkb);
    snprintf(b, sizeof b, "%.2f", g_dw.mouse_sens / 100.0);
    env("RECOMP_MKB_SENS", b);
    envi("RECOMP_MKB_INVERT_Y", g_dw.invert_y);
    env("RECOMP_MKB_HOST_CURSOR", "1");
    env("RECOMP_MKB_INI", "-");             /* no mkb.ini: the table comes from here */
    /* Audio: a little more queue than the toolkit's 2 x 512 default; the
     * desktop's scheduler is less punctual than the console's. */
    env("RECOMP_AUDIO_BUF_SAMPLES", "1024");
    env("RECOMP_AUDIO_BUF_COUNT", "4");
    envi("RECOMP_APU_VOLUME", g_dw.volume);
    /* For bug reports: the first stream-voice waveform jumps, with the
     * methods that preceded them (apu_vp.c jump_check), go to the log. */
    env("RECOMP_APU_JUMP_TRACE", "60");
    env_file();
}

void dw_settings_apply_input(void)
{
    MkbBind b[MKB_MAX_BINDS];
    int n = 0, a, k;
    for (a = 0; a < dw_action_count; a++)
        for (k = 0; k < 2; k++)
            if (g_dw.keys[a][k] && n < MKB_MAX_BINDS) {
                b[n].input = g_dw.keys[a][k];
                b[n].control = dw_actions[a].control;
                n++;
            }
    mkb_set_binds(b, n);
    xbox_InputSetPadRemap(g_dw.pad_src);
    xbox_InputSetRumble(g_dw.rumble);
}
