/* fly_hud: instrument overlay drawn onto a rendered frame.
 *
 * Style: open, minimal, antialiased. Widgets are frameless — text and
 * gauges sit directly on the scene with soft drop shadows for contrast;
 * translucent rounded cards appear only where a surface is needed
 * (overlays, the radar disc, banners). The layout is margin-based and
 * collision-free from 480x270 up. Instruments adapt to the airframe:
 * planes get ladder/airspeed/stall, drones get tilt bubble/GS/VSI/
 * endurance; module widgets (gun, radar) appear when fitted. */
#ifndef FLY_HUD_H
#define FLY_HUD_H

#include "fly_font.h"
#include "fly_game.h"
#include "fly_img.h"

void fly_hud_draw(fly_img *im, const fly_game *g);

/* A power's livery as packed RGBA, at the alpha the caller wants. Shared with
 * fly_ui so the chart, the legend and the radar cannot drift apart from each
 * other or from the pennant flying over the site they are describing. */
uint32_t fly_hud_faction_rgba(int faction, int alpha);

/* The boxes the HUD reserves for its corner clusters, in the order it draws
 * them. Exposed so "nothing overlaps" can be a measurement rather than a
 * promise: the layout is derived from these, so a test that finds two of them
 * intersecting has found the bug before it reaches a frame. Returns how many
 * were written, up to `max`. */
typedef struct { float x, y, w, h; } fly_rectf;
int fly_hud_layout(int w, int h, const fly_game *g, fly_rectf *out, int max);

/* The type ladder and the row pitch, shared by every surface. Both files used
 * to define their own copy of the sizes, which is how the HUD ended up with a
 * 17 px cash figure over an 11 px hull figure in the same three-row block:
 * nothing said they were the same kind of thing, so nothing kept them alike.
 *
 * FLY_FS_KEY is every property label, FLY_FS_VAL every property value, and
 * FLY_UI_ROW the pitch they sit on. A row is a band of that height and the
 * text sits on its centre line, so rows read as evenly spaced because they
 * are, rather than because three separate offsets happened to agree. */
#define FLY_FS_S 11.0f     /* ticks, footnotes, scale labels */
#define FLY_FS_KEY 11.0f   /* every property label */
#define FLY_FS_VAL 13.0f   /* every property value */
#define FLY_FS_HEAD 17.0f  /* headline figures and banners */
#define FLY_FS_TITLE 26.0f
#define FLY_UI_ROW 16.0f

/* top of a box of height `px` centred in a row at `y` — bars and swatches */
#define FLY_ROW_BOX(y, px) ((y) + (FLY_UI_ROW - (px)) * 0.5f)
/* the row's centre line, which is where text goes: hand it to fly_text_mid and
 * an 11 px label and a 13 px value in the same row sit on the same line. Doing
 * it by the em box instead put them a pixel apart, in opposite directions. */
#define FLY_ROW_MID(y) ((y) + FLY_UI_ROW * 0.5f)

/* Shared style (fly_ui uses it too).
 *
 * Text is one near-white, and the weights are separations of *alpha* rather
 * than of hue. A panel whose labels are blue-grey and whose values are
 * near-white is reading two colours where it means two emphases; one ink at
 * three alphas says the same thing without spending a colour on it. The tint
 * on the dimmer two is the same cool cast the glass has, so a faint label
 * looks like less of the same ink rather than like grey paint. Colour is left
 * for the four signals — accent, warning, bad, good — which is the only place
 * it carries information.
 *
 * Everything readable goes through fly_text_sh, so every glyph carries its own
 * drop shadow and stays legible over a snowfield as well as over a night sky.
 * That shadow is what carries the widgets with no panel at all — the tapes,
 * the heading strip — and it is why they can be drawn straight on the world. */
#define FLY_UI_FG FLY_RGBA(255, 255, 255, 255)
#define FLY_UI_DIM FLY_RGBA(233, 243, 250, 224)
#define FLY_UI_FAINT FLY_RGBA(214, 230, 242, 196)
#define FLY_UI_ACCENT FLY_RGBA(122, 226, 255, 255)
#define FLY_UI_WARN FLY_RGBA(255, 198, 104, 255)
#define FLY_UI_BAD FLY_RGBA(255, 124, 112, 255)
/* The fourth signal, and it was a literal in seven places before it had a
 * name: a pact, a power that will sell to you, a standing that has earned
 * something. Colour carries information here, so a colour used seven times
 * deserves one definition. */
#define FLY_UI_GOOD FLY_RGBA(126, 232, 150, 255)
#define FLY_UI_SHADOW FLY_RGBA(0, 0, 0, 190)
/* An overlay is a sheet lying on a page, and both are the same material, so
 * the only thing that can separate them is elevation: the card is the pane
 * that stands *off* the surface under it. Cool rather than white, because the
 * glass it lands on is cool and a white lift on a blue pane is a grey stain. */
#define FLY_UI_CARD FLY_RGBA(134, 172, 206, 46)

