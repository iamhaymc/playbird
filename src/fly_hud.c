#include "fly_hud.h"

#include <stdio.h>
#include <string.h>

/* font sizes (pixel heights) */
#define FS_S FLY_FS_S
#define FS_B FLY_FS_VAL
#define FS_M FLY_FS_HEAD
#define FS_L FLY_FS_TITLE

#define HUD_MARGIN 14.0f

/* The edge every pane of glass gets: one hairline all the way round, and a
 * brighter run along the top lip where the light would land on a real sheet.
 *
 * It is drawn as two passes rather than one because the lip is the half of the
 * edge that does the work — an unlit outline reads as a drawn box, and an
 * outline with a lit top reads as a sheet with a thickness. The lip is clipped
 * to the top third of the footprint by drawing a taller rounded rect and
 * letting the pane's own outline sit over the rest of it. */
static void fly__pane_edge(fly_img *im, float x, float y, float w, float h, float rad) {
    fly_img_round_rect_line(im, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, rad, 1.0f,
                            FLY_UI_GLASS_EDGE);
    /* the lit lip: the same outline, one pixel in, drawn only across the top */
    {
        float r = rad < w * 0.5f ? rad : w * 0.5f;
        float x0 = x + r * 0.55f, x1 = x + w - r * 0.55f;
        if (x1 > x0)
            fly_img_aa_line(im, x0, y + 0.9f, x1, y + 0.9f, 1.0f, FLY_UI_GLASS_LIP);
    }
}

/* A card is a pane of glass with a drop shadow: the same surface every group
 * uses, lifted off the world so an overlay reads as being in front of it. */
void fly_ui_card(fly_img *im, float x, float y, float w, float h, uint32_t fill) {
    fly_img_soft_shadow(im, x + 1, y + 3, w, h, FLY_UI_GLASS_RAD, 9.0f,
                        FLY_RGBA(0, 0, 0, 120));
    fly_img_frost(im, x, y, w, h, FLY_UI_GLASS_RAD, FLY_UI_GLASS_BLUR, fill);
    fly__pane_edge(im, x, y, w, h, FLY_UI_GLASS_RAD);
}

/* The surface a corner group sits on: one pane of frosted glass.
 *
 * What this replaced was three concentric rounded rects at rising alpha,
 * meant to read as a soft gradient. At the sizes the HUD actually uses they
 * read as a triple border — three visible edges around every corner of the
 * screen — and between them they put about eighty per cent of ink over the
 * world. One pane, one edge, and the blur rather than the ink is what buys
 * the contrast, so the panel can be thin enough to see the ground through.
 *
 * One edge, though, and not none. The pane went without a hairline for a
 * while on the argument that the change in focus is the edge — which is true
 * of a still and false of a game, where the world behind a corner group is
 * moving and about a third of the time it is a flat sky with nothing in it to
 * be out of focus. A pane over an overcast horizon had no boundary at all.
 *
 * The corner radius is asked for as "as round as this box goes", capped at
 * FLY_UI_GLASS_RAD. A fixed 12 px on a 22 px banner is a slab with the
 * corners filed off; the same request on a two-row cluster is a proper
 * rounded rect. One rule, and short things come out as capsules — which is
 * most of what stops a screen full of panes reading as a screen full of
 * boxes. */
void fly_ui_scrim(fly_img *im, float x, float y, float w, float h) {
    fly_img_frost(im, x, y, w, h, FLY_UI_GLASS_RAD, FLY_UI_GLASS_BLUR, FLY_UI_GLASS);
    fly__pane_edge(im, x, y, w, h, FLY_UI_GLASS_RAD);
}

/* A disc of the same glass: the radar, anything round and filled. Same edge
 * as every other pane, so a round group and a square one are the same
 * material seen from two sides rather than two different materials. */
void fly_ui_glass_disc(fly_img *im, float cx, float cy, float r, uint32_t tint) {
    fly_img_frost(im, cx - r, cy - r, 2.0f * r, 2.0f * r, r, FLY_UI_GLASS_BLUR, tint);
    fly_img_aa_circle(im, cx, cy, r - 0.5f, 1.0f, FLY_UI_GLASS_EDGE);
    fly_img_aa_line(im, cx - r * 0.5f, cy - r * 0.84f, cx + r * 0.5f, cy - r * 0.84f,
                    1.0f, FLY_UI_GLASS_LIP);
}

/* A full-screen page is the same material at full bleed: the world behind it,
 * blurred hard, levelled, and tinted one stop deeper than a corner group.
 *
 * Deeper because a page is read rather than looked through, and because it has
 * no edge to say where it is — the whole frame is the panel, so the only thing
 * that can announce it is the depth of the tint. It is still glass: the world
 * you were flying is out of focus behind the profile you are reading, which is
 * the point, and one lost airframe or six paragraphs of guide later it is still
 * there. It used to be lit from the other side, two white washes over the blur,
 * and every page in the game came out pale grey with white type on it. */
void fly_ui_page(fly_img *im) {
    fly_img_frost(im, -2.0f, -2.0f, (float)im->w + 4.0f, (float)im->h + 4.0f, 0.0f,
                  15.0f, FLY_UI_PAGE);
}

/* And as a band. The reticle is the one group that must not be a surface: a
 * filled disc in the middle of the frame hides the aircraft and the ground it
 * is pointed at, which is the whole of what the instrument is for. */
void fly_ui_glass_ring(fly_img *im, float cx, float cy, float r, float thick,
                       uint32_t tint) {
    fly_img_frost_ring(im, cx, cy, r, thick, FLY_UI_GLASS_BLUR, tint);
}

/* ---------------- the sprite set ----------------
 *
 * One sprite per kind of thing a row can be. Menus are read by shape before
 * they are read by word — a column of sprites tells you where the market ends
 * and the hangar begins without your having to parse a single label — and the
 * whole argument only works if the shape is legible at the size it is drawn.
 *
 * These used to be line drawings: four or five hairline strokes at 13% of the
 * icon's own width, in one colour, no fill. That weight cannot win. Thin enough
 * to look drawn and the glyph disappears at 11 px against a moving world; heavy
 * enough to survive and it closes up into a blob. A parcel and a chip drawn in
 * strokes at 10 px are the same small grey smudge, which is the state the
 * actions menu was in: twenty rows of identical smudges down the left margin,
 * doing none of the sorting the icons were there to do.
 *
 * So they are objects now rather than glyphs. Each is a silhouette built from
 * filled convex pieces, painted in four roles — body, lit face, shaded face and
 * trim — and each carries its own contact shadow so it sits on the panel rather
 * than in it. A silhouette reads at any size, and shading it in two tones is
 * what makes a crate look like a crate rather than like a square.
 *
 * Two ways to ask for one:
 *
 *   fly_ui_sprite  full colour, dark outline, for menus and headings. This is
 *                  the one that carries meaning by hue as well as by shape —
 *                  fuel is amber, the market is green-and-amber, a module is
 *                  violet — so a long list sorts itself before it is read.
 *   fly_ui_icon    the same artwork in one colour, for instruments. A gauge's
 *                  colour is its *state* — accent, warn, bad — and a sprite
 *                  that brought its own palette to that row would be arguing
 *                  with the only thing the row is trying to say.
 *
 * Both go through the same shapes, so the fuel can on the refuel row and the
 * fuel can over the fuel gauge cannot drift apart from each other. */

/* the four roles every sprite is painted in */
enum { ART_BODY, ART_LITE, ART_DARK, ART_TRIM, ART_ROLES };

/* rgb scaled by k, alpha untouched */
static uint32_t fly__tone(uint32_t c, float k) {
    uint32_t out = c & 0xFF000000u;
    int i;
    for (i = 0; i < 3; ++i) {
        float v = (float)((c >> (i * 8)) & 255u) * k;
        out |= (uint32_t)(v > 255.0f ? 255.0f : v < 0.0f ? 0.0f : v) << (i * 8);
    }
    return out;
}

/* the same colour at a fraction of its alpha */
static uint32_t fly__fade(uint32_t c, int a) {
    uint32_t v = ((c >> 24) & 255u) * (uint32_t)a / 255u;
    return (c & 0x00FFFFFFu) | (v << 24);
}

/* --- the palettes ---
 *
 * Saturated enough to carry across a menu row, light enough that white type
 * beside them is still the brightest thing in the row. Each is body / lit /
 * shaded / trim, and where a sprite only needs two of them the other two are
 * repeats rather than invented colours — an icon with four hues in it is a
 * picture, and a picture at 12 px is noise. */
typedef struct { uint32_t body, lite, dark, trim; } fly__pal;
static const fly__pal fly__pals[FLY_ICON_COUNT] = {
    /* Keyed rather than ordered. A table this long written positionally is one
     * inserted enum away from every sprite after it wearing somebody else's
     * colours, and the failure is silent — the icons still draw. */
    [FLY_ICON_NONE] = { 0, 0, 0, 0 },
    /* service: a drop of fuel, and the blue of every fluid in the game */
    [FLY_ICON_SERVICE] = { FLY_RGB(86, 176, 236), FLY_RGB(168, 226, 255),
                           FLY_RGB(38, 108, 168), FLY_RGB(214, 242, 255) },
    /* task: a parcel, kraft and strapping */
    [FLY_ICON_TASK] = { FLY_RGB(206, 158, 96), FLY_RGB(238, 200, 142),
                        FLY_RGB(140, 98, 54), FLY_RGB(96, 66, 40) },
    /* trade: what you are buying against what you are selling */
    [FLY_ICON_TRADE] = { FLY_RGB(112, 210, 140), FLY_RGB(168, 236, 190),
                         FLY_RGB(52, 132, 82), FLY_RGB(240, 186, 96) },
    /* part: a module, and the violet nothing else in the game uses */
    [FLY_ICON_PART] = { FLY_RGB(166, 150, 232), FLY_RGB(210, 200, 252),
                        FLY_RGB(88, 76, 148), FLY_RGB(238, 226, 150) },
    /* craft: an airframe seen from above, bare metal */
    [FLY_ICON_CRAFT] = { FLY_RGB(180, 196, 214), FLY_RGB(228, 238, 248),
                         FLY_RGB(96, 114, 136), FLY_RGB(120, 208, 244) },
    /* route: go there */
    [FLY_ICON_ROUTE] = { FLY_RGB(122, 214, 250), FLY_RGB(190, 238, 255),
                         FLY_RGB(48, 132, 176), FLY_RGB(190, 238, 255) },
    /* cash */
    [FLY_ICON_CASH] = { FLY_RGB(238, 194, 82), FLY_RGB(255, 232, 158),
                        FLY_RGB(160, 116, 30), FLY_RGB(122, 84, 20) },
    /* fuel: the can */
    [FLY_ICON_FUEL] = { FLY_RGB(226, 162, 66), FLY_RGB(252, 208, 130),
                        FLY_RGB(146, 94, 26), FLY_RGB(96, 62, 24) },
    /* power */
    [FLY_ICON_POWER] = { FLY_RGB(252, 214, 92), FLY_RGB(255, 244, 186),
                         FLY_RGB(184, 132, 24), FLY_RGB(255, 244, 186) },
    /* shield: structure */
    [FLY_ICON_SHIELD] = { FLY_RGB(120, 206, 196), FLY_RGB(186, 240, 232),
                          FLY_RGB(46, 122, 122), FLY_RGB(226, 250, 246) },
    /* wear: the glass runs warm as it empties */
    [FLY_ICON_WEAR] = { FLY_RGB(198, 210, 222), FLY_RGB(238, 246, 252),
                        FLY_RGB(104, 122, 140), FLY_RGB(232, 168, 92) },
    /* wind */
    [FLY_ICON_WIND] = { FLY_RGB(168, 214, 240), FLY_RGB(222, 242, 255),
                        FLY_RGB(90, 138, 174), FLY_RGB(222, 242, 255) },
    /* life: the cell */
    [FLY_ICON_LIFE] = { FLY_RGB(126, 216, 138), FLY_RGB(188, 244, 196),
                        FLY_RGB(52, 128, 66), FLY_RGB(214, 226, 236) },
    /* gust */
    [FLY_ICON_GUST] = { FLY_RGB(158, 200, 232), FLY_RGB(216, 238, 255),
                        FLY_RGB(80, 126, 162), FLY_RGB(240, 200, 120) },
    /* prop: nozzle and plume */
    [FLY_ICON_PROP] = { FLY_RGB(196, 208, 224), FLY_RGB(238, 246, 254),
                        FLY_RGB(96, 112, 134), FLY_RGB(255, 168, 74) },
    /* heat */
    [FLY_ICON_HEAT] = { FLY_RGB(232, 236, 244), FLY_RGB(255, 255, 255),
                        FLY_RGB(120, 134, 154), FLY_RGB(244, 96, 74) },
    /* flag: the staff is grey, the colours are the caller's business */
    [FLY_ICON_FLAG] = { FLY_RGB(120, 214, 250), FLY_RGB(196, 240, 255),
                        FLY_RGB(46, 126, 170), FLY_RGB(198, 212, 226) },
    /* ord: what is on the rails */
    [FLY_ICON_ORD] = { FLY_RGB(196, 206, 220), FLY_RGB(240, 246, 252),
                       FLY_RGB(98, 112, 132), FLY_RGB(238, 96, 78) },
    /* decoy: what it looks at instead */
    [FLY_ICON_DECOY] = { FLY_RGB(255, 206, 118), FLY_RGB(255, 246, 214),
                         FLY_RGB(196, 108, 30), FLY_RGB(255, 250, 236) },
    /* pilot: a flight suit and a visor */
    [FLY_ICON_PILOT] = { FLY_RGB(196, 214, 232), FLY_RGB(238, 246, 252),
                         FLY_RGB(94, 112, 134), FLY_RGB(120, 208, 244) },
    /* repair: oiled steel */
    [FLY_ICON_REPAIR] = { FLY_RGB(172, 190, 210), FLY_RGB(228, 240, 250),
                          FLY_RGB(78, 96, 118), FLY_RGB(255, 178, 84) }
};


