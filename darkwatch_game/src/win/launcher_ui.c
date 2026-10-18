/*
 * launcher_ui.c -- Darkwatch's launcher: the title screen (the game's own
 * main-menu art, with Play / Settings / Controls / Exit where the console's
 * menu sits) and the settings screens (a sidebar of pages and a panel of
 * rows), in the style of the N64 recompilations' launchers.
 *
 * Immediate mode: every frame the screen is laid out and drawn from the
 * settings, and each widget handles the input aimed at it as it is drawn.
 * Keyboard and controller focus moves spatially between the widgets the
 * last frame drew. Nothing here calls the platform: the host (launcher.c on
 * Windows, tools/launcher_preview.c anywhere) supplies input and pixels.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_ui.h"
#include "dw_settings.h"
#include "dw_vk.h"

/* ── look ────────────────────────────────────────────────────────── */

#define C_BONE      UI_RGB(236, 228, 214)
#define C_TEXT2     UI_RGB(160, 150, 138)
#define C_TEXT3     UI_RGB(112, 104, 96)
#define C_CRIMSON   UI_RGB(178, 28, 34)
#define C_CRIMSON2  UI_RGB(120, 16, 22)
#define C_CRIMSONHI UI_RGB(214, 52, 58)
#define C_PANEL     UI_RGBA(13, 11, 15, 228)
#define C_PANEL_BRD UI_RGBA(92, 30, 36, 255)
#define C_FIELD     UI_RGB(30, 25, 29)
#define C_FIELD_BRD UI_RGB(64, 50, 54)
#define C_SEP       UI_RGBA(255, 255, 255, 18)
#define C_GOOD      UI_RGB(86, 200, 120)
#define C_BAD       UI_RGB(226, 70, 70)
#define C_MOON      UI_RGB(170, 182, 222)

static UiFont *F_disp, *F_body, *F_bold, *F_black;
static UiImage I_bg, I_blur, I_emblem, I_word, I_game;

static int s_screen = LUI_SCREEN_HOME, s_page = LUI_PAGE_GRAPHICS;
static int s_ctl_tab;                   /* controls: 0 keyboard & mouse, 1 controller */
static int s_home_sel;
static int s_focus = -1, s_zone;        /* zone 0: sidebar / home menu, 1: content */
static int s_last_content_focus = -1;
static int s_active = -1;               /* slider being dragged */
static float s_scroll[LUI_PAGE_COUNT * 2], s_scroll_max;
static int s_capture, s_cap_action, s_cap_slot;
static double s_cap_t0;
static int s_ret;
static int s_anim;
static float S = 1.0f;
static const LuiInput *IP;
static const LuiEnv *ENV;
static UiCanvas *CV;
static UiRect s_content_view;           /* clip of the scrolling area */

#define U(v) ((int)((v) * S + 0.5f))
#define UF(v) ((v) * S)

/* ── focusable items of the last frame ───────────────────────────── */

typedef struct { int id, zone, adjustable; UiRect r; } Item;
static Item s_items[512], s_prev_items[512];
static int s_nitems, s_nprev;

static void item(int id, int zone, UiRect r, int adjustable)
{
    if (s_nitems < 512) {
        Item *it = &s_items[s_nitems++];
        it->id = id;
        it->zone = zone;
        it->r = r;
        it->adjustable = adjustable;
    }
}

static const Item *prev_item(int id)
{
    int i;
    for (i = 0; i < s_nprev; i++)
        if (s_prev_items[i].id == id)
            return &s_prev_items[i];
    return NULL;
}

/* Per-id highlight, eased toward 1 while focused. */
static struct { int id; float v; } s_glow[64];
static float glow(int id, int on)
{
    int i, free_i = -1;
    float target = on ? 1.0f : 0.0f;
    for (i = 0; i < 64; i++) {
        if (s_glow[i].id == id)
            break;
        if (free_i < 0 && s_glow[i].v <= 0.001f)
            free_i = i;
    }
    if (i == 64) {
        if (!on || free_i < 0)
            return 0.0f;
        i = free_i;
        s_glow[i].id = id;
        s_glow[i].v = 0.0f;
    }
    s_glow[i].v += (target - s_glow[i].v) * 0.35f;
    if (fabsf(target - s_glow[i].v) < 0.01f)
        s_glow[i].v = target;
    else
        s_anim = 1;
    return s_glow[i].v;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0;
    int i;
    if (t <= 0) return a;
    if (t >= 1) return b;
    for (i = 0; i < 32; i += 8) {
        float x = (float)((a >> i) & 255), y = (float)((b >> i) & 255);
        o |= (uint32_t)(x + (y - x) * t + 0.5f) << i;
    }
    return o;
}

/* ── mouse helpers ───────────────────────────────────────────────── */

static int hover(UiRect r)
{
    UiRect v = CV->clip;
    return ui_in(r, IP->mx, IP->my) && ui_in(v, IP->mx, IP->my) && !s_capture;
}

static int clicked(UiRect r)
{
    return IP->mrelease && hover(r);
}

/* Focus follows the mouse when it moves over an item. */
static void mouse_focus(int id, int zone, UiRect r)
{
    if (IP->mouse_moved && hover(r)) {
        s_focus = id;
        s_zone = zone;
    }
    if (IP->mpress && hover(r)) {
        s_focus = id;
        s_zone = zone;
    }
}

static int focused(int id) { return s_focus == id; }
static int activated(int id, UiRect r)
{
    return (focused(id) && IP->ok && !s_capture) || clicked(r);
}

static void changed(void)
{
    if (s_ret == LUI_NONE)
        s_ret = LUI_CHANGED;
}

/* ── text helpers ────────────────────────────────────────────────── */

static void text(UiFont *f, float px, float tr, int x, int y, uint32_t c, const char *s)
{
    ui_text(CV, f, UF(px), UF(tr), (float)x, (float)y, c, s);
}

static int text_w(UiFont *f, float px, float tr, const char *s)
{
    return (int)ceilf(ui_text_width(f, UF(px), UF(tr), s));
}

static void text_center(UiFont *f, float px, float tr, UiRect r, uint32_t c, const char *s)
{
    int w = text_w(f, px, tr, s);
    float h = ui_font_height(f, UF(px));
    text(f, px, tr, r.x + (r.w - w) / 2, r.y + (int)((r.h - h) / 2), c, s);
}

static void text_shadow(UiFont *f, float px, float tr, int x, int y, uint32_t c, const char *s)
{
    text(f, px, tr, x + U(2), y + U(2), UI_RGBA(0, 0, 0, 170), s);
    text(f, px, tr, x, y, c, s);
}

/* Friendly names for bound inputs. */
static const char *input_label(int in)
{
    static char b[4][24];
    static int k;
    static const struct { int c; const char *n; } names[] = {
        { MKB_IN_MOUSE1, "Left Mouse" }, { MKB_IN_MOUSE2, "Right Mouse" },
        { MKB_IN_MOUSE3, "Middle Mouse" }, { MKB_IN_MOUSE4, "Mouse 4" },
        { MKB_IN_MOUSE5, "Mouse 5" }, { MKB_IN_WHEELUP, "Wheel Up" },
        { MKB_IN_WHEELDOWN, "Wheel Down" },
        { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_ESCAPE, "Esc" },
        { VK_TAB, "Tab" }, { VK_BACK, "Backspace" }, { VK_CAPITAL, "Caps Lock" },
        { VK_LSHIFT, "Left Shift" }, { VK_RSHIFT, "Right Shift" },
        { VK_LCONTROL, "Left Ctrl" }, { VK_RCONTROL, "Right Ctrl" },
        { VK_LMENU, "Left Alt" }, { VK_RMENU, "Right Alt" },
        { VK_UP, "Up Arrow" }, { VK_DOWN, "Down Arrow" }, { VK_LEFT, "Left Arrow" },
        { VK_RIGHT, "Right Arrow" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" },
        { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "Page Up" }, { VK_NEXT, "Page Down" },
        { VK_OEM_COMMA, "," }, { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" }, { VK_OEM_1, ";" },
        { VK_OEM_7, "'" }, { VK_OEM_4, "[" }, { VK_OEM_6, "]" }, { VK_OEM_5, "\\" },
        { VK_OEM_MINUS, "-" }, { VK_OEM_PLUS, "=" }, { VK_OEM_3, "`" },
    };
    size_t i;
    if (!in)
        return "";
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].c == in)
            return names[i].n;
    k = (k + 1) & 3;
    if ((in >= 'A' && in <= 'Z') || (in >= '0' && in <= '9'))
        snprintf(b[k], sizeof b[k], "%c", in);
    else if (in >= VK_F1 && in <= VK_F1 + 23)
        snprintf(b[k], sizeof b[k], "F%d", in - VK_F1 + 1);
    else if (in >= VK_NUMPAD0 && in <= VK_NUMPAD0 + 9)
        snprintf(b[k], sizeof b[k], "Numpad %d", in - VK_NUMPAD0);
    else
        snprintf(b[k], sizeof b[k], "Key %02X", in);
    return b[k];
}

