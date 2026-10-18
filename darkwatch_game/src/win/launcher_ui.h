/*
 * launcher_ui.h -- the launcher's screens, independent of the platform: the
 * host feeds input and a pixel buffer, and acts on what comes back.
 */
#ifndef DW_LAUNCHER_UI_H
#define DW_LAUNCHER_UI_H

#include "ui_draw.h"

typedef struct {
    int mx, my;                 /* pointer, canvas pixels */
    int mouse_moved;
    int mdown;                  /* left button held */
    int mpress, mrelease;       /* left button edges this frame */
    int wheel;                  /* notches, + is up */
    /* navigation from keyboard or controller (edges, with repeat) */
    int up, down, left, right, ok, back, page_prev, page_next, start;
    /* while capturing a binding (lui_capture_mode) */
    int cap_input;              /* VK code or MKB_IN_*, 0 = none */
    int cap_pad;                /* physical pad control 0..15, -1 = none */
    int cap_cancel, cap_clear;
    double time;                /* seconds */
} LuiInput;

typedef struct {
    char gpu[8][256];
    int ngpu;
    int display_w, display_h, display_refresh;
    int fullscreen;             /* the window, now */
    int pad_connected;
    const char *notice;         /* a failed start's message, or NULL */
    const char *version;
    const char *log_path;
    int in_game;                /* the F1 overlay over a running game */
} LuiEnv;

enum {
    LUI_NONE, LUI_PLAY, LUI_QUIT, LUI_BROWSE_GAME, LUI_BROWSE_SAVE, LUI_OPEN_SAVE,
    LUI_OPEN_LOG, LUI_CHANGED, LUI_RESUME
};

enum { LUI_SCREEN_HOME, LUI_SCREEN_SETTINGS };
enum { LUI_PAGE_GRAPHICS, LUI_PAGE_CONTROLS, LUI_PAGE_SOUND, LUI_PAGE_GENERAL,
       LUI_PAGE_ABOUT, LUI_PAGE_COUNT };

/* Fonts: display (headings), body, bold, black. Data must stay alive. */
int  lui_init(const void *f_display, int n_display, const void *f_body, int n_body,
              const void *f_bold, int n_bold, const void *f_black, int n_black);
/* The game's last frame (0xAARRGGBB, w x h) as the overlay's backdrop. */
void lui_set_backdrop(const uint32_t *px, int w, int h);
/* Optional art (PNG paths); missing files fall back to plain colours. */
void lui_load_art(const char *bg, const char *bg_blur, const char *emblem, const char *wordmark);

/* One frame: handle input, draw, return an action (LUI_*). */
int  lui_frame(UiCanvas *c, const LuiInput *in, const LuiEnv *env);
/* The title art with "Loading...", for the moment between Play and the
 * game's first frame. */
void lui_draw_loading(UiCanvas *c);
/* 0 none, 1 a key or mouse button, 2 a controller button. */
int  lui_capture_mode(void);
/* Still animating: draw again soon even without input. */
int  lui_animating(void);
/* Jump to a screen/page (the preview tool; "Controls" on the title). */
void lui_goto(int screen, int page);
/* Preview: open the binding prompt for action a. */
void lui_preview_capture(int a);

#endif