/* --- normalized drawing ---
 *
 * Every shape below is written in units of the icon's own half-extent, so one
 * description serves an 11 px gauge glyph and a 26 px menu sprite and the two
 * cannot come out as different drawings. */
static void art_poly(fly_img *im, float cx, float cy, float r,
                     const float *uv, int n, uint32_t col) {
    float pts[2 * FLY_IMG_POLY_MAX];
    int i;
    if (n > FLY_IMG_POLY_MAX) n = FLY_IMG_POLY_MAX;
    for (i = 0; i < n; ++i) {
        pts[2 * i] = cx + uv[2 * i] * r;
        pts[2 * i + 1] = cy + uv[2 * i + 1] * r;
    }
    fly_img_aa_poly(im, pts, n, col);
}

static void art_box(fly_img *im, float cx, float cy, float r, float u0, float v0,
                    float u1, float v1, float rad, uint32_t col) {
    fly_img_round_rect(im, cx + u0 * r, cy + v0 * r, (u1 - u0) * r, (v1 - v0) * r,
                       rad * r, col);
}

/* the same box, lit down its length: what turns a rectangle into a face */
static void art_grad(fly_img *im, float cx, float cy, float r, float u0, float v0,
                     float u1, float v1, float rad, uint32_t top, uint32_t bot) {
    fly_img_round_rect_grad(im, cx + u0 * r, cy + v0 * r, (u1 - u0) * r, (v1 - v0) * r,
                            rad * r, top, bot);
}

static void art_disc(fly_img *im, float cx, float cy, float r, float u, float v,
                     float rr, uint32_t col) {
    fly_img_aa_disc(im, cx + u * r, cy + v * r, rr * r, col);
}

static void art_line(fly_img *im, float cx, float cy, float r, float u0, float v0,
                     float u1, float v1, float wid, uint32_t col) {
    float w = wid * r;
    fly_img_aa_line(im, cx + u0 * r, cy + v0 * r, cx + u1 * r, cy + v1 * r,
                    w < 1.0f ? 1.0f : w, col);
}

/* --- the artwork ---
 *
 * `c` is the four roles. Every sprite is drawn to the same vertical extent,
 * +/-0.9, and that is not only so they look like a family: a row of gauges is
 * measured on a lattice, and a glyph whose topmost lit pixel depends on its own
 * shape puts each row's ink at a different height — three pixels of drift
 * across four gauges, which reads as a wobble even though the rows are exactly
 * evenly spaced. */
static void fly__art(fly_img *im, fly_icon ic, float cx, float cy, float r,
                     const uint32_t *c) {
    switch (ic) {
    case FLY_ICON_SERVICE: {   /* a drop, about to fall */
        static const float drop[6] = { 0.0f, -0.95f, 0.56f, 0.34f, -0.56f, 0.34f };
        art_disc(im, cx, cy, r, 0.0f, 0.26f, 0.62f, c[ART_BODY]);
        art_poly(im, cx, cy, r, drop, 3, c[ART_BODY]);
        art_disc(im, cx, cy, r, -0.20f, 0.24f, 0.24f, c[ART_LITE]);
        art_disc(im, cx, cy, r, 0.26f, 0.40f, 0.20f, fly__fade(c[ART_DARK], 150));
        break;
    }
    case FLY_ICON_TASK:        /* a parcel, strapped */
        art_grad(im, cx, cy, r, -0.84f, -0.72f, 0.84f, 0.72f, 0.14f,
                 c[ART_LITE], c[ART_BODY]);
        art_box(im, cx, cy, r, -0.84f, -0.72f, 0.84f, -0.40f, 0.14f,
                fly__fade(c[ART_LITE], 190));                       /* the lid */
        art_box(im, cx, cy, r, -0.17f, -0.72f, 0.17f, 0.72f, 0.0f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.84f, -0.04f, 0.84f, 0.22f, 0.0f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.84f, 0.54f, 0.84f, 0.72f, 0.10f,
                fly__fade(c[ART_DARK], 170));
        break;
    case FLY_ICON_TRADE: {     /* two arrows passing: in at a price, out at one */
        static const float up[6] = { 0.24f, -0.92f, 0.92f, -0.30f, 0.24f, 0.14f };
        static const float dn[6] = { -0.24f, 0.92f, -0.92f, 0.30f, -0.24f, -0.14f };
        art_box(im, cx, cy, r, -0.92f, -0.56f, 0.34f, -0.22f, 0.12f, c[ART_BODY]);
        art_poly(im, cx, cy, r, up, 3, c[ART_BODY]);
        art_box(im, cx, cy, r, -0.34f, 0.22f, 0.92f, 0.56f, 0.12f, c[ART_TRIM]);
        art_poly(im, cx, cy, r, dn, 3, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.92f, -0.56f, 0.34f, -0.42f, 0.08f,
                fly__fade(c[ART_LITE], 150));
        break;
    }
    case FLY_ICON_PART:        /* a module: a chip with its legs out */
        art_box(im, cx, cy, r, -0.95f, -0.42f, 0.95f, -0.20f, 0.06f, c[ART_DARK]);
        art_box(im, cx, cy, r, -0.95f, 0.20f, 0.95f, 0.42f, 0.06f, c[ART_DARK]);
        art_box(im, cx, cy, r, -0.42f, -0.95f, -0.20f, 0.95f, 0.06f, c[ART_DARK]);
        art_box(im, cx, cy, r, 0.20f, -0.95f, 0.42f, 0.95f, 0.06f, c[ART_DARK]);
        art_grad(im, cx, cy, r, -0.66f, -0.66f, 0.66f, 0.66f, 0.16f,
                 c[ART_LITE], c[ART_BODY]);
        art_box(im, cx, cy, r, -0.30f, -0.30f, 0.30f, 0.30f, 0.08f,
                fly__fade(c[ART_DARK], 200));
        art_disc(im, cx, cy, r, -0.46f, -0.46f, 0.10f, c[ART_TRIM]);
        break;
    case FLY_ICON_CRAFT: {     /* an airframe from above */
        /* Pulled up by a tenth against the geometric centre: the nose is a
         * thin triangle and the tail a solid slab, so a planform centred on
         * its origin has all its weight below the line and reads low in a row
         * of sprites that are all centred on their ink. */
        static const float wing[8] = { -0.98f, -0.20f, 0.98f, -0.20f,
                                       0.70f, 0.26f, -0.70f, 0.26f };
        static const float tail[8] = { -0.44f, 0.44f, 0.44f, 0.44f,
                                       0.28f, 0.76f, -0.28f, 0.76f };
        static const float nose[6] = { 0.0f, -0.96f, 0.20f, -0.46f, -0.20f, -0.46f };
        art_poly(im, cx, cy, r, wing, 4, c[ART_BODY]);
        art_poly(im, cx, cy, r, tail, 4, c[ART_BODY]);
        art_box(im, cx, cy, r, -0.20f, -0.64f, 0.20f, 0.78f, 0.19f, c[ART_LITE]);
        art_poly(im, cx, cy, r, nose, 3, c[ART_LITE]);
        art_box(im, cx, cy, r, -0.98f, 0.06f, 0.98f, 0.26f, 0.08f,
                fly__fade(c[ART_DARK], 160));
        art_disc(im, cx, cy, r, 0.0f, -0.32f, 0.17f, c[ART_TRIM]);
        break;
    }
    case FLY_ICON_ROUTE: {     /* a chevron: that way */
        static const float top[8] = { -0.62f, -0.88f, -0.16f, -0.88f,
                                      0.62f, 0.0f, 0.16f, 0.0f };
        static const float bot[8] = { 0.62f, 0.0f, 0.16f, 0.0f,
                                      -0.62f, 0.88f, -0.16f, 0.88f };
        art_poly(im, cx, cy, r, top, 4, c[ART_LITE]);
        art_poly(im, cx, cy, r, bot, 4, c[ART_BODY]);
        break;
    }
    case FLY_ICON_CASH: {      /* a struck token */
        static const float mark[8] = { 0.0f, -0.46f, 0.38f, 0.0f, 0.0f, 0.46f, -0.38f, 0.0f };
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.94f, c[ART_DARK]);
        art_disc(im, cx, cy, r, 0.0f, -0.04f, 0.80f, c[ART_BODY]);
        art_disc(im, cx, cy, r, -0.24f, -0.28f, 0.36f, fly__fade(c[ART_LITE], 130));
        art_poly(im, cx, cy, r, mark, 4, c[ART_TRIM]);
        break;
    }
    /* --- the instrument set --- */
    case FLY_ICON_FUEL:        /* the can: a handle, a cap and a rib */
        art_box(im, cx, cy, r, -0.62f, -0.96f, 0.06f, -0.58f, 0.12f, c[ART_TRIM]);
        art_box(im, cx, cy, r, 0.30f, -0.96f, 0.72f, -0.60f, 0.10f, c[ART_TRIM]);
        art_grad(im, cx, cy, r, -0.60f, -0.62f, 0.68f, 0.94f, 0.16f,
                 c[ART_LITE], c[ART_BODY]);
        art_line(im, cx, cy, r, -0.32f, -0.30f, 0.40f, 0.58f, 0.13f,
                 fly__fade(c[ART_DARK], 210));
        art_line(im, cx, cy, r, 0.40f, -0.30f, -0.32f, 0.58f, 0.13f,
                 fly__fade(c[ART_DARK], 210));
        art_box(im, cx, cy, r, -0.60f, 0.64f, 0.68f, 0.94f, 0.14f,
                fly__fade(c[ART_DARK], 150));
        break;
    case FLY_ICON_POWER: {     /* the bolt */
        static const float up[6] = { 0.36f, -0.92f, -0.44f, 0.10f, 0.14f, 0.10f };
        static const float dn[6] = { -0.34f, 0.92f, 0.44f, -0.08f, -0.12f, -0.08f };
        art_poly(im, cx, cy, r, up, 3, c[ART_BODY]);
        art_poly(im, cx, cy, r, dn, 3, c[ART_BODY]);
        art_poly(im, cx, cy, r, up, 3, fly__fade(c[ART_LITE], 90));
        break;
    }
    case FLY_ICON_SHIELD: {    /* structure */
        static const float sh[10] = { -0.76f, -0.86f, 0.76f, -0.86f, 0.68f, 0.16f,
                                      0.0f, 0.92f, -0.68f, 0.16f };
        static const float in[10] = { -0.50f, -0.62f, 0.50f, -0.62f, 0.45f, 0.12f,
                                      0.0f, 0.60f, -0.45f, 0.12f };
        art_poly(im, cx, cy, r, sh, 5, c[ART_DARK]);
        art_poly(im, cx, cy, r, in, 5, c[ART_BODY]);
        art_box(im, cx, cy, r, -0.46f, -0.62f, -0.10f, 0.34f, 0.08f,
                fly__fade(c[ART_LITE], 170));
        break;
    }
    case FLY_ICON_WEAR: {      /* the glass, half run through */
        static const float top[6] = { -0.56f, -0.70f, 0.56f, -0.70f, 0.0f, 0.02f };
        static const float bot[6] = { -0.56f, 0.70f, 0.56f, 0.70f, 0.0f, -0.02f };
        static const float sand[6] = { -0.34f, 0.70f, 0.34f, 0.70f, 0.0f, 0.24f };
        art_poly(im, cx, cy, r, top, 3, fly__fade(c[ART_BODY], 150));
        art_poly(im, cx, cy, r, bot, 3, fly__fade(c[ART_BODY], 150));
        art_poly(im, cx, cy, r, sand, 3, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.68f, -0.92f, 0.68f, -0.68f, 0.10f, c[ART_LITE]);
        art_box(im, cx, cy, r, -0.68f, 0.68f, 0.68f, 0.92f, 0.10f, c[ART_LITE]);
        break;
    }
    case FLY_ICON_WIND: {      /* three streamlines, the middle one running on */
        static const float a[8] = { -0.92f, -0.86f, 0.36f, -0.86f, 0.46f, -0.62f, -0.92f, -0.62f };
        static const float b[8] = { -0.92f, -0.14f, 0.72f, -0.14f, 0.82f, 0.12f, -0.92f, 0.12f };
        static const float d[8] = { -0.92f, 0.60f, 0.10f, 0.60f, 0.20f, 0.86f, -0.92f, 0.86f };
        art_poly(im, cx, cy, r, a, 4, fly__fade(c[ART_BODY], 210));
        art_poly(im, cx, cy, r, b, 4, c[ART_LITE]);
        art_poly(im, cx, cy, r, d, 4, fly__fade(c[ART_BODY], 210));
        break;
    }
    case FLY_ICON_GUST: {      /* the same, torn: wind that is not steady */
        static const float a[8] = { -0.92f, -0.86f, -0.16f, -0.86f, -0.06f, -0.62f, -0.92f, -0.62f };
        static const float b[8] = { 0.20f, -0.86f, 0.86f, -0.86f, 0.96f, -0.62f, 0.30f, -0.62f };
        static const float d[8] = { -0.92f, 0.06f, 0.52f, 0.06f, 0.62f, 0.32f, -0.92f, 0.32f };
        static const float e[6] = { 0.30f, 0.32f, 0.62f, 0.32f, 0.18f, 0.90f };
        art_poly(im, cx, cy, r, a, 4, c[ART_LITE]);
        art_poly(im, cx, cy, r, b, 4, fly__fade(c[ART_BODY], 210));
        art_poly(im, cx, cy, r, d, 4, c[ART_LITE]);
        art_poly(im, cx, cy, r, e, 3, c[ART_TRIM]);
        break;
    }
    case FLY_ICON_LIFE:        /* the cell, and what is left in it */
        /* Narrow, with a terminal proud of the top. It used to be as wide as
         * the fuel can and rounded the same way, and at 19 px the two
         * silhouettes were the same rounded rectangle — which on the craft
         * cluster put the tank gauge and the airframe-life gauge under
         * indistinguishable sprites. */
        art_box(im, cx, cy, r, -0.20f, -0.96f, 0.20f, -0.72f, 0.06f, c[ART_TRIM]);
        art_grad(im, cx, cy, r, -0.46f, -0.74f, 0.46f, 0.94f, 0.10f,
                 c[ART_LITE], c[ART_BODY]);
        art_box(im, cx, cy, r, -0.46f, -0.74f, 0.46f, -0.20f, 0.10f,
                fly__fade(c[ART_DARK], 200));
        art_box(im, cx, cy, r, -0.28f, 0.06f, 0.28f, 0.28f, 0.04f,
                fly__fade(c[ART_TRIM], 210));
        art_box(im, cx, cy, r, -0.28f, 0.46f, 0.28f, 0.68f, 0.04f,
                fly__fade(c[ART_TRIM], 210));
        break;
    case FLY_ICON_PROP: {      /* a nozzle and its plume */
        static const float bell[8] = { -0.34f, -0.94f, 0.34f, -0.94f,
                                       0.74f, -0.16f, -0.74f, -0.16f };
        static const float fl[6] = { -0.44f, -0.18f, 0.44f, -0.18f, 0.0f, 0.96f };
        static const float in[6] = { -0.20f, -0.12f, 0.20f, -0.12f, 0.0f, 0.52f };
        art_poly(im, cx, cy, r, fl, 3, c[ART_TRIM]);
        art_poly(im, cx, cy, r, in, 3, fly__fade(c[ART_LITE], 235));
        art_poly(im, cx, cy, r, bell, 4, c[ART_BODY]);
        art_box(im, cx, cy, r, -0.32f, -0.94f, -0.04f, -0.20f, 0.06f,
                fly__fade(c[ART_LITE], 170));
        break;
    }
    case FLY_ICON_HEAT:        /* what the air is doing to the hull */
        /* The whole instrument sits left of centre because the scale marks
         * hang off its right, and a glyph is centred on its ink and not on
         * its origin: drawn about the stem it read two pixels right of every
         * other sprite in the column. */
        art_disc(im, cx, cy, r, -0.34f, 0.54f, 0.40f, c[ART_BODY]);
        art_box(im, cx, cy, r, -0.54f, -0.92f, -0.14f, 0.58f, 0.20f, c[ART_BODY]);
        art_disc(im, cx, cy, r, -0.34f, 0.54f, 0.26f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.43f, -0.44f, -0.25f, 0.58f, 0.09f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.50f, -0.86f, -0.38f, -0.50f, 0.06f,
                fly__fade(c[ART_LITE], 200));
        art_box(im, cx, cy, r, 0.10f, -0.62f, 0.84f, -0.44f, 0.07f, c[ART_DARK]);
        art_box(im, cx, cy, r, 0.10f, -0.16f, 0.84f, 0.02f, 0.07f, c[ART_DARK]);
        break;
    case FLY_ICON_FLAG: {      /* a pennant on a staff: the colours you fly */
        static const float pen[6] = { -0.44f, -0.88f, 0.88f, -0.46f, -0.44f, 0.02f };
        art_poly(im, cx, cy, r, pen, 3, c[ART_BODY]);
        art_poly(im, cx, cy, r, pen, 3, fly__fade(c[ART_LITE], 70));
        art_box(im, cx, cy, r, -0.60f, -0.92f, -0.36f, 0.92f, 0.10f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.78f, 0.78f, -0.18f, 0.92f, 0.07f, c[ART_DARK]);
        break;
    }
    case FLY_ICON_ORD: {       /* a finned round, nose up */
        static const float nose[6] = { 0.0f, -0.94f, 0.34f, -0.28f, -0.34f, -0.28f };
        static const float lf[6] = { -0.34f, 0.34f, -0.34f, 0.92f, -0.86f, 0.92f };
        static const float rf[6] = { 0.34f, 0.34f, 0.34f, 0.92f, 0.86f, 0.92f };
        art_poly(im, cx, cy, r, lf, 3, c[ART_DARK]);
        art_poly(im, cx, cy, r, rf, 3, c[ART_DARK]);
        art_grad(im, cx, cy, r, -0.34f, -0.32f, 0.34f, 0.92f, 0.10f,
                 c[ART_LITE], c[ART_BODY]);
        art_poly(im, cx, cy, r, nose, 3, c[ART_TRIM]);
        break;
    }
    case FLY_ICON_REPAIR: {    /* a cog: what the pad does to the machine */
        int t;
        for (t = 0; t < 8; ++t) {
            float a = (float)t * 0.7853982f, ca = cosf(a), sa = sinf(a);
            float uv[8];
            uv[0] = ca * 0.50f - sa * 0.26f; uv[1] = sa * 0.50f + ca * 0.26f;
            uv[2] = ca * 0.96f - sa * 0.17f; uv[3] = sa * 0.96f + ca * 0.17f;
            uv[4] = ca * 0.96f + sa * 0.17f; uv[5] = sa * 0.96f - ca * 0.17f;
            uv[6] = ca * 0.50f + sa * 0.26f; uv[7] = sa * 0.50f - ca * 0.26f;
            art_poly(im, cx, cy, r, uv, 4, c[ART_BODY]);
        }
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.70f, c[ART_BODY]);
        art_disc(im, cx, cy, r, -0.16f, -0.16f, 0.46f, fly__fade(c[ART_LITE], 160));
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.30f, c[ART_DARK]);
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.15f, c[ART_TRIM]);
        break;
    }
    case FLY_ICON_PILOT: {     /* head and shoulders, in a helmet */
        static const float sh[8] = { -0.92f, 0.94f, -0.62f, 0.24f,
                                     0.62f, 0.24f, 0.92f, 0.94f };
        art_poly(im, cx, cy, r, sh, 4, c[ART_BODY]);
        art_disc(im, cx, cy, r, 0.0f, -0.34f, 0.56f, c[ART_LITE]);
        art_box(im, cx, cy, r, -0.50f, -0.40f, 0.50f, -0.10f, 0.14f, c[ART_TRIM]);
        art_box(im, cx, cy, r, -0.10f, 0.24f, 0.10f, 0.94f, 0.05f,
                fly__fade(c[ART_DARK], 190));
        break;
    }
    case FLY_ICON_DECOY:       /* a flare, throwing light */
        art_line(im, cx, cy, r, -0.92f, -0.92f, -0.34f, -0.34f, 0.16f, c[ART_DARK]);
        art_line(im, cx, cy, r, 0.92f, -0.92f, 0.34f, -0.34f, 0.16f, c[ART_DARK]);
        art_line(im, cx, cy, r, -0.92f, 0.92f, -0.34f, 0.34f, 0.16f, c[ART_DARK]);
        art_line(im, cx, cy, r, 0.92f, 0.92f, 0.34f, 0.34f, 0.16f, c[ART_DARK]);
        art_line(im, cx, cy, r, 0.0f, -0.95f, 0.0f, -0.40f, 0.18f, c[ART_BODY]);
        art_line(im, cx, cy, r, 0.0f, 0.95f, 0.0f, 0.40f, 0.18f, c[ART_BODY]);
        art_line(im, cx, cy, r, -0.95f, 0.0f, -0.40f, 0.0f, 0.18f, c[ART_BODY]);
        art_line(im, cx, cy, r, 0.95f, 0.0f, 0.40f, 0.0f, 0.18f, c[ART_BODY]);
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.40f, c[ART_BODY]);
        art_disc(im, cx, cy, r, 0.0f, 0.0f, 0.22f, c[ART_TRIM]);
        break;
    default: break;
    }
}