/* ── widgets ─────────────────────────────────────────────────────── */

/* A pill button. style 0 plain, 1 crimson (primary), 2 subtle. */
static int button(int id, int zone, UiRect r, const char *label, int style, float px)
{
    float g;
    uint32_t fill, brd, tc;
    mouse_focus(id, zone, r);
    item(id, zone, r, 0);
    g = glow(id, focused(id) || hover(r));
    if (style == 1) {
        fill = mix(C_CRIMSON, C_CRIMSONHI, g * 0.6f);
        brd = mix(UI_RGB(150, 40, 46), UI_RGB(240, 120, 120), g);
        tc = UI_RGB(255, 246, 236);
    } else {
        fill = mix(style == 2 ? UI_RGB(24, 20, 23) : C_FIELD, UI_RGB(58, 30, 34), g);
        brd = mix(C_FIELD_BRD, UI_RGB(190, 60, 66), g);
        tc = mix(UI_RGB(222, 212, 198), UI_RGB(255, 250, 244), g);
    }
    ui_fill_round(CV, r, UF(r.h * 0.22f / S), fill);
    if (style == 1)
        ui_vgradient(CV, ui_rect(r.x + U(2), r.y + U(2), r.w - U(4), r.h / 2 - U(2)),
                     UI_RGBA(255, 255, 255, 30), UI_RGBA(255, 255, 255, 0));
    ui_stroke_round(CV, r, UF(r.h * 0.22f / S), UF(1.5f + g), brd);
    text_center(F_bold, px, 1.2f, r, tc, label);
    return activated(id, r);
}

/* The right-hand control area of a settings row. */
static UiRect row_ctl(UiRect row, int w)
{
    return ui_rect(row.x + row.w - U(28) - U(w), row.y + (row.h - U(40)) / 2, U(w), U(40));
}

static void arrow(int x, int y, int sz, int dir, uint32_t c)
{
    if (dir < 0)
        ui_fill_triangle(CV, (float)x, (float)y, (float)(x + sz), (float)(y - sz),
                         (float)(x + sz), (float)(y + sz), c);
    else
        ui_fill_triangle(CV, (float)(x + sz), (float)y, (float)x, (float)(y - sz),
                         (float)x, (float)(y + sz), c);
}

/* A settings row's frame: background, label, description; returns the
 * focus glow. */
static float row_frame(int id, UiRect row, const char *label, const char *desc, int adjustable)
{
    float g;
    mouse_focus(id, 1, row);
    item(id, 1, row, adjustable);
    g = glow(id, focused(id));
    if (g > 0) {
        ui_fill_round(CV, row, UF(10), UI_RGBA(150, 30, 38, (int)(46 * g)));
        ui_fill_round(CV, ui_rect(row.x, row.y + U(10), U(4), row.h - U(20)), UF(2),
                      UI_ALPHA(C_CRIMSONHI, (int)(255 * g)));
    }
    text(F_bold, 21, 0.2f, row.x + U(26), row.y + U(desc ? 12 : (row.h / S - 26) / 2), C_BONE, label);
    if (desc)
        ui_text_wrap(CV, F_body, UF(15.5f),
                     ui_rect(row.x + U(26), row.y + U(42), row.w - U(26) - U(470), row.h),
                     1.18f, C_TEXT2, desc);
    ui_fill(CV, ui_rect(row.x + U(14), row.y + row.h - 1, row.w - U(28), 1), C_SEP);
    return g;
}

/* Option: ◀ value ▶ over n choices. Returns 1 when changed. */
static int option(int id, UiRect row, const char *label, const char *desc,
                  int *val, int n, const char *const *names)
{
    UiRect c = row_ctl(row, 360), lh, rh;
    float g = row_frame(id, row, label, desc, 1);
    int old = *val;
    lh = ui_rect(c.x, c.y, c.w / 4, c.h);
    rh = ui_rect(c.x + c.w - c.w / 4, c.y, c.w / 4, c.h);
    ui_fill_round(CV, c, UF(10), mix(C_FIELD, UI_RGB(44, 30, 34), g));
    ui_stroke_round(CV, c, UF(10), UF(1.4f), mix(C_FIELD_BRD, UI_RGB(170, 56, 62), g));
    arrow(c.x + U(16), c.y + c.h / 2, U(7), -1, hover(lh) ? C_BONE : C_CRIMSONHI);
    arrow(c.x + c.w - U(23), c.y + c.h / 2, U(7), 1, hover(rh) ? C_BONE : C_CRIMSONHI);
    {
        UiRect t = ui_rect(c.x + U(34), c.y, c.w - U(68), c.h);
        text_center(F_bold, 17, 0.3f, t, C_BONE,
                    ui_text_fit(F_bold, UF(17), UF(0.3f), (float)t.w, names[*val]));
    }
    if (clicked(lh) || (focused(id) && IP->left && !s_capture))
        *val = (*val + n - 1) % n;
    else if (clicked(rh) || clicked(ui_rect(c.x + c.w / 4, c.y, c.w / 2, c.h))
             || (focused(id) && (IP->right || IP->ok) && !s_capture))
        *val = (*val + 1) % n;
    if (*val != old) {
        changed();
        return 1;
    }
    return 0;
}

static int toggle(int id, UiRect row, const char *label, const char *desc, int *val)
{
    UiRect c = row_ctl(row, 64);
    float g = row_frame(id, row, label, desc, 1), k;
    int old = *val;
    UiRect sw = ui_rect(c.x, c.y + U(5), U(64), U(30));
    if (activated(id, row) || (focused(id) && (IP->left || IP->right) && !s_capture))
        *val = !*val;
    k = glow(id + 100000, *val);
    ui_fill_round(CV, sw, UF(15), mix(UI_RGB(58, 50, 54), C_CRIMSON, k));
    ui_stroke_round(CV, sw, UF(15), UF(1.2f), mix(UI_RGB(84, 72, 76), UI_RGB(230, 90, 96), g * 0.6f + k * 0.4f));
    ui_fill_circle(CV, sw.x + UF(15) + k * UF(34), sw.y + sw.h / 2.0f, UF(11), C_BONE);
    text(F_bold, 15, 0.5f, c.x - U(52), c.y + U(10), *val ? C_BONE : C_TEXT3, *val ? "ON" : "OFF");
    if (*val != old) {
        changed();
        return 1;
    }
    return 0;
}

/* Slider over integer positions 0..n-1; value label drawn by the caller's
 * names. Returns 1 when changed. */
static int slider(int id, UiRect row, const char *label, const char *desc, int *pos, int n,
                  const char *value_text)
{
    UiRect c = row_ctl(row, 360);
    UiRect tr = ui_rect(c.x + U(8), c.y + c.h / 2 - U(4), c.w - U(110), U(8));
    float g = row_frame(id, row, label, desc, 1), f;
    int old = *pos;
    UiRect grab = ui_rect(tr.x - U(10), c.y, tr.w + U(20), c.h);
    if (IP->mpress && hover(grab))
        s_active = id;
    if (s_active == id) {
        if (IP->mdown || IP->mpress) {
            float t = (float)(IP->mx - tr.x) / (float)tr.w;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            *pos = (int)(t * (n - 1) + 0.5f);
        } else {
            s_active = -1;
        }
    }
    if (focused(id) && !s_capture) {
        if (IP->left && *pos > 0) (*pos)--;
        if (IP->right && *pos < n - 1) (*pos)++;
    }
    f = n > 1 ? (float)*pos / (n - 1) : 0;
    ui_fill_round(CV, tr, UF(4), UI_RGB(52, 44, 48));
    ui_fill_round(CV, ui_rect(tr.x, tr.y, (int)(tr.w * f) + U(4), tr.h), UF(4),
                  mix(C_CRIMSON, C_CRIMSONHI, g * 0.5f));
    ui_fill_circle(CV, tr.x + tr.w * f, tr.y + tr.h / 2.0f, UF(11 + g * 2), C_BONE);
    ui_fill_circle(CV, tr.x + tr.w * f, tr.y + tr.h / 2.0f, UF(4), C_CRIMSON2);
    {
        int w = text_w(F_bold, 17, 0.3f, value_text);
        text(F_bold, 17, 0.3f, c.x + c.w - w, c.y + U(9), C_BONE, value_text);
    }
    if (*pos != old) {
        changed();
        return 1;
    }
    return 0;
}

