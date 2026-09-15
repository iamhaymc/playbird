#include "fly_img.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fly_math.h"

int fly_img_init(fly_img *im, int w, int h) {
    im->w = w;
    im->h = h;
    im->px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    return im->px ? 0 : -1;
}

void fly_img_free(fly_img *im) {
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

void fly_img_fill(fly_img *im, uint32_t color) {
    int i, n = im->w * im->h;
    for (i = 0; i < n; ++i) im->px[i] = color;
}

void fly_img_set(fly_img *im, int x, int y, uint32_t color) {
    if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[y * im->w + x] = color;
}

uint32_t fly_img_get(const fly_img *im, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= im->w) x = im->w - 1;
    if (y >= im->h) y = im->h - 1;
    return im->px[y * im->w + x];
}

void fly_img_blend(fly_img *im, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint32_t a = color >> 24;
    if (a == 255) { im->px[y * im->w + x] = color; return; }
    if (a == 0) return;
    uint32_t dst = im->px[y * im->w + x];
    uint32_t ia = 255 - a;
    uint32_t r = (((color) & 0xFF) * a + ((dst) & 0xFF) * ia) / 255;
    uint32_t g = (((color >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    uint32_t b = (((color >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    im->px[y * im->w + x] = FLY_RGBA(r, g, b, 255);
}

void fly_img_line(fly_img *im, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fly_img_blend(im, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fly_img_rect(fly_img *im, int x, int y, int w, int h, uint32_t color) {
    fly_img_line(im, x, y, x + w - 1, y, color);
    fly_img_line(im, x, y + h - 1, x + w - 1, y + h - 1, color);
    fly_img_line(im, x, y, x, y + h - 1, color);
    fly_img_line(im, x + w - 1, y, x + w - 1, y + h - 1, color);
}

void fly_img_fill_rect(fly_img *im, int x, int y, int w, int h, uint32_t color) {
    int i, j;
    for (j = y; j < y + h; ++j)
        for (i = x; i < x + w; ++i) fly_img_blend(im, i, j, color);
}

void fly_img_circle(fly_img *im, int cx, int cy, int r, uint32_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        fly_img_blend(im, cx + x, cy + y, color); fly_img_blend(im, cx - x, cy + y, color);
        fly_img_blend(im, cx + x, cy - y, color); fly_img_blend(im, cx - x, cy - y, color);
        fly_img_blend(im, cx + y, cy + x, color); fly_img_blend(im, cx - y, cy + x, color);
        fly_img_blend(im, cx + y, cy - x, color); fly_img_blend(im, cx - y, cy - x, color);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

void fly_img_fill_circle(fly_img *im, int cx, int cy, int r, uint32_t color) {
    int x, y;
    for (y = -r; y <= r; ++y)
        for (x = -r; x <= r; ++x)
            if (x * x + y * y <= r * r) fly_img_blend(im, cx + x, cy + y, color);
}

void fly_img_blit(fly_img *dst, const fly_img *src, int x, int y) {
    int i, j;
    for (j = 0; j < src->h; ++j)
        for (i = 0; i < src->w; ++i)
            fly_img_blend(dst, x + i, y + j, src->px[j * src->w + i]);
}

/* ---- antialiased primitives ---- */

static void fly__blend_cov(fly_img *im, int x, int y, uint32_t color, float cov) {
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;
    uint32_t a = (uint32_t)((float)(color >> 24) * cov);
    if (!a) return;
    fly_img_blend(im, x, y, (color & 0x00FFFFFFu) | (a << 24));
}

void fly_img_aa_line(fly_img *im, float x0, float y0, float x1, float y1,
                     float width, uint32_t color) {
    float hw = width * 0.5f;
    int minx = (int)floorf(fminf(x0, x1) - hw - 1), maxx = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int miny = (int)floorf(fminf(y0, y1) - hw - 1), maxy = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = len2 > 1e-6f ? fly_clampf((px * dx + py * dy) / len2, 0.0f, 1.0f) : 0.0f;
            float ex = px - t * dx, ey = py - t * dy;
            float d = sqrtf(ex * ex + ey * ey);
            fly__blend_cov(im, x, y, color, hw + 0.5f - d);
        }
}

void fly_img_aa_circle(fly_img *im, float cx, float cy, float r, float thick, uint32_t color) {
    float ht = thick * 0.5f;
    int minx = (int)floorf(cx - r - ht - 1), maxx = (int)ceilf(cx + r + ht + 1);
    int miny = (int)floorf(cy - r - ht - 1), maxy = (int)ceilf(cy + r + ht + 1);
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - r);
            fly__blend_cov(im, x, y, color, ht + 0.5f - d);
        }
}

void fly_img_aa_disc(fly_img *im, float cx, float cy, float r, uint32_t color) {
    int minx = (int)floorf(cx - r - 1), maxx = (int)ceilf(cx + r + 1);
    int miny = (int)floorf(cy - r - 1), maxy = (int)ceilf(cy + r + 1);
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            fly__blend_cov(im, x, y, color, r + 0.5f - d);
        }
}

/* filled antialiased triangle (winding-agnostic, edge-distance coverage) */
void fly_img_aa_tri(fly_img *im, float x0, float y0, float x1, float y1,
                    float x2, float y2, uint32_t color) {
    /* ensure counter-clockwise so inside = negative edge distance */
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area < 0.0f) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    int minx = (int)floorf(fminf(fminf(x0, x1), x2) - 1);
    int maxx = (int)ceilf(fmaxf(fmaxf(x0, x1), x2) + 1);
    int miny = (int)floorf(fminf(fminf(y0, y1), y2) - 1);
    int maxy = (int)ceilf(fmaxf(fmaxf(y0, y1), y2) + 1);
    float ex[3] = { y1 - y0, y2 - y1, y0 - y2 };
    float ey[3] = { x0 - x1, x1 - x2, x2 - x0 };
    float ox[3] = { x0, x1, x2 }, oy[3] = { y0, y1, y2 };
    int k;
    for (k = 0; k < 3; ++k) {
        float l = sqrtf(ex[k] * ex[k] + ey[k] * ey[k]);
        if (l > 1e-6f) { ex[k] /= l; ey[k] /= l; }
    }
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = -1e9f;
            for (k = 0; k < 3; ++k) {
                float dk = (px - ox[k]) * ex[k] + (py - oy[k]) * ey[k];
                if (dk > d) d = dk;
            }
            fly__blend_cov(im, x, y, color, 0.5f - d);
        }
}

/* Filled antialiased convex polygon: the triangle above with n sides.
 *
 * The sprite set is drawn out of these. A glyph built from strokes is a glyph
 * with a weight problem — thin enough to look drawn and it disappears at 11 px,
 * heavy enough to survive and it is a blob — where a filled silhouette reads at
 * any size, which is the whole argument for a shape over a stroke. Convex only,
 * and quietly clipped rather than checked, because every caller here hands it a
 * hull it wrote out by hand. */
void fly_img_aa_poly(fly_img *im, const float *pts, int n, uint32_t color) {
    float ex[FLY_IMG_POLY_MAX], ey[FLY_IMG_POLY_MAX];
    float px_[FLY_IMG_POLY_MAX], py_[FLY_IMG_POLY_MAX];
    float area = 0.0f, lox, loy, hix, hiy;
    int minx, maxx, miny, maxy, i, k, x, y;
    if (n < 3) return;
    if (n > FLY_IMG_POLY_MAX) n = FLY_IMG_POLY_MAX;
    for (i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += pts[2 * i] * pts[2 * j + 1] - pts[2 * j] * pts[2 * i + 1];
    }
    /* counter-clockwise, so "inside" is a negative distance on every edge */
    for (i = 0; i < n; ++i) {
        int s = area < 0.0f ? n - 1 - i : i;
        px_[i] = pts[2 * s];
        py_[i] = pts[2 * s + 1];
    }
    lox = hix = px_[0];
    loy = hiy = py_[0];
    for (i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        float dx = px_[j] - px_[i], dy = py_[j] - py_[i];
        float l = sqrtf(dx * dx + dy * dy);
        ex[i] = l > 1e-6f ? dy / l : 0.0f;
        ey[i] = l > 1e-6f ? -dx / l : 0.0f;
        if (px_[i] < lox) lox = px_[i];
        if (px_[i] > hix) hix = px_[i];
        if (py_[i] < loy) loy = py_[i];
        if (py_[i] > hiy) hiy = py_[i];
    }
    minx = (int)floorf(lox - 1);
    maxx = (int)ceilf(hix + 1);
    miny = (int)floorf(loy - 1);
    maxy = (int)ceilf(hiy + 1);
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float qx = (float)x + 0.5f, qy = (float)y + 0.5f, d = -1e9f;
            for (k = 0; k < n; ++k) {
                float dk = (qx - px_[k]) * ex[k] + (qy - py_[k]) * ey[k];
                if (dk > d) d = dk;
            }
            fly__blend_cov(im, x, y, color, 0.5f - d);
        }
}

/* signed distance to a rounded rectangle */
static float fly__rrect_sdf(float px, float py, float cx, float cy,
                            float hw, float hh, float rad) {
    float qx, qy;
    /* A radius larger than the half-extent is a pill, not a bug: clamp it here
     * so a caller can ask for "as round as this box goes" by passing a big
     * number, instead of every caller doing the same min() by hand. Left
     * unclamped the corner offsets go positive on the inside and the box grows
     * a bulge along its short axis. */
    if (rad > hw) rad = hw;
    if (rad > hh) rad = hh;
    if (rad < 0.0f) rad = 0.0f;
    qx = fabsf(px - cx) - (hw - rad);
    qy = fabsf(py - cy) - (hh - rad);
    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
    float outside = sqrtf(ox * ox + oy * oy);
    float inside = fminf(fmaxf(qx, qy), 0.0f);
    return outside + inside - rad;
}

void fly_img_round_rect(fly_img *im, float x, float y, float w, float h,
                        float rad, uint32_t color) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int minx = (int)floorf(x - 1), maxx = (int)ceilf(x + w + 1);
    int miny = (int)floorf(y - 1), maxy = (int)ceilf(y + h + 1);
    int xi, yi;
    for (yi = miny; yi <= maxy; ++yi)
        for (xi = minx; xi <= maxx; ++xi) {
            float d = fly__rrect_sdf((float)xi + 0.5f, (float)yi + 0.5f, cx, cy, hw, hh, rad);
            fly__blend_cov(im, xi, yi, color, 0.5f - d);
        }
}

/* The same footprint as an outline: a band of `thick` centred on the edge.
 *
 * Every pane in the game has one of these on it. A frosted panel with no edge
 * is a soft patch that could be a lens flare; one hairline and it is a sheet of
 * glass lying on the picture, which is the whole difference between a HUD that
 * looks assembled and one that looks smeared on. */
void fly_img_round_rect_line(fly_img *im, float x, float y, float w, float h,
                             float rad, float thick, uint32_t color) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float ht = thick * 0.5f;
    int minx = (int)floorf(x - ht - 1), maxx = (int)ceilf(x + w + ht + 1);
    int miny = (int)floorf(y - ht - 1), maxy = (int)ceilf(y + h + ht + 1);
    int xi, yi;
    if (w <= 0.0f || h <= 0.0f || thick <= 0.0f) return;
    for (yi = miny; yi <= maxy; ++yi)
        for (xi = minx; xi <= maxx; ++xi) {
            float d = fly__rrect_sdf((float)xi + 0.5f, (float)yi + 0.5f, cx, cy, hw, hh, rad);
            fly__blend_cov(im, xi, yi, color, ht + 0.5f - fabsf(d));
        }
}