/* The outline and the contact shadow.
 *
 * A sprite drawn straight onto a panel is a sticker; one with a dark rim and a
 * shadow under it is an object lying on the panel, and the difference is what
 * "pop" actually is. Both passes are the same artwork at a larger radius —
 * scaling the whole design about its own centre gives a true silhouette, so a
 * shape does not need a second, thicker description of itself to be outlined.
 *
 * The rim is opaque. It has to be: the artwork is convex pieces that overlap,
 * and a translucent pass would double-blend along every seam and draw its own
 * creases into the outline. */
static void fly__art_relief(fly_img *im, fly_icon ic, float cx, float cy, float r,
                            int rim) {
    uint32_t shadow[ART_ROLES], edge[ART_ROLES];
    float t = r * 0.14f;
    int i;
    if (t < 0.9f) t = 0.9f;
    if (t > 2.2f) t = 2.2f;
    for (i = 0; i < ART_ROLES; ++i) {
        shadow[i] = FLY_RGBA(0, 0, 0, 96);
        edge[i] = FLY_RGBA(8, 13, 20, 255);
    }
    fly__art(im, ic, cx + t * 0.5f, cy + t * 0.9f, r + t, shadow);
    if (rim) fly__art(im, ic, cx, cy, r + t * 0.75f, edge);
}

void fly_ui_sprite(fly_img *im, fly_icon ic, float cx, float cy, float s) {
    const fly__pal *p;
    uint32_t c[ART_ROLES];
    float r = s * 0.5f;
    if (ic <= FLY_ICON_NONE || ic >= FLY_ICON_COUNT) return;
    /* The palette is opaque (FLY_RGB), and it has to be: the artwork is convex
     * pieces that overlap, and a translucent role would double-blend along
     * every seam and draw creases the design does not have. Anything that
     * wants to fade goes through fly__fade on the shape rather than on the
     * role, where the overlap is known. */
    p = &fly__pals[ic];
    c[ART_BODY] = p->body;
    c[ART_LITE] = p->lite;
    c[ART_DARK] = p->dark;
    c[ART_TRIM] = p->trim;
    fly__art_relief(im, ic, cx, cy, r, 1);
    fly__art(im, ic, cx, cy, r, c);
}

void fly_ui_icon(fly_img *im, fly_icon ic, float cx, float cy, float s, uint32_t col) {
    uint32_t c[ART_ROLES];
    float r = s * 0.5f;
    if (ic <= FLY_ICON_NONE || ic >= FLY_ICON_COUNT) return;
    /* One hue, four values. An instrument's colour is its state — accent,
     * warn, bad — so the sprite is painted in whatever the gauge is saying and
     * keeps only the shading that makes the shape readable. No rim: a hard
     * black outline around a dimmed white glyph reads as grime. */
    c[ART_BODY] = col;
    c[ART_LITE] = fly__tone(col, 1.32f);
    c[ART_DARK] = fly__tone(col, 0.42f);
    c[ART_TRIM] = fly__tone(col, 0.62f);
    fly__art_relief(im, ic, cx, cy, r, 0);
    fly__art(im, ic, cx, cy, r, c);
}
void fly_ui_bar(fly_img *im, float x, float y, float w, float frac, uint32_t color) {
    frac = fly_clampf(frac, 0.0f, 1.0f);
    fly_img_round_rect(im, x + 1, y + 1, w, 5, 2.5f, FLY_RGBA(0, 0, 0, 90)); /* shadow */
    fly_img_round_rect(im, x, y, w, 5, 2.5f, FLY_RGBA(92, 116, 132, 175)); /* track */
    if (frac > 0.02f) fly_img_round_rect(im, x, y, w * frac, 5, 2.5f, color);
}

/* elide s into out so it fits max_w at the given size */
static void fit_line(char *out, size_t n, fly_font_face face, float px, float max_w, const char *s) {
    size_t len = strlen(s);
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
    while (len > 3 && fly_text_width(face, px, out) > max_w) {
        out[len - 3] = '.'; out[len - 2] = '.'; out[len - 1] = '\0';
        --len;
    }
}

/* ---------------- the cluster box model ----------------
 *
 * Every corner group is a content rect inflated by HUD_PAD on all four sides.
 * The clusters used to place their washes by eye — minus six here, minus four
 * there — so each corner had a different amount of air in it and the wash sat
 * off-centre behind its own text. One pad, applied in one place, and the box a
 * cluster reports is the box it draws.
 *
 * Keys are left, values are right, and the gap between them is whatever is
 * left over. A key column wide enough for the longest key and no wider puts
 * "CASH" four pixels from its own number; letting the value hang off the right
 * edge of the box gives the pair the width of the cluster to breathe in, and
 * costs nothing, because the numbers wanted to be right-aligned anyway. */
#define HUD_PAD FLY_UI_PAD
#define HUD_ROW_H FLY_UI_ROW
/* The sprite column, and the column the value or the bar starts in. Both live
 * in the header now (FLY_HUD_ICON_W, FLY_HUD_GAUGE_X) because the lattice test
 * measures the gutter. The gutter was 54 back when the key was four letters
 * wide; leaving it there after the letters became a sprite left forty pixels of
 * nothing down the middle of every cluster, which is a hole the eye reads as a
 * misalignment. */
#define HUD_ICON_W FLY_HUD_ICON_W
#define HUD_GAUGE_X FLY_HUD_GAUGE_X

/* Every cluster is a whole number of rows tall. The heights used to be sums of
 * a title height, a pitch and a fudge — 19 + 3*13 + 5 — which is how three
 * blocks of the same kind of content ended up with three different rhythms. */
#define HUD_TASK_W 230.0f
#define HUD_TASK_H (HUD_ROW_H * 2.0f)
/* The width the four gauge bars want, which is the floor rather than the
 * answer: the card is as wide as its title row needs — see hud_craft_w. */
#define HUD_CRAFT_W 134.0f
#define HUD_TITLE_GAP 10.0f   /* between the name and the figures beside it */
#define HUD_TITLE_SLACK 3.0f  /* so a name that measures exactly is not elided */
/* The craft card's height is content-dependent and lives in hud_craft_rows;
 * a spacecraft, a beam and a loaded rack each add a row that means nothing to
 * anybody not carrying one, so the card grows rather than every aeroplane in
 * the game carrying three dead gauges. */
#define HUD_WX_H (HUD_ROW_H * 2.0f)
#define HUD_STACK_GAP 9.0f /* between the two boxes in the bottom-left stack */
/* A tape shorter than this is not an instrument, it is a decoration. When the
 * frame is too short to hold the tapes and the whole bottom-left stack, the
 * weather rose is what goes: wind and gust are the two readings you can fly
 * without, and a tape you cannot read a number off is worse than no rose. */
