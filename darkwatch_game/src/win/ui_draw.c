/*
 * ui_draw.c -- the launcher's software renderer (see ui_draw.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_draw.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_DEFAULT
#include "stb_image.h"

/* ── canvas ──────────────────────────────────────────────────────── */

UiRect ui_inset(UiRect r, int d)
{
    r.x += d; r.y += d; r.w -= 2 * d; r.h -= 2 * d;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

void ui_canvas_init(UiCanvas *c, uint32_t *px, int w, int h)
{
    c->px = px;
    c->w = w;
    c->h = h;
    ui_reset_clip(c);
}

void ui_reset_clip(UiCanvas *c)
{
    c->clip = ui_rect(0, 0, c->w, c->h);
}

static UiRect isect(UiRect a, UiRect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    UiRect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

void ui_set_clip(UiCanvas *c, UiRect r)
{
    c->clip = isect(r, ui_rect(0, 0, c->w, c->h));
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
    /* src is straight RGB; a is 0..255 */
    uint32_t ia = 255 - a;
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * ia + 0x800080) >> 8;
    uint32_t g  = ((src & 0x00FF00) * a + (dst & 0x00FF00) * ia + 0x008000) >> 8;
    return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}

static inline void put(UiCanvas *c, int x, int y, uint32_t argb, float cov)
{
    uint32_t a;
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h)
        return;
    if (cov <= 0.0f)
        return;
    if (cov > 1.0f)
        cov = 1.0f;
    a = (uint32_t)((float)(argb >> 24) * cov + 0.5f);
    if (!a)
        return;
    c->px[(size_t)y * c->w + x] = a >= 255 ? (argb | 0xFF000000u)
                                           : blend(c->px[(size_t)y * c->w + x], argb, a);
}

void ui_clear(UiCanvas *c, uint32_t rgb)
{
    size_t i, n = (size_t)c->w * c->h;
    for (i = 0; i < n; i++)
        c->px[i] = rgb | 0xFF000000u;
}

void ui_fill(UiCanvas *c, UiRect r, uint32_t argb)
{
    UiRect k = isect(r, c->clip);
    uint32_t a = argb >> 24;
    int x, y;
    if (!a || !k.w || !k.h)
        return;
    for (y = k.y; y < k.y + k.h; y++) {
        uint32_t *p = c->px + (size_t)y * c->w + k.x;
        if (a == 255)
            for (x = 0; x < k.w; x++) p[x] = argb;
        else
            for (x = 0; x < k.w; x++) p[x] = blend(p[x], argb, a);
    }
}

/* Signed distance from (px,py) to a rounded box (centre, half size, r). */
static inline float sd_round(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static void round_shape(UiCanvas *c, UiRect r, float rad, float width, uint32_t argb)
{
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hx = r.w * 0.5f, hy = r.h * 0.5f;
    UiRect k = isect(r, c->clip);
    int x, y, ir;
    if (rad > hx) rad = hx;
    if (rad > hy) rad = hy;
    ir = (int)ceilf(rad) + 1;
    for (y = k.y; y < k.y + k.h; y++) {
        int corner_row = y < r.y + ir || y >= r.y + r.h - ir;
        for (x = k.x; x < k.x + k.w; x++) {
            float d, cov;
            if (!corner_row && width <= 0 && x >= r.x + 1 && x < r.x + r.w - 1) {
                /* interior of a fill: a run */
                int x1 = r.x + r.w - 1 < k.x + k.w ? r.x + r.w - 1 : k.x + k.w;
                uint32_t a = argb >> 24, *p = c->px + (size_t)y * c->w;
                for (; x < x1; x++)
                    p[x] = a == 255 ? argb : blend(p[x], argb, a);
                x--;
                continue;
            }
            if (!corner_row && width > 0 && x >= r.x + ir + (int)width + 1
                && x < r.x + r.w - ir - (int)width - 1) {
                x = r.x + r.w - ir - (int)width - 2;
                continue;
            }
            d = sd_round(x + 0.5f, y + 0.5f, cx, cy, hx, hy, rad);
            if (width > 0)
                cov = 0.5f - (fabsf(d + width * 0.5f) - width * 0.5f);
            else
                cov = 0.5f - d;
            if (cov > 0)
                put(c, x, y, argb, cov);
        }
    }
}

void ui_fill_round(UiCanvas *c, UiRect r, float radius, uint32_t argb)
{
    if (argb >> 24)
        round_shape(c, r, radius, 0, argb);
}

void ui_stroke_round(UiCanvas *c, UiRect r, float radius, float width, uint32_t argb)
{
    if (argb >> 24 && width > 0)
        round_shape(c, r, radius, width, argb);
}

static uint32_t lerp_c(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0;
    int i;
    for (i = 0; i < 32; i += 8) {
        float va = (float)((a >> i) & 255), vb = (float)((b >> i) & 255);
        o |= (uint32_t)(va + (vb - va) * t + 0.5f) << i;
    }
    return o;
}

void ui_vgradient(UiCanvas *c, UiRect r, uint32_t top, uint32_t bottom)
{
    int y;
    for (y = 0; y < r.h; y++)
        ui_fill(c, ui_rect(r.x, r.y + y, r.w, 1), lerp_c(top, bottom, r.h > 1 ? (float)y / (r.h - 1) : 0));
}

void ui_hgradient(UiCanvas *c, UiRect r, uint32_t left, uint32_t right)
{
    int x;
    for (x = 0; x < r.w; x++)
        ui_fill(c, ui_rect(r.x + x, r.y, 1, r.h), lerp_c(left, right, r.w > 1 ? (float)x / (r.w - 1) : 0));
}

void ui_fill_circle(UiCanvas *c, float cx, float cy, float rad, uint32_t argb)
{
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1);
    int y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1), x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            put(c, x, y, argb, rad + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void ui_fill_triangle(UiCanvas *c, float x0, float y0, float x1, float y1,
                      float x2, float y2, uint32_t argb)
{
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    float area = edge(x0, y0, x1, y1, x2, y2);
    int x, y, sx, sy;
    if (fabsf(area) < 1e-3f)
        return;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++) {
            int in = 0;
            for (sy = 0; sy < 4; sy++)
                for (sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    float w0 = edge(x1, y1, x2, y2, px, py), w1 = edge(x2, y2, x0, y0, px, py);
                    float w2 = edge(x0, y0, x1, y1, px, py);
                    if (area < 0) { w0 = -w0; w1 = -w1; w2 = -w2; }
                    if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                        in++;
                }
            if (in)
                put(c, x, y, argb, in / 16.0f);
        }
}