/* A key-binding cell. */
static int key_cell(int id, UiRect r, const char *label, int waiting)
{
    float g;
    uint32_t fill, brd;
    mouse_focus(id, 1, r);
    item(id, 1, r, 0);
    g = glow(id, focused(id) || hover(r));
    fill = waiting ? UI_RGB(92, 22, 28) : mix(C_FIELD, UI_RGB(56, 30, 34), g);
    brd = waiting ? C_CRIMSONHI : mix(C_FIELD_BRD, UI_RGB(196, 64, 70), g);
    ui_fill_round(CV, r, UF(8), fill);
    ui_stroke_round(CV, r, UF(8), UF(1.3f), brd);
    if (waiting)
        text_center(F_bold, 15, 0.4f, r, C_BONE, "...");
    else if (label && *label)
        text_center(F_bold, 16, 0.2f, r, C_BONE, ui_text_fit(F_bold, UF(16), UF(0.2f), (float)r.w - U(12), label));
    else
        text_center(F_body, 16, 0, r, C_TEXT3, "\xE2\x80\x94");
    return activated(id, r);
}

/* ── backgrounds (cached per size) ───────────────────────────────── */

static uint32_t *s_bgcache;
static int s_bg_w, s_bg_h, s_bg_kind = -1;

static void draw_home_background(UiCanvas *c)
{
    /* The art at full height, centred; the sides from its blurred copy, so
     * a wide window keeps the whole picture (and its wordmark). */
    int H = c->h, W = c->w;
    if (I_blur.px)
        ui_image_cover(c, &I_blur, ui_rect(0, 0, W, H), 255);
    else
        ui_vgradient(c, ui_rect(0, 0, W, H), UI_RGB(16, 18, 30), UI_RGB(4, 4, 8));
    if (I_bg.px) {
        int iw = H * I_bg.w / I_bg.h, ix = (W - iw) / 2, fade = iw / 10, x;
        if (iw >= W) {
            ui_image_cover(c, &I_bg, ui_rect(0, 0, W, H), 255);
        } else {
            ui_image(c, &I_bg, 0, 0, I_bg.w, I_bg.h, ui_rect(ix, 0, iw, H), 255);
            /* soft seams */
            for (x = 0; x < fade; x++) {
                int a = 255 - x * 255 / fade;
                UiRect l = ui_rect(ix + x, 0, 1, H), r = ui_rect(ix + iw - 1 - x, 0, 1, H);
                ui_set_clip(c, l);
                ui_image_cover(c, &I_blur, ui_rect(0, 0, W, H), a);
                ui_set_clip(c, r);
                ui_image_cover(c, &I_blur, ui_rect(0, 0, W, H), a);
            }
            ui_reset_clip(c);
        }
    }
    /* darker toward the bottom right, where the menu sits */
    ui_vgradient(c, ui_rect(0, H * 5 / 10, W, H - H * 5 / 10), UI_RGBA(0, 0, 0, 0), UI_RGBA(0, 0, 0, 150));
    ui_hgradient(c, ui_rect(W / 2, 0, W - W / 2, H), UI_RGBA(0, 0, 0, 0), UI_RGBA(0, 0, 0, 90));
}

static void settings_layout(int W, int H, UiRect *side, UiRect *main)
{
    int m = U(36);
    *side = ui_rect(m, m, U(330), H - 2 * m);
    *main = ui_rect(side->x + side->w + U(28), m, W - side->w - U(28) - 2 * m, H - 2 * m);
}

static int sidebar_art(UiCanvas *c, UiRect p, int draw);

static void panel(UiCanvas *c, UiRect p)
{
    ui_shadow(c, p, UF(18), U(14), UI_RGBA(0, 0, 0, 160));
    ui_fill_round(c, p, UF(18), C_PANEL);
    ui_stroke_round(c, p, UF(18), UF(1.5f), C_PANEL_BRD);
}

static void draw_settings_background(UiCanvas *c, int in_game)
{
    if (in_game && I_game.px) {
        /* the game, stopped behind the panels */
        ui_image_cover(c, &I_game, ui_rect(0, 0, c->w, c->h), 255);
        ui_fill(c, ui_rect(0, 0, c->w, c->h), UI_RGBA(4, 2, 6, 120));
    } else if (I_blur.px)
        ui_image_cover(c, &I_blur, ui_rect(0, 0, c->w, c->h), 255);
    else
        ui_vgradient(c, ui_rect(0, 0, c->w, c->h), UI_RGB(18, 16, 24), UI_RGB(5, 4, 7));
    ui_fill(c, ui_rect(0, 0, c->w, c->h), UI_RGBA(6, 4, 8, 90));
    ui_vgradient(c, ui_rect(0, 0, c->w, c->h / 3), UI_RGBA(60, 8, 12, 50), UI_RGBA(0, 0, 0, 0));
    {
        /* the two panels and the sidebar's art never change: drawn once */
        UiRect side, main;
        UiCanvas *keep = CV;
        settings_layout(c->w, c->h, &side, &main);
        panel(c, side);
        panel(c, main);
        CV = c;
        sidebar_art(c, side, 1);
        CV = keep;
    }
}

static void background(UiCanvas *c, int kind)
{
    size_t n = (size_t)c->w * c->h;
    if (!s_bgcache || s_bg_w != c->w || s_bg_h != c->h || s_bg_kind != kind) {
        UiCanvas t;
        free(s_bgcache);
        s_bgcache = (uint32_t *)malloc(n * 4);
        if (!s_bgcache) {
            ui_clear(c, 0);
            return;
        }
        ui_canvas_init(&t, s_bgcache, c->w, c->h);
        ui_clear(&t, 0);
        if (kind == 0)
            draw_home_background(&t);
        else
            draw_settings_background(&t, kind == 2);
        s_bg_w = c->w;
        s_bg_h = c->h;
        s_bg_kind = kind;
    }
    memcpy(c->px, s_bgcache, n * 4);
}

/* ── the title screen ────────────────────────────────────────────── */

static void a_button_glyph(int x, int y, int r, uint32_t col, const char *l)
{
    ui_fill_circle(CV, (float)x, (float)y, (float)r, col);
    ui_fill_circle(CV, (float)x, (float)y, (float)r - UF(1.6f), UI_RGB(18, 18, 18));
    text_center(F_black, r * 1.25f / S, 0, ui_rect(x - r, y - r, 2 * r, 2 * r), col, l);
}