#define HUD_TAPE_MIN 52.0f
#define HUD_TAPE_PAD 17.0f /* wash and label above/below the tape's own line */

enum { HUD_ESTATE, HUD_TASK, HUD_CRAFT, HUD_WX, HUD_RADAR,
       HUD_TAPE_L, HUD_TAPE_R, HUD_HEAD, HUD_BOXES };

/* How tall the craft card actually is, in rows.
 *
 * A function rather than two constants because the card now grows for two
 * different reasons — the rocket stack, and a magazine — and they can both be
 * true at once. The layout and the drawing call this same function, so a row
 * that is drawn is a row the box was measured for; the old arrangement had the
 * height as a constant picked by one `if` and the content by another, which is
 * exactly the shape that puts a gauge through the bottom edge of its own card.
 *
 * Name, fuel, power, hull, wear; then a heat row whenever anything can heat the
 * airframe, a propellant row for the stack, and an arms row for whatever is on
 * the rails. */
static int hud_craft_rows(const fly_game *g) {
    int rows = 5;
    if (g->player.airframe.propellant_cap > 0.0f) rows += 2;          /* propellant + heat */
    else if (g->player.airframe.beam_heat > 0.0f) rows += 1;          /* heat alone */
    if (g->player.airframe.ord_kind || g->player.airframe.cm_kind) rows += 1;
    return rows;
}

/* The condition figures on the craft card's title row, hard right of the name.
 *
 * A function because two things print it: the card, and the card's own width.
 * The same argument hud_craft_rows carries — a row that is drawn is a row the
 * box was measured for — applies across a row as well as down a card.
 *
 * The g figure carries the limit it is measured against. On its own it is a
 * number with no scale — 3.1 is a brisk turn in a Vulture and most of the way
 * to bending a Slagback — and the whole point of giving the wing a structural
 * envelope is that the pilot can see where the edge is before they arrive at
 * it. A drone carries its endurance with it, because a battery has no gauge a
 * pilot can read in litres. */
static void hud_craft_figures(const fly_game *g, char *buf, size_t n) {
    float glim = fly_airframe_g_limit(&g->player.airframe);
    if (g->player.airframe.kind == FLY_CRAFT_DRONE) {
        float draw = g->player.airframe.burn_rate * (0.12f + 0.88f * g->player.craft.throttle);
        float mins = draw > 1e-6f ? g->player.craft.fuel / draw / 60.0f : 999.0f;
        snprintf(buf, n, "%.0f min  %.1f/%.1fg", mins > 999.0f ? 999.0f : mins,
                 g->player.craft.gload, glim);
    } else {
        snprintf(buf, n, "%.1f/%.1fg", g->player.craft.gload, glim);
    }
}

/* The craft card's content width, and the same story as hud_estate_w's.
 *
 * It was a flat 134 px, which is the width the four gauge bars want and has
 * nothing to do with the title row above them: sprite column, name, and the
 * figures hard right came to 25 + 57 + 10 + 54 for the *shortest* airframe in
 * the game, so the name was elided on every frame the HUD has ever drawn.
 * "Skylar.." is in every screenshot in the gallery. A drone is worse, since it
 * carries its endurance on the same row.
 *
 * So the card is as wide as what it has to say, floored at the width the
 * gauges want and capped like the task summary's so a narrow frame gets its
 * width back and elides instead. The weather rose under it takes the same
 * number: the bottom-left stack is one column, so it is one width. */
static float hud_craft_w(const fly_game *g, float frame_w) {
    char buf[48];
    float cap = frame_w * 0.30f, w;
    hud_craft_figures(g, buf, sizeof buf);
    /* HUD_TITLE_SLACK, and it is not a fudge: the width is computed from the
     * same two text measurements the drawing then fits into, so without it the
     * name lands at exactly the space it is given and whether it is elided is
     * decided by the last bit of a float. Two airframes of eleven came out
     * elided on a card sized for them. */
    w = HUD_GAUGE_X + fly_text_width(FLY_FONT_UI, FLY_FS_VAL, g->player.airframe.name)
        + HUD_TITLE_GAP + HUD_TITLE_SLACK
        + fly_text_width(FLY_FONT_MONO, FLY_FS_KEY, buf);
    if (w < HUD_CRAFT_W) w = HUD_CRAFT_W;
    return w < cap ? w : cap;
}

/* The estate cluster's content width: sprite column, a gap, and the widest of
 * the three figures it will actually print. It used to be a flat 124 px, which
 * on a young operator's three-digit balance left sixty pixels of empty glass
 * between the coin and the number — the single biggest thing making the top
 * left read as a slab rather than as a readout. */
static float hud_estate_w(const fly_game *g) {
    char buf[24];
    float wide = 0.0f, t;
    int k;
    for (k = 0; k < 4; ++k) {
        if (k == 0) snprintf(buf, sizeof buf, "%ld", g->player.tokens);
        else if (k == 1) snprintf(buf, sizeof buf, "%.1f", fly_game_reserve_hulls(g));
        else if (k == 2) snprintf(buf, sizeof buf, "100%%");
        else snprintf(buf, sizeof buf, "%s %+d", fly_faction_tag(g->player.faction), -100);
        t = fly_text_width(FLY_FONT_MONO, FLY_FS_VAL, buf);
        if (t > wide) wide = t;
    }
    return HUD_ICON_W + 14.0f + wide;
}

/* The cluster is one row taller while the operator is wearing colours. An
 * unaligned one is told nothing, because there is nothing to tell — a row
 * reading "FREE 0" would be four permanent pixels of no information. */
static int hud_estate_rows(const fly_game *g) {
    return 2 + (fly_game_fitted(g) ? 1 : 0) +
           (g->player.faction != FLY_FACTION_FREE ? 1 : 0);
}

/* the contract the task cluster is showing, or -1 */
static int hud_task(const fly_game *g) {
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i)
        if (g->contracts[i].state == 2) return i;
    return -1;
}

/* The two lines of the task summary, written once so the box can be measured
 * from the same strings that get drawn in it. A fixed 230 px box around a line
 * that is usually 150 left a hole where the wash had nothing to sit behind. */
#define HUD_TASK_GUTTER 24.0f /* the bearing chevron's column, left of the text */
static void hud_task_text(const fly_game *g, int idx, char *sum, size_t ns,
                          char *det, size_t nd) {
    const fly_contract *ct = &g->contracts[idx];
    const fly_location *L = &g->world.loc[ct->to];
    double left = ct->deadline - g->time_s;
    snprintf(sum, ns, "%.0f %s > %s", ct->qty, fly_resource_name((fly_resource)ct->res), L->name);
    snprintf(det, nd, "%.1f km   %s%.0f min   %ld tk",
             fly_world_dist(L->pos, fly_wpos_of(g->player.craft.pos)) / 1000.0f,
             left < 0 ? "-" : "", fabs(left) / 60.0, ct->reward);
}

static float hud_task_w(const fly_game *g, int idx, float frame_w) {
    char sum[64], det[64];
    float a, b, cap = frame_w * 0.26f;
    /* Nothing on the books is a state, not a sentence. An empty manifest glyph
     * says it in a 14 px square; "NO TASK" said it in seventy pixels of type
     * that the eye has to read before it can discover there is nothing to
     * read. */
    if (idx < 0) return 14.0f;
    hud_task_text(g, idx, sum, sizeof sum, det, sizeof det);
    a = fly_text_width(FLY_FONT_UI, FS_B, sum) + HUD_TASK_GUTTER;
    b = fly_text_width(FLY_FONT_MONO, FS_S, det);
    if (b > a) a = b;
    if (cap > HUD_TASK_W) cap = HUD_TASK_W;
    return a < cap ? a : cap;
}

static fly_rectf hud_box(float x, float y, float cw, float ch) {
    fly_rectf b;
    b.x = x; b.y = y; b.w = cw + 2.0f * HUD_PAD; b.h = ch + 2.0f * HUD_PAD;
    return b;
}

/* content rect of a box: what the cluster is allowed to draw in */
static fly_rectf hud_inner(fly_rectf b) {
    fly_rectf c;
    c.x = b.x + HUD_PAD; c.y = b.y + HUD_PAD;
    c.w = b.w - 2.0f * HUD_PAD; c.h = b.h - 2.0f * HUD_PAD;
    return c;
}

/* The right-hand instrument margin. Everything on that side lines up on it —
 * task summary, altitude tape, radar — because the touch throttle owns the
 * column outboard of it and a HUD with two different right edges looks like an
 * accident even when nothing actually collides. */
#define HUD_RIGHT(w) ((w) - FLY_TOUCH_STRIP - HUD_STACK_GAP)

static int hud_boxes(float w, float h, const fly_game *g, fly_rectf *r) {
    float m = HUD_MARGIN;
    float radar_r = g->player.airframe.radar_range > 0 ? 44.0f : 36.0f;
    float est_h = HUD_ROW_H * (float)hud_estate_rows(g);
    /* The task summary is as wide as its own two lines, capped so it gives
     * width back on a narrow frame. It is the widest thing on the top row and
     * it is elided anyway; holding it at 230 px squeezed the heading strip
     * until the strip drew through it, and left the wash sitting behind
     * nothing whenever the line was short. */
    float task_w = hud_task_w(g, hud_task(g), w);
    float task_h = hud_task(g) >= 0 ? HUD_TASK_H : HUD_ROW_H;

    r[HUD_ESTATE] = hud_box(m, m, hud_estate_w(g), est_h);
    /* under the nav buttons, not through them */
    r[HUD_TASK] = hud_box(0, FLY_NAV_Y + FLY_NAV_ICON_R + HUD_STACK_GAP, task_w, task_h);
    r[HUD_TASK].x = HUD_RIGHT(w) - r[HUD_TASK].w;
    float craft_w = hud_craft_w(g, w);
    r[HUD_CRAFT] = hud_box(m, 0, craft_w,
                           HUD_ROW_H * (float)hud_craft_rows(g));
    r[HUD_CRAFT].y = h - m - r[HUD_CRAFT].h;
    r[HUD_WX] = hud_box(m, 0, craft_w, HUD_WX_H);
    r[HUD_WX].y = r[HUD_CRAFT].y - HUD_STACK_GAP - r[HUD_WX].h;
    /* a zero-size box is an absent one; see HUD_TAPE_MIN */
    if (r[HUD_WX].y - h * 0.5f - HUD_TAPE_PAD < HUD_TAPE_MIN)
        r[HUD_WX] = (fly_rectf){ 0, 0, 0, 0 };
    r[HUD_RADAR] = (fly_rectf){ HUD_RIGHT(w) - 2 * radar_r, h - m - 2 * radar_r,
                                2 * radar_r, 2 * radar_r };

    /* The tape columns are layout, not decoration: they are the tallest thing
     * on the frame and the only widgets whose height depends on everything
     * else. Reporting them here is what makes "the airspeed tape runs through
     * the weather rose at 640x360" a failing test rather than a screenshot. */
    {
        float cy = h * 0.5f;
        float floor_y = r[HUD_WX].w > 0.0f ? r[HUD_WX].y : r[HUD_CRAFT].y;
        float lim = (floor_y < r[HUD_RADAR].y ? floor_y : r[HUD_RADAR].y)
                    - cy - HUD_TAPE_PAD - HUD_STACK_GAP;
        float half = h / 6.0f;
        float bh;
        if (half > lim) half = lim;
        if (half < 24.0f) half = 24.0f;
        bh = 2.0f * half + 12.0f + 2.0f * HUD_PAD;
        r[HUD_TAPE_L] = (fly_rectf){ m + 34.0f - 3.0f - HUD_PAD,
                                     cy - half - 6.0f - HUD_PAD,
                                     43.0f + 2.0f * HUD_PAD, bh };
        r[HUD_TAPE_R] = (fly_rectf){ HUD_RIGHT(w) - 14.0f - 40.0f - HUD_PAD,
                                     cy - half - 6.0f - HUD_PAD,
                                     43.0f + 2.0f * HUD_PAD, bh };
    }

    /* Heading strip: as wide as the gap between the top corners allows, up to
     * 280. Sizing it off w/3 is what put the scale's own rule through the task
     * card at 640x360 — a third of a narrow frame is wider than the space
     * between two clusters that are themselves sized in pixels. */
    {
        float cx = w * 0.5f, hw = w / 3.0f;
        float lgap = cx - (r[HUD_ESTATE].x + r[HUD_ESTATE].w) - HUD_STACK_GAP - HUD_PAD;
        float rgap = r[HUD_TASK].x - cx - HUD_STACK_GAP - HUD_PAD;
        float lim = 2.0f * (lgap < rgap ? lgap : rgap);
        if (hw > 280.0f) hw = 280.0f;
        if (hw > lim) hw = lim;
        if (hw < 90.0f) hw = 90.0f;
        r[HUD_HEAD] = hud_box(cx - hw * 0.5f - HUD_PAD, 21.0f - HUD_PAD, hw, 35.0f);
    }
    return HUD_BOXES;
}

/* where the tape's own line sits inside its column, and how far it runs */
static float hud_tape_x(fly_rectf b, int side) {
    return side > 0 ? b.x + b.w - HUD_PAD - 3.0f : b.x + HUD_PAD + 3.0f;
}
/* The scale's half-height. A tape is a label row and then the scale, both
 * inside the one pad — the label used to be placed at a fixed offset above the
 * scale, which put SPD and ALT a pixel off the top edge of their own glass
 * while every other group had eight. */
static float hud_tape_half(fly_rectf b) {
    return (b.h - 12.0f - HUD_ROW_H - 2.0f * HUD_PAD) * 0.5f;
}
static float hud_tape_cy(fly_rectf b) {
    return b.y + HUD_PAD + HUD_ROW_H + hud_tape_half(b) + 6.0f;
}

/* One property row: key on the left, value hard right, both sitting on the
 * row's centre line. Every key/value pair on the HUD goes through here, which
 * is what makes "the rows are evenly spaced and the labels are one size" a
 * property of the code rather than of four call sites agreeing. */

