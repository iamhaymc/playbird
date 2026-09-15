#include "fly_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FLY_HAS_STB
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#define FLY_FONT_FIRST 32
#define FLY_FONT_COUNT 96
/* Baked atlases kept per face. When this fills, `fly__bake` hands back the
 * last atlas it made rather than failing — which draws the text at somebody
 * else's size, silently, and is the worst shape a limit can have. The type
 * ladder plus the splash asks for five sizes on the UI face and four on the
 * mono one, so there is room for a screen's worth of new ones before that
 * becomes reachable; it is sixteen rather than eight so that adding a size
 * cannot quietly break an unrelated view. */
#define FLY_FONT_MAX_SIZES 16

#ifdef FLY_HAS_STB
typedef struct {
    float px;
    int aw, ah;
    unsigned char *atlas;
    stbtt_bakedchar chars[FLY_FONT_COUNT];
    float ascent;
} fly__baked;

typedef struct {
    int state; /* 0 untried, 1 ok, -1 failed */
    unsigned char *data;
    stbtt_fontinfo info;
    fly__baked sizes[FLY_FONT_MAX_SIZES];
    int nsizes;
} fly__face;

static fly__face fly__faces[2];

/* Where the faces are, relative to wherever the process was started: the app
 * directory, the repository root, the build directory, the RID directory under
 * it. The second of those said `apps/fly99/` and named nothing — the project is
 * fly99 and its directory is `apps/fly` — so from the repository root the only
 * prefix that could have worked did not, and the whole interface silently fell
 * back to the built-in bitmap face.
 *
 * fly_app.c carries the same list for the module registry and the airframe
 * assets, and the two have to agree: they are the same question about the same
 * `data/` directory, asked by the lowest module that needs a file and by the
 * one that owns loading them. */
#define FLY_FONT_PATHS 4
static const char *fly__paths[2][FLY_FONT_PATHS] = {
    { "data/fonts/ui.ttf", "apps/fly/data/fonts/ui.ttf",
            "../data/fonts/ui.ttf", "../../data/fonts/ui.ttf" },
    { "data/fonts/mono.ttf", "apps/fly/data/fonts/mono.ttf",
            "../data/fonts/mono.ttf", "../../data/fonts/mono.ttf" },
};

static unsigned char *fly__read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

static fly__face *fly__load(fly_font_face which) {
    fly__face *fc = &fly__faces[which];
    if (fc->state) return fc->state > 0 ? fc : NULL;
    fc->state = -1;
    int i;
    for (i = 0; i < FLY_FONT_PATHS && !fc->data; ++i)
        fc->data = fly__read_file(fly__paths[which][i], NULL);
    if (!fc->data) return NULL;
    if (!stbtt_InitFont(&fc->info, fc->data, stbtt_GetFontOffsetForIndex(fc->data, 0))) {
        free(fc->data);
        fc->data = NULL;
        return NULL;
    }
    fc->state = 1;
    return fc;
}

static fly__baked *fly__bake(fly__face *fc, float px) {
    int i;
    for (i = 0; i < fc->nsizes; ++i)
        if (fabsf(fc->sizes[i].px - px) < 0.01f) return &fc->sizes[i];
    if (fc->nsizes == FLY_FONT_MAX_SIZES) return &fc->sizes[FLY_FONT_MAX_SIZES - 1];
    fly__baked *b = &fc->sizes[fc->nsizes];
    b->px = px;
    b->aw = b->ah = px <= 20.0f ? 256 : 512;
    b->atlas = (unsigned char *)malloc((size_t)b->aw * (size_t)b->ah);
    if (!b->atlas) return NULL;
    if (stbtt_BakeFontBitmap(fc->data, 0, px, b->atlas, b->aw, b->ah,
                             FLY_FONT_FIRST, FLY_FONT_COUNT, b->chars) <= 0) {
        /* atlas too small for this size: try the big one once */
        free(b->atlas);
        b->aw = b->ah = 1024;
        b->atlas = (unsigned char *)malloc((size_t)b->aw * (size_t)b->ah);
        if (!b->atlas || stbtt_BakeFontBitmap(fc->data, 0, px, b->atlas, b->aw, b->ah,
                                              FLY_FONT_FIRST, FLY_FONT_COUNT, b->chars) <= 0) {
            free(b->atlas);
            b->atlas = NULL;
            return NULL;
        }
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fc->info, &asc, &desc, &gap);
    b->ascent = (float)asc * stbtt_ScaleForPixelHeight(&fc->info, px);
    ++fc->nsizes;
    return b;
}
#endif /* FLY_HAS_STB */