static void home_screen(void)
{
    static const char *const items[] = { "PLAY", "SETTINGS", "CONTROLS", "EXIT" };
    int W = CV->w, H = CV->h, iw = H * 4 / 3 < W ? H * 4 / 3 : W, ix = (W - iw) / 2;
    int x = ix + iw * 64 / 100, y = H * 61 / 100, i;
    float px = H * 0.047f / S;
    int lh = (int)(H * 0.066f);
    char line[512];
    int ok = dw_game_dir_ok(dw_game_dir());

    background(CV, 0);
    if (s_zone != 0 || s_focus < 1 || s_focus > 4)
        s_focus = 1 + s_home_sel, s_zone = 0;
    for (i = 0; i < 4; i++) {
        int id = 1 + i;
        int w = text_w(F_black, px, 1.5f, items[i]);
        UiRect r = ui_rect(x - U(14), y + i * lh - U(4), w + U(28), lh);
        float g;
        mouse_focus(id, 0, r);
        item(id, 0, r, 0);
        g = glow(id, focused(id));
        if (g > 0.01f) {
            /* a smouldering bar behind the chosen item */
            UiRect gb = ui_rect(x - U(40), y + i * lh - U(2), w + U(160), lh - U(10));
            ui_hgradient(CV, ui_rect(gb.x, gb.y, gb.w / 3, gb.h), UI_RGBA(120, 10, 16, 0),
                         UI_RGBA(120, 10, 16, (int)(150 * g)));
            ui_hgradient(CV, ui_rect(gb.x + gb.w / 3, gb.y, gb.w - gb.w / 3, gb.h),
                         UI_RGBA(120, 10, 16, (int)(150 * g)), UI_RGBA(120, 10, 16, 0));
            ui_fill_triangle(CV, (float)(x - U(26)), (float)(y + i * lh + lh / 2 - U(14)),
                             (float)(x - U(26)), (float)(y + i * lh + lh / 2 + U(2)),
                             (float)(x - U(16)), (float)(y + i * lh + lh / 2 - U(6)),
                             UI_ALPHA(C_CRIMSONHI, (int)(255 * g)));
        }
        text_shadow(F_black, px, 1.5f, x + (int)(UF(6) * g), y + i * lh,
                    mix(UI_RGB(206, 206, 210), UI_RGB(214, 40, 46), g), items[i]);
        if (focused(id))
            s_home_sel = i;
        if (activated(id, r)) {
            if (i == 0) s_ret = ok ? LUI_PLAY : s_ret;
            if (i == 0 && !ok) { s_screen = LUI_SCREEN_SETTINGS; s_page = LUI_PAGE_GENERAL; s_focus = -1; }
            if (i == 1) { s_screen = LUI_SCREEN_SETTINGS; s_page = LUI_PAGE_GRAPHICS; s_focus = -1; }
            if (i == 2) { s_screen = LUI_SCREEN_SETTINGS; s_page = LUI_PAGE_CONTROLS; s_focus = -1; }
            if (i == 3) s_ret = LUI_QUIT;
        }
    }
    if (IP->start && ok)
        s_ret = LUI_PLAY;
    /* the prompt, where the console shows its own */
    {
        int r = (int)(H * 0.017f), gx = ix + iw * 82 / 100, gy = H * 90 / 100;
        a_button_glyph(gx, gy, r, UI_RGB(110, 200, 80), "A");
        text_shadow(F_black, H * 0.024f / S, 1.0f, gx + r + U(10), gy - (int)(H * 0.016f), UI_RGB(214, 214, 218), "SELECT");
    }
    /* status, bottom left */
    {
        int sx = U(28), sy = H - U(74);
        ui_fill_circle(CV, (float)sx + UF(6), (float)sy + UF(10), UF(5), ok ? C_GOOD : C_BAD);
        snprintf(line, sizeof line, ok ? "Game data: %s" : "Game data not found (%s) - open Settings > General",
                 dw_game_dir());
        text_shadow(F_body, 15, 0, sx + U(20), sy, ok ? UI_RGB(200, 200, 205) : UI_RGB(255, 140, 140),
                    ui_text_fit(F_body, UF(15), 0, (float)(iw * 6 / 10), line));
        if (ENV->ngpu > 0)
            snprintf(line, sizeof line, "%s  -  %dx%d @ %d Hz  -  %s", ENV->gpu[g_dw.gpu >= 0 && g_dw.gpu < ENV->ngpu ? g_dw.gpu : 0],
                     ENV->display_w, ENV->display_h, ENV->display_refresh, ENV->version ? ENV->version : "");
        else
            snprintf(line, sizeof line, "No Vulkan driver found  -  %s", ENV->version ? ENV->version : "");
        text_shadow(F_body, 14, 0, sx + U(20), sy + U(26), UI_RGB(150, 150, 160), line);
    }
    if (ENV->notice && *ENV->notice) {
        UiRect n = ui_rect(W / 2 - U(380), U(24), U(760), U(110));
        ui_fill_round(CV, n, UF(12), UI_RGBA(60, 8, 12, 235));
        ui_stroke_round(CV, n, UF(12), UF(1.5f), C_CRIMSONHI);
        ui_text_wrap(CV, F_body, UF(16), ui_inset(n, U(18)), 1.2f, C_BONE, ENV->notice);
    }
}

/* ── settings: sidebar ───────────────────────────────────────────── */

static const char *const s_page_names[LUI_PAGE_COUNT] = {
    "GRAPHICS", "CONTROLS", "SOUND", "GENERAL", "ABOUT"
};

/* The emblem, wordmark and tag line; returns the y below them. */
static int sidebar_art(UiCanvas *c, UiRect p, int draw)
{
    int y = p.y + U(26);
    if (I_emblem.px) {
        int eh = U(150), ew = eh * I_emblem.w / I_emblem.h;
        if (draw)
            ui_image(c, &I_emblem, 0, 0, I_emblem.w, I_emblem.h, ui_rect(p.x + (p.w - ew) / 2, y, ew, eh), 255);
        y += eh + U(10);
    }
    if (I_word.px) {
        int ww = p.w - U(70), wh = ww * I_word.h / I_word.w;
        if (draw)
            ui_image(c, &I_word, 0, 0, I_word.w, I_word.h, ui_rect(p.x + (p.w - ww) / 2, y, ww, wh), 255);
        y += wh + U(4);
    } else {
        if (draw)
            text_center(F_disp, 34, 2, ui_rect(p.x, y, p.w, U(44)), C_BONE, "DARKWATCH");
        y += U(46);
    }
    if (draw)
        text_center(F_bold, 12.5f, 4.0f, ui_rect(p.x, y, p.w, U(20)), C_CRIMSONHI, "RECOMPILED  FOR  PC");
    return y + U(40);
}

static void sidebar(UiRect p)
{
    int i, y, bw = p.w - U(48), bx = p.x + U(24), bh = U(50);
    y = sidebar_art(CV, p, 0);
    for (i = 0; i < LUI_PAGE_COUNT; i++) {
        UiRect r = ui_rect(bx, y, bw, bh);
        int id = 10 + i, sel = s_page == i;
        float g;
        mouse_focus(id, 0, r);
        item(id, 0, r, 0);
        g = glow(id, focused(id) || hover(r));
        if (sel) {
            ui_fill_round(CV, r, UF(11), C_CRIMSON);
            ui_vgradient(CV, ui_rect(r.x + U(2), r.y + U(2), r.w - U(4), r.h / 2), UI_RGBA(255, 255, 255, 26), UI_RGBA(255, 255, 255, 0));
            ui_stroke_round(CV, r, UF(11), UF(1.5f + g), mix(UI_RGB(220, 90, 96), UI_RGB(255, 200, 200), g));
        } else {
            ui_fill_round(CV, r, UF(11), mix(UI_RGB(28, 23, 26), UI_RGB(48, 30, 34), g));
            ui_stroke_round(CV, r, UF(11), UF(1.3f), mix(UI_RGB(62, 48, 52), UI_RGB(190, 64, 70), g));
        }
        text_center(F_black, 17, 2.4f, r, sel ? UI_RGB(255, 246, 236) : mix(UI_RGB(214, 204, 190), UI_RGB(255, 255, 255), g),
                    s_page_names[i]);
        if (activated(id, r) && s_page != i) {
            s_page = i;
            s_active = -1;
        }
        y += bh + U(10);
    }
    y += U(8);
    ui_fill(CV, ui_rect(bx, y, bw, 1), UI_RGBA(255, 255, 255, 24));
    y += U(18);
    if (ENV->in_game) {
        if (button(20, 0, ui_rect(bx, y, bw, U(58)), "RESUME", 1, 22))
            s_ret = LUI_RESUME;
        y += U(58) + U(10);
        if (button(21, 0, ui_rect(bx, y, bw, U(46)), "QUIT TO DESKTOP", 0, 15))
            s_ret = LUI_QUIT;
        y += U(56);
        text_center(F_body, 13.5f, 0, ui_rect(bx, y, bw, U(20)), C_TEXT3, "F1 or Esc closes this");
    } else {
        int ok = dw_game_dir_ok(dw_game_dir());
        UiRect r = ui_rect(bx, y, bw, U(58));
        if (button(20, 0, r, ok ? "PLAY" : "PLAY  (NO GAME DATA)", 1, ok ? 22 : 15)) {
            if (ok)
                s_ret = LUI_PLAY;
            else {
                s_page = LUI_PAGE_GENERAL;
                s_focus = -1;
            }
        }
        y += U(58) + U(10);
        if (button(21, 0, ui_rect(bx, y, bw, U(46)), "BACK TO TITLE", 0, 15)) {
            s_screen = LUI_SCREEN_HOME;
            s_focus = -1;
        }
    }
}

/* ── settings: pages ─────────────────────────────────────────────── */