/* A rounded rect filled with a vertical two-stop ramp.
 *
 * What it is for is the sprites: a flat silhouette is a symbol and a shaded one
 * is an object, and the difference costs one lerp per pixel. Both stops carry
 * their own alpha so a body can fade as well as darken down its length. */
void fly_img_round_rect_grad(fly_img *im, float x, float y, float w, float h,
                             float rad, uint32_t top, uint32_t bot) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int minx = (int)floorf(x - 1), maxx = (int)ceilf(x + w + 1);
    int miny = (int)floorf(y - 1), maxy = (int)ceilf(y + h + 1);
    int xi, yi, c;
    if (w <= 0.0f || h <= 0.0f) return;
    for (yi = miny; yi <= maxy; ++yi) {
        float t = fly_clampf(((float)yi + 0.5f - y) / h, 0.0f, 1.0f);
        uint32_t mix = 0;
        for (c = 0; c < 4; ++c) {
            float a = (float)((top >> (c * 8)) & 255u), b = (float)((bot >> (c * 8)) & 255u);
            mix |= (uint32_t)(a + (b - a) * t + 0.5f) << (c * 8);
        }
        for (xi = minx; xi <= maxx; ++xi) {
            float d = fly__rrect_sdf((float)xi + 0.5f, (float)yi + 0.5f, cx, cy, hw, hh, rad);
            fly__blend_cov(im, xi, yi, mix, 0.5f - d);
        }
    }
}