/* the same row, with a gauge bar in place of the value */
#define HUD_BAR_H 5.0f
/* A gauge is a sprite and a bar. The keys used to be four letters — FUEL POWR
 * HULL WEAR — which is four words the eye has to read before it can look at
 * the thing it came for, and four blocks of type competing with the bars they
 * label. A sprite is read at a glance and takes a third of the width.
 *
 * The sprite is in full colour and the bar carries the state, which is a
 * division of labour rather than a contradiction: the can says *what* is being
 * measured and the bar says how much is left and whether that is a problem.
 * Four grey glyphs at the head of four bars said only "here are four gauges". */
static void gauge_row(fly_img *im, float x, float bx, float y, float bw,
                      fly_icon ic, float frac, uint32_t color) {
    fly_ui_sprite(im, ic, x + HUD_ICON_W * 0.5f, FLY_ROW_MID(y), HUD_ICON_W);
    fly_ui_bar(im, bx, FLY_ROW_BOX(y, HUD_BAR_H), bw, frac, color);
}

/* and the same for a value row: sprite, then the number hard right */
static void icon_row(fly_img *im, float x, float rx, float y, fly_icon ic,
                     const char *val, uint32_t vcol) {
    fly_ui_sprite(im, ic, x + HUD_ICON_W * 0.5f, FLY_ROW_MID(y), HUD_ICON_W);
    fly_text_mid(im, FLY_FONT_MONO, rx, FLY_ROW_MID(y), FLY_FS_VAL,
                 vcol, FLY_ALIGN_RIGHT, val);
}

/* ---------------- attitude: plane ---------------- */

/* --- the three instruments the new flight model made necessary -----------
 *
 * None of these is decoration. Adverse yaw, propeller torque and a side force
 * on the fuselage mean a turn is now something you coordinate, and there is no
 * way to coordinate one without a ball. A wing that lets go at an angle rather
 * than at a speed means the airspeed tape is no longer the stall warning, and
 * an aeroplane you have to hold in pitch for the whole flight needs somewhere
 * to read the trim you set instead.
 *
 * All three live inside the reticle's own radius, on the glass that is already
 * there, so nothing about the layout changes and `hud.layout` still measures
 * the same boxes. They are also the three things a pilot looks at in the same
 * glance as the horizon, which is the honest reason to put them there.
 */

/* Angle of attack as a fraction of the stall, which is the only form of it
 * worth showing: 0 is the hangar and 1 is the break, whatever the aeroplane
 * weighs and however fast it is going. The band above four fifths is where
 * the buffet lives and is drawn in the warning colour; over the top it is red
 * and the caret sits on the stop. */