/* --- frosted glass ---
 *
 * Every group the HUD draws sits on one of these: the frame behind it blurred,
 * levelled by the material itself (fly_img_frost), then a single cool tint over
 * the result, then a hairline around the edge. One surface, one edge, one tint.
 *
 * The tint is dark and cool, and it used to be white. White glass was chosen so
 * a pane would lift what was behind it rather than cut a hole in the picture,
 * and the argument is sound — but it was being asked to carry white type, and
 * over an overcast noon the pane came out at 200 and the type at 255. Fifty
 * levels is not a contrast, it is a rumour, and the profile page proved it:
 * white-on-pale-grey, unreadable, at the one time of day the game looks best.
 *
 * Dark glass over a levelled backdrop is the other way round and it holds
 * everywhere: the pane lands near 60 whatever the weather, white type has two
 * hundred levels under it, and the accent, warn and bad colours — which are
 * bright and saturated, and were fighting a bright panel — separate cleanly.
 * The panel is still glass and not a box: the blur and the level keep the
 * world's own light and colour moving under it, and the hairline is what says
 * where it ends. */
#define FLY_UI_GLASS FLY_RGBA(16, 26, 40, 92)
#define FLY_UI_GLASS_BLUR 7.0f
/* The reticle's band is the one piece of glass that must not go dark. It sits
 * over the middle of the frame, around the aircraft and whatever the aircraft
 * is pointed at, and the dark tint every other pane wears turns it into a
 * heavy black donut in the centre of the picture. A pale rim reads as the edge
 * of a lens, which is what an instrument you look *through* should look like. */
#define FLY_UI_LENS FLY_RGBA(206, 230, 248, 54)
/* the same material at full bleed, one stop deeper: see fly_ui_page */
#define FLY_UI_PAGE FLY_RGBA(12, 20, 32, 138)
/* Asked for, not obeyed: fly__rrect_sdf clamps the radius to the box's own
 * half-extent, so this is an upper bound and anything shorter than 30 px comes
 * out as a capsule. Banners, pills and one-row clusters get their roundness for
 * free, and no call site carries its own min(). */
#define FLY_UI_GLASS_RAD 15.0f
/* The hairline around a pane, the way a real sheet of glass catches the sky.
 * One pixel, and the only edge a group has — drawn all the way round, with a
 * brighter run along the top lip where the light would actually land. It is
 * the edge that makes a panel read as a thing lying on the picture rather than
 * as a soft patch in it, and it is cheap enough that every pane gets one. */
#define FLY_UI_GLASS_EDGE FLY_RGBA(198, 226, 244, 92)
#define FLY_UI_GLASS_LIP FLY_RGBA(236, 250, 255, 132)

/* One pad, everywhere. A group's content rect is this far inside its glass on
 * every side, and no caller is allowed a second opinion — the altitude and
 * airspeed tapes were drawn hard against their own edges because they measured
 * their own insets. */
#define FLY_UI_PAD 11.0f

/* The HUD's row geometry: the sprite gutter and the column the value or the
 * gauge bar starts in. Shared with the tests because "every row in a cluster
 * sits on the same lattice" is a claim about pixels, and a test that measured
 * it against its own guess at where the gutter ends would be measuring the
 * guess. The gutter is a little under the row pitch, so a sprite fills its row
 * and still leaves the rows above and below a clear pixel. */
#define FLY_HUD_ICON_W 13.0f
#define FLY_HUD_GAUGE_X 25.0f

/* Nav icon geometry: the two circular buttons in the top-right corner, drawn by
 * fly_ui and shared with the tests. It lives here, with the rest of the shared
 * chrome, because the HUD has to stack its own top-right cluster clear of them
 * — the task summary used to be drawn straight underneath the buttons, and a
 * constant copied into two files is how it would happen again. */
#define FLY_NAV_ICON_R 13.0f
#define FLY_NAV_HIT_R 17.0f
#define FLY_NAV_Y 19.0f
#define FLY_NAV_SELF_X(w) ((float)(w) - 66.0f)
#define FLY_NAV_HELP_X(w) ((float)(w) - 30.0f)

/* The right-hand column belongs to the touch throttle: fly_ui draws the slider
 * in it and hit-tests it, and the HUD lays its altitude tape inboard of it. The
 * two used to be sized independently — a 0.93w hit zone against a tape pinned
 * 48 px off the right edge — so on any frame wider than about 700 px the slider
 * was drawn straight down the middle of the tape. */
#define FLY_TOUCH_STRIP 44.0f
#define FLY_TOUCH_X(w) ((float)(w) - FLY_TOUCH_STRIP * 0.5f)
#define FLY_TOUCH_TOP(h) ((float)(h) * 0.24f)
#define FLY_TOUCH_BOT(h) ((float)(h) * 0.86f)