/* The four running sums back into a pixel. Through unsigned, because the alpha
 * byte does not fit anywhere else: `255 << 24` on an int is 4,278,190,080,
 * which is not representable in a 32-bit signed int, and a shift whose result
 * is not representable is undefined rather than merely wrapped — the compiler
 * is entitled to assume it cannot happen. It produced the bits anybody would
 * expect here, and it is still a defect. */
static uint32_t fly__pack_acc(const int acc[4], int n) {
    return ((uint32_t)(acc[0] / n) & 255u) | (((uint32_t)(acc[1] / n) & 255u) << 8) |
           (((uint32_t)(acc[2] / n) & 255u) << 16) | (((uint32_t)(acc[3] / n) & 255u) << 24);
}

/* One separable box pass over a scratch buffer, in place, using `tmp` as the
 * intermediate. Two of these read as a blur; one reads as a smear. */
/* A running sum, not a re-summed window. The straightforward version reads
 * 2r+1 pixels per output pixel per axis, which for the page blur (r=15 over a
 * full 960x540 frame, twice) is sixty-four million channel adds a frame — the
 * profile page measured 350 ms, and it is a menu. Sliding the window costs two
 * reads per output pixel whatever r is. Every tap is clamped at the edges the
 * same way the summed version clamped, and the divisor is the same 2r+1, so
 * this is the identical image and not merely a similar one. */