static int s_row_y;                     /* layout cursor inside the content */
static UiRect s_content;                /* the scrolling area */

static UiRect next_row(int h)
{
    UiRect r = ui_rect(s_content.x, s_row_y, s_content.w, U(h));
    s_row_y += U(h);
    return r;
}

static void section(const char *title)
{
    s_row_y += U(18);
    text(F_black, 13.5f, 3.0f, s_content.x + U(26), s_row_y, C_CRIMSONHI, title);
    s_row_y += U(30);
}

/* A description, plus a note when the change waits for a restart. */
static const char *later(const char *desc)
{
    static char buf[4][512];
    static int k;
    if (!ENV->in_game)
        return desc;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "%s  Takes effect the next time the game starts.", desc);
    return buf[k];
}

static const int s_fps_steps[] = { 30, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 280, 300, 360, 0 };
#define FPS_STEPS ((int)(sizeof s_fps_steps / sizeof s_fps_steps[0]))

static void page_graphics(void)
{
    static const char *const modes[] = { "Fullscreen (borderless)", "Windowed" };
    static const char *const aspects[] = { "4:3 (original)", "Stretch to window" };
    char autoname[64], val[64], gdesc[256];
    const char *scales[5];
    const char *gpus[9];
    int v, i, pos;

    snprintf(autoname, sizeof autoname, "Auto (%dx for %dp)",
             ENV->display_h > 0 ? ((ENV->display_h + 479) / 480 < 2 ? 2 : (ENV->display_h + 479) / 480 > 4 ? 4 : (ENV->display_h + 479) / 480) : 3,
             ENV->display_h);
    scales[0] = autoname;
    scales[1] = "1x  640 x 480 (original)";
    scales[2] = "2x  1280 x 960";
    scales[3] = "3x  1920 x 1440";
    scales[4] = "4x  2560 x 1920";

    section("DISPLAY");
    v = g_dw.fullscreen ? 0 : 1;
    if (option(100, next_row(84), "Display mode",
               "F11 or Alt+Enter switches at any time, in game too.", &v, 2, modes))
        g_dw.fullscreen = v == 0;
    option(101, next_row(84), "Internal resolution",
           later("The 3D is drawn at this multiple of the console's 640 x 480, then scaled to the window: "
                 "sharper edges and textures."), &g_dw.res_scale, 5, scales);
    v = g_dw.aspect;
    option(102, next_row(84), "Aspect ratio", "The game is 4:3. Stretching fills a widescreen display.",
           &g_dw.aspect, 2, aspects);
    (void)v;

    section("SMOOTHNESS");
    toggle(103, next_row(84), "Frame interpolation",
           later("Draws in-between frames so motion is smooth at your display's refresh rate. "
                 "The game itself keeps its original speed."), &g_dw.interp);
    pos = FPS_STEPS - 1;
    for (i = 0; i < FPS_STEPS; i++)
        if (s_fps_steps[i] == g_dw.fps_cap)
            pos = i;
    if (g_dw.fps_cap)
        snprintf(val, sizeof val, "%d fps", g_dw.fps_cap);
    else
        snprintf(val, sizeof val, "Unlimited");
    if (slider(104, next_row(84), "Frame rate limit",
               g_dw.vsync ? "With V-Sync on, Unlimited means one frame per display refresh."
                          : "Without V-Sync, Unlimited draws as many frames as the PC can.",
               &pos, FPS_STEPS, val))
        g_dw.fps_cap = s_fps_steps[pos];
    snprintf(gdesc, sizeof gdesc, "Waits for the display (%d Hz): no tearing. Off shows each frame as soon as "
             "it is ready.", ENV->display_refresh);
    toggle(105, next_row(84), "V-Sync", gdesc, &g_dw.vsync);

    section("HARDWARE");
    gpus[0] = "Automatic (fastest)";
    for (i = 0; i < ENV->ngpu && i < 8; i++)
        gpus[i + 1] = ENV->gpu[i];
    v = g_dw.gpu + 1;
    if (v < 0 || v > ENV->ngpu) v = 0;
    if (option(106, next_row(84), "Graphics card", later("Which GPU draws the game."), &v, ENV->ngpu + 1, gpus))
        g_dw.gpu = v - 1;
    toggle(107, next_row(84), "Frame rate in the title bar",
           "Windowed mode: shows the displayed and the game's frame rate.", &g_dw.show_fps);
}

static void page_sound(void)
{
    char val[32];
    int pos = g_dw.volume / 5;
    section("AUDIO");
    snprintf(val, sizeof val, "%d %%", g_dw.volume);
    if (slider(300, next_row(84), "Volume", "The game's whole output.", &pos, 21, val))
        g_dw.volume = pos * 5;
    toggle(301, next_row(84), "Mute in the background",
           "Silence the game while another window has the focus.", &g_dw.mute_unfocused);
}

static void path_row(int id, int btn_id, const char *label, const char *path, int ok_state,
                     int action, int open_id, int open_action)
{
    UiRect row = next_row(96), b;
    float g = row_frame(id, row, label, "", 0);
    (void)g;
    if (ok_state >= 0)
        ui_fill_circle(CV, row.x + UF(32), row.y + UF(58), UF(5), ok_state ? C_GOOD : C_BAD);
    text(F_body, 15.5f, 0, row.x + U(ok_state >= 0 ? 46 : 26), row.y + U(48),
         ok_state == 0 ? UI_RGB(255, 140, 140) : C_TEXT2,
         ui_text_fit(F_body, UF(15.5f), 0, (float)(row.w - U(420)), path));
    b = row_ctl(row, open_id ? 280 : 150);
    if (open_id) {
        UiRect o = ui_rect(b.x, b.y, U(120), b.h);
        if (button(open_id, 1, o, "Open", 0, 15))
            s_ret = open_action;
        b.x += U(130);
        b.w = U(150);
    }
    if (button(btn_id, 1, b, "Browse...", 0, 15) || (focused(id) && IP->ok))
        s_ret = action;
}

static void page_general(void)
{
    static const char *const lag[] = { "Off", "1 frame (recommended)", "2 frames" };
    static const char *const thr[] = { "Automatic", "0 (executor only)", "1", "2", "3", "4", "6", "8" };
    static const int thr_v[] = { -1, 0, 1, 2, 3, 4, 6, 8 };
    int v, i;
    int ok = dw_game_dir_ok(dw_game_dir());

    section("FOLDERS");
    path_row(400, 401, "Game folder (default.xbe and the .pck files)", dw_game_dir(), ok,
             LUI_BROWSE_GAME, 0, 0);
    path_row(402, 403, "Save folder", dw_save_dir(), -1, LUI_BROWSE_SAVE, 404, LUI_OPEN_SAVE);

    section("STARTUP");
    toggle(405, next_row(84), "Skip this launcher",
           "Start straight into the game. Hold Shift while starting to see the launcher again.",
           &g_dw.skip_launcher);
    toggle(406, next_row(84), "Log console", later("Opens a console window with the runtime's log (for bug reports)."),
           &g_dw.log_console);

    section("ADVANCED");
    if (option(407, next_row(84), "CPU / GPU overlap",
               later("Lets the game prepare the next frame while the last one draws."), &g_dw.kick_lag, 3, lag))
        ;
    v = 0;
    for (i = 0; i < 8; i++)
        if (thr_v[i] == g_dw.xf_threads)
            v = i;
    if (option(408, next_row(84), "Vertex threads", later("Helper threads that run the console's vertex programs."),
               &v, 8, thr))
        g_dw.xf_threads = thr_v[v];
    {
        UiRect row = next_row(84), b;
        row_frame(409, row, "Log file", ENV->log_path ? ENV->log_path : "darkwatch-log.txt", 0);
        b = row_ctl(row, 150);
        if (button(410, 1, b, "Open", 0, 15) || (focused(409) && IP->ok))
            s_ret = LUI_OPEN_LOG;
    }
    {
        UiRect row = next_row(84), b;
        row_frame(411, row, "Reset all settings", "Back to the defaults, key bindings included.", 0);
        b = row_ctl(row, 150);
        if (button(412, 1, b, "Reset", 0, 15) || (focused(411) && IP->ok)) {
            char gd[DW_PATH_MAX], sd[DW_PATH_MAX];
            memcpy(gd, g_dw.game_dir, sizeof gd);
            memcpy(sd, g_dw.save_dir, sizeof sd);
            dw_settings_defaults(&g_dw);
            memcpy(g_dw.game_dir, gd, sizeof gd);
            memcpy(g_dw.save_dir, sd, sizeof sd);
            changed();
        }
    }
}

