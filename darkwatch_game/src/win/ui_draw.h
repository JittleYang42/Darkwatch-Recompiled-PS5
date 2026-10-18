/*
 * ui_draw.h -- a small software renderer for the launcher: anti-aliased
 * rounded rectangles, gradients, scaled images and TrueType text, drawn into
 * a 32-bit pixel buffer that the host blits to its window. No platform
 * calls, so the launcher can be rendered (and checked) anywhere.
 */
#ifndef DW_UI_DRAW_H
#define DW_UI_DRAW_H

#include <stdint.h>

typedef struct { int x, y, w, h; } UiRect;

typedef struct {
    uint32_t *px;               /* 0xAARRGGBB, row-major, w per row */
    int w, h;
    UiRect clip;
} UiCanvas;

typedef struct {
    uint32_t *px;               /* premultiplied 0xAARRGGBB */
    int w, h;
} UiImage;

typedef struct UiFont UiFont;

#define UI_RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define UI_RGB(r, g, b)     UI_RGBA(r, g, b, 255)
#define UI_ALPHA(c, a)      (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

static inline UiRect ui_rect(int x, int y, int w, int h) { UiRect r = { x, y, w, h }; return r; }
static inline int ui_in(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
UiRect ui_inset(UiRect r, int d);

void ui_canvas_init(UiCanvas *c, uint32_t *px, int w, int h);
void ui_set_clip(UiCanvas *c, UiRect r);      /* intersected with the canvas */
void ui_reset_clip(UiCanvas *c);

void ui_clear(UiCanvas *c, uint32_t rgb);
void ui_fill(UiCanvas *c, UiRect r, uint32_t argb);
void ui_fill_round(UiCanvas *c, UiRect r, float radius, uint32_t argb);
void ui_stroke_round(UiCanvas *c, UiRect r, float radius, float width, uint32_t argb);
void ui_vgradient(UiCanvas *c, UiRect r, uint32_t top, uint32_t bottom);
void ui_hgradient(UiCanvas *c, UiRect r, uint32_t left, uint32_t right);
void ui_fill_circle(UiCanvas *c, float cx, float cy, float rad, uint32_t argb);
void ui_fill_triangle(UiCanvas *c, float x0, float y0, float x1, float y1,
                      float x2, float y2, uint32_t argb);
/* Soft shadow under a rounded rectangle. */
void ui_shadow(UiCanvas *c, UiRect r, float radius, int spread, uint32_t argb);

/* Images (premultiplied). The source rectangle (sx,sy,sw,sh) scaled
 * bilinearly into dst; alpha 0..255 on top of the image's own. */
int  ui_image_load(UiImage *img, const char *path);
int  ui_image_load_mem(UiImage *img, const void *data, int len);
void ui_image_free(UiImage *img);
void ui_image(UiCanvas *c, const UiImage *img, int sx, int sy, int sw, int sh, UiRect dst, int alpha);
/* Fill dst with the image, cropped to keep its aspect ("cover"). */
void ui_image_cover(UiCanvas *c, const UiImage *img, UiRect dst, int alpha);

/* Fonts: TrueType data that stays alive as long as the font. */
UiFont *ui_font_load(const void *ttf, int len);
UiFont *ui_font_load_file(const char *path);
float ui_font_ascent(UiFont *f, float px);
float ui_font_height(UiFont *f, float px);      /* ascent - descent */
float ui_text_width(UiFont *f, float px, float tracking, const char *utf8);
/* y is the top of the line; returns the x after the text. */
float ui_text(UiCanvas *c, UiFont *f, float px, float tracking, float x, float y,
              uint32_t argb, const char *utf8);
/* Word-wrapped into r (top-left aligned); returns the height used. */
int ui_text_wrap(UiCanvas *c, UiFont *f, float px, UiRect r, float line_gap,
                 uint32_t argb, const char *utf8);
/* Height a wrapped text would need at width w. */
int ui_text_wrap_height(UiFont *f, float px, int w, float line_gap, const char *utf8);
/* Text shortened with "..." to fit max_w. Returns a static buffer. */
const char *ui_text_fit(UiFont *f, float px, float tracking, float max_w, const char *utf8);

#endif