static void draw_alpha_band(fly_img *im, const fly_game *g, float x, float cy, float half) {
    float astall = g->player.airframe.alpha_stall > 0.01f ? g->player.airframe.alpha_stall : 0.26f;
    float frac = fly_clampf(g->player.craft.alpha / astall, -0.35f, 1.15f);
    float y0 = cy + half, y1 = cy - half;   /* 0 at the bottom, the break at the top */
    float warn_y = fly_lerpf(y0, y1, 0.80f);
    float y = fly_lerpf(y0, y1, fly_clampf(frac, 0.0f, 1.0f));
    uint32_t col = g->player.craft.stalled ? FLY_UI_BAD
                 : g->player.craft.buffet > 0.02f ? FLY_UI_WARN : FLY_UI_FG;
    if (g->player.airframe.kind == FLY_CRAFT_DRONE) return;
    fly_img_aa_line(im, x + 1, y0 + 1, x + 1, y1 + 1, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_line(im, x, y0, x, y1, 1.2f, FLY_UI_FAINT);
    /* the last fifth, where the airframe starts talking */
    fly_img_aa_line(im, x, warn_y, x, y1, 1.6f, FLY_UI_WARN);
    /* the caret, pointing in at the scale */
    fly_img_aa_line(im, x - 5.0f, y + 1.0f, x - 0.5f, y + 1.0f, 1.4f, FLY_UI_SHADOW);
    fly_img_aa_line(im, x - 5.0f, y, x - 0.5f, y, 1.4f, col);
    fly_text_sh(im, FLY_FONT_MONO, x - 6.0f, y1 - 12.0f, FS_S,
                g->player.craft.stalled ? FLY_UI_BAD : FLY_UI_FAINT, FLY_ALIGN_CENTER, "A");
}

/* Where the trim wheel is. A bare index on a bare scale: the number is
 * meaningless and the position is not, because what a pilot needs to know is
 * how far from neutral they have wound it and which way. */
static void draw_trim_index(fly_img *im, const fly_game *g, float x, float cy, float half) {
    float t = fly_clampf(g->player.controls.trim, -1.0f, 1.0f);
    float y = cy - t * half;
    fly_img_aa_line(im, x + 1, cy - half + 1, x + 1, cy + half + 1, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_line(im, x, cy - half, x, cy + half, 1.2f, FLY_UI_FAINT);
    fly_img_aa_line(im, x - 2.5f, cy, x + 2.5f, cy, 1.0f, FLY_UI_FAINT);
    fly_img_aa_line(im, x + 0.5f, y + 1.0f, x + 5.0f, y + 1.0f, 1.4f, FLY_UI_SHADOW);
    fly_img_aa_line(im, x + 0.5f, y, x + 5.0f, y, 1.4f,
                    fabsf(t) > 0.02f ? FLY_UI_ACCENT : FLY_UI_FAINT);
    fly_text_sh(im, FLY_FONT_MONO, x + 6.0f, cy - half - 12.0f, FS_S, FLY_UI_FAINT,
                FLY_ALIGN_CENTER, "T");
}

/* The ball. Ten degrees of sideslip either side of centre, which is about as
 * much as an airframe of this size will hold before the fin wins, and two
 * cage lines so that centred reads as centred rather than as nearly. */
static void draw_slip_ball(fly_img *im, const fly_game *g, float cx, float cy, float half) {
    float slip = fly_clampf(g->player.craft.slip * FLY_RAD2DEG / 10.0f, -1.0f, 1.0f);
    float bx = cx + slip * half;
    uint32_t col = fabsf(slip) > 0.45f ? FLY_UI_WARN : FLY_UI_FG;
    if (g->player.airframe.kind == FLY_CRAFT_DRONE) return;
    fly_img_aa_line(im, cx - half - 4.0f, cy + 1, cx + half + 4.0f, cy + 1, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_line(im, cx - half - 4.0f, cy, cx + half + 4.0f, cy, 1.2f, FLY_UI_FAINT);
    fly_img_aa_line(im, cx - 3.5f, cy - 3.0f, cx - 3.5f, cy + 3.0f, 1.0f, FLY_UI_FAINT);
    fly_img_aa_line(im, cx + 3.5f, cy - 3.0f, cx + 3.5f, cy + 3.0f, 1.0f, FLY_UI_FAINT);
    fly_img_aa_disc(im, bx + 1, cy + 1, 2.6f, FLY_UI_SHADOW);
    fly_img_aa_disc(im, bx, cy, 2.6f, col);
}

static void draw_horizon(fly_img *im, const fly_game *g, float cx, float cy, float r) {
    float roll, pitch, yaw;
    /* The reticle is a group like any other, so it is made of the same glass:
     * the world behind it stays visible and loses its detail, which is exactly
     * what you want under a ladder you are trying to read a horizon off. */
    /* A band, not a surface. This one sits over the middle of the frame where
     * the aircraft and whatever it is pointed at are, so the instrument is an
     * edge to read against rather than something laid over the view. */
    fly_ui_glass_ring(im, cx, cy, r, 6.0f, FLY_UI_LENS);
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    float ppr = r * 1.9f; /* pixels per radian of pitch */
    float cr = cosf(-roll), sr = sinf(-roll);
    int k;
    for (k = -3; k <= 3; ++k) {
        float ang = (float)k * 10.0f * FLY_DEG2RAD;
        float off = ((-pitch) - ang) * ppr;
        if (fabsf(off) > r * 0.92f) continue;
        float half = k == 0 ? r - 8.0f : r / 3.0f;
        float x0 = cx - half * cr + off * sr, y0 = cy + off * cr + half * sr;
        float x1 = cx + half * cr + off * sr, y1 = cy + off * cr - half * sr;
        /* Every rung is the same white. The horizon used to be an accent
         * stripe running the full width of the reticle, which made the one
         * line the eye tracks continuously the loudest thing on the screen —
         * and colour here says nothing the position does not. */
        uint32_t col = k == 0 ? FLY_UI_FG : FLY_UI_DIM;
        fly_img_aa_line(im, x0 + 1, y0 + 1, x1 + 1, y1 + 1, 1.4f, FLY_UI_SHADOW);
        fly_img_aa_line(im, x0, y0, x1, y1, k == 0 ? 1.6f : 1.4f, col);
        if (k != 0) {
            char buf[8];
            snprintf(buf, sizeof buf, "%d", k * 10); /* rungs above horizon read positive */
            fly_text_sh(im, FLY_FONT_MONO, x1 + 5.0f * cr, y1 - 5.0f, FS_S, FLY_UI_DIM,
                        FLY_ALIGN_LEFT, buf);
        }
    }
    /* fixed reference wings */
    fly_img_aa_line(im, cx - 17, cy, cx - 6, cy, 2.0f, FLY_UI_FG);
    fly_img_aa_line(im, cx + 6, cy, cx + 17, cy, 2.0f, FLY_UI_FG);
    fly_img_aa_disc(im, cx, cy, 1.6f, FLY_UI_FG);
    fly_img_aa_circle(im, cx, cy - 0.0f, r + 3.5f, 1.0f, FLY_UI_GLASS_EDGE);
    draw_alpha_band(im, g, cx - r * 0.86f, cy, r * 0.5f);
    draw_trim_index(im, g, cx + r * 0.86f, cy, r * 0.5f);
    draw_slip_ball(im, g, cx, cy + r * 0.72f, r * 0.34f);
}

/* ---------------- attitude: drone ---------------- */

static void draw_tilt_bubble(fly_img *im, const fly_game *g, float cx, float cy, float r) {
    float roll, pitch, yaw;
    fly_ui_glass_ring(im, cx, cy, r, 6.0f, FLY_UI_LENS);
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    fly_img_aa_circle(im, cx, cy, r + 3.5f, 1.0f, FLY_UI_GLASS_EDGE);
    fly_img_aa_circle(im, cx, cy, r * 0.5f, 1.0f, FLY_UI_FAINT);
    int k;
    for (k = 0; k < 4; ++k) {
        float a = (float)k * FLY_PI * 0.5f;
        fly_img_aa_line(im, cx + cosf(a) * (r - 6), cy + sinf(a) * (r - 6),
                        cx + cosf(a) * r, cy + sinf(a) * r, 1.2f, FLY_UI_DIM);
    }
    float max_tilt = 35.0f * FLY_DEG2RAD;
    float bx = cx + fly_clampf(roll / max_tilt, -1, 1) * (r - 8);
    float by = cy - fly_clampf(pitch / max_tilt, -1, 1) * (r - 8);
    fly_img_aa_disc(im, bx + 1, by + 1, 4.5f, FLY_UI_SHADOW);
    fly_img_aa_disc(im, bx, by, 4.5f, FLY_UI_ACCENT);
    fly_img_aa_disc(im, cx, cy, 1.4f, FLY_UI_FG);
    float tilt = sqrtf(roll * roll + pitch * pitch) * FLY_RAD2DEG;
    char buf[16];
    snprintf(buf, sizeof buf, "%.0f`", tilt);
    fly_text_sh(im, FLY_FONT_MONO, cx, cy + r + 6, FS_S,
                tilt > 30.0f ? FLY_UI_WARN : FLY_UI_DIM, FLY_ALIGN_CENTER, buf);
}

/* ---------------- tapes ---------------- */

/* vertical tape: thin AA line, ticks toward the center of the screen,
 * current value in mono with a soft pill marker. side: -1 left, +1 right */
static void draw_tape(fly_img *im, fly_rectf b, int side,
                      float value, float per_px, const char *label, uint32_t vcol) {
    /* the wash is the column the layout reserved, so what is drawn and what is
     * measured for overlap are the same rectangle */
    float x = hud_tape_x(b, side), half = hud_tape_half(b);
    float cy = hud_tape_cy(b);
    /* No pane. A tape is a line with ticks on it — the tallest thing on the
     * frame, and putting a slab behind it was most of what made the HUD read
     * as a row of boxes. The scale carries its own shadows and the value sits
     * on a pill, which is the only part that needs a surface. */
    fly_img_aa_line(im, x + 1, cy - half + 1, x + 1, cy + half + 1, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_line(im, x, cy - half, x, cy + half, 1.2f, FLY_UI_DIM);
    float step = 10.0f;
    while (step / per_px < 16.0f) step *= 5.0f;
    int k;
    /* ticks and labels point inward so the outer edges stay clear (touch
     * slider, screen margin); the value pill covers the scale like a real
     * tape window */
    for (k = -5; k <= 5; ++k) {
        float v = (floorf(value / step) + (float)k) * step;
        float py = cy - (v - value) / per_px;
        if (v < 0 || py < cy - half + 4 || py > cy + half - 4) continue;
        fly_img_aa_line(im, x, py, x - 5 * side, py, 1.1f, FLY_UI_DIM);
        char buf[16];
        snprintf(buf, sizeof buf, "%.0f", v);
        fly_text_sh(im, FLY_FONT_MONO, x - 8.0f * side, py - 5.5f, FS_S, FLY_UI_FG,
                    side > 0 ? FLY_ALIGN_RIGHT : FLY_ALIGN_LEFT, buf);
    }
    /* current value pill */
    char buf[16];
    snprintf(buf, sizeof buf, "%.0f", value);
    float tw = fly_text_width(FLY_FONT_MONO, FS_M, buf) + 12.0f;
    float px = side > 0 ? x - tw - 6 : x + 6;
    fly_ui_card(im, px, cy - 11, tw, 22, FLY_UI_CARD);
    fly_text_sh(im, FLY_FONT_MONO, px + tw * 0.5f, cy - 8.5f, FS_M, vcol, FLY_ALIGN_CENTER, buf);
    fly_text_mid(im, FLY_FONT_UI, x + (side > 0 ? 2.0f : -2.0f),
                 FLY_ROW_MID(b.y + HUD_PAD), FS_S,
                 FLY_UI_DIM, side > 0 ? FLY_ALIGN_RIGHT : FLY_ALIGN_LEFT, label);
}

static void draw_vsi(fly_img *im, const fly_game *g, float x, float cy, float half) {
    float vz = g->player.craft.vel.z;
    fly_img_aa_line(im, x, cy - half, x, cy + half, 1.1f, FLY_UI_FAINT);
    fly_img_aa_line(im, x - 3, cy, x + 3, cy, 1.1f, FLY_UI_DIM);
    float len = fly_clampf(vz / 8.0f, -1.0f, 1.0f) * half;
    uint32_t col = vz < -4.0f ? FLY_UI_WARN : FLY_UI_ACCENT;
    if (fabsf(len) > 1.0f) fly_img_aa_line(im, x, cy, x, cy - len, 3.0f, col);
    char buf[16];
    snprintf(buf, sizeof buf, "%+.0f", vz);
    fly_text_sh(im, FLY_FONT_MONO, x, cy + half + 5, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER, buf);
    fly_text_sh(im, FLY_FONT_UI, x, cy - half - 16, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER, "VSI");
}

/* ---------------- heading strip ---------------- */

static void draw_heading(fly_img *im, const fly_game *g, fly_rectf b) {
    float cx = b.x + b.w * 0.5f, y = b.y + HUD_PAD + 5.0f, w = b.w - 2.0f * HUD_PAD;
    float roll, pitch, yaw;
    /* the compass is a rule, not a panel */
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    float hdg = fmodf(90.0f - yaw * FLY_RAD2DEG + 720.0f, 360.0f);
    fly_img_aa_line(im, cx - w / 2 + 1, y + 13, cx + w / 2 + 1, y + 13, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_line(im, cx - w / 2, y + 12, cx + w / 2, y + 12, 1.2f, FLY_UI_FAINT);
    int k;
    for (k = -6; k <= 6; ++k) {
        float m = floorf(hdg / 15.0f) * 15.0f + (float)k * 15.0f;
        float px = cx + (m - hdg) * w / 180.0f;
        if (px < cx - w / 2 + 6 || px > cx + w / 2 - 6) continue;
        float mm = fmodf(m + 720.0f, 360.0f);
        int major = fmodf(mm, 45.0f) < 0.1f;
        fly_img_aa_line(im, px, y + (major ? 8.0f : 10.0f), px, y + 12, 1.1f, FLY_UI_FAINT);
        if (major) {
            char buf[8];
            const char *card = NULL;
            if (mm < 0.1f) card = "N";
            else if (fabsf(mm - 90) < 0.1f) card = "E";
            else if (fabsf(mm - 180) < 0.1f) card = "S";
            else if (fabsf(mm - 270) < 0.1f) card = "W";
            else { snprintf(buf, sizeof buf, "%.0f", mm); card = buf; }
            fly_text_sh(im, FLY_FONT_MONO, px, y - 5, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER, card);
        }
    }
    char buf[8];
    snprintf(buf, sizeof buf, "%03.0f", hdg);
    fly_img_aa_line(im, cx, y + 12, cx, y + 16, 1.4f, FLY_UI_ACCENT);
    fly_text_sh(im, FLY_FONT_MONO, cx, y + 17, FS_B, FLY_UI_FG, FLY_ALIGN_CENTER, buf);
}

/* ---------------- factions on the glass ----------------
 *
 * Two colours, and they are answers to two different questions. A power's own
 * mark says *whose* — the same colour that flies over its fields, so a chart
 * and the world agree. The relation colour says *what it is to you*, on the
 * three-signal scale the rest of the HUD already speaks: bad, warn, good.
 *
 * Nothing here invents a palette. `fly_faction_mark` is the one source, and
 * this only converts it into the packed form the image code wants. */
uint32_t fly_hud_faction_rgba(int faction, int alpha) {
    fly_v3 c = fly_faction_mark(faction);
    return FLY_RGBA((int)(fly_clampf(c.x, 0, 1) * 255.0f),
                    (int)(fly_clampf(c.y, 0, 1) * 255.0f),
                    (int)(fly_clampf(c.z, 0, 1) * 255.0f),
                    alpha < 0 ? 0 : alpha > 255 ? 255 : alpha);
}

static uint32_t hud_contact_colour(const fly_game *g, const fly_pilot *p) {
    int pf = g->player.faction;
    if (p->kind == FLY_PILOT_PIRATE) return FLY_UI_BAD;
    if (pf == FLY_FACTION_FREE) return FLY_UI_DIM;   /* nobody's quarrel is yours */
    if (p->faction == pf) return FLY_UI_GOOD;
    switch (fly_diplomacy_state(&g->world.dip, pf, p->faction)) {
    case FLY_REL_WAR:  return FLY_UI_BAD;
    case FLY_REL_COLD: return FLY_UI_WARN;
    case FLY_REL_PACT: return FLY_UI_GOOD;
    default:           return FLY_UI_DIM;
    }
}

/* The same question about a column: whose freight is it, and does that make it
 * a target, a neighbour or nothing to do with you. One rule with the air
 * contacts, except that a column under no flag is nobody's enemy — and that an
 * operator who has already shot at one has made it their problem. */
static uint32_t hud_convoy_colour(const fly_game *g, const fly_convoy *c) {
    int pf = g->player.faction;
    if (c->provoked) return FLY_UI_BAD;
    if (pf == FLY_FACTION_FREE || c->faction == FLY_FACTION_FREE) return FLY_UI_DIM;
    if (c->faction == pf) return FLY_UI_GOOD;
    switch (fly_diplomacy_state(&g->world.dip, pf, c->faction)) {
    case FLY_REL_WAR:  return FLY_UI_BAD;
    case FLY_REL_COLD: return FLY_UI_WARN;
    case FLY_REL_PACT: return FLY_UI_GOOD;
    default:           return FLY_UI_DIM;
    }
}

/* ---------------- radar ---------------- */

static void draw_radar(fly_img *im, const fly_game *g, float cx, float cy, float r) {
    /* The blur is a falloff *outside* the footprint, so it is part of how wide
       this widget is: at 11 it reached two pixels into the throttle column the
       layout had reserved. A shadow has to fit in the box its group claims. */
    fly_img_soft_shadow(im, cx - r, cy - r, 2 * r, 2 * r, r, 9.0f, FLY_RGBA(0, 0, 0, 100));
    fly_ui_glass_disc(im, cx, cy, r, FLY_RGBA(255, 255, 255, 52));
    fly_img_aa_circle(im, cx, cy, r, 1.0f, FLY_UI_GLASS_EDGE);
    fly_img_aa_circle(im, cx, cy, r * 0.5f, 1.0f, FLY_RGBA(255, 255, 255, 70));
    fly_img_aa_line(im, cx, cy - r + 3, cx, cy - r + 8, 1.1f, FLY_UI_DIM); /* heading-up mark */
    float range = fly_game_radar_range(g);
    float roll, pitch, yaw;
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    int i;
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_location *L = &g->world.loc[i];
        if (!L->discovered) continue;
        fly_v2 q = fly_world_delta(L->pos, fly_wpos_of(g->player.craft.pos));
        float d = sqrtf(q.x * q.x + q.y * q.y);
        if (d > range * 2.0f) continue;
        float a = atan2f(q.y, q.x) - yaw;
        float rr = fly_clampf(d / (range * 2.0f), 0, 1) * (r - 5);
        fly_img_aa_disc(im, cx + sinf(a) * rr, cy - cosf(a) * rr, 2.0f,
                        fly_hud_faction_rgba(fly_game_site_owner(g, i), 255));
        /* A ring round a site somebody is prising loose. It is the only thing
         * on this instrument that is about the war rather than about the next
         * ten minutes of flying, and it earns the pixels: a contested field is
         * where the fighting is, which is both the danger and the work. */
        if (fly_faction_contested(&g->world, i))
            fly_img_aa_circle(im, cx + sinf(a) * rr, cy - cosf(a) * rr, 4.2f, 1.0f,
                              FLY_UI_WARN);
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        if (!g->pilots[i].active) continue;
        float dx = g->pilots[i].craft.pos.x - g->player.craft.pos.x;
        float dy = g->pilots[i].craft.pos.y - g->player.craft.pos.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > range * 2.0f) continue;
        float a = atan2f(dy, dx) - yaw;
        float rr = fly_clampf(d / (range * 2.0f), 0, 1) * (r - 5);
        /* Contacts by what they are to *you*, not by what they are. Every blip
         * on this scope used to be the same alarm red, which is the least
         * useful thing a radar can say once there are seven powers in the air:
         * the question is never "is that an aircraft", it is "is that one a
         * problem". */
        fly_img_aa_disc(im, cx + sinf(a) * rr, cy - cosf(a) * rr, 2.4f,
                        hud_contact_colour(g, &g->pilots[i]));
    }
    /* Surface traffic, as a square rather than a disc: a column is a contact
     * you can do something about at your leisure — it is on a road or on a
     * lane, it is going where that goes, and it will still be there next pass.
     * Coloured by whose freight it is, on the same rule the air contacts use,
     * because the question is the same one: is that one a problem. */
    for (i = 0; i < FLY_CONVOY_MAX; ++i) {
        const fly_convoy *c = &g->convoys[i];
        float dx, dy, d, a, rr;
        if (!c->active) continue;
        dx = c->pos.x - g->player.craft.pos.x;
        dy = c->pos.y - g->player.craft.pos.y;
        d = sqrtf(dx * dx + dy * dy);
        if (d > range * 2.0f) continue;
        a = atan2f(dy, dx) - yaw;
        rr = fly_clampf(d / (range * 2.0f), 0, 1) * (r - 5);
        fly_img_fill_rect(im, (int)(cx + sinf(a) * rr - 1.6f),
                          (int)(cy - cosf(a) * rr - 1.6f), 3, 3,
                          hud_convoy_colour(g, c));
    }
    /* And whatever is in the air that is not an aeroplane, drawn last so it is
     * on top of everything else on the scope. A round is the only contact here
     * that resolves within seconds, so it is the only one worth drawing over a
     * settlement — and it is drawn at true bearing on a fixed ring rather than
     * at range, because at four hundred metres it would otherwise be a dot
     * indistinguishable from the aeroplane at the middle. Which way it is
     * coming from is the whole of the useful information. */
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        const fly_ord *o = &g->ord[i];
        float dx, dy, d, a, rr;
        if (!o->active || o->damage <= 0.0f) continue;
        dx = o->pos.x - g->player.craft.pos.x;
        dy = o->pos.y - g->player.craft.pos.y;
        d = sqrtf(dx * dx + dy * dy);
        if (d > 3200.0f) continue;
        a = atan2f(dy, dx) - yaw;
        rr = fly_clampf(0.45f + 0.55f * d / 3200.0f, 0.0f, 1.0f) * (r - 5);
        {   /* a chevron pointing the way it is going, so an inbound round and
             * one crossing behind you do not read the same */
            float hx = o->vel.x, hy = o->vel.y;
            float ha = atan2f(hy, hx) - yaw;
            float px = cx + sinf(a) * rr, py = cy - cosf(a) * rr;
            uint32_t col = o->owner.kind == FLY_ORD_ACTOR_PLAYER ? FLY_UI_ACCENT : FLY_UI_BAD;
            fly_img_aa_line(im, px - sinf(ha) * 3.4f, py + cosf(ha) * 3.4f,
                            px + sinf(ha) * 3.4f, py - cosf(ha) * 3.4f, 1.6f, col);
            fly_img_aa_disc(im, px + sinf(ha) * 3.4f, py - cosf(ha) * 3.4f, 1.7f, col);
        }
    }
    fly_img_aa_disc(im, cx, cy, 1.6f, FLY_UI_FG);
    char buf[16];
    snprintf(buf, sizeof buf, "%.0fkm", range * 2.0f / 1000.0f);
    fly_text_sh(im, FLY_FONT_MONO, cx, cy + r - 14, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER, buf);
}

/* ---------------- clusters ---------------- */

/* Money, then the two numbers that say how close the run is to over. A fail
 * state nobody can see coming reads as the game cheating, so reserve — what the
 * whole estate would raise, in units of the cheapest hull — is on screen at all
 * times, and so is the life left in the airframe carrying it.
 *
 * All three read as KEY ... value across the cluster, because a reader scans a
 * column and parses a sentence. "reserve 8.3 hulls" and "airframe life 100%"
 * were sentences: they said the same thing at four times the width, in prose
 * that changed length as the numbers did, so nothing lined up with anything. */
static void draw_estate_cluster(fly_img *im, const fly_game *g, fly_rectf b) {
    fly_rectf c = hud_inner(b);
    char buf[24];
    float hulls = fly_game_reserve_hulls(g);
    const uint32_t *fitted = fly_game_fitted(g);
    float life = fitted ? 1.0f - g->owned[g->active_airframe].fatigue : 1.0f;
    float rx = c.x + c.w;

    fly_ui_scrim(im, b.x, b.y, b.w, b.h);
    snprintf(buf, sizeof buf, "%ld", g->player.tokens);
    icon_row(im, c.x, rx, c.y, FLY_ICON_CASH, buf, FLY_UI_FG);

    snprintf(buf, sizeof buf, "%.1f", hulls);
    icon_row(im, c.x, rx, c.y + HUD_ROW_H, FLY_ICON_CRAFT, buf,
             hulls < 1.0f ? FLY_UI_BAD : hulls < 2.0f ? FLY_UI_WARN : FLY_UI_DIM);
    if (fitted) {
        snprintf(buf, sizeof buf, "%.0f%%", life * 100.0f);
        icon_row(im, c.x, rx, c.y + HUD_ROW_H * 2.0f, FLY_ICON_LIFE, buf,
                 life < 0.1f ? FLY_UI_BAD : life < 0.25f ? FLY_UI_WARN : FLY_UI_DIM);
    }
    /* Whose colours, and what they currently make of you. In the power's own
     * livery rather than in a signal colour: this row is identity, not alarm,
     * and it is the same paint as the fin of the aeroplane it is drawn over. */
    if (g->player.faction != FLY_FACTION_FREE) {
        int st = fly_game_standing(g, g->player.faction);
        snprintf(buf, sizeof buf, "%s %+d", fly_faction_tag(g->player.faction), st);
        icon_row(im, c.x, rx, c.y + HUD_ROW_H * (float)(fitted ? 3 : 2), FLY_ICON_FLAG,
                 buf, fly_hud_faction_rgba(g->player.faction, 255));
    }
}

static void draw_task_cluster(fly_img *im, const fly_game *g, fly_rectf b) {
    fly_rectf c = hud_inner(b);
    float rx = c.x + c.w;
    int active = hud_task(g);
    char buf[64], line[64];

    fly_ui_scrim(im, b.x, b.y, b.w, b.h);
    if (active < 0) {
        fly_ui_sprite(im, FLY_ICON_TASK, rx - HUD_ICON_W * 0.5f, FLY_ROW_MID(c.y),
                      HUD_ICON_W);
        return;
    }
    {
        const fly_contract *ct = &g->contracts[active];
        const fly_location *L = &g->world.loc[ct->to];
        fly_v2 tq = fly_world_delta(L->pos, fly_wpos_of(g->player.craft.pos));
        float dx = tq.x, dy = tq.y;
        double left = ct->deadline - g->time_s;
        float roll, pitch, yaw, rel, sum_w, chx, chy;
        char det[64];
        hud_task_text(g, active, buf, sizeof buf, det, sizeof det);
        /* the chevron wants its gutter at the left of the line, so the line is
         * elided to leave it — the bearing is the part you read at a glance */
        fit_line(line, sizeof line, FLY_FONT_UI, FLY_FS_VAL, c.w - HUD_TASK_GUTTER, buf);
        sum_w = fly_text_mid(im, FLY_FONT_UI, rx, FLY_ROW_MID(c.y), FLY_FS_VAL,
                             FLY_UI_FG, FLY_ALIGN_RIGHT, line);
        fly_text_mid(im, FLY_FONT_MONO, rx, FLY_ROW_MID(c.y + HUD_ROW_H),
                     FLY_FS_KEY, left < 600 ? FLY_UI_WARN : FLY_UI_DIM, FLY_ALIGN_RIGHT, det);
        fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
        rel = fly_wrap_pi(atan2f(dy, dx) - yaw);
        chx = rx - sum_w - 13.0f;
        if (chx < c.x + 9.0f) chx = c.x + 9.0f;
        chy = c.y + HUD_ROW_H * 0.5f;
        fly_img_aa_line(im, chx, chy, chx + sinf(rel) * 8, chy - cosf(rel) * 8, 1.6f,
                        FLY_UI_ACCENT);
    }
}

static void draw_craft_cluster(fly_img *im, const fly_game *g, fly_rectf b) {
    fly_rectf c = hud_inner(b);
    float rx = c.x + c.w, ly, ff, hf, bw = c.w - HUD_GAUGE_X;
    char buf[48], name[32];

    fly_ui_scrim(im, b.x, b.y, b.w, b.h);
    /* title: airframe on the left, condition figures hard right — and the card
     * is sized for both of them, which is hud_craft_w's whole job */
    float glim = fly_airframe_g_limit(&g->player.airframe);
    uint32_t gcol = g->player.craft.gload > glim ? FLY_UI_BAD
                  : g->player.craft.gload > glim * 0.8f ? FLY_UI_WARN : FLY_UI_DIM;
    hud_craft_figures(g, buf, sizeof buf);
    /* The title row carries a planform glyph like every other row carries its
     * own, and not only for the family resemblance: a 12 px sprite puts its
     * topmost lit pixel two pixels into the row, and 13 px type puts its
     * ascender four or five in depending on what the pane behind it happens to
     * be doing. Four sprite rows under one text row measured as a 13 px gap
     * followed by three 16s — a visible hitch at the top of the card, and one
     * that moved when the glass got darker. */
    fly_ui_sprite(im, FLY_ICON_CRAFT, c.x + HUD_ICON_W * 0.5f, FLY_ROW_MID(c.y), HUD_ICON_W);
    fit_line(name, sizeof name, FLY_FONT_UI, FLY_FS_VAL,
             c.w - HUD_GAUGE_X - fly_text_width(FLY_FONT_MONO, FLY_FS_KEY, buf)
                 - HUD_TITLE_GAP,
             g->player.airframe.name);
    fly_text_mid(im, FLY_FONT_UI, c.x + HUD_GAUGE_X, FLY_ROW_MID(c.y),
                 FLY_FS_VAL, FLY_UI_FG, FLY_ALIGN_LEFT, name);
    fly_text_mid(im, FLY_FONT_MONO, rx, FLY_ROW_MID(c.y), FLY_FS_KEY,
                 gcol, FLY_ALIGN_RIGHT, buf);

    ly = c.y + HUD_ROW_H;
    ff = g->player.craft.fuel / g->player.airframe.fuel_cap;
    hf = g->player.craft.hp / g->player.airframe.structure;
    /* Four keys, four letters each, one column, and the bars run to the right
     * edge of the cluster. CHG/THR against FUEL/HULL/WEAR was three lengths in
     * four rows and the bars started wherever the longest happened to end. */
    gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly, bw, FLY_ICON_FUEL,
              ff, ff < 0.2f ? FLY_UI_BAD : FLY_UI_ACCENT);
    gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly + HUD_ROW_H, bw, FLY_ICON_POWER,
              g->player.craft.throttle, FLY_UI_FG);
    gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly + HUD_ROW_H * 2, bw, FLY_ICON_SHIELD,
              hf, hf < 0.35f ? FLY_UI_BAD : FLY_UI_ACCENT);
    gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly + HUD_ROW_H * 3, bw, FLY_ICON_WEAR,
              g->player.craft.wear, g->player.craft.wear > 0.5f ? FLY_UI_WARN : FLY_UI_DIM);
    {
        float row = 4.0f;
        if (g->player.airframe.propellant_cap > 0.0f) {
            float pf = g->player.craft.propellant / g->player.airframe.propellant_cap;
            gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly + HUD_ROW_H * row, bw, FLY_ICON_PROP,
                      pf, pf < 0.15f ? FLY_UI_WARN : FLY_UI_ACCENT);
            row += 1.0f;
        }
        /* Heat is on the card whenever something aboard can make it. It used
         * to belong to the rocket stack alone because re-entry was the only
         * source; a beam emitter is a second one, and a weapon that locks out
         * at a number the pilot cannot see is a weapon that fails without
         * saying why. */
        if (g->player.airframe.propellant_cap > 0.0f || g->player.airframe.beam_heat > 0.0f) {
            gauge_row(im, c.x, c.x + HUD_GAUGE_X, ly + HUD_ROW_H * row, bw, FLY_ICON_HEAT,
                      g->player.craft.heat,
                      g->player.craft.beam_locked || g->player.craft.heat > 0.6f ? FLY_UI_BAD
                      : g->player.craft.heat > 0.2f ? FLY_UI_WARN : FLY_UI_DIM);
            row += 1.0f;
        }
        /* And the magazines, as counts rather than as bars.
         *
         * Four rounds is not 40% of anything — it is four, and the decision
         * "do I take this shot or keep it" is made on the integer. A bar would
         * be the same information rendered so that you cannot read it, which
         * is the one thing a weapon readout must not be. */
        if (g->player.airframe.ord_kind || g->player.airframe.cm_kind) {
            const fly_ord_class *ok = fly_ord_class_get(g->player.airframe.ord_kind);
            const fly_ord_class *ck = fly_ord_class_get(g->player.airframe.cm_kind);
            float ay = ly + HUD_ROW_H * row;
            /* Two halves of one row when both are aboard, so each count sits
             * under its own glyph. An aeroplane carrying only one of them gets
             * the whole width, which is also the only case where the number can
             * run to three digits. */
            float split = (ok && ck) ? c.x + c.w * 0.5f : rx;
            char buf[16];
            if (ok) {
                snprintf(buf, sizeof buf, "%s %d", ok->tag, g->player.craft.ammo);
                icon_row(im, c.x, split, ay, FLY_ICON_ORD, buf,
                         g->player.craft.ammo ? FLY_UI_FG : FLY_UI_BAD);
            }
            if (ck) {
                snprintf(buf, sizeof buf, "%s %d", ck->tag, g->player.craft.cm);
                icon_row(im, ok ? split : c.x, rx, ay, FLY_ICON_DECOY, buf,
                         g->player.craft.cm ? FLY_UI_FG : FLY_UI_BAD);
            }
        }
    }
}