void ui_shadow(UiCanvas *c, UiRect r, float radius, int spread, uint32_t argb)
{
    int i;
    uint32_t a = argb >> 24;
    for (i = spread; i > 0; i--) {
        float t = (float)i / (float)spread;
        uint32_t ai = (uint32_t)(a * (1.0f - t) * (1.0f - t) / (float)spread * 3.0f);
        if (ai > 255) ai = 255;
        ui_stroke_round(c, ui_rect(r.x - i, r.y - i + spread / 3, r.w + 2 * i, r.h + 2 * i),
                        radius + i, 1.5f, UI_ALPHA(argb, ai));
    }
}

/* ── images ──────────────────────────────────────────────────────── */

static int image_from_rgba(UiImage *img, unsigned char *d, int w, int h)
{
    int i;
    img->px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!img->px)
        return 0;
    img->w = w;
    img->h = h;
    for (i = 0; i < w * h; i++) {
        uint32_t r = d[i * 4], g = d[i * 4 + 1], b = d[i * 4 + 2], a = d[i * 4 + 3];
        r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255;
        img->px[i] = a << 24 | r << 16 | g << 8 | b;
    }
    return 1;
}

int ui_image_load_mem(UiImage *img, const void *data, int len)
{
    int w, h, n, ok;
    unsigned char *d = stbi_load_from_memory((const unsigned char *)data, len, &w, &h, &n, 4);
    memset(img, 0, sizeof *img);
    if (!d)
        return 0;
    ok = image_from_rgba(img, d, w, h);
    stbi_image_free(d);
    return ok;
}

int ui_image_load(UiImage *img, const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    int ok;
    memset(img, 0, sizeof *img);
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    ok = ui_image_load_mem(img, buf, (int)n);
    free(buf);
    return ok;
}

void ui_image_free(UiImage *img)
{
    free(img->px);
    memset(img, 0, sizeof *img);
}