static void fly__box_blur(uint32_t *buf, uint32_t *tmp, int w, int h, int r) {
    int x, y, c, n = 2 * r + 1;
    int acc[4];
    if (r < 1 || w < 1 || h < 1) return;
    for (y = 0; y < h; ++y) {
        const uint32_t *src = buf + (size_t)y * (size_t)w;
        uint32_t *dst = tmp + (size_t)y * (size_t)w;
        for (c = 0; c < 4; ++c) acc[c] = 0;
        for (x = -r; x <= r; ++x) {
            uint32_t p = src[x < 0 ? 0 : x >= w ? w - 1 : x];
            for (c = 0; c < 4; ++c) acc[c] += (int)((p >> (c * 8)) & 255u);
        }
        for (x = 0; x < w; ++x) {
            uint32_t lo = src[x - r < 0 ? 0 : x - r];
            uint32_t hi = src[x + r + 1 >= w ? w - 1 : x + r + 1];
            dst[x] = fly__pack_acc(acc, n);
            for (c = 0; c < 4; ++c)
                acc[c] += (int)((hi >> (c * 8)) & 255u) - (int)((lo >> (c * 8)) & 255u);
        }
    }
    for (x = 0; x < w; ++x) {
        for (c = 0; c < 4; ++c) acc[c] = 0;
        for (y = -r; y <= r; ++y) {
            uint32_t p = tmp[(size_t)(y < 0 ? 0 : y >= h ? h - 1 : y) * (size_t)w + x];
            for (c = 0; c < 4; ++c) acc[c] += (int)((p >> (c * 8)) & 255u);
        }
        for (y = 0; y < h; ++y) {
            uint32_t lo = tmp[(size_t)(y - r < 0 ? 0 : y - r) * (size_t)w + x];
            uint32_t hi = tmp[(size_t)(y + r + 1 >= h ? h - 1 : y + r + 1) * (size_t)w + x];
            buf[(size_t)y * (size_t)w + x] = fly__pack_acc(acc, n);
            for (c = 0; c < 4; ++c)
                acc[c] += (int)((hi >> (c * 8)) & 255u) - (int)((lo >> (c * 8)) & 255u);
        }
    }
}