static void keys_table(void)
{
    int a, k;
    UiRect hdr = next_row(36);
    int cw = U(190), c1 = hdr.x + hdr.w - U(28) - 2 * cw - U(12), c2 = c1 + cw + U(12);
    text(F_black, 12.5f, 2.5f, hdr.x + U(26), hdr.y + U(10), C_TEXT3, "ACTION");
    text(F_black, 12.5f, 2.5f, c1 + U(8), hdr.y + U(10), C_TEXT3, "PRIMARY");
    text(F_black, 12.5f, 2.5f, c2 + U(8), hdr.y + U(10), C_TEXT3, "SECONDARY");
    for (a = 0; a < dw_action_count; a++) {
        UiRect row = next_row(52);
        int rid = 1000 + a * 4;
        if ((a & 1) == 0)
            ui_fill_round(CV, ui_rect(row.x + U(10), row.y + U(2), row.w - U(20), row.h - U(4)), UF(8),
                          UI_RGBA(255, 255, 255, 7));
        text(F_body, 17, 0, row.x + U(26), row.y + U(15), C_BONE, dw_actions[a].label);
        for (k = 0; k < 2; k++) {
            UiRect cell = ui_rect(k ? c2 : c1, row.y + U(7), cw, row.h - U(14));
            int waiting = s_capture == 1 && s_cap_action == a && s_cap_slot == k;
            if (key_cell(rid + k, cell, input_label(g_dw.keys[a][k]), waiting)) {
                s_capture = 1;
                s_cap_action = a;
                s_cap_slot = k;
                s_cap_t0 = IP->time;
            }
        }
    }
}

static void pad_table(void)
{
    int i;
    UiRect hdr = next_row(36);
    int cw = U(250), c1 = hdr.x + hdr.w - U(28) - cw;
    text(F_black, 12.5f, 2.5f, hdr.x + U(26), hdr.y + U(10), C_TEXT3, "ACTION");
    text(F_black, 12.5f, 2.5f, c1 + U(8), hdr.y + U(10), C_TEXT3, "CONTROLLER BUTTON");
    for (i = 0; i < 16; i++) {
        const DwPadRow *pr = &dw_pad_rows[i];
        UiRect row = next_row(52), cell = ui_rect(c1, row.y + U(7), cw, row.h - U(14));
        int waiting = s_capture == 2 && s_cap_action == i;
        if ((i & 1) == 0)
            ui_fill_round(CV, ui_rect(row.x + U(10), row.y + U(2), row.w - U(20), row.h - U(4)), UF(8),
                          UI_RGBA(255, 255, 255, 7));
        text(F_body, 17, 0, row.x + U(26), row.y + U(15), C_BONE, pr->label);
        if (key_cell(2000 + i, cell, dw_pad_button_name(g_dw.pad_src[pr->control]), waiting)) {
            s_capture = 2;
            s_cap_action = i;
            s_cap_slot = 0;
            s_cap_t0 = IP->time;
        }
    }
}

static void page_controls(void)
{
    static const char *const tabs[] = { "KEYBOARD & MOUSE", "CONTROLLER" };
    char val[32];
    int i, pos;
    {
        /* the two tabs */
        UiRect bar = next_row(64);
        int tw = U(250);
        for (i = 0; i < 2; i++) {
            UiRect r = ui_rect(bar.x + U(26) + i * (tw + U(10)), bar.y + U(12), tw, U(44));
            int id = 200 + i;
            float g;
            mouse_focus(id, 1, r);
            item(id, 1, r, 0);
            g = glow(id, focused(id) || hover(r));
            ui_fill_round(CV, r, UF(10), s_ctl_tab == i ? C_CRIMSON : mix(UI_RGB(28, 23, 26), UI_RGB(48, 30, 34), g));
            ui_stroke_round(CV, r, UF(10), UF(1.3f + g), s_ctl_tab == i ? UI_RGB(230, 100, 106)
                            : mix(UI_RGB(62, 48, 52), UI_RGB(190, 64, 70), g));
            text_center(F_black, 14.5f, 2.0f, r, s_ctl_tab == i ? UI_RGB(255, 246, 236) : C_BONE, tabs[i]);
            if (activated(id, r))
                s_ctl_tab = i;
        }
    }
    if (s_ctl_tab == 0) {
        section("MOUSE");
        toggle(210, next_row(84), "Keyboard & mouse",
               "Play with the keys below and mouse aim. F9 frees the mouse while the game runs.", &g_dw.mkb);
        pos = (g_dw.mouse_sens - 10) / 5;
        snprintf(val, sizeof val, "%.2fx", g_dw.mouse_sens / 100.0);
        if (slider(211, next_row(84), "Mouse sensitivity", "How far the view turns for the same mouse movement.",
                   &pos, 99, val))
            g_dw.mouse_sens = 10 + pos * 5;
        toggle(212, next_row(84), "Invert mouse Y", "Push the mouse forward to look down.", &g_dw.invert_y);
        section("KEY BINDINGS  -  CLICK A KEY TO CHANGE IT");
        keys_table();
        {
            UiRect row = next_row(84), b;
            row_frame(213, row, "Reset key bindings", "Back to the default keyboard and mouse layout.", 0);
            b = row_ctl(row, 150);
            if (button(214, 1, b, "Reset", 0, 15) || (focused(213) && IP->ok)) {
                DwSettings d;
                dw_settings_defaults(&d);
                memcpy(g_dw.keys, d.keys, sizeof d.keys);
                g_dw.mouse_sens = d.mouse_sens;
                g_dw.invert_y = d.invert_y;
                changed();
            }
        }
    } else {
        section("CONTROLLER");
        {
            UiRect row = next_row(84);
            row_frame(220, row, "Controller", ENV->pad_connected
                      ? "An XInput controller is connected (Xbox, or another pad through Steam / DS4Windows)."
                      : "No XInput controller found. Xbox controllers work as they are; PlayStation pads through "
                        "Steam Input or DS4Windows.", 0);
            ui_fill_circle(CV, row.x + row.w - UF(46), row.y + row.h / 2.0f, UF(7),
                           ENV->pad_connected ? C_GOOD : UI_RGB(90, 80, 84));
        }
        toggle(221, next_row(84), "Vibration", "Rumble when the game asks for it.", &g_dw.rumble);
        section("BUTTONS  -  CLICK ONE TO CHANGE IT");
        pad_table();
        {
            UiRect row = next_row(84), b;
            row_frame(222, row, "Reset controller layout", "Back to the console's own layout.", 0);
            b = row_ctl(row, 150);
            if (button(223, 1, b, "Reset", 0, 15) || (focused(222) && IP->ok)) {
                for (i = 0; i < 16; i++)
                    g_dw.pad_src[i] = (uint8_t)i;
                changed();
            }
        }
    }
}

static void page_about(void)
{
    static const char *const lines[] = {
        "Darkwatch (Xbox, 2005) running natively on Windows: the game's own code, translated "
        "instruction by instruction into C and compiled for the PC. Nothing is emulated at the CPU "
        "level; the console's graphics chip is replaced by a Vulkan renderer and its sound chip by a "
        "software model.",
        "You need your own copy of the game: default.xbe and the .pck files from the disc, in the game "
        "folder (Settings > General).",
        "In game:  F11 or Alt+Enter  fullscreen / window   -   F9  free the mouse   -   Alt+F4  quit.",
        "The picture is drawn at a higher internal resolution and, with frame interpolation, shown at "
        "your display's refresh rate while the game logic keeps its original timing.",
        "Built on the xboxrecomp toolkit, with sound emulation from xemu (MCPX APU) and fonts Cinzel "
        "and Lato (SIL Open Font License). Darkwatch is a trademark of its owners; this project is "
        "not affiliated with them.",
    };
    int i;
    section("ABOUT");
    for (i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
        int h = ui_text_wrap_height(F_body, UF(17), s_content.w - U(60), 1.3f, lines[i]);
        ui_text_wrap(CV, F_body, UF(17), ui_rect(s_content.x + U(26), s_row_y, s_content.w - U(60), h),
                     1.3f, i == 2 ? C_BONE : C_TEXT2, lines[i]);
        s_row_y += h + U(16);
    }
    if (ENV->version) {
        text(F_bold, 14, 1.0f, s_content.x + U(26), s_row_y + U(8), C_TEXT3, ENV->version);
        s_row_y += U(40);
    }
}