int fly_font_available(fly_font_face face) {
#ifdef FLY_HAS_STB
    return fly__load(face) != NULL;
#else
    (void)face;
    return 0;
#endif
}

float fly_line_height(float px) {
    return px * 1.42f;
}

/* ---- one glyph at a time ----
 *
 * The atlas is baked over ASCII and the game's text is UTF-8, so a character
 * outside the atlas has to come out as one glyph and not as one per byte. It
 * used to come out as one per byte: an em-dash is three of them and every one
 * is outside the range, so "Selkie 95 went down — wreckage on the deck" drew
 * as "went down ??? wreckage" — and, worse, *measured* three glyphs wide, so
 * every layout that asks fly_text_width where a string ends was told the wrong
 * answer and the HUD placed the next thing against it.
 *
 * Decoding here rather than folding at the call sites is the point: measuring
 * and drawing both come through this, so they cannot disagree about how many
 * glyphs a string is. The fold is small on purpose — the typography the game
 * actually writes in is dashes and quotes, and at these pixel sizes the ASCII
 * that stands in for them is indistinguishable. Anything else is one '?',
 * which is what a missing glyph should look like: one hole, not three. */
static unsigned fly__utf8(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = *s++, cp;
    int n;
    if (c < 0x80u) { *ps = (const char *)s; return c; }
    if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; n = 1; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; n = 2; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; n = 3; }
    else { *ps = (const char *)s; return 0xFFFDu; } /* stray continuation byte */
    while (n-- > 0) {
        if ((*s & 0xC0u) != 0x80u) { *ps = (const char *)s; return 0xFFFDu; }
        cp = (cp << 6) | (unsigned)(*s++ & 0x3Fu);
    }
    *ps = (const char *)s;
    return cp;
}

/* the codepoint as a baked ASCII index, or '?' when there is nothing to use */
static unsigned char fly__glyph(unsigned cp) {
    switch (cp) {
    case 0x00A0u: return ' ';                       /* no-break space */
    case 0x2010u: case 0x2011u: case 0x2012u:
    case 0x2013u: case 0x2014u: case 0x2015u:
    case 0x2212u: return '-';                       /* the dash family */
    case 0x2018u: case 0x2019u: return '\'';
    case 0x201Cu: case 0x201Du: return '"';
    case 0x00D7u: return 'x';
    default: break;
    }
    if (cp >= FLY_FONT_FIRST && cp < FLY_FONT_FIRST + FLY_FONT_COUNT)
        return (unsigned char)cp;
    return '?';
}

/* ---- fallback metrics (built-in 5x7 bitmap font) ---- */

static int fly__fb_scale(float px) {
    int s = (int)(px / 8.0f + 0.5f);
    return s < 1 ? 1 : s;
}

static float fly__line_width(fly_font_face face, float px, const char *s, const char **next);

float fly_text_width(fly_font_face face, float px, const char *s) {
    float best = 0.0f;
    const char *p = s;
    while (p && *p) {
        const char *nl;
        float w = fly__line_width(face, px, p, &nl);
        if (w > best) best = w;
        p = nl;
    }
    return best;
}

#ifdef FLY_HAS_STB
/* Extra advance between one glyph and the next, in pixels.
 *
 * Everything readable in this game goes through fly_text_sh, which lays a
 * one-pixel outline on four sides of every glyph and a drop shadow after it.
 * That outline is one pixel at *every* type size, so it always spends two
 * pixels of whatever gap the face leaves between neighbouring glyphs — and the
 * gap is proportional to the size. Google Sans Code leaves about 2.1 px at
 * 26 px and the outline fits inside it; at FLY_FS_S, which is the size of
 * every tick label, tape number and gauge value on the HUD, it leaves 0.9, so
 * the outlines of adjacent digits meet and a three-figure altitude is drawn as
 * one connected blob. `550` read as one glyph with a notch in it.
 *
 * Give the small sizes those two pixels back. It is deliberately a function of
 * the size alone rather than of the pair, because the outline it is paying for
 * is a function of the size alone; it fades out by 20 px, where the face's own
 * sidebearings are already wide enough to carry it. Both the measuring and the
 * drawing path go through this, so a laid-out row still measures what it
 * draws and fly_hud_layout keeps agreeing with the pixels. */