/* --- vibrancy ---
 *
 * A pane's job is to give the type something to sit on, and a white wash over
 * an already-white sky gives it nothing: the estate cluster over a bright
 * overcast came out at 213 against a 204 sky, so white text had nine levels of
 * separation and the panel may as well not have been there. Over dark ground
 * the same pane was 69 against 30, which is plenty.
 *
 * So the tint is not the whole material. Before the caller's tint goes on, the
 * glass *levels* what is behind it: one wash whose colour and alpha are solved
 * for from the blurred backdrop's own mean, so the pane comes out at the
 * material's level whatever it was laid over. Real frosted glass behaves this
 * way round — it scatters what is behind it, which lifts a dark backdrop and
 * settles a bright one — and the practical consequence is that one text colour
 * works everywhere.
 *
 * It used to be one-sided: a black wash over bright backdrops and nothing at
 * all over dark ones, capped at a fifth of a stop. That put the pane anywhere
 * between 30 and 200 depending on the weather, and at the top of that band
 * white type had fifty levels to live on — which is why the profile page read
 * as grey-on-grey over an overcast noon and as white-on-black at midnight. Two
 * sides and a level worth aiming at instead: FLY__FROST_LEVEL is where a pane
 * lands, dark enough that white type has two hundred levels under it, and the
 * authority caps are what keeps some of the world's own light and colour in
 * the panel rather than flattening it to a painted rectangle.
 *
 * Writes the wash's colour to *color and returns its alpha, 0..255. */
#define FLY__FROST_LEVEL 0.30f  /* where a pane sits, 0..1 of full white */
#define FLY__FROST_DOWN 0.78f   /* how much authority it has over a bright field */
#define FLY__FROST_UP 0.62f     /* and over a dark one */
static uint32_t fly__frost_level(const uint32_t *buf, int bw, int bh, uint32_t *color) {
    /* The two washes. Down is a cool near-black rather than black, and up a
     * cool off-white rather than white, so a levelled pane keeps the blue cast
     * the rest of the chrome is drawn in instead of going flat grey. */
    static const float dn[3] = { 0.055f, 0.065f, 0.085f };
    static const float up[3] = { 0.60f, 0.68f, 0.80f };
    long acc = 0;
    int i, n = bw * bh;
    float lum, a;
    const float *c;
    if (n < 1) return 0;
    for (i = 0; i < n; ++i) {
        uint32_t p = (uint32_t)buf[i];
        /* the same luma weights the tone mapper uses, at integer precision */
        acc += (long)(54u * (p & 255u) + 183u * ((p >> 8) & 255u) +
                      19u * ((p >> 16) & 255u)) >> 8;
    }
    lum = (float)acc / (float)n / 255.0f;
    if (lum > FLY__FROST_LEVEL) {
        c = dn;
        a = (lum - FLY__FROST_LEVEL) / (lum - 0.5f * (dn[0] + dn[2]));
        if (a > FLY__FROST_DOWN) a = FLY__FROST_DOWN;
    } else {
        c = up;
        a = (FLY__FROST_LEVEL - lum) / (0.5f * (up[0] + up[2]) - lum);
        if (a > FLY__FROST_UP) a = FLY__FROST_UP;
    }
    if (a <= 0.0f) return 0;
    *color = FLY_RGBA((int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
                      (int)(c[2] * 255.0f + 0.5f), (int)(a * 255.0f + 0.5f));
    return (uint32_t)(a * 255.0f + 0.5f);
}

/* Blur the frame inside a box and hand back the scratch, or NULL. The caller
 * owns the buffer and decides which of those blurred pixels actually land —
 * a rounded rect and a ring differ only in that mask. */
static uint32_t *fly__frost_grab(fly_img *im, int minx, int miny, int maxx, int maxy,
                                 int *bw_out, int *bh_out, float blur) {
    int bw, bh, bx, by;
    uint32_t *buf;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > im->w - 1) maxx = im->w - 1;
    if (maxy > im->h - 1) maxy = im->h - 1;
    bw = maxx - minx + 1;
    bh = maxy - miny + 1;
    if (bw < 1 || bh < 1) return NULL;
    buf = (uint32_t *)malloc((size_t)bw * (size_t)bh * sizeof(uint32_t) * 2);
    if (!buf) return NULL;
    for (by = 0; by < bh; ++by)
        for (bx = 0; bx < bw; ++bx)
            buf[by * bw + bx] = im->px[(miny + by) * im->w + (minx + bx)];
    fly__box_blur(buf, buf + (size_t)bw * (size_t)bh, bw, bh, (int)(blur + 0.5f));
    *bw_out = bw;
    *bh_out = bh;
    return buf;
}

