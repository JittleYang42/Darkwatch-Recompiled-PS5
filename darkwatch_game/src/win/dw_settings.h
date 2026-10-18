/*
 * dw_settings.h -- the PC port's settings: what the launcher edits, what
 * darkwatch.ini stores, and how they become the runtime's switches.
 */
#ifndef DW_SETTINGS_H
#define DW_SETTINGS_H

#include <stdint.h>
#include "mkb_input.h"

#define DW_PATH_MAX 520

/* One player action: what it is on the Xbox pad, and up to two keys or
 * mouse inputs (0 = unbound). */
typedef struct {
    const char *id;             /* ini name: key.<id>=... */
    const char *label;          /* as the launcher shows it */
    uint8_t control;            /* MKB_C_* */
    uint16_t def[2];            /* default inputs */
} DwAction;

extern const DwAction dw_actions[];
extern const int dw_action_count;

/* Controller rows: the pad's own controls and what they do in Darkwatch. */
typedef struct {
    uint8_t control;            /* MKB_C_A .. MKB_C_RS */
    const char *label;
} DwPadRow;
extern const DwPadRow dw_pad_rows[16];
const char *dw_pad_button_name(int phys);   /* "A", "LB", "RT", "D-Pad Up" ... */

typedef struct {
    /* general */
    char game_dir[DW_PATH_MAX];     /* "" = search beside the exe */
    char save_dir[DW_PATH_MAX];     /* "" = <exe>\saves */
    int  skip_launcher;
    int  show_fps;                  /* FPS in the window title */
    /* graphics */
    int  fullscreen;                /* borderless fullscreen */
    int  win_w, win_h;              /* windowed client size, 0 = automatic */
    int  res_scale;                 /* 0 = automatic (from the display), 1..4 */
    int  interp;                    /* frame interpolation */
    int  vsync;
    int  fps_cap;                   /* 0 = unlimited, else frames a second */
    int  aspect;                    /* 0 = 4:3, 1 = stretch */
    int  gpu;                       /* -1 = automatic, else device index */
    /* controls */
    int  mkb;                       /* keyboard & mouse on */
    int  mouse_sens;                /* percent, 10..500 */
    int  invert_y;
    uint16_t keys[64][2];           /* per dw_actions entry */
    uint8_t pad_src[16];            /* physical control behind each pad control */
    int  rumble;
    /* audio */
    int  volume;                    /* 0..100 */
    int  mute_unfocused;
    /* advanced */
    int  kick_lag;                  /* DW_KICK_LAG */
    int  xf_threads;                /* RECOMP_XF_THREADS, -1 = automatic */
    int  log_console;               /* a console window with the log */
} DwSettings;

extern DwSettings g_dw;

const char *dw_exe_dir(void);           /* without a trailing backslash */
void dw_settings_defaults(DwSettings *s);
void dw_settings_load(void);            /* darkwatch.ini beside the exe */
void dw_settings_save(void);
const char *dw_settings_path(void);

/* The resolved folders (settings, or the defaults). */
const char *dw_game_dir(void);
const char *dw_save_dir(void);
int dw_game_dir_ok(const char *dir);    /* default.xbe is there */

/* The settings as the runtime's environment variables, before boot. An
 * env.txt beside the exe (KEY=VALUE lines) is applied after them and wins. */
void dw_settings_apply_env(int display_height);

/* Push the bindings, pad map and rumble into the input layer (also while
 * the game runs). */
void dw_settings_apply_input(void);

#endif