static void draw_weather_cluster(fly_img *im, const fly_game *g, fly_rectf b) {
    fly_rectf c;
    if (b.w <= 0.0f) return;
    c = hud_inner(b);
    float ws = fly_v3len(g->weather.wind);
    float roll, pitch, yaw, a;
    /* The rose sits at the outboard end so the two boxes in the bottom-left
     * stack share a key column and a bar column. With it on the left, WIND and
     * GUST were indented 28 px from FUEL and POWR directly below them, which
     * is the kind of near-miss alignment that reads as sloppiness. */
    float cy = c.y + c.h * 0.5f, rx = c.x + c.w;
    float rose = rx - 11.0f, kx = c.x, vx = rx - 28.0f;
    char buf[24];

    fly_ui_scrim(im, b.x, b.y, b.w, b.h);
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    a = atan2f(g->weather.wind.y, g->weather.wind.x) - yaw;
    fly_img_aa_circle(im, rose + 1, cy + 1, 11, 1.2f, FLY_UI_SHADOW);
    fly_img_aa_circle(im, rose, cy, 11, 1.2f, FLY_UI_DIM);
    fly_img_aa_line(im, rose - sinf(a) * 8, cy + cosf(a) * 8,
                    rose + sinf(a) * 8, cy - cosf(a) * 8, 1.6f, FLY_UI_FG);
    fly_img_aa_disc(im, rose + sinf(a) * 8, cy - cosf(a) * 8, 2.0f, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f", ws);
    icon_row(im, kx, vx, c.y, FLY_ICON_WIND, buf, FLY_UI_DIM);
    gauge_row(im, kx, kx + HUD_GAUGE_X, c.y + HUD_ROW_H, vx - kx - HUD_GAUGE_X,
              FLY_ICON_GUST, g->weather.turbulence,
              g->weather.turbulence > 0.5f ? FLY_UI_WARN : FLY_UI_ACCENT);
}

/* ---------------- main ---------------- */

/* The end of a run. Deliberately says what it cost and what carries over: the
 * world is still there, and so is everything this operator dropped in it. */
static void draw_ruin(fly_img *im, const fly_game *g, float cx, float cy) {
    char buf[80];
    /* The same material as everywhere else, at page strength: the world you
     * just lost the airframe in stays on screen behind the glass, out of focus.
     * A flat 84%-opaque slab of near-black painted it out entirely, which is
     * the one moment in the game where seeing where you ended up is the point. */
    fly_ui_page(im);
    fly_ui_card(im, cx - 190, cy - 62, 380, 124, FLY_UI_CARD);
    fly_text_sh(im, FLY_FONT_UI, cx, cy - 48, FS_L, FLY_UI_BAD, FLY_ALIGN_CENTER, "GROUNDED");
    snprintf(buf, sizeof buf, "%d airframe%s lost. Nothing left to raise another.",
             g->hulls_lost, g->hulls_lost == 1 ? "" : "s");
    fly_text_sh(im, FLY_FONT_UI, cx, cy - 8, FS_B, FLY_UI_FG, FLY_ALIGN_CENTER, buf);
    fly_text_sh(im, FLY_FONT_UI, cx, cy + 12, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER,
                "The world keeps flying. Sign on again and it will still be there,");
    fly_text_sh(im, FLY_FONT_UI, cx, cy + 26, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER,
                "your wrecks included.");
    fly_text_sh(im, FLY_FONT_UI, cx, cy + 46, FS_B, FLY_UI_ACCENT, FLY_ALIGN_CENTER,
                "TAB  >  sign on as a new operator");
}

/* The single source of the corner geometry. fly_hud_draw lays its clusters out
 * from exactly these boxes, so the test that checks them for overlap is
 * checking the thing the frame is built from and not a copy of it. */
int fly_hud_layout(int w_, int h_, const fly_game *g, fly_rectf *out, int max) {
    fly_rectf r[HUD_BOXES];
    int n = hud_boxes((float)w_, (float)h_, g, r), i;
    for (i = 0; i < n && i < max; ++i) out[i] = r[i];
    return n < max ? n : max;
}

void fly_hud_draw(fly_img *im, const fly_game *g) {
    float w = (float)im->w, h = (float)im->h;
    float cx = w * 0.5f, cy = h * 0.5f;
    float m = HUD_MARGIN;

    if (g->ruin) { draw_ruin(im, g, cx, cy); return; }

    if (g->mode == FLY_MODE_WALK) {
        char sp[24], hold[24];
        float speed = fly_v3len(fly_v3mk(g->walker.vel.x, g->walker.vel.y, 0));
        float wa, wb, bw, x0, my = h - 31.0f;
        /* Two sprites and two numbers: what you are doing, and what you are
         * carrying. The line used to spell out "ON FOOT" and "hold", which is
         * eleven characters of label for eleven characters of reading — and it
         * was centred as one string, so the numbers moved as the digits
         * changed. Each pair is laid out from its own glyph instead. */
        snprintf(sp, sizeof sp, "%.1f m/s", speed);
        snprintf(hold, sizeof hold, "%d/%d", g->item_count, FLY_ITEM_MAX);
        wa = fly_text_width(FLY_FONT_MONO, FS_S, sp);
        wb = fly_text_width(FLY_FONT_MONO, FS_S, hold);
        bw = 2.0f * FLY_UI_PAD + 13.0f + 6.0f + wa + 20.0f + 13.0f + 6.0f + wb;
        x0 = cx - bw * 0.5f + FLY_UI_PAD;
        fly_ui_card(im, cx - bw * 0.5f, h - 44, bw, 26, FLY_UI_CARD);
        fly_ui_sprite(im, FLY_ICON_ROUTE, x0 + 6.5f, my, 14.0f);
        fly_text_mid(im, FLY_FONT_MONO, x0 + 19.0f, my, FS_S, FLY_UI_ACCENT,
                     FLY_ALIGN_LEFT, sp);
        fly_ui_sprite(im, FLY_ICON_PART, x0 + 19.0f + wa + 26.5f, my, 14.0f);
        fly_text_mid(im, FLY_FONT_MONO, x0 + 19.0f + wa + 39.0f, my, FS_S,
                     FLY_UI_ACCENT, FLY_ALIGN_LEFT, hold);
        fly_text_sh(im, FLY_FONT_UI, cx, cy + 12, FS_S, FLY_UI_DIM, FLY_ALIGN_CENTER,
                    "G interact   SPACE jump   drag to look");
        return;
    }
    if (g->mode == FLY_MODE_RAIL) {
        char buf[120];
        float progress = g->rail_route.length > 0 ? g->rail.distance / g->rail_route.length : 0;
        float bw;
        snprintf(buf, sizeof buf, "%s   %.0f%%   %.0f m/s%s",
                 g->world.loc[g->rail_route.to_location].name, progress * 100.0f,
                 g->rail.speed, g->rail.can_exit ? "   G exit" : "");
        bw = fly_text_width(FLY_FONT_MONO, FS_S, buf) + 2.0f * FLY_UI_PAD + 24.0f;
        fly_ui_card(im, cx - bw * 0.5f, 21, bw, 26, FLY_UI_CARD);
        /* The chevron replaces "RAIL >": the banner only appears while you are
         * on a rail, so the word was telling you a thing the screen already
         * was. The arrow keeps the "to somewhere" half, which is the part the
         * destination name needs. */
        fly_ui_sprite(im, FLY_ICON_ROUTE, cx - bw * 0.5f + FLY_UI_PAD + 6.5f, 34.0f, 14.0f);
        fly_text_mid(im, FLY_FONT_MONO, cx - bw * 0.5f + FLY_UI_PAD + 19.0f, 34.0f, FS_S,
                     FLY_UI_ACCENT, FLY_ALIGN_LEFT, buf);
        fly_ui_bar(im, cx - 120, 52, 240, progress, FLY_UI_ACCENT);
        return;
    }

    if (g->hit_flash > 0.0f)
        fly_img_fill_rect(im, 0, 0, im->w, im->h,
                          FLY_RGBA(255, 40, 30, (int)(70.0f * g->hit_flash / 0.6f)));

    fly_rectf b[HUD_BOXES];
    hud_boxes(w, h, g, b);

    if (g->docked < 0) {
        /* The tapes are sized against the bottom stack, not against a fraction
         * of the frame: h/6 fits at 720 and drives the left tape straight into
         * the craft gauges at 360. */
        float half = hud_tape_half(b[HUD_TAPE_L]);
        float tape_r = hud_tape_x(b[HUD_TAPE_R], 1);
        draw_heading(im, g, b[HUD_HEAD]);
        if (g->player.airframe.kind == FLY_CRAFT_PLANE) {
            draw_horizon(im, g, cx, cy, h / 5.0f);
            /* The speed reads warning in the buffet and red at the break.
             * The stall is an angle rather than a speed now, so the tape
             * cannot show you where it is — but it is still the instrument a
             * pilot's eye is on, so it is where the warning has to arrive. */
            draw_tape(im, b[HUD_TAPE_L], -1, fly_craft_airspeed(&g->player.craft, &g->weather),
                      0.6f, "SPD", g->player.craft.stalled ? FLY_UI_BAD
                                 : g->player.craft.buffet > 0.02f ? FLY_UI_WARN : FLY_UI_FG);
            draw_tape(im, b[HUD_TAPE_R], 1, g->player.craft.pos.z, 3.0f, "ALT", FLY_UI_FG);
            /* SPIN rather than STALL once the wing is autorotating, because
             * they are different emergencies with different recoveries and a
             * banner that cannot tell them apart is telling you to do the
             * wrong thing half the time. */
            if (g->player.craft.spin > 0.15f)
                fly_text_sh(im, FLY_FONT_UI, cx, cy - h / 5.0f - 22, FS_M, FLY_UI_BAD,
                            FLY_ALIGN_CENTER, "SPIN");
            else if (g->player.craft.stalled)
                fly_text_sh(im, FLY_FONT_UI, cx, cy - h / 5.0f - 22, FS_M, FLY_UI_BAD,
                            FLY_ALIGN_CENTER, "STALL");
        } else {
            draw_tilt_bubble(im, g, cx, cy, h / 7.0f);
            float gs = fly_v3len(fly_v3mk(g->player.craft.vel.x, g->player.craft.vel.y, 0));
            draw_tape(im, b[HUD_TAPE_L], -1, gs, 0.4f, "GS", FLY_UI_FG);
            draw_tape(im, b[HUD_TAPE_R], 1, g->player.craft.pos.z, 3.0f, "ALT", FLY_UI_FG);
            draw_vsi(im, g, tape_r - 51.0f, cy, h / 7.0f);
        }
        {
            char buf[24];
            float gz = fly_world_ground(&g->world, g->player.craft.pos.x, g->player.craft.pos.y);
            snprintf(buf, sizeof buf, "AGL %.0f", g->player.craft.pos.z - gz);
            fly_text_sh(im, FLY_FONT_MONO, tape_r + 2.0f, cy + half + 8, FS_S, FLY_UI_FG,
                        FLY_ALIGN_RIGHT, buf);
        }
        if (g->autopilot) {
            float tw = fly_text_width(FLY_FONT_UI, FS_S, "AUTO");
            float pw = tw + 13.0f + 6.0f + 2.0f * FLY_UI_PAD;
            float px = cx - pw * 0.5f, py = cy + h / 5.0f + 10.0f;
            fly_ui_card(im, px, py, pw, 22, FLY_UI_CARD);
            fly_ui_sprite(im, FLY_ICON_ROUTE, px + FLY_UI_PAD + 6.5f, py + 11.0f, 14.0f);
            fly_text_mid(im, FLY_FONT_UI, px + FLY_UI_PAD + 19.0f, py + 11.0f, FS_S,
                         FLY_UI_ACCENT, FLY_ALIGN_LEFT, "AUTO");
        }
        if (g->player.airframe.gun_dps > 0.0f) {
            /* A beam locked out is a weapon that will not answer, and the
               reticle has to say so where the pilot is already looking rather
               than only on the heat gauge in the corner. */
            uint32_t gc = g->player.craft.beam_locked ? FLY_UI_BAD
                        : g->gun_flash > 0 ? FLY_UI_WARN : FLY_UI_DIM;
            fly_img_aa_circle(im, cx, cy, 5.0f, 1.2f, gc);
            fly_img_aa_line(im, cx, cy - 10, cx, cy - 6, 1.2f, gc);
            fly_img_aa_line(im, cx, cy + 6, cx, cy + 10, 1.2f, gc);
            fly_img_aa_line(im, cx - 10, cy, cx - 6, cy, 1.2f, gc);
            fly_img_aa_line(im, cx + 6, cy, cx + 10, cy, 1.2f, gc);
        }
        /* The launcher's own cue: a diamond round the reticle when the round on
           the rail has somewhere to go. It is drawn from the same predicate the
           trigger applies, so it cannot promise a shot that will be refused —
           and without it a guided launcher is a key that silently does nothing
           most of the time, which reads as a broken weapon rather than as an
           envelope. */
        if (g->player.airframe.ord_kind && g->player.craft.ammo > 0) {
            fly_v3 mark;
            int have = fly_game_ord_solution(g, &mark);
            uint32_t lc = !have ? FLY_UI_DIM
                        : g->player.craft.ord_cooldown > 0.0f ? FLY_UI_WARN : FLY_UI_ACCENT;
            float d = have ? 15.0f : 19.0f;   /* it closes onto the sight when it locks */
            fly_img_aa_line(im, cx, cy - d, cx + d, cy, 1.2f, lc);
            fly_img_aa_line(im, cx + d, cy, cx, cy + d, 1.2f, lc);
            fly_img_aa_line(im, cx, cy + d, cx - d, cy, 1.2f, lc);
            fly_img_aa_line(im, cx - d, cy, cx, cy - d, 1.2f, lc);
        }
        /* And the warning, which is unconditional and does not depend on
           carrying a receiver. A missile you were told about is a mechanic and
           one you were not is an ambush; the bearing is half the message,
           because knowing something is coming and not which way makes you turn
           at random. */
        {
            fly_v3 from;
            int okind = 0;
            float tti = fly_game_ord_inbound(g, &from, &okind);
            if (tti >= 0.0f) {
                float roll, pitch, yaw, a, rr = h * 0.22f;
                char buf[24];
                uint32_t wc = tti < 3.0f ? FLY_UI_BAD : FLY_UI_WARN;
                fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
                a = atan2f(from.y - g->player.craft.pos.y, from.x - g->player.craft.pos.x) - yaw;
                {   /* a chevron on a ring at the bearing it is coming from */
                    float px = cx + sinf(a) * rr, py = cy - cosf(a) * rr;
                    fly_img_aa_line(im, px - 9.0f, py + 6.0f, px, py - 4.0f, 2.0f, wc);
                    fly_img_aa_line(im, px + 9.0f, py + 6.0f, px, py - 4.0f, 2.0f, wc);
                }
                snprintf(buf, sizeof buf, "%s  %.0fs",
                         fly_ord_is_radar(okind) ? "DART" : "MISSILE", tti);
                fly_text_sh(im, FLY_FONT_UI, cx, cy - h * 0.30f, FS_B, wc,
                            FLY_ALIGN_CENTER, buf);
            }
        }
    }

    /* Corner stacks, laid out from the one box model rather than from
     * constants that happened to look right at one resolution. The old bottom
     * left put the weather rose at h-m-76 and the craft gauges at h-m-54, which
     * is a 22 px gap for a 54 px cluster: the two drew straight through each
     * other, and the last gauge ran off the bottom of the frame. */
    {
        draw_estate_cluster(im, g, b[HUD_ESTATE]);
        draw_task_cluster(im, g, b[HUD_TASK]);
        draw_craft_cluster(im, g, b[HUD_CRAFT]);
        draw_weather_cluster(im, g, b[HUD_WX]);
        draw_radar(im, g, b[HUD_RADAR].x + b[HUD_RADAR].w * 0.5f,
                   b[HUD_RADAR].y + b[HUD_RADAR].h * 0.5f, b[HUD_RADAR].w * 0.5f);

        /* The event log is centred on the frame, full stop. It used to be
         * centred on the gap between the two bottom corners, which is not the
         * same thing — the left cluster is wider than the right, so the text
         * sat 25 px left of centre and looked like a mistake, because it was
         * one. It is elided to whichever side has less room, so centring it
         * honestly cannot make it collide. */
        {
            float lgap = cx - (b[HUD_CRAFT].x + b[HUD_CRAFT].w + HUD_STACK_GAP);
            float rgap = (b[HUD_RADAR].x - HUD_STACK_GAP) - cx;
            float span = 2.0f * (lgap < rgap ? lgap : rgap) - 2.0f * HUD_PAD;
            int n = g->log_count < 3 ? g->log_count : 3;
            int i;
            char lines[3][96];
            float wmax = 0.0f, top;
            if (span < 120.0f) span = 120.0f;
            for (i = 0; i < n; ++i) {
                float lw;
                fit_line(lines[i], sizeof lines[i], FLY_FONT_UI, FS_S, span,
                         g->log[g->log_count - n + i]);
                lw = fly_text_width(FLY_FONT_UI, FS_S, lines[i]);
                if (lw > wmax) wmax = lw;
            }
            /* one padded box, same as every cluster, and the lines are centred
             * in it because the box is centred on the frame */
            top = h - m - (float)n * HUD_ROW_H - 2.0f * HUD_PAD + 3.0f;
            if (n)
                fly_ui_scrim(im, cx - wmax * 0.5f - HUD_PAD, top,
                             wmax + 2.0f * HUD_PAD, (float)n * HUD_ROW_H - 3.0f + 2.0f * HUD_PAD);
            for (i = 0; i < n; ++i)
                fly_text_sh(im, FLY_FONT_UI, cx, top + HUD_PAD + (float)i * HUD_ROW_H, FS_S,
                            i == n - 1 ? FLY_UI_FG : FLY_UI_DIM, FLY_ALIGN_CENTER, lines[i]);
        }
    }

    if (g->docked >= 0) {
        const fly_location *L = &g->world.loc[g->docked];
        char buf[72];
        /* The pad sprite says "docked"; the word did too, and cost forty
         * pixels of the widest type on the frame to do it. */
        snprintf(buf, sizeof buf, "%s   %s", L->name, fly_loc_kind_name(L->kind));
        float tw = fly_text_width(FLY_FONT_UI, FS_M, buf);
        float pw = tw + 16.0f + 8.0f + 2.0f * FLY_UI_PAD;
        float px = cx - pw * 0.5f, py = h * 0.16f;
        fly_ui_card(im, px, py, pw, 30, FLY_UI_CARD);
        fly_ui_sprite(im, FLY_ICON_SERVICE, px + FLY_UI_PAD + 8.0f, py + 15.0f, 17.0f);
        fly_text_mid(im, FLY_FONT_UI, px + FLY_UI_PAD + 24.0f, py + 15.0f, FS_M,
                     FLY_UI_ACCENT, FLY_ALIGN_LEFT, buf);
    }
}