void fly_img_frost(fly_img *im, float x, float y, float w, float h,
                   float rad, float blur, uint32_t tint) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int minx = (int)floorf(x) - 1, miny = (int)floorf(y) - 1;
    int bw = 0, bh = 0, bx, by;
    uint32_t shade, level = 0;
    uint32_t *buf;
    if (!im || !im->px || w <= 0.0f || h <= 0.0f) return;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    buf = fly__frost_grab(im, minx, miny, (int)ceilf(x + w) + 1, (int)ceilf(y + h) + 1,
                          &bw, &bh, blur);
    if (!buf) { fly_img_round_rect(im, x, y, w, h, rad, tint); return; }
    /* Put the blur back only inside the footprint, at the rect's own coverage,
     * so the glass has the same antialiased edge every other panel does. */
    for (by = 0; by < bh; ++by)
        for (bx = 0; bx < bw; ++bx) {
            float d = fly__rrect_sdf((float)(minx + bx) + 0.5f, (float)(miny + by) + 0.5f,
                                     cx, cy, hw, hh, rad);
            float cov = 0.5f - d;
            uint32_t p;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            p = buf[by * bw + bx];
            fly__blend_cov(im, minx + bx, miny + by,
                           (p & 0x00FFFFFFu) | 0xFF000000u, cov);
        }
    shade = fly__frost_level(buf, bw, bh, &level);
    free(buf);
    /* level first, tint second: the wash is part of the material and the tint
     * is what this particular pane is made of, so a caller asking for a warmer
     * or brighter pane gets it on top of a surface that is already at the
     * material's own level rather than at the weather's. */
    if (shade) fly_img_round_rect(im, x, y, w, h, rad, level);
    fly_img_round_rect(im, x, y, w, h, rad, tint);
}

/* The band does not take the levelling wash the slab does, and that is the
 * point of it being a different call. Levelling exists to give *type* a
 * predictable surface, and nothing is ever printed on this — it is a rim
 * around the middle of the frame. Pulled to the material's own dark level it
 * came out as a heavy black donut over the aircraft, which is the opposite of
 * an instrument you look through. Blur, tint, and the world's own light. */