static float fly__tracking(float px) {
    return px >= 20.0f ? 0.0f : (20.0f - px) * 0.19f;
}

static float fly__ttf_line_width(fly__face *fc, fly__baked *b, const char *s, const char **next) {
    float x = 0.0f, y = 0.0f, tr = fly__tracking(b->px);
    int n = 0;
    while (*s && *s != '\n') {
        unsigned char c = fly__glyph(fly__utf8(&s));
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(b->chars, b->aw, b->ah, c - FLY_FONT_FIRST, &x, &y, &q, 1);
        x += tr;
        ++n;
    }
    if (next) *next = *s == '\n' ? s + 1 : s;
    (void)fc;
    /* no tracking after the last glyph: a right-aligned column would otherwise
     * hang a phantom pixel of air off the edge it is aligned to */
    return n ? x - tr : x;
}
#endif

static float fly__line_width(fly_font_face face, float px, const char *s, const char **next) {
#ifdef FLY_HAS_STB
    fly__face *fc = fly__load(face);
    if (fc) {
        fly__baked *b = fly__bake(fc, px);
        if (b) return fly__ttf_line_width(fc, b, s, next);
    }
#endif
    /* fallback */
    {
        int scale = fly__fb_scale(px);
        int n = 0;
        while (*s && *s != '\n') { fly__utf8(&s); ++n; }
        if (next) *next = *s == '\n' ? s + 1 : s;
        (void)face;
        return (float)(n * 6 * scale);
    }
}

#ifdef FLY_HAS_STB
static void fly__ttf_draw_line(fly_img *im, fly__baked *b, float x, float y_top,
                               uint32_t color, const char *s) {
    float pen_x = x, pen_y = y_top + b->ascent, tr = fly__tracking(b->px);
    uint32_t ca = color >> 24;
    while (*s && *s != '\n') {
        unsigned char c = fly__glyph(fly__utf8(&s));
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(b->chars, b->aw, b->ah, c - FLY_FONT_FIRST, &pen_x, &pen_y, &q, 1);
        pen_x += tr;
        int x0 = (int)floorf(q.x0), y0 = (int)floorf(q.y0);
        int gw = (int)ceilf(q.x1 - q.x0), gh = (int)ceilf(q.y1 - q.y0);
        int sx = (int)(q.s0 * (float)b->aw), sy = (int)(q.t0 * (float)b->ah);
        int i, j;
        for (j = 0; j < gh; ++j)
            for (i = 0; i < gw; ++i) {
                int ax = sx + i, ay = sy + j;
                if (ax < 0 || ay < 0 || ax >= b->aw || ay >= b->ah) continue;
                unsigned cov = b->atlas[ay * b->aw + ax];
                if (!cov) continue;
                uint32_t a = cov * ca / 255u;
                fly_img_blend(im, x0 + i, y0 + j, (color & 0x00FFFFFFu) | (a << 24));
            }
    }
}
#endif

/* --- the ink box ---
 *
 * Where the marks are, rather than where the em box is. The baked quads carry
 * exactly this — a glyph's box in pixels around the pen — so walking the
 * string and taking the union of them is the measurement, not an estimate of
 * it. Blank strings report an empty box at the origin, which is the honest
 * answer and leaves a caller centring "" exactly where it asked. */
void fly_text_ink(fly_font_face face, float px, const char *s, float *top, float *bot) {
    float lo = 1e9f, hi = -1e9f;
#ifdef FLY_HAS_STB
    fly__face *fc = fly__load(face);
    fly__baked *b = fc ? fly__bake(fc, px) : NULL;
    if (b) {
        const char *p = s;
        float pen_x = 0.0f, pen_y = b->ascent, tr = fly__tracking(px);
        while (p && *p) {
            if (*p == '\n') { ++p; pen_x = 0.0f; pen_y += fly_line_height(px); continue; }
            {
                unsigned char c = fly__glyph(fly__utf8(&p));
                stbtt_aligned_quad q;
                stbtt_GetBakedQuad(b->chars, b->aw, b->ah, c - FLY_FONT_FIRST,
                                   &pen_x, &pen_y, &q, 1);
                pen_x += tr;
                /* a space draws nothing, and its quad is a degenerate box that
                   would otherwise drag the union up to the baseline */
                if (q.x1 - q.x0 <= 0.0f || q.y1 - q.y0 <= 0.0f) continue;
                if (q.y0 < lo) lo = q.y0;
                if (q.y1 > hi) hi = q.y1;
            }
        }
    } else
#endif
    {
        /* the built-in 5x7: fly_text draws it one scale below the given y */
        int scale = fly__fb_scale(px);
        const char *p = s;
        int marks = 0;
        while (p && *p) { if (*p != ' ' && *p != '\n') ++marks; ++p; }
        if (marks) { lo = (float)scale; hi = (float)(scale + 7 * scale); }
        (void)face;
    }
    if (hi < lo) lo = hi = 0.0f;
    if (top) *top = lo;
    if (bot) *bot = hi;
}

