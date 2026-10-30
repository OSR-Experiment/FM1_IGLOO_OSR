/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise: drawing on the whole-screen canvas (gfx.c's cv_px, 240 x 240, pixels byte-swapped RGB565).
 * The browser prototype drew with translucent fills, ellipses and gradients: here are the same, small.
 * Included by app.c (the unity build: gfx.c is in scope). Alpha is 0..32. */
#pragma once

/* no libc on the device: the few string helpers we need */
static int bq_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int32_t bq_len(const char *s) { int32_t n = 0; while (s[n]) n++; return n; }

#define SW 240
#define SH 240

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a)
{
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    uint32_t r = ((f * a + b * (32u - a)) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

static inline uint32_t A32(float a) { return a <= 0.0f ? 0u : a >= 1.0f ? 32u : (uint32_t)(a * 32.0f + 0.5f); }

static inline void dot(int32_t x, int32_t y, uint16_t c)
{
    if ((uint32_t)x < SW && (uint32_t)y < SH) cv_px[y * SW + x] = swap16(c);
}
static inline void dot_a(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    if ((uint32_t)x < SW && (uint32_t)y < SH && a) {
        uint16_t *p = &cv_px[y * SW + x];
        *p = a >= 32u ? swap16(c) : swap16(blend565(swap16(*p), c, a));
    }
}

static void span(int32_t x0, int32_t x1, int32_t y, uint16_t c, uint32_t a)   /* x0 .. x1-1 */
{
    uint16_t *p, sc = swap16(c);
    if ((uint32_t)y >= SH || !a) return;
    if (x0 < 0) x0 = 0;
    if (x1 > SW) x1 = SW;
    p = &cv_px[y * SW];
    if (a >= 32u) for (; x0 < x1; x0++) p[x0] = sc;
    else for (; x0 < x1; x0++) p[x0] = swap16(blend565(swap16(p[x0]), c, a));
}

static void box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { int32_t j; for (j = 0; j < h; j++) span(x, x + w, y + j, c, 32u); }
static void box_a(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint32_t a) { int32_t j; for (j = 0; j < h; j++) span(x, x + w, y + j, c, a); }

/* an ellipse, filled: half 0 whole, 1 the top half only (the igloo's dome) */
static void oval(float cx, float cy, float rx, float ry, uint16_t c, uint32_t a, int half)
{
    int32_t y0 = (int32_t)fm_floorf(cy - ry), y1 = half ? (int32_t)fm_floorf(cy) : (int32_t)fm_floorf(cy + ry), y;
    if (ry < 0.5f || rx < 0.5f) return;
    for (y = y0; y <= y1; y++) {
        float d = ((float)y + 0.5f - cy) / ry, w;
        if (d < -1.0f || d > 1.0f) continue;
        w = rx * fm_sqrtf(1.0f - d * d);
        span((int32_t)(cx - w + 0.5f), (int32_t)(cx + w + 0.5f), y, c, a);
    }
}

/* the outline of an ellipse (one pixel) */
static void oring(float cx, float cy, float rx, float ry, uint16_t c, uint32_t a)
{
    int32_t i, n = (int32_t)((rx + ry) * 3.2f) + 8;
    int32_t lx = -99999, ly = -99999;
    for (i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        int32_t x = (int32_t)fm_floorf(cx + fm_cosf(t * 6.2831853f) * rx), y = (int32_t)fm_floorf(cy + fm_sinf(t * 6.2831853f) * ry);
        if (x != lx || y != ly) dot_a(x, y, c, a);
        lx = x; ly = y;
    }
}

/* a filled triangle */
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c, uint32_t a)
{
    float t;
    int32_t y, ya, yb;
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 - y0 < 0.5f) return;
    ya = (int32_t)fm_floorf(y0 + 0.5f); yb = (int32_t)fm_floorf(y2 + 0.5f);
    for (y = ya; y < yb; y++) {
        float fy = (float)y + 0.5f, xa = x0 + (x2 - x0) * (fy - y0) / (y2 - y0), xb;
        if (fy < y1) xb = y1 - y0 > 0.01f ? x0 + (x1 - x0) * (fy - y0) / (y1 - y0) : x1;
        else xb = y2 - y1 > 0.01f ? x1 + (x2 - x1) * (fy - y1) / (y2 - y1) : x1;
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        span((int32_t)(xa + 0.5f), (int32_t)(xb + 0.5f), y, c, a);
    }
}

static void line_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, uint32_t a)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy, k = 0;
    for (;;) {
        dot_a(x0, y0, c, a);
        if ((x0 == x1 && y0 == y1) || ++k > 800) break;
        { int32_t e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
    }
}

/* distances from a point, for the round glows and the halo: |dx|, |dy| < 128 (a quarter, mirrored) */
static uint8_t dist_q[128 * 128] PLAT_POOL;
static void dist_init(void)
{
    int32_t x, y;
    for (y = 0; y < 128; y++)
        for (x = 0; x < 128; x++) {
            float d = fm_sqrtf((float)(x * x + y * y));
            dist_q[y * 128 + x] = (uint8_t)(d > 255.0f ? 255.0f : d);
        }
}
static inline int32_t dist_at(int32_t dx, int32_t dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 127 || dy > 127) return 255;
    return dist_q[dy * 128 + dx];
}

/* a round glow: alpha a0 at the centre, nothing at radius r */
static void glow(int32_t cx, int32_t cy, int32_t r, uint16_t c, float a0)
{
    int32_t x, y;
    if (r > 127) r = 127;
    if (cx + r < 0 || cx - r >= SW || cy + r < 0 || cy - r >= SH || a0 <= 0.0f) return;
    for (y = cy - r; y <= cy + r; y++) {
        if ((uint32_t)y >= SH) continue;
        for (x = cx - r; x <= cx + r; x++) {
            int32_t d;
            if ((uint32_t)x >= SW) continue;
            d = dist_at(x - cx, y - cy);
            if (d < r) dot_a(x, y, c, A32(a0 * (1.0f - (float)d / (float)r)));
        }
    }
}

/* a pixel-art sprite: rows of characters, '.' clear, the rest looked up in a palette */
typedef struct { char ch; uint16_t c; } pal_t;
static void sprite(const char *const *rows, int32_t nrows, int32_t sx, int32_t sy, int flip, const pal_t *pal, int32_t npal)
{
    int32_t r, c, k, w;
    for (r = 0; r < nrows; r++) {
        const char *row = rows[r];
        w = bq_len(row);
        for (c = 0; c < w; c++) {
            char ch = row[flip ? w - 1 - c : c];
            if (ch == '.') continue;
            for (k = 0; k < npal; k++) if (pal[k].ch == ch) { dot(sx + c, sy + r, pal[k].c); break; }
        }
    }
}

/* text with a soft dark box behind it, centred on x */
static void text_box_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t ink, uint32_t a_box, int32_t pad)
{
    int32_t w = text_w(f, s);
    box_a(cx - w / 2 - pad, y, w + 2 * pad, (int32_t)f->h + 2, RGB(8, 14, 22), a_box);
    cv_text(cx - w / 2, y + 1, f, s, ink);
}