void fly_img_frost_ring(fly_img *im, float cx, float cy, float r, float thick,
                        float blur, uint32_t tint) {
    float ro = r + thick * 0.5f, ri = r - thick * 0.5f;
    int minx = (int)floorf(cx - ro) - 1, miny = (int)floorf(cy - ro) - 1;
    int bw = 0, bh = 0, bx, by;
    uint32_t *buf;
    if (!im || !im->px || r <= 0.0f || thick <= 0.0f) return;
    if (ri < 0.0f) ri = 0.0f;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    buf = fly__frost_grab(im, minx, miny, (int)ceilf(cx + ro) + 1, (int)ceilf(cy + ro) + 1,
                          &bw, &bh, blur);
    if (!buf) {
        fly_img_aa_circle(im, cx, cy, r, thick, tint);
        return;
    }
    for (by = 0; by < bh; ++by)
        for (bx = 0; bx < bw; ++bx) {
            float px = (float)(minx + bx) + 0.5f - cx, py = (float)(miny + by) + 0.5f - cy;
            float d = sqrtf(px * px + py * py);
            /* coverage of the band, antialiased on both edges at once */
            float cov = (ro - d) < (d - ri) ? (ro - d) : (d - ri);
            uint32_t p;
            cov += 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            p = buf[by * bw + bx];
            fly__blend_cov(im, minx + bx, miny + by,
                           (p & 0x00FFFFFFu) | 0xFF000000u, cov);
        }
    free(buf);
    fly_img_aa_circle(im, cx, cy, r, thick, tint);
}

void fly_img_soft_shadow(fly_img *im, float x, float y, float w, float h,
                         float rad, float blur, uint32_t color) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int minx = (int)floorf(x - blur - 1), maxx = (int)ceilf(x + w + blur + 1);
    int miny = (int)floorf(y - blur - 1), maxy = (int)ceilf(y + h + blur + 1);
    int xi, yi;
    for (yi = miny; yi <= maxy; ++yi)
        for (xi = minx; xi <= maxx; ++xi) {
            float d = fly__rrect_sdf((float)xi + 0.5f, (float)yi + 0.5f, cx, cy, hw, hh, rad);
            if (d >= blur) continue;
            float t = d <= 0.0f ? 1.0f : 1.0f - d / blur;
            fly__blend_cov(im, xi, yi, color, t * t);
        }
}

/* classic 5x7 bitmap font, ASCII 32..126, column-major bytes (bit0 = top row) */
static const uint8_t fly__font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},
};

int fly_img_text(fly_img *im, int x, int y, int scale, uint32_t color, const char *text) {
    int cx = x;
    const char *p;
    for (p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = fly__font5x7[c - 32];
        int col, row, sx, sy;
        for (col = 0; col < 5; ++col)
            for (row = 0; row < 7; ++row)
                if (glyph[col] & (1 << row))
                    for (sy = 0; sy < scale; ++sy)
                        for (sx = 0; sx < scale; ++sx)
                            fly_img_blend(im, cx + col * scale + sx, y + row * scale + sy, color);
        cx += 6 * scale;
    }
    return cx - x;
}

int fly_img_text_width(int scale, const char *text) {
    int n = 0, best = 0;
    const char *p;
    for (p = text; *p; ++p) {
        if (*p == '\n') { n = 0; continue; }
        n += 6 * scale;
        if (n > best) best = n;
    }
    return best;
}

void fly_img_downsample(fly_img *dst, const fly_img *src, int factor) {
    int x, y, i, j;
    int f2 = factor * factor;
    for (y = 0; y < dst->h; ++y) {
        for (x = 0; x < dst->w; ++x) {
            uint32_t r = 0, g = 0, b = 0;
            for (j = 0; j < factor; ++j)
                for (i = 0; i < factor; ++i) {
                    uint32_t c = src->px[(y * factor + j) * src->w + (x * factor + i)];
                    r += c & 0xFF; g += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF;
                }
            /* Round the box average instead of truncating: truncation loses a
             * systematic half-LSB per channel, and it would also throw away the
             * sub-LSB precision the renderer's dithered resolve encodes across
             * the f^2 samples — which is what keeps smooth gradients from
             * banding once they land at the output resolution. */
            int h = f2 / 2;
            dst->px[y * dst->w + x] =
                FLY_RGB((int)((r + (uint32_t)h) / (uint32_t)f2),
                        (int)((g + (uint32_t)h) / (uint32_t)f2),
                        (int)((b + (uint32_t)h) / (uint32_t)f2));
        }
    }
}

