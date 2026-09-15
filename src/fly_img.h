/* fly_img: RGBA8 framebuffer, drawing primitives, text, portable PNG/TGA output */
#ifndef FLY_IMG_H
#define FLY_IMG_H

#include <stdint.h>

typedef struct {
    int w, h;
    uint32_t *px; /* 0xAABBGGRR little-endian byte order R,G,B,A */
} fly_img;

#define FLY_RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define FLY_RGB(r, g, b) FLY_RGBA(r, g, b, 255)

int fly_img_init(fly_img *im, int w, int h);
void fly_img_free(fly_img *im);
void fly_img_fill(fly_img *im, uint32_t color);
void fly_img_set(fly_img *im, int x, int y, uint32_t color);
uint32_t fly_img_get(const fly_img *im, int x, int y);
void fly_img_blend(fly_img *im, int x, int y, uint32_t color); /* src-over by alpha */
void fly_img_line(fly_img *im, int x0, int y0, int x1, int y1, uint32_t color);
void fly_img_rect(fly_img *im, int x, int y, int w, int h, uint32_t color);       /* outline */
void fly_img_fill_rect(fly_img *im, int x, int y, int w, int h, uint32_t color);  /* alpha blended */
void fly_img_circle(fly_img *im, int cx, int cy, int r, uint32_t color);
void fly_img_fill_circle(fly_img *im, int cx, int cy, int r, uint32_t color);
void fly_img_blit(fly_img *dst, const fly_img *src, int x, int y);

/* antialiased primitives (float coordinates, alpha-blended edges) */
void fly_img_aa_line(fly_img *im, float x0, float y0, float x1, float y1,
                     float width, uint32_t color);
void fly_img_aa_circle(fly_img *im, float cx, float cy, float r, float thick, uint32_t color);
void fly_img_aa_disc(fly_img *im, float cx, float cy, float r, uint32_t color);
void fly_img_aa_tri(fly_img *im, float x0, float y0, float x1, float y1,
                    float x2, float y2, uint32_t color);
/* filled antialiased convex polygon; `pts` is n x,y pairs, in either winding.
 * The sprite set is built out of these — a filled silhouette reads at 10 px
 * where a stroked outline of the same shape does not. */
#define FLY_IMG_POLY_MAX 16
void fly_img_aa_poly(fly_img *im, const float *pts, int n, uint32_t color);
void fly_img_round_rect(fly_img *im, float x, float y, float w, float h,
                        float rad, uint32_t color);
/* the same footprint as a `thick`-wide outline centred on the edge: the
 * hairline that makes a pane of glass a sheet rather than a soft patch */
void fly_img_round_rect_line(fly_img *im, float x, float y, float w, float h,
                             float rad, float thick, uint32_t color);
/* and filled with a vertical two-stop ramp, so a sprite body can be shaded */
void fly_img_round_rect_grad(fly_img *im, float x, float y, float w, float h,
                             float rad, uint32_t top, uint32_t bot);
/* --- frosted glass ---
 *
 * Blur what is already in the frame under a rounded rect, then lay a
 * translucent tint over it. A flat wash hides the world; frost keeps the world
 * there and takes its detail away, which is what makes text on top readable
 * without the panel reading as a box stuck to the screen.
 *
 * `blur` is the box-filter radius, applied twice, which is close enough to a
 * Gaussian at these sizes and costs two passes over the rect. `tint` is
 * composited over the blurred backdrop, and its alpha is how much glass there
 * is. Nothing outside the rounded footprint is touched, edges included: the
 * same coverage the rect itself would have drawn with. */
void fly_img_frost(fly_img *im, float x, float y, float w, float h,
                   float rad, float blur, uint32_t tint);
/* The same glass as a band rather than a slab: `thick` wide, centred on radius
 * `r`. What it is for is the reticle — a filled disc in the middle of the
 * frame hides the aircraft and the ground it is pointed at, and a ring gives
 * the instrument an edge to read against without taking the view away. */
void fly_img_frost_ring(fly_img *im, float cx, float cy, float r, float thick,
                        float blur, uint32_t tint);

/* soft drop shadow for a rounded-rect footprint; blur = falloff distance */
void fly_img_soft_shadow(fly_img *im, float x, float y, float w, float h,
                         float rad, float blur, uint32_t color);

/* embedded 5x7 font; scale >= 1; returns text advance in pixels */
int fly_img_text(fly_img *im, int x, int y, int scale, uint32_t color, const char *text);
int fly_img_text_width(int scale, const char *text);

/* box-filter downsample of src (must be dst size * factor) into dst; SSAA resolve */
void fly_img_downsample(fly_img *dst, const fly_img *src, int factor);

/* writers return 0 on success */
int fly_img_write_png(const fly_img *im, const char *path);
int fly_img_write_tga(const fly_img *im, const char *path);

#endif /* FLY_IMG_H */
