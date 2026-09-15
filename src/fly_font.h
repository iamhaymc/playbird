/* fly_font: antialiased TTF text rendering with vendored stb_truetype.
 *
 * Two faces — UI (proportional) and MONO (tabular numbers, telemetry) —
 * are lazy-loaded from data/fonts/{ui,mono}.ttf (several relative search
 * paths so the CLI works from the app dir, repo root or build dir). Glyphs
 * are baked per pixel-size into cached coverage atlases and composited
 * with alpha blending, so text is antialiased at any size. When the TTFs
 * or TTF files are unavailable everything falls back to the built-in 5x7
 * bitmap font.
 *
 * y is the TOP of the line, not the baseline; multi-line text ('\n')
 * advances by fly_line_height(px).
 */
#ifndef FLY_FONT_H
#define FLY_FONT_H

#include "fly_img.h"

typedef enum {
    FLY_FONT_UI,
    FLY_FONT_MONO
} fly_font_face;

typedef enum {
    FLY_ALIGN_LEFT,
    FLY_ALIGN_CENTER,
    FLY_ALIGN_RIGHT
} fly_align;

/* 1 if a real TTF backs this face (0 = bitmap fallback) */
int fly_font_available(fly_font_face face);

float fly_text_width(fly_font_face face, float px, const char *s); /* widest line */
float fly_line_height(float px);

/* --- where the marks actually are ---
 *
 * The ink box of `s`: the topmost and bottommost lit row the string will draw,
 * as offsets from the y this call would be given. Both are 0 for a string with
 * no marks in it (spaces, or nothing at all).
 *
 * Centring text in a circle by its type size is centring the wrong box. A face
 * reserves room for the descenders of every glyph whether or not this string
 * has one, so `?` — which has no descender and does not reach the em top
 * either — sits about a pixel and a half high in a box centred on its size,
 * and inside a 20 px button that is plainly visible. Every other centred glyph
 * in the game has the same problem to a different degree, because the amount
 * of it depends on which glyph it is. Measure the marks instead. */
void fly_text_ink(fly_font_face face, float px, const char *s, float *top, float *bot);

/* draw; x is the anchor per `align`; returns the drawn width */
float fly_text(fly_img *im, fly_font_face face, float x, float y, float px,
               uint32_t color, fly_align align, const char *s);
/* same with a soft drop shadow underneath (contrast on any scene) */
float fly_text_sh(fly_img *im, fly_font_face face, float x, float y, float px,
                  uint32_t color, fly_align align, const char *s);
/* the same, centred on (cx, cy) by its ink box: one call for every glyph that
 * has to sit in the middle of a button, a pill or a row band */
float fly_text_mid(fly_img *im, fly_font_face face, float cx, float cy, float px,
                   uint32_t color, fly_align align, const char *s);

#endif /* FLY_FONT_H */