/* ---- PNG writer (uncompressed "stored" deflate blocks, no dependencies) ---- */

static uint32_t fly__crc_table[256];
static int fly__crc_ready = 0;

static uint32_t fly__crc32(uint32_t crc, const uint8_t *buf, size_t len) {
    size_t i;
    if (!fly__crc_ready) {
        uint32_t c;
        int n, k;
        for (n = 0; n < 256; ++n) {
            c = (uint32_t)n;
            for (k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            fly__crc_table[n] = c;
        }
        fly__crc_ready = 1;
    }
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < len; ++i) crc = fly__crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void fly__be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int fly__png_chunk(FILE *f, const char *tag, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8], crcb[4];
    uint32_t crc;
    fly__be32(hdr, len);
    memcpy(hdr + 4, tag, 4);
    crc = fly__crc32(0, hdr + 4, 4);
    if (len) crc = fly__crc32(crc, data, len);
    fly__be32(crcb, crc);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    if (fwrite(crcb, 1, 4, f) != 4) return -1;
    return 0;
}

int fly_img_write_png(const fly_img *im, const char *path) {
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(sig, 1, 8, f) != 8) { fclose(f); return -1; }

    uint8_t ihdr[13];
    fly__be32(ihdr, (uint32_t)im->w);
    fly__be32(ihdr + 4, (uint32_t)im->h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (fly__png_chunk(f, "IHDR", ihdr, 13) != 0) { fclose(f); return -1; }

    /* raw scanlines: filter byte 0 + RGBA per pixel */
    size_t stride = (size_t)im->w * 4 + 1;
    size_t raw_len = stride * (size_t)im->h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    int x, y;
    for (y = 0; y < im->h; ++y) {
        uint8_t *row = raw + stride * (size_t)y;
        row[0] = 0;
        for (x = 0; x < im->w; ++x) {
            uint32_t c = im->px[y * im->w + x];
            row[1 + x * 4 + 0] = (uint8_t)(c & 0xFF);
            row[1 + x * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
            row[1 + x * 4 + 2] = (uint8_t)((c >> 16) & 0xFF);
            row[1 + x * 4 + 3] = 255;
        }
    }

    /* zlib stream with stored deflate blocks */
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    if (!z) { free(raw); fclose(f); return -1; }
    size_t zi = 0, off = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    size_t i;
    for (i = 0; i < raw_len; ++i) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    while (off < raw_len) {
        size_t n = raw_len - off;
        if (n > 65535) n = 65535;
        z[zi++] = (off + n == raw_len) ? 1 : 0;
        z[zi++] = (uint8_t)(n & 0xFF); z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF); z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }
    fly__be32(z + zi, (s2 << 16) | s1);
    zi += 4;
    free(raw);

    int rc = fly__png_chunk(f, "IDAT", z, (uint32_t)zi);
    free(z);
    if (rc == 0) rc = fly__png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return rc;
}

int fly_img_write_tga(const fly_img *im, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[18] = { 0 };
    hdr[2] = 2; /* uncompressed truecolor */
    hdr[12] = (uint8_t)(im->w & 0xFF); hdr[13] = (uint8_t)(im->w >> 8);
    hdr[14] = (uint8_t)(im->h & 0xFF); hdr[15] = (uint8_t)(im->h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28; /* top-left origin, 8 alpha bits */
    if (fwrite(hdr, 1, 18, f) != 18) { fclose(f); return -1; }
    int x, y;
    for (y = 0; y < im->h; ++y)
        for (x = 0; x < im->w; ++x) {
            uint32_t c = im->px[y * im->w + x];
            uint8_t p[4];
            p[0] = (uint8_t)((c >> 16) & 0xFF); /* B */
            p[1] = (uint8_t)((c >> 8) & 0xFF);  /* G */
            p[2] = (uint8_t)(c & 0xFF);         /* R */
            p[3] = 255;
            if (fwrite(p, 1, 4, f) != 4) { fclose(f); return -1; }
        }
    fclose(f);
    return 0;
}