/* --- the action column ---
 *
 * The touch buttons are a group like every other: one pane of glass, inboard
 * of the throttle strip, with its buttons stacked and centred vertically
 * inside it. FIRE used to be a bare circle at (0.86w, 0.57h) with a hit zone
 * at (0.80..0.92w, 0.50..0.64h) — a button and its target described by two
 * different sets of fractions, which is how they drift apart. One box, and
 * both the drawing and the hit test read it.
 *
 * Sized for two buttons so a second one lands under the first without moving
 * anything, which is what a column is for. */
#define FLY_ACT_BTN 34.0f    /* button diameter */
#define FLY_ACT_GAP 10.0f    /* between stacked buttons */
#define FLY_ACT_W (FLY_ACT_BTN + 2.0f * FLY_UI_PAD)
/* the column's box for `n` buttons, centred on the throttle strip's own band */
#define FLY_ACT_H(n) ((float)(n) * FLY_ACT_BTN + ((float)(n) - 1.0f) * FLY_ACT_GAP \
                      + 2.0f * FLY_UI_PAD)
#define FLY_ACT_X(w) ((float)(w) - FLY_TOUCH_STRIP - FLY_ACT_W - 6.0f)
#define FLY_ACT_CY(h) ((FLY_TOUCH_TOP(h) + FLY_TOUCH_BOT(h)) * 0.5f)
#define FLY_ACT_Y(h, n) (FLY_ACT_CY(h) - FLY_ACT_H(n) * 0.5f)
/* centre of button `i` of `n` in that column */
#define FLY_ACT_BX(w) (FLY_ACT_X(w) + FLY_ACT_W * 0.5f)
#define FLY_ACT_BY(h, n, i) (FLY_ACT_Y(h, n) + FLY_UI_PAD + FLY_ACT_BTN * 0.5f \
                             + (float)(i) * (FLY_ACT_BTN + FLY_ACT_GAP))

/* translucent rounded card with a soft drop shadow */
void fly_ui_card(fly_img *im, float x, float y, float w, float h, uint32_t fill);
/* the pane of frosted glass every corner group sits on */
void fly_ui_scrim(fly_img *im, float x, float y, float w, float h);
/* and the same material at full bleed: the surface a whole page is read on */
void fly_ui_page(fly_img *im);
/* the same glass, round: the radar, anything circular and filled */
void fly_ui_glass_disc(fly_img *im, float cx, float cy, float r, uint32_t tint);
/* and as a band, for the reticle: an instrument you look through */
void fly_ui_glass_ring(fly_img *im, float cx, float cy, float r, float thick,
                       uint32_t tint);
/* slim rounded gauge bar (frameless, antialiased) */
void fly_ui_bar(fly_img *im, float x, float y, float w, float frac, uint32_t color);
/* One sprite from the shared set, drawn centred in a box of side `s`.
 *
 * Two ways in. fly_ui_sprite is the full-colour object: menus and headings,
 * where the hue does half the sorting before a word is read. fly_ui_icon is
 * the same artwork in one colour, for instruments, where the colour of the row
 * is already saying something — accent, warn, bad — and a sprite bringing its
 * own palette would be arguing with it. */
typedef enum {
    FLY_ICON_NONE, FLY_ICON_SERVICE, FLY_ICON_TASK, FLY_ICON_TRADE,
    FLY_ICON_PART, FLY_ICON_CRAFT, FLY_ICON_ROUTE, FLY_ICON_CASH,
    /* the instrument set: what a gauge is measuring, said in one glyph.
     * Four-letter keys — FUEL POWR HULL WEAR — are four words the eye has to
     * read before it can look at the bar it came for. */
    FLY_ICON_FUEL, FLY_ICON_POWER, FLY_ICON_SHIELD, FLY_ICON_WEAR,
    FLY_ICON_WIND, FLY_ICON_LIFE, FLY_ICON_GUST,
    /* the space instruments: propellant in the tanks, heat in the hull */
    FLY_ICON_PROP, FLY_ICON_HEAT,
    /* a pennant on a staff: whose colours the operator is wearing. It is the
     * same shape as the thing flying over every settlement, on purpose — the
     * glyph and the mast should be recognisably one idea. */
    FLY_ICON_FLAG,
    /* What is on the rails, and what is in the dispenser. Two glyphs rather
     * than one, because the magazine row prints both counts and a single icon
     * over "SKR 4 FLR 18" would be labelling half of what it sits next to. */
    FLY_ICON_ORD, FLY_ICON_DECOY,
    /* head and shoulders: the operator, on the nav button and over the profile
     * page it opens. The same shape in both places, so the button and the page
     * are visibly the same door. */
    FLY_ICON_PILOT,
    /* a cog: what the pad does to the machine. Refuel and repair used to wear
     * the same droplet, which made the two things you can buy at every field
     * in the game one thing with two labels. */
    FLY_ICON_REPAIR,
    FLY_ICON_COUNT
} fly_icon;
void fly_ui_icon(fly_img *im, fly_icon ic, float cx, float cy, float s, uint32_t col);
void fly_ui_sprite(fly_img *im, fly_icon ic, float cx, float cy, float s);

#endif /* FLY_HUD_H */