void ui_image(UiCanvas *c, const UiImage *img, int sx, int sy, int sw, int sh, UiRect dst, int alpha)
{
    UiRect k = isect(dst, c->clip);
    int x, y;
    float fx, fy;
    if (!img->px || !k.w || !k.h || sw <= 0 || sh <= 0 || alpha <= 0)
        return;
    fx = (float)sw / dst.w;
    fy = (float)sh / dst.h;
    for (y = k.y; y < k.y + k.h; y++) {
        float v = sy + (y - dst.y + 0.5f) * fy - 0.5f;
        int y0 = (int)floorf(v), y1;
        float ty = v - y0;
        uint32_t *row = c->px + (size_t)y * c->w;
        if (y0 < 0) { y0 = 0; ty = 0; }
        y1 = y0 + 1 < img->h ? y0 + 1 : img->h - 1;
        if (y0 >= img->h) y0 = y1 = img->h - 1;
        for (x = k.x; x < k.x + k.w; x++) {
            float u = sx + (x - dst.x + 0.5f) * fx - 0.5f;
            int x0 = (int)floorf(u), x1;
            float tx = u - x0;
            uint32_t p00, p01, p10, p11, s = 0, sa, d;
            int ch;
            if (x0 < 0) { x0 = 0; tx = 0; }
            x1 = x0 + 1 < img->w ? x0 + 1 : img->w - 1;
            if (x0 >= img->w) x0 = x1 = img->w - 1;
            p00 = img->px[(size_t)y0 * img->w + x0]; p01 = img->px[(size_t)y0 * img->w + x1];
            p10 = img->px[(size_t)y1 * img->w + x0]; p11 = img->px[(size_t)y1 * img->w + x1];
            for (ch = 0; ch < 32; ch += 8) {
                float a = (float)((p00 >> ch) & 255) * (1 - tx) + (float)((p01 >> ch) & 255) * tx;
                float b = (float)((p10 >> ch) & 255) * (1 - tx) + (float)((p11 >> ch) & 255) * tx;
                uint32_t v8 = (uint32_t)((a * (1 - ty) + b * ty) * alpha / 255.0f + 0.5f);
                s |= (v8 > 255 ? 255 : v8) << ch;
            }
            /* premultiplied over */
            sa = s >> 24;
            d = row[x];
            if (sa >= 255) {
                row[x] = s | 0xFF000000u;
            } else if (sa || (s & 0xFFFFFF)) {
                uint32_t ia = 255 - sa, o = 0xFF000000u;
                for (ch = 0; ch < 24; ch += 8) {
                    uint32_t v8 = ((s >> ch) & 255) + (((d >> ch) & 255) * ia + 127) / 255;
                    o |= (v8 > 255 ? 255 : v8) << ch;
                }
                row[x] = o;
            }
        }
    }
}

void ui_image_cover(UiCanvas *c, const UiImage *img, UiRect dst, int alpha)
{
    float ia, da;
    int sw, sh;
    if (!img->px)
        return;
    ia = (float)img->w / img->h;
    da = (float)dst.w / dst.h;
    if (da > ia) {
        sw = img->w;
        sh = (int)(img->w / da);
    } else {
        sh = img->h;
        sw = (int)(img->h * da);
    }
    ui_image(c, img, (img->w - sw) / 2, (img->h - sh) / 2, sw, sh, dst, alpha);
}

/* ── fonts ───────────────────────────────────────────────────────── */

struct UiFont {
    stbtt_fontinfo info;
    const unsigned char *data;
    int ascent, descent, gap;
};

typedef struct {
    UiFont *f;
    int size;                   /* px * 4 */
    int cp;
    int w, h, xoff, yoff;
    float adv;
    unsigned char *bmp;
} Glyph;

#define GLYPHS 8192
static Glyph s_glyphs[GLYPHS];
static int s_nglyphs;

static void glyph_cache_clear(void)
{
    int i;
    for (i = 0; i < GLYPHS; i++) {
        free(s_glyphs[i].bmp);
        memset(&s_glyphs[i], 0, sizeof s_glyphs[i]);
    }
    s_nglyphs = 0;
}

static Glyph *glyph(UiFont *f, float px, int cp)
{
    int size = (int)(px * 4 + 0.5f);
    unsigned h = ((unsigned)(uintptr_t)f * 2654435761u) ^ ((unsigned)size * 40503u) ^ ((unsigned)cp * 97u);
    unsigned i, k;
    Glyph *g;
    float scale;
    int x0, y0, x1, y1, adv, lsb;

    for (k = 0; k < 64; k++) {
        i = (h + k) & (GLYPHS - 1);
        g = &s_glyphs[i];
        if (!g->f)
            break;
        if (g->f == f && g->size == size && g->cp == cp)
            return g;
    }
    if (k == 64 || s_nglyphs > GLYPHS * 3 / 4) {
        glyph_cache_clear();
        i = h & (GLYPHS - 1);
    }
    g = &s_glyphs[i];
    scale = stbtt_ScaleForPixelHeight(&f->info, size / 4.0f);
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    stbtt_GetCodepointBitmapBox(&f->info, cp, scale, scale, &x0, &y0, &x1, &y1);
    g->f = f;
    g->size = size;
    g->cp = cp;
    g->adv = adv * scale;
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    g->bmp = NULL;
    if (g->w > 0 && g->h > 0) {
        g->bmp = (unsigned char *)malloc((size_t)g->w * g->h);
        if (g->bmp)
            stbtt_MakeCodepointBitmap(&f->info, g->bmp, g->w, g->h, g->w, scale, scale, cp);
    }
    s_nglyphs++;
    return g;
}