float fly_text(fly_img *im, fly_font_face face, float x, float y, float px,
               uint32_t color, fly_align align, const char *s) {
    float widest = 0.0f;
    const char *p = s;
    float ly = y;
#ifdef FLY_HAS_STB
    fly__face *fc = fly__load(face);
    fly__baked *b = fc ? fly__bake(fc, px) : NULL;
#endif
    while (p && *p) {
        const char *nl;
        float w = fly__line_width(face, px, p, &nl);
        float lx = align == FLY_ALIGN_CENTER ? x - w * 0.5f : align == FLY_ALIGN_RIGHT ? x - w : x;
#ifdef FLY_HAS_STB
        if (b) {
            fly__ttf_draw_line(im, b, lx, ly, color, p);
        } else
#endif
        {
            /* The 5x7 table is indexed by byte, so the line is folded to one
             * ASCII glyph per character on the way in — the same fold the
             * measurement above used, or this path would draw a different
             * number of glyphs than it was laid out for. */
            int scale = fly__fb_scale(px);
            char line[256];
            const char *q = p;
            size_t n = 0;
            while (*q && *q != '\n' && n + 1 < sizeof line)
                line[n++] = (char)fly__glyph(fly__utf8(&q));
            line[n] = '\0';
            fly_img_text(im, (int)lx, (int)ly + scale, scale, color, line);
        }
        if (w > widest) widest = w;
        p = nl;
        ly += fly_line_height(px);
    }
    return widest;
}

/* White text that has to hold over anything.
 *
 * The HUD is white type on white glass over a rendered world, and an overcast
 * sky is the brightest thing in the game. A single offset drop shadow leaves
 * the up-left edge of every stroke bare, which over a snowfield is most of the
 * letter — so this lays a one-pixel outline on four sides first and the drop
 * shadow after it. Six passes of a bitmap font at these sizes is nothing, and
 * it is the difference between type that sits on the picture and type that
 * dissolves into it.
 *
 * All of it tracks the text's own alpha, so a dimmed label dims its outline
 * with it rather than turning into grey type in a hard black box. */
float fly_text_sh(fly_img *im, fly_font_face face, float x, float y, float px,
                  uint32_t color, fly_align align, const char *s) {
    float off = px >= 16.0f ? 1.5f : 1.0f;
    uint32_t a = color >> 24;
    uint32_t edge = (a * 150u) / 255u, drop = (a * 175u) / 255u;
    uint32_t ec = FLY_RGBA(0, 0, 0, edge);
    fly_text(im, face, x - 1.0f, y, px, ec, align, s);
    fly_text(im, face, x + 1.0f, y, px, ec, align, s);
    fly_text(im, face, x, y - 1.0f, px, ec, align, s);
    fly_text(im, face, x, y + 1.0f, px, ec, align, s);
    fly_text(im, face, x + off, y + off, px, FLY_RGBA(0, 0, 0, drop), align, s);
    return fly_text(im, face, x, y, px, color, align, s);
}

/* Centred on a point, by the marks rather than by the size.
 *
 * `cy` is the middle of the ink, so a glyph lands in the middle of whatever it
 * is being centred in — the '?' in the help button, the FIRE on its pad, a
 * value on a row's centre line. Every one of those used to be written as
 * `y - px * 0.5f` at the call site, which centres the em box: the face
 * reserves descender room the string may not use, so the marks came out high
 * by a different amount for every string, and in a 20 px circle that reads as
 * a glyph stuck to the top of its button.
 *
 * The shadow comes with it, because everything readable in this game carries
 * one and a centring helper that quietly dropped it would be a trap. */
float fly_text_mid(fly_img *im, fly_font_face face, float cx, float cy, float px,
                   uint32_t color, fly_align align, const char *s) {
    float top = 0.0f, bot = 0.0f;
    fly_text_ink(face, px, s, &top, &bot);
    return fly_text_sh(im, face, cx, cy - (top + bot) * 0.5f, px, color, align, s);
}