static const char *const s_page_sub[LUI_PAGE_COUNT] = {
    "Resolution, smoothness and display.",
    "Keyboard, mouse and controller layouts.",
    "Volume.",
    "Folders, startup and advanced options.",
    "What this is.",
};

static void content(UiRect p)
{
    float *scroll = &s_scroll[s_page * 2 + (s_page == LUI_PAGE_CONTROLS ? s_ctl_tab : 0)];
    int top, view_h, total;
    /* heading */
    {
        static const char *const titles[LUI_PAGE_COUNT] = { "Graphics", "Controls", "Sound", "General", "About" };
        text(F_disp, 50, 1.0f, p.x + U(40), p.y + U(26), C_BONE, titles[s_page]);
        ui_fill(CV, ui_rect(p.x + U(42), p.y + U(96), U(64), U(3)), C_CRIMSONHI);
        text(F_body, 16.5f, 0, p.x + U(42), p.y + U(108), C_TEXT2, s_page_sub[s_page]);
    }
    top = p.y + U(140);
    view_h = p.y + p.h - U(58) - top;
    s_content_view = ui_rect(p.x + U(12), top, p.w - U(24), view_h);
    s_content = ui_rect(p.x + U(14), top, p.w - U(28) - U(14), view_h);
    /* scroll by the wheel over the panel */
    if (IP->wheel && ui_in(p, IP->mx, IP->my) && !s_capture)
        *scroll -= IP->wheel * UF(90);
    if (*scroll > s_scroll_max) *scroll = s_scroll_max;
    if (*scroll < 0) *scroll = 0;
    s_row_y = top - (int)*scroll;
    ui_set_clip(CV, s_content_view);
    switch (s_page) {
    case LUI_PAGE_GRAPHICS: page_graphics(); break;
    case LUI_PAGE_CONTROLS: page_controls(); break;
    case LUI_PAGE_SOUND:    page_sound(); break;
    case LUI_PAGE_GENERAL:  page_general(); break;
    default:                page_about(); break;
    }
    ui_reset_clip(CV);
    total = s_row_y + (int)*scroll - top + U(20);
    s_scroll_max = (float)(total > view_h ? total - view_h : 0);
    /* scroll bar */
    if (s_scroll_max > 0) {
        int bh = view_h * view_h / total, by = top + (int)((view_h - bh) * (*scroll / s_scroll_max));
        ui_fill_round(CV, ui_rect(p.x + p.w - U(12), top, U(4), view_h), UF(2), UI_RGBA(255, 255, 255, 16));
        ui_fill_round(CV, ui_rect(p.x + p.w - U(12), by, U(4), bh), UF(2), UI_RGBA(214, 52, 58, 200));
        /* fades at the edges of the scrolling area */
        if (*scroll > 1)
            ui_vgradient(CV, ui_rect(p.x + U(2), top, p.w - U(4), U(24)), UI_RGBA(13, 11, 15, 220), UI_RGBA(13, 11, 15, 0));
        if (*scroll < s_scroll_max - 1)
            ui_vgradient(CV, ui_rect(p.x + U(2), top + view_h - U(24), p.w - U(4), U(24)), UI_RGBA(13, 11, 15, 0), UI_RGBA(13, 11, 15, 220));
    }
    /* the hint bar */
    {
        int y = p.y + p.h - U(44);
        ui_fill(CV, ui_rect(p.x + U(20), y - U(8), p.w - U(40), 1), C_SEP);
        text(F_body, 14, 0.2f, p.x + U(40), y + U(4), C_TEXT3,
             "Arrows / D-Pad  move      Left / Right  change      Enter / A  select      "
             "Esc / B  back      PgUp PgDn / LB RB  pages");
    }
}

/* The binding prompt over everything. */
static void capture_overlay(void)
{
    UiRect b;
    char msg[160];
    const char *what;
    double left = 6.0 - (IP->time - s_cap_t0);
    if (s_capture == 1)
        what = dw_actions[s_cap_action].label;
    else
        what = dw_pad_rows[s_cap_action].label;
    ui_fill(CV, ui_rect(0, 0, CV->w, CV->h), UI_RGBA(0, 0, 0, 150));
    b = ui_rect(CV->w / 2 - U(330), CV->h / 2 - U(110), U(660), U(220));
    ui_shadow(CV, b, UF(16), U(18), UI_RGBA(0, 0, 0, 200));
    ui_fill_round(CV, b, UF(16), UI_RGB(20, 15, 18));
    ui_stroke_round(CV, b, UF(16), UF(2), C_CRIMSONHI);
    snprintf(msg, sizeof msg, "%s", s_capture == 1 ? "Press a key or mouse button" : "Press a button on the controller");
    text_center(F_disp, 30, 0.5f, ui_rect(b.x, b.y + U(28), b.w, U(44)), C_BONE, msg);
    snprintf(msg, sizeof msg, "for  \"%s\"", what);
    text_center(F_bold, 19, 0.3f, ui_rect(b.x, b.y + U(84), b.w, U(30)), C_CRIMSONHI, msg);
    snprintf(msg, sizeof msg, "%s   -   cancels in %d s",
             s_capture == 1 ? "Esc cancels, Delete clears" : "Esc cancels", left > 0 ? (int)left + 1 : 0);
    text_center(F_body, 15.5f, 0, ui_rect(b.x, b.y + U(150), b.w, U(30)), C_TEXT2, msg);
    s_anim = 1;
}

static void capture_input(void)
{
    if (!s_capture)
        return;
    if (IP->time - s_cap_t0 > 6.0 || IP->cap_cancel) {
        s_capture = 0;
        return;
    }
    if (IP->time - s_cap_t0 < 0.2)
        return;                         /* the click that opened it */
    if (s_capture == 1) {
        int in = IP->cap_clear ? -1 : IP->cap_input;
        if (in) {
            int a, k;
            if (in > 0)                 /* one action per input */
                for (a = 0; a < dw_action_count; a++)
                    for (k = 0; k < 2; k++)
                        if (g_dw.keys[a][k] == in)
                            g_dw.keys[a][k] = 0;
            g_dw.keys[s_cap_action][s_cap_slot] = (uint16_t)(in > 0 ? in : 0);
            s_capture = 0;
            changed();
        }
    } else if (IP->cap_pad >= 0) {
        int c = dw_pad_rows[s_cap_action].control, i, old = g_dw.pad_src[c];
        for (i = 0; i < 16; i++)
            if (i != c && g_dw.pad_src[i] == IP->cap_pad)
                g_dw.pad_src[i] = (uint8_t)old;    /* swap, so nothing is lost */
        g_dw.pad_src[c] = (uint8_t)IP->cap_pad;
        s_capture = 0;
        changed();
    }
}

/* ── navigation between items ────────────────────────────────────── */

static int nearest(int dir_x, int dir_y, int zone)
{
    const Item *cur = prev_item(s_focus);
    int i, best = -1;
    float bd = 1e30f, cx, cy;
    if (!cur)
        return -1;
    cx = cur->r.x + cur->r.w * 0.5f;
    cy = cur->r.y + cur->r.h * 0.5f;
    for (i = 0; i < s_nprev; i++) {
        const Item *it = &s_prev_items[i];
        float x = it->r.x + it->r.w * 0.5f, y = it->r.y + it->r.h * 0.5f, d;
        float along, across;
        if (it->id == s_focus || it->zone != zone)
            continue;
        along = dir_y ? (y - cy) * dir_y : (x - cx) * dir_x;
        across = dir_y ? fabsf(x - cx) : fabsf(y - cy);
        if (along <= 2)
            continue;
        if (dir_x && across > cur->r.h)
            continue;                   /* left/right stays on the row */
        d = along + across * (dir_y ? 0.35f : 2.0f);
        if (d < bd) {
            bd = d;
            best = it->id;
        }
    }
    return best;
}

static void ensure_visible(int id)
{
    const Item *it = prev_item(id);
    float *scroll = &s_scroll[s_page * 2 + (s_page == LUI_PAGE_CONTROLS ? s_ctl_tab : 0)];
    if (!it || it->zone != 1)
        return;
    if (it->r.y < s_content_view.y + U(10))
        *scroll -= (float)(s_content_view.y + U(10) - it->r.y);
    else if (it->r.y + it->r.h > s_content_view.y + s_content_view.h - U(10))
        *scroll += (float)(it->r.y + it->r.h - (s_content_view.y + s_content_view.h - U(10)));
    if (*scroll < 0) *scroll = 0;
}