UiFont *ui_font_load(const void *ttf, int len)
{
    UiFont *f = (UiFont *)calloc(1, sizeof *f);
    (void)len;
    if (!f)
        return NULL;
    f->data = (const unsigned char *)ttf;
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        free(f);
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
    return f;
}

UiFont *ui_font_load_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    unsigned char *buf;
    UiFont *f;
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    f = ui_font_load(buf, (int)n);
    if (!f)
        free(buf);
    return f;
}

static float font_scale(UiFont *f, float px)
{
    return stbtt_ScaleForPixelHeight(&f->info, px);
}

float ui_font_ascent(UiFont *f, float px)
{
    return f->ascent * font_scale(f, px);
}

float ui_font_height(UiFont *f, float px)
{
    return (f->ascent - f->descent) * font_scale(f, px);
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp;
    if (!*p)
        return 0;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = (p[0] & 0x1F) << 6 | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); *s += 3;
    } else if (p[1] && p[2] && p[3]) {
        cp = (p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F); *s += 4;
    } else { cp = '?'; *s += 1; }
    return cp;
}

float ui_text_width(UiFont *f, float px, float tracking, const char *s)
{
    float w = 0, scale = font_scale(f, px);
    int cp, prev = 0;
    while ((cp = utf8_next(&s))) {
        Glyph *g = glyph(f, px, cp);
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * scale;
        w += g->adv + tracking;
        prev = cp;
    }
    return w > tracking ? w - tracking : w;
}

float ui_text(UiCanvas *c, UiFont *f, float px, float tracking, float x, float y,
              uint32_t argb, const char *s)
{
    float scale = font_scale(f, px), base = y + f->ascent * scale;
    int cp, prev = 0;
    while ((cp = utf8_next(&s))) {
        Glyph *g = glyph(f, px, cp);
        if (prev)
            x += stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * scale;
        if (g->bmp) {
            int gx = (int)floorf(x + 0.5f) + g->xoff, gy = (int)floorf(base + 0.5f) + g->yoff, i, j;
            for (j = 0; j < g->h; j++) {
                int yy = gy + j;
                if (yy < c->clip.y || yy >= c->clip.y + c->clip.h)
                    continue;
                for (i = 0; i < g->w; i++) {
                    unsigned char a = g->bmp[j * g->w + i];
                    if (a)
                        put(c, gx + i, yy, argb, a / 255.0f);
                }
            }
        }
        x += g->adv + tracking;
        prev = cp;
    }
    return x;
}

/* Wrapping: break at spaces; a word longer than the line is not split. */
static int wrap_lines(UiCanvas *c, UiFont *f, float px, UiRect r, float line_gap,
                      uint32_t argb, const char *s, int draw)
{
    float lh = ui_font_height(f, px) * line_gap;
    float y = (float)r.y;
    char line[1024];
    while (*s) {
        const char *brk = NULL, *q;
        size_t n;
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        q = s;
        for (;;) {
            while (*q && *q != ' ' && *q != '\n')
                q++;
            n = (size_t)(q - s);
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, s, n);
            line[n] = 0;
            if (brk && ui_text_width(f, px, 0, line) > r.w)
                break;                          /* the previous break stands */
            brk = q;
            if (!*q || *q == '\n')
                break;
            q++;
        }
        n = (size_t)(brk - s);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        if (draw)
            ui_text(c, f, px, 0, (float)r.x, y, argb, line);
        y += lh;
        s = brk;
        if (*s == '\n')
            s++;
    }
    return (int)(y - r.y + 0.5f);
}

int ui_text_wrap(UiCanvas *c, UiFont *f, float px, UiRect r, float line_gap,
                 uint32_t argb, const char *s)
{
    return wrap_lines(c, f, px, r, line_gap, argb, s, 1);
}

int ui_text_wrap_height(UiFont *f, float px, int w, float line_gap, const char *s)
{
    return wrap_lines(NULL, f, px, ui_rect(0, 0, w, 0), line_gap, 0, s, 0);
}

const char *ui_text_fit(UiFont *f, float px, float tracking, float max_w, const char *s)
{
    static char buf[1024];
    size_t n = strlen(s);
    if (n >= sizeof buf - 4)
        n = sizeof buf - 4;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (ui_text_width(f, px, tracking, buf) <= max_w)
        return buf;
    /* keep the end of a path: "...\folder\file" reads better than the start */
    while (n > 1) {
        const char *tail = s + strlen(s) - n + 1;
        snprintf(buf, sizeof buf, "...%s", tail);
        if (ui_text_width(f, px, tracking, buf) <= max_w)
            return buf;
        n--;
    }
    return "...";
}