static void navigate(void)
{
    const Item *cur = prev_item(s_focus);
    int n = -1;
    if (s_capture)
        return;
    if (s_screen == LUI_SCREEN_HOME) {
        if (IP->up) s_focus = 1 + (s_home_sel + 3) % 4;
        if (IP->down) s_focus = 1 + (s_home_sel + 1) % 4;
        return;
    }
    if (IP->page_prev || IP->page_next) {
        s_page = (s_page + (IP->page_next ? 1 : LUI_PAGE_COUNT - 1)) % LUI_PAGE_COUNT;
        s_focus = 10 + s_page;
        s_zone = 0;
        return;
    }
    if (IP->start) {
        if (ENV->in_game)
            s_ret = LUI_RESUME;
        else if (dw_game_dir_ok(dw_game_dir()))
            s_ret = LUI_PLAY;
        return;
    }
    if (!cur) {
        if (IP->up || IP->down || IP->left || IP->right || IP->ok) {
            s_focus = 10 + s_page;
            s_zone = 0;
        }
        return;
    }
    if (IP->back) {
        if (s_zone == 1) {
            s_last_content_focus = s_focus;
            s_focus = 10 + s_page;
            s_zone = 0;
        } else if (ENV->in_game) {
            s_ret = LUI_RESUME;
        } else {
            s_screen = LUI_SCREEN_HOME;
            s_focus = -1;
        }
        return;
    }
    if (IP->up || IP->down)
        n = nearest(0, IP->down ? 1 : -1, s_zone);
    else if (IP->right && !cur->adjustable) {
        n = nearest(1, 0, s_zone);
        if (n < 0 && s_zone == 0) {
            /* into the page: the first item, or the last one used */
            int i, first = -1;
            for (i = 0; i < s_nprev; i++)
                if (s_prev_items[i].zone == 1) {
                    if (first < 0 || s_prev_items[i].r.y < prev_item(first)->r.y)
                        first = s_prev_items[i].id;
                    if (s_prev_items[i].id == s_last_content_focus)
                        n = s_last_content_focus;
                }
            if (n < 0)
                n = first;
            if (n >= 0)
                s_zone = 1;
        }
    } else if (IP->left && !cur->adjustable) {
        n = nearest(-1, 0, s_zone);
        if (n < 0 && s_zone == 1) {
            s_last_content_focus = s_focus;
            n = 10 + s_page;
            s_zone = 0;
        }
    }
    if (n >= 0) {
        s_focus = n;
        ensure_visible(n);
    }
}

/* ── the frame ───────────────────────────────────────────────────── */

int lui_frame(UiCanvas *c, const LuiInput *in, const LuiEnv *env)
{
    CV = c;
    IP = in;
    ENV = env;
    s_ret = LUI_NONE;
    s_anim = 0;
    S = fminf(c->w / 1600.0f, c->h / 900.0f);
    if (S < 0.55f) S = 0.55f;
    if (S > 3.0f) S = 3.0f;

    /* last frame's items drive keyboard navigation */
    memcpy(s_prev_items, s_items, sizeof s_items[0] * (size_t)s_nitems);
    s_nprev = s_nitems;
    s_nitems = 0;
    capture_input();
    navigate();

    ui_reset_clip(c);
    if (env->in_game && s_screen == LUI_SCREEN_HOME)
        s_screen = LUI_SCREEN_SETTINGS;
    if (s_screen == LUI_SCREEN_HOME) {
        home_screen();
    } else {
        UiRect side, main;
        background(c, env->in_game ? 2 : 1);
        settings_layout(c->w, c->h, &side, &main);
        if (s_focus < 0) {
            s_focus = 10 + s_page;
            s_zone = 0;
        }
        sidebar(side);
        content(main);
        if (s_capture)
            capture_overlay();
    }
    /* Esc / B on the title screen does nothing; the window's close box quits. */
    return s_ret;
}

/* After Play: the art, dimmed, with a loading line, shown until the game's
 * first frame replaces it. */
void lui_draw_loading(UiCanvas *c)
{
    CV = c;
    S = fminf(c->w / 1600.0f, c->h / 900.0f);
    if (S < 0.55f) S = 0.55f;
    ui_reset_clip(c);
    background(c, 0);
    ui_fill(c, ui_rect(0, 0, c->w, c->h), UI_RGBA(0, 0, 0, 120));
    {
        int iw = c->h * 4 / 3 < c->w ? c->h * 4 / 3 : c->w, ix = (c->w - iw) / 2;
        text_shadow(F_black, c->h * 0.034f / S, 3.0f, ix + iw * 64 / 100, c->h * 70 / 100,
                    UI_RGB(214, 40, 46), "LOADING...");
    }
}

int lui_capture_mode(void) { return s_capture; }
int lui_animating(void) { return s_anim || s_active >= 0; }

void lui_goto(int screen, int page)
{
    s_screen = screen;
    if (page >= 0 && page < LUI_PAGE_COUNT)
        s_page = page;
    s_focus = -1;
}

void lui_preview_capture(int a)
{
    s_capture = 1;
    s_cap_action = a;
    s_cap_slot = 0;
    s_cap_t0 = 0;
}

int lui_init(const void *f_display, int n_display, const void *f_body, int n_body,
             const void *f_bold, int n_bold, const void *f_black, int n_black)
{
    F_disp = ui_font_load(f_display, n_display);
    F_body = ui_font_load(f_body, n_body);
    F_bold = ui_font_load(f_bold, n_bold);
    F_black = ui_font_load(f_black, n_black);
    if (!F_bold) F_bold = F_body;
    if (!F_black) F_black = F_bold;
    if (!F_disp) F_disp = F_bold;
    return F_body != NULL;
}

/* A cheap blur: box filters, three passes each way, at a quarter size. */
void lui_set_backdrop(const uint32_t *px, int w, int h)
{
    int sw = w / 4, sh = h / 4, x, y, pass, ch;
    uint32_t *a, *b;
    ui_image_free(&I_game);
    s_bg_kind = -1;
    if (!px || sw < 8 || sh < 8)
        return;
    a = (uint32_t *)malloc((size_t)sw * sh * 4);
    b = (uint32_t *)malloc((size_t)sw * sh * 4);
    if (!a || !b) {
        free(a);
        free(b);
        return;
    }
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            uint32_t acc[3] = { 0, 0, 0 };
            int i, j;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    uint32_t p = px[(size_t)(y * 4 + j) * w + x * 4 + i];
                    acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255;
                }
            a[y * sw + x] = 0xFF000000u | (acc[2] / 16) << 16 | (acc[1] / 16) << 8 | (acc[0] / 16);
        }
    for (pass = 0; pass < 3; pass++) {
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++) {
                uint32_t acc[3] = { 0, 0, 0 }, n = 0;
                int k;
                for (k = -3; k <= 3; k++) {
                    int xx = x + k < 0 ? 0 : x + k >= sw ? sw - 1 : x + k;
                    uint32_t p = a[y * sw + xx];
                    for (ch = 0; ch < 3; ch++) acc[ch] += (p >> (ch * 8)) & 255;
                    n++;
                }
                b[y * sw + x] = 0xFF000000u | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
            }
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++) {
                uint32_t acc[3] = { 0, 0, 0 }, n = 0;
                int k;
                for (k = -3; k <= 3; k++) {
                    int yy = y + k < 0 ? 0 : y + k >= sh ? sh - 1 : y + k;
                    uint32_t p = b[yy * sw + x];
                    for (ch = 0; ch < 3; ch++) acc[ch] += (p >> (ch * 8)) & 255;
                    n++;
                }
                a[y * sw + x] = 0xFF000000u | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
            }
    }
    free(b);
    I_game.px = a;
    I_game.w = sw;
    I_game.h = sh;
}

void lui_load_art(const char *bg, const char *bg_blur, const char *emblem, const char *wordmark)
{
    if (bg) ui_image_load(&I_bg, bg);
    if (bg_blur) ui_image_load(&I_blur, bg_blur);
    if (emblem) ui_image_load(&I_emblem, emblem);
    if (wordmark) ui_image_load(&I_word, wordmark);
    s_bg_kind = -1;
}
