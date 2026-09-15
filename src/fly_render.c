#include "fly_render.h"

#include "fly_glsl.h"
#include "fly_gpu.h"

#include <stdlib.h>
#include <string.h>

/* Worker threads for the path tracer, when the toolchain has them.
 *
 * Probed and defined by make.py exactly as FLY_GPU is, and absent by default,
 * so a build without them is a build that runs the same tracer on one core and
 * produces the same frame. Nothing else in the app is threaded: the simulation
 * is a serial state machine on purpose, and this is a pure function of the
 * world evaluated a million times over. */
#ifdef FLY_THREADS
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#endif

/* ---------------- phase timing ----------------
 *
 * See fly_render_timings. One monotonic clock, nine accumulators and a stack of
 * one: phases do not nest, so entering a new one closes the last. The clock is
 * read twice per phase, which is nanoseconds against a frame, so this is always
 * on rather than behind a build flag — a measurement nobody can take without a
 * special build is a measurement nobody takes. */
#ifdef _WIN32
#include <windows.h>
static double fly__clock_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double fly__clock_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}
#endif

#ifdef _WIN32
/* windef.h (pulled in by windows.h) defines `far` and `near` as empty macros
 * and `FAR`/`NEAR` as aliases of them; the renderer uses all four as
 * identifiers. Undo the macros so the code below compiles as written. */
#undef far
#undef near
#undef FAR
#undef NEAR
#endif

static double g_phase_ms[FLY_PHASE_COUNT];  /* published: the last whole frame */
static double g_phase_acc[FLY_PHASE_COUNT]; /* the frame being drawn */
static double g_phase_t0;
static int g_phase_cur = -1;
static long g_phase_verts, g_phase_verts_acc;

static void phase_begin(void) {
    int i;
    for (i = 0; i < FLY_PHASE_COUNT; ++i) g_phase_acc[i] = 0.0;
    g_phase_verts_acc = 0;
    g_phase_cur = -1;
}

/* Close the open phase and open `p`; FLY_PHASE_COUNT closes without opening. */
static void phase(int p) {
    double t = fly__clock_ms();
    if (g_phase_cur >= 0 && g_phase_cur < FLY_PHASE_COUNT)
        g_phase_acc[g_phase_cur] += t - g_phase_t0;
    g_phase_cur = p < FLY_PHASE_COUNT ? p : -1;
    g_phase_t0 = t;
}

static void phase_end(void) {
    int i;
    phase(FLY_PHASE_COUNT);
    for (i = 0; i < FLY_PHASE_COUNT; ++i) g_phase_ms[i] = g_phase_acc[i];
    g_phase_verts = g_phase_verts_acc;
}

/* Object vertices, counted where they are built rather than where they are
 * uploaded.
 *
 * It used to be counted off the capture buffer, which meant a machine with no
 * GL backend reported a build of zero — for a build it had done all of, on the
 * software rasterizer, and then rasterized as well. The count is gated on the
 * object phase being the open one instead, which is exactly the claim the
 * printout makes: the terrain rings run their triangles through the same two
 * rasterizers, and they run them in FLY_PHASE_TERRAIN. */
static void phase_verts(int n) {
    if (g_phase_cur == FLY_PHASE_OBJECTS) g_phase_verts_acc += n;
}

void fly_render_timings(double *ms, long *verts) {
    int i;
    if (ms) for (i = 0; i < FLY_PHASE_COUNT; ++i) ms[i] = g_phase_ms[i];
    if (verts) *verts = g_phase_verts;
}

const char *fly_render_phase_name(int p) {
    static const char *names[FLY_PHASE_COUNT] = {
        "setup", "shadow", "terrain", "objects", "upload",
        "finish", "readback", "resolve", "overlay"
    };
    return p >= 0 && p < FLY_PHASE_COUNT ? names[p] : "?";
}

#define FLY_WATER_Z FLY_WATER_LEVEL
/* Sand, both as the terrain laid over the shore band and as the bed seen
 * through shallow water. One constant so the wet strip and the dry strip
 * either side of the waterline are the same beach. */
#define FLY_WATER_BED fly_v3mk(0.55f, 0.50f, 0.36f)
#define FLY_CLOUD_Z 2600.0f
/* Cloud radiance, as three numbers instead of six colours. `ALB` is a thick
 * water cloud's reflectance, `SKY` turns the radiance straight up at the deck
 * into the irradiance the top sees over its whole hemisphere (above one,
 * because a sky is brightest at its own horizon), and `THRU` is how much of
 * the sun reaches an underside. See fly__env.cloud_lit. */
#define FLY_CLOUD_ALB 0.92f
#define FLY_CLOUD_SKY 1.7f
#define FLY_CLOUD_THRU 0.18f
/* What falls on anything when the sun is down: one number, because there is
 * one moon, and every surface in the world is under it.
 *
 * It used to be a radiance of (0.05, 0.055, 0.085) added to both faces of the
 * cloud deck and (0.04, 0.05, 0.08) added on top of every lit surface —
 * borrowed each from the other on the argument that a cloud is a surface too.
 * Neither is a radiance. A radiance added after the albedo is light a surface
 * emits, not light that falls on it, so it does not go through what the
 * surface is made of: snow and basalt came out the same colour at night, which
 * is the one thing moonlight never does.
 *
 * The size was the visible half of it. A floor of 0.056 put the night deck at
 * twelve times the sky behind it — a moonlit deck under a moonless sky, the
 * two halves of one night disagreeing about whether there is a moon in it —
 * and it put the *ground* at 0.0569 against 0.0552 at noon. Those are the same
 * number. The landscape did not get dark when the sun went down; it stayed at
 * its own midday shade under a sky nine tenths gone, so every night frame in
 * the gallery is a pale plain with a black sky over it and the ground five or
 * six times brighter than the sky it sits under.
 *
 * So it is an irradiance now, and it goes through the albedo like every other
 * light in the scene. On the deck it reaches the underside through
 * FLY_CLOUD_THRU, so the two cloud faces stay a lit one and a shaded one; on
 * the ground it lands at 0.22 of the sky at twenty degrees and 0.066 of its own
 * noon — under the sky rather than over it. Set against the sky rather than by eye at both
 * ends: an overcast night base comes out at 0.0070 against 0.0048 of the sky
 * it covers, the same order rather than an order above it, and it is still
 * lit — dropping it to zero would leave a black hole in a night sky for
 * anyone flying over the deck, and a landscape nobody can walk out of.
 * `render.atmosphere` holds the deck's two ends and `render.ambient` the
 * ground's. */
#define FLY_MOON fly_v3mk(0.0100f, 0.0110f, 0.0170f)
/* Where a cloud tap gives up its fine octaves and where it gives up the field
 * altogether, both in metres of tap movement per output pixel. See the march. */
#define FLY_CLOUD_FINE 250.0f
#define FLY_CLOUD_FLAT 1800.0f
/* Extinction: metres of unit-density cloud that attenuate what is behind them
 * by 1/e. The march integrates `1 - exp(-dens*seg/L)` over each of its four
 * segments, so what a ray picks up is the length of cloud it crossed and not
 * merely the fact that it crossed some. 322 m is not a taste setting: it is
 * `145 / 0.45`, the number that reproduces the old fixed alpha for a ray
 * straight down through the slab, whose four segments are 145 m each. That
 * keeps the deck the same deck from underneath — where the game is flown and
 * where every cloud measurement in this file was taken — and changes only the
 * rays the old model got wrong, which are the long ones. */
#define FLY_CLOUD_EXT 322.0f
/* How much of its own segment a tap has to average over before the field is
 * allowed to keep its coarse octaves. The march samples each segment at three
 * points a third of a segment apart, so the whole interval is walked at twelve
 * uniform stations, and 0.65 of a segment is a little over twice that spacing
 * — which is what it takes to keep the quadrature from aliasing. Measured over
 * the four hours `render.atmosphere` sweeps: at 0.5 the worst elevation kink is
 * 0.71%, at 0.65 it is 0.54% and at 0.9 it is 0.35%, while the deck's own
 * detail at eight degrees goes the other way — 0.00464, 0.00459, 0.00450
 * against a bound of 0.0045. 0.65 is where both have room. */
#define FLY_CLOUD_SEGW 0.65f
/* --- how far up a cloud goes ---------------------------------------------
 *
 * The deck was two parallel planes 580 m apart, everywhere, with one triangular
 * vertical profile inside them. Coverage decided how opaque a column was and
 * nothing decided how *tall* it was, so every cloud in the world had the same
 * top at the same altitude: a lid rather than a sky. From underneath that is
 * only a little wrong, but the whole game is flown at cloud height and from
 * the side or from above it is the difference between weather and a ceiling.
 *
 * The base stays flat, which is not a simplification — a cumulus field really
 * does have one base, because it is the height the air it rises through
 * reaches saturation, and a photograph of one shows the flat bottoms lined up.
 * What varies is the top, and it varies with the same coverage field that
 * decides how thick the cloud is: a bare column stays a sheet at
 * FLY_CLOUD_MIN of the full depth, a full one builds the whole way.
 *
 * The march keeps its four taps and spends them between the base and *this
 * column's* top rather than between the base and the highest top in the world.
 * That costs one coverage lookup a ray, not a tap, and it is what stops the
 * thin cloud — which is most of the sky on a fair day — from being sampled
 * four times across a slab it only occupies a third of. */
#define FLY_CLOUD_BASE 260.0f  /* below FLY_CLOUD_Z: the condensation level */
#define FLY_CLOUD_RISE 320.0f  /* above it, where the tallest column tops out */
#define FLY_CLOUD_MIN 0.42f    /* of the full depth, where the field is bare */
/* --- the ice, seven kilometres up ----------------------------------------
 *
 * The deck is one layer of water cloud at one altitude, and on a fair day —
 * when the coverage field is mostly under its threshold — what the player
 * flies under is an empty gradient. What is missing on those days is not more
 * cumulus, it is cirrus: ice, high enough to be lit when everything below it
 * is not, drawn out into bands by the shear that made it.
 *
 * A sheet and not a volume. One plane intersection and one field lookup, no
 * march: at seven kilometres a cirrus deck is a few hundred metres thick and
 * tens of kilometres across, so its own depth is inside a pixel from anywhere
 * the game is flown and marching it would be four taps spent on nothing. What
 * a slant ray does pick up is a longer path *through* the sheet, and that it
 * gets — the same Beer-Lambert the deck's march uses, over `1/|rd.z|` of the
 * vertical crossing, capped where the geometry stops meaning anything.
 *
 * It has one colour rather than a lit/dark pair, because a sheet this thin has
 * no underside to shade: what you see from below is mostly sunlight that came
 * through it. Its albedo is well under the deck's for the same reason, and it
 * reads as a veil the sky comes through rather than as a second deck — at noon
 * over a lattice of columns the ice measures 0.71 against the deck's 0.83.
 *
 * `FLY_CIRRUS_FINE` and `FLY_CIRRUS_FLAT` are the deck's two ramps at this
 * field's scale. Its finest octave is 1.1 km across the wind against the
 * deck's 650 m, and both constants keep the deck's ratio to that: the sheet
 * sits three times higher than the deck, so its tap slides three times further
 * per pixel at the same elevation and it needs those ramps more, not less. */
#define FLY_CIRRUS_Z 7000.0f
#define FLY_CIRRUS_ALB 0.55f     /* ice, and thin */
#define FLY_CIRRUS_TAU 0.62f     /* optical depth straight down at full cover */
#define FLY_CIRRUS_SLANT 14.0f   /* how much of the slant path is worth having */
#define FLY_CIRRUS_LEN 34000.0f  /* the band, along the prevailing bearing */
#define FLY_CIRRUS_WID 9000.0f   /* and across it */
#define FLY_CIRRUS_FINE 420.0f
#define FLY_CIRRUS_FLAT 3100.0f
/* --- what the two layers take out of the sunbeam -------------------------
 *
 * `FLY_CLOUD_SHADE` is the deck's, and it is not a transmittance: a thick
 * water cloud passes essentially no direct beam, and the 0.38 a full deck
 * leaves is the deck's own base standing in for one, because the ground under
 * an overcast is lit by the cloud rather than by nothing. It has been that
 * number since the shadow was written; naming it is all that changed.
 *
 * The ice's is derived rather than picked. `FLY_CIRRUS_TAU` is the sheet's
 * optical depth straight down at full cover, so it removes `1 - exp(-0.62)` =
 * 0.462 of the beam, and what is removed is lost to the ground below in the
 * same proportion the deck's is — 0.62 of it, the rest arriving as the forward
 * scattering that makes a cirrus sky bright around the sun. 0.62 * 0.462 =
 * 0.286: a full sheet leaves 0.71 of the sun, which is a sun that has gone
 * soft rather than one that has gone out, and that is the whole visible effect
 * of a cirrus veil on a landscape.
 *
 * How the two compose is the part that had to be decided, and it is not a
 * product. Multiplying the two visibilities would take a further 29% off the
 * 0.38 a full deck leaves — and that 0.38 is not beam, it is the deck's own
 * light, which is dimmed by the ice where the ice is: the march below carries
 * it there (see `cloud_slab`), and the sky irradiance the ambient half reads
 * carries it because that sky is sampled through both layers. Dimming it again
 * here would be counting the same sheet twice. So the ice shades what the deck
 * left clear and nothing else: `1 - cd*SHADE - ci*CIRRUS_SHADE*(1 - cd)`,
 * which is smooth, never doubles a loss, and reduces to each layer alone. */
#define FLY_CLOUD_SHADE 0.62f
#define FLY_CIRRUS_SHADE 0.286f
/* What the deck's crown is worth against its base.
 *
 * The march's colour used to be `lerp(cloud_lit, cloud_dark, cs*0.85 +
 * dens*0.25)`, and neither term knew where in the layer the sample sat — so
 * every tap of a column carried its base's shading, and an aeroplane above the
 * deck read the same number as a pilot underneath it. At night, the one time
 * the two faces are furthest apart, it read the shaded base while looking
 * straight down at a moonlit top.
 *
 * The pair of colours the lighting already computes is exactly the two faces:
 * `cloud_lit` is the albedo times what falls on the top, `cloud_dark` is what
 * leaks through to the base. So what the shading term really asks is how much
 * cloud lies between this sample and the face the light is on, and both halves
 * of it — the neighbouring column's shadow and the sample's own density — are
 * proportional to how far below the crown it sits. `hh`, which the column's
 * own top already costs, is that depth, and it is the whole key.
 *
 * Not to zero at the crown. Cloud tops are white with grey between the towers,
 * and that grey is the neighbour shadow this term is made of; two thirds off
 * is what puts the crown near `cloud_lit` while leaving the towers their shape.
 *
 * Keyed on the sample and not on the view direction, which was tried first and
 * is wrong in the one place it is easy to check: `rd.z` says a camera *inside*
 * the deck looking down is above the layer, so the six hundred metres of cloud
 * between it and a hillside came out as sunlit crown and `render.atmosphere`'s
 * wash measure went from 0.43 of the low camera's spread to 1.75, which is past
 * what the check measures with the deck taken out of the aerial perspective
 * altogether. The depth key gets that frame right for the same reason it gets
 * the aeroplane right: the march is front-to-back, so whichever face the eye is
 * on is the face its first taps land on, and nothing has to be told which
 * that is.
 *
 * Two more things are measured rather than chosen.
 *
 * The height is the sample's in the *layer* — `(p.z - cb) / depth` — and not
 * its height in its own column, which is what `hh` two lines below already
 * costs and was the obvious thing to reach for. The column's own top has the
 * local coverage in its denominator, so keying on it puts that field into the
 * tap's colour a second time and far harder than the `dens * 0.25` term does:
 * the elevation kink `render.atmosphere` watches went 0.54% to 0.77% against a
 * bound of 0.70%, at 5.4 degrees, where a grazing ray's four taps span the
 * whole slab. On the layer's own depth it is 0.63%. What that costs is the
 * shallow column, whose crown sits inside the layer and gets less of this than
 * a tower's does — which is the honest answer anyway, since a low top has the
 * towers around it standing over it.
 *
 * And the ramp starts at FLY_CLOUD_FACE_LO rather than at the base. A linear
 * ramp puts a quarter of the effect at mid-layer, and mid-layer is where a
 * camera inside the deck sits: the wash measure came back at 0.71 of the low
 * camera's spread against a bound of 0.55. The lit skin of a cloud top is
 * thinner than that — one optical depth at FLY_CLOUD_EXT is 322 m of the
 * slab's 580 — so the ramp holds off until the top half and the interior stays
 * the interior. */
#define FLY_CLOUD_FACE 0.65f
#define FLY_CLOUD_FACE_LO 0.35f
/* The height the deck is lit at, and the one `cloud_shadow` puts it at: one
 * representative top rather than the tallest, because both are asking what the
 * deck as a whole does and neither marches it. Unchanged from when the top was
 * a plane, so the lighting this feeds did not move when the tops did. */
#define FLY_CLOUD_LIT_TOP 320.0f
/* Scale on the sunbeam. Set so a noon horizontal surface lands where the old
 * hand-tuned pair put it: 1.25 * luma(1, 0.97, 0.9) / luma(T at noon). */
#define FLY_SUN_E0 1.360f

/* ---------------- target ---------------- */

int fly_rt_init(fly_render_target *rt, int w, int h) {
    if (fly_img_init(&rt->img, w, h) != 0) return -1;
    rt->depth = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    rt->hdr = (fly_v3 *)malloc(sizeof(fly_v3) * (size_t)w * (size_t)h);
    if (!rt->depth || !rt->hdr) {
        fly_img_free(&rt->img);
        free(rt->depth);
        free(rt->hdr);
        return -1;
    }
    rt->w = w;
    rt->h = h;
    return 0;
}

void fly_rt_free(fly_render_target *rt) {
    fly_img_free(&rt->img);
    free(rt->depth);
    free(rt->hdr);
    rt->depth = NULL;
    rt->hdr = NULL;
}

void fly_rt_clear(fly_render_target *rt, uint32_t color) {
    fly_img_fill(&rt->img, color);
    int i, n = rt->w * rt->h;
    for (i = 0; i < n; ++i) {
        rt->depth[i] = 1e30f;
        rt->hdr[i] = fly_v3zero();
    }
}

fly_render_opts fly_render_opts_default(void) {
    fly_render_opts o;
    o.mode = FLY_RENDER_RASTER;
    o.pt_blend = 0.5f;
    o.pt_samples = 1;
    o.ao_target = 12;
    o.ao_budget = 0;
    o.ssaa = 2;
    o.post = 1;
    o.detail = 1;
    /* Multisampling is nearly free next to supersampling — coverage and depth
     * per sample, one shader run per pixel — so it is on by default and every
     * geometry edge is antialiased even at ssaa 1. */
    o.msaa = 4;
    o.studio = 0;
    return o;
}

fly_render_opts fly_render_opts_quality(fly_render_quality q) {
    fly_render_opts o = fly_render_opts_default();
    int d = (int)q;
    if (d < 0) d = 0;
    if (d > FLY_QUALITY_ULTRA) d = FLY_QUALITY_ULTRA;
    o.detail = d;
    switch (d) {
    case FLY_QUALITY_LOW:
        /* Nothing is sampled more than once. Supersampling is the single
         * largest multiplier in the renderer — ssaa 2 is four times the
         * shading — and a level whose whole definition is a frame budget
         * cannot afford it. Multisampling stays on because it is a GPU
         * feature: where there is no GPU it costs nothing because it does
         * nothing, and where there is one it is the cheapest edge quality
         * available. Traced occlusion is off; it is a ray budget. */
        o.ssaa = 1;
        o.ao_target = 0;
        break;
    case FLY_QUALITY_MEDIUM:
        o.ssaa = 1;
        o.ao_target = 12;
        break;
    case FLY_QUALITY_HIGH:
        /* Four shaded samples an output pixel and two coverage samples inside
         * each of them: eight along an edge. See the note below for why that
         * is not the sixteen the rung used to ask for. */
        o.ssaa = 2;
        o.msaa = 2;
        o.ao_target = 16;
        break;
    default:
        /* The ceiling. Nine samples a pixel, eighteen along an edge, and
         * occlusion traced to convergence rather than to a budget. */
        o.ssaa = 3;
        o.msaa = 2;
        o.ao_target = 24;
        break;
    }
    /* --- why the top rungs multisample *less* than the cheap ones ---
     *
     * The two kinds of sampling compose, and the ladder used to price them as
     * though they did not: supersampling renders the frame at ssaa^2 the
     * pixels and multisampling stores msaa coverage samples inside every one
     * of those, so an edge at the old top rung was resolved from nine times
     * eight — seventy-two samples for one output pixel — and the ceiling was
     * paying for the seventy-two and seeing about the eighteen.
     *
     * That is not free the way an unused knob is free. A multisample target is
     * real memory and real bandwidth: at 1080p the old top rung asked a GPU
     * for a 5760x3240 colour buffer at eight samples of RGBA16F, which is over
     * a gigabyte before the depth buffer, and every fragment the pipeline
     * retires writes coverage into all eight of them. An implementation that
     * cannot make that target falls back to *one* sample — so the rung that
     * asked for the most edge quality was the one most likely to end up with
     * none.
     *
     * So the rungs above the first buy their edges with the sampling they are
     * already paying for and keep multisampling at the two that costs least
     * and covers the case supersampling covers worst. The cheap rungs keep
     * four because it is the only edge quality they have — where there is no
     * GPU it costs nothing because it does nothing, and where there is one it
     * is the cheapest quality on the board. */
    return o;
}

const char *fly_render_quality_name(fly_render_quality q) {
    switch (q) {
    case FLY_QUALITY_LOW: return "low";
    case FLY_QUALITY_MEDIUM: return "medium";
    case FLY_QUALITY_HIGH: return "high";
    case FLY_QUALITY_ULTRA: return "ultra";
    }
    return "medium";
}

/* --- what a graphics level buys, in numbers ---
 *
 * Every cost the renderer can trade is priced here, in one table, rather than
 * as `detail >= 2` scattered through nine thousand lines. That is not tidiness.
 * A level is a claim about a frame budget, and a budget that is spent in
 * twenty places nobody can see at once is a budget nobody can hold: the way a
 * "low" preset stops being low is one `>= 1` at a time, each of them locally
 * reasonable. Written down together they can be read, measured and defended.
 *
 * What is *not* here is worth naming too. Smooth-shading a lofted hull is not a
 * level: it costs two vector adds a vertex and no triangles at all, and it is
 * the single largest difference in how a model reads, so it is unconditional
 * and the cheapest rung gets it too — see loft_hull. A knob that would never be
 * turned off is not a knob. */

/* The widest ring, the finest section and the longest hull any level may ask
 * for. These bound the stack buffers the model builders loft through, so they
 * are the ceiling rather than the setting; `lod_for` picks what is actually
 * used, and the top rung is the only one that reaches them. */
#define FLY_FOIL_N 16   /* points around an aerofoil section */
#define FLY_RING_N 18   /* points around the widest body ring */
#define FLY_LOFT_MAX 8  /* stations in the longest hull */

typedef struct {
    int ring;        /* points around the widest body ring */
    int foil;        /* points around an aerofoil section */
    float grass;     /* grass scatter reach, metres (0 = no grass layer) */
    int grass_den;   /* of 8 candidate cells, how many grow a tuft */
    float trees;     /* woodland reach and LOD distance multiplier */
    int puffs;       /* lobes per fireball and per smoke plume */
    int shafts;      /* volumetric march steps per pixel (0 = off) */
    /* Output pixels per software sky sample; 1 evaluates every one.
     *
     * Only the budget rung interpolates, and that is a rule about what the
     * rungs are for rather than a tuning. The default rung is the reference —
     * it is what the GPU pipeline is held to and what the gallery is shot at —
     * and a lattice cannot promise to resolve a feature thinner than its own
     * cell, whatever the refinement test says, because the test is itself made
     * of lattice samples. Interpolation is a way to spend less; it is not a way
     * to draw the same thing. */
    int sky;
    /* Shade the terrain rings in a fragment stage rather than at their
     * vertices. Off on the budget rung and on everywhere else, and that split
     * is the same rule `sky` above states: the default rung is the reference —
     * it is what the GPU pipeline is held to and what the gallery is shot at,
     * and the GPU has shaded terrain per fragment since it had a fragment
     * stage — while LOW is a frame budget on one CPU core, and a per-pixel
     * `terrain_surface` is the most expensive thing that could be put in front
     * of it. See raster_tri_ground for what it buys. */
    int ground_px;
} fly__lod;

static fly__lod lod_for(int detail) {
    fly__lod l;
    if (detail < 0) detail = 0;
    if (detail > FLY_QUALITY_ULTRA) detail = FLY_QUALITY_ULTRA;
    switch (detail) {
    case FLY_QUALITY_LOW:
        l.ring = 10; l.foil = 8;
        l.grass = 0.0f; l.grass_den = 0; l.trees = 0.55f;
        l.puffs = 4; l.shafts = 0; l.sky = 4; l.ground_px = 0;
        break;
    case FLY_QUALITY_MEDIUM:
        l.ring = 10; l.foil = 8;
        l.grass = 300.0f; l.grass_den = 4; l.trees = 1.0f;
        l.puffs = 7; l.shafts = 0; l.sky = 1; l.ground_px = 1;
        break;
    case FLY_QUALITY_HIGH:
        l.ring = 14; l.foil = 12;
        l.grass = 420.0f; l.grass_den = 6; l.trees = 1.25f;
        l.puffs = 10; l.shafts = 12; l.sky = 1; l.ground_px = 1;
        break;
    default:
        l.ring = FLY_RING_N; l.foil = FLY_FOIL_N;
        l.grass = 560.0f; l.grass_den = 8; l.trees = 1.5f;
        l.puffs = 14; l.shafts = 24; l.sky = 1; l.ground_px = 1;
        break;
    }
    /* The aircraft is not where a software frame goes. A whole airframe is a
     * few hundred triangles against tens of thousands in the terrain ring
     * mesh, so the cheapest level keeps the model it has and spends its
     * savings on the ground; what the higher rungs buy is a rounder hull, not
     * a hull the low rung had to give up. */
    return l;
}

/* ---------------- camera ---------------- */

fly_cam fly_cam_chase(const fly_game *g) {
    fly_cam c;
    fly_v3 fwd = fly_qrot(g->player.craft.ori, fly_v3mk(1, 0, 0));
    fly_v3 flat = fly_v3norm(fly_v3mk(fwd.x, fwd.y, 0));
    if (fly_v3len(flat) < 0.1f) flat = fly_v3mk(1, 0, 0);
    c.pos = fly_v3add(g->player.craft.pos, fly_v3add(fly_v3scale(flat, -26.0f), fly_v3mk(0, 0, 9.0f)));
    float gz = fly_world_ground(&g->world, c.pos.x, c.pos.y) + 2.5f;
    if (c.pos.z < gz) c.pos.z = gz;
    c.fwd = fly_v3norm(fly_v3sub(fly_v3add(g->player.craft.pos, fly_v3scale(fwd, 30.0f)), c.pos));
    /* bank gently with the craft; widen the view with speed */
    {
        float roll, pitch, yaw;
        fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
        float bank = fly_clampf(roll, -1.0f, 1.0f) * 0.30f;
        fly_v3 up0 = fly_v3mk(0, 0, 1);
        fly_v3 right = fly_v3norm(fly_v3cross(c.fwd, up0));
        c.up = fly_v3norm(fly_v3add(fly_v3scale(up0, cosf(bank)), fly_v3scale(right, sinf(bank))));
    }
    float speed = fly_v3len(g->player.craft.vel);
    c.fov = (60.0f + fly_clampf(speed / (g->player.airframe.vne + 1.0f), 0.0f, 1.0f) * 14.0f) * FLY_DEG2RAD;
    /* Airframe shake.
     *
     * Rough air moves the aeroplane, and the camera is bolted to it. Three
     * incommensurate sine terms per axis rather than noise, so it never finds
     * a beat and never repeats; amplitude follows the turbulence the weather
     * actually has and the dynamic pressure the airframe is flying at, because
     * an aircraft parked in a gale does not shake and one at cruise in the
     * same gale does. A closed form of the clock, not an accumulator: the
     * camera is not simulation state and must not become a way for the view to
     * change what the replay does. */
    {
        float turb = fly_clampf(g->weather.turbulence, 0.0f, 1.0f);
        float q = fly_clampf(speed / (g->player.airframe.vne * 0.6f + 1.0f), 0.0f, 1.2f);
        float amp = turb * q * 0.085f * (g->player.craft.on_ground ? 0.15f : 1.0f);
        float t = (float)g->time_s;
        fly_v3 right = fly_v3norm(fly_v3cross(c.fwd, c.up));
        float sx = sinf(t * 8.7f) * 0.6f + sinf(t * 19.3f + 1.7f) * 0.3f + sinf(t * 31.1f) * 0.1f;
        float sy = sinf(t * 7.1f + 2.4f) * 0.6f + sinf(t * 17.9f) * 0.3f + sinf(t * 29.3f + 0.8f) * 0.1f;
        c.pos = fly_v3add(c.pos, fly_v3add(fly_v3scale(right, sx * amp * 1.6f),
                                           fly_v3scale(c.up, sy * amp * 1.6f)));
        c.fwd = fly_v3norm(fly_v3add(c.fwd, fly_v3add(fly_v3scale(right, sx * amp * 0.07f),
                                                      fly_v3scale(c.up, sy * amp * 0.07f))));
    }
    return c;
}

fly_cam fly_cam_player(const fly_game *g) {
    if (g->mode == FLY_MODE_FLIGHT) return fly_cam_chase(g);
    fly_cam c;
    float yaw = g->walker.yaw, pitch = g->walker.pitch;
    if (g->mode == FLY_MODE_RAIL) {
        /* Trailing the pod, and above its roof rather than level with it — the
         * pod rides on the crown of the pipe now, so an eye at the old height
         * was inside it. Far enough back that the pod reads as something you
         * are following down the line instead of a wall in front of you. */
        yaw += atan2f(g->rail.tangent.y, g->rail.tangent.x);
        c.pos = fly_v3add(g->rail.pos,
                          fly_v3add(fly_v3scale(g->rail.tangent, -7.2f), fly_v3mk(0, 0, 4.1f)));
    } else {
        c.pos = fly_v3add(g->walker.pos, fly_v3mk(0, 0, g->walker.eye_height));
    }
    c.fwd = fly_v3norm(fly_v3mk(cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch)));
    c.up = fly_v3mk(0, 0, 1);
    c.fov = 68.0f * FLY_DEG2RAD;
    c.ortho_h = 0.0f;
    return c;
}

fly_cam fly_cam_orbit(fly_v3 center, float yaw, float pitch, float dist) {
    fly_cam c;
    fly_v3 dir = fly_v3mk(cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch));
    c.pos = fly_v3add(center, fly_v3scale(dir, dist));
    c.fwd = fly_v3scale(dir, -1.0f);
    c.up = fly_v3mk(0, 0, 1);
    c.fov = 55.0f * FLY_DEG2RAD;
    c.ortho_h = 0.0f;
    return c;
}

fly_cam fly_cam_ortho(fly_v3 center, float yaw, float pitch, float dist, float height) {
    fly_cam c;
    fly_v3 dir = fly_v3mk(cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch));
    c.pos = fly_v3add(center, fly_v3scale(dir, dist));
    c.fwd = fly_v3scale(dir, -1.0f);
    /* world up degenerates when looking straight down; fall back to +X so a
     * top-down elevation still has a well-defined frame (nose toward frame up) */
    c.up = fabsf(c.fwd.z) > 0.985f ? fly_v3mk(1, 0, 0) : fly_v3mk(0, 0, 1);
    c.fov = 55.0f * FLY_DEG2RAD;
    c.ortho_h = height > 0.001f ? height : 1.0f;
    return c;
}

/* view-space basis */
typedef struct {
    fly_v3 right, up, fwd;
    fly_v3 pos;
    float sx, sy;
    int ortho;   /* parallel projection: screen offset does not divide by depth */
    /* Supersample factor this target is rendered at. Anything sized in *output*
     * pixels — the rail's minimum on-screen width — has to divide by it, or
     * supersampling silently draws a fatter object instead of a better sampled
     * one, and the frame stops being the same scene at a different sample
     * count. Nothing else should need it. */
    int aa;
} fly__view;

static fly__view make_view(const fly_cam *cam, int w, int h) {
    fly__view v;
    v.pos = cam->pos;
    v.fwd = fly_v3norm(cam->fwd);
    v.right = fly_v3norm(fly_v3cross(v.fwd, cam->up));
    v.up = fly_v3cross(v.right, v.fwd);
    if (cam->ortho_h > 0.0f) {
        v.ortho = 1;
        v.sy = (float)h / cam->ortho_h; /* pixels per world unit */
    } else {
        v.ortho = 0;
        v.sy = ((float)h * 0.5f) / tanf(cam->fov * 0.5f);
    }
    v.sx = v.sy;
    v.aa = 1;
    (void)w;
    return v;
}

/* Conservative view-cone reject for an object's bounding sphere.
 *
 * Settlements, craft, ground items and rail spans are drawn from a distance
 * list only, so every discovered site used to build and shade its geometry
 * even when it sat behind the camera — the per-triangle work (cloud deck, and
 * a full sky sample for the fog colour) runs before anything is projected, so
 * off-screen sites were not free. */
static int sphere_visible(const fly__view *v, int w, int h, fly_v3 c, float r) {
    fly_v3 d = fly_v3sub(c, v->pos);
    float z = fly_v3dot(d, v->fwd);
    float ex = (float)w * 0.5f / v->sx, ey = (float)h * 0.5f / v->sy;
    float ax = fabsf(fly_v3dot(d, v->right)), ay = fabsf(fly_v3dot(d, v->up));
    if (v->ortho) return z > -r && ax <= ex + r && ay <= ey + r;
    if (z < -r) return 0;
    if (z < 0.0f) z = 0.0f;
    /* the slab margin is r/cos(half-angle) = r*sqrt(1 + tan^2) */
    if (ax > z * ex + r * sqrtf(1.0f + ex * ex)) return 0;
    if (ay > z * ey + r * sqrtf(1.0f + ey * ey)) return 0;
    return 1;
}

static int project(const fly__view *v, int w, int h, fly_v3 p, float *sx, float *sy, float *depth) {
    fly_v3 d = fly_v3sub(p, v->pos);
    float z = fly_v3dot(d, v->fwd);
    if (v->ortho) {
        if (z < 0.001f) return 0;
        *sx = (float)w * 0.5f + fly_v3dot(d, v->right) * v->sx;
        *sy = (float)h * 0.5f - fly_v3dot(d, v->up) * v->sy;
        *depth = z;
        return 1;
    }
    if (z < 0.35f) return 0;
    *sx = (float)w * 0.5f + fly_v3dot(d, v->right) / z * v->sx;
    *sy = (float)h * 0.5f - fly_v3dot(d, v->up) / z * v->sy;
    *depth = z;
    return 1;
}

/* Perspective-correct barycentrics.
 *
 * Screen-space linear interpolation of view depth is only right for a parallel
 * projection: under perspective the linear quantity is 1/z. Interpolating z
 * itself put metres of error into long triangles — a landing pad's fan
 * stretches from its centre to its rim across a 50 m depth range, and whole
 * wedges of it lost the z-test to flat terrain 0.6 m *below* the deck.
 * Dividing once per pixel also makes the interpolated vertex colours
 * perspective-correct, the way the GPU treats the same geometry.
 *
 * The error is bounded by (zmax-zmin)^2/(8*zmin), so a triangle whose corners
 * sit at nearly one depth — which is most of the scatter and most object faces
 * — is left on the cheap affine path. Setting up costs three divides, so that
 * test matters most for the many tiny triangles, not the few large ones. */
#define FLY_PERSP_SETUP(v_, az_, bz_, cz_, ia_, ib_, ic_, persp_)              \
    float ia_ = 0.0f, ib_ = 0.0f, ic_ = 0.0f;                                  \
    int persp_ = 0;                                                            \
    do {                                                                       \
        float lo_ = fminf(fminf(az_, bz_), cz_);                               \
        float hi_ = fmaxf(fmaxf(az_, bz_), cz_);                               \
        /* 0.16*zmin is the span at which the affine error reaches 2 cm */     \
        persp_ = !(v_)->ortho && (hi_ - lo_) * (hi_ - lo_) > 0.16f * lo_;      \
        if (persp_) { ia_ = 1.0f / (az_); ib_ = 1.0f / (bz_); ic_ = 1.0f / (cz_); } \
    } while (0)

#define FLY_PERSPECTIVE(persp_, uu_, u_, vv_, az_, bz_, cz_, ia_, ib_, ic_, z_) \
    do {                                                                       \
        if (!(persp_)) {                                                       \
            (z_) = (uu_) * (az_) + (u_) * (bz_) + (vv_) * (cz_);                \
        } else {                                                               \
            float iz_ = (uu_) * (ia_) + (u_) * (ib_) + (vv_) * (ic_);           \
            (z_) = 1.0f / iz_;                                                  \
            (uu_) *= (ia_) * (z_);                                              \
            (u_) *= (ib_) * (z_);                                               \
            (vv_) *= (ic_) * (z_);                                              \
        }                                                                      \
    } while (0)

/* ---------------- unified sun shadow map ----------------
 *
 * One coherent occlusion story for the raster + MIX pipelines: terrain,
 * settlements and craft are rasterized into a sun-space (orthographic) depth
 * map, then every lit surface samples it with PCF. This replaces the old
 * per-vertex terrain shadow march, the screen-space site/blob shadow sprites
 * and the shadow-free buildings — towers now fall across the streets and pad,
 * a craft casts a true silhouette on approach, and mountains shade valleys.
 * Two cascades cover both crisp local contact shadows and mountain-scale
 * occlusion. (The path tracer keeps its own traced shadows.) */

/* Three, not two.
 *
 * A cascade's texel size is what decides both of the things that make a shadow
 * map look wrong. It sets the amplitude of the staircase along an edge, and —
 * because the depth bias and the normal offset are both measured in texels —
 * it sets how far a shadow floats away from the object casting it. Two cascades
 * had to span the ground under the wheels and a mountain twenty kilometres out,
 * which put the near one on one-to-four-metre texels: a four-pixel staircase on
 * a tower edge and a metre of daylight between a tower and its own shadow.
 *
 * Three lets each one cover about five times the last, so the near cascade can
 * be small enough that its texels land under a screen pixel. */
#define FLY_SM_CASCADES FLY_SHADOW_CASCADES

typedef struct fly__shadow {
    int active;                       /* built and usable this frame? */
    int gpu;                          /* cascades live in GL textures, not depth[] */
    /* Texels per side, per cascade. The far cascade is built at half the
     * others': its casters come off a terrain grid whose cells are already
     * tens of times coarser than its texels, so the extra resolution records
     * nothing that is in the geometry, and the build pass — not the filter —
     * is what a cascade costs. */
    int size[FLY_SM_CASCADES];
    int cells;                        /* terrain grid resolution per cascade */
    fly_v3 right, up, fwd;            /* light basis; fwd = light travel = -sun */
    fly_v3 center[FLY_SM_CASCADES];   /* world focus each cascade is fit around */
    float umin[FLY_SM_CASCADES];      /* light-plane origin (dot with right) */
    float vmin[FLY_SM_CASCADES];      /* light-plane origin (dot with up) */
    float texel[FLY_SM_CASCADES];     /* world units per texel */
    float half[FLY_SM_CASCADES];      /* world half-extent each cascade covers */
    float dmin[FLY_SM_CASCADES];      /* light-space depth interval the cascade */
    float dmax[FLY_SM_CASCADES];      /*   spans; only the GPU build clips to it */
    float *depth[FLY_SM_CASCADES];    /* size*size, nearest-to-sun dot(P, fwd) */
    /* Filter radius per cascade, in texels beyond the bilinear 2x2 base. It is
     * per-cascade because the point of the near one is small texels, and a
     * kernel counted in texels would shrink its penumbra to nothing at exactly
     * the range where a soft contact shadow is the whole effect. */
    int pcf[FLY_SM_CASCADES];
} fly__shadow;

/* --- the sun is a disc, and a penumbra is a distance ---------------------
 *
 * Tangent of the sun's angular radius, as this renderer models it. It is the
 * same disc `sun_jitter` samples for the path tracer's soft shadows, so both
 * pipelines describe one light rather than each inventing its own — and the
 * shared number is the whole reason the rasterizer's edge can be checked
 * against the tracer's at all.
 *
 * Several times the real sun's 0.0047, deliberately and from the tracer's
 * side: a disc that small throws a penumbra narrower than one near-cascade
 * texel out to a hundred metres of gap, so every pipeline draws a hard edge
 * and the softness the eye reads on real ground — most of which comes from
 * the sky's own half of the light, not from the sun's disc — is missing
 * everywhere.
 *
 * A filter of fixed width cannot express this. It is right where the caster
 * meets its shadow and far too sharp at the other end: a tower's shadow is
 * one edge at its foot and, two hundred metres out, a band metres wide. What
 * the width answers to is the *gap* between blocker and receiver, which is
 * what the depth map already holds and nothing was reading. */
#define FLY_SM_SUN_TAN 0.028f

/* How wide the penumbra may grow, in texels of the cascade being read.
 *
 * Five texels is what a wider filter costs: the kernel stays contiguous — one
 * tap a texel, the way the base filter already works — and grows in count, so
 * the cap is a tap budget. Twelve taps an axis against the near cascade's six
 * is four times the lookup, and only for the pixels that have something far
 * enough away standing over them.
 *
 * The cheap alternative was tried and is worse than doing nothing: spread a
 * fixed thirty-six taps across a four-metre box and visibility comes in
 * thirty-seven levels, with every tap crossing a texel boundary at nearly the
 * same moment, so the level changes in stairs and the ground contours. The
 * measurement is unambiguous — an aeroplane's shadow from 150 m came out
 * *sharper* than the same one from 16 m, because the steepest step in the scan
 * was a stair and not the edge.
 *
 * The cap being counted in texels makes it per-cascade for free, and in the
 * right direction: five near-cascade texels is a metre or two of penumbra (a
 * gap of some tens of metres), five far-cascade ones is sixty metres (a gap of
 * two kilometres). Small shadows soften over small distances and mountain
 * shadows over large ones, which is the ladder the cascades already are. */
#define FLY_SM_PEN_RMAX 5.0f

/* taps an axis the widest kernel can ask for, plus slack for the two
   fractional ends: the row of weights is built once and reused down the box */
#define FLY_SM_PEN_TAPMAX 14
/* When non-NULL, the solid-geometry primitives (tri_lit, wall_quad) hand their
 * triangles to the shadow pass instead of drawing to the screen, and every
 * other screen writer no-ops. That lets the shadow pass reuse the exact same
 * building and craft geometry the beauty pass draws — no duplicated meshes to
 * drift out of sync — so the caster silhouettes always match. */
static fly__shadow *g_shadow_cast = NULL;

static void shadow_cast_tri(fly__shadow *sm, fly_v3 a, fly_v3 b, fly_v3 c);
static float shadow_sample(const fly__shadow *sm, fly_v3 p, float ndotl);
/* A settlement capture borrows the caster's seam; see "what a settlement
 * occludes", where both of these are defined. */
struct fly__siteocc;
static struct fly__siteocc *g_occ_build;
static void occ_cast_tri(fly_v3 a, fly_v3 b, fly_v3 c);
static void shadow_free(fly__shadow *sm);

/* ---------------- shared environment ---------------- */

typedef struct {
    /* The limb needs to know what colour the ground is four hundred kilometres
     * away, and a seed alone cannot answer that. */
    const fly_world *world;
    fly_v3 sun;
    float day;    /* 0 night .. 1 noon */
    float dusk;   /* warm grading near the horizon crossing */
    float storm;
    /* How wet the world's surfaces are, 0 dry .. 1 running with it.
     *
     * The weather already had precipitation in it and the renderer only ever
     * spent it on streaks across the lens, so it rained on a landscape that
     * stayed bone dry — which is the one thing that gives away a weather
     * system as an overlay. Everything that is out in it reads this: the
     * ground darkens and takes a sheen, standing water collects on the flat,
     * and pavement — which is the surface a viewer knows best what wet looks
     * like on — goes dark, glossy and reflective. One number a frame, from the
     * same `precip` the streaks are drawn from, so the two can never disagree
     * about whether it is raining. */
    float wet;
    double time;
    uint32_t seed;
    fly_v3 wind;
    const fly__shadow *shadow;  /* sun shadow map (raster/MIX); NULL for pure PT */
    /* Output pixels per world unit at one metre's distance, so shading can ask
     * how big a feature lands on screen and drop detail it cannot resolve.
     * Distance alone cannot answer that: the same 2 m tussock is four pixels at
     * 320 wide and twenty-four at 1920, and a fade tuned for one shimmers at
     * the other.
     *
     * Output pixels, not render-target pixels: supersampling divides out. A
     * quality setting should render the same world more cleanly, not a more
     * detailed one — otherwise the gallery stills carry detail the interactive
     * view does not, and supersampling stops being measurable as antialiasing
     * because the thing it is compared against is a different scene. */
    float pxscale;
    /* How much of the ground is the ball's rather than the chart's, 0 inside
     * the air and 1 from orbit. One number a frame — see chart_gives_way. */
    float ball;
    /* Per-biome grade, sampled once a frame at the camera and applied at the
     * very end of the resolve. A place has a cast — a moor is not a savanna is
     * not a snowfield — and the scattering model cannot supply it, because it
     * is about the air and not about the ground under it. Kept small and
     * bounded on purpose: this is a grade, and a grade that can wash the frame
     * is a bug waiting for a screenshot. */
    fly_v3 grade;  /* per-channel gain */
    float sat;     /* saturation multiplier */
    /* Mean cloud cover over the deck around the camera, one number a frame.
     * The shaft gate in fly__scatter needs a long-path limit: over tens of
     * kilometres of air the sunlight reaching the beam is the deck's *average*
     * transmission, not one tap of it. See the note there. */
    float cloud_mean;
    /* The sky's own irradiance, as second-order spherical harmonics.
     *
     * Everything a surface is not lit by directly, it is lit by the sky, and
     * until now that was a hand-tuned zenith/horizon gradient with the wrong
     * shape: a lerp on n.z alone, which gives a wall facing the sunrise exactly
     * the same shade as the wall behind it. The sky is not like that — half of
     * it is bright and half is not, and which half a surface faces is most of
     * what its shade looks like.
     *
     * So the ambient is the real integral now, projected off the same
     * scattering model the sky is painted with and reconstructed per vertex.
     *
     * Second order, nine coefficients, not four. First order was tried and
     * measured against the exact hemisphere integral, and it rings: nine per
     * cent too bright at the zenith, forty too dark facing away from the sun,
     * and — because each channel gets its own linear term and blue's is
     * proportionally the largest — a shaded wall came back at a blue/red of
     * 2.57 where the truth is 1.89. Too blue and too dark is the wrong pair of
     * errors to have on the shaded side of everything. The quadratic band costs
     * five more coefficients and about fifteen more instructions, and brings
     * the worst of that inside a few per cent. */
    fly_v3 sh[9];
    /* What the ground under the camera sends back up: its own albedo times the
     * light falling on it. The lower half of every surface's hemisphere is
     * filled by this and it used to be the constant (0.10, 0.12, 0.07) — a
     * green, which is right over a meadow and wrong over sand, snow, water and
     * a city. One sample a frame, the same way the biome grade is taken. */
    fly_v3 ground_lit;
    /* What a cloud is lit by, once a frame.
     *
     * These were a hardcoded pair — (0.98,0.96,0.95) sunlit, (0.38,0.39,0.44)
     * shaded, scaled by the time of day — from before the atmosphere was a
     * model. When the scattering re-tune moved the sky's radiance, everything
     * keyed to the model moved with it and the deck did not, so a sunlit cloud
     * ended up dimmer than the horizon behind it and the sky read as empty.
     * Same bug the ambient term had: a constant standing in for an integral.
     *
     * A cloud is a thick, high-albedo scattering medium, so to a good
     * approximation its top returns its own albedo times the irradiance on it —
     * the sun through the air above the deck, plus the sky it can see, which at
     * 2.6 km is most of a hemisphere. The base gets what leaks through and the
     * sky at grazing angles, which is a fraction of the same. */
    fly_v3 cloud_lit;
    fly_v3 cloud_dark;
    fly_v3 cirrus_lit;   /* the ice sheet, which has no dark side — see cirrus_sheet */
    fly_v3 cirrus_dir;   /* unit xy the sheet's bands are drawn out along */
    float cirrus_mean;
    /* The sunbeam, per channel, measured across the beam rather than on the
     * ground — the cosine belongs to the surface and is applied there.
     *
     * This replaces `clamp(sun.z*1.7, 0, 1.25)` times a hand-lerp from
     * (1,0.97,0.9) to (1,0.55,0.3). Both halves were wrong and the first one
     * badly: `sun.z` *is* a cosine, and every place that used it also
     * multiplied by `dot(n, sun)`, so the sun's obliquity was counted twice.
     * A wall held square to a low sun lost its light along with the ground it
     * stood on, which is why dawn had no terminator — the whole frame went to
     * ambient together instead of splitting into a lit side and a dark one.
     *
     * What survives along the beam is something the scattering model already
     * computes, and it does not agree with the hand-lerp either: at fourteen
     * degrees of elevation the transmittance is (0.61, 0.33, 0.07), a red over
     * blue of 8.5, where the lerp tops out at 3.3. */
    fly_v3 sunlight;
    float sun_i;        /* its luminance, where a scalar is wanted */
    /* What this frame's graphics level buys, resolved once — see lod_for.
     * make_env cannot see the options, so it fills in the middle rung and
     * fly_render_frame overwrites it; a probe that never sets it therefore
     * measures the default world rather than an empty one. */
    fly__lod lod;
} fly__env;

/* regional grass species field: the grade reads it before it is defined */
static float grass_region(uint32_t seed, float x, float y);
/* traced sky visibility on the world lattice; the pixel loop reads it */
static float ao_at(float x, float y, float agl, float dist);
/* The plain heightfield under a scattered thing, memoised. Declared here
 * because everything that stands on the ground has to know how far off it, to
 * read the occlusion window at its own height. */
static float scatter_ground(const fly_world *w, float x, float y);

/* How far a primitive's centre stands above the ground, never negative: the
 * argument `ao_at` blends its two channels with. Defined with the occlusion
 * window, whose lattice it reads. */
static float tri_agl(const fly__env *e, fly_v3 mid);
/* the scattering integral: make_env lights the cloud deck with it */
static void fly__scatter(const fly__env *e, fly_v3 ro, fly_v3 rd, float t,
                         fly_v3 *transmit, fly_v3 *inscat);
/* likewise the cloud deck, which make_env averages */
static float cloud_cover(const fly__env *e, float wx, float wy);
/* and the ice sheet above it, which make_env averages the same way */
static float cirrus_cover(const fly__env *e, float wx, float wy);
/* and how much of the beam it leaves, which is what lights the deck under it */
static float cirrus_shadow(const fly__env *e, fly_v3 p);
/* and the sky itself, which make_env projects into an irradiance basis */
static fly_v3 sky_sample(const fly__env *e, fly_v3 ro, fly_v3 rd, unsigned flags);
/* where a ray meets the ball, or < 0 if it is above the horizon */
static float fly__planet_hit(fly_v3 ro, fly_v3 rd, fly_v3 *n_out);
/* the sky's irradiance on a surface facing n, from the basis make_env fits */
static fly_v3 sky_irradiance(const fly__env *e, fly_v3 n);
/* and the ground, whose albedo make_env reads for the bounce */
static fly_v3 terrain_surface(const fly_world *w, float x, float y, float h, float slope,
                              float px_per_m, float ball, float rain, fly_v3 *mat_out);
/* how far the flat chart has given way to the ball, from the camera's altitude */
static float chart_gives_way(float altitude);
#define FLY_SKY_NOSUN 2u
/* Scale on the sky's irradiance: the one free constant, set so a noon
 * horizontal surface lands where the hand-tuned gradient used to put it. */
#define FLY_SKY_IRR 1.0f
/* Form factor for the ground bounce; see make_env. */
#define FLY_BOUNCE 0.30f

static fly__env make_env(const fly_game *g, const fly__view *v) {
    fly__env e;
    float t = (float)(fmod(g->time_s, 86400.0) / 86400.0);
    float ang = (t - 0.25f) * 2.0f * FLY_PI;
    float el = sinf(ang);
    e.sun = fly_v3norm(fly_v3mk(cosf(ang), 0.35f, el));
    e.day = fly_clampf(el * 1.6f + 0.12f, 0.02f, 1.0f);
    e.dusk = expf(-fabsf(el) * 9.0f);
    e.storm = g->weather.turbulence > 0.35f ? fly_clampf((g->weather.turbulence - 0.35f) * 1.6f, 0, 1) : 0.0f;
    /* Saturating, and it starts before the streaks are legible: the ground is
     * visibly damp long before rain is heavy enough to see falling, and a
     * surface that only turns wet in a downpour reads as a switch. */
    e.wet = fly_clampf(g->weather.precip * 2.4f, 0.0f, 1.0f);
    e.time = g->time_s;
    e.seed = g->seed;
    e.world = &g->world;
    e.wind = g->weather.wind;
    e.shadow = NULL;
    e.lod = lod_for(FLY_QUALITY_MEDIUM);
    /* With no view — the probes, which ask the model questions rather than
     * render — stand in a nominal 720p 60-degree camera. One pixel per world
     * unit at a metre is not a neutral default but a pinhole: everything keyed
     * to the pixel footprint (terrain octaves, the cloud march) would see a
     * frame so coarse that it drops all of its detail, and a probe would then
     * be measuring a resolution nothing renders at. */
    e.pxscale = v ? v->sy / (float)(v->aa > 0 ? v->aa : 1)
                  : (720.0f * 0.5f) / 0.5773503f;
    /* how far the flat chart has given way to the ball under it — see
     * chart_gives_way; a probe with no camera is asking about the ground */
    e.ball = v ? chart_gives_way(v->pos.z) : 0.0f;
    /* The biome under the camera, not under each pixel: one lookup a frame,
     * and a flight from moor to shore crosses the blend rather than snapping,
     * because the fields it reads are continuous. */
    {
        fly_v3 gr = fly_v3mk(1.0f, 1.0f, 1.0f);
        float sat = 1.0f, gz, region, wood, snow, shore;
        fly_v3 at = v ? v->pos : fly_v3zero();
        gz = fly_world_ground(&g->world, at.x, at.y);
        region = grass_region(g->world.seed, at.x, at.y);
        wood = fly_world_forest_mask(&g->world, at.x, at.y);
        snow = fly_smoothstepf(1500.0f, 2200.0f, gz);
        shore = 1.0f - fly_smoothstepf(FLY_WATER_LEVEL + 2.0f, FLY_WATER_LEVEL + 70.0f, gz);
        /* dry country runs warm and flat; moorland cool and muted; a wood
         * pulls green and gains a little contrast; snow goes blue and clean;
         * the coast takes the cyan bounce off the water */
        gr = fly_v3lerp(gr, fly_v3mk(1.055f, 1.012f, 0.945f),
                        fly_clampf(region * 2.2f, 0.0f, 1.0f));
        sat *= 1.0f - 0.06f * fly_clampf(region * 2.2f, 0.0f, 1.0f);
        gr = fly_v3lerp(gr, fly_v3mk(0.968f, 0.985f, 1.045f),
                        fly_clampf(-region * 2.0f, 0.0f, 1.0f));
        sat *= 1.0f - 0.07f * fly_clampf(-region * 2.0f, 0.0f, 1.0f);
        gr = fly_v3lerp(gr, fly_v3mk(0.975f, 1.030f, 0.968f), wood * 0.8f);
        sat *= 1.0f + 0.05f * wood;
        gr = fly_v3lerp(gr, fly_v3mk(0.972f, 0.992f, 1.060f), snow);
        gr = fly_v3lerp(gr, fly_v3mk(0.980f, 1.008f, 1.035f), shore * 0.55f);
        /* the one hard promise: a grade cannot become a colour wash */
        e.grade = fly_v3mk(fly_clampf(gr.x, 0.88f, 1.12f), fly_clampf(gr.y, 0.88f, 1.12f),
                           fly_clampf(gr.z, 0.88f, 1.12f));
        e.sat = fly_clampf(sat, 0.90f, 1.10f);
    }

    /* The deck's mean cover over the forty kilometres around the camera: a 9x9
     * lattice at five-kilometre spacing, which is the base wavelength of the
     * cloud field, so the taps are near enough independent for the mean to be
     * steady as the deck drifts. Eighty-one fbm evaluations a frame buys the
     * long-path limit of the shaft gate, and there is nowhere cheaper to put
     * it: fly__scatter runs per pixel. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        float acc = 0.0f;
        int i, j;
        e.cloud_mean = 0.0f; /* cloud_cover does not read it */
        for (j = -4; j <= 4; ++j)
            for (i = -4; i <= 4; ++i)
                acc += cloud_cover(&e, at.x + (float)i * 5000.0f, at.y + (float)j * 5000.0f);
        e.cloud_mean = acc * (1.0f / 81.0f);
    }
    /* Which way the ice runs: one bearing per world, off the seed.
     *
     * The obvious source is `e.wind`, since shear is what draws a cirrus band
     * out in the first place. It is the wrong one. The wind here is the wind
     * at the aeroplane and it turns — with the weather, with a gust, with a
     * storm cell going past — and using it as the frame the field is read in
     * does not advect the sheet, it *rotates* the whole sky with it. It is
     * also flatly zero in calm air, which would lay every band in every calm
     * world along the world's own x axis: a grid, and one a player would see.
     *
     * So the bands lie along a prevailing bearing the world is born with, and
     * the weather moves them along it rather than turning them. Settled once a
     * frame, which also keeps the square root off the per-tap path. */
    {
        float th = (float)(e.seed & 1023u) * (6.2831853f / 1024.0f);
        e.cirrus_dir = fly_v3mk(cosf(th), sinf(th), 0.0f);
    }
    /* The ice sheet's mean, on its own lattice. The same 9x9 the deck gets,
     * but at twelve kilometres rather than five, because the field it is
     * averaging is nine kilometres across the wind and thirty-four along it —
     * a five-kilometre lattice would put every tap inside the same band and
     * report that band's cover as the sky's. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        float acc = 0.0f;
        int i, j;
        e.cirrus_mean = 0.0f; /* cirrus_cover does not read it */
        for (j = -4; j <= 4; ++j)
            for (i = -4; i <= 4; ++i)
                acc += cirrus_cover(&e, at.x + (float)i * 12000.0f,
                                    at.y + (float)j * 12000.0f);
        e.cirrus_mean = acc * (1.0f / 81.0f);
    }

    /* What the sunbeam carries. One transmittance along the sun's own path,
     * measured once a frame. `FLY_SUN_E0` is the scale, and the only free
     * number here: it is set so a noon horizontal surface lands exactly where
     * the old constants put it, so this changes the shape of the day without
     * moving its middle. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        fly_v3 ro = fly_v3mk(at.x, at.y, v ? fly_clampf(at.z, 0.0f, 8000.0f) : 200.0f);
        if (e.sun.z < 0.005f) {
            e.sunlight = fly_v3zero();
        } else {
            fly_v3 T;
            fly__scatter(&e, ro, e.sun, 320000.0f, &T, NULL);
            e.sunlight = fly_v3scale(T, FLY_SUN_E0 * (1.0f - e.storm * 0.6f));
        }
        e.sun_i = 0.2126f * e.sunlight.x + 0.7152f * e.sunlight.y + 0.0722f * e.sunlight.z;
    }

    /* What the deck is lit by. Computed off `fly__scatter`, which knows nothing
     * about clouds, so this can be settled before the spherical-harmonic pass
     * below — that pass samples the sky *through* the deck and would otherwise
     * need these to already exist.
     *
     * The ground bounce is left out. A cloud base over snow really is lit from
     * below, but `ground_lit` is not known until after the harmonics and the
     * term is small against the sky at 2.6 km; saying so is cheaper than
     * reordering make_env around it. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        fly_v3 top = fly_v3mk(at.x, at.y, FLY_CLOUD_Z + FLY_CLOUD_LIT_TOP);
        fly_v3 skyup = fly_v3zero(), sun_on_top;
        fly__scatter(&e, top, fly_v3mk(0.0f, 0.0f, 1.0f), 200000.0f, NULL, &skyup);
        /* radiance straight up to irradiance over the hemisphere the top sees:
           the sky is brightest at its horizon, so this is above one */
        skyup = fly_v3scale(skyup, FLY_CLOUD_SKY);
        /* The beam at the deck's own altitude, not the one at the ground:
         * a cloud top at 2.9 km has most of the air below it and sees a
         * markedly stronger, less reddened sun. Reusing `e.sunlight` here made
         * the deck about half as bright as it should be, which the cloud
         * contrast check caught. A cloud top is horizontal, so it does take
         * the cosine. */
        fly_v3 tsun = fly_v3zero();
        if (e.sun.z >= 0.005f) {
            fly_v3 Tc;
            fly__scatter(&e, top, e.sun, 320000.0f, &Tc, NULL);
            tsun = fly_v3scale(Tc, FLY_SUN_E0 * (1.0f - e.storm * 0.6f));
        }
        sun_on_top = fly_v3scale(tsun, fly_clampf(e.sun.z, 0.0f, 1.0f));
        /* And the beam has crossed the ice on the way down. `cirrus_shadow`
         * is the same lookup the ground gets, taken at the deck's own lit top,
         * so a sheet over the weather dims the cloud under it as well as the
         * landscape under that — which is what a cirrus veil does and what
         * nothing here used to do.
         *
         * One number a frame, at the camera's own column, which is not a
         * shortcut for this term: `cloud_lit` and `cloud_dark` are already one
         * pair of colours a frame taken at exactly this point, so putting the
         * sheet here holds it to the same fidelity as everything else the deck
         * is lit by. Per tap in the march is the honest version and it was
         * built and measured: five field lookups a tap instead of four, +28%
         * on the worst sky frame in the game, interleaved and minimum of five
         * — against the twenty per cent the whole ice layer cost to add. What
         * it buys over this is shadow *bands* laid across the cloud tops; see
         * TODO.
         *
         * It goes on the beam and not on the moon beside it. The moon is a
         * stand-in for one light in an empty sky and the ice's share of it is
         * under a hundredth of the deck's night colour. */
        sun_on_top = fly_v3scale(sun_on_top, cirrus_shadow(&e, top));
        /* Moonlight joins the beam rather than the answer: it is light falling
         * on the top, so it is reflected by the same albedo and reaches the
         * underside through the same transmission the sun does. See
         * FLY_MOON for why the old additive floor was wrong. */
        sun_on_top = fly_v3add(sun_on_top,
                               fly_v3scale(FLY_MOON, 1.0f - e.day));
        e.cloud_lit = fly_v3scale(fly_v3add(sun_on_top, skyup), FLY_CLOUD_ALB);
        e.cloud_dark = fly_v3scale(fly_v3add(fly_v3scale(sun_on_top, FLY_CLOUD_THRU), skyup),
                                   FLY_CLOUD_ALB * 0.5f);
        e.cloud_dark = fly_v3lerp(e.cloud_dark,
                                  fly_v3scale(e.cloud_dark, 0.72f), e.storm * 0.8f);
    }

    /* What the ice is lit by, seven kilometres up. The same construction as
     * the deck's top and for the same reason — the sun at that altitude, not
     * the one at the ground — with two differences that are the difference
     * between ice and water cloud: one colour rather than a pair, because a
     * sheet that thin has no shaded underside, and a much lower albedo,
     * because it is thin. Over a lattice of columns at noon that puts the ice
     * at 0.71 against the deck's 0.83 — a veil the sky comes through rather
     * than a second deck.
     *
     * Above the storm, deliberately: cirrus sits over the weather rather than
     * in it, so it does not take the storm's attenuation on its own beam. What
     * a storm does to it is hide it, which the deck below already does. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        fly_v3 ice = fly_v3mk(at.x, at.y, FLY_CIRRUS_Z);
        fly_v3 skyup = fly_v3zero(), on_top = fly_v3zero();
        fly__scatter(&e, ice, fly_v3mk(0.0f, 0.0f, 1.0f), 200000.0f, NULL, &skyup);
        skyup = fly_v3scale(skyup, FLY_CLOUD_SKY);
        if (e.sun.z >= 0.005f) {
            fly_v3 Ti;
            fly__scatter(&e, ice, e.sun, 320000.0f, &Ti, NULL);
            on_top = fly_v3scale(Ti, FLY_SUN_E0 * fly_clampf(e.sun.z, 0.0f, 1.0f));
        }
        on_top = fly_v3add(on_top, fly_v3scale(FLY_MOON, 1.0f - e.day));
        e.cirrus_lit = fly_v3scale(fly_v3add(on_top, skyup), FLY_CIRRUS_ALB);
    }

    /* Project the sky's radiance onto second-order spherical harmonics.
     *
     * Sixty-four directions on a Fibonacci hemisphere — uniform in solid angle,
     * and not a lattice, so nothing beats against the cloud field — with the
     * sky sampled minus its sun, because the disc is a directional light and
     * would otherwise be counted twice. L_lm = sum L(w) Y_lm(w) * 2pi/N. The
     * cosine convolution and the 1/pi of the diffuse BRDF are folded into the
     * reconstruction in sky_irradiance, which is where they belong.
     *
     * FLY_SKY_IRR is the one free constant here, and it is the same kind as
     * FLY_SCAT_SUN: the model gives the sky's shape and ratios, this sets where
     * a noon horizontal surface lands. It is 1.0, because the integral landed
     * within six per cent of the old hand-tuned value on its own — those
     * constants were a fair fit to noon and an invention everywhere else. */
    {
        fly_v3 ro = fly_v3mk(v ? v->pos.x : 0.0f, v ? v->pos.y : 0.0f,
                             v ? fly_clampf(v->pos.z, 0.0f, 8000.0f) : 200.0f);
        const int N = 64;
        int i, k;
        for (k = 0; k < 9; ++k) e.sh[k] = fly_v3zero();
        for (i = 0; i < N; ++i) {
            float z = ((float)i + 0.5f) / (float)N;
            float r = sqrtf(1.0f - z * z);
            float ph = (float)i * 2.39996323f;
            float x = r * cosf(ph), y = r * sinf(ph);
            fly_v3 L = sky_sample(&e, ro, fly_v3mk(x, y, z), FLY_SKY_NOSUN);
            float Y[9];
            Y[0] = 0.282095f;
            Y[1] = 0.488603f * y;
            Y[2] = 0.488603f * z;
            Y[3] = 0.488603f * x;
            Y[4] = 1.092548f * x * y;
            Y[5] = 1.092548f * y * z;
            Y[6] = 0.315392f * (3.0f * z * z - 1.0f);
            Y[7] = 1.092548f * x * z;
            Y[8] = 0.546274f * (x * x - y * y);
            for (k = 0; k < 9; ++k) e.sh[k] = fly_v3add(e.sh[k], fly_v3scale(L, Y[k]));
        }
        {
            float w = 2.0f * FLY_PI / (float)N * FLY_SKY_IRR;
            for (k = 0; k < 9; ++k) e.sh[k] = fly_v3scale(e.sh[k], w);
        }
    }

    /* What the ground sends back up, one sample a frame under the camera.
     *
     * The lower half of every surface's hemisphere is filled by the ground, and
     * it used to be filled by the literal (0.10, 0.12, 0.07): a green, which is
     * right over a meadow, wrong over sand, badly wrong over snow and water,
     * and does not go out at dusk. This is the same Lambertian term the ground
     * itself is shaded with — its albedo times the sky above it plus the sun on
     * it — so an aircraft over a snowfield is lit from below by snow and one
     * over an estuary by water, on the same schedule the ground is.
     *
     * FLY_BOUNCE is the form factor: what fraction of a downward-facing
     * surface's hemisphere the ground actually fills at aircraft heights, times
     * the second bounce being weaker than an infinite lit plane. Set at 0.30 so
     * a face pointing straight down over noon grass lands where the old
     * constant put it, which is the only place the old constant was right. */
    {
        fly_v3 at = v ? v->pos : fly_v3zero();
        float gz = fly_world_ground(&g->world, at.x, at.y);
        fly_v3 mat, alb;
        if (gz < FLY_WATER_Z) {
            /* Water is not ground with a colour: it is dark from above and the
             * sky's own image. Its albedo for a bounce is low and cool. */
            alb = fly_v3mk(0.045f, 0.075f, 0.105f);
        } else {
            float e4 = 4.0f;
            float hx0 = fly_world_ground(&g->world, at.x - e4, at.y);
            float hx1 = fly_world_ground(&g->world, at.x + e4, at.y);
            float hy0 = fly_world_ground(&g->world, at.x, at.y - e4);
            float hy1 = fly_world_ground(&g->world, at.x, at.y + e4);
            fly_v3 gn = fly_v3norm(fly_v3mk(-(hx1 - hx0) / (2 * e4), -(hy1 - hy0) / (2 * e4), 1.0f));
            alb = terrain_surface(&g->world, at.x, at.y, gz, 1.0f - gn.z, 0.05f, e.ball,
                                  e.wet, &mat);
        }
        {

            fly_v3 onto = fly_v3add(sky_irradiance(&e, fly_v3mk(0, 0, 1)),
                                    fly_v3scale(e.sunlight, fly_clampf(e.sun.z, 0.0f, 1.0f)));
            e.ground_lit = fly_v3scale(fly_v3mul(alb, onto), FLY_BOUNCE);
        }
    }

    return e;
}

/* The sky's irradiance arriving on a surface facing `n`, per unit albedo.
 *
 * Ramamoorthi and Hanrahan's reconstruction: the clamped-cosine kernel is
 * almost entirely contained in the first three bands, so convolving with it is
 * three scalars — one per band — applied to coefficients that were projected
 * without it. The 1/pi of the diffuse BRDF is folded in with them, because
 * every call site multiplies by albedo alone.
 *
 * Clamped at zero. Even at second order a truncated fit can dip slightly
 * negative under a very directional sky, and negative light is not a thing. */
static fly_v3 sky_irradiance(const fly__env *e, fly_v3 n) {
    /* The clamped-cosine kernel is A0 = pi, A1 = 2pi/3, A2 = pi/4, and the
     * 1/pi of the diffuse BRDF divides straight through them. The Y_lm factors
     * are separate and belong to the direction, not to the band — folding them
     * into these by mistake is a flat 65% too dark, which is how this was
     * caught. */
    const float a0 = 1.0f;
    const float a1 = 2.0f / 3.0f;
    const float a2 = 0.25f;
    float x = n.x, y = n.y, z = n.z;
    fly_v3 c = fly_v3scale(e->sh[0], a0 * 0.282095f);
    c = fly_v3add(c, fly_v3scale(e->sh[1], a1 * 0.488603f * y));
    c = fly_v3add(c, fly_v3scale(e->sh[2], a1 * 0.488603f * z));
    c = fly_v3add(c, fly_v3scale(e->sh[3], a1 * 0.488603f * x));
    c = fly_v3add(c, fly_v3scale(e->sh[4], a2 * 1.092548f * x * y));
    c = fly_v3add(c, fly_v3scale(e->sh[5], a2 * 1.092548f * y * z));
    c = fly_v3add(c, fly_v3scale(e->sh[6], a2 * 0.315392f * (3.0f * z * z - 1.0f)));
    c = fly_v3add(c, fly_v3scale(e->sh[7], a2 * 1.092548f * x * z));
    c = fly_v3add(c, fly_v3scale(e->sh[8], a2 * 0.546274f * (x * x - y * y)));
    return fly_v3mk(c.x > 0.0f ? c.x : 0.0f, c.y > 0.0f ? c.y : 0.0f,
                    c.z > 0.0f ? c.z : 0.0f);
}

static fly_v3 v3sat(fly_v3 c) {
    return fly_v3mk(fly_clampf(c.x, 0, 1), fly_clampf(c.y, 0, 1), fly_clampf(c.z, 0, 1));
}

static uint32_t v3_to_rgb(fly_v3 c) {
    c = v3sat(c);
    float r = sqrtf(c.x), g = sqrtf(c.y), b = sqrtf(c.z);
    return FLY_RGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static float v3_lum(fly_v3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/* interpret a display color as linear radiance (for legacy uint32 call sites) */
static fly_v3 lin_from_u32(uint32_t c) {
    float r = (float)(c & 0xFF) / 255.0f, g = (float)((c >> 8) & 0xFF) / 255.0f,
          b = (float)((c >> 16) & 0xFF) / 255.0f;
    return fly_v3mk(r * r, g * g, b * b);
}

/* The biome grade, applied to linear radiance immediately before the tonemap.
 * Gain first, then saturation about the luminance the gain produced, so the two
 * do not fight: gain alone shifts the white point and saturation alone leaves a
 * dry landscape as green as a wet one. Twin in FLY_POST_FS. */
static fly_v3 grade_apply(fly_v3 c, fly_v3 gain, float sat) {
    float l;
    c = fly_v3mul(c, gain);
    l = v3_lum(c);
    return fly_v3mk(l + (c.x - l) * sat, l + (c.y - l) * sat, l + (c.z - l) * sat);
}

/* Narkowicz ACES filmic approximation */
static float aces1(float x) {
    return fly_clampf((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

/* Four octaves of a cloud field, blending toward the two-octave answer as
 * `coarse` comes on.
 *
 * Both answers come out of one walk of the octaves. `fly_fbm2` sums them in
 * order and divides by the running normalizer, so the two-octave value is the
 * four-octave one caught halfway through: same taps, same order, same
 * arithmetic, bit for bit. Asking the shared function twice recomputed the
 * first two octaves to get the second answer, and on a grazing ray — which is
 * where `coarse` is nonzero and where the march now takes three density
 * samples a tap — that was half of what the deck cost: 223 ms of a
 * horizon-filling 480x270 raster frame against 195. Written out here rather
 * than as a second entry point on `fly_fbm2` because the noise is a shared
 * primitive with a GLSL twin and a parity gate; this is a caller's arithmetic,
 * not a new kind of noise. */
static float cloud_fbm_lod(uint32_t seed, float u, float v, float coarse) {
    float n0, n1, n2, n3, a2, a4;
    if (coarse <= 0.01f) return fly_fbm2(seed, u, v, 4);
    n0 = fly_noise2(seed, u, v);
    n1 = fly_noise2(seed + 0x9E37u, u * 2.0f, v * 2.0f);
    n2 = fly_noise2(seed + 0x9E37u * 2u, u * 4.0f, v * 4.0f);
    n3 = fly_noise2(seed + 0x9E37u * 3u, u * 8.0f, v * 8.0f);
    a2 = 0.5f * n0 + 0.25f * n1;
    a4 = a2 + 0.125f * n2 + 0.0625f * n3;
    return fly_lerpf(a4 / 0.9375f, a2 / 0.75f, coarse);
}

/* Cloud coverage 0..1 at a world xy point on the deck, as much of it as a
 * pixel at this range can carry.
 *
 * `coarse` drops the field from four octaves to two and `flat` takes what is
 * left to the deck's mean. Two knobs and not one because the first attempt had
 * only the second, and it emptied the sky: a grazing ray's tap slides hundreds
 * of metres per pixel, which does outrun the 650 m octave and alias — but it
 * does not outrun the 5.2 km one, and collapsing straight to a single number
 * threw away every cloud mass on the horizon along with the aliasing. The
 * coarse field is the same walk of the octaves stopped halfway (see
 * `cloud_fbm_lod`); the mean is left for the last degree above level, where
 * even 5 km features slide faster than a pixel. */
static float cloud_cover_lod(const fly__env *e, float wx, float wy,
                             float coarse, float flat) {
    float t = (float)(e->time * 0.004);
    float u = wx / 5200.0f + e->wind.x * t * 0.02f + t * 0.4f;
    float v = wy / 5200.0f + e->wind.y * t * 0.02f;
    float lo = 0.62f - e->storm * 0.34f;
    float d = cloud_fbm_lod(e->seed + 41, u, v, coarse) * 0.5f + 0.5f;
    float c = fly_smoothstepf(lo, lo + 0.24f, d);
    return flat > 0.001f ? fly_lerpf(c, e->cloud_mean, flat) : c;
}

/* The ice sheet's coverage, on the same two ramps as the deck's.
 *
 * Drawn out into bands, because that is what makes a cirrus sky look like
 * cirrus rather than like a second deck: the structure across a band is four
 * times finer than the structure along it. The frame the field is read in is
 * the world's own prevailing bearing (`cirrus_dir`, settled once a frame) and
 * the weather moves the bands along it rather than turning them — see make_env
 * for why the wind at the aeroplane is the wrong thing to turn a whole sky by.
 *
 * Its threshold is higher than the deck's. Ice at this height is genuinely a
 * sometimes thing, and a sheet that is always there is a lid — the whole point
 * of the layer is the fair day, where the deck is under its own threshold and
 * the sky above is otherwise empty. */
static float cirrus_cover_lod(const fly__env *e, float wx, float wy,
                              float coarse, float flat) {
    float t = (float)(e->time * 0.004);
    float ax = e->cirrus_dir.x, ay = e->cirrus_dir.y;
    float along = (wx * ax + wy * ay) / FLY_CIRRUS_LEN + t * 0.13f;
    float across = (wy * ax - wx * ay) / FLY_CIRRUS_WID;
    float d = cloud_fbm_lod(e->seed + 63, along, across, coarse) * 0.5f + 0.5f;
    float c = fly_smoothstepf(0.60f, 0.86f, d);
    return flat > 0.001f ? fly_lerpf(c, e->cirrus_mean, flat) : c;
}

static float cirrus_cover(const fly__env *e, float wx, float wy) {
    return cirrus_cover_lod(e, wx, wy, 0.0f, 0.0f);
}

static float cloud_cover(const fly__env *e, float wx, float wy) {
    return cloud_cover_lod(e, wx, wy, 0.0f, 0.0f);
}

/* Sun visibility through both cloud layers from a point under them.
 *
 * One plane crossing per layer — where the sun ray from `p` leaves the deck,
 * and where it leaves the ice four kilometres higher — and one field lookup at
 * each. The sheet is a plane, so its shadow has exactly the shape the deck's
 * always had; what it is not is a second factor. See FLY_CIRRUS_SHADE for why
 * the two losses add over the ground the deck left clear rather than
 * multiplying, and for where the ice's dimming of the deck itself lives. */
static float cloud_shadow(const fly__env *e, fly_v3 p) {
    float cc = 0.0f, lost;
    if (e->sun.z <= 0.02f) return 1.0f;
    if (p.z < FLY_CLOUD_Z) {
        float t = (FLY_CLOUD_Z - p.z) / e->sun.z;
        cc = cloud_cover(e, p.x + e->sun.x * t, p.y + e->sun.y * t);
    }
    lost = cc * FLY_CLOUD_SHADE;
    if (p.z < FLY_CIRRUS_Z) {
        float t = (FLY_CIRRUS_Z - p.z) / e->sun.z;
        float ci = cirrus_cover(e, p.x + e->sun.x * t, p.y + e->sun.y * t);
        lost += ci * FLY_CIRRUS_SHADE * (1.0f - cc);
    }
    return 1.0f - lost;
}

/* The ice's share of the same thing, on its own: how much of the beam reaches
 * a point that the deck cannot be asked about because it is inside it. What
 * lights the deck is the one caller — see make_env. */
static float cirrus_shadow(const fly__env *e, fly_v3 p) {
    float t;
    if (e->sun.z <= 0.02f || p.z >= FLY_CIRRUS_Z) return 1.0f;
    t = (FLY_CIRRUS_Z - p.z) / e->sun.z;
    return 1.0f - cirrus_cover(e, p.x + e->sun.x * t, p.y + e->sun.y * t) *
                      FLY_CIRRUS_SHADE;
}

/* Full sky radiance along rd from ro: gradient, sun, clouds, and — only when
 * `stars` is set — the star field.
 *
 * Stars are a point sample of a hash, which is the right thing for a ray that
 * is actually looking at the sky and the wrong thing everywhere else the sky
 * gets sampled. Fog mixes distant terrain toward the sky colour in the
 * direction it lies, so with stars in that sample a night scene came out with
 * the whole landscape speckled white — the star field showing through the
 * mountains rather than behind them, which is what it looks like when snow
 * turns into sky. Water reflections have the same problem for the same reason.
 * A fog or ambient colour wants the sky's *average* in a direction, and the
 * average of a star field is the gradient underneath it. */
/* --- the atmosphere ------------------------------------------------------
 *
 * One single-scattering model, and everything the air does comes out of it:
 * the colour of the sky, the colour of the sun, and what distance does to a
 * hillside. Those used to be three separately tuned gradients — a zenith/rim
 * lerp, a hand-picked disc tint, and an exponential fog whose colour was a
 * second sky lookup — which meant they agreed only where someone had matched
 * the constants by eye, and disagreed everywhere else. A sunset reddened the
 * rim on a schedule rather than because the light had travelled through more
 * air, and the fog was grey while the sky it faded into was blue.
 *
 * Rayleigh scattering off air molecules is strongly wavelength-dependent —
 * blue is scattered six times as hard as red — which is why the sky is blue
 * overhead and why the sun goes red when its light has to cross a long, dense
 * path to reach you. Mie scattering off aerosols is nearly grey and sharply
 * forward-biased, which is the white glare around the sun and the haze that
 * thickens in a storm. Both are exponential in altitude with their own scale
 * height, so both integrate in closed form along a straight ray.
 *
 * The one liberty taken with the physics is FLY_SCAT_SPREAD, and where it is
 * applied matters more than its value.
 *
 * This world is twenty kilometres across, so at real sea-level densities a
 * ridge three kilometres off is barely touched and there is no aerial
 * perspective to speak of. It used to buy some by multiplying the whole
 * atmosphere by three — both species, both scale heights, every direction.
 * That thickened the *vertical* column as well, which is a different claim
 * entirely, and it showed: sunlight arrived at noon with a red/blue of 1.94
 * where Earth's is about 1.22, and the zenith came back at a blue/red of 2.31
 * where a clear sky is nearer four. Permanent golden hour, and a sky with the
 * blue half saturated out of it.
 *
 * The compression is horizontal, so it is applied horizontally. The world is a
 * scale model across and full size upward — the air is eight kilometres deep
 * and the map is twenty wide — so a path's optical depth is scaled by how much
 * of it is horizontal: three for a level ray, one for a vertical one, and the
 * cosine in between. Ratios, phase functions and the coupling between
 * extinction and in-scattering are all untouched. Haze over a valley is what it
 * was; noon overhead is now what noon is.
 *
 * The factor three turns out not to be arbitrary either, and that only became
 * visible once the column was integrated on the ball. A grazing ray at sea
 * level crosses sqrt(2*pi*H*R) of air: 173.7 km on a six-hundred-kilometre
 * planet against Earth's 566 km, short by 3.26 because the ground curves away
 * that much faster. So the horizontal compression is very nearly exactly what
 * it takes to make a small planet's horizon as thick as a real one's, while the
 * vertical column — which does not depend on R at all — stays untouched at H.
 * That is the whole liberty, stated twice: once as a scale model, once as a
 * ratio of two square roots. */
#define FLY_SCAT_HR 8000.0f   /* Rayleigh scale height, m */
#define FLY_SCAT_HM 1200.0f   /* Mie (aerosol) scale height, m */
#define FLY_SCAT_SPREAD 3.0f  /* optical depth per metre of *horizontal* path */
/* How far out a ray still meets air worth counting: a few scale heights of
 * slant range from anywhere in the world, and the reference length the sun
 * sample's midpoint is taken along. */
#define FLY_SCAT_FAR 320000.0f
/* How far a sky ray integrates. Not the same number, and the difference is the
 * atmospheric ring: from six hundred kilometres up, a ray a degree above the
 * limb does not reach its perigee for a thousand kilometres, so at 320 km it is
 * still 326 km above the surface and has crossed nothing. Three megametres
 * clears the far side of the ball from any altitude the game reaches, and past
 * the air the integrand is zero, so the extra length costs one closed form and
 * buys the only feature that lives out there. */
#define FLY_SKY_FAR 3000000.0f
/* Sun irradiance in the renderer's linear units. Not a physical figure: it
 * is the one free scale in the model, set so the noon zenith lands where the
 * hand-tuned gradient used to put it — (0.10, 0.24, 0.44) — because the
 * exposure, the cloud brightness and the water were all tuned against that. */
#define FLY_SCAT_SUN 8.4f
#define FLY_SCAT_G 0.76f      /* Mie asymmetry: forward-scattering glare */
/* how far under the horizon the sun's own column keeps lengthening; see fly__scatter */
#define FLY_SCAT_SUNFLOOR (-0.35f)
/* sky radiance at which a star is half its own brightness; see sky_sample */
#define FLY_STAR_SKY 0.012f
/* Multiple scattering, as one isotropic term proportional to how much of the
 * path has scattered at all. A single-scattering sky is too dark and the error
 * is not uniform: it is largest exactly where the optical depth is, which is
 * the horizon and twilight, so leaving it out does not dim the sky evenly, it
 * collapses dusk and drains the blue out of the band above the horizon. */
#define FLY_SCAT_MS 0.50f
/* How grey a storm makes a fully saturated path. See the storm term at the
 * end of fly__scatter — this used to be a lerp on the sky alone. */
#define FLY_SCAT_STORM 0.55f

/* exp(z^2)*erfc(z) for z >= 0, to a relative 1.1e-7.
 *
 * The scaled form rather than erfc itself, because the Chapman column below
 * wants exactly this product and both of its factors run out of float long
 * before the product does: at the elevations a steep ray sits at, z is in the
 * hundreds, exp(z^2) overflows and erfc(z) underflows, and their product is a
 * perfectly ordinary 0.56/z. Numerical Recipes' rational-exponential fit,
 * which is one exp and a degree-nine polynomial in 1/(1 + z/2) — cheap enough
 * to sit in a per-pixel integral, and short enough to have an exact GLSL twin.
 *
 * `erfcf` would do on this side and there is nothing to call on the other, and
 * a shader that disagreed with the C by a percent in the transition would show
 * as a seam between the two renderers rather than as an error in either. */
static float fly__erfcx(float z) {
    float t = 1.0f / (1.0f + 0.5f * z);
    return t * expf(-1.26551223f + t * (1.00002368f + t * (0.37409196f + t * (0.09678418f +
                t * (-0.18628806f + t * (0.27886807f + t * (-1.13520398f + t * (1.48851587f +
                t * (-0.82215223f + t * 0.17087277f)))))))));
}

/* Sea-level-equivalent metres of air from a point at radius `r` out to space,
 * along a ray leaving it at zenith cosine `c` >= 0. The Chapman column.
 *
 * Along the ray the radius is exactly sqrt(r^2 + 2 r c u + u^2). Expanded to
 * second order that is r + c*u + (1-c^2)*u^2/(2r), so the integrand is a
 * Gaussian times an exponential and the integral is an erfc — which is the
 * whole trick, because the u^2 term is the planet curving away underneath and
 * it is the term the flat model does not have.
 *
 * Both limits come out exactly right, which is what makes one formula enough.
 * Straight up (c = 1) it is H, the vertical column. Along the horizontal
 * (c = 0) it is H*sqrt(pi*r/(2H)) = sqrt(pi*H*r/2) — half of the sqrt(2*pi*H*R)
 * that a grazing ray at sea level crosses, 86.8 km of air against the vertical
 * column's 8, and the reason the ring around the planet is a bright feature.
 * In between the erfc's own asymptote hands back 1/c, the flat answer, as soon
 * as the ray is steep enough for the curvature not to matter. */
static float scat_chapman(float H, float r, float c) {
    float X, s, rho;
    /* There is no air below the surface. Clamping the radius rather than
     * letting exp(-(r-R)/H) run away there is what keeps this total: the most
     * air any ray can ever cross is the two grazing halves at r = R, which is
     * sqrt(2*pi*H*R), and no argument can talk it into more. */
    if (r < FLY_PLANET_R) r = FLY_PLANET_R;
    X = r / H;
    s = sqrtf(fly_clampf(1.0f - c * c, 0.0f, 1.0f));
    rho = expf(-(r - FLY_PLANET_R) / H);
    if (s < 1e-5f) s = 1e-5f;
    return H * rho * sqrtf(0.5f * FLY_PI * X) / s * fly__erfcx(c * sqrtf(0.5f * X) / s);
}

/* Sea-level-equivalent metres of air along ro + rd*t, on the ball.
 *
 * `z0` is the altitude at the ray's start and `dz` the ray's vertical
 * component; the chart is a tangent plane, so the planet's centre is r0 = R+z0
 * straight down and that is all the geometry this needs.
 *
 * This used to be the flat closed form, H/dz * (exp(-a/H) - exp(-b/H)), which
 * takes altitude to be linear in t. It is not, and the error is not a small
 * one at the angles that matter: a level ray at sea level climbs 333 m above
 * the surface over the first twenty kilometres and never comes back down, so
 * the flat model has it crossing 320 km of air out to the sky integral's end
 * where the ball says 86.8 km — 3.7 times too much, spent entirely on the
 * horizon and on the setting sun. Lengthening that integral to reach a grazing
 * ray's perigee from orbit, which is what the atmospheric ring needs, made the
 * linear model worse than useless: it keeps descending past the perigee, drives
 * the endpoint underground, and exp(-b/H) of a negative altitude saturates the
 * clamp into a black band.
 *
 * The segment is the difference of two columns-to-space, each of which is a
 * closed form. Which two depends on where the perigee is, and the case split is
 * not cosmetic: taking the tail off a descending ray would subtract a column
 * that continues underground, where the density runs away, and a difference of
 * two enormous numbers is not the small one wanted. Reversing the ray keeps
 * both terms finite and comparable; a segment that contains its own perigee is
 * two ascending halves back to back.
 *
 * Where the ray stops is still not decided here — fly__scatter caps the length
 * against the real horizon before calling. */
static float scat_thick(float H, float z0, float dz, float t) {
    /* No ceiling on the altitude. The flat model needed one — it read a craft
     * at six hundred kilometres as being at forty and painted a blue sky over
     * vacuum — and a ceiling here is worse than useless now: at 1800 km it puts
     * the observer 800 km closer to the ground than it is, which tips a ray
     * that clears the atmosphere into one whose perigee is buried, and the
     * density under the surface overflows the column to a grey band and then to
     * NaN. On the ball the far field needs no clamp: a ray that misses the air
     * has a perigee above it and integrates to nothing on its own. */
    float r0 = FLY_PLANET_R + (z0 > 0.0f ? z0 : 0.0f);
    float c0 = fly_clampf(dz, -1.0f, 1.0f);
    float r1, c1, v;
    if (t <= 0.0f) return 0.0f;
    r1 = sqrtf(r0 * r0 + 2.0f * r0 * c0 * t + t * t);
    c1 = (r0 * c0 + t) / r1;
    if (c0 >= 0.0f)                    /* climbing: perigee is behind the start */
        v = scat_chapman(H, r0, c0) - scat_chapman(H, r1, c1);
    else if (c1 <= 0.0f)               /* still descending: the same, ray reversed */
        v = scat_chapman(H, r1, -c1) - scat_chapman(H, r0, -c0);
    else {                             /* through the perigee: two halves */
        float rp = r0 * sqrtf(fly_clampf(1.0f - c0 * c0, 0.0f, 1.0f));
        v = 2.0f * scat_chapman(H, rp, 0.0f) - scat_chapman(H, r0, -c0)
            - scat_chapman(H, r1, c1);
    }
    return v > 0.0f ? v : 0.0f;
}

/* --- the planet's own shadow -----------------------------------------------
 *
 * What share of a ray's air is still in sunlight. One number, and the whole of
 * twilight comes out of it.
 *
 * The sky after sunset is not lit by a schedule. The ground is in shadow and
 * the air over it is not: the planet's shadow is a cylinder of the planet's own
 * radius cast down-sun, so at any moment it has swallowed everything below a
 * certain height and nothing above it, and that height climbs as the sun goes
 * further under. A ray's in-scatter is what its *lit* air scatters, and since
 * the air thins exponentially the share falls away with the shadow's climb —
 * two thirds of the column still lit at six degrees under, a fifth at twelve,
 * a fiftieth at eighteen, which is civil, nautical and astronomical twilight
 * arriving out of geometry rather than out of a table.
 *
 * It gives the arch for free, which a schedule cannot. The shadow's boundary is
 * further away along a ray pointed at where the sun went than along one pointed
 * away from it, so more of that ray is lit: the glow stays in the sunset's own
 * quarter of the sky and narrows as the night comes up, which is what a
 * twilight arch is.
 *
 * A point P from the planet's centre is in shadow when it is behind the
 * terminator plane, P.s < 0, and within R of the shadow's axis,
 * |P|^2 - (P.s)^2 < R^2. Along the ray both are quadratics in u, so the
 * shadowed part is one interval, and the lit column is the whole column less
 * that interval's share of it — one extra Chapman evaluation, and only when
 * there is a shadow to find. `total` is the ray's own Rayleigh column,
 * unspread; the horizontal compression is a scale on both and cancels.
 *
 * Rayleigh alone. The aerosol is under the first kilometre and a half, so its
 * lit share is either all of it or none of it and it is the same answer either
 * way; carrying a second Chapman pair to say so would double the cost of the
 * only part of this that is not free. */
static float scat_lit(float z0, fly_v3 rd, float sz0, float mu, float t, float total) {
    float rm = FLY_PLANET_R + (z0 > 0.0f ? z0 : 0.0f);
    float a = 1.0f - mu * mu;
    float b = rm * (rd.z - mu * sz0);
    float c = rm * rm * (1.0f - sz0 * sz0) - FLY_PLANET_R * FLY_PLANET_R;
    float u0, u1, disc, r0, px, py, pz, dark;
    if (total <= 0.0f || t <= 0.0f) return 1.0f;
    if (a < 1e-8f) {                    /* looking straight down-sun or up it */
        if (c >= 0.0f) return 1.0f;     /* and outside the cylinder for good */
        u0 = 0.0f;
        u1 = t;
    } else {
        disc = b * b - a * c;
        if (disc <= 0.0f) return 1.0f;  /* the ray never enters the cylinder */
        disc = sqrtf(disc);
        u0 = (-b - disc) / a;
        u1 = (-b + disc) / a;
    }
    /* and the half of the cylinder that is behind the terminator: u*mu + rm*sz0 < 0 */
    if (mu > 1e-8f) { float ue = -rm * sz0 / mu; if (ue < u1) u1 = ue; }
    else if (mu < -1e-8f) { float ue = -rm * sz0 / mu; if (ue > u0) u0 = ue; }
    else if (sz0 >= 0.0f) return 1.0f;
    if (u0 < 0.0f) u0 = 0.0f;
    if (u1 > t) u1 = t;
    if (u1 <= u0) return 1.0f;
    /* the shadowed sub-column, re-parametrised at its own start so the same
       closed form answers it: a difference of two columns-to-here is exact */
    px = rd.x * u0;
    py = rd.y * u0;
    pz = rm + rd.z * u0;
    r0 = sqrtf(px * px + py * py + pz * pz);
    if (r0 < 1.0f) return 1.0f;
    dark = scat_thick(FLY_SCAT_HR, r0 - FLY_PLANET_R,
                      (px * rd.x + py * rd.y + pz * rd.z) / r0, u1 - u0);
    if (dark >= total) return 0.0f;
    return (total - dark) / total;
}

/* Extinction and in-scattered light along ro + rd*t.
 *
 * `transmit` is what survives of whatever is at the far end; `inscat` is what
 * the air between here and there adds. Sky, sun and aerial perspective are all
 * this one call — the sky is simply the case where the far end is space and
 * there is nothing behind it. */
static void fly__scatter(const fly__env *e, fly_v3 ro, fly_v3 rd, float t,
                         fly_v3 *transmit, fly_v3 *inscat) {
    const fly_v3 bR0 = { 5.802e-6f, 13.558e-6f, 33.100e-6f };
    float haze = 1.0f + e->storm * 7.0f + e->dusk * 1.2f;
    fly_v3 bR = bR0;
    float bM = 3.996e-6f * haze;
    float bMe = bM * 1.11f;              /* aerosols absorb as well as scatter */
    /* the horizontal compression, weighted by how horizontal this ray is */
    float spread = FLY_SCAT_SPREAD +
                   (1.0f - FLY_SCAT_SPREAD) * fly_clampf(fabsf(rd.z), 0.0f, 1.0f);
    /* No air past the surface. A ray steeper than the horizon dip ends on the
     * ground and the column stops there; a shallower one misses the planet
     * entirely and runs to space, however far below the horizontal it points.
     * Only the sphere can tell those apart. */
    float ground_t = fly__planet_hit(ro, rd, NULL);
    if (ground_t > 0.0f && t > ground_t) t = ground_t;
    float tr0 = scat_thick(FLY_SCAT_HR, ro.z, rd.z, t);
    float tr = tr0 * spread;
    float tm = scat_thick(FLY_SCAT_HM, ro.z, rd.z, t) * spread;
    fly_v3 tau = fly_v3mk(bR.x * tr + bMe * tm, bR.y * tr + bMe * tm, bR.z * tr + bMe * tm);
    fly_v3 T = fly_v3mk(expf(-tau.x), expf(-tau.y), expf(-tau.z));
    float mu = fly_v3dot(rd, e->sun);
    float pr = 0.0596831f * (1.0f + mu * mu);                  /* 3/16pi (1+mu^2) */
    float g2 = FLY_SCAT_G * FLY_SCAT_G;
    float dn = 1.0f + g2 - 2.0f * FLY_SCAT_G * mu;
    float pm = 0.0795775f * (1.0f - g2) / (dn * sqrtf(dn) + 1e-4f);
    /* Sun transmittance at the middle of the segment. One sample instead of an
     * inner integral: the shadowing of sunlight by the air it crosses varies
     * slowly compared with the density it is weighting.
     *
     * Two things about where that sample is taken were flat-chart answers, and
     * both are wrong on the ball for the same shallow rays the column was.
     *
     * The altitude at the midpoint is the sphere's, not the slope's. A ray half
     * a degree below level from 400 m never gets below 377 m — it is above the
     * horizon dip, so it descends for five kilometres and climbs for ever after
     * — where the slope says it is a kilometre underground by 160 km out. The
     * old expression clamped that to zero and lit the far half of a shallow ray
     * with sea-level sunlight.
     *
     * And the midpoint is taken along the first FLY_SCAT_FAR of the ray rather
     * than along all of it. That is what the sample is for: it stands in for
     * where the air the segment crosses is, and past a few hundred kilometres a
     * longer ray adds no air at all, so measuring the midpoint of the whole
     * thing would only push the sample somewhere emptier the further the ray
     * reaches. Below the old 320 km the two are the same expression.
     *
     * The elevation floor used to be 0.035 because the *flat* path length ran
     * away below about two degrees. On the ball it does not: the horizontal
     * column is finite and reddens the last light on its own. What is floored
     * now is only the point where a sun below the horizon would send the sample
     * underground, and the daylight term is fully off by then anyway. */
    float zmt = (t < FLY_SCAT_FAR ? t : FLY_SCAT_FAR) * 0.5f;
    float rm = FLY_PLANET_R + (ro.z > 0.0f ? ro.z : 0.0f);
    float zm = fly_clampf(sqrtf(rm * rm + 2.0f * rm * rd.z * zmt + zmt * zmt)
                          - FLY_PLANET_R, 0.0f, 40000.0f);
    /* How high the sun is *where the air is*, which is not the same question as
     * how high it is over the camera, and on a ball the difference is the whole
     * day/night terminator.
     *
     * `e->sun` is one direction for the whole chart, which is right — the sun is
     * far enough away that its rays are parallel — but its *elevation* is a
     * local quantity, `dot(sun, up)`, and `up` turns with the ground. Taking it
     * as `sun.z` says the sun is equally high everywhere on the planet, so the
     * air over the night side scatters daylight. That was invisible while a sky
     * ray from orbit returned black; with the ring drawn it is the whole far
     * limb glowing at midnight.
     *
     * Asked at the segment's *lowest* point, which is where its air is: the
     * perigee when the segment contains one, and the nearer end when it does
     * not. For everything flown inside the atmosphere that point is a few
     * kilometres away and the answer is the one it always was — a level ray at
     * sea level has its perigee at the camera. For a ray grazing the limb from
     * six hundred kilometres up it is a thousand kilometres downrange, sixty
     * degrees of arc away, and it is the only sample that knows whether that
     * piece of air is in daylight.
     *
     * The altitude above is deliberately *not* moved to the same point. The two
     * samples answer different questions and only this one is a geometry
     * question: `zm` feeds the sun's own column, which for a sky ray is
     * effectively unattenuated, and FLY_SCAT_SUN is calibrated against exactly
     * that. Sampling it where the air is instead is a better model and a
     * re-tune of the whole exposure — see TODO. */
    float sun_up, zlow;
    {
        float tp = -rm * rd.z;                        /* the perigee, if any */
        float ts = tp < 0.0f ? 0.0f : (tp > t ? t : tp);
        float px = rd.x * ts, py = rd.y * ts, pz = rm + rd.z * ts;
        float rl = sqrtf(px * px + py * py + pz * pz);
        sun_up = rl > 1.0f
               ? (e->sun.x * px + e->sun.y * py + e->sun.z * pz) / rl
               : e->sun.z;
        zlow = rl - FLY_PLANET_R;
    }
    /* How much of the *column* over that point is still in the sun.
     *
     * Multiply-scattered light does not come from the air in front of the eye,
     * so it cannot be gated by how much of the ray is lit: after sunset the
     * anti-solar half of the sky is shadowed along every ray in it, and it is
     * lit all the same — by the half that is not. The sky's own illumination is
     * the honest gate for it, and vertically the same geometry has a closed
     * form with no Chapman in it, because the shadow's top over a point is at
     * R(1/sqrt(1-c^2) - 1) and an exponential atmosphere above it is one exp.
     *
     * The airglow rides on the same number: it is emission from the whole air
     * column, so what it fills in as is the sky going out, not one direction of
     * it going out. */
    float sky_lit = 1.0f;
    if (sun_up < 0.0f) {
        /* clamped because sun_up is a dot product over a length and can come
           back a hair past -1: sqrtf of a negative is a NaN, the comparison
           below is then false, and the answer would be a *fully lit* night */
        float su = sun_up < -0.999999f ? -0.999999f : sun_up;
        float hs = FLY_PLANET_R * (1.0f / sqrtf(1.0f - su * su) - 1.0f);
        sky_lit = hs > zlow ? expf(-(hs - zlow) / FLY_SCAT_HR) : 1.0f;
    }
    /* The floor is the deepest the sun's own column is allowed to get, and it
     * used to be -0.14 because that was also where the daylight term was
     * switched off: past it the number could not be seen, so it did not have to
     * be right. It can be seen now — twilight is the light that comes down a
     * grazing column, and what makes it the colour it is, is exactly how far
     * that column has run. Taken to -0.35 the last of the glow reddens the way
     * a real one does and then goes out on its own transmittance, where at
     * -0.14 it stopped deepening and faded out grey. It cannot run away below
     * that: scat_chapman has no air under the surface, so the column saturates
     * at the two grazing halves however far the sun goes down. */
    float sz = sun_up > FLY_SCAT_SUNFLOOR ? sun_up : FLY_SCAT_SUNFLOOR;
    float ssp = FLY_SCAT_SPREAD + (1.0f - FLY_SCAT_SPREAD) * fabsf(sz);
    float sr = scat_thick(FLY_SCAT_HR, zm, sz, 1e6f) * ssp;
    float sm = scat_thick(FLY_SCAT_HM, zm, sz, 1e6f) * ssp;
    fly_v3 Ts = fly_v3mk(expf(-(bR.x * sr + bMe * sm)), expf(-(bR.y * sr + bMe * sm)),
                         expf(-(bR.z * sr + bMe * sm)));
    /* The density-weighted mean of the view transmittance over the segment,
     * per channel. Exact for a single species; the error where two are mixed
     * is far below what a second sample would cost.
     *
     * Per channel is not a detail. Weighting all three by the mean optical
     * depth leaves the saturating end of a long path still blue, which put the
     * noon horizon *bluer* than the zenith — the opposite of what air does.
     * Divided by its own optical depth each channel saturates at its own
     * single-scattering albedo, so a path long enough to scatter everything
     * comes back white, which is what a hazy horizon is. */
    fly_v3 avg = fly_v3mk(tau.x > 1e-5f ? (1.0f - T.x) / tau.x : 1.0f,
                          tau.y > 1e-5f ? (1.0f - T.y) / tau.y : 1.0f,
                          tau.z > 1e-5f ? (1.0f - T.z) / tau.z : 1.0f);
    float taua = (tau.x + tau.y + tau.z) * (1.0f / 3.0f);
    /* How much of this air is in daylight at all — the share of the ray's own
     * column that the planet's shadow has not reached. See scat_lit: this used
     * to be a smoothstep on the sun's local elevation, which is a schedule
     * standing in for a geometry, and a schedule has no twilight in it. */
    float lit = scat_lit(ro.z, rd, e->sun.z, mu, t, tr0);
    fly_v3 s = fly_v3scale(fly_v3mk(bR.x * tr * pr + bM * tm * pm,
                                    bR.y * tr * pr + bM * tm * pm,
                                    bR.z * tr * pr + bM * tm * pm), lit);
    /* Shafts, as shadowed fog rather than as streaks smeared out of a bright
     * pixel. Air only scatters sunlight that reaches it, so the in-scatter is
     * gated by what the cloud deck lets through: look across a gap and the air
     * in the beam lights up, look along a shadow and it does not.
     *
     * One tap at the midpoint answers that for a short segment. It answers
     * nothing at all for a long one, and a sky ray's far end is space — 320 km,
     * so the tap sat 160 km out, sliding most of a cell of a 5.2 km cloud field
     * between one pixel and the next. That is what hung a row of hard-edged
     * grey slabs along the horizon of every outdoor scene, in all three
     * pipelines at once, and moving the tap does not fix it: any single tap
     * that far out is a point sample of noise, so it aliases wherever it lands.
     *
     * The limit is what fixes it. Sunlight crossing tens of kilometres of air
     * is gated by the deck's *average* transmission, not by one point of it, so
     * the tap fades into that average over the deck's own base wavelength. Near
     * field — every shaft this exists for — is untouched at blend ~0; a sky ray
     * lands on a constant, and a constant cannot alias. */
    float shaft = cloud_shadow(e, fly_v3add(ro, fly_v3scale(rd, t * 0.5f)));
    float wide = fly_clampf(t * 0.5f / 5200.0f, 0.0f, 1.0f);
    shaft = fly_lerpf(shaft, 1.0f - e->cloud_mean * 0.62f, wide);
    /* Multiple scattering. One extra isotropic term, weighted by how much of
     * the path has scattered at all: nothing at the zenith at noon, where the
     * air is thin and single scattering is very nearly the whole story, and
     * most of the light near the horizon and at dusk, where it is not. This is
     * the term whose absence made a physically-correct sky come out too dark
     * and too grey exactly where a real one is brightest. */
    {
        float ms = FLY_SCAT_MS * (1.0f - expf(-taua)) * sky_lit;
        float iso = 0.0795775f;   /* 1/4pi: scattered many times, from nowhere */
        fly_v3 sm2 = fly_v3mk((bR.x * tr + bM * tm) * iso, (bR.y * tr + bM * tm) * iso,
                              (bR.z * tr + bM * tm) * iso);
        s = fly_v3add(s, fly_v3scale(sm2, ms));
    }
    s = fly_v3scale(fly_v3mul(fly_v3mul(s, Ts), avg), FLY_SCAT_SUN * shaft);
    /* the sky is not black at night: airglow and scattered moonlight, weighted
     * the same way the daylight in-scatter is so it fades in as the sun goes —
     * and attenuated the same way too, by `avg`, because light emitted or
     * scattered along a path is dimmed by the air between it and the eye
     * exactly as the daylight in-scatter above it is.
     *
     * Leaving that out is invisible from the ground, where a zenith ray has
     * `avg` within a fifth of one, and wrong from anywhere else: a ray grazing
     * the limb from six hundred kilometres up saturates `taua` completely, so
     * it collected the *full* airglow — the brightest this term can be — with
     * nothing taking any of it back. That put a floor of 0.015 under the night
     * side of the planet seen from orbit and is why `render.planet` reads 17x
     * across the dawn limb where it wants 20. It is not the multiple-scattering
     * term the TODO guessed at: that one is already gated by `night` and by the
     * sun's own transmittance, and it goes to nothing on the far side. */
    {
        float amb = (1.0f - sky_lit) * (1.0f - expf(-taua * 1.6f));
        s = fly_v3add(s, fly_v3mul(fly_v3scale(fly_v3mk(0.010f, 0.016f, 0.032f), amb), avg));
    }
    /* Storms grey what you see through them: rain, spray and the sub-visible
     * cloud under a deck, none of which the two-species integral above
     * carries. It used to be one lerp on the finished *sky* colour, which
     * left the air itself ungreyed — so a ridge forty kilometres out
     * converged to the ungreyed in-scatter and came out 1.4x brighter than
     * the sky it sat against. A surface darker than the sky cannot pass it,
     * however far away it is; it has to approach it from below.
     *
     * So the grey belongs to the air, and the share of it a path collects is
     * how saturated that path is: nothing at arm's length, all of it once the
     * path has scattered everything. A sky ray saturates long before its
     * 320 km are up, so the horizon greys exactly as it did before — what
     * changed is that the air a ridge is seen through now greys with it.
     *
     * The in-scatter only. Attenuating the transmittance by the same weight
     * would make `surf*T + s` an exact lerp toward the grey, which is tidier,
     * but the same call supplies the sun beam in make_env, where the storm
     * dimming is already spent as `1 - storm*0.6` — so it cost a further 29%
     * of the direct sun in a storm for a term that is 0.02% of a forty-
     * kilometre pixel. Measured, not assumed. */
    if (e->storm > 0.0f) {
        float w = e->storm * FLY_SCAT_STORM * (1.0f - expf(-taua));
        s = fly_v3lerp(s, fly_v3scale(fly_v3mk(0.22f, 0.23f, 0.26f), e->day + 0.05f), w);
    }
    if (transmit) *transmit = T;
    if (inscat) *inscat = s;
}

/* Aerial perspective: what a distance does to a colour. */
static void fog_pair(const fly__env *e, fly_v3 ro, fly_v3 d, float dist,
                     fly_v3 *transmit, fly_v3 *inscat);

static fly_v3 apply_fog(const fly__env *e, fly_v3 col, fly_v3 ro, fly_v3 dirn, float dist) {
    fly_v3 T, s;
    fog_pair(e, ro, fly_v3norm(dirn), dist, &T, &s);
    return fly_v3add(fly_v3mul(col, T), s);
}

/* --- the planet, from far enough away to be one --------------------------
 *
 * The chart is flat and the terrain marches stop at about twenty kilometres,
 * so every ray that reaches past them used to come back sky. Down low that is
 * invisible, because thirty kilometres of haze has already saturated: the
 * ground you cannot see and the sky you can are the same colour. From a
 * hundred and fifty kilometres up it is the whole picture — a twenty-kilometre
 * patch of terrain directly underneath and sky in every other direction, which
 * is not what a planet looks like.
 *
 * So the background pass hit-tests the ball. The chart is a tangent plane, so
 * the centre is exactly r0 = R + altitude straight down from the ray origin,
 * and the intersection is the ordinary quadratic:
 *
 *     t^2 + 2 t (r0 rd.z) + (r0^2 - R^2) = 0
 *
 * whose discriminant R^2 - r0^2 (1 - rd.z^2) is negative exactly when the ray
 * is above the horizon. That one sign test is the horizon: no angle to tune,
 * no fade to place, and it lands at arcsin(R/r0) off nadir because that is
 * where the geometry puts it.
 *
 * `n_out` is the *spherical* normal at the hit, not the chart's +z, which is
 * what a shaded limb will need: the sun's angle against it varies across the
 * face, so a disc lit through it reads as a ball rather than as a coin.
 *
 * Nothing draws with this yet — see TODO, `the planet from space`. It is
 * here, and gated, because the geometry is the part that has an exactly
 * right answer and the shading is the part that does not. */
static float fly__planet_hit(fly_v3 ro, fly_v3 rd, fly_v3 *n_out) {
    float r0 = FLY_PLANET_R + ro.z;
    float b = r0 * rd.z;
    float disc = FLY_PLANET_R * FLY_PLANET_R - r0 * r0 * (1.0f - rd.z * rd.z);
    float t;
    if (disc <= 0.0f) return -1.0f;
    t = -b - sqrtf(disc);
    if (t <= 0.0f) return -1.0f;
    if (n_out) {
        fly_v3 hp = fly_v3add(ro, fly_v3scale(rd, t));
        *n_out = fly_v3norm(fly_v3mk(hp.x - ro.x, hp.y - ro.y, hp.z - (ro.z - r0)));
    }
    return t;
}

/* --- the face of a planet ------------------------------------------------
 *
 * Three tones off the heightfield — sea, lowland, highland — was the whole of
 * this, and with the ocean basins in the ground it stopped being enough in a
 * specific way: it draws a world whose land is one colour from the equator to
 * the ice, which no planet with an atmosphere on it looks like. What you can
 * actually resolve from four hundred kilometres is not detail, it is climate:
 * where the water is, where the desert belts are, where the ice starts, and
 * the weather sitting on top of all of it.
 *
 * So the albedo is those four things and nothing finer. Each is a field
 * fly_world already owns and world-gen already reads, which is what keeps this
 * honest rather than decorative: the limb is green where the ground shader
 * draws grass, white where terrain_surface lays snow (the snow line is
 * anchored on its 2050 m midpoint), and dry where home would not have been
 * sited. `terrain_surface` itself is still the wrong function to call here —
 * it resolves litter and slope at a range where a whole valley is inside a
 * pixel — and this stays short enough that the twin in fly_glsl.h is exact.
 *
 * The numbers are the measured ones rather than picked: ocean 0.03-0.07,
 * forest 0.08-0.12, grassland to scrub 0.16-0.20, sand 0.3-0.4, snow 0.6-0.9.
 * The old lowland tone sat at 0.14 and the old highland at 0.34, so a planet
 * of them was uniformly as bright as a steppe with a desert's highlands.
 *
 * They are also where the ground shader's own albedo lands — `world.forest`
 * measures a wood at 0.092 and open ground at 0.192 — and that is a
 * requirement rather than a coincidence: `terrain_surface` crosses over into
 * this function as the camera climbs (see chart_gives_way), so the nineteen
 * kilometres of ground the rasterizer draws and the four hundred the ball
 * draws have to meet in one colour. Two sets of tones tuned separately meet at
 * a visible edge, which is exactly what the square of near terrain looked like
 * from a hundred and fifty kilometres once there was any contrast on the ball
 * to see it against. */
static fly_v3 planet_albedo(uint32_t seed, fly_v3 u, float gh) {
    float arid = fly_world_aridity(seed, u);
    /* Where the snow starts, and it is not only how cold it is: nothing lies
     * on a range nothing falls on, which is why the dry side of a mountain
     * chain is bare rock at an altitude the wet side is a glacier at. */
    float snow = fly_world_snowline(seed, u) + arid * 800.0f;
    fly_v3 c;
    if (gh <= FLY_WATER_LEVEL) {
        /* Deep water is nearly black and blue; a shelf is neither, because the
         * bed is close enough to send light back up. The two hundred metres
         * that separates them is the continental slope, so the pale ring lands
         * on the shelf by itself rather than being drawn round the coast. */
        c = fly_v3lerp(fly_v3mk(0.052f, 0.098f, 0.120f),
                       fly_v3mk(0.008f, 0.020f, 0.052f),
                       fly_smoothstepf(15.0f, 700.0f, FLY_WATER_LEVEL - gh));
    } else {
        c = fly_v3lerp(fly_v3mk(0.042f, 0.098f, 0.030f),   /* forest */
                       fly_v3mk(0.205f, 0.180f, 0.088f),   /* steppe */
                       fly_smoothstepf(0.22f, 0.62f, arid));
        c = fly_v3lerp(c, fly_v3mk(0.400f, 0.305f, 0.165f),  /* sand */
                       fly_smoothstepf(0.55f, 0.90f, arid));
        /* Above the treeline it is stone whatever the climate is — and the
         * treeline is the snow line's own, five hundred metres under it,
         * rather than a fixed altitude. Trees stop where it is too cold for
         * them, which is a question about latitude first: a fixed 900 m band
         * put bare rock over half the temperate world and green tundra inside
         * the arctic circle. */
        c = fly_v3lerp(c, fly_v3mk(0.135f, 0.125f, 0.115f),
                       fly_smoothstepf(snow - 500.0f, snow + 400.0f, gh));
    }
    /* And the snow line runs through both: on land it is a treeline-shaped
     * band that follows the relief, and where it falls below the waterline the
     * sea is over it and the cap closes over the pole. One expression, because
     * they are one fact.
     *
     * The band is 900 m deep and that is the difference between snow and a
     * white wire. A ridged mountain field crosses any given contour along a
     * line a few hundred metres wide, so a 400 m band picked out every ridge
     * on the planet as a bright thread at full white — thousands of them,
     * evenly scattered, which is not what snow on a world looks like from
     * outside it. Over 900 m a summit that only just clears the line is a
     * fifth white, a range holds snow along its spine and bare rock down its
     * flanks, and full white is left for what is genuinely a kilometre into
     * the cold: true summits, and the caps. */
    c = fly_v3lerp(c, fly_v3mk(0.62f, 0.66f, 0.72f),
                   fly_smoothstepf(snow, snow + 900.0f,
                                   gh > FLY_WATER_LEVEL ? gh : FLY_WATER_LEVEL));
    return c;
}

/* The weather, at the size weather actually is.
 *
 * The cloud deck is a 5.2 km field on the chart and neither half of that
 * survives the trip out: 5.2 km is a fifth of a pixel from a hundred and fifty
 * kilometres, so sampling it there is the same speckle the ponds were, and a
 * chart-space field has a seam in it wherever the chart comes back on itself.
 * What is left at that range is the shape of the circulation — cloud along the
 * convergence at the equator, a clear band over the descending air that makes
 * the deserts underneath it, and the storm track at mid latitudes — which is a
 * field on the ball a few hundred kilometres across.
 *
 * The two are never both in the picture. The deck's march fades into haze on
 * exp(-t/38000) and this fades in on exactly the complement of it, so at
 * twenty kilometres there is nothing here and from orbit there is nothing
 * there. The same number, used twice, is what makes that a rule rather than
 * two fades that happen to meet. */
static float planet_cloud(const fly__env *e, fly_v3 u) {
    float lat = fabsf(u.z);
    float itcz = (lat - 0.0f) / 0.20f;
    float dry = (lat - 0.42f) / 0.16f;
    float track = (lat - 0.80f) / 0.22f;
    /* the systems turn slowly, and they turn with the world's own clock */
    float d = (float)(e->time * 4.0e-6);
    /* Four octaves, 150 km down to 19. Three drew fronts with no edges on
     * them — a cloud whose only scale is the size of the system it belongs to
     * is fog — and five put a 9 km octave under four pixels from six hundred
     * kilometres, which is the range this is most of the picture at. Nineteen
     * is the finest thing that still resolves from out there. The bands say
     * where weather is, the octaves say what it looks like, and the deck below
     * has no say in either — see above. */
    float n = fly_fbm3(e->seed + 62, u.x * 4.0f + d, u.y * 4.0f, u.z * 4.0f, 4);
    float cov = 0.5f + 0.5f * n + 0.18f * expf(-itcz * itcz) -
                0.24f * expf(-dry * dry) + 0.14f * expf(-track * track);
    /* The far edge of the ramp is past anything the field reaches, and that is
     * the difference between weather and a fog bank. A ramp that saturates
     * takes every system's middle to the same flat white, so a front covering
     * a quarter of the disc is one featureless mass with a soft rim; left
     * unsaturated, the octaves inside it survive as the texture that says how
     * thick it is where. Nothing is ever quite opaque, which is also true. */
    return fly_smoothstepf(0.52f, 1.02f, cov);
}

/* what sky_sample is allowed to add on top of the scattering integral */
#define FLY_SKY_STARS 1u
/* Shade the ball where the ray meets it. Only camera rays ask for this: the
 * irradiance projection has its own ground-bounce term and would otherwise
 * count the surface twice. */
#define FLY_SKY_PLANET 4u
/* Leave both cloud layers out: the air on its own.
 *
 * Nothing in the renderer asks for this — the sky that lights the world is the
 * sky that is there, cloud included, and `sky_ambient` still means exactly
 * that. It exists because a claim about *air* cannot be measured through
 * cloud. `render.atmosphere` asserts Rayleigh at a fixed eye and hour — the
 * noon zenith is blue by better than 1.9 in blue over red, the eight o'clock
 * one by better than 4.4 — and those numbers held only because the deck
 * happened to be clear over that one spot on that one seed. When a second
 * layer was first tried it put cloud there, and the changelog records what the
 * gate then read: 1.56 against the 2.03 it wanted 1.9 of, and 1.25 against
 * 4.92 in the morning. That is a gate correctly measuring a cloud and
 * incorrectly reporting the atmosphere. The measurement wanted was always the
 * clear-air one; there was simply no way to ask for it. */
#define FLY_SKY_NOCLOUD 8u

/* FLY_SKY_NOSUN is declared with the forward declaration above: it leaves the
 * disc and its halo out, because the irradiance projection adds the sun as a
 * directional light and would otherwise count it twice. */

/* --- the cloud deck as a medium ------------------------------------------
 *
 * A short march through the slab with sun self-shadowing, so cloud bases
 * darken where thick and edges stay soft; it works from below, from above
 * looking down on the deck, and from inside the layer. Returns how much of the
 * path the cloud covers, 0..1, and the colour it covers it with, so the caller
 * can `lerp` whatever was behind it toward the deck.
 *
 * `tmax` is where the ray stops. For a sky ray that is the far clamp and the
 * whole layer is on the path; for a surface ray it is the surface, and the
 * cloud between the eye and a mountain is what this exists to draw. That
 * second case is a modelling gap that was open for a long time: with the
 * camera inside the deck the slab was marched for sky rays and for nothing
 * else, so a mountain top *in* cloud came out crisp with a grey wash behind
 * it, which is the one thing being inside cloud does not look like.
 *
 * The march has to survive grazing rays, which is where it used to fail in
 * exactly the way the shaft gate does. Neither of the two changes is optional
 * near the horizon, because the slab is 580 m thick and a ray a degree above
 * level spends fifty kilometres inside it:
 *
 *  - the four taps are then eight kilometres apart, so each is an independent
 *    point sample of a 5.2 km field. They fade into the deck's mean cover once
 *    the step outgrows what a pixel can resolve, the same trade terrain and
 *    water already make against pxscale;
 *  - and there is no distance cutoff. Rejecting the march past 60 km drew a
 *    horizontal line across the sky at whatever elevation that landed on —
 *    about two degrees from a hilltop. The haze fade already takes distant
 *    deck to nothing; it just has to be allowed to do it smoothly.
 *
 * The alpha per tap is Beer-Lambert over the segment the tap stands for, and
 * for a long time it was not: it was a fixed fraction of the density, so a ray
 * crossing fifty kilometres of deck sideways accumulated no more than one
 * crossing 580 m straight down. That is wrong in exactly the direction that
 * shows — horizon cloud you can see the sky through — and it could not be
 * corrected on its own, because the honest integral makes a grazing ray opaque
 * and the deck's night colour used to sit twelve times above a moonless sky.
 * Opaque plus that floor is a bright torn strip under the stars. The floor came
 * down first (see FLY_MOON, where the night deck is set against the sky
 * it covers rather than against the ground), and the integral follows it here.
 * `FLY_CLOUD_EXT` is set so that the one ray the old model was calibrated on —
 * straight down through the slab — comes out where it always did. */
static float cloud_slab(const fly__env *e, fly_v3 ro, fly_v3 rd, float tmax,
                        fly_v3 *out) {
    float cb = FLY_CLOUD_Z - FLY_CLOUD_BASE, ctop = FLY_CLOUD_Z + FLY_CLOUD_RISE;
    float depth = ctop - cb;
    float dz = rd.z, t0, t1, fade, zr, perm, seg, acc = 0.0f;
    fly_v3 bright, dark, ccol = fly_v3zero();
    int si;
    *out = fly_v3zero();
    if (tmax <= 0.0f) return 0.0f;
    if (fabsf(dz) < 1e-4f) dz = dz < 0.0f ? -1e-4f : 1e-4f;
    t0 = (cb - ro.z) / dz;
    t1 = (ctop - ro.z) / dz;
    if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 > tmax) t1 = tmax;
    if (t1 <= t0) return 0.0f;
    fade = expf(-t0 / 38000.0f); /* distant clouds sink into haze */
    if (fade <= 0.002f) return 0.0f;
    bright = e->cloud_lit;
    dark = e->cloud_dark;
    /* How much deck one pixel covers where the taps land. The tap slides with
     * the view direction at t metres per radian across and t/|rd.z| up — the
     * second is what bites, because a ray a degree above level puts the tap a
     * hundred kilometres out and moves it a quarter of a kilometre per
     * scanline. Once that outruns the field's finest octave the tap is
     * sampling noise, so it fades into the mean: the same trade terrain and
     * water already make against pxscale, for the same reason.
     *
     * Per tap, not once for the ray, and this is the part two earlier versions
     * got wrong. Keying it on `t0`, where the ray *enters* the slab, is wrong
     * wherever the camera is inside the layer: t0 is then zero, the fade never
     * engages at all, and the four taps still spread from the camera out to the
     * exit clamp. Every scanline lands in a different cell and the sky above
     * the skyline comes out as a band of hard horizontal stripes, which is what
     * every night and dusk frame with the camera near cloud height had.
     * Suppressing the slab drops the band's row-to-row second difference from
     * 0.50 to 0.03, which is how it was pinned on the march. Keying it on the
     * last tap instead fixes the engagement and not the stripes, because the
     * four taps are then in completely different regimes — six kilometres out
     * and forty-four, for a ray a degree below level inside the deck — and one
     * fade for all of them either aliases the far tap or flattens the near one.
     *
     * The ramp is `s^2/(s^2 + k^2)` rather than a smoothstep on two thresholds.
     * The slide goes as the inverse square of the elevation, so a smoothstep is
     * squeezed into about a degree of sky however far apart its thresholds are,
     * and a transition that happens within a degree is itself the horizontal
     * line the fade exists to remove. This one has no thresholds to squeeze,
     * approaches its limit smoothly from both ends, and costs a multiply and a
     * divide where ramping on a logarithm cost a logf per tap.
     *
     * The floor on |rd.z| is only there to keep the divide finite, so it
     * belongs far below the angles this is meant to describe. At 0.02 it sat at
     * 1.1 degrees — inside the band — and capped the computed slide at a third
     * of the truth exactly where the taps move fastest. */
    zr = fabsf(rd.z) > 0.0015f ? fabsf(rd.z) : 0.0015f;
    perm = 1.0f / (zr * (e->pxscale + 1e-6f));
    /* what one tap stands for, in metres of ray: the whole interval shared
       between the four of them */
    seg = (t1 - t0) * 0.25f;
    /* The early-out is at 0.995 and used to be at 0.98, which was harmless
     * while a tap could only ever add 0.45 of its density and is not now. What
     * the march returns is `ccol/acc` — the mean colour of the taps it took —
     * so stopping two per cent short does not drop two per cent of the light,
     * it drops a whole tap out of that mean, and it does so at whatever
     * elevation the saturation happens to cross. That is a step across the sky
     * at one elevation, which is precisely what the kink gate is for: it was
     * worth 0.55% at 16:00 on its own, against a bound of 0.7%. */
    for (si = 0; si < 4 && acc < 0.995f; ++si) {
        float t = fly_lerpf(t0, t1, ((float)si + 0.5f) / 4.0f);
        fly_v3 p = fly_v3add(ro, fly_v3scale(rd, t));
        /* Two footprints, because the tap has two extents and they are not the
         * same size. Across the ray it covers what a pixel covers, `slp`;
         * along the ray it stands for its whole segment, and near the horizon
         * that is kilometres where the pixel is metres. The field is isotropic
         * and cannot be filtered in one direction only, so which footprint an
         * octave is fetched at decides what is lost:
         *
         *  - the fine octaves are what a cloud mass is *made of* across the
         *    sky, and the ring `render.atmosphere` walks at eight degrees is
         *    exactly that measurement, so they are fetched at the pixel rate
         *    and averaged along the ray by the three stations below;
         *  - the collapse to the deck's mean is what stops the base octave
         *    aliasing, and the base octave is what the long extent actually
         *    undersamples, so that ramp takes the segment.
         *
         * The self-shadow lookup takes the segment on both, because it is a
         * lighting term rather than a shape: nothing measures its detail, and
         * it swings the tap between `cloud_lit` and `cloud_dark`, which is the
         * largest contrast in the march. Blurring it cost nothing measurable
         * and took a quarter off the worst kink at 16:00. */
        float slp = t * perm;
        float sw = seg * FLY_CLOUD_SEGW;
        float sl = slp > sw ? slp : sw, s2 = sl * sl;
        float sp2 = slp * slp;
        float coarse = sp2 / (sp2 + FLY_CLOUD_FINE * FLY_CLOUD_FINE);
        float coarse_s = s2 / (s2 + FLY_CLOUD_FINE * FLY_CLOUD_FINE);
        float wide = s2 / (s2 + FLY_CLOUD_FLAT * FLY_CLOUD_FLAT);
        /* The density a tap stands for is the mean over its segment, not a
         * point sample of it — three stations a third of a segment apart, so
         * the four taps between them walk the interval at twelve uniform
         * points. With one point per tap a grazing ray samples a 2.6 km field
         * every 2.2 km, which is under Nyquist, and Beer-Lambert then turns
         * that into a swing rather than a wobble: the exponential's slope in
         * the density is `seg/L`, where the old fixed alpha's was 0.45, so at
         * five degrees it multiplies the field's own sampling error by six.
         * That is the whole of why the elevation kink went from 0.36% to 2.26%
         * when the integral changed and nothing else did. */
        float hx = rd.x * seg * (1.0f / 3.0f), hy = rd.y * seg * (1.0f / 3.0f);
        float cov = (1.0f / 3.0f) *
                    (cloud_cover_lod(e, p.x - hx, p.y - hy, coarse, wide) +
                     cloud_cover_lod(e, p.x, p.y, coarse, wide) +
                     cloud_cover_lod(e, p.x + hx, p.y + hy, coarse, wide));
        /* How far up this column goes, and the shape inside it.
         *
         * The top comes off the coverage the tap has already paid for, so the
         * whole of the deck's third dimension costs two multiplies. Taken per
         * tap and not per ray: a ceiling resolved once and applied down the
         * length of a ray is a step in *elevation*, which lands at the same
         * place on every ray that shares it and draws a horizontal line across
         * the sky — measured at 1.9% of the mean on the kink `render.atmosphere`
         * watches for, against a bound of 0.7%.
         *
         * The profile is in units of the column's own depth, so a sheet and a
         * tower are the same shape at two sizes rather than a tower and the
         * bottom third of one. Firm base, soft crown: the old profile was a
         * symmetric triangle about the slab's middle, which faded the underside
         * out over as many metres as the top — and the underside is the one
         * edge of a cloud that really is an edge, because it is where the air
         * stopped being saturated. */
        float hh = (p.z - cb) / (depth * (FLY_CLOUD_MIN + (1.0f - FLY_CLOUD_MIN) * cov));
        float prof = fly_smoothstepf(-0.10f, 0.08f, hh) * fly_smoothstepf(1.0f, 0.66f, hh);
        float dens = cov * prof;
        fly_v3 lit;
        float cs, a, crown;
        /* 0.01 was a 0.45% step in the finished pixel, which is a visible kink
         * at one elevation; this is under a twentieth of that and still skips
         * the empty taps */
        if (dens < 0.0005f) continue;
        /* self-shadow: how much cloud lies sunward of this sample */
        cs = cloud_cover_lod(e, p.x + e->sun.x * 620.0f,
                             p.y + e->sun.y * 620.0f, coarse_s, wide);
        /* and the two faces: what stands between this sample and the light is
           what is above it, so the shading it carries is scaled by how near the
           layer's own crown it sits. See FLY_CLOUD_FACE. */
        crown = fly_smoothstepf(FLY_CLOUD_FACE_LO, 1.0f, (p.z - cb) / depth); /*FACEKEY*/
        lit = fly_v3lerp(bright, dark,
                         fly_clampf((cs * 0.85f + dens * 0.25f) *
                                        (1.0f - FLY_CLOUD_FACE * crown),
                                    0, 1));
        /* Beer-Lambert over the segment, not a fixed share of the density —
           see the note above the function for what that costs and buys */
        a = 1.0f - expf(-dens * seg * (1.0f / FLY_CLOUD_EXT));
        ccol = fly_v3add(ccol, fly_v3scale(lit, (1.0f - acc) * a));
        acc += (1.0f - acc) * a;
    }
    if (acc <= 0.003f) return 0.0f;
    *out = fly_v3scale(ccol, 1.0f / acc);
    return acc * fade;
}

/* --- the ice sheet as one surface ----------------------------------------
 *
 * Where the ray crosses `FLY_CIRRUS_Z`, how much of it the sheet covers and
 * the colour it covers it with — the same contract `cloud_slab` has, so the
 * two layers composite the same way. See the constants for why this is a plane
 * and not a slab.
 *
 * The two things that make this hard are both about how far away it is. Seven
 * kilometres is three times the deck's height, so at any elevation the tap is
 * three times further out and slides three times faster per pixel: the ramps
 * are the deck's, at this field's scale, and without them the sheet reads as
 * horizontal stripes exactly the way the deck did before it had them. And the
 * haze fade is doing most of the work below five degrees — at three and a half
 * the sheet is 114 km away and `exp(-t/38000)` has already taken it to a
 * twentieth — which is why the elevation kink `render.atmosphere` watches
 * barely moves when this layer goes in. */
static float cirrus_sheet(const fly__env *e, fly_v3 ro, fly_v3 rd, float tmax,
                          fly_v3 *out) {
    float dz = fabsf(rd.z), t, fade, slant, sl, s2, coarse, wide, cov;
    *out = fly_v3zero();
    if (tmax <= 0.0f || dz < 1e-4f) return 0.0f;
    t = (FLY_CIRRUS_Z - ro.z) / rd.z;
    if (t <= 0.0f || t > tmax) return 0.0f;
    fade = expf(-t / 38000.0f);
    if (fade <= 0.002f) return 0.0f;
    sl = t / (dz * (e->pxscale + 1e-6f));
    s2 = sl * sl;
    coarse = s2 / (s2 + FLY_CIRRUS_FINE * FLY_CIRRUS_FINE);
    wide = s2 / (s2 + FLY_CIRRUS_FLAT * FLY_CIRRUS_FLAT);
    cov = cirrus_cover_lod(e, ro.x + rd.x * t, ro.y + rd.y * t, coarse, wide);
    if (cov <= 0.001f) return 0.0f;
    /* the slant path through it, capped: past about four degrees the geometry
       says the ray is in the sheet for a hundred kilometres, which is true of
       a plane and not of a cloud */
    slant = 1.0f / dz;
    if (slant > FLY_CIRRUS_SLANT) slant = FLY_CIRRUS_SLANT;
    *out = e->cirrus_lit;
    return (1.0f - expf(-cov * FLY_CIRRUS_TAU * slant)) * fade;
}

/* Which of the two layers the eye reaches first. Below the ice everything the
 * deck does is in front of it; above the ice it is the other way round, and
 * between them only one layer is ever on the ray at all. */
#define FLY_ICE_IS_FAR(ro) ((ro).z <= FLY_CIRRUS_Z)

static fly_v3 sky_sample(const fly__env *e, fly_v3 ro, fly_v3 rd, unsigned flags) {
    /* The gradient is the in-scattering integral out to space. Nothing here
     * says "blue at the top, warm at the bottom": a ray toward the zenith
     * crosses little air and keeps its Rayleigh blue, a ray toward the horizon
     * crosses a great deal and arrives red, and the band around a setting sun
     * is where the Mie lobe points. */
    fly_v3 vT, c;
    float s;
    float phit = fly__planet_hit(ro, rd, NULL);
    fly__scatter(e, ro, rd, FLY_SKY_FAR, &vT, &c);

    /* The disc and its glare are the sun seen *through* that same air, so they
     * redden on exactly the schedule the horizon does — the tint is the
     * transmittance, not a curve someone matched to it. */
    s = fly_v3dot(rd, e->sun);
    if (e->sun.z > -0.08f && !(flags & FLY_SKY_NOSUN)) {
        fly_v3 sunc = fly_v3scale(vT, FLY_SCAT_SUN);
        if (s > 0.9996f) c = fly_v3add(c, fly_v3scale(sunc, 0.55f));
        if (s > 0.0f) {
            float halo = powf(s, 180.0f) * 0.16f + powf(s, 10.0f) * 0.02f;
            c = fly_v3add(c, fly_v3scale(sunc, halo));
        }
    }

    /* stars fade in as the sun sets (stable per direction) */
    if ((flags & FLY_SKY_STARS) && e->sun.z < 0.06f && rd.z > 0.0f) {
        /* A star field on a lattice reads as graph paper.
         *
         * Three things were wrong with the old one and all three showed. The
         * cell was indexed by azimuth in radians against rd.z directly, which
         * are different angular scales, so every star came out stretched. The
         * star sat at the exact centre of its cell, so the whole sky was a
         * regular grid. And a single cell was tested, so a star near an edge
         * was cut in half by its own neighbour's cell.
         *
         * Both axes are radians now, the star is jittered inside its cell, and
         * the four nearest cells are tested so nothing is clipped. Colour
         * varies with the same hash: real stars are not all white. */
        /* Stars do not come out. They are there all day, and what decides
         * whether one is visible is whether the air in front of it is brighter
         * than it is — so that is what this is weighed against, the sky's own
         * radiance in this very direction rather than the clock.
         *
         * Keyed on the sun's elevation the whole field arrived at once and
         * arrived early: full brightness by seven degrees under, which is
         * civil twilight, and drawn at that brightness straight across the gold
         * band along the horizon. With the sky measured instead they come up
         * where a real first star does — at the zenith, where the air goes dark
         * first, while the sunset quarter still has none in it — and the same
         * expression keeps them out of a daylit sky for free.
         *
         * Squared, because a ratio that is linear in the sky leaves a third of
         * the field standing at sunset. `FLY_STAR_SKY` is the radiance at which
         * a star is half itself, and it is set at the twilight zenith rather
         * than at the night sky: at 0.012 the first of them show around eight
         * degrees under, which is where the first of them show. */
        float skyl = (c.x + c.y + c.z) * (1.0f / 3.0f);
        float ni = (FLY_STAR_SKY * FLY_STAR_SKY) /
                   (FLY_STAR_SKY * FLY_STAR_SKY + skyl * skyl);
        float u = atan2f(rd.y, rd.x) * 150.0f;
        float v2 = asinf(fly_clampf(rd.z, -1.0f, 1.0f)) * 150.0f;
        int iu = (int)floorf(u), iv = (int)floorf(v2), du, dv;
        for (dv = 0; dv <= 1; ++dv)
            for (du = 0; du <= 1; ++du) {
                uint32_t hsh = fly_hash2(e->seed + 7, iu + du, iv + dv);
                float jx, jy, dx, dy, fall, mag, warm;
                if ((hsh & 31u) != 0) continue;
                jx = (float)((hsh >> 16) & 255) / 255.0f;
                jy = (float)((hsh >> 24) & 255) / 255.0f;
                dx = u - ((float)(iu + du) + jx);
                dy = v2 - ((float)(iv + dv) + jy);
                fall = fly_clampf(1.0f - (dx * dx + dy * dy) * 11.0f, 0, 1);
                if (fall <= 0.0f) continue;
                mag = 0.35f + (float)((hsh >> 8) & 255) / 255.0f;
                warm = (float)((hsh >> 4) & 15) / 15.0f;   /* blue-white to amber */
                c = fly_v3add(c, fly_v3scale(fly_v3mk(0.72f + 0.34f * warm, 0.86f + 0.10f * warm,
                                                      1.05f - 0.30f * warm),
                                             fall * fall * mag * ni));
            }
    }

    /* The two cloud layers. Same march and same sheet the aerial perspective
     * runs over a surface ray — see cloud_slab and cirrus_sheet — with nothing
     * in front of them, so the whole of both out to the clamp is on the path.
     * The farther one goes down first, which below seven kilometres is the
     * ice: the deck is drawn over it, so a broken deck shows cirrus through
     * its gaps rather than the other way about. */
    if (!(flags & FLY_SKY_NOCLOUD)) {
        fly_v3 ccol, icol;
        float ca = cloud_slab(e, ro, rd, 400000.0f, &ccol);
        float ia = cirrus_sheet(e, ro, rd, 400000.0f, &icol);
        if (FLY_ICE_IS_FAR(ro)) {
            if (ia > 0.0f) c = fly_v3lerp(c, icol, ia);
            if (ca > 0.0f) c = fly_v3lerp(c, ccol, ca);
        } else {
            if (ca > 0.0f) c = fly_v3lerp(c, ccol, ca);
            if (ia > 0.0f) c = fly_v3lerp(c, icol, ia);
        }
    }
    /* And the surface at the far end, seen through all of that.
     *
     * Deliberately coarse: at these ranges a metre of terrain is far inside a
     * pixel, so this asks the same albedo function the ground under your wheels
     * uses, at a scale that resolves nothing but the biome. What sells it is
     * not the detail, it is the normal — Lambert against the sphere's own up,
     * so the day side falls away into a terminator across the face of the disc
     * instead of the whole thing taking one light like a coin.
     *
     * fly__scatter has already stopped the column at this same hit, so `vT` is
     * the transmittance over exactly the path being looked through and `c` is
     * the air in front of it. From a hundred and fifty kilometres that leaves
     * the surface coming through at 55-90%, blue-shifted, which is what the
     * model says and what a planet looks like from outside one.
     *
     * The rasterizer's own terrain rings reach about nineteen kilometres and
     * are drawn over the top of this, so near the sub-point you get real
     * ground and everywhere else you get the ball. The two meet inside the
     * haze, which is why the seam does not show. */
    if (phit > 0.0f && (flags & FLY_SKY_PLANET) && e->world) {
        fly_v3 n, hp = fly_v3add(ro, fly_v3scale(rd, phit));
        fly_v3 alb, lit;
        fly_v3 u = fly_world_ball_dir(e->world->frame, hp.x, hp.y);
        float gh = fly_world_ground(e->world, hp.x, hp.y), lam;
        float far = 1.0f - expf(-phit / 38000.0f);
        fly__planet_hit(ro, rd, &n);
        /* Water, climate, ice — see planet_albedo, and the cloud that is over
         * all of it once the deck's own march has faded out. */
        alb = planet_albedo(e->seed, u, gh);
        if (far > 0.002f)
            alb = fly_v3lerp(alb, fly_v3mk(0.62f, 0.64f, 0.67f),
                             planet_cloud(e, u) * far);
        lam = fly_v3dot(n, e->sun);
        /* Skylight belongs to the day side. `e->sh` is the sky over the
         * camera, which is the only sky the renderer has, and spreading it
         * over the whole ball lit the night face with the daylight of a place
         * a quarter of the planet away — a grey disc where a night side should
         * be, and a halo on the dark limb that `render.planet` reads as the
         * ring being only fifteen times brighter toward the sun than away from
         * it. The sun's own elevation at the point is what says how much sky
         * that point has, and it is already computed for the Lambert term. */
        lit = fly_v3scale(e->sunlight, lam > 0.0f ? lam : 0.0f);
        lit = fly_v3add(lit, fly_v3scale(sky_irradiance(e, n),
                                         FLY_SKY_IRR * 0.5f *
                                         fly_smoothstepf(-0.10f, 0.15f, lam)));
        c = fly_v3add(c, fly_v3mk(alb.x * lit.x * vT.x, alb.y * lit.y * vT.y,
                                  alb.z * lit.z * vT.z));
    }

    /* The storm grey is not applied here any more: it is in the air, in
     * fly__scatter, weighted by optical depth. A sky ray is 320 km of it and
     * saturates long before the end, so the horizon is greyed exactly as it
     * was — and the aerial perspective on a distant ridge now converges to
     * that same grey instead of overshooting it. */
    return c;
}

/* what a ray looking at the sky sees */
static fly_v3 sky_radiance(const fly__env *e, fly_v3 ro, fly_v3 rd) {
    return sky_sample(e, ro, rd, FLY_SKY_STARS | FLY_SKY_PLANET);
}
/* the sky as a light or fog source: the gradient without the star field */
static fly_v3 sky_ambient(const fly__env *e, fly_v3 ro, fly_v3 rd) {
    return sky_sample(e, ro, rd, 0u);
}

/* One layer folded into the pair: what covers the path attenuates the surface
 * and what the layer is lit with is added on. */
static void fog_layer(fly_v3 col, float a, fly_v3 *transmit, fly_v3 *inscat) {
    if (a <= 0.0f) return;
    if (transmit) *transmit = fly_v3scale(*transmit, 1.0f - a);
    if (inscat) *inscat = fly_v3add(fly_v3scale(*inscat, 1.0f - a), fly_v3scale(col, a));
}

/* How much of a colour survives `dist`, per channel, and what the air in front
 * of it adds. Blue is scattered out of a long path six times as hard as red,
 * which is why a far ridge goes blue rather than merely pale — a single scalar
 * fog factor cannot do that, and the scalar this replaced was also paying for a
 * whole sky lookup, cloud march and all, once per triangle, to find out what
 * colour to fade toward.
 *
 * The cloud deck is part of what is in front of it, and used to not be. The
 * slab was marched for sky rays and for nothing else, so with the camera inside
 * the layer a mountain top *in* cloud rendered crisp against a grey wash — the
 * one thing being inside cloud does not look like. The march stops at the
 * surface here rather than at the far clamp, which is what makes it a medium in
 * front of the ground instead of a backdrop behind it, and it folds into the
 * pair the same way the air does: what covers the path attenuates the surface
 * and what the cloud is lit with is added on. Callers that cross-fade a lit and
 * a dark colour through this stay exact, because both go through the same T
 * and the same s.
 *
 * It costs nothing where there is no cloud on the segment, which is most rays
 * most of the time: a ray from a camera below the deck to a hillside never
 * reaches 2340 m and `cloud_slab` returns on the interval test. */
static void fog_pair(const fly__env *e, fly_v3 ro, fly_v3 d, float dist,
                     fly_v3 *transmit, fly_v3 *inscat) {
    fly_v3 ccol, icol;
    float ca, ia;
    fly__scatter(e, ro, d, dist, transmit, inscat);
    ca = cloud_slab(e, ro, d, dist, &ccol);
    ia = cirrus_sheet(e, ro, d, dist, &icol);
    /* Same order as the sky: the farther layer is folded in first, so the
     * nearer one attenuates it. Only a ray that started above seven kilometres
     * — looking down at the ground from the top of a climb — ever has the ice
     * in front of the deck, and only such a ray has the ice on it at all. */
    if (FLY_ICE_IS_FAR(ro)) {
        fog_layer(icol, ia, transmit, inscat);
        fog_layer(ccol, ca, transmit, inscat);
    } else {
        fog_layer(ccol, ca, transmit, inscat);
        fog_layer(icol, ia, transmit, inscat);
    }
}

/* ---- one answer for a whole object -------------------------------------
 *
 * The rule above is about a triangle, and a triangle is not the coarsest thing
 * the air cannot tell apart. A tree at eight hundred metres is thirty triangles
 * inside a box a dozen metres across, and both of the expensive things the
 * shading asks about that box — how much of the colour survives the air in
 * front of it, and whether the cloud deck is over it — are the same number for
 * all thirty. `fog_tri` already shares one integral across a triangle; an
 * anchor shares it across everything drawn between `begin` and `end`.
 *
 * The air keeps the *same* bound the triangle rule uses — a hundredth of the
 * range — because measuring says that is the bound that matters. Two points
 * either side of a shared answer differ in two ways, in how far away they are
 * and in what bearing they are on, and only the second one costs anything: over
 * fifty metres of spread the error tracks the bearing almost exactly and
 * ignores the metres (29 degrees of it is 4.2 code values, 1.1 degrees is 1.1,
 * half a degree is 0.4), because the Mie phase function turns far faster with
 * angle than the column does with distance. So the group gets the triangle
 * rule, which keeps it no looser than what the renderer already accepts, and
 * gains an absolute cap on top of it: at a hundredth of the range the spread
 * would reach 120 m at the far edge of the scatter, and there the metres do
 * start to tell. `render.fog` holds both halves.
 *
 * A group outside the bound is not an error; it simply does not get to share,
 * and every triangle in it pays for itself exactly as before. That is what
 * makes this safe to open around anything: a settlement's whole footprint is
 * too big and falls back, a crown is not, and a crown close enough for its own
 * width to matter falls back too.
 *
 * The cloud tap takes the cap alone and no angular bound, because it is a
 * different question. It is a lookup in a field whose features are kilometres
 * across, not an integral along the ray, so what it costs is set by the spread
 * in metres and nothing else — the terrain rings have interpolated it across a
 * 26 m cell since they were written.
 *
 * One anchor at a time, opened and closed by a wrapper around the model it
 * holds, so no path through that model can leave one installed. Not thread-safe
 * and does not need to be, for the same reason the heightfield memos are not:
 * every caller is an object builder on the main thread, and the path tracer —
 * the one threaded part of the renderer — evaluates its own scattering along
 * each ray and never comes here. */
#define FLY_FOG_ANCHOR_R 26.0f

static struct {
    int open;      /* an anchor is installed */
    int cap;       /* small enough in metres: the cloud tap may be shared */
    fly_v3 c;      /* its centre */
    float r;       /* and the radius of what it holds */
    int have_air;  /* the integral has been taken */
    fly_v3 T, S;
    int have_cloud;
    float cloud;
} g_anchor;
static int g_fog_per_tri; /* test hook: refuse every anchor — see the header */

int fly_render_fog_per_tri(int on) {
    int was = g_fog_per_tri;
    g_fog_per_tri = on != 0;
    return was;
}

/* `r` is the radius of everything the caller is about to draw. */
static void fog_anchor_begin(fly_v3 c, float r) {
    g_anchor.open = 1;
    g_anchor.cap = !g_fog_per_tri && r <= FLY_FOG_ANCHOR_R;
    g_anchor.c = c;
    g_anchor.r = r;
    g_anchor.have_air = 0;
    g_anchor.have_cloud = 0;
}

static void fog_anchor_end(void) { g_anchor.open = 0; }

/* The anchor's aerial perspective, if it is entitled to one: 1 when `fT`/`fS`
 * have been filled for all three corners, 0 to let fog_tri do its own work.
 *
 * The angular half of the bound is settled here rather than in `begin` because
 * it needs the range, and the range is not known until something is actually
 * being fogged. Settled once and remembered for the rest of the object —
 * `have_air` is a tri-state — so a crown does not re-ask it thirty times to get
 * the same no. */
static int fog_anchor_take(const fly__env *e, const fly__view *v,
                           fly_v3 *fT, fly_v3 *fS) {
    if (!g_anchor.open || !g_anchor.cap || g_anchor.have_air < 0) return 0;
    if (!g_anchor.have_air) {
        fly_v3 d = fly_v3sub(g_anchor.c, v->pos);
        float dist = fly_v3len(d);
        /* the triangle rule, asked about the group: too near for its own width
           and the anchor stands down for everything in it */
        if (g_anchor.r > dist * 0.01f) {
            g_anchor.have_air = -1;
            return 0;
        }
        fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist,
                 &g_anchor.T, &g_anchor.S);
        g_anchor.have_air = 1;
    }
    fT[0] = fT[1] = fT[2] = g_anchor.T;
    fS[0] = fS[1] = fS[2] = g_anchor.S;
    return 1;
}

/* The cloud deck over the anchor, or over `p` when there is no anchor. */
static float cloud_shadow_at(const fly__env *e, fly_v3 p) {
    if (!g_anchor.open || !g_anchor.cap) return cloud_shadow(e, p);
    if (!g_anchor.have_cloud) {
        g_anchor.cloud = cloud_shadow(e, g_anchor.c);
        g_anchor.have_cloud = 1;
    }
    return g_anchor.cloud;
}

/* The aerial perspective over a whole triangle, per corner or once for all
 * three.
 *
 * The scattering integral is the most expensive thing a shaded vertex does —
 * four Chapman columns and half a dozen exponentials — and on a wooded frame
 * it is most of what the object pass spends: a distant tree is one to a dozen
 * triangles a couple of metres across, and the air in front of its three
 * corners is the same air. Evaluating it three times there is not accuracy,
 * it is arithmetic.
 *
 * Whether once will do is a question about the triangle and not about the
 * graphics level, so it is the triangle that is measured: what fog does across
 * a primitive is a function of how much the *range* and the *bearing* change
 * across it, and both are bounded by how big it is next to how far away it is.
 * Inside a hundredth of its own range — a crown, a windsock, a fence post,
 * every leaf card in the wood — the three answers agree to far better than a
 * quantisation step and one is shared. A runway, a pier or a ridge running
 * away from the eye is not, and still pays per corner, which is the case the
 * shortcut would actually show in: a gradient down a long surface is exactly
 * what aerial perspective is for.
 *
 * `p` are the three corners; `fT` and `fS` receive each one's transmittance
 * and in-scatter, so the caller's per-vertex loop is unchanged. */

static void fog_tri(const fly__env *e, const fly__view *v, const fly_v3 *p,
                    fly_v3 *fT, fly_v3 *fS) {
    fly_v3 mid, md;
    float mdist, lim, r2 = 0.0f;
    int i;
    if (fog_anchor_take(e, v, fT, fS)) return;
    mid = fly_v3scale(fly_v3add(fly_v3add(p[0], p[1]), p[2]), 1.0f / 3.0f);
    md = fly_v3sub(mid, v->pos);
    mdist = fly_v3len(md);
    lim = mdist * 0.01f;
    for (i = 0; i < 3; ++i) {
        float dx = p[i].x - mid.x, dy = p[i].y - mid.y, dz = p[i].z - mid.z;
        float q = dx * dx + dy * dy + dz * dz;
        if (q > r2) r2 = q;
    }
    if (r2 <= lim * lim) {
        fly_v3 T, S;
        fog_pair(e, v->pos, fly_v3scale(md, 1.0f / (mdist + 1e-5f)), mdist, &T, &S);
        fT[0] = fT[1] = fT[2] = T;
        fS[0] = fS[1] = fS[2] = S;
        return;
    }
    for (i = 0; i < 3; ++i) {
        fly_v3 d = fly_v3sub(p[i], v->pos);
        float dist = fly_v3len(d);
        fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist, &fT[i], &fS[i]);
    }
}

/* ---------------- terrain material ---------------- */

/* meadow species region: -1 heather moor .. 0 lush green .. +1 dry golden */
static float grass_region(uint32_t seed, float x, float y) {
    return fly_noise2(seed + 34, x / 2600.0f, y / 2600.0f);
}

/* fade a feature in over the span where it grows from 5 to 13 pixels across */
static float detail_amount(float world_size, float px_per_m) {
    return fly_smoothstepf(5.0f, 13.0f, world_size * px_per_m);
}

/* --- where the chart gives way to the ball ------------------------------
 *
 * The LOD rings, the water, the scatter, the settlements and the rail are one
 * thing: a description of the ground you fly over, drawn on a flat 38 km
 * square of chart centred under the camera. It is the right description from
 * inside the air and the wrong one from outside it, and the failure is not
 * subtle — from a hundred and fifty kilometres up the square is a tile of a
 * different world laid over the planet, with a straight edge on all four sides
 * and trees on it a sixtieth of a pixel across.
 *
 * The number this is keyed on is the camera's altitude, and that is not a
 * stand-in for distance or for a pixel rate. The chart is a *plane*: at range
 * d it stands d^2/2R above the sphere it is a map of. What matters is how much
 * of what you can see it accounts for, and that is a question about altitude
 * alone — the ground you can see reaches sqrt(2Rz), so the 19 km ring is a
 * fifth of the visible radius from eight kilometres up, a ninth from
 * twenty-five and a fifteenth from sixty.
 *
 * The band is 10 to 26 km. An air-breathing airframe with its nose up tops out
 * around ten with the tanks full, so ordinary flight sits at the bottom of it
 * and sees a few per cent; a rocket climbing out crosses it in about fifteen
 * seconds; and everything in space is past the top, where the near field is
 * neither drawn nor missed. It was first set at 25 to 60 and that was simply
 * too high: at thirty kilometres the square was the most obvious thing in the
 * frame, a pastel tile with a straight edge on all four sides laid over a
 * planet, and the fade had not started.
 *
 * One number a frame, in `fly__env`, because the ring under the camera spans
 * 19 km against an altitude of tens: keying it per pixel would say the same
 * thing at ten times the cost, and would also collide with the pixel-rate LOD
 * ladder, which is about what a pixel can resolve rather than about which
 * surface the ground is on. */
static float chart_gives_way(float altitude) {
    return fly_smoothstepf(10000.0f, 26000.0f, altitude);
}

static int near_field_worth_drawing(const fly__env *e) {
    return e->ball < 1.0f;
}

/* Terrain colour and the material it is made of, in one pass over the same
 * noise. `mat` comes back as (vegetation, normal-incidence reflectance, Blinn
 * exponent) and is carried through exactly the same chain of mixes as the
 * colour, so the two can never disagree about what a point is made of.
 *
 * Vegetation is worth knowing about because grass does not shade like a green
 * plane — see terrain_light. Reflectance and exponent are worth knowing about
 * because a wet shore, a snowfield and a bare rock face differ far more in how
 * they catch the sun than in what colour they are, and until this the renderer
 * had one answer for all three. */
/* Plane mean of smoothstep(0.55, 0.8, fly_noise2), measured the same way as
 * FLY_NOISE_MS: what a patch of ground averages out to once its dirt patches
 * are too small to make out individually. */
#define FLY_DIRT_MEAN 0.0622f
/* The same number for the worn-earth ribbons below: plane mean of
 * smoothstep(0.93, 1.0, 1 - |fly_noise2|), which is 5.6% of the ground. */
#define FLY_PATH_MEAN 0.0562f
/* And for standing water: plane mean of smoothstep(0.34, 0.70, fly_noise2). */
#define FLY_POOL_MEAN 0.1345f
/* What a hedge covers, as a fraction of the worked ground it encloses: the
 * band within FLY_HEDGE_W of a parcel boundary, ramped, measured the same way
 * as the means above — over four seeds and 640k samples of farmland it is
 * 0.0492 of it. A parcel is a couple of hundred metres across and a hedge is
 * four wide, so once a pixel outgrows the hedge the honest answer for the
 * ground it covers is this much hedge everywhere rather than a ribbon
 * resampled at random. */
#define FLY_HEDGE_MEAN 0.0492f
#define FLY_HEDGE_W 4.2f

/* --- what is standing in a field -----------------------------------------
 *
 * Four crops in the rotation, chosen by the parcel's own value, and the whole
 * reason worked ground reads from ten kilometres up: a wood is one colour with
 * texture in it and a parish is four colours in hard-edged blocks, so the eye
 * separates them at any range where the blocks are bigger than a pixel.
 *
 * `veg` is the vegetation channel of the material, which is what the shading
 * needs to know: a ploughed field is soil and catches the sun like soil, and a
 * standing crop is a canopy and does not.
 *
 * Bands rather than a ramp, because a field is uniform and its neighbour is
 * not — a gradient across the parcel grid is a wash and reads as haze. The
 * within-band tint is taken off the same value scaled up and wrapped, so no
 * two fields of the same crop are quite the same colour without a second noise
 * tap, and it averages to exactly 1 so the band means below stay true. */
static fly_v3 crop_colour(float crop, float *veg) {
    fly_v3 c;
    float tint = 0.86f + 0.28f * (crop * 7.0f - floorf(crop * 7.0f));
    if (crop < 0.28f) { c = fly_v3mk(0.23f, 0.16f, 0.10f); *veg = 0.00f; }
    else if (crop < 0.50f) { c = fly_v3mk(0.45f, 0.38f, 0.15f); *veg = 0.55f; }
    else if (crop < 0.74f) { c = fly_v3mk(0.14f, 0.28f, 0.09f); *veg = 1.00f; }
    else { c = fly_v3mk(0.36f, 0.33f, 0.18f); *veg = 0.35f; }
    return fly_v3scale(c, tint);
}

/* And what a parish averages out to once the parcels are too small to tell
 * apart. Not the mean of the four colours: the parcel value is a noise sample
 * and its distribution is not flat, so the bands are weighted by how much of
 * the worked ground each actually covers — 0.202, 0.322, 0.323 and 0.152 over
 * four seeds — and the same weights give the vegetation channel's mean. */
#define FLY_CROP_MEAN fly_v3mk(0.291f, 0.295f, 0.125f)
#define FLY_CROP_MEAN_VEG 0.5533f

static fly_v3 terrain_surface(const fly_world *w, float x, float y, float h, float slope,
                              float px_per_m, float ball, float rain, fly_v3 *mat) {
    /* (vegetation, f0, blinn exponent) per material */
    const fly_v3 m_grass = { 1.0f, 0.020f, 14.0f };
    const fly_v3 m_dirt = { 0.0f, 0.030f, 18.0f };
    const fly_v3 m_rock = { 0.0f, 0.035f, 26.0f };
    const fly_v3 m_snow = { 0.0f, 0.055f, 46.0f };
    const fly_v3 m_sand = { 0.0f, 0.030f, 18.0f };
    fly_v3 m = m_grass;
    /* The level of the water this ground is beside, taken once. Everything
     * below that used to be written against sea level is written against this
     * instead: the sand under the waterline, the wet band above it, the
     * treeline. A river three hundred metres up has a shore, and a beach
     * measured off the sea would put it on the sea floor. The *shore* level
     * rather than the drawn surface — see fly_world_shore for why the two are
     * not the same number at the edge of a body of water. */
    float wz = fly_world_shore(w, x, y);
    /* Each octave fades toward the mean of what it contributes rather than
     * being carried at full strength past the point it can be resolved.
     * Point-sampling it there is not a blur, it is a bias: the mixes below are
     * nonlinear, so a sample of the noise and the average of the noise give
     * different colours, and the far ground came out a different colour from
     * the near ground rather than a smoother version of it.
     *
     * A faded octave is also a skipped one. detail_amount is a smoothstep, so
     * it reaches exactly zero rather than approaching it, and below that the
     * tap can be dropped for precisely the value it would have produced — which
     * is what pays for shading every ring in the fragment stage. */
    float wf = detail_amount(47.0f, px_per_m);
    float wm = detail_amount(12.0f, px_per_m);
    float wd = detail_amount(90.0f, px_per_m);
    float mottle = 0.5f + 0.5f * fly_noise2(w->seed + 31, x / 380.0f, y / 380.0f);
    float fine = 0.5f, micro = 0.5f;
    if (wf > 0.0f) fine += 0.5f * wf * fly_noise2(w->seed + 32, x / 47.0f, y / 47.0f);
    if (wm > 0.0f) micro += 0.5f * wm * fly_noise2(w->seed + 36, x / 12.0f, y / 12.0f);
    /* grass species vary by region: lush lowland, dry golden savanna,
     * violet-tinged heather moor */
    float region = grass_region(w->seed, x, y);
    fly_v3 lush = fly_v3lerp(fly_v3mk(0.10f, 0.26f, 0.12f), fly_v3mk(0.24f, 0.30f, 0.12f), mottle);
    fly_v3 dry = fly_v3lerp(fly_v3mk(0.30f, 0.26f, 0.10f), fly_v3mk(0.38f, 0.31f, 0.13f), mottle);
    fly_v3 heather = fly_v3lerp(fly_v3mk(0.14f, 0.17f, 0.12f), fly_v3mk(0.22f, 0.16f, 0.20f), mottle);
    fly_v3 grass = lush;
    grass = fly_v3lerp(grass, dry, fly_smoothstepf(0.12f, 0.5f, region));
    grass = fly_v3lerp(grass, heather, fly_smoothstepf(-0.18f, -0.55f, region));
    grass = fly_v3scale(grass, (0.85f + 0.3f * fine) * (0.9f + 0.2f * micro));
    /* The grain: the variance the faded octaves took with them, put back at a
     * wavelength that is always about a dozen pixels across.
     *
     * Fading an octave toward its mean is the right way to stop it aliasing
     * and it is also why distant ground goes velvet: by two kilometres both
     * the 47 m and the 12 m octaves are gone and what is left is a smooth
     * gradient. The energy has to go somewhere or the surface stops looking
     * like a surface. It goes into one more sample whose wavelength is tied to
     * the projection rather than to the world — 14 px worth of metres, so it
     * is resolvable by construction at any range and cannot alias — carrying
     * exactly the amplitude the octaves above lost.
     *
     * Amplitudes come straight off the two lines above: fine reaches the
     * colour as 1 + 0.15*wf*n and micro as 1 + 0.10*wm*n, so the variance
     * missing at this distance is 0.15^2(1 - wf^2) + 0.10^2(1 - wm^2). Near
     * the camera nothing has faded, the term is zero, and the noise call is
     * skipped rather than multiplied out. */
    {
        float lost = 0.0225f * (1.0f - wf * wf) + 0.01f * (1.0f - wm * wm);
        if (lost > 1e-5f && px_per_m > 1e-6f) {
            /* Capped, because a wavelength tied to the projection runs away
             * once the projection does: at the pixel scale a coverage test
             * uses to force full fading, 14 px is three kilometres of ground,
             * and a "grain" that size is not detail, it is a bias on the
             * answer. Sixty metres still lands inside fourteen pixels at every
             * distance the world actually spans, so the cap costs nothing that
             * can be seen and keeps the term honestly zero-mean over any patch
             * big enough to average. */
            float lam = 14.0f / px_per_m;
            if (lam > 60.0f) lam = 60.0f;
            /* And where the cap has taken the wavelength below what the
             * caller can sample, the grain cannot be a grain.
             *
             * The whole design of this term is that it is always about a dozen
             * pixels across and therefore always resolvable; the cap breaks
             * that as soon as a pixel outgrows sixty metres, which is 125 m a
             * pixel at thirty kilometres and 620 m at a hundred and fifty. A
             * 60 m noise read at that rate is not texture, it is a different
             * random number per sample — and on the CPU rasterizer, which
             * resolves one colour per vertex, "per sample" means per 950 m
             * cell of the outermost ring. That is what turned the near field
             * into a pastel quilt seen from above the air: not a colour
             * mismatch with the ball, a field of noise. Two samples per
             * wavelength is the least that can carry one, and below it the
             * honest amount to add back is none. */
            if (lam * px_per_m < 2.0f) lam = 0.0f;
            float gn = lam > 0.0f ? fly_noise2(w->seed + 37, x / lam, y / lam) : 0.0f;
            /* Not all of it, and the fraction is not a taste setting.
             *
             * Handing back the whole lost amplitude undoes the reason the
             * octave was faded: fading exists so that a sample lands nearer
             * the truth for the patch it covers than a point sample does, and
             * a grain carrying the full variance gives that up. How much may
             * come back is therefore set by the guarantee, not by eye —
             * `render.material` requires the faded answer to sit at least
             * twice as close to the patch mean as point-sampling, and most of
             * that headroom is already spent by the coarse octaves this does
             * not touch: with no grain at all the error is 0.0069 against a
             * point sample's 0.0179, so everything available lies between
             * 0.0069 and 0.00895. Measured, 0.55 gives 0.0099, 0.42 gives
             * 0.0092, 0.35 gives 0.0090 — all over. 0.30 gives 0.0089 and is
             * the most that fits.
             *
             * So distant ground carries about a third of the contrast it lost,
             * which is less than near ground and is the right direction
             * anyway. What it no longer is, is velvet. */
            grass = fly_v3scale(grass, 1.0f + sqrtf(lost) * gn * 0.30f);
        }
    }
    /* --- the worked ground ------------------------------------------------
     *
     * Everything above this line is a landscape nobody has touched. This is
     * the one term in the function that is not a noise field: where a
     * settlement works the ground, the ground is divided into parcels with
     * straight edges, each carrying a crop of its own, with a hedge or a wall
     * along the boundary. Enclosure is what makes a country read as inhabited
     * from cruise altitude, where a building is two pixels and a field is
     * fifty.
     *
     * Taken here rather than at the end because two of the terms below have to
     * give way to it: the worn-earth ribbons do not run through a standing
     * crop, and neither does the needle litter. See fly_world_tilth, which is
     * where the belt and the parcel frame are decided, and fly_tilth in
     * fly_glsl.h, which is the shader's copy of it. */
    fly_tilth ti;
    fly_world_tilth(w, x, y, h, &ti);
    /* worn bare-dirt patches; too small to tell apart, they are just a wash */
    float dirt = FLY_DIRT_MEAN;
    if (wd > 0.0f)
        dirt = fly_lerpf(dirt, fly_smoothstepf(0.55f, 0.8f,
                                               fly_noise2(w->seed + 35, x / 90.0f, y / 90.0f)), wd);
    /* --- the tracks, and what is under the turf where it has gone ----------
     *
     * The patch field above is blobs, and blobs are what a meadow's *bald*
     * spots look like; what it cannot make is a path. A path is a line, and
     * the cheapest field whose level sets are lines rather than islands is
     * ridged noise — 1 - |n| — thresholded near its ridge. One tap buys a
     * network of winding bare-earth tracks a few tens of metres wide that
     * joins up across kilometres, which is the single thing that stopped the
     * open ground reading as one colour with speckle on it.
     *
     * Faded toward its own plane mean the same way the patches are, and for
     * the same reason: it is a nonlinear function of a noise tap, so past the
     * range where the ridge is resolvable a point sample is a bias rather than
     * a blur. See the octave bookkeeping above.
     *
     * Soil and gravel are one material with two colours and a grain between
     * them: the ribbons run to bare soil in the middle and to a paler, grittier
     * shoulder at the edges, which is what a track that has been driven on
     * looks like from anywhere. The grain is zero-mean and fades to nothing, so
     * it costs nothing at range and needs no mean of its own. */
    float path = FLY_PATH_MEAN;
    {
        float wp = detail_amount(340.0f, px_per_m);
        if (wp > 0.0f)
            path = fly_lerpf(path,
                             fly_smoothstepf(0.93f, 1.0f,
                                             1.0f - fabsf(fly_noise2(w->seed + 41,
                                                                     x / 340.0f, y / 340.0f))),
                             wp);
    }
    /* A track goes round a field, not through it: the ribbons and the bald
     * patches both give way to whatever is being grown. */
    path *= 1.0f - ti.work * 0.92f;
    dirt *= 1.0f - ti.work * 0.75f;
    {
        float bare = dirt * 0.7f + path * (1.0f - dirt * 0.7f);
        fly_v3 soil = fly_v3mk(0.30f, 0.24f, 0.16f);
        /* the shoulder: paler, coarser, and only where the ribbon is */
        soil = fly_v3lerp(soil, fly_v3mk(0.37f, 0.34f, 0.29f), path * 0.55f);
        float grit = detail_amount(2.4f, px_per_m);
        if (grit > 0.0f)
            soil = fly_v3scale(soil, 1.0f + 0.26f * grit * bare *
                                             fly_noise2(w->seed + 43, x / 2.4f, y / 2.4f));
        grass = fly_v3lerp(grass, soil, bare);
        m = fly_v3lerp(m, m_dirt, bare);
    }
    /* --- the crop, and the hedge round it ----------------------------------
     *
     * The parcel colour fades toward the parish mean on projected size like
     * every other octave here, and for the same reason: past the range where a
     * field is a pixel across, a sample of one parcel is a bias and the
     * average of them all is the answer. What it does *not* do is fade out —
     * the enclosure survives to the horizon as a wash of crop colour, which is
     * what a farmed country looks like from twenty kilometres.
     *
     * The hedge is a line and fades on its own much smaller size, toward the
     * fraction of the ground it covers. Green where the grass is lush and
     * stone where it is not: a wall is what a parish builds where a hedge will
     * not grow, and the region field already knows which is which. */
    if (ti.work > 0.0f) {
        float wc = detail_amount(FLY_PARCEL_MIN, px_per_m);
        float wh = detail_amount(FLY_HEDGE_W * 2.0f, px_per_m);
        float veg = FLY_CROP_MEAN_VEG;
        fly_v3 crop = FLY_CROP_MEAN;
        float hedge = FLY_HEDGE_MEAN;
        if (wc > 0.0f) {
            float cv;
            fly_v3 cc = crop_colour(ti.crop, &cv);
            crop = fly_v3lerp(crop, cc, wc);
            veg = fly_lerpf(veg, cv, wc);
        }
        if (wh > 0.0f)
            hedge = fly_lerpf(hedge,
                              1.0f - fly_smoothstepf(1.1f, FLY_HEDGE_W, ti.edge), wh);
        grass = fly_v3lerp(grass, crop, ti.work * 0.90f);
        m = fly_v3lerp(m, fly_v3mk(veg, 0.024f, 16.0f), ti.work * 0.90f);
        {
            fly_v3 hcol = fly_v3lerp(fly_v3mk(0.085f, 0.135f, 0.062f),
                                     fly_v3mk(0.31f, 0.30f, 0.27f),
                                     fly_smoothstepf(0.10f, 0.48f, region));
            float hm = ti.work * hedge * 0.85f;
            grass = fly_v3lerp(grass, hcol, hm);
            m = fly_v3lerp(m, fly_v3mk(0.75f, 0.026f, 20.0f), hm);
        }
    }
    /* Needle litter and canopy shade, off the same field the scatter plants
     * stands from. A wood whose floor is the same green as the meadow beside
     * it reads as trees standing on a lawn, and between the crowns the floor
     * is most of what a forest shows you from the air. The settlement clearing
     * term is deliberately absent here and in the GLSL twin — a shader cannot
     * walk the location list, and the ground inside a town is decked over. */
    {
        float fmask = fly_world_forest_mask(w, x, y);
        fmask *= fly_smoothstepf(wz + 1.0f, wz + 8.0f, h);
        fmask *= 1.0f - fly_smoothstepf(640.0f, 990.0f, h);
        /* and the wood gives way to the plough here exactly as it does in the
         * field the scatter plants from — see forest_open, whose ramp this is */
        fmask *= 1.0f - fly_smoothstepf(0.10f, 0.45f, ti.work);
        grass = fly_v3lerp(grass, fly_v3mk(0.075f, 0.098f, 0.055f), fmask * 0.88f);
        m = fly_v3lerp(m, m_dirt, fmask * 0.35f);
    }
    fly_v3 c = grass;
    float rockness = fly_smoothstepf(0.25f, 0.55f, slope);
    float high = fly_smoothstepf(900.0f, 1600.0f, h) * 0.7f;
    /* Level lowland is most of the world and none of it is stone, so the rock
       branch — its own noise tap and a sine for the strata — is skipped where
       neither the slope nor the altitude asks for any. Both are smoothsteps
       that reach exactly zero, so this changes nothing it does not skip. */
    if (rockness + high > 0.0f) {
        fly_v3 rock = fly_v3lerp(fly_v3mk(0.26f, 0.22f, 0.19f), fly_v3mk(0.36f, 0.33f, 0.30f), fine);
        /* sedimentary strata banding on exposed rock faces */
        float strata = 0.5f + 0.5f * sinf(h * 0.05f +
                                          fly_noise2(w->seed + 33, x / 850.0f, y / 850.0f) * 2.4f);
        rock = fly_v3scale(rock, 0.82f + 0.18f * strata);
        c = fly_v3lerp(c, rock, rockness);
        m = fly_v3lerp(m, m_rock, rockness);
        c = fly_v3lerp(c, rock, high);
        m = fly_v3lerp(m, m_rock, high);
    }
    if (h > 1850.0f) {
        float snow = fly_smoothstepf(1850.0f, 2250.0f, h) *
                     (1.0f - fly_smoothstepf(0.45f, 0.75f, slope));
        c = fly_v3lerp(c, fly_v3mk(0.86f, 0.89f, 0.94f), snow);
        m = fly_v3lerp(m, m_snow, snow);
    }
    if (h < wz + 1.6f) {
        float t = fly_smoothstepf(wz + 0.2f, wz + 1.6f, h);
        c = fly_v3lerp(FLY_WATER_BED, c, t);
        m = fly_v3lerp(m_sand, m, t);
    }
    /* Wet darkening just above the waterline, and the reason it reads as wet:
     * a film of water over sand or rock is a smooth dielectric, so the band
     * takes a tight bright highlight the dry ground beside it does not. The
     * darkening alone only ever looked like a dirtier stripe of sand. */
    float wet = fly_smoothstepf(wz + 2.6f, wz + 0.3f, h);
    c = fly_v3scale(c, 1.0f - 0.35f * wet);
    m = fly_v3lerp(m, fly_v3mk(m.x, 0.10f, 200.0f), wet);
    /* --- rain on the ground ------------------------------------------------
     *
     * The weather drew streaks across the lens and stopped there, so it rained
     * on a landscape that stayed as dry as it was at noon. Water on a surface
     * does two things and neither of them is a streak: it darkens the albedo,
     * because a film fills the pores and light that would have scattered
     * straight back out of them takes another bounce first; and it replaces a
     * rough surface with a smooth dielectric one, which is what actually reads
     * as wet — a tight highlight and the sky reflected in it, exactly the pair
     * the shore band above already uses.
     *
     * Grass darkens least: a blade is already smooth and the film runs off it.
     * Soil and rock darken most. `m.x` is vegetation, so it is the split.
     *
     * And then the water that does not run off. A puddle is standing water and
     * therefore only ever on ground flat enough to hold it, which is why the
     * mask is gated on slope before anything else — a mirror on a hillside is
     * the one thing that would give this away instantly. Where it stands, the
     * surface is water: dark, near-mirror, and lit by the sky it reflects
     * rather than by its own albedo, which the reflection term in
     * `terrain_light` already supplies for any material with a high enough
     * exponent. Faded on the projected size of a puddle like every other
     * octave here, so at range it stops being sampled rather than aliasing.
     *
     * Neither applies under the waterline (already water), on snow (already
     * something else), or when it is not raining, and the whole block is
     * skipped in the dry case rather than multiplied out by zero. */
    if (rain > 0.002f && h > FLY_WATER_Z + 0.6f) {
        float dry = 1.0f - fly_smoothstepf(1850.0f, 2250.0f, h);   /* not on snow */
        float damp = rain * dry;
        float pool;
        c = fly_v3scale(c, 1.0f - 0.34f * damp * (1.0f - 0.55f * m.x));
        /* Toward water's own reflectance, and that is downward.
         *
         * The film is the outer interface once it is there, and water is
         * n = 1.33, which is F0 = 0.02 — below the mineral it covers rather
         * than above it. What this used to lerp to, 0.048, is F0 for n = 1.56,
         * which is the Fresnel of polished plastic, and that is precisely what
         * a rained-on meadow read as. Nothing about a wet surface is a
         * reflectance that goes up: what makes it wet is the roughness
         * collapsing and the albedo going down with it.
         *
         * And how far the exponent may climb is a question about what the film
         * is lying on, exactly as the darkening on the line above is. That was
         * the one place the split was missing. Soil and rock under a film are
         * a smooth dielectric and belong at 85. A canopy is not a sheet: each
         * blade is its own tilted mirror, the aggregate stays rough at every
         * scale this shades at, and taking grass to 85 puts a lobe on it tight
         * enough to resolve the 7.4 m hummocks `terrain_detail` perturbs the
         * normal with — a bump authored to break up a matte diffuse, rendered
         * as a specular relief map. Measured over the city pad, that left a
         * rained-on meadow shading 1.24x *brighter* than the same meadow dry,
         * off a 0.78x albedo. It is 0.88x now, and bare ground is unmoved. */
        m.y = fly_lerpf(m.y, 0.021f, damp * 0.85f);
        m.z = fly_lerpf(m.z, fly_lerpf(85.0f, 20.0f, m.x), damp * 0.85f);
        pool = damp * (1.0f - fly_smoothstepf(0.05f, 0.17f, slope));
        if (pool > 0.002f) {
            float wp = detail_amount(11.0f, px_per_m);
            float k = wp > 0.0f
                          ? fly_lerpf(FLY_POOL_MEAN,
                                      fly_smoothstepf(0.34f, 0.70f,
                                                      fly_noise2(w->seed + 44,
                                                                 x / 11.0f, y / 11.0f)), wp)
                          : FLY_POOL_MEAN;
            pool *= k;
            /* Both go most of the way to water, and the reflectance is
             * water's own — 0.055 is glass, and a puddle carrying it read as a
             * pale grey patch of paint rather than as a hole with sky in it.
             *
             * Held back from taking the albedo down before this, on the
             * grounds that a puddle seen steeply from above has little Fresnel
             * to brighten it and went to a black stain that read as a scorch
             * mark. That was true and it was not this term's fault: the meadow
             * around it was rendering a quarter brighter than dry ground, so
             * anything as dark as water actually is read as a hole burnt in
             * it. With the surrounding damp ground shading darker than dry,
             * which is the way round it belongs, the puddle can be as dark as
             * a puddle. */
            c = fly_v3lerp(c, fly_v3mk(0.030f, 0.036f, 0.040f), pool * 0.88f);
            m = fly_v3lerp(m, fly_v3mk(m.x * 0.2f, 0.021f, 240.0f), pool * 0.9f);
        }
    }
    /* And the last thing to give way is the chart itself.
     *
     * Every octave above fades toward its own mean once a pixel outgrows it.
     * This is not one of those: it is the crossover from the flat chart's own
     * albedo to the ball's — `planet_albedo`, the same tones the limb is drawn
     * with — and it is keyed on the camera's altitude rather than on a pixel
     * rate, because which surface the ground is on is not a question about
     * resolution. See chart_gives_way. What it buys is that the 38 km square
     * the rasterizer draws under the camera stops being a square: by the time
     * the rings are dropped they are already the colour of the planet under
     * them, so the join is nowhere in particular rather than at an edge.
     *
     * `m` is deliberately not faded with it: how a surface catches the sun is
     * a claim about a surface, and at this altitude the ball's own Lambert
     * term is the only one that survives anyway. */
    if (ball > 0.002f)
        c = fly_v3lerp(c, planet_albedo(w->seed, fly_world_ball_dir(w->frame, x, y), h), ball);
    if (mat) *mat = m;
    return c;
}

/* Relief and colour at the scale you are standing on.
 *
 * The heightfield's finest sample is metres across and the albedo's finest
 * noise was twelve, so the ground within a hundred metres of the camera was one
 * flat colour on one flat normal — a painted plane, which is exactly what it
 * looked like. Three scales fix that:
 *
 *  - Relief, as a bump rather than as geometry. Two octaves — hummocks at
 *    7.4 m and tussocks at 2.3 m — summed into one height field and
 *    differenced once, so six taps buy a two-scale gradient instead of the
 *    nine three separate gradients would cost. One octave alone is worse than
 *    none in the middle distance: a single frequency at a fixed amplitude
 *    reads as a regular stipple laid over the ground rather than as ground.
 *  - Blades, ~0.55 m. Colour only, one tap, and a hue shift rather than a
 *    brightness one — dry blades stand yellow among green, which is most of
 *    what breaks up a lawn. A gradient here would buy relief you cannot
 *    resolve at the range where it is visible, for two more taps.
 *
 * Each scale fades on how many pixels it covers, not on distance. Distance is
 * the wrong variable: the same tussock is four pixels wide at 320 across and
 * twenty-four at 1920, so a fade tuned at one resolution shimmers at the other
 * — and supersampling, which should let *more* detail through, would get less.
 * Fading on projected size lets each scale live exactly as far out as it can be
 * resolved, which is both the sharpest and the cheapest place to stop.
 *
 * Returns an albedo tint and perturbs `n`. The caller must pass the *geometric*
 * normal to terrain_surface first: feeding it a tussock-tilted one turns the
 * far side of every clump into rock.
 *
 * It takes the whole material and not just the vegetation because standing
 * water is level. Relief here is a claim about turf and stone, and a sheet of
 * water has none of its own whatever is drowned under it — it takes the shape
 * of the hollow it is sitting in and stops. Hummocks embossed on a puddle are
 * the one thing that gives it away instantly, and the exponent is already the
 * renderer's own statement of how smooth a surface is, so a bump that
 * contradicts it is double counting rather than detail. */
#define FLY_DETAIL_HUMMOCK 7.4f
#define FLY_DETAIL_TUSSOCK 2.3f
#define FLY_DETAIL_BLADE 0.55f

/* the two relief octaves, in metres, as one height field */
static float detail_relief(const fly_world *w, float x, float y, float a0, float a1) {
    return fly_noise2(w->seed + 37, x / FLY_DETAIL_HUMMOCK, y / FLY_DETAIL_HUMMOCK) * a0 +
           fly_noise2(w->seed + 39, x / FLY_DETAIL_TUSSOCK, y / FLY_DETAIL_TUSSOCK) * a1;
}

static fly_v3 terrain_detail(const fly_world *w, float x, float y, fly_v3 mat,
                             float px_per_m, fly_v3 *n) {
    float veg = mat.x;
    /* standing water (240) and the film at the shore (200) are level; damp
     * soil (85) and snow (46) still break the way the ground under them does */
    float level = fly_smoothstepf(90.0f, 220.0f, mat.z);
    /* rock breaks coarser and harder than turf does */
    float rough = (1.0f + 0.5f * (1.0f - veg)) * (1.0f - level);
    float a0 = detail_amount(FLY_DETAIL_HUMMOCK, px_per_m) * 0.62f * rough;
    float a1 = detail_amount(FLY_DETAIL_TUSSOCK, px_per_m) * 0.26f * rough;
    float fine = detail_amount(FLY_DETAIL_BLADE, px_per_m) * veg;
    fly_v3 tint = fly_v3mk(1.0f, 1.0f, 1.0f);
    if (a0 + a1 > 0.002f) {
        const float E = 0.7f;
        float c0 = detail_relief(w, x, y, a0, a1);
        float cx = detail_relief(w, x + E, y, a0, a1);
        float cy = detail_relief(w, x, y + E, a0, a1);
        fly_v3 m = *n;
        m.x -= (cx - c0) / E;
        m.y -= (cy - c0) / E;
        *n = fly_v3norm(m);
        /* clumps that stand proud are drier and lighter than the hollows */
        tint = fly_v3scale(tint, 1.0f + c0 * 0.20f);
    }
    if (fine > 0.002f) {
        float b = fly_noise2(w->seed + 38, x / FLY_DETAIL_BLADE, y / FLY_DETAIL_BLADE) * fine;
        tint = fly_v3mul(tint, fly_v3mk(1.0f + b * 0.26f, 1.0f + b * 0.17f, 1.0f - b * 0.05f));
    }
    return tint;
}

static fly_v3 terrain_normal(const fly_world *w, float x, float y, float e) {
    float hx = fly_world_ground(w, x + e, y) - fly_world_ground(w, x - e, y);
    float hy = fly_world_ground(w, x, y + e) - fly_world_ground(w, x, y - e);
    return fly_v3norm(fly_v3mk(-hx / (2 * e), -hy / (2 * e), 1.0f));
}

/* ---------------- the microfacet lobe ----------------
 *
 * Every specular in this renderer used to be a normalised Blinn lobe: pow(n.h,
 * m) times (m+8)/8pi, with a Schlick Fresnel and a hand-set ceiling to stop it
 * turning grass into a mirror at grazing angles. It is the right shape and the
 * wrong function, and the three ways it is wrong are all visible rather than
 * academic.
 *
 *  - It has no masking term. A lobe that only ever multiplies has nothing to
 *    take light away when the microgeometry shadows itself, so a rough surface
 *    turned edge-on to the sun reflects more than it receives. That is what the
 *    hand-set ceiling was patching, in the one place someone noticed.
 *  - Its tails are wrong. Blinn falls off exponentially and real roughness
 *    falls off as a power, so a Blinn highlight is a hard bright coin with
 *    nothing around it, where a real one has a core and a wide skirt. The skirt
 *    is most of what reads as "how rough is this".
 *  - Its normalisation is only right for a mirror-ish surface seen head on, so
 *    changing the exponent changed the total energy as well as the tightness,
 *    and every material's exponent had to be re-tuned against every other's.
 *
 * So: GGX for the distribution, height-correlated Smith for the masking, and
 * Schlick for the Fresnel — the standard, energy-obeying trio. What comes back
 * is a factor to multiply the sunbeam by, cosine already folded in, so a caller
 * writes `beam * ggx_sun(...)` and is done.
 *
 * The materials keep talking in Blinn exponents. That is deliberate: the
 * exponent is how the material tables and the public probe describe a surface,
 * and the conversion alpha = sqrt(2/(m+2)) is the standard one, so nothing that
 * names a material had to be re-tuned to change the function underneath it. */
static float ggx_alpha(float gloss) {
    float a = sqrtf(2.0f / ((gloss > 0.0f ? gloss : 0.0f) + 2.0f));
    return fly_clampf(a, 0.012f, 1.0f);
}

/* D * V * n.l, with V the height-correlated Smith visibility (the 1/4 n.l n.v
 * already folded in). Zero when either the light or the eye is below the
 * surface, which is the other thing an unmasked lobe gets wrong. */
static float ggx_sun(float ndl, float ndv, float ndh, float a) {
    float a2 = a * a, d, den, gl, gv, vis;
    if (ndl <= 0.0f || ndv <= 0.0f) return 0.0f;
    den = ndh * ndh * (a2 - 1.0f) + 1.0f;
    d = a2 / (FLY_PI * den * den + 1e-9f);
    gl = ndl * sqrtf(ndv * ndv * (1.0f - a2) + a2);
    gv = ndv * sqrtf(ndl * ndl * (1.0f - a2) + a2);
    vis = 0.5f / (gl + gv + 1e-6f);
    return d * vis * ndl;
}

/* Schlick, with the grazing end of it capped by the roughness rather than by
 * hand. Every surface goes to a perfect mirror edge-on if you believe Schlick,
 * which is true of a flat interface and false of everything here; the cap is
 * the split-sum result that a rough surface cannot reach one. */
static float schlick_f(float f0, float fcap, float vdh) {
    float om = 1.0f - fly_clampf(vdh, 0.0f, 1.0f);
    float om2 = om * om;
    if (fcap < f0) fcap = f0;
    return f0 + (fcap - f0) * om2 * om2 * om;
}

/* The ambient half of the same lobe: what a surface of reflectance `f0` and
 * roughness `a` reflects of the sky, as a fraction of the sky's radiance in the
 * mirror direction.
 *
 * This is the split-sum DFG term — the environment integral with the lighting
 * factored out — in Karis's four-constant fit. It replaces `f0 + (cap - f0) *
 * (1-n.v)^5` applied to the sky, which is Schlick where Schlick does not
 * belong: the Fresnel of a *single* microfacet is not the reflectance of a
 * rough surface under a whole hemisphere, and using it there is what made a
 * matte flank pick up a mirror's rim. The fit carries the two things Schlick
 * alone cannot — that a rough surface loses energy to masking, and that its
 * grazing response saturates well below one — in about a dozen instructions. */
static float ggx_env(float f0, float ndv, float a) {
    float rx = -1.0f * a + 1.0f, ry = -0.0275f * a + 0.0425f;
    float rz = -0.572f * a + 1.04f, rw = 0.022f * a - 0.04f;
    float e = exp2f(-9.28f * fly_clampf(ndv, 0.0f, 1.0f));
    float a004 = (rx * rx < e ? rx * rx : e) * rx + ry;
    float A = a004 * -1.04f + rz, B = a004 * 1.04f + rw;
    float r = f0 * A + B;
    return r > 0.0f ? r : 0.0f;
}

/* Lighting for a terrain/water/object point.
 *
 * `ao` is ambient occlusion — crevice visibility for terrain, 1 elsewhere. It
 * scales the sky and bounce terms only: direct sun is already occluded by the
 * shadow map, and multiplying it by AO as well double-counts and leaves sunlit
 * hollows unnaturally dark. */
static fly_v3 lit_surface(const fly__env *e, fly_v3 alb, fly_v3 n, float shadow, float ao) {
    float ndl = fly_clampf(fly_v3dot(n, e->sun), 0, 1);
    /* Sky ambient is the sky's own irradiance, integrated off the same
     * scattering model the sky is painted with — see make_env.
     *
     * What it replaces is worth naming, because it looked reasonable and was
     * shaped wrong. It was a zenith/horizon lerp on n.z alone, which says a
     * wall facing a sunrise is lit exactly like the wall behind it. Half the
     * sky is bright and half is not, and which half a surface faces is most of
     * what its shade looks like: at six in the morning the model puts 1.8x as
     * much light on the sunward side of a tank as on the shaded side, and
     * makes it warm rather than blue while it does it. A lerp on n.z cannot
     * express either, and no amount of tuning its endpoints will.
     *
     * The old endpoints were not wrong so much as extrapolated. At noon they
     * land within six per cent of the integral, which is why the scale needed
     * no fudge; it is dawn, dusk, overcast and night that they invented. */
    fly_v3 c = fly_v3mul(alb, fly_v3scale(sky_irradiance(e, n), ao));
    c = fly_v3add(c, fly_v3mul(fly_v3scale(alb, ndl * shadow), e->sunlight));
    /* Ground bounce: the lower half of the hemisphere, filled by whatever the
     * ground under the camera actually is and however it is actually lit — see
     * make_env. The weight is the fraction of the hemisphere below the horizon
     * that a surface facing `n` can see, which is 1 straight down and 0 straight
     * up, and unlike the old clamp it does not stop at horizontal: a vertical
     * wall sees half the ground. */
    float down = 0.5f - 0.5f * fly_clampf(n.z, -1.0f, 1.0f);
    c = fly_v3add(c, fly_v3mul(alb, fly_v3scale(e->ground_lit, down * ao)));
    /* Moonlight, weighted by how much sky the surface faces — and clamped at
     * nothing, because a light term that goes negative is not a light term.
     * Below n.z = -2/3 the old weight was negative: the moon was *removing*
     * light from anything facing down. Nothing faced down for as long as the
     * aircraft were flat plates, and the first thing a solid one with an
     * underside, a belly and wheels did was drive a pixel to -0.0074 — which is
     * a NaN out of the auto-exposure's log-average, and a whole frame of black
     * out of that.
     *
     * Through the albedo, like every other light here, and at the size the
     * night sky is: see FLY_MOON for what it was and what that cost. */
    c = fly_v3add(c, fly_v3mul(alb, fly_v3scale(FLY_MOON,
                                 (1.0f - e->day) * fly_clampf(0.4f + 0.6f * n.z, 0.0f, 1.0f))));
    return c;
}

/* Terrain lighting: lit_surface plus what the ground is actually made of.
 *
 * Lambert with a per-material albedo is a fair model of paint and a poor one of
 * a meadow. Grass is not a green surface, it is a few centimetres of thin
 * translucent blades over soil, and three things follow from that, none of
 * which a cosine can express and all of which cost a handful of instructions:
 *
 *  - Light wraps. A blade in the shade of the blade next to it is still lit by
 *    what came through and around it, so a canopy's terminator is soft and it
 *    never gets as dark as a plane at the same angle. That is the `wrap`.
 *  - It has a hot spot. Look down-sun and a canopy hides its own shadows;
 *    look up-sun and every one of them is showing. Real grass is markedly
 *    brighter with the sun behind you, and this opposition surge is the single
 *    cue that reads as "a surface made of blades" rather than as green paint.
 *  - It glows from behind. A blade is thin enough to transmit, so grass lit
 *    from the far side is brighter than grass lit from the near side and
 *    yellower, because chlorophyll passes green and red more than blue.
 *
 * The specular is the same GGX/Smith lobe the objects use, driven by the
 * material's reflectance and exponent. It is nearly invisible on grass, a broad
 * sheen on snow, and a tight highlight on the wet band at the shore, which is
 * what makes wet ground read as wet rather than as darker sand.
 *
 * Still exactly linear in `shadow`, so the callers that precompute a lit and a
 * dark colour per vertex and cross-fade them by the shadow map stay exact. */
static fly_v3 terrain_light(const fly__env *e, fly_v3 alb, fly_v3 mat, fly_v3 n,
                            fly_v3 vdir, float shadow, float ao) {
    float veg = mat.x;
    float raw = fly_v3dot(n, e->sun);
    float wrap = 0.35f * veg;
    float ndl = fly_clampf((raw + wrap) / (1.0f + wrap), 0.0f, 1.0f);
    fly_v3 suncol = e->sunlight;   /* the beam; the cosine is ndl, below */
    float align = fly_clampf(fly_v3dot(vdir, e->sun), 0.0f, 1.0f);
    float back = fly_clampf(-fly_v3dot(vdir, e->sun), 0.0f, 1.0f);
    fly_v3 hv = fly_v3norm(fly_v3add(vdir, e->sun));
    float nh = fly_clampf(fly_v3dot(n, hv), 0.0f, 1.0f);
    float nv = fly_clampf(fly_v3dot(n, vdir), 0.0f, 1.0f);
    float nl = fly_clampf(raw, 0.0f, 1.0f);
    /* How far reflectance is allowed to climb at grazing incidence.
     *
     * Schlick takes every surface to a perfect mirror edge-on, which is true of
     * a flat interface and false of everything here. It is worst on grass: at
     * ten degrees off the ground plane the plain form reaches 0.4, so a meadow
     * lit from behind washed out into a sheet of reflected sky and read as
     * polished. What actually happens is that the microgeometry shadows the
     * grazing reflection away, so the ceiling is set by the roughness — and by
     * vegetation, because a canopy of blades has no coherent reflection to
     * speak of at any angle. Grass lands back on its own F0 of 0.02 and stays
     * there; snow, rock and the wet band at the shore keep the sheen they
     * should have. */
    float alpha = ggx_alpha(mat.z);
    float fcap = (1.0f - alpha) * (1.0f - fly_clampf(veg, 0.0f, 1.0f));
    float fres = schlick_f(mat.y, fcap, fly_v3dot(vdir, hv));
    /* GGX with height-correlated Smith masking, the same lobe the objects use.
     * The cosine is inside it, which is why `direct` below does not carry one
     * for the specular the way it does for the diffuse. */
    float lobe = ggx_sun(nl, nv, nh, alpha);
    fly_v3 c = lit_surface(e, alb, n, 0.0f, ao); /* ambient half */
    /* The sky this surface reflects, as against the sun in it. Same split-sum
     * response, same irradiance basis as tri_lit, and it goes in the ambient
     * half so terrain_light stays exactly linear in `shadow` — the near rings
     * depend on that to cross-fade a lit and a dark colour per vertex. It is
     * what makes a wet shore and a snowfield brighten as they turn edge-on,
     * which is the difference between wet ground and darker ground. */
    {
        fly_v3 refl = fly_v3sub(fly_v3scale(n, 2.0f * fly_v3dot(n, vdir)), vdir);
        /* vegetation has no coherent reflection at any angle, so the canopy
         * keeps its own F0 where a bare surface gets the full environment term */
        float vg = fly_clampf(veg, 0.0f, 1.0f);
        float fe = ggx_env(mat.y, nv, alpha) * (1.0f - vg) + mat.y * vg;
        c = fly_v3add(c, fly_v3scale(sky_irradiance(e, fly_v3norm(refl)), fe * ao));
    }
    fly_v3 direct = fly_v3mul(fly_v3scale(alb, ndl), suncol);
    direct = fly_v3scale(direct, 1.0f + veg * 0.38f * align * align);
    /* transmission, only where the near side is not already facing the sun */
    direct = fly_v3add(direct, fly_v3mul(fly_v3mul(alb, fly_v3mk(1.15f, 1.30f, 0.55f)),
                                         fly_v3scale(suncol, veg * 0.55f * back * back * back *
                                                             (1.0f - fly_clampf(raw, 0.0f, 1.0f)))));
    if (raw > 0.0f)
        direct = fly_v3add(direct, fly_v3scale(suncol, lobe * fres));
    return fly_v3add(c, fly_v3scale(direct, shadow));
}

/* ---------------- near-plane clipping ----------------
 *
 * `project` cannot place a vertex that is level with or behind the eye, so the
 * software rasterizers used to drop any triangle with one — which is not a
 * clip, it is a deletion. Close geometry did not merely lose its near part, it
 * vanished: pad decks opened wedges that showed terrain through them, and the
 * rail span under a rider became a stray fragment. Clipping the triangle
 * against the near plane first, interpolating its vertex attributes along the
 * cut, is what the GPU does with the same geometry. */

#define FLY_CLIP_ATTR 12 /* most per-vertex floats any primitive carries */

typedef struct {
    fly_v3 p;
    float a[FLY_CLIP_ATTR];
} fly__cv;

/* Sutherland-Hodgman against the single near plane: a triangle becomes at most
 * a quad, so at most two triangles. Returns how many were written. */
static int near_clip_tri(const fly__view *v, const fly__cv in[3], fly__cv out[6], int na) {
    float nz = v->ortho ? 0.0015f : 0.36f;
    fly__cv poly[4];
    float z[3];
    int n = 0, i, k;
    for (i = 0; i < 3; ++i) z[i] = fly_v3dot(fly_v3sub(in[i].p, v->pos), v->fwd);
    if (z[0] >= nz && z[1] >= nz && z[2] >= nz) {
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        return 1; /* wholly in front: the common case, untouched */
    }
    if (z[0] < nz && z[1] < nz && z[2] < nz) return 0;
    for (i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        int ini = z[i] >= nz, inj = z[j] >= nz;
        if (ini) poly[n++] = in[i];
        if (ini != inj) {
            float t = (nz - z[i]) / (z[j] - z[i]);
            fly__cv c;
            c.p = fly_v3add(in[i].p, fly_v3scale(fly_v3sub(in[j].p, in[i].p), t));
            for (k = 0; k < na; ++k) c.a[k] = in[i].a[k] + (in[j].a[k] - in[i].a[k]) * t;
            poly[n++] = c;
        }
    }
    if (n < 3) return 0;
    out[0] = poly[0]; out[1] = poly[1]; out[2] = poly[2];
    if (n == 4) {
        out[3] = poly[0]; out[4] = poly[2]; out[5] = poly[3];
        return 2;
    }
    return 1;
}

static fly_v3 cv3(const fly__cv *c, int i) { return fly_v3mk(c->a[i], c->a[i + 1], c->a[i + 2]); }

/* ---------------- object capture ----------------
 *
 * When a capture buffer is installed the solid-geometry rasterizers append
 * their triangles instead of filling pixels, so the same CPU code that builds
 * and shades settlements, craft, rail and scatter can hand its geometry to the
 * GPU. Only rasterization moves: the Gouraud lit/occluded pair is still
 * computed per vertex exactly as before, and the fragment shader does the same
 * per-pixel shadow cross-fade the CPU rasterizer does.
 *
 * Emissive sprites (nav lights, beacons) are screen-space splats that read the
 * z-buffer, so they are recorded here too and replayed once the frame has been
 * read back and the depth they test against exists. */

#define FLY_OBJ_FLOATS 14 /* pos3 + lit3 + dark3 + normal3 + ndotl + agl */

typedef struct {
    float *v;
    int nvert, cap;
    int overflow;
} fly__tribuf;

typedef struct {
    float sx, sy, z, size;
    uint32_t col;
} fly__lightsplat;

#define FLY_LIGHT_MAX 512

static fly__tribuf *g_capture = NULL;
static fly__lightsplat g_lights[FLY_LIGHT_MAX];
static int g_nlights = 0, g_capture_lights = 0;

/* ---------------- the wood, as an instance lattice ----------------
 *
 * A crown is the same shape wherever it stands. What differs between two trees
 * is where they are, how big they are, what colour their leaves are and which
 * way round they are pointing — ten floats — and yet the capture above sent the
 * GPU every vertex of every one of them, shaded, at fourteen floats a vertex:
 * a closed-canopy frame at 1280x720 handed it 276,213 of them, 15.5 MB, every
 * frame. And the shading it had already paid for on the way is the part a
 * geometry cache could never have held, because the canopy BRDF's transmission
 * and its down-sun surge are functions of the view direction.
 *
 * So the crowns are drawn the way the terrain rings are: one static mesh,
 * instanced, lifted and shaded in the vertex and fragment stages.
 *
 * The template is *captured from the model rather than written again*, which is
 * the whole reason this is small enough to be worth doing. `draw_tree_model`
 * runs unchanged with a unit size at the origin and a white leaf colour, and
 * `tri_leaf` — whose arguments are already exactly a position, a shading hub,
 * an albedo and an occlusion term — appends to this buffer instead of drawing.
 * There is one model, not two, and no parity to keep.
 *
 * Per-tree variety comes back two ways. Eight templates are baked per species
 * and level of detail from eight different hashes, so the jitter, the lean and
 * the face tints `crown_pt` and `leaf_face_tint` put in are eight distinct
 * shapes rather than one; and the vertex stage turns each instance about its
 * own axis by an angle off the tree's own hash, which is a continuum on top of
 * that. Sixty-four poses of one shape is what the changelog records fixing; a
 * free rotation of eight is past it, and `world.canopy`'s tonal spread — the
 * measure that actually divides a canopy from a texture — goes 287 to 339.
 *
 * The boles stay on the capture above. They are three triangles of thirty, they
 * shade through `tri_lit_n`'s specular block rather than the canopy BRDF, and
 * that block has no GLSL twin — so moving them would cost a new one for a
 * tenth of the geometry. The scrub, the ambient skirt and the shadow cascade's
 * occluder replay are untouched and still CPU: casting is a silhouette, and the
 * near cascade's footprint is small.
 *
 * Everything here is an optimisation with a fallback. If there is no GL driver,
 * or either program will not build, `g_wood_collect` is never set and the model
 * draws as it always did — which is also what the software rasterizer and both
 * traced modes get. */

#define FLY_WOOD_SHAPES 8        /* baked shapes per species and LOD */
#define FLY_WOOD_TFLOATS 8       /* template vertex: pos3 + hub3 + albedo + ao */
#define FLY_WOOD_IFLOATS 10      /* instance: base3 + size + leaf3 + spin2 + agl */

typedef struct {
    float *v;
    int nvert, cap;
    int overflow;
} fly__woodbuf;

/* Set while a template is being captured out of `draw_tree_model`. */
static fly__woodbuf *g_wood_tmpl = NULL;
/* Set while the scatter is collecting instances instead of drawing crowns. */
static int g_wood_collect = 0;
/* Set while drawing the half of a tree an instance does not carry: the bole. */
static int g_wood_bole_only = 0;

static void woodbuf_vert(fly__woodbuf *wb, fly_v3 p, fly_v3 hub, float alb, float ao) {
    float *o;
    if (wb->nvert + 1 > wb->cap) {
        int want = wb->cap ? wb->cap * 2 : 1024;
        float *nv = (float *)realloc(wb->v, (size_t)want * FLY_WOOD_TFLOATS * sizeof(float));
        if (!nv) { wb->overflow = 1; return; }
        wb->v = nv;
        wb->cap = want;
    }
    o = &wb->v[(size_t)wb->nvert * FLY_WOOD_TFLOATS];
    o[0] = p.x; o[1] = p.y; o[2] = p.z;
    o[3] = hub.x; o[4] = hub.y; o[5] = hub.z;
    o[6] = alb;
    o[7] = ao;
    ++wb->nvert;
}

static void tribuf_reset(fly__tribuf *tb) {
    tb->nvert = 0;
    tb->overflow = 0;
}

static void tribuf_tri(fly__tribuf *tb, fly_v3 a, fly_v3 b, fly_v3 c,
                       fly_v3 la, fly_v3 lb, fly_v3 lc,
                       fly_v3 da, fly_v3 db, fly_v3 dc, fly_v3 nrm, float ndotl,
                       float agl) {
    const fly_v3 *p[3], *l[3], *d[3];
    int i;
    if (tb->nvert + 3 > tb->cap) {
        int want = tb->cap ? tb->cap * 2 : 4096;
        float *nv;
        while (want < tb->nvert + 3) want *= 2;
        nv = (float *)realloc(tb->v, (size_t)want * FLY_OBJ_FLOATS * sizeof(float));
        if (!nv) { tb->overflow = 1; return; }
        tb->v = nv;
        tb->cap = want;
    }
    p[0] = &a; p[1] = &b; p[2] = &c;
    l[0] = &la; l[1] = &lb; l[2] = &lc;
    d[0] = &da; d[1] = &db; d[2] = &dc;
    for (i = 0; i < 3; ++i) {
        float *o = &tb->v[(size_t)(tb->nvert + i) * FLY_OBJ_FLOATS];
        o[0] = p[i]->x; o[1] = p[i]->y; o[2] = p[i]->z;
        o[3] = l[i]->x; o[4] = l[i]->y; o[5] = l[i]->z;
        o[6] = d[i]->x; o[7] = d[i]->y; o[8] = d[i]->z;
        /* the face normal rides along so the fragment stage can slide its
         * shadow sample off the surface (fly_sm_offset) rather than lean on a
         * depth bias big enough to detach the shadow from its caster */
        o[9] = nrm.x; o[10] = nrm.y; o[11] = nrm.z;
        o[12] = ndotl;
        /* and how far off the ground this primitive stands, so the fragment
         * stage can read the occlusion window at the height it is at rather
         * than at the height of the ground underneath it */
        o[13] = agl;
    }
    tb->nvert += 3;
}

/* ---------------- raster core (Gouraud) ---------------- */

/* `clip` optionally carries a per-vertex scalar: pixels where it interpolates
 * below `clip_min` are skipped. Water uses it so the shoreline follows the
 * contour instead of stepping along whole grid cells — without it a single wet
 * corner floods its entire cell, which near the camera paints a slab of sea
 * across dry ground. */
static void raster_tri_hdr_raw(fly_render_target *rt, const fly__view *v,
                               fly_v3 a, fly_v3 b, fly_v3 c,
                               fly_v3 ca, fly_v3 cb, fly_v3 cc,
                               const float *clip, float clip_min) {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    if (!project(v, rt->w, rt->h, a, &ax, &ay, &az)) return;
    if (!project(v, rt->w, rt->h, b, &bx, &by, &bz)) return;
    if (!project(v, rt->w, rt->h, c, &cx, &cy, &cz)) return;
    int minx = (int)fminf(fminf(ax, bx), cx), maxx = (int)fmaxf(fmaxf(ax, bx), cx);
    int miny = (int)fminf(fminf(ay, by), cy), maxy = (int)fmaxf(fmaxf(ay, by), cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= rt->w) maxx = rt->w - 1;
    if (maxy >= rt->h) maxy = rt->h - 1;
    if (minx > maxx || miny > maxy) return;
    float d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(d) < 1e-6f) return;
    float inv = 1.0f / d;
    FLY_PERSP_SETUP(v, az, bz, cz, ia, ib, ic, persp);
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
            float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
            float uu = 1.0f - u - vv;
            float z;
            if (u < 0 || vv < 0 || uu < 0) continue;
            FLY_PERSPECTIVE(persp, uu, u, vv, az, bz, cz, ia, ib, ic, z);
            if (clip && uu * clip[0] + u * clip[1] + vv * clip[2] < clip_min) continue;
            int idx = y * rt->w + x;
            if (z < rt->depth[idx]) {
                rt->depth[idx] = z;
                rt->hdr[idx] = fly_v3mk(uu * ca.x + u * cb.x + vv * cc.x,
                                        uu * ca.y + u * cb.y + vv * cc.y,
                                        uu * ca.z + u * cb.z + vv * cc.z);
            }
        }
}

static void raster_tri_hdr_clip(fly_render_target *rt, const fly__view *v,
                                fly_v3 a, fly_v3 b, fly_v3 c,
                                fly_v3 ca, fly_v3 cb, fly_v3 cc,
                                const float *clip, float clip_min) {
    fly__cv in[3], out[6];
    int nt, k;
    if (g_shadow_cast) return; /* shadow pass: decals/markings cast nothing */
    phase_verts(3);
    /* flat colour and no shadow lookup: ndotl < 0 tells the shader to skip it */
    if (g_capture && !clip) {
        tribuf_tri(g_capture, a, b, c, ca, cb, cc, ca, cb, cc, fly_v3zero(), -1.0f, 0.0f);
        return;
    }
    in[0].p = a; in[1].p = b; in[2].p = c;
    in[0].a[0] = ca.x; in[0].a[1] = ca.y; in[0].a[2] = ca.z;
    in[1].a[0] = cb.x; in[1].a[1] = cb.y; in[1].a[2] = cb.z;
    in[2].a[0] = cc.x; in[2].a[1] = cc.y; in[2].a[2] = cc.z;
    for (k = 0; k < 3; ++k) in[k].a[3] = clip ? clip[k] : 0.0f;
    nt = near_clip_tri(v, in, out, 4);
    for (k = 0; k < nt; ++k) {
        const fly__cv *t = &out[k * 3];
        float kc[3] = { t[0].a[3], t[1].a[3], t[2].a[3] };
        raster_tri_hdr_raw(rt, v, t[0].p, t[1].p, t[2].p,
                           cv3(&t[0], 0), cv3(&t[1], 0), cv3(&t[2], 0),
                           clip ? kc : NULL, clip_min);
    }
}

static void raster_tri_hdr(fly_render_target *rt, const fly__view *v,
                           fly_v3 a, fly_v3 b, fly_v3 c,
                           fly_v3 ca, fly_v3 cb, fly_v3 cc) {
    raster_tri_hdr_clip(rt, v, a, b, c, ca, cb, cc, NULL, 0.0f);
}

static void raster_tri_hdr1(fly_render_target *rt, const fly__view *v,
                            fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 col) {
    raster_tri_hdr(rt, v, a, b, c, col, col, col);
}

static void raster_tri_view(fly_render_target *rt, const fly__view *v,
                            fly_v3 a, fly_v3 b, fly_v3 c, uint32_t color) {
    raster_tri_hdr1(rt, v, a, b, c, lin_from_u32(color));
}

void fly_raster_tri(fly_render_target *rt, const fly_cam *cam,
                    fly_v3 a, fly_v3 b, fly_v3 c, uint32_t color) {
    fly__view v = make_view(cam, rt->w, rt->h);
    raster_tri_view(rt, &v, a, b, c, color);
}

/* ---------------- sun shadow map: raster + sample ---------------- */

/* project a world point onto a cascade: (texel x, texel y, depth-along-light) */
static void sm_project(const fly__shadow *sm, int c, fly_v3 p,
                       float *sx, float *sy, float *sw) {
    *sx = (fly_v3dot(p, sm->right) - sm->umin[c]) / sm->texel[c];
    *sy = (fly_v3dot(p, sm->up) - sm->vmin[c]) / sm->texel[c];
    *sw = fly_v3dot(p, sm->fwd);
}

/* rasterize one occluder triangle into a cascade, keeping the nearest-to-sun
 * depth per texel (orthographic, so depth is just dot(P, light-forward)) */
static void sm_raster_tri(fly__shadow *sm, int c, fly_v3 A, fly_v3 B, fly_v3 C) {
    float ax, ay, aw, bx, by, bw, cx, cy, cw;
    sm_project(sm, c, A, &ax, &ay, &aw);
    sm_project(sm, c, B, &bx, &by, &bw);
    sm_project(sm, c, C, &cx, &cy, &cw);
    int minx = (int)floorf(fminf(fminf(ax, bx), cx)), maxx = (int)ceilf(fmaxf(fmaxf(ax, bx), cx));
    int miny = (int)floorf(fminf(fminf(ay, by), cy)), maxy = (int)ceilf(fmaxf(fmaxf(ay, by), cy));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= sm->size[c]) maxx = sm->size[c] - 1;
    if (maxy >= sm->size[c]) maxy = sm->size[c] - 1;
    if (minx > maxx || miny > maxy) return;
    float d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(d) < 1e-9f) return;
    float inv = 1.0f / d;
    float *depth = sm->depth[c];
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
            float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
            float uu = 1.0f - u - vv;
            if (u < 0 || vv < 0 || uu < 0) continue;
            float w = uu * aw + u * bw + vv * cw;
            int idx = y * sm->size[c] + x;
            if (w < depth[idx]) depth[idx] = w;
        }
}

/* Occluder triangles recorded during the cast replay: world positions only,
 * nine floats per triangle. Both builders consume this — the CPU one scan
 * converts it, the GPU one uploads it as a mesh — so the replay walks the
 * scene once regardless of which backend finishes the map, and a GL failure
 * can fall back without re-running it. */
static float *g_smcast;
static int g_smcast_n, g_smcast_cap;
static int g_smcast_overflow;

static void shadow_cast_tri(fly__shadow *sm, fly_v3 a, fly_v3 b, fly_v3 c) {
    float *d;
    (void)sm;
    /* A site capture borrows the caster to find out what a settlement puts
     * where — every rule about what may cast a shadow is also a rule about
     * what may occlude the sky, so it rides the same seam rather than a second
     * one beside it. It does not want the triangle list. */
    if (g_occ_build) { occ_cast_tri(a, b, c); return; }
    if (g_smcast_n + 9 > g_smcast_cap) {
        int cap = g_smcast_cap ? g_smcast_cap * 2 : 8192;
        float *nb = (float *)realloc(g_smcast, (size_t)cap * sizeof(float));
        if (!nb) { g_smcast_overflow = 1; return; }
        g_smcast = nb;
        g_smcast_cap = cap;
    }
    d = &g_smcast[g_smcast_n];
    d[0] = a.x; d[1] = a.y; d[2] = a.z;
    d[3] = b.x; d[4] = b.y; d[5] = b.z;
    d[6] = c.x; d[7] = c.y; d[8] = c.z;
    g_smcast_n += 9;
}

static void shadow_replay_cpu(fly__shadow *sm) {
    int i, k;
    for (i = 0; i + 8 < g_smcast_n; i += 9) {
        fly_v3 a = fly_v3mk(g_smcast[i + 0], g_smcast[i + 1], g_smcast[i + 2]);
        fly_v3 b = fly_v3mk(g_smcast[i + 3], g_smcast[i + 4], g_smcast[i + 5]);
        fly_v3 c = fly_v3mk(g_smcast[i + 6], g_smcast[i + 7], g_smcast[i + 8]);
        for (k = 0; k < FLY_SM_CASCADES; ++k) sm_raster_tri(sm, k, a, b, c);
    }
}

/* PCF sun visibility in one cascade: fraction of taps the point is in front of.
 * Off-map taps count as lit so the map's border never rings with false dark. */
/* Bilinear-weighted PCF box.
 *
 * A nearest-tap box jumps by a whole tap the moment the sample crosses a texel
 * edge. Wherever a shadow texel is smaller than a screen pixel — which is most
 * of the frame from any altitude — that reads as a fixed checkerboard woven
 * across the terrain, a few percent of luma and quite visible. Extending the
 * box by one tap on each side and weighting the outer row/column by the
 * sub-texel position makes visibility continuous in the sample position, which
 * removes the weave outright. Costs 16 taps instead of 9 at pcf 1. */
/* Percentage-closer filter over a box of half-width `rad` texels, tapered.
 *
 * Two things make the radius a real number rather than a count of taps. The
 * weights are the box's overlap with each texel, which is where the base
 * filter's fractional end weights come from — derived here rather than
 * special-cased — and the tap is then tapered by how far it sits from the
 * centre, linearly to nothing at the box's own edge.
 *
 * Both are about continuity, and the taper is the one that had to be found by
 * measurement. Without it the outermost tap carries full weight, so a caster
 * crossing the kernel's edge changes the answer by a whole tap at once — and
 * because the search that sizes the kernel cannot be as sensitive as the
 * kernel itself without costing exactly as much, that step lands on a contour
 * around every soft shadow. Measured on an aeroplane's shadow from 76 m: a
 * clean seven-per-cent rim, one sample wide, all the way round it. With the
 * taper the edge tap is worth nothing, so a caster arrives at zero weight and
 * grows, and whether the search noticed it a sample sooner or later stops
 * mattering.
 *
 * It tapers over the *widening* and not over the whole kernel, which is the
 * difference between a filter and a narrower filter. A taper across the lot is
 * a tent, and a tent of a given support is materially tighter than the box of
 * the same support: it took the lattice-stability gate from 0.0033 to 0.0045
 * against a 0.0040 bound, which is sharper edges handed back as crawl. At the
 * base radius the shoulder is zero wide and this is exactly the kernel the
 * fixed-width filter used, tap for tap and weight for weight — which matters
 * for more than continuity, since the far cascade's kernel is four taps of
 * bilinear and any taper at all across it degenerates to nearest-tap.
 *
 * The nearest-tap version of any of this weaves a checkerboard across the
 * terrain, which is why the fractional edges were there in the first place. */
static float sm_cascade_vis(const fly__shadow *sm, int c, float sx, float sy,
                            float w, float bias, float rad, float *bsum, int *bn) {
    const float *depth = sm->depth[c];
    int sz = sm->size[c];
    float u = sx - 0.5f, v = sy - 0.5f;
    float ulo = u - rad, uhi = u + rad + 1.0f;
    float vlo = v - rad, vhi = v + rad + 1.0f;
    int x0 = (int)floorf(ulo), x1 = (int)floorf(uhi);
    int y0 = (int)floorf(vlo), y1 = (int)floorf(vhi);
    float hw = rad + 0.5f;   /* the box's own half-width, texels */
    float sh = fminf(1.0f, rad - (float)sm->pcf[c]);   /* shoulder width, texels */
    float wxs[FLY_SM_PEN_TAPMAX];
    float lit = 0.0f, tot = 0.0f;
    int i, j;
    if (x1 - x0 >= FLY_SM_PEN_TAPMAX) x1 = x0 + FLY_SM_PEN_TAPMAX - 1;
    /* the column's weights do not depend on the row, so build them once: the
       same expression evaluated inside both loops costs more than the taps */
    for (i = x0; i <= x1; ++i) {
        float wx = fminf((float)(i + 1), uhi) - fmaxf((float)i, ulo);
        if (sh > 0.0f) wx *= fly_clampf((hw - fabsf((float)i - u)) / sh, 0.0f, 1.0f);
        wxs[i - x0] = wx > 0.0f ? wx : 0.0f;
    }
    for (j = y0; j <= y1; ++j) {
        float wy = fminf((float)(j + 1), vhi) - fmaxf((float)j, vlo);
        const float *row;
        if (wy <= 0.0f) continue;
        if (sh > 0.0f) wy *= fly_clampf((hw - fabsf((float)j - v)) / sh, 0.0f, 1.0f);
        if (wy <= 0.0f) continue;
        row = (j >= 0 && j < sz) ? depth + (size_t)j * sz : NULL;
        for (i = x0; i <= x1; ++i) {
            float wgt = wxs[i - x0] * wy, d;
            if (wgt <= 0.0f) continue;
            tot += wgt;
            if (!row || i < 0 || i >= sz) { lit += wgt; continue; }
            d = row[i];
            if (d > 1e29f || w <= d + bias) { lit += wgt; continue; }
            /* it is in the way: the depth comparison this tap already made is
               the blocker search, so record it rather than paying for it twice */
            *bsum += d;
            *bn += 1;
        }
    }
    return tot > 0.0f ? lit / tot : 1.0f;
}

/* Slope-scaled depth bias (in world units): grows on grazing surfaces to kill
 * self-shadow acne without floating the contact shadows off their casters.
 *
 * `rad` is the filter's half-width in texels, because that is what the bias is
 * covering — how far the receiver's own depth can travel across the footprint
 * being compared against it. A kernel spread to soften a distant edge samples
 * a proportionally wider patch of receiver, and a bias sized for the base
 * kernel stripes it. At `rad == pcf[c]` the span is exactly 1 and this is the
 * bias the fixed-width filter was tuned with. */
static float sm_bias(const fly__shadow *sm, int c, float ndotl, float rad) {
    float slope = fly_clampf((1.0f - ndotl) / fmaxf(ndotl, 0.12f), 0.0f, 7.0f);
    float span = (rad + 0.5f) / ((float)sm->pcf[c] + 0.5f);
    return sm->texel[c] * (0.9f + slope * 0.55f) * span + 0.05f;
}

/* The blockers the base kernel cannot see.
 *
 * A search inside the filter finds the caster over a receiver that is already
 * in shadow and misses the one standing just beyond it — which is precisely
 * the receiver in the *outer* half of a wide penumbra, the half that decides
 * whether an edge softens both ways or only inward. So a ring of taps at the
 * widest radius the filter may reach, eight of them, and nothing between: it
 * is answering "is there something far enough away out there", which is the
 * lowest-frequency question the map is ever asked. A caster thinner than the
 * ring's spacing is missed and the surface keeps the base radius, so the
 * failure mode is the fixed-width shadow it replaced rather than a wrong one. */
static void sm_ring(const fly__shadow *sm, int c, float sx, float sy,
                    float w, float bias, float *bsum, int *bn) {
    /* eight points on a circle at the widest kernel's own outer reach, written
       out rather than turned, so the C and the GLSL walk the same eight
       texels. On the reach exactly, because a caster the ring first sees there
       is one the kernel tapers to nothing, so the two agree at the moment the
       wide kernel takes over and the transition has no step in it. */
    static const float ox[8] = { 1.0f, -1.0f, 0.0f, 0.0f, 0.7071f, -0.7071f, 0.7071f, -0.7071f };
    static const float oy[8] = { 0.0f, 0.0f, 1.0f, -1.0f, 0.7071f, 0.7071f, -0.7071f, -0.7071f };
    const float *depth = sm->depth[c];
    int sz = sm->size[c], k;
    for (k = 0; k < 8; ++k) {
        int x = (int)floorf(sx + ox[k] * (FLY_SM_PEN_RMAX + 0.5f));
        int y = (int)floorf(sy + oy[k] * (FLY_SM_PEN_RMAX + 0.5f));
        float d;
        if (x < 0 || y < 0 || x >= sz || y >= sz) continue;
        d = depth[y * sz + x];
        if (d > 1e29f || w <= d + bias) continue;   /* nothing in the way */
        *bsum += d;
        *bn += 1;
    }
}

/* Filter half-width in texels for a receiver at light-space depth `w` whose
 * blocker sits at `dblock`. The gap between them is measured along the light's
 * own travel — which is exactly what `dot(P, fwd)` is — so it is the distance
 * the sun's disc has to spread over, and the penumbra is that gap times the
 * disc's tangent. Floored at the cascade's base radius, which is the width
 * that is right at contact and is all a fixed filter ever had. */
static float sm_penumbra(const fly__shadow *sm, int c, float w, float bsum, int bn) {
    float base = (float)sm->pcf[c], gap;
    if (bn <= 0) return base;
    gap = w - bsum / (float)bn;
    if (gap <= 0.0f) return base;
    return fly_clampf(gap * FLY_SM_SUN_TAN / sm->texel[c], base, FLY_SM_PEN_RMAX);
}

/* Finest cascade at or after `from` whose interior contains this light-plane
 * point, or -1. Every consumer picks a cascade this way, so the normal offset
 * is always sized in the texels the lookup will actually use. */
static int sm_pick(const fly__shadow *sm, float pu, float pv, int from,
                   float *sx, float *sy) {
    int c;
    for (c = from; c < FLY_SM_CASCADES; ++c) {
        float sz = (float)sm->size[c];
        float x = (pu - sm->umin[c]) / sm->texel[c];
        float y = (pv - sm->vmin[c]) / sm->texel[c];
        if (x >= 1 && y >= 1 && x < sz - 1 && y < sz - 1) { *sx = x; *sy = y; return c; }
    }
    return -1;
}

/* Normal offset: see FLY_GLSL_SHADOW's fly_sm_offset for why the sample slides
 * along the surface normal rather than the comparison depth sliding back. `n`
 * must already face the sun; callers orient it once per primitive. */
static fly_v3 sm_offset(const fly__shadow *sm, fly_v3 p, fly_v3 n, float ndotl) {
    float pu, pv, sx, sy, graze;
    int c;
    if (!sm || !sm->active) return p;
    pu = fly_v3dot(p, sm->right);
    pv = fly_v3dot(p, sm->up);
    c = sm_pick(sm, pu, pv, 0, &sx, &sy);
    if (c < 0) return p;
    graze = sqrtf(fly_clampf(1.0f - ndotl * ndotl, 0.0f, 1.0f));
    return fly_v3add(p, fly_v3scale(n, sm->texel[c] * ((float)sm->pcf[c] + 1.6f) * graze + 0.04f));
}

/* One cascade's answer: filter at the base width, find out how far off the
 * caster was while doing it, and filter again at the width that gap deserves.
 *
 * The order is what keeps this affordable. The base kernel is a lookup the
 * cascade already paid for and every tap in it is a depth compared against the
 * receiver, so the blocker mean falls out of it for nothing; the ring adds
 * eight taps and is the whole extra cost of the common case. Only a receiver
 * that turns out to have something distant over it — an edge that is genuinely
 * soft — pays for the second, wider kernel. Under a canopy, where the crown is
 * five metres up and the answer is the base radius, nothing is spent at all.
 *
 * The bias is sized twice for the same reason it exists: it covers how far the
 * receiver's own depth travels across the footprint, and the two kernels have
 * different footprints. */
static float sm_cascade(const fly__shadow *sm, int c, float sx, float sy,
                        float w, float ndotl) {
    float pcf = (float)sm->pcf[c];
    float base = sm_bias(sm, c, ndotl, pcf);
    float bsum = 0.0f, rad, vis;
    int bn = 0;
    vis = sm_cascade_vis(sm, c, sx, sy, w, base, pcf, &bsum, &bn);
    sm_ring(sm, c, sx, sy, w, base, &bsum, &bn);
    rad = sm_penumbra(sm, c, w, bsum, bn);
    if (rad <= pcf + 1e-3f) return vis;
    bsum = 0.0f;
    bn = 0;
    return sm_cascade_vis(sm, c, sx, sy, w, sm_bias(sm, c, ndotl, rad), rad, &bsum, &bn);
}

/* ---------------- what the wood lets through ----------------
 *
 * `draw_foliage` puts the scatter's real crowns through the caster inside the
 * near cascade's own footprint and nowhere else — the whole 2.6 km scatter is
 * far too much geometry to rasterize three times a frame — so from about a
 * hundred and thirty metres out to the horizon a wood cast nothing at all and
 * was lit as though it were transparent. That is most of every frame flown
 * over land, and it is the reason a wooded landscape read as a set of models
 * standing on a lawn: not that the floor was the wrong colour (the litter and
 * the traced ambient had that) but that no wood laid a shadow anywhere.
 *
 * What replaces the geometry out there is the wood as a *medium*, which is
 * what a stand a kilometre away is: one number for how much sun gets past it.
 *
 * Casting it as a solid lid on the caster grid was built first and measured
 * wrong, which is worth recording because it is the obvious move. A shadow map
 * answers in one bit — the receiver is behind the caster or it is not — so a
 * lid at any height the depth bias can resolve puts the whole wood floor in
 * full shade. Against the crowns' own answer over 220 points of closed canopy
 * that is 0.00 where the truth is 0.84, and the only heights that do better
 * are the ones thin enough for the normal offset to leak through, which is
 * tuning a canopy against the noise floor of a depth bias. The height sweep
 * ran 3.0 / 4.5 / 5.8 / 7.0 / 9.3 m and measured 0.72 / 0.11 / 0.00 / 0.00 /
 * 0.00: there is no value in it, only a cliff.
 *
 * A transmittance has no cliff and is the better model besides. The tap is
 * taken where the sun ray crosses the crown layer rather than overhead, which
 * costs nothing and is the whole directional half of the effect: a wood shades
 * the ground on its own dark side, so an edge throws a fringe onto the meadow
 * beside it and an opening in a stand is bright on the sunward side. The
 * offset is FLY_CANOPY_H/sun.z, which is metres at noon and tens of metres at
 * dusk, exactly as a tree's shadow is.
 *
 * The ground height at the tap is the shaded point's own. The offset is at
 * most FLY_CANOPY_OFFSET and the two falloffs it feeds are a waterline eight
 * metres wide and a treeline three hundred and fifty, so re-sampling the
 * heightfield to move either of them a few centimetres would double the cost
 * of the term to change nothing it returns. */
#define FLY_CANOPY_OFFSET 60.0f  /* how far the tap may slide as the sun drops */
#define FLY_CANOPY_SUNZ 0.05f    /* below this the sun term is gone anyway */

/* How much of the crown layer's shading this point should take from the
 * medium rather than from geometry: 0 where `draw_foliage` is casting real
 * crowns, 1 beyond them.
 *
 * The two descriptions are the same wood at two resolutions and the fade is
 * the seam between them, so it is keyed on exactly what decides which one is
 * in force — the near cascade's footprint, which is what `draw_foliage`'s
 * caster radius is. Over the outer quarter of it, because that is where the
 * scatter is already shrinking its trees away to keep the radius from being a
 * visible ring on the ground. */
static float canopy_medium(const fly__shadow *sm, fly_v3 p) {
    float r, dx, dy;
    if (!sm || !sm->active) return 1.0f;
    dx = p.x - sm->center[0].x;
    dy = p.y - sm->center[0].y;
    r = sm->half[0] * 1.35f;
    return fly_smoothstepf(r * 0.75f, r, sqrtf(dx * dx + dy * dy));
}

/* Sun transmittance through the canopy over `p`, 1 where nothing grows.
 *
 * `agl` is how far the surface stands above the ground, and it is not a
 * refinement: every primitive the rasterizer shades against the shadow map
 * comes through here, the crowns included, and a crown lit by the shade of the
 * wood floor it grows out of is the whole effect upside down. A wood from the
 * air is lit crowns over a dark floor, so what the leaves are shaded by has to
 * be only the leaves above them. It is the same argument the occlusion window
 * makes for holding two heights instead of one. */
static float canopy_sun(const fly__env *e, fly_v3 p, float agl) {
    float t, d, k;
    if (!e->world || e->sun.z <= FLY_CANOPY_SUNZ) return 1.0f;
    if (agl >= FLY_CANOPY_TOP) return 1.0f;
    k = canopy_medium(e->shadow, p);
    if (k <= 0.0f) return 1.0f;
    if (agl > 0.0f) k *= 1.0f - agl * (1.0f / FLY_CANOPY_TOP);
    t = FLY_CANOPY_H / e->sun.z;
    if (t > FLY_CANOPY_OFFSET) t = FLY_CANOPY_OFFSET;
    d = fly_world_canopy(e->world, p.x + e->sun.x * t, p.y + e->sun.y * t, p.z) *
        (1.0f / FLY_CANOPY_H);
    return expf(-FLY_CANOPY_K * d * k);
}

/* Sun visibility 0..1 at a world point: the finest cascade that covers it,
 * cross-faded into the next coarser one across its border band. At most two
 * kernels are ever evaluated, whatever the cascade count. */
static float shadow_sample(const fly__shadow *sm, fly_v3 p, float ndotl) {
    /* a GPU-built map has no CPU-side texels; nothing on the software path
     * should reach here with one, and reporting "lit" beats reading NULL */
    if (!sm || !sm->active || !sm->depth[0]) return 1.0f;
    float pu = fly_v3dot(p, sm->right), pv = fly_v3dot(p, sm->up), w = fly_v3dot(p, sm->fwd);
    float sz, sx, sy, fx, fy, edge, band, vis, vcoarse;
    int c = sm_pick(sm, pu, pv, 0, &sx, &sy), c2;
    if (c < 0) return 1.0f;
    sz = (float)sm->size[c];
    vis = sm_cascade(sm, c, sx, sy, w, ndotl);
    edge = fminf(fminf(sx, sz - 1 - sx), fminf(sy, sz - 1 - sy));
    band = sz * 0.10f;
    if (edge >= band) return vis;
    c2 = sm_pick(sm, pu, pv, c + 1, &fx, &fy);
    vcoarse = c2 >= 0 ? sm_cascade(sm, c2, fx, fy, w, ndotl) : 1.0f;
    return fly_lerpf(vcoarse, vis, fly_clampf(edge / band, 0, 1));
}

/* How much sun reaches a point on the ground: the cast shadows the map holds,
 * and then whatever canopy is standing over it. Both halves, in one place, so
 * the per-pixel and per-vertex terrain paths cannot answer differently — the
 * two of them shade the same ground on either side of one LOD boundary. */
static float terrain_sun(const fly__env *e, fly_v3 p, fly_v3 nrm, float ndotl, float agl) {
    return shadow_sample(e->shadow, sm_offset(e->shadow, p, nrm, ndotl), ndotl) *
           canopy_sun(e, p, agl);
}

/* One unfiltered tap, for the volumetric march.
 *
 * A surface lookup filters because the shadow's edge *is* the pixel: one
 * comparison per texel steps from lit to dark in one texel and the eye reads
 * the staircase, so the kernel above spends up to thirty-six taps softening
 * it. A parcel of air is not that. The march already averages `steps` taps
 * along the ray under a per-pixel dither and the result is bilinearly
 * upsampled from a buffer at a fraction of the frame's resolution, so it is
 * softened twice over before it is composited — and a kernel underneath all
 * that is the same softening a third time, priced per step. Measured on the
 * high rung it was 54% of the whole frame.
 *
 * The cascade cross-fade stays. It is two taps rather than one and it is not
 * cosmetic: a cascade border is a step in *world* space, so it lands in the
 * same place on every ray that crosses it and no amount of averaging along a
 * ray or across neighbouring pixels takes it out. */
static float shadow_beam(const fly__shadow *sm, fly_v3 p) {
    float pu, pv, w, sx, sy, sz, edge, band, vis, vcoarse;
    int c, c2;
    if (!sm || !sm->active || !sm->depth[0]) return 1.0f;
    pu = fly_v3dot(p, sm->right);
    pv = fly_v3dot(p, sm->up);
    w = fly_v3dot(p, sm->fwd);
    c = sm_pick(sm, pu, pv, 0, &sx, &sy);
    if (c < 0) return 1.0f;
    /* sm_pick only returns a cascade whose interior contains the point, so
     * both indices are inside the map and need no second clamp */
    {
        float d = sm->depth[c][(int)sy * sm->size[c] + (int)sx];
        vis = (d > 1e29f || w <= d + sm_bias(sm, c, 1.0f, (float)sm->pcf[c])) ? 1.0f : 0.0f;
    }
    sz = (float)sm->size[c];
    edge = fminf(fminf(sx, sz - 1 - sx), fminf(sy, sz - 1 - sy));
    band = sz * 0.10f;
    if (edge >= band) return vis;
    c2 = sm_pick(sm, pu, pv, c + 1, &sx, &sy);
    vcoarse = 1.0f;
    if (c2 >= 0) {
        float d = sm->depth[c2][(int)sy * sm->size[c2] + (int)sx];
        vcoarse = (d > 1e29f || w <= d + sm_bias(sm, c2, 1.0f, (float)sm->pcf[c2])) ? 1.0f : 0.0f;
    }
    return fly_lerpf(vcoarse, vis, fly_clampf(edge / band, 0, 1));
}

/* Rasterize a shadow-receiving triangle: interpolate the fully-lit and the
 * sun-occluded vertex colors (both already fogged, so fog stays consistent)
 * and cross-fade between them per pixel by the shadow map's visibility. This
 * keeps the crisp shadow edge independent of the mesh tessellation while the
 * rest of the shading stays cheap Gouraud. */
static void raster_tri_shadowed_raw(fly_render_target *rt, const fly__view *v, const fly__env *e,
                                    fly_v3 a, fly_v3 b, fly_v3 c,
                                    fly_v3 la, fly_v3 lb, fly_v3 lc,
                                    fly_v3 da, fly_v3 db, fly_v3 dc, float agl) {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    if (!project(v, rt->w, rt->h, a, &ax, &ay, &az)) return;
    if (!project(v, rt->w, rt->h, b, &bx, &by, &bz)) return;
    if (!project(v, rt->w, rt->h, c, &cx, &cy, &cz)) return;
    int minx = (int)fminf(fminf(ax, bx), cx), maxx = (int)fmaxf(fmaxf(ax, bx), cx);
    int miny = (int)fminf(fminf(ay, by), cy), maxy = (int)fmaxf(fmaxf(ay, by), cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= rt->w) maxx = rt->w - 1;
    if (maxy >= rt->h) maxy = rt->h - 1;
    if (minx > maxx || miny > maxy) return;
    float d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(d) < 1e-6f) return;
    float inv = 1.0f / d;
    FLY_PERSP_SETUP(v, az, bz, cz, ia, ib, ic, persp);
    fly_v3 nrm = fly_v3norm(fly_v3cross(fly_v3sub(b, a), fly_v3sub(c, a)));
    if (fly_v3dot(nrm, e->sun) < 0.0f) nrm = fly_v3scale(nrm, -1.0f);
    float ndotl = fly_v3dot(nrm, e->sun);
    if (ndotl < 0.0f) ndotl = -ndotl;
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
            float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
            float uu = 1.0f - u - vv;
            float z;
            if (u < 0 || vv < 0 || uu < 0) continue;
            FLY_PERSPECTIVE(persp, uu, u, vv, az, bz, cz, ia, ib, ic, z);
            int idx = y * rt->w + x;
            if (z >= rt->depth[idx]) continue;
            fly_v3 wp = fly_v3mk(uu * a.x + u * b.x + vv * c.x,
                                 uu * a.y + u * b.y + vv * c.y,
                                 uu * a.z + u * b.z + vv * c.z);
            float s = terrain_sun(e, wp, nrm, ndotl, agl);
            fly_v3 lit = fly_v3mk(uu * la.x + u * lb.x + vv * lc.x,
                                  uu * la.y + u * lb.y + vv * lc.y,
                                  uu * la.z + u * lb.z + vv * lc.z);
            fly_v3 dark = fly_v3mk(uu * da.x + u * db.x + vv * dc.x,
                                   uu * da.y + u * db.y + vv * dc.y,
                                   uu * da.z + u * db.z + vv * dc.z);
            rt->depth[idx] = z;
            /* `dark` is the ambient half and `lit - dark` the direct half, so
               the traced occlusion scales exactly the term it belongs to and
               leaves the sun alone. Read per pixel: this is the whole reason
               the cache is a lattice and not a per-vertex attribute. */
            {
                float vis = ao_at(wp.x, wp.y, agl, fly_v3dist(wp, v->pos));
                rt->hdr[idx] = fly_v3add(fly_v3scale(dark, vis),
                                         fly_v3scale(fly_v3sub(lit, dark), s));
            }
        }
}

/* ---------------- water surface ----------------
 *
 * The wave normal drives fresnel and the sun glint, both sharply non-linear,
 * and the water rings are as coarse as the terrain's (26 m at the finest). One
 * colour per vertex therefore broke the surface into large soft triangles
 * wherever water filled the near field. Only the two expensive lookups stay
 * per vertex — the reflected sky (a cloud march) and the cloud deck, neither of
 * which turns appreciably across a cell. The GPU water shader is the same
 * split, function for function. */
typedef struct {
    fly_v3 sky;   /* sky reflected about the mean surface */
    fly_v3 fogT;  /* aerial perspective: what survives of the surface colour */
    fly_v3 fogS;  /* and what the air between adds on top of it */
    float depth;  /* metres of water; negative above the surface */
    float z;      /* where the surface stands here: sea level, or a lake's own */
    float cloud;  /* cloud-deck sun transmission */
} fly__wv;

/* Mean square of fly_noise2 over the plane. Value noise is a smoothstep-blended
 * bilinear of independent uniforms, so it is nowhere near the 1/3 a uniform
 * would give; measured over two million samples it is 0.184. */
#define FLY_NOISE_MS 0.184f

/* The water surface, before fog. One function for all three renderers.
 *
 * It used to be three: the CPU rasterizer's, an exact GLSL port of it, and a
 * third inside the path tracer that shared none of it — different wave normal,
 * no glint, no surf, and a Fresnel term the wrong way round, so the traced sea
 * went blue at grazing angles where a water surface is very nearly a mirror and
 * reflective looked straight down where it should be dark. The two pipelines
 * disagreed most exactly where they are hardest to miss, along a coast.
 *
 * `sky` is what the surface reflects: the rasterizers hand it a sky sample, the
 * tracer hands it a traced reflection ray, which is the tracer's advantage and
 * the only thing that should differ between them. */
static fly_v3 water_surface(const fly__env *e, fly_v3 eye, float wx, float wy, float wz,
                            float depth, fly_v3 sky, float cloud) {
    float t = (float)(e->time);
    /* wind-driven waves: swell + chop aligned with the wind */
    float wspd = fly_v3len(fly_v3mk(e->wind.x, e->wind.y, 0));
    float chopamp = fly_clampf(wspd / 16.0f, 0.15f, 1.0f);
    fly_v3 wd = wspd > 0.5f ? fly_v3norm(fly_v3mk(e->wind.x, e->wind.y, 0)) : fly_v3mk(1, 0, 0);
    fly_v3 p = fly_v3mk(wx, wy, wz);
    fly_v3 view = fly_v3norm(fly_v3sub(eye, p));
    /* Wave detail is dropped octave by octave as it falls under what a pixel can
     * resolve, and the slope variance each dropped octave carried is handed to
     * the glint's lobe instead of being thrown away.
     *
     * Both halves matter. A 7 m ripple at three kilometres is a fifth of a
     * pixel, and a normal built from noise that fine, run through pow(n.h, 140)
     * and a clamped whitecap term, turns the sea into a field of isolated white
     * pixels that boils with any camera movement — which is what it did. But
     * simply removing it flattens a distant sea into a mirror, because the
     * roughness went somewhere: widening the lobe by exactly the variance that
     * left keeps the energy, and it is also the physics. Roughness too fine to
     * resolve geometrically is roughness. */
    float px = e->pxscale / (fly_v3dist(p, eye) + 1e-3f);
    float s1 = detail_amount(42.0f, px), s2 = detail_amount(14.0f, px);
    float s3 = detail_amount(7.7f, px), sf = detail_amount(8.5f, px);
    /* Two more octaves, and neither exists unless a pixel can hold it.
     *
     * The coarsest ripple here used to be six and a half metres, so water close
     * enough to see properly had nothing on it at all: a sheet with a slow
     * swell in it, which is why it read as glass. Real water is textured right
     * down to the centimetre. These are the two that matter at swimming
     * distance — a 1.6 m ripple and a 0.5 m one — and they cost nothing at
     * range because `detail_amount` returns zero there and the noise is
     * skipped outright rather than multiplied by zero. */
    float s4 = detail_amount(1.6f, px), s5 = detail_amount(0.55f, px);
    float a4 = 0.0f, a5 = 0.0f;
    float c4 = 0.055f * (0.4f + 0.6f * chopamp), c5 = 0.040f * (0.4f + 0.6f * chopamp);
    float a1 = fly_noise2(e->seed + 51, wx / 46.0f + t * 0.31f, wy / 39.0f - t * 0.22f) * s1;
    float a2 = fly_noise2(e->seed + 52, wx / 13.0f - t * 0.53f, wy / 15.0f + t * 0.44f) * s2;
    float a3 = fly_noise2(e->seed + 53, (wx * wd.x + wy * wd.y) / 6.5f - t * 1.3f,
                          (wx * -wd.y + wy * wd.x) / 9.0f) * s3;
    float c3 = 0.06f * chopamp, d3 = 0.04f * chopamp;
    float lost;
    if (s4 > 0.0f)
        a4 = fly_noise2(e->seed + 55, (wx * wd.x + wy * wd.y) / 1.6f - t * 2.4f,
                        (wx * -wd.y + wy * wd.x) / 2.1f + t * 0.5f) * s4;
    if (s5 > 0.0f)
        a5 = fly_noise2(e->seed + 56, wx / 0.55f + t * 3.6f, wy / 0.62f - t * 2.9f) * s5;
    lost = ((0.10f * 0.10f + 0.07f * 0.07f) * (1.0f - s1 * s1) +
            (0.05f * 0.05f + 0.05f * 0.05f) * (1.0f - s2 * s2) +
            (c3 * c3 + d3 * d3) * (1.0f - s3 * s3) +
            (c4 * c4 * 2.0f) * (1.0f - s4 * s4) +
            (c5 * c5 * 2.0f) * (1.0f - s5 * s5)) * 0.5f * FLY_NOISE_MS;
    fly_v3 nrm = fly_v3norm(fly_v3mk(a1 * 0.10f + a2 * 0.05f + a3 * c3 + a4 * c4 + a5 * c5,
                                     a1 * 0.07f - a2 * 0.05f + a3 * d3 - a4 * c4 + a5 * c5,
                                     1.0f));
    /* Schlick fresnel between the water body and the reflected sky */
    float fres = 0.04f + 0.96f * powf(1.0f - fly_clampf(fly_v3dot(view, nrm), 0, 1), 5.0f);
    /* The body of the water: the bed showing through, going to the deep tint as
     * it gets further away.
     *
     * Without this the shallows had no bottom — a metre of water over pale sand
     * came out the same dark blue-green as the open sea, so the only thing left
     * to see there was reflected sky, and against a bright overcast that turned
     * the entire shelf into a white sheet. It read as fog lying on the water and
     * it was the loudest thing about a coastline. Beer-Lambert with a metre and
     * a half of extinction: the bed is gone by knee depth and the turquoise it
     * leaves behind is what makes a coast look like a coast. FLY_WATER_BED
     * matches the sand terrain_surface lays down over the same band, so the
     * strip that is just dry and the strip that is just wet agree. */
    float clarity = expf(-fly_clampf(depth, 0.0f, 40.0f) / 1.1f);
    /* what comes back off the bed has been through the water twice, so it is
     * both dimmer and tinted: water absorbs red first, which is the whole
     * reason shallows over pale sand are turquoise and not simply bright */
    fly_v3 bed = fly_v3mul(FLY_WATER_BED, fly_v3mk(0.34f, 0.62f, 0.60f));
    bed = fly_v3scale(bed, 0.25f + 0.75f * e->day * cloud);
    fly_v3 deep = fly_v3lerp(fly_v3mk(0.02f, 0.07f, 0.10f), fly_v3mk(0.06f, 0.15f, 0.14f),
                             fly_clampf(1.0f - depth / 25.0f, 0, 1));
    fly_v3 body = fly_v3lerp(fly_v3scale(deep, 0.2f + e->day), bed, clarity);
    fly_v3 col = fly_v3lerp(body, sky, 0.22f + 0.68f * fres);
    /* sun glint sharpens in calm air, spreads in wind — and in the roughness
       the octaves above stopped resolving (Toksvig: m' = m/(1 + m*variance)) */
    float shine = fly_lerpf(140.0f, 40.0f, chopamp);
    fly_v3 hv = fly_v3norm(fly_v3add(view, e->sun));
    float spec;
    shine = shine / (1.0f + shine * lost * 8.0f);
    spec = powf(fly_clampf(fly_v3dot(nrm, hv), 0, 1), shine);
    col = fly_v3add(col, fly_v3scale(fly_v3mk(1.4f, 1.2f, 0.9f),
                                     spec * e->day * (1.0f - e->storm) * cloud));
    /* Surf, and whitecaps in wind. Both fade toward their own mean rather than
       toward zero, so a coast does not lose its surf as it recedes — it loses
       the speckle and keeps the brightness.

       Surf breaks in a band a few metres wide at the waterline, and the shape
       of the falloff is the whole difference between a coast and a painted
       stripe. The old term ramped over depth 0.15 m to 1.6 m with a 0.45 floor
       and a 0.85 ceiling, which on a shelf that shallow is forty metres of the
       shore held at four-fifths white — the coastline read as a bank of fog
       lying on the water. Squaring the ramp pulls the bright part into the last
       metre, the floor goes so that the noise decides where it breaks instead
       of merely modulating a stripe, and the ceiling comes down: foam is wet
       and lit, not a light source. */
    float edge = fly_smoothstepf(0.9f, 0.02f, depth);
    float run = 0.35f + 0.65f * fly_lerpf(0.5f, 0.5f + 0.5f *
                    fly_noise2(e->seed + 54, wx / 11.0f - t * 0.7f, wy / 10.0f), sf);
    float foam = edge * edge * run * 0.62f;
    /* Whitecaps break on the crests, not evenly over the sea.
     *
     * The old term spread foam across the water in proportion to the wind and
     * nothing else, which brightens everything and picks out nothing — a
     * whitecap is where a wave has got too steep to hold together. Steepness
     * is what is measured: the tilt the octaves above just built, thresholded
     * so only the top of a wave qualifies. It fades toward its own mean like
     * the surf does, so a distant sea keeps the brightness a crowd of caps
     * gives it and loses only the speckle it could not resolve anyway. */
    {
        float tilt = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y);
        float crest = fly_smoothstepf(0.055f, 0.16f, tilt * (0.6f + 0.4f * chopamp));
        float sc = s3 > s4 ? s3 : s4;
        foam += chopamp * chopamp * 0.55f * fly_lerpf(0.16f, crest, sc);
    }
    col = fly_v3lerp(col, fly_v3scale(fly_v3mk(0.90f, 0.93f, 0.95f), 0.25f + 0.75f * e->day),
                     fly_clampf(foam, 0.0f, 0.62f));
    return col;
}

/* Rasterizer entry: the shared surface, then this frame's fog. */
static fly_v3 water_shade(const fly__env *e, const fly__view *v, float wx, float wy, float wz,
                          float depth, fly_v3 sky, float cloud,
                          fly_v3 fogT, fly_v3 fogS) {
    fly_v3 col = water_surface(e, v->pos, wx, wy, wz, depth, sky, cloud);
    return fly_v3add(fly_v3mul(col, fogT), fogS);
}

/* Rasterize one water triangle, shading the surface per pixel and clipping
 * against the waterline so the shoreline follows the contour rather than
 * stepping along whole grid cells. */
static void raster_water_raw(fly_render_target *rt, const fly__view *v, const fly__env *e,
                             fly_v3 a, fly_v3 b, fly_v3 c,
                             const fly__wv *wa, const fly__wv *wb, const fly__wv *wc) {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    if (!project(v, rt->w, rt->h, a, &ax, &ay, &az)) return;
    if (!project(v, rt->w, rt->h, b, &bx, &by, &bz)) return;
    if (!project(v, rt->w, rt->h, c, &cx, &cy, &cz)) return;
    int minx = (int)fminf(fminf(ax, bx), cx), maxx = (int)fmaxf(fmaxf(ax, bx), cx);
    int miny = (int)fminf(fminf(ay, by), cy), maxy = (int)fmaxf(fmaxf(ay, by), cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= rt->w) maxx = rt->w - 1;
    if (maxy >= rt->h) maxy = rt->h - 1;
    if (minx > maxx || miny > maxy) return;
    float d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(d) < 1e-6f) return;
    float inv = 1.0f / d;
    FLY_PERSP_SETUP(v, az, bz, cz, ia, ib, ic, persp);
    int x, y;
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
            float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
            float uu = 1.0f - u - vv;
            float z;
            if (u < 0 || vv < 0 || uu < 0) continue;
            FLY_PERSPECTIVE(persp, uu, u, vv, az, bz, cz, ia, ib, ic, z);
            float depth = uu * wa->depth + u * wb->depth + vv * wc->depth;
            if (depth < -0.4f) continue;
            int idx = y * rt->w + x;
            if (z >= rt->depth[idx]) continue;
            fly_v3 sky = fly_v3mk(uu * wa->sky.x + u * wb->sky.x + vv * wc->sky.x,
                                  uu * wa->sky.y + u * wb->sky.y + vv * wc->sky.y,
                                  uu * wa->sky.z + u * wb->sky.z + vv * wc->sky.z);
            fly_v3 fT = fly_v3mk(uu * wa->fogT.x + u * wb->fogT.x + vv * wc->fogT.x,
                                 uu * wa->fogT.y + u * wb->fogT.y + vv * wc->fogT.y,
                                 uu * wa->fogT.z + u * wb->fogT.z + vv * wc->fogT.z);
            fly_v3 fS = fly_v3mk(uu * wa->fogS.x + u * wb->fogS.x + vv * wc->fogS.x,
                                 uu * wa->fogS.y + u * wb->fogS.y + vv * wc->fogS.y,
                                 uu * wa->fogS.z + u * wb->fogS.z + vv * wc->fogS.z);
            float cloud = uu * wa->cloud + u * wb->cloud + vv * wc->cloud;
            float wx = uu * a.x + u * b.x + vv * c.x, wy = uu * a.y + u * b.y + vv * c.y;
            float wz = uu * a.z + u * b.z + vv * c.z;
            rt->depth[idx] = z;
            rt->hdr[idx] = water_shade(e, v, wx, wy, wz, depth, sky, cloud, fT, fS);
        }
}

static void raster_tri_shadowed(fly_render_target *rt, const fly__view *v, const fly__env *e,
                                fly_v3 a, fly_v3 b, fly_v3 c,
                                fly_v3 la, fly_v3 lb, fly_v3 lc,
                                fly_v3 da, fly_v3 db, fly_v3 dc, float agl) {
    fly__cv in[3], out[6];
    int nt, k;
    phase_verts(3);
    if (g_capture) {
        fly_v3 nrm = fly_v3norm(fly_v3cross(fly_v3sub(b, a), fly_v3sub(c, a)));
        float nd = fly_v3dot(nrm, e->sun);
        /* orient toward the sun once here: fly_sm_offset must not push the
           sample into the surface, and the fragment stage has no sun to test */
        if (nd < 0.0f) { nrm = fly_v3scale(nrm, -1.0f); nd = -nd; }
        tribuf_tri(g_capture, a, b, c, la, lb, lc, da, db, dc, nrm, nd, agl);
        return;
    }
    in[0].p = a; in[1].p = b; in[2].p = c;
    in[0].a[0] = la.x; in[0].a[1] = la.y; in[0].a[2] = la.z;
    in[1].a[0] = lb.x; in[1].a[1] = lb.y; in[1].a[2] = lb.z;
    in[2].a[0] = lc.x; in[2].a[1] = lc.y; in[2].a[2] = lc.z;
    in[0].a[3] = da.x; in[0].a[4] = da.y; in[0].a[5] = da.z;
    in[1].a[3] = db.x; in[1].a[4] = db.y; in[1].a[5] = db.z;
    in[2].a[3] = dc.x; in[2].a[4] = dc.y; in[2].a[5] = dc.z;
    nt = near_clip_tri(v, in, out, 6);
    for (k = 0; k < nt; ++k) {
        const fly__cv *t = &out[k * 3];
        raster_tri_shadowed_raw(rt, v, e, t[0].p, t[1].p, t[2].p,
                                cv3(&t[0], 0), cv3(&t[1], 0), cv3(&t[2], 0),
                                cv3(&t[0], 3), cv3(&t[1], 3), cv3(&t[2], 3), agl);
    }
}

/* Object triangles light per-VERTEX (Gouraud): specular and fog are
 * evaluated at each corner and interpolated by the rasterizer, so shared
 * vertices shade identically and flat surfaces (pad decks, roofs) stay
 * smooth instead of showing per-triangle facets. The fog color (expensive:
 * sky sample) is taken once per primitive; only its amount varies.
 * A separate sun-occluded color is carried alongside the lit one so the
 * shadow map can cut crisp shadow edges across each face per pixel.
 *
 * `nrm` is three surface normals, one per corner, or NULL for the flat face
 * normal. That argument is what a curved hull is made of: a lofted fuselage has
 * always had the vertices of a cylinder and the *shading* of a prism, and no
 * number of sides fixes a prism — eight flat facets at eight sides, sixteen
 * flat facets at sixteen. Interpolating the surface normal across the facet
 * instead costs two vector adds a vertex, adds no triangles at all, and is the
 * difference between a shape that reads as round and one that reads as
 * chamfered. See loft_hull, which is where the normals come from. */
static void tri_lit_n(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 a, fly_v3 b, fly_v3 c, const fly_v3 *nrm, fly_v3 alb,
                      float shadow, float spec, float gloss) {
    /* A canopy template is leaves and nothing else — the bole keeps going
     * through the ordinary capture, one tree at a time. See the instance
     * lattice above. */
    if (g_wood_tmpl) return;
    if (g_shadow_cast) { shadow_cast_tri(g_shadow_cast, a, b, c); return; }
    fly_v3 face = fly_v3norm(fly_v3cross(fly_v3sub(b, a), fly_v3sub(c, a)));
    if (fly_v3dot(face, fly_v3sub(v->pos, a)) < 0.0f) face = fly_v3scale(face, -1.0f);
    /* the passed 'shadow' (wall/roof ambient-occlusion fudge) and the cloud
     * deck scale the sun term; the shadow map handles cast occlusion per pixel */
    fly_v3 mid = fly_v3scale(fly_v3add(fly_v3add(a, b), c), 1.0f / 3.0f);
    float sunlit = shadow * cloud_shadow_at(e, mid);
    /* Flat is the common case and it evaluates the ambient integral once for
     * the whole triangle; a smooth one has a different normal at every corner
     * and has to pay for three. */
    int smooth = nrm != NULL;
    fly_v3 base_lit = smooth ? fly_v3zero() : lit_surface(e, alb, face, sunlit, 1.0f);
    fly_v3 base_dark = smooth ? fly_v3zero() : lit_surface(e, alb, face, 0.0f, 1.0f);
    int shadowed = e->shadow && e->shadow->active;
    fly_v3 mdir = fly_v3sub(mid, v->pos);
    float mdist = fly_v3len(mdir);
    /* A captured triangle is fogged by FLY_ENV_OBJ_FS, per pixel, off the
     * position the fragment is actually at — see that shader. */
    int fogged = !g_capture && mdist > 400.0f;
    float alpha = ggx_alpha(gloss);
    fly_v3 p[3] = { a, b, c };
    fly_v3 lit[3], dark[3], fT[3], fS[3];
    int i;
    if (fogged) fog_tri(e, v, p, fT, fS);
    for (i = 0; i < 3; ++i) {
        fly_v3 n = face;
        fly_v3 cl, cd;
        if (smooth) {
            n = nrm[i];
            /* A normal that has turned past the silhouette shades a corner
             * with light from behind the surface it is on. Bending it back to
             * the horizon rather than to the face keeps the gradient going —
             * clamping to the face is what puts a hard flat band around the
             * edge of a smooth cylinder. */
            {
                fly_v3 tov = fly_v3sub(v->pos, p[i]);
                float tl = fly_v3len(tov);
                if (tl > 1e-5f) {
                    fly_v3 vd = fly_v3scale(tov, 1.0f / tl);
                    float nv = fly_v3dot(n, vd);
                    if (nv < 0.0f) n = fly_v3norm(fly_v3sub(n, fly_v3scale(vd, nv * 1.02f)));
                }
            }
            cl = lit_surface(e, alb, n, sunlit, 1.0f);
            cd = lit_surface(e, alb, n, 0.0f, 1.0f);
        } else {
            cl = base_lit;
            cd = base_dark;
        }
        if (spec > 0.0f) {
            fly_v3 view = fly_v3norm(fly_v3sub(v->pos, p[i]));
            fly_v3 hv = fly_v3norm(fly_v3add(view, e->sun));
            float ndl = fly_clampf(fly_v3dot(n, e->sun), 0.0f, 1.0f);
            float ndv = fly_clampf(fly_v3dot(n, view), 0.0f, 1.0f);
            float ndh = fly_clampf(fly_v3dot(n, hv), 0.0f, 1.0f);
            float vdh = fly_clampf(fly_v3dot(view, hv), 0.0f, 1.0f);
            /* Painted metal, not metal. `spec` is the material's specular
             * level on the usual 0..1 scale, where 0.5 is an ordinary
             * dielectric — so the map to normal-incidence reflectance is
             * 0.08*spec, putting that ordinary case at the 4% every painted,
             * glazed or plastic surface actually has. It is not the old
             * meaning: `spec` used to scale the whole lobe, which made a wing
             * with spec 0.55 reflect like polished metal because nothing
             * downstream ever applied a Fresnel to it.
             *
             * The ceiling the grazing Fresnel may climb to is the split-sum one
             * — a rough surface never reaches a mirror — rather than the 1.0 a
             * bare Schlick would hand it. */
            float f0 = spec * 0.08f;
            float fcap = 1.0f - alpha;
            float fres = schlick_f(f0, fcap, vdh);
            float lobe = ggx_sun(ndl, ndv, ndh, alpha) * fres * sunlit;
            /* The sun is not the only thing a surface reflects, and on an
             * overcast afternoon it is not even the brightest. The other half of
             * the specular integral is the sky itself, taken in the mirror
             * direction against the same irradiance basis the diffuse uses. It
             * is the low-frequency part of the reflection and honestly so — a
             * cosine-convolved sky is far too blurry to be a mirror, and nothing
             * here is a mirror except the water, which traces its own. What it
             * buys is the thing a constant cannot: a fuselage picking up a
             * bright rim against the sky, and a tank's flank lightening as it
             * turns away. Both terms are added, not mixed: they are different
             * light. */
            float env = ggx_env(f0, ndv, alpha);
            fly_v3 refl = fly_v3sub(fly_v3scale(n, 2.0f * fly_v3dot(n, view)), view);
            fly_v3 sky = fly_v3scale(sky_irradiance(e, fly_v3norm(refl)), env);
            cl = fly_v3add(cl, fly_v3scale(fly_v3mul(fly_v3mk(1.0f, 0.96f, 0.88f), e->sunlight),
                                           lobe));
            cl = fly_v3add(cl, sky);
            cd = fly_v3add(cd, sky);
        }
        if (fogged) {
            cl = fly_v3add(fly_v3mul(cl, fT[i]), fS[i]);
            cd = fly_v3add(fly_v3mul(cd, fT[i]), fS[i]);
        }
        lit[i] = cl;
        dark[i] = cd;
    }
    if (shadowed)
        raster_tri_shadowed(rt, v, e, a, b, c, lit[0], lit[1], lit[2],
                            dark[0], dark[1], dark[2], tri_agl(e, mid));
    else
        raster_tri_hdr(rt, v, a, b, c, lit[0], lit[1], lit[2]);
}

static void tri_lit(fly_render_target *rt, const fly__view *v, const fly__env *e,
                    fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 alb, float shadow,
                    float spec, float gloss) {
    tri_lit_n(rt, v, e, a, b, c, NULL, alb, shadow, spec, gloss);
}

/* Foliage shading. A crown is not a stack of flat cards, and the two things
 * that made it look like one cost nothing to fix.
 *
 *  - The normal comes from the crown's centre, not from the triangle. A
 *    crossed billboard's geometric normal is horizontal and gets flipped
 *    toward the camera, so a spruce was lit identically whether the sun was
 *    overhead or on the horizon, and both halves of the cross came out the
 *    same colour. A centre-out normal lights a crown as the roughly spherical
 *    mass of needles it is — bright on the sun side, dark opposite, bright on
 *    top under a high sun — and being per vertex it shades smoothly across a
 *    facet instead of flat, which is most of what "detailed" means here.
 *
 *  - Leaves use the canopy BRDF the ground already uses for grass, so they
 *    wrap around the terminator, surge when you look down-sun, and transmit:
 *    a crown between you and the sun glows green-gold instead of going black.
 *    That is `terrain_light` with vegetation set to 1, not a new model.
 *
 * The dark half of the shadow-map cross-fade is `lit_surface` at shadow 0,
 * which is exactly terrain_light's ambient term — not an approximation of it —
 * so the pair stays exact while costing one powf per vertex rather than two. */
static void tri_leaf(fly_render_target *rt, const fly__view *v, const fly__env *e,
                     fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 alb, fly_v3 centre, float ao) {
    /* (vegetation, f0, blinn exponent): leaves are waxy, so a little sheen */
    const fly_v3 leaf_mat = { 1.0f, 0.022f, 12.0f };
    fly_v3 p[3], lit[3], dark[3], fT[3], fS[3];
    int fogged;
    fly_v3 mid = fly_v3scale(fly_v3add(fly_v3add(a, b), c), 1.0f / 3.0f);
    float sunlit, mdist;
    int i;
    /* Capturing a canopy template: this function's arguments already *are* the
     * template — a position, the hub the normal runs out from, an albedo and an
     * occlusion term — so the model builds it by running unchanged. The leaf
     * colour is white for the capture, which makes `alb` the scalar multiplier
     * every call in `draw_tree_model` applies to it. See the lattice above. */
    if (g_wood_tmpl) {
        woodbuf_vert(g_wood_tmpl, a, centre, alb.x, ao);
        woodbuf_vert(g_wood_tmpl, b, centre, alb.x, ao);
        woodbuf_vert(g_wood_tmpl, c, centre, alb.x, ao);
        return;
    }
    /* ...and the mirror of it: the pass that draws only what the template left
     * behind, for a tree whose crown an instance is carrying. */
    if (g_wood_bole_only) return;
    if (g_shadow_cast) { shadow_cast_tri(g_shadow_cast, a, b, c); return; }
    sunlit = cloud_shadow_at(e, mid);
    mdist = fly_v3len(fly_v3sub(mid, v->pos));
    fogged = !g_capture && mdist > 400.0f;
    p[0] = a; p[1] = b; p[2] = c;
    if (fogged) fog_tri(e, v, p, fT, fS);
    for (i = 0; i < 3; ++i) {
        fly_v3 d = fly_v3sub(p[i], centre);
        float dl = fly_v3len(d);
        fly_v3 n = dl > 1e-3f ? fly_v3scale(d, 1.0f / dl) : fly_v3mk(0, 0, 1);
        fly_v3 vdir = fly_v3norm(fly_v3sub(v->pos, p[i]));
        /* lift the normal: a crown's outer surface faces up as well as out,
           and a purely radial normal leaves the underside lit from below */
        n = fly_v3norm(fly_v3add(n, fly_v3mk(0, 0, 0.35f)));
        lit[i] = terrain_light(e, alb, leaf_mat, n, vdir, sunlit, ao);
        dark[i] = lit_surface(e, alb, n, 0.0f, ao);
        if (fogged) {
            lit[i] = fly_v3add(fly_v3mul(lit[i], fT[i]), fS[i]);
            dark[i] = fly_v3add(fly_v3mul(dark[i], fT[i]), fS[i]);
        }
    }
    if (e->shadow && e->shadow->active)
        raster_tri_shadowed(rt, v, e, a, b, c, lit[0], lit[1], lit[2],
                            dark[0], dark[1], dark[2], tri_agl(e, mid));
    else
        raster_tri_hdr(rt, v, a, b, c, lit[0], lit[1], lit[2]);
}

static void tri_shaded(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 alb, float shadow) {
    tri_lit(rt, v, e, a, b, c, alb, shadow, 0.0f, 1.0f);
}

/* ---------------- translucent geometry ---------------- */

/* A turning propeller is the one surface in this renderer that has to be seen
 * through, and neither rasterizer blends — both write an opaque colour wherever
 * they pass the depth test. That is not an oversight to route around locally:
 * an earlier attempt passed an alpha into `tri_shaded`, whose last argument is
 * an ambient-occlusion term, and bolted an opaque plate to the nose.
 *
 * So the few triangles that need alpha are recorded here instead, shaded and
 * fogged at the moment they are built, and composited once at the end of the
 * frame against the finished z-buffer — depth-tested, writing no depth of their
 * own. Deferring rather than blending in place is what makes the ordering right
 * in every pipeline: in pure raster the objects are on the GPU and there is no
 * z-buffer to test against until the readback, and in the traced modes a craft
 * drawn later would otherwise paint over a disc in front of it.
 *
 * The list is composited in build order rather than sorted back to front. The
 * disc's own bands do not overlap and its alphas are a few per cent, so the
 * error where two do is well under a quantisation step; a case that needs more
 * than that needs a sort, not a bigger constant. */
#define FLY_BLEND_MAX 2048

typedef struct {
    fly_v3 p[3];
    fly_v3 col;      /* already lit and fogged */
    float alpha[3];  /* per vertex: a disc's opacity is a gradient in radius */
} fly__blendtri;

static fly__blendtri g_blend[FLY_BLEND_MAX];
static int g_nblend = 0;

/* depth-tested, no depth write, src-over into the HDR buffer */
static void blend_tri_raw(fly_render_target *rt, const fly__view *v,
                          fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 col, const float *al) {
    float ax, ay, az, bx, by, bz, cx, cy, cz, d, inv;
    int minx, maxx, miny, maxy, x, y;
    if (!project(v, rt->w, rt->h, a, &ax, &ay, &az)) return;
    if (!project(v, rt->w, rt->h, b, &bx, &by, &bz)) return;
    if (!project(v, rt->w, rt->h, c, &cx, &cy, &cz)) return;
    minx = (int)fminf(fminf(ax, bx), cx); maxx = (int)fmaxf(fmaxf(ax, bx), cx);
    miny = (int)fminf(fminf(ay, by), cy); maxy = (int)fmaxf(fmaxf(ay, by), cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= rt->w) maxx = rt->w - 1;
    if (maxy >= rt->h) maxy = rt->h - 1;
    if (minx > maxx || miny > maxy) return;
    d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(d) < 1e-6f) return;
    inv = 1.0f / d;
    FLY_PERSP_SETUP(v, az, bz, cz, ia, ib, ic, persp);
    for (y = miny; y <= maxy; ++y)
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
            float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
            float uu = 1.0f - u - vv;
            float z;
            int idx;
            if (u < 0 || vv < 0 || uu < 0) continue;
            FLY_PERSPECTIVE(persp, uu, u, vv, az, bz, cz, ia, ib, ic, z);
            idx = y * rt->w + x;
            if (z >= rt->depth[idx]) continue;
            rt->hdr[idx] = fly_v3lerp(rt->hdr[idx], col,
                                      uu * al[0] + u * al[1] + vv * al[2]);
        }
}

static void blend_flush(fly_render_target *rt, const fly__view *v) {
    int i, k;
    for (i = 0; i < g_nblend; ++i) {
        const fly__blendtri *t = &g_blend[i];
        fly__cv in[3], out[6];
        int nt, j;
        for (j = 0; j < 3; ++j) { in[j].p = t->p[j]; in[j].a[0] = t->alpha[j]; }
        nt = near_clip_tri(v, in, out, 1);
        for (k = 0; k < nt; ++k) {
            const fly__cv *q = &out[k * 3];
            float al[3] = { q[0].a[0], q[1].a[0], q[2].a[0] };
            blend_tri_raw(rt, v, q[0].p, q[1].p, q[2].p, t->col, al);
        }
    }
    g_nblend = 0;
}

/* Shade a triangle the way tri_lit would, then queue it instead of filling it.
 * Sun, cloud deck and aerial perspective all apply — a prop disc on an aircraft
 * two kilometres off has to sit in the same haze its wings do. */
static void blend_shaded(const fly__view *v, const fly__env *e,
                         fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 alb,
                         float a0, float a1, float a2) {
    fly_v3 n, mid, col, d;
    float dist;
    if (g_shadow_cast) return;   /* you can see through it; it casts nothing */
    if (g_nblend >= FLY_BLEND_MAX) return;
    if (a0 <= 0.004f && a1 <= 0.004f && a2 <= 0.004f) return;
    n = fly_v3norm(fly_v3cross(fly_v3sub(b, a), fly_v3sub(c, a)));
    if (fly_v3dot(n, fly_v3sub(v->pos, a)) < 0.0f) n = fly_v3scale(n, -1.0f);
    mid = fly_v3scale(fly_v3add(fly_v3add(a, b), c), 1.0f / 3.0f);
    col = lit_surface(e, alb, n, cloud_shadow(e, mid), 1.0f);
    d = fly_v3sub(mid, v->pos);
    dist = fly_v3len(d);
    if (dist > 400.0f) {
        fly_v3 fT, fS;
        fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist, &fT, &fS);
        col = fly_v3add(fly_v3mul(col, fT), fS);
    }
    {
        fly__blendtri *o = &g_blend[g_nblend++];
        o->p[0] = a; o->p[1] = b; o->p[2] = c;
        o->col = col;
        o->alpha[0] = fly_clampf(a0, 0.0f, 1.0f);
        o->alpha[1] = fly_clampf(a1, 0.0f, 1.0f);
        o->alpha[2] = fly_clampf(a2, 0.0f, 1.0f);
    }
}

/* The same queue, given radiance instead of an albedo to light.
 *
 * `blend_shaded` orients the normal at the *viewer* and lights that, which is
 * right for a prop disc and wrong for anything that makes its own light or is
 * lit from a direction of its own. A fireball is emissive and has no normal
 * worth speaking of; a smoke column is lit from above whatever the camera is
 * doing, and asking blend_shaded for one produced 0.009 of linear grey — a puff
 * that was technically drawn and completely invisible. Aerial perspective still
 * applies, because a plume two kilometres off has to sit in the same haze the
 * terrain does. */
static void blend_radiance(const fly__view *v, const fly__env *e,
                           fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 rad,
                           float a0, float a1, float a2) {
    fly_v3 mid, d;
    float dist;
    if (g_shadow_cast) return;
    if (g_nblend >= FLY_BLEND_MAX) return;
    if (a0 <= 0.004f && a1 <= 0.004f && a2 <= 0.004f) return;
    mid = fly_v3scale(fly_v3add(fly_v3add(a, b), c), 1.0f / 3.0f);
    d = fly_v3sub(mid, v->pos);
    dist = fly_v3len(d);
    if (dist > 400.0f) {
        fly_v3 fT, fS;
        fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist, &fT, &fS);
        rad = fly_v3add(fly_v3mul(rad, fT), fS);
    }
    {
        fly__blendtri *o = &g_blend[g_nblend++];
        o->p[0] = a; o->p[1] = b; o->p[2] = c;
        o->col = rad;
        o->alpha[0] = fly_clampf(a0, 0.0f, 1.0f);
        o->alpha[1] = fly_clampf(a1, 0.0f, 1.0f);
        o->alpha[2] = fly_clampf(a2, 0.0f, 1.0f);
    }
}

/* ---------------- lamplight ----------------------------------------------
 *
 * What a lamp puts on the ground — and it is not a shape laid on the ground.
 *
 * It used to be one: an ellipse of translucent triangles, seated at the
 * surface's own grade a hand's breadth above it and depth-tested like any
 * other geometry. Every fault it had came out of that single decision. A flat
 * ellipse twenty metres long lies on exactly one plane, and the ground under a
 * road lamp is three — the carriageway, the batter the column stands on, and
 * the field beside it — so the far half of the pool went into the tarmac, the
 * outer half floated over the verge, and what the depth test left was a bright
 * patch with a torn edge that stopped where the embankment started. It was
 * mixed into the frame rather than added to it, so two lamps overlapping came
 * out no brighter than one and a pool over anything already lit *darkened* it.
 * And its falloff was a linear fan from a peak to a rim, which is a cone, and a
 * cone reads as an airbrushed disc painted on the road.
 *
 * So the light is deferred instead. A lamp is queued as what it physically is
 * — a source at a point, throwing a distribution, reaching so far — and once
 * the frame has a depth buffer, every pixel inside its reach is unprojected to
 * the surface the frame actually drew there and lit by it. Nothing is laid on
 * anything: the light falls on the carriageway, the kerb, the batter, the
 * grass, the column, the truck going past and the girder over the top, because
 * all of those are what the depth buffer says is in front of the lamp. There
 * is no edge to tear because there is no polygon, and nothing to z-fight with
 * because nothing is drawn.
 *
 * The distribution is a semi-cutoff road lantern's, which is most of what makes
 * it read as lighting rather than as a spot. Intensity climbs off the nadir
 * (FLY_LAMP_WING), so the pool stays even out to a couple of mounting heights
 * instead of collapsing as the cube of the cosine the way a bare point source
 * would; and it climbs further along the way the lamp is aimed than across it
 * (FLY_LAMP_ACROSS), which is what lays a long pool down a road instead of a
 * circle on it. Times cos/d^2 for the surface, which is the inverse-square law,
 * and between them they give a bright core with a skirt that fades to nothing
 * without ever showing a rim.
 *
 * The surface is taken as horizontal. A normal off the depth buffer is a
 * screen-space derivative, and it is wrong at exactly the silhouettes a frame
 * full of lamp-posts is made of; what a lamp actually lights is the ground, the
 * deck and the verge, and those are horizontal to within the cosine's own
 * error. It costs nothing and it cannot speckle.
 *
 * Cost is bounded twice: by the lamp's own screen circle, and inside it by the
 * one exact rejection a view depth allows. The distance from the lantern to a
 * surface is at least the difference between their view depths, so a pixel
 * whose depth is further than `reach` from the lamp's cannot be lit and is
 * rejected in two compares — which is every sky pixel and nearly every ground
 * one. That is why a lamp the camera is standing under costs about what its
 * pool covers rather than what its circle does. */

/* A night frame over the network is the worst case and it is not close to
 * this: the chain thins with distance (`lamp_stride`), so the most any frame in
 * the suite queues is 429 — a road seen end-on from seven hundred metres, where
 * every lamp on it for twenty-six kilometres is inside the cone. The ceiling is what a frame
 * that found more would lose, and it is a lamp's worth of memory apiece. */
#define FLY_LAMP_MAX 1024
/* How much brighter a lantern is well off its own nadir than straight down
 * under itself. A bare point source falls off as cos^3 over a plane and lights
 * a disc a mounting height across; a semi-cutoff lantern is built to fight
 * exactly that, and this is the fight. */
#define FLY_LAMP_WING 3.4f
/* ...and how much of that boost the across-axis gets. A road lantern throws
 * down the road, not over the fence. */
#define FLY_LAMP_ACROSS 0.22f
/* The near singularity, in square metres. Nothing is ever this close to a
 * lantern that is metres up a pole, so it only ever keeps the arithmetic
 * honest. */
#define FLY_LAMP_SOFT 0.75f

typedef struct {
    fly_v3 p;      /* the lantern */
    fly_v3 ax;     /* unit, horizontal: the way it throws */
    fly_v3 rad;    /* linear radiance at the nadir, times the drop, fogged */
    float reach;   /* metres; past it the lamp contributes nothing */
} fly__lamp;

static fly__lamp g_lamp[FLY_LAMP_MAX];
static int g_nlamp = 0;

/* Queue a lantern at `p`, throwing along `yaw`.
 *
 * `power` is the radiance it lays on level ground `drop` metres directly below
 * it — quoting the lamp by what it puts on the road rather than by an emitter
 * strength is what lets a truck's headlamps, a road column and a lamp thirteen
 * metres up a viaduct be written in the same units. `reach` is where it stops.
 */
static void lamp_light(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 p, float yaw, float drop, float reach,
                       fly_v3 tint, float power) {
    fly_v3 d;
    float dist;
    if (g_shadow_cast) return;              /* light casts nothing */
    if (g_nlamp >= FLY_LAMP_MAX) return;
    if (power <= 0.003f || drop <= 0.05f || reach <= 0.5f) return;
    /* Wet tarmac under a lamp is the brightest thing in a rainy night frame:
       the film is a dielectric mirror and most of what lands on it comes back
       up instead of scattering away into the aggregate. */
    power *= 1.0f + 0.45f * e->wet;
    reach *= 1.0f + 0.20f * e->wet;
    if (!sphere_visible(v, rt->w, rt->h, p, reach)) return;
    d = fly_v3sub(p, v->pos);
    dist = fly_v3len(d);
    tint = fly_v3scale(tint, power * drop * drop);
    /* Aerial perspective, and only the transmittance half of it: what the air
       between adds is already in the surface this is landing on, and a lamp
       does not get to add it a second time. */
    if (dist > 400.0f) {
        fly_v3 fT, fS;
        fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist, &fT, &fS);
        tint = fly_v3mul(tint, fT);
        (void)fS;
    }
    {
        fly__lamp *o = &g_lamp[g_nlamp++];
        o->p = p;
        o->ax = fly_v3mk(cosf(yaw), sinf(yaw), 0.0f);
        o->rad = tint;
        o->reach = reach;
    }
}

/* The pixels a lamp can possibly touch, as a rectangle in image pixels.
 *
 * One definition, because two passes ask: the software flush below and the
 * quad `gpu_overlay` hands the fragment stage, and a bound computed twice is a
 * bound that can disagree with itself. Returns 0 when the lamp is off-frame.
 *
 * The circle of a sphere of radius `reach` at view depth `zl`, which is exact
 * — until the camera is inside the reach, where there is no circle and the
 * answer is the frame. That case is not the disaster it looks: the depth
 * rejection inside costs two compares and throws out every pixel further from
 * the lamp than its reach, which off a pool is nearly all of them. */
static int lamp_rect(const fly__view *v, int w, int h, const fly__lamp *L, float *r) {
    float zl = fly_v3dot(fly_v3sub(L->p, v->pos), v->fwd);
    float x0 = 0.0f, y0 = 0.0f, x1 = (float)w, y1 = (float)h;
    if (v->ortho || zl > L->reach + 0.5f) {
        float sx, sy, z, rr;
        if (!project(v, w, h, L->p, &sx, &sy, &z)) return 0;
        rr = v->ortho ? L->reach * v->sy
                      : L->reach * v->sy / sqrtf(zl * zl - L->reach * L->reach);
        x0 = sx - rr; x1 = sx + rr;
        y0 = sy - rr; y1 = sy + rr;
        if (x0 < 0.0f) x0 = 0.0f;
        if (y0 < 0.0f) y0 = 0.0f;
        if (x1 > (float)w) x1 = (float)w;
        if (y1 > (float)h) y1 = (float)h;
        if (x0 >= x1 || y0 >= y1) return 0;
    } else if (zl < -L->reach) return 0;
    r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
    return 1;
}

/* Every queued lamp, laid over the frame the depth buffer has just finished.
 * Added, never mixed: light is additive, which is the whole of why two lamps
 * overlap into a brighter patch and why light over a lit surface does not dim
 * it. Twin of FLY_LAMP_FS, which does the same arithmetic per fragment. */
static void lamp_flush(fly_render_target *rt, const fly__view *v) {
    float cx = (float)rt->w * 0.5f, cy = (float)rt->h * 0.5f;
    int i;
    for (i = 0; i < g_nlamp; ++i) {
        const fly__lamp *L = &g_lamp[i];
        fly_v3 sd = fly_v3mk(-L->ax.y, L->ax.x, 0.0f);
        float zl = fly_v3dot(fly_v3sub(L->p, v->pos), v->fwd);
        float r2 = L->reach * L->reach;
        float rect[4];
        int x0, y0, x1, y1, x, y;
        if (!lamp_rect(v, rt->w, rt->h, L, rect)) continue;
        x0 = (int)floorf(rect[0]);
        y0 = (int)floorf(rect[1]);
        x1 = (int)ceilf(rect[2]) - 1;
        y1 = (int)ceilf(rect[3]) - 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= rt->w) x1 = rt->w - 1;
        if (y1 >= rt->h) y1 = rt->h - 1;
        for (y = y0; y <= y1; ++y) {
            /* The ray through this row of pixels, carried one unit of view
               depth per unit of length, so a surface at depth z is exactly z of
               these from the eye. Under a parallel projection it is the origin
               that moves across the frame and the direction that does not. */
            float uy = -((float)y + 0.5f - cy) / v->sy;
            fly_v3 rowb = v->ortho ? fly_v3add(v->pos, fly_v3scale(v->up, uy)) : v->pos;
            fly_v3 rowd = v->ortho ? v->fwd : fly_v3add(v->fwd, fly_v3scale(v->up, uy));
            for (x = x0; x <= x1; ++x) {
                int idx = y * rt->w + x;
                float zs = rt->depth[idx];
                float dz = zs - zl;
                float ux, q2, inv, h, a, b, lat, gain, t;
                fly_v3 s, dv;
                /* the sky included: its depth is further off than any reach */
                if (dz > L->reach || dz < -L->reach) continue;
                ux = ((float)x + 0.5f - cx) / v->sx;
                s = v->ortho
                        ? fly_v3add(fly_v3add(rowb, fly_v3scale(v->right, ux)),
                                    fly_v3scale(rowd, zs))
                        : fly_v3add(rowb,
                                    fly_v3scale(fly_v3add(rowd, fly_v3scale(v->right, ux)), zs));
                dv = fly_v3sub(s, L->p);
                q2 = fly_v3dot(dv, dv);
                if (q2 >= r2) continue;
                inv = 1.0f / sqrtf(q2 + 1e-6f);
                h = -dv.z * inv;               /* the surface's cosine, taken level */
                if (h <= 0.0f) continue;       /* nothing above the lantern is lit */
                a = (dv.x * L->ax.x + dv.y * L->ax.y) * inv;
                b = (dv.x * sd.x + dv.y * sd.y) * inv;
                lat = a * a + b * b * FLY_LAMP_ACROSS;
                gain = (1.0f + FLY_LAMP_WING * lat * lat) * h / (q2 + FLY_LAMP_SOFT);
                /* and to nothing at the reach, so the circle never shows */
                t = 1.0f - q2 / r2;
                rt->hdr[idx] = fly_v3add(rt->hdr[idx], fly_v3scale(L->rad, gain * t * t));
            }
        }
    }
    g_nlamp = 0;
}

static void tri_spec(fly_render_target *rt, const fly__view *v, const fly__env *e,
                     fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 alb, float shadow,
                     float spec, float gloss) {
    tri_lit(rt, v, e, a, b, c, alb, shadow, spec, gloss);
}

/* vertical-gradient wall quad: lit at the top, ambient-occluded at the base;
 * fog per vertex so adjacent walls meet without banding */
static void wall_quad(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 b0, fly_v3 b1, fly_v3 t1, fly_v3 t0, fly_v3 alb, float shade) {
    if (g_shadow_cast) {
        shadow_cast_tri(g_shadow_cast, b0, b1, t1);
        shadow_cast_tri(g_shadow_cast, b0, t1, t0);
        return;
    }
    fly_v3 n = fly_v3norm(fly_v3cross(fly_v3sub(b1, b0), fly_v3sub(t0, b0)));
    if (fly_v3dot(n, fly_v3sub(v->pos, b0)) < 0.0f) n = fly_v3scale(n, -1.0f);
    fly_v3 mid = fly_v3scale(fly_v3add(fly_v3add(b0, b1), fly_v3add(t0, t1)), 0.25f);
    float sunlit = 0.92f * shade * cloud_shadow(e, mid);
    fly_v3 top_lit = lit_surface(e, alb, n, sunlit, 1.0f);
    fly_v3 top_dark = lit_surface(e, alb, n, 0.0f, 1.0f);
    fly_v3 bot_lit = fly_v3scale(top_lit, 0.55f), bot_dark = fly_v3scale(top_dark, 0.55f);
    fly_v3 mdir = fly_v3sub(fly_v3scale(fly_v3add(b0, t1), 0.5f), v->pos);
    float mdist = fly_v3len(mdir);
    fly_v3 p[4] = { b0, b1, t1, t0 };
    fly_v3 lit[4] = { bot_lit, bot_lit, top_lit, top_lit };
    fly_v3 dark[4] = { bot_dark, bot_dark, top_dark, top_dark };
    int shadowed = e->shadow && e->shadow->active;
    int i;
    int fogged = mdist > 400.0f;
    for (i = 0; i < 4; ++i) {
        if (fogged) {
            fly_v3 d = fly_v3sub(p[i], v->pos), fT, fS;
            float dist = fly_v3len(d);
            fog_pair(e, v->pos, fly_v3scale(d, 1.0f / (dist + 1e-5f)), dist, &fT, &fS);
            lit[i] = fly_v3add(fly_v3mul(lit[i], fT), fS);
            dark[i] = fly_v3add(fly_v3mul(dark[i], fT), fS);
        }
    }
    if (shadowed) {
        float agl = tri_agl(e, fly_v3scale(fly_v3add(fly_v3add(b0, b1), fly_v3add(t0, t1)), 0.25f));
        raster_tri_shadowed(rt, v, e, b0, b1, t1, lit[0], lit[1], lit[2],
                            dark[0], dark[1], dark[2], agl);
        raster_tri_shadowed(rt, v, e, b0, t1, t0, lit[0], lit[2], lit[3],
                            dark[0], dark[2], dark[3], agl);
    } else {
        raster_tri_hdr(rt, v, b0, b1, t1, lit[0], lit[1], lit[2]);
        raster_tri_hdr(rt, v, b0, t1, t0, lit[0], lit[2], lit[3]);
    }
}

/* ---------------- sky pass ---------------- */

/* The sky, sampled on a lattice and refined where refining changes anything.
 *
 * A sky pixel is the most expensive pixel in a software frame: a full
 * scattering integral, the cloud deck, the planet's own limb and the stars, and
 * on an alpine vista in the budget preset that one function was over half of
 * the frame. It is also, over most of its area, the smoothest thing in the
 * frame — a gradient that takes a hundred pixels to change by one code value.
 * Sampling every pixel of it is paying full price for a bilinear interpolation.
 *
 * So a coarse lattice is evaluated once, shared by the four cells that meet at
 * each point, and a cell is filled by interpolating its four corners when
 * interpolation is right. What decides that is *curvature*, not spread: the
 * whole sky is a gradient, so the corners of any cell differ by a lot, and the
 * first version of this tested exactly that and refined ninety per cent of the
 * frame for nothing. A linear ramp is what bilinear interpolation reproduces
 * exactly. So the cell's centre is evaluated too and compared against what
 * interpolating the corners would have put there: agree, and the cell is a
 * ramp and gets filled; disagree — the sun's disc and halo, a cloud edge, the
 * horizon, the planet's limb — and it is evaluated pixel by pixel exactly as
 * before. Relative to the cell's own brightness, so it neither goes blind at
 * dusk nor refines everything at noon.
 *
 * Two things it deliberately will not do. It does not run at night, because a
 * star is a sub-pixel feature no lattice can see and interpolating would erase
 * the sky's entire content. And the step is in *output* pixels — divided by the
 * supersample factor — because supersampling is meant to sample the same scene
 * more finely, not to hand the interpolator a coarser one. */
static fly_v3 sky_at(const fly__env *e, const fly__view *v, int w, int h,
                     float x, float y) {
    fly_v3 rd = fly_v3norm(fly_v3add(v->fwd,
                    fly_v3add(fly_v3scale(v->right, (x - (float)w * 0.5f) / v->sx),
                              fly_v3scale(v->up, ((float)h * 0.5f - y) / v->sy))));
    return sky_radiance(e, v->pos, rd);
}

/* Is any of this block still showing sky? The environment draws the ground
 * first now, so most of a valley view never asks the atmosphere anything. */
static int sky_block_open(const fly_render_target *rt, int x0, int y0, int x1, int y1) {
    int x, y;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            if (rt->depth[y * rt->w + x] >= 1e29f) return 1;
    return 0;
}

static void raster_sky(fly_render_target *rt, const fly__env *e, const fly__view *v) {
    /* the lattice, kept between frames: it is a few tens of kilobytes and the
     * frame size rarely changes */
    static fly_v3 *grid;
    static unsigned char *have;
    static int gw, gh, gstep;
    int step = e->lod.sky / (v->aa > 0 ? v->aa : 1);
    int x, y, cx, cy, nx, ny;
    if (step < 1 || e->sun.z < 0.06f) step = 1;
    if (step == 1) {
        for (y = 0; y < rt->h; ++y)
            for (x = 0; x < rt->w; ++x)
                if (rt->depth[y * rt->w + x] >= 1e29f)
                    rt->hdr[y * rt->w + x] = sky_at(e, v, rt->w, rt->h,
                                                    (float)x, (float)y);
        return;
    }
    nx = (rt->w + step - 1) / step + 1;
    ny = (rt->h + step - 1) / step + 1;
    if (nx != gw || ny != gh || step != gstep) {
        fly_v3 *ng = (fly_v3 *)realloc(grid, (size_t)nx * (size_t)ny * sizeof *ng);
        unsigned char *nh = (unsigned char *)realloc(have, (size_t)nx * (size_t)ny);
        if (ng) grid = ng;
        if (nh) have = nh;
        if (!ng || !nh) { gw = gh = gstep = 0; return; }
        gw = nx; gh = ny; gstep = step;
    }
    memset(have, 0, (size_t)nx * (size_t)ny);
#define FLY_SKY_GRID(gx_, gy_)                                                \
    (have[(gy_) * nx + (gx_)] ? grid[(gy_) * nx + (gx_)]                       \
     : (have[(gy_) * nx + (gx_)] = 1,                                          \
        grid[(gy_) * nx + (gx_)] =                                             \
            sky_at(e, v, rt->w, rt->h,                                         \
                   (float)((gx_) * step < rt->w ? (gx_) * step : rt->w - 1),   \
                   (float)((gy_) * step < rt->h ? (gy_) * step : rt->h - 1))))
    for (cy = 0; cy * step < rt->h; ++cy)
        for (cx = 0; cx * step < rt->w; ++cx) {
            int px0 = cx * step, py0 = cy * step;
            int px1 = px0 + step < rt->w ? px0 + step : rt->w - 1;
            int py1 = py0 + step < rt->h ? py0 + step : rt->h - 1;
            fly_v3 c00, c10, c01, c11, mid, want;
            float scale;
            int flat;
            if (!sky_block_open(rt, px0, py0, px1, py1)) continue;
            c00 = FLY_SKY_GRID(cx, cy);
            c10 = FLY_SKY_GRID(cx + 1, cy);
            c01 = FLY_SKY_GRID(cx, cy + 1);
            c11 = FLY_SKY_GRID(cx + 1, cy + 1);
            mid = sky_at(e, v, rt->w, rt->h, 0.5f * (float)(px0 + px1),
                         0.5f * (float)(py0 + py1));
            want = fly_v3scale(fly_v3add(fly_v3add(c00, c10), fly_v3add(c01, c11)), 0.25f);
            scale = 0.3333f * (want.x + want.y + want.z) + 1e-5f;
            /* half a per cent of the cell's own brightness: a quarter of a code
             * value after the tonemap, and the resolve's dither covers that */
            flat = fabsf(mid.x - want.x) + fabsf(mid.y - want.y) +
                   fabsf(mid.z - want.z) <= 0.005f * scale;
            for (y = py0; y <= py1 && y < rt->h; ++y) {
                float ty = (float)(y - py0) / (float)(py1 > py0 ? py1 - py0 : 1);
                for (x = px0; x <= px1 && x < rt->w; ++x) {
                    fly_v3 c;
                    if (rt->depth[y * rt->w + x] < 1e29f) continue;
                    if (flat) {
                        float tx = (float)(x - px0) / (float)(px1 > px0 ? px1 - px0 : 1);
                        c = fly_v3lerp(fly_v3lerp(c00, c10, tx),
                                       fly_v3lerp(c01, c11, tx), ty);
                    } else {
                        c = sky_at(e, v, rt->w, rt->h, (float)x, (float)y);
                    }
                    rt->hdr[y * rt->w + x] = c;
                }
            }
        }
#undef FLY_SKY_GRID
}

/* ---------------- terrain: LOD rings with per-vertex light ---------------- */

#define RING_MAX 41 /* cells per side */

typedef struct {
    fly_v3 pos;
    /* The heightfield's own normal, which is what the slope, the shadow map's
       offset and the cosine are all defined against — never the relief-tilted
       one, which is resolved per pixel and would turn the far side of every
       clump into rock. */
    fly_v3 nrm;
    fly_v3 fogT; /* aerial perspective over the segment back to the camera */
    fly_v3 fogS;
    float cloud; /* what the two cloud layers leave of the beam */
    float ao;    /* crevice occlusion, and the canopy where the traced term is off */
    float shad;  /* the shadow map, where the ring resolves it per vertex */
    fly_v3 lit;  /* fully sunlit radiance (cloud deck folded in) — budget rung */
    fly_v3 dark; /* sun-occluded radiance (ambient/bounce only) — budget rung */
    fly_v3 col;  /* per-vertex resolved color, for the far (non-per-pixel) path */
    int wet;     /* below water level */
} fly__tv;

/* --- the ground, shaded in a fragment stage -------------------------------
 *
 * The CPU rasterizer resolved one colour per vertex of the terrain mesh, and
 * that is the whole of what was left of the terrain quilt. A vertex stands for
 * a 950 m cell on the outermost ring, and the fields the albedo is made of do
 * not all fade: the 380 m mottle and the 290 m woodland field are read through
 * nonlinear mixes and the woodland field is bimodal, so fading them toward
 * their own global means lands *further* from the cell average than point
 * sampling does — 0.0851 against 0.0045, measured, and in the changelog so it
 * is not tried again. Taking the mesh's own sampling rate rather than the
 * pixel's got the change from one cell to the next from 0.0862 to 0.0815
 * against 0.0614 for the true cell averages, and that was as far as a rate
 * could take it. What carries those fields honestly is per-pixel shading,
 * which is what the GPU rasterizer and the path tracer already do.
 *
 * So this is the CPU's fragment stage for terrain, and it is the GPU one
 * function for function: `terrain_surface` for the albedo and the material at
 * the pixel's own rate, `terrain_detail` for the near-field relief and the
 * blades — which the software path simply did not have — then `terrain_light`
 * with the shadow on the direct half and the occlusion on the ambient one,
 * exactly the split `FLY_ENV_TERRAIN_FS` makes.
 *
 * Three things stay per vertex, and the same three do on the GPU: the cloud
 * shadow, the crevice occlusion, and the aerial perspective. The first two are
 * lighting terms that turn slowly across a cell. The third is the expensive
 * one — a Chapman integral and a cloud march — and it is the reason the ground
 * is not simply handed `apply_fog`: it changes slowly across the frame, which
 * is what the GPU vertex stage says about it too, and re-marching it per pixel
 * would cost more than everything above it put together.
 *
 * `per_pixel` is the shadow map's, not the shading's, and mirrors the GPU's
 * `uPerPixel`: the near rings sample the map per pixel for crisp cast edges,
 * the far rings interpolate one lookup per vertex because a mountain's shadow
 * edge is soft at that range anyway. */
static void raster_tri_ground_raw(fly_render_target *rt, const fly__view *v,
                                  const fly__env *e, const fly__cv *t, int per_pixel) {
    const fly_world *w = e->world;
    fly_v3 a = t[0].p, b = t[1].p, c = t[2].p;
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    if (!w) return;
    if (!project(v, rt->w, rt->h, a, &ax, &ay, &az)) return;
    if (!project(v, rt->w, rt->h, b, &bx, &by, &bz)) return;
    if (!project(v, rt->w, rt->h, c, &cx, &cy, &cz)) return;
    int minx = (int)fminf(fminf(ax, bx), cx), maxx = (int)fmaxf(fmaxf(ax, bx), cx);
    int miny = (int)fminf(fminf(ay, by), cy), maxy = (int)fmaxf(fmaxf(ay, by), cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= rt->w) maxx = rt->w - 1;
    if (maxy >= rt->h) maxy = rt->h - 1;
    if (minx > maxx || miny > maxy) return;
    {
        float d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        float inv;
        int x, y;
        if (fabsf(d) < 1e-6f) return;
        inv = 1.0f / d;
        FLY_PERSP_SETUP(v, az, bz, cz, ia, ib, ic, persp);
        for (y = miny; y <= maxy; ++y)
            for (x = minx; x <= maxx; ++x) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                float u = ((cy - ay) * (px - ax) - (cx - ax) * (py - ay)) * inv;
                float vv = -((by - ay) * (px - ax) - (bx - ax) * (py - ay)) * inv;
                float uu = 1.0f - u - vv;
                float z;
                int idx;
                if (u < 0 || vv < 0 || uu < 0) continue;
                FLY_PERSPECTIVE(persp, uu, u, vv, az, bz, cz, ia, ib, ic, z);
                idx = y * rt->w + x;
                if (z >= rt->depth[idx]) continue;
                rt->depth[idx] = z;
                {
                    fly_v3 wp = fly_v3mk(uu * a.x + u * b.x + vv * c.x,
                                         uu * a.y + u * b.y + vv * c.y,
                                         uu * a.z + u * b.z + vv * c.z);
                    fly_v3 gn = fly_v3norm(fly_v3mk(
                        uu * t[0].a[0] + u * t[1].a[0] + vv * t[2].a[0],
                        uu * t[0].a[1] + u * t[1].a[1] + vv * t[2].a[1],
                        uu * t[0].a[2] + u * t[1].a[2] + vv * t[2].a[2]));
                    fly_v3 fT = fly_v3mk(
                        uu * t[0].a[3] + u * t[1].a[3] + vv * t[2].a[3],
                        uu * t[0].a[4] + u * t[1].a[4] + vv * t[2].a[4],
                        uu * t[0].a[5] + u * t[1].a[5] + vv * t[2].a[5]);
                    fly_v3 fS = fly_v3mk(
                        uu * t[0].a[6] + u * t[1].a[6] + vv * t[2].a[6],
                        uu * t[0].a[7] + u * t[1].a[7] + vv * t[2].a[7],
                        uu * t[0].a[8] + u * t[1].a[8] + vv * t[2].a[8]);
                    float cloud = uu * t[0].a[9] + u * t[1].a[9] + vv * t[2].a[9];
                    float ao = uu * t[0].a[10] + u * t[1].a[10] + vv * t[2].a[10];
                    float shad = uu * t[0].a[11] + u * t[1].a[11] + vv * t[2].a[11];
                    fly_v3 dir = fly_v3sub(wp, v->pos);
                    float dist = fly_v3len(dir);
                    float rate = e->pxscale / (dist + 1e-3f);
                    fly_v3 mat, sn = gn, alb, col;
                    float nd, s;
                    alb = terrain_surface(w, wp.x, wp.y, wp.z, 1.0f - gn.z, rate,
                                          e->ball, e->wet, &mat);
                    alb = fly_v3mul(alb, terrain_detail(w, wp.x, wp.y, mat, rate, &sn));
                    if (per_pixel) {
                        nd = fly_v3dot(gn, e->sun);
                        s = terrain_sun(e, wp, nd < 0.0f ? fly_v3scale(gn, -1.0f) : gn,
                                        nd < 0.0f ? -nd : nd, 0.0f);
                    } else {
                        s = shad;
                    }
                    col = terrain_light(e, alb, mat, sn,
                                        fly_v3scale(dir, -1.0f / (dist + 1e-5f)),
                                        cloud * s, ao * ao_at(wp.x, wp.y, 0.0f, dist));
                    rt->hdr[idx] = fly_v3add(fly_v3mul(col, fT), fS);
                }
            }
    }
}

static void raster_tri_ground(fly_render_target *rt, const fly__view *v,
                              const fly__env *e, const fly__tv *ta, const fly__tv *tb,
                              const fly__tv *tc, int per_pixel) {
    const fly__tv *g3[3];
    fly__cv in[3], out[6];
    int nt, i, k;
    g3[0] = ta; g3[1] = tb; g3[2] = tc;
    for (i = 0; i < 3; ++i) {
        in[i].p = g3[i]->pos;
        in[i].a[0] = g3[i]->nrm.x;  in[i].a[1] = g3[i]->nrm.y;  in[i].a[2] = g3[i]->nrm.z;
        in[i].a[3] = g3[i]->fogT.x; in[i].a[4] = g3[i]->fogT.y; in[i].a[5] = g3[i]->fogT.z;
        in[i].a[6] = g3[i]->fogS.x; in[i].a[7] = g3[i]->fogS.y; in[i].a[8] = g3[i]->fogS.z;
        in[i].a[9] = g3[i]->cloud;
        in[i].a[10] = g3[i]->ao;
        in[i].a[11] = g3[i]->shad;
    }
    nt = near_clip_tri(v, in, out, 12);
    for (k = 0; k < nt; ++k) raster_tri_ground_raw(rt, v, e, &out[k * 3], per_pixel);
}

/* ---------------- world-space ambient occlusion ----------------
 *
 * What a patch of ground can see of the sky, traced, cached where the world
 * is rather than where the screen is, and refined a few rays at a time.
 *
 * It replaces `ao *= 1 - 0.42 * forest_mask(x, y)`. That was a scalar off a
 * density field evaluated per terrain vertex: it darkened woodland on average
 * and knew nothing about any particular tree, so a clearing was as dark as the
 * thicket beside it and nothing had a shadow gathering under it. It is the one
 * term in the frame that is still a table rather than an integral.
 *
 * The cache is in world space, and that is the whole design rather than an
 * optimisation. The usual way to amortise a traced term over frames is to
 * reproject last frame's pixels into this frame's camera, which throws away
 * every sample at a disocclusion — precisely the silhouettes of a tree line,
 * and precisely what an aircraft generates constantly. But occlusion of the
 * ambient hemisphere is a property of the world and not of the view, and this
 * world is a pure function of position and seed, so the sample belongs to the
 * ground and not to the pixel. Fly away and back and it is still there.
 *
 * The key is exact rather than quantised. `terrain_ring` looks camera-anchored
 * — its origin is `floorf(pos/step)*step` — but that means every vertex it ever
 * emits lands on the global lattice `{k * step}`. The mesh slides by whole
 * cells as the camera moves and the vertices underneath it do not move at all.
 *
 * Sample n of a cell always comes from the same place in the same sequence,
 * seeded by the cell rather than by a running generator, so a cell with K
 * samples holds the same number however and whenever those K were taken. That
 * is what lets a cache live under a test suite that renders single frames and
 * asserts on pixels: with `ao_budget` at zero every visible cell is filled to
 * `ao_target` before the frame is drawn, and the frame is exactly reproducible.
 * With a budget it converges over successive frames instead, which is the
 * interactive path. */

/* The store is a toroidal window on a fixed world lattice rather than a hash.
 * Same idea — the sample belongs to the ground — but a lattice can be walked in
 * order, filled under a budget before anything is drawn, read per pixel with a
 * bilinear tap, and handed to the GPU as a texture, none of which a hash can
 * do. Indices wrap, and a tag on each cell says which patch of the world is
 * currently living in that slot: flying scrolls the window and only the cells
 * that just came over the horizon are cold. */

#define FLY_AO_LAT 4.0f       /* metres per lattice step */
#define FLY_AO_N 256          /* window side, power of two: 1.02 km */
#define FLY_AO_R 46.0f        /* how far a ray looks for an occluder */
#define FLY_AO_GROUND 26.0f   /* and how far it bothers marching terrain */
#define FLY_AO_CELL 44.0f     /* the foliage scatter's own cell */
#define FLY_AO_TREES 96       /* occluders gathered around one lattice point */
#define FLY_AO_FILL 160.0f    /* radius the pre-pass keeps warm */

/* --- and the same question a canopy's height up ---------------------------
 *
 * One channel is a field over the *ground*, and only the ground is on it.
 * Everything else in the world stands up out of it: a crown at canopy height
 * sees most of the sky the floor beneath it cannot, a tower's parapet sees all
 * of it, and reading one number at the ground position gives a spruce the
 * shade of the wood floor it is growing out of. Which is the same mistake as
 * giving the floor open sky, in the other direction.
 *
 * So the window holds two numbers per lattice point: what the ground sees, and
 * what a point FLY_AO_HI above it sees. Sixteen metres, because that clears
 * the tallest crown the scatter builds (a 15.5 m spruce tops out at 16.3), so
 * the upper channel is terrain occlusion and nothing else — a valley wall
 * still shades a crown, and the wood no longer does.
 *
 * Between the two it is linear in height and clamped above, and that is a
 * decision rather than a model: visibility through a canopy really falls off
 * something more like exponentially, and two traced samples cannot tell which.
 * Two honest ends and a straight line between them beats a curve fitted to
 * nothing, and both ends are measurements.
 *
 * The upper channel is traced at a quarter of the lower one's rays. It is not
 * a saving taken carelessly: from above the canopy the integrand is open sky
 * over most of the hemisphere with a terrain horizon at the bottom, so its
 * variance is a fraction of the floor's, where the answer is decided by which
 * gaps between which crowns a ray happens to find. */
#define FLY_AO_HI 16.0f
#define FLY_AO_HI_RAYS(target) ((target) >= 4 ? (target) / 4 : 1)

typedef struct {
    int32_t tx, ty;    /* which lattice point is in this slot */
    uint16_t samples, hi_samples;
    uint8_t live;      /* 0 until the slot has ever been claimed */
    float sum, hi_sum;
} fly__aocell;

static fly__aocell *g_ao;
static int g_ao_target;
static long g_ao_left;      /* rays remaining in this frame's budget */
static long g_ao_spent;     /* what the frame actually used, for the gate */
static unsigned g_ao_rev;   /* bumped whenever a texel's value changes */
static const fly_world *g_ao_world; /* for the scalar the fade falls back to */

typedef struct { float x, y, z, r; } fly__occ;

/* The stand around a point, at the density the world has rather than the
 * density the camera is drawing. `draw_foliage` thins its scatter with
 * distance, which is right for triangles and would make the ground darken as
 * you flew toward it; this reads the closed-canopy count every time. */
static int ao_gather(const fly_world *w, float x, float y, fly__occ *out, int max) {
    int ix0 = (int)floorf((x - FLY_AO_R) / FLY_AO_CELL);
    int ix1 = (int)floorf((x + FLY_AO_R) / FLY_AO_CELL);
    int iy0 = (int)floorf((y - FLY_AO_R) / FLY_AO_CELL);
    int iy1 = (int)floorf((y + FLY_AO_R) / FLY_AO_CELL);
    int ix, iy, n = 0;
    for (iy = iy0; iy <= iy1 && n < max; ++iy)
        for (ix = ix0; ix <= ix1 && n < max; ++ix) {
            float cx = ((float)ix + 0.5f) * FLY_AO_CELL;
            float cy = ((float)iy + 0.5f) * FLY_AO_CELL;
            float dens, gz0, conif, region;
            uint32_t h1;
            int cnt, j;
            dens = fly_world_forest(w, cx, cy, &gz0);
            if (dens <= 0.0f) continue;
            h1 = fly_hash2(w->seed + 71, ix, iy);
            cnt = (int)(dens * 16.0f + (float)(h1 & 255u) / 255.0f);
            region = grass_region(w->seed, cx, cy);
            for (j = 0; j < cnt && n < max; ++j) {
                uint32_t h = fly_hash2(h1 ^ 0x9E3779B9u, ix * 7 + j, iy * 13 - j);
                float px = cx + ((float)((h >> 3) & 63) / 63.0f - 0.5f) * FLY_AO_CELL;
                float py = cy + ((float)((h >> 9) & 63) / 63.0f - 0.5f) * FLY_AO_CELL;
                float gz, s, tintr;
                int species;
                if (fabsf(px - x) > FLY_AO_R || fabsf(py - y) > FLY_AO_R) continue;
                gz = fly_world_ground(w, px, py);
                if (gz < FLY_WATER_Z + 0.5f) continue;
                conif = fly_clampf((gz - 120.0f) / 420.0f, 0.05f, 0.95f);
                species = (h & 31u) == 5u ? 2
                          : ((float)((h >> 5) & 255) / 255.0f < conif ? 0 : 1);
                if (region > 0.30f && (h & 7u) == 3u) species = 2;
                if (species == 2) continue;          /* deadwood casts nothing */
                tintr = (float)((h >> 16) & 7) / 7.0f;
                s = species == 0 ? 6.5f + tintr * 9.0f : 5.5f + tintr * 6.5f;
                out[n].x = px;
                out[n].y = py;
                out[n].z = gz + s * 0.60f;
                out[n].r = s * 0.45f;
                ++n;
            }
        }
    return n;
}

/* ---------------- what a settlement occludes ----------------
 *
 * The traced term gathers trees and marches the heightfield, and until now
 * that was all of it: a hangar wall, a raised pad deck, a tank farm and a
 * sixty-metre tower occluded nothing at all, so the ground at the foot of a
 * building took the same open sky as the meadow outside the perimeter and a
 * street between two towers took the same as the apron. That is the whole
 * difference between structures standing on the ground and structures placed
 * on it, and on the overcast afternoon most of the gallery is shot on — where
 * there is barely a sun to cast and the ambient half *is* the picture — it is
 * the only difference there is.
 *
 * What a settlement occludes is a column grid rather than a list of shapes.
 * The question the ambient integral asks is "how high does something stand
 * over there", the same question it already asks of the terrain, so the answer
 * goes in the same form and rides the same march. A tower is a tall column and
 * a fence is a short one; neither needs a bounding volume of its own.
 *
 * The grid is filled by *drawing the site*, not by walking a layout the way
 * the layout code would. `draw_location` is the only thing that knows where a
 * settlement puts its buildings, and a second description of that in here
 * would be wrong within a release. The capture rides the shadow caster's own
 * seam — the primitives already hand their triangles over when a cast is in
 * force, and every rule about what may cast comes with it for free: decals and
 * markings occlude nothing, glass occludes nothing, a prop disc occludes
 * nothing, and the near-field detail gates are already bypassed for a caster,
 * so what is captured does not depend on where the camera was.
 *
 * Per site and cached, because a settlement is a pure function of the world
 * and the AO cache it feeds is world-space and persistent. Building it from a
 * per-frame replay instead would put the camera into a cache that exists
 * precisely to be independent of it. */
#define FLY_OCC_LAT 8.0f   /* metres per column */
#define FLY_OCC_N 64       /* columns a side: 512 m, wider than any clearing */

typedef struct fly__siteocc {
    float *top;    /* FLY_OCC_N^2 absolute heights, -1e30 where nothing stands */
    float ox, oy;  /* world position of column (0,0) */
    int built;
} fly__siteocc;

static fly__siteocc g_site_occ[FLY_MAX_LOC];
static const fly_game *g_occ_game; /* whose sites g_site_occ currently holds */
static uint32_t g_occ_seed;
static int g_occ_any;              /* any site captured at all? the common
                                    * lattice point is nowhere near one */

/* Bin one captured triangle. The whole 2D footprint and not the vertices: a
 * roof is four vertices over thirty metres and a column grid that took only
 * those would have a hole in the middle of every building. Conservative in
 * height — the triangle's own top — because an occluder that is too short
 * lets light through a wall, which is the failure that shows. */
static void occ_cast_tri(fly_v3 a, fly_v3 b, fly_v3 c) {
    fly__siteocc *o = g_occ_build;
    float z = fmaxf(a.z, fmaxf(b.z, c.z));
    float x0 = fminf(a.x, fminf(b.x, c.x)), x1 = fmaxf(a.x, fmaxf(b.x, c.x));
    float y0 = fminf(a.y, fminf(b.y, c.y)), y1 = fmaxf(a.y, fmaxf(b.y, c.y));
    int i0 = (int)floorf((x0 - o->ox) / FLY_OCC_LAT), i1 = (int)floorf((x1 - o->ox) / FLY_OCC_LAT);
    int j0 = (int)floorf((y0 - o->oy) / FLY_OCC_LAT), j1 = (int)floorf((y1 - o->oy) / FLY_OCC_LAT);
    int i, j;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > FLY_OCC_N - 1) i1 = FLY_OCC_N - 1;
    if (j1 > FLY_OCC_N - 1) j1 = FLY_OCC_N - 1;
    for (j = j0; j <= j1; ++j)
        for (i = i0; i <= i1; ++i) {
            float *t = &o->top[j * FLY_OCC_N + i];
            if (z > *t) *t = z;
        }
}

/* How high built geometry stands at (x, y), or -1e30 where nothing does. */
static float occ_top(const fly_game *g, float x, float y) {
    float best = -1e30f;
    int li;
    for (li = 0; li < g->world.nloc; ++li) {
        const fly__siteocc *o = &g_site_occ[li];
        int i, j;
        if (!o->built) continue;
        i = (int)floorf((x - o->ox) / FLY_OCC_LAT);
        j = (int)floorf((y - o->oy) / FLY_OCC_LAT);
        if (i < 0 || j < 0 || i >= FLY_OCC_N || j >= FLY_OCC_N) continue;
        if (o->top[j * FLY_OCC_N + i] > best) best = o->top[j * FLY_OCC_N + i];
    }
    return best;
}

/* one ray: the canopy first, because it is what the term is for, then the
   ground, marched coarsely — this is a visibility question, not a silhouette */
static int ao_blocked(const fly_world *w, fly_v3 p, fly_v3 d,
                      const fly__occ *occ, int nocc, int built) {
    int i;
    for (i = 0; i < nocc; ++i) {
        float ox = occ[i].x - p.x, oy = occ[i].y - p.y, oz = occ[i].z - p.z;
        float tc = ox * d.x + oy * d.y + oz * d.z;
        float d2;
        if (tc <= 0.0f || tc > FLY_AO_R) continue;
        d2 = ox * ox + oy * oy + oz * oz - tc * tc;
        if (d2 < occ[i].r * occ[i].r) return 1;
    }
    for (i = 1; i <= 5; ++i) {
        float t = FLY_AO_R * (float)i / 5.0f;
        float gx = p.x + d.x * t, gy = p.y + d.y * t;
        if (fly_world_ground(w, gx, gy) > p.z + d.z * t + 0.5f) return 1;
    }
    /* Built geometry, on its own march and twice as fine. The terrain's five
     * steps are nine metres apart, which is the resolution relief that big
     * needs; a wall is two metres thick and a lamp post is not thick at all,
     * so the same five steps walk straight through most of a settlement.
     * Eleven steps is an array index apiece against the heightfield's five
     * five-octave samples, so the finer march is the cheaper half. */
    if (built && g_occ_any)
        for (i = 1; i <= 11; ++i) {
            float t = FLY_AO_R * (float)i / 11.0f;
            if (occ_top(g_occ_game, p.x + d.x * t, p.y + d.y * t) >
                p.z + d.z * t + 0.5f) return 1;
        }
    return 0;
}

/* the cell's own sample sequence: a radical inverse for one axis and the
   golden ratio for the other, offset by the cell, so sample n is the same
   direction whoever asks for it and whenever */
static fly_v3 ao_dir(fly_v3 n, uint32_t key, int i) {
    uint32_t b = (uint32_t)i + 1u;
    float u1, u2, r, ph, sx, sy, sz;
    fly_v3 t, bt;
    b = ((b & 0x55555555u) << 1) | ((b >> 1) & 0x55555555u);
    b = ((b & 0x33333333u) << 2) | ((b >> 2) & 0x33333333u);
    b = ((b & 0x0F0F0F0Fu) << 4) | ((b >> 4) & 0x0F0F0F0Fu);
    b = (b << 24) | ((b & 0xFF00u) << 8) | ((b >> 8) & 0xFF00u) | (b >> 24);
    u1 = (float)b * 2.3283064e-10f;
    u2 = fmodf((float)i * 0.6180339887f + (float)(key & 1023u) * 0.0009765625f, 1.0f);
    r = sqrtf(u1);
    ph = u2 * 2.0f * FLY_PI;
    sx = r * cosf(ph);
    sy = r * sinf(ph);
    sz = sqrtf(1.0f - u1 > 0.0f ? 1.0f - u1 : 0.0f);
    t = fabsf(n.z) < 0.9f ? fly_v3mk(0, 0, 1) : fly_v3mk(1, 0, 0);
    bt = fly_v3norm(fly_v3cross(t, n));
    t = fly_v3cross(n, bt);
    return fly_v3norm(fly_v3add(fly_v3add(fly_v3scale(bt, sx), fly_v3scale(t, sy)),
                                fly_v3scale(n, sz)));
}

void fly_render_ao_reset(void) {
    int i;
    if (g_ao) memset(g_ao, 0, (size_t)FLY_AO_N * FLY_AO_N * sizeof *g_ao);
    /* The settlement grids go with it. They are half of what the window
     * records, so a reset that kept them would leave the cache holding one
     * world's buildings and another world's rays. */
    for (i = 0; i < FLY_MAX_LOC; ++i) g_site_occ[i].built = 0;
    g_occ_any = 0;
    g_occ_game = NULL;
    ++g_ao_rev;
}

long fly_render_ao_rays(void) { return g_ao_spent; }

static fly__aocell *ao_slot(int tx, int ty) {
    fly__aocell *c = &g_ao[((ty & (FLY_AO_N - 1)) * FLY_AO_N) + (tx & (FLY_AO_N - 1))];
    if (!c->live || c->tx != tx || c->ty != ty) {
        c->tx = tx;
        c->ty = ty;
        c->samples = 0;
        c->hi_samples = 0;
        c->sum = 0.0f;
        c->hi_sum = 0.0f;
        c->live = 1;
        ++g_ao_rev;
    }
    return c;
}

/* Fill one lattice point to `target`, budget permitting. The occluders are
 * gathered once for the point and shared by all of its rays, which is why the
 * budget counts rays and not points. */
static void ao_fill(const fly_world *w, fly__aocell *c, int tx, int ty, int target) {
    fly__occ occ[FLY_AO_TREES];
    fly_v3 p, n;
    float x, y, e4 = 3.0f, hx0, hx1, hy0, hy1;
    int nocc, s, hi_target = FLY_AO_HI_RAYS(target);
    if ((c->samples >= target && c->hi_samples >= hi_target) || g_ao_left <= 0) return;
    x = (float)tx * FLY_AO_LAT;
    y = (float)ty * FLY_AO_LAT;
    p = fly_v3mk(x, y, fly_world_ground(w, x, y));
    hx0 = fly_world_ground(w, x - e4, y);
    hx1 = fly_world_ground(w, x + e4, y);
    hy0 = fly_world_ground(w, x, y - e4);
    hy1 = fly_world_ground(w, x, y + e4);
    n = fly_v3norm(fly_v3mk(-(hx1 - hx0) / (2 * e4), -(hy1 - hy0) / (2 * e4), 1.0f));
    nocc = ao_gather(w, x, y, occ, FLY_AO_TREES);
    for (s = c->samples; s < target && g_ao_left > 0; ++s) {
        uint32_t key = fly_hash2(0x9E3779B9u, tx, ty);
        c->sum += ao_blocked(w, p, ao_dir(n, key, s), occ, nocc, 1) ? 0.0f : 1.0f;
        ++c->samples;
        --g_ao_left;
        ++g_ao_spent;
        ++g_ao_rev;
    }
    /* Same point, same lobe, sixteen metres up. The *terrain* normal and not
       the vertical, so the two channels are the same question asked twice at
       two heights and the difference between them is exactly what stands in
       between — which is what the blend by height is interpolating. */
    {
        fly_v3 ph = fly_v3add(p, fly_v3mk(0, 0, FLY_AO_HI));
        for (s = c->hi_samples; s < hi_target && g_ao_left > 0; ++s) {
            uint32_t key = fly_hash2(0x85EBCA6Bu, tx, ty);
            /* Terrain and canopy only — no settlement.
             *
             * The upper channel's contract is that it is terrain occlusion and
             * nothing else, which is what makes it the right answer for a
             * surface standing above the ground rather than lying on it, and
             * a facade is exactly that surface. Sixteen metres above a tower's
             * footprint is *inside the tower*, so a column grid on this channel
             * puts every wall of every building in its own shadow: measured on
             * `render.facade`, one tone went from owning a fifth of the city's
             * glazing and cladding to owning 56.5% of it, which is a black
             * building. What a wall is occluded by is the *next* building
             * along, and that is a question this lattice — a field over the
             * ground, read at one point — cannot be asked. */
            c->hi_sum += ao_blocked(w, ph, ao_dir(n, key, s), occ, nocc, 0) ? 0.0f : 1.0f;
            ++c->hi_samples;
            --g_ao_left;
            ++g_ao_spent;
            ++g_ao_rev;
        }
    }
}

/* Open the frame's occlusion budget and warm the window around the camera.
 *
 * Nearest first, so a budget that runs out spends itself on the ground the eye
 * is closest to. A budget of zero is unlimited, which is what every single
 * frame render wants: the cache is then purely an optimisation and the frame is
 * the same whether it was warm or cold. */
static void site_occ_sync(const fly_game *g, float x, float y);

static void ao_begin(const fly_game *g, int target, long budget) {
    g_ao_target = target;
    g_ao_spent = 0;
    g_ao_left = budget > 0 ? budget : 0x7FFFFFFFL;
    g_ao_world = &g->world;
}

/* Warm the window, once the world it is a window on is known.
 *
 * Split from `ao_begin` because a settlement only becomes an occluder after it
 * has been captured, and a cell traced before that caches a sky it does not
 * have — the cache is persistent, so the wrong answer would outlive the frame
 * that made it. Sites first, then rays. */
static void ao_warm(const fly_game *g, const fly__view *v) {
    int r, tx0, ty0, ring, half, target = g_ao_target;
    if (target <= 0) return;
    site_occ_sync(g, v->pos.x, v->pos.y);
    if (!g_ao) {
        g_ao = (fly__aocell *)calloc((size_t)FLY_AO_N * FLY_AO_N, sizeof *g_ao);
        if (!g_ao) { g_ao_target = 0; return; }
    }
    tx0 = (int)floorf(v->pos.x / FLY_AO_LAT + 0.5f);
    ty0 = (int)floorf(v->pos.y / FLY_AO_LAT + 0.5f);
    half = (int)(FLY_AO_FILL / FLY_AO_LAT);
    if (half > FLY_AO_N / 2 - 1) half = FLY_AO_N / 2 - 1;
    /* square rings outward from the camera: the order is what makes a partial
       budget spend itself where it shows */
    for (ring = 0; ring <= half && g_ao_left > 0; ++ring) {
        int i;
        if (ring == 0) { ao_fill(&g->world, ao_slot(tx0, ty0), tx0, ty0, target); continue; }
        for (i = -ring; i <= ring && g_ao_left > 0; ++i) {
            int tx = tx0 + i;
            ao_fill(&g->world, ao_slot(tx, ty0 - ring), tx, ty0 - ring, target);
            ao_fill(&g->world, ao_slot(tx, ty0 + ring), tx, ty0 + ring, target);
        }
        for (i = -ring + 1; i <= ring - 1 && g_ao_left > 0; ++i) {
            int ty = ty0 + i;
            ao_fill(&g->world, ao_slot(tx0 - ring, ty), tx0 - ring, ty, target);
            ao_fill(&g->world, ao_slot(tx0 + ring, ty), tx0 + ring, ty, target);
        }
    }
    r = 0;
    (void)r;
}

/* Read it, bilinearly, at whatever rate the caller likes — which is the point
 * of a lattice: the mesh samples every 26 m and the trees vary every 15.5, so
 * anything read at vertices can only broaden a wood. This is read per pixel.
 *
 * `dist` fades the term out as the window's edge approaches, and that is not
 * cosmetic. The window is toroidal, so a slot holds whichever patch of world
 * last claimed it; beyond the filled radius the wrap serves up ground from a
 * kilometre away. The CPU could check the tag and refuse, but the GPU samples a
 * texture and cannot, so both fade instead and neither can read a stale wrap.
 *
 * What it fades *to* is the scalar, not to open sky, and getting that wrong was
 * a measured regression rather than a theoretical one. The window is 160 m
 * around the camera; an aircraft at cruise sees almost none of the ground it is
 * looking at from inside it. Handing those pixels 1.0 deleted woodland
 * occlusion from every aerial view — on the coverage gallery's aerial wood,
 * 5796 pixels changed and **every one of them got lighter**, by 4.7 of 255.
 * The old `1 - 0.42 * mask` is a poor description of a wood floor up close,
 * which is why the tracer replaces it, but at half a kilometre it is a
 * perfectly good one, and the two now agree closely enough for the crossover to
 * be invisible: closed canopy traces to 0.534 against the scalar's 0.58. */
static float ao_at(float x, float y, float agl, float dist) {
    float fx, fy, u, vv, k, far, lo, hi, t;
    float s00, s10, s01, s11, h00, h10, h01, h11;
    int tx, ty;
    if (g_ao_target <= 0) return 1.0f; /* the caller applied the scalar itself */
    t = fly_clampf(agl / FLY_AO_HI, 0.0f, 1.0f);
    /* the scalar lifts with height too, or a crown beyond the window would go
       back to reading the wood floor's darkening the moment the trace faded */
    far = g_ao_world ? 1.0f - 0.42f * fly_world_forest_mask(g_ao_world, x, y) * (1.0f - t) : 1.0f;
    if (!g_ao) return far;
    k = 1.0f - fly_smoothstepf(FLY_AO_FILL * 0.70f, FLY_AO_FILL, dist);
    if (k <= 0.0f) return far;
    fx = x / FLY_AO_LAT;
    fy = y / FLY_AO_LAT;
    tx = (int)floorf(fx);
    ty = (int)floorf(fy);
    u = fx - (float)tx;
    vv = fy - (float)ty;
    {
        fly__aocell *a = &g_ao[((ty) & (FLY_AO_N - 1)) * FLY_AO_N + ((tx) & (FLY_AO_N - 1))];
        fly__aocell *b = &g_ao[((ty) & (FLY_AO_N - 1)) * FLY_AO_N + ((tx + 1) & (FLY_AO_N - 1))];
        fly__aocell *c = &g_ao[((ty + 1) & (FLY_AO_N - 1)) * FLY_AO_N + ((tx) & (FLY_AO_N - 1))];
        fly__aocell *d = &g_ao[((ty + 1) & (FLY_AO_N - 1)) * FLY_AO_N + ((tx + 1) & (FLY_AO_N - 1))];
        s00 = (a->live && a->samples) ? a->sum / a->samples : 1.0f;
        s10 = (b->live && b->samples) ? b->sum / b->samples : 1.0f;
        s01 = (c->live && c->samples) ? c->sum / c->samples : 1.0f;
        s11 = (d->live && d->samples) ? d->sum / d->samples : 1.0f;
        h00 = (a->live && a->hi_samples) ? a->hi_sum / a->hi_samples : 1.0f;
        h10 = (b->live && b->hi_samples) ? b->hi_sum / b->hi_samples : 1.0f;
        h01 = (c->live && c->hi_samples) ? c->hi_sum / c->hi_samples : 1.0f;
        h11 = (d->live && d->hi_samples) ? d->hi_sum / d->hi_samples : 1.0f;
    }
    lo = fly_lerpf(fly_lerpf(s00, s10, u), fly_lerpf(s01, s11, u), vv);
    hi = fly_lerpf(fly_lerpf(h00, h10, u), fly_lerpf(h01, h11, u), vv);
    return fly_lerpf(far, fly_lerpf(lo, hi, t), k);
}

/* How far a primitive's centre stands above the ground, never negative: the
 * argument `ao_at` blends its two channels with.
 *
 * Read off the occlusion window's own lattice and interpolated, rather than
 * evaluated under the primitive. Three reasons, and the third is the one that
 * decided it. The window's cells *are* four metres, so a height read finer
 * than that is precision the answer does not have. The window's own reference
 * height is `fly_world_ground` at the lattice point, so taking the same
 * function at the same points is the only way the two agree about where zero
 * is. And the heightfield memo is keyed on exact coordinates, deliberately —
 * so a lookup under each triangle's own centroid misses every single time, and
 * a wood is ninety thousand triangles: measured at 124 -> 484 ms on the
 * closed-canopy object build, against 129 for the four lattice taps, which
 * land on four keys a whole tree shares. */
static float tri_agl(const fly__env *e, fly_v3 mid) {
    const fly_world *w = e->world;
    float fx, fy, u, vv, g, x0, y0;
    int ix, iy;
    if (!w) return 0.0f;
    fx = mid.x / FLY_AO_LAT;
    fy = mid.y / FLY_AO_LAT;
    ix = (int)floorf(fx);
    iy = (int)floorf(fy);
    u = fx - (float)ix;
    vv = fy - (float)iy;
    x0 = (float)ix * FLY_AO_LAT;
    y0 = (float)iy * FLY_AO_LAT;
    g = fly_lerpf(fly_lerpf(scatter_ground(w, x0, y0),
                            scatter_ground(w, x0 + FLY_AO_LAT, y0), u),
                  fly_lerpf(scatter_ground(w, x0, y0 + FLY_AO_LAT),
                            scatter_ground(w, x0 + FLY_AO_LAT, y0 + FLY_AO_LAT), u), vv);
    return mid.z - g > 0.0f ? mid.z - g : 0.0f;
}

/* The same window as a texture, so the GPU reads the numbers the CPU traced
 * rather than a second implementation of them. Unfilled slots go up as 1.0,
 * which is exactly what ao_at returns for them.
 *
 * Repacked only when a texel actually changed. A warm frame traces nothing, and
 * the window is a quarter of a megabyte: rebuilding and re-uploading it every
 * frame would have made the read cost something for no reason at all, on the
 * frames that are supposed to be free. */
static const float *ao_texels(unsigned *rev) {
    static float *buf;
    static unsigned built = 0u;
    static int have = 0;
    int i, n = FLY_AO_N * FLY_AO_N;
    if (!g_ao) return NULL;
    if (!buf) {
        buf = (float *)malloc((size_t)n * 2 * sizeof *buf);
        if (!buf) return NULL;
    }
    if (!have || built != g_ao_rev) {
        for (i = 0; i < n; ++i) {
            buf[i * 2] = (g_ao[i].live && g_ao[i].samples)
                             ? g_ao[i].sum / (float)g_ao[i].samples : 1.0f;
            buf[i * 2 + 1] = (g_ao[i].live && g_ao[i].hi_samples)
                                 ? g_ao[i].hi_sum / (float)g_ao[i].hi_samples : 1.0f;
        }
        built = g_ao_rev;
        have = 1;
    }
    if (rev) *rev = built;
    return buf;
}

float fly_render_built_top(const fly_game *g, float x, float y) {
    if (!g) return -1e30f;
    site_occ_sync(g, x, y);
    return occ_top(g, x, y);
}

int fly_render_ao_probe(const fly_game *g, fly_v3 p, float agl, int target, float *vis) {
    int tx, ty;
    fly__aocell *c;
    if (!g || !vis || target <= 0) return -1;
    g_ao_target = target;
    g_ao_left = 0x7FFFFFFFL;
    g_ao_spent = 0;
    g_ao_world = &g->world;
    if (!g_ao) {
        g_ao = (fly__aocell *)calloc((size_t)FLY_AO_N * FLY_AO_N, sizeof *g_ao);
        if (!g_ao) return -1;
    }
    /* the same settlements the frame would have captured: a probe that
       answered without them would be measuring a different renderer */
    site_occ_sync(g, p.x, p.y);
    tx = (int)floorf(p.x / FLY_AO_LAT + 0.5f);
    ty = (int)floorf(p.y / FLY_AO_LAT + 0.5f);
    c = ao_slot(tx, ty);
    ao_fill(&g->world, c, tx, ty, target);
    if (!c->samples || !c->hi_samples) return -1;
    /* the cell's own two numbers blended by height, without the bilinear tap
       or the window fade — this asks what the tracer found, not what a pixel
       a metre away would read */
    *vis = fly_lerpf(c->sum / (float)c->samples, c->hi_sum / (float)c->hi_samples,
                     fly_clampf(agl / FLY_AO_HI, 0.0f, 1.0f));
    return 0;
}

/* per_pixel: near rings sample the shadow map in the pixel loop for crisp cast
 * edges; far rings resolve one lookup per vertex (cheaper, and mountain-scale
 * shadows are soft anyway). It is the map's rate and not the shading's — where
 * the level asks for it, the albedo, the material and the light are resolved
 * per pixel on every ring; see raster_tri_ground. */
/* --- how coarse the drawn ground is out there ----------------------------
 *
 * The terrain is drawn through four rings that coarsen with distance, so the
 * *drawn* ground at a point can stand above what `fly_world_ground` reports by
 * about what the height varies across the covering ring's cell. Anything laid
 * on the ground rather than built out of it has to know that number or it is
 * swallowed by its own hillside a kilometre away: the road deck already does
 * (see road_shelf and ground_bias), and so does a river, whose surface is
 * below the ground on both sides of it by construction.
 *
 * One ladder, here, used by the pass that draws the rings and by everything
 * that has to ask how coarse they are. `half` is how many cells each ring
 * reaches from the camera, so a ring covers out to step*(half-1) and its
 * coarser sibling takes over there. */
#define FLY_LOD_RINGS 4
static const float fly__ring_step[FLY_LOD_RINGS] = { 26.0f, 60.0f, 240.0f, 950.0f };
static const int fly__ring_half[FLY_LOD_RINGS] = { 24, 22, 22, 20 };

/* The step of the ring covering a point this far from the camera. The rings
 * are squares centred on it, so the distance that decides is the Chebyshev
 * one, which is what the caller passes. */
static float ring_cell(float d) {
    int i;
    for (i = 0; i + 1 < FLY_LOD_RINGS; ++i)
        if (d < fly__ring_step[i] * (float)(fly__ring_half[i] - 1))
            return fly__ring_step[i];
    return fly__ring_step[FLY_LOD_RINGS - 1];
}

static void terrain_ring(fly_render_target *rt, const fly_game *g, const fly__env *e,
                         const fly__view *v, float step, int half, float inner,
                         int per_pixel) {
    const fly_world *w = &g->world;
    int n = half * 2; /* cells */
    if (n > RING_MAX * 2) n = RING_MAX * 2;
    static fly__tv verts[(RING_MAX * 2 + 1) * (RING_MAX * 2 + 1)];
    int vn = n + 1;
    float cx0 = floorf(v->pos.x / step) * step - half * step;
    float cy0 = floorf(v->pos.y / step) * step - half * step;
    int i, j;
    for (j = 0; j < vn; ++j)
        for (i = 0; i < vn; ++i) {
            float wx = cx0 + i * step, wy = cy0 + j * step;
            fly__tv *tv = &verts[j * vn + i];
            float h = fly_world_ground(w, wx, wy);
            tv->pos = fly_v3mk(wx, wy, h);
            tv->wet = h < FLY_WATER_Z;
            /* normal + concavity share the same four taps; concave spots get
             * sky-visibility ambient occlusion (crevice GI) */
            float ee = step * 0.4f;
            float hx0 = fly_world_ground(w, wx - ee, wy), hx1 = fly_world_ground(w, wx + ee, wy);
            float hy0 = fly_world_ground(w, wx, wy - ee), hy1 = fly_world_ground(w, wx, wy + ee);
            fly_v3 nrm = fly_v3norm(fly_v3mk(-(hx1 - hx0) / (2 * ee), -(hy1 - hy0) / (2 * ee), 1.0f));
            float conc = (hx0 + hx1 + hy0 + hy1) * 0.25f - h;
            float ao = 1.0f - fly_clampf(conc / (ee * 0.8f), 0.0f, 0.4f);
            /* A canopy is a lid. Litter made the floor browner, which is what
             * it is made of; this is how much sky it can still see, which is
             * what it is lit by — the two are different halves of shade and
             * albedo alone reads as a painted-on patch. Same term in the GLSL
             * vertex twin, off the same mask. */
            /* Canopy occlusion is *not* folded in here when the traced term is
               live. A vertex is 26 m from its neighbour and the trees vary
               every 15.5, so anything applied at this rate can only broaden a
               wood; the traced value is read per pixel instead, in
               raster_tri_shadowed_raw, and multiplies the ambient half there.
               With the term off this is the density scalar it replaces. */
            if (g_ao_target <= 0)
                ao *= 1.0f - 0.42f * fly_world_forest_mask(w, wx, wy);
            float slope = 1.0f - nrm.z;
            fly_v3 mat;
            fly_v3 dir = fly_v3sub(tv->pos, v->pos);
            float dist = fly_v3len(dir);
            float cs = cloud_shadow(e, tv->pos);
            fly_v3 fT, fS;
            /* one integral for the whole vertex, whichever stage shades it:
               the two cloud layers and the air between here and the camera */
            fog_pair(e, v->pos, fly_v3scale(dir, 1.0f / (dist + 1e-5f)), dist, &fT, &fS);
            tv->nrm = nrm;
            tv->cloud = cs;
            tv->ao = ao;
            tv->fogT = fT;
            tv->fogS = fS;
            /* The far rings resolve the map here and the near ones do it in
               the pixel loop, whichever stage the colour is resolved in. */
            tv->shad = 1.0f;
            if (!per_pixel) {
                float nd = fly_v3dot(nrm, e->sun);
                if (nd < 0.0f) nd = -nd;
                tv->shad = terrain_sun(e, tv->pos, nrm, nd, 0.0f);
            }
            if (e->lod.ground_px) continue;
            /* --- the budget rung's own path, from here down ---
               Everything above resolves the colour in the fragment stage; this
               is what the cheapest rung does instead, and the rate it has to
               ask for is the reason the entry existed at all.

               The per-vertex path resolves one colour per vertex, so what it
               can represent is bounded by the cell and not by the pixel: an
               octave finer than the mesh is point-sampled at the corners and
               interpolated across, which is a bias rather than a blur and is
               exactly what the GPU path stopped doing when it moved shading to
               the fragment stage.
               `terrain_surface` takes a *rate* — samples per metre — and the
               mesh's rate is one sample every `step` metres. The clamp used to
               pass `dist / step`, which is a count, so it stood at ten to
               twenty-six where the pixel rate stands at hundredths and the
               comparison never once bound. Whichever is coarser wins, and now
               it can: past about a kilometre the cell is always the coarser of
               the two, so the outer rings stop being handed octaves their own
               cells cannot carry. */
            float px = e->pxscale / (dist + 1e-3f);
            float cell = step > 0.0f ? 1.0f / step : px;
            float rate = px < cell ? px : cell;
            {
                fly_v3 alb = terrain_surface(w, wx, wy, h, slope, rate,
                                             e->ball, e->wet, &mat);
                fly_v3 vdir = fly_v3scale(dir, -1.0f / (dist + 1e-5f));
                fly_v3 lit = terrain_light(e, alb, mat, nrm, vdir, cs, ao);
                fly_v3 dark = terrain_light(e, alb, mat, nrm, vdir, 0.0f, ao);
                /* the pair travels the same air and the same cloud to get
                   here, so one integral does for both */
                tv->lit = fly_v3add(fly_v3mul(lit, fT), fS);
                tv->dark = fly_v3add(fly_v3mul(dark, fT), fS);
                if (!per_pixel) tv->col = fly_v3lerp(tv->dark, tv->lit, tv->shad);
            }
        }
    for (j = 0; j < n; ++j)
        for (i = 0; i < n; ++i) {
            fly__tv *p00 = &verts[j * vn + i], *p10 = &verts[j * vn + i + 1];
            fly__tv *p01 = &verts[(j + 1) * vn + i], *p11 = &verts[(j + 1) * vn + i + 1];
            /* skip cells fully covered by the finer inner ring */
            float mx = (p00->pos.x + p11->pos.x) * 0.5f - v->pos.x;
            float my = (p00->pos.y + p11->pos.y) * 0.5f - v->pos.y;
            /* Cull by the cell's outer edge, not its centre. A ring's guaranteed
             * coverage is step*(half-1) — its origin snaps to the step grid, so up
             * to one cell of the nominal extent can fall on the far side. Culling
             * by centre therefore drops coarse cells reaching half a cell past what
             * the finer ring covers, and the sky shows through the seam as a hard
             * line around every LOD boundary. */
            if (inner > 0.0f && fabsf(mx) + step * 0.5f < inner &&
                fabsf(my) + step * 0.5f < inner) continue;
            if (e->lod.ground_px) {
                raster_tri_ground(rt, v, e, p00, p10, p11, per_pixel);
                raster_tri_ground(rt, v, e, p00, p11, p01, per_pixel);
            } else if (per_pixel) {
                /* the ground is the ground: zero, without paying for a lookup
                   that would return it on the hottest path in the frame */
                raster_tri_shadowed(rt, v, e, p00->pos, p10->pos, p11->pos,
                                    p00->lit, p10->lit, p11->lit,
                                    p00->dark, p10->dark, p11->dark, 0.0f);
                raster_tri_shadowed(rt, v, e, p00->pos, p11->pos, p01->pos,
                                    p00->lit, p11->lit, p01->lit,
                                    p00->dark, p11->dark, p01->dark, 0.0f);
            } else {
                raster_tri_hdr(rt, v, p00->pos, p10->pos, p11->pos, p00->col, p10->col, p11->col);
                raster_tri_hdr(rt, v, p00->pos, p11->pos, p01->pos, p00->col, p11->col, p01->col);
            }
        }
}

static void raster_water_tri(fly_render_target *rt, const fly__view *v, const fly__env *e,
                             fly_v3 a, fly_v3 b, fly_v3 c,
                             const fly__wv *wa, const fly__wv *wb, const fly__wv *wc) {
    const fly__wv *w[3];
    fly__cv in[3], out[6];
    fly__wv cw[3];
    int nt, i, k;
    if (g_shadow_cast) return; /* water casts no shadow */
    w[0] = wa; w[1] = wb; w[2] = wc;
    in[0].p = a; in[1].p = b; in[2].p = c;
    for (i = 0; i < 3; ++i) {
        in[i].a[0] = w[i]->sky.x; in[i].a[1] = w[i]->sky.y; in[i].a[2] = w[i]->sky.z;
        in[i].a[3] = w[i]->fogT.x; in[i].a[4] = w[i]->fogT.y; in[i].a[5] = w[i]->fogT.z;
        in[i].a[6] = w[i]->fogS.x; in[i].a[7] = w[i]->fogS.y; in[i].a[8] = w[i]->fogS.z;
        in[i].a[9] = w[i]->depth;
        in[i].a[10] = w[i]->cloud;
    }
    nt = near_clip_tri(v, in, out, 11);
    for (k = 0; k < nt; ++k) {
        const fly__cv *t = &out[k * 3];
        for (i = 0; i < 3; ++i) {
            cw[i].sky = cv3(&t[i], 0);
            cw[i].fogT = cv3(&t[i], 3);
            cw[i].fogS = cv3(&t[i], 6);
            cw[i].depth = t[i].a[9];
            cw[i].cloud = t[i].a[10];
        }
        raster_water_raw(rt, v, e, t[0].p, t[1].p, t[2].p, &cw[0], &cw[1], &cw[2]);
    }
}

/* animated water plane with sun glint, drawn over submerged terrain */
static void water_ring(fly_render_target *rt, const fly_game *g, const fly__env *e,
                       const fly__view *v, float step, int half, float inner) {
    const fly_world *w = &g->world;
    int n = half * 2;
    if (n > RING_MAX * 2) n = RING_MAX * 2;
    int vn = n + 1;
    static fly__wv wv[(RING_MAX * 2 + 1) * (RING_MAX * 2 + 1)];
    float cx0 = floorf(v->pos.x / step) * step - half * step;
    float cy0 = floorf(v->pos.y / step) * step - half * step;
    int i, j;
    /* depths first: a vertex needs its sky and fog samples whenever any cell
     * touching it is drawn, even where that vertex itself is dry.
     *
     * The surface is the *still* water at each vertex — the sea, or a lake at
     * its own spill point — and the mesh carries it, so a lake three hundred
     * metres up is drawn by this lattice the way the sea is. The rivers are
     * left out on purpose: a channel forty metres across sampled on a sixty
     * metre lattice breaks into dashes, and a cell with one corner in the
     * water and the next outside the channel would run the surface from the
     * river down to the sea across a single triangle. A line is drawn as a
     * line — see draw_rivers. */
    for (j = 0; j < vn; ++j)
        for (i = 0; i < vn; ++i) {
            float wx = cx0 + i * step, wy = cy0 + j * step;
            float gz = fly_world_ground(w, wx, wy);
            wv[j * vn + i].z = fly_world_still(w, wx, wy, gz);
            wv[j * vn + i].depth = wv[j * vn + i].z - gz;
        }
    for (j = 0; j < vn; ++j)
        for (i = 0; i < vn; ++i) {
            fly__wv *o = &wv[j * vn + i];
            float wx = cx0 + i * step, wy = cy0 + j * step;
            int near_wet = o->depth > -0.4f;
            if (!near_wet && i > 0) near_wet = wv[j * vn + i - 1].depth > -0.4f;
            if (!near_wet && i + 1 < vn) near_wet = wv[j * vn + i + 1].depth > -0.4f;
            if (!near_wet && j > 0) near_wet = wv[(j - 1) * vn + i].depth > -0.4f;
            if (!near_wet && j + 1 < vn) near_wet = wv[(j + 1) * vn + i].depth > -0.4f;
            if (!near_wet) {
                o->sky = o->fogS = fly_v3zero();
                o->fogT = fly_v3mk(1, 1, 1);
                o->cloud = 1.0f;
                continue;
            }
            {
                fly_v3 p = fly_v3mk(wx, wy, o->z);
                fly_v3 view = fly_v3norm(fly_v3sub(v->pos, p));
                fly_v3 dir = fly_v3sub(p, v->pos);
                o->cloud = cloud_shadow(e, p);
                /* reflect about the mean surface: the wave tilt moves this by a
                 * few degrees, far less than the sky varies across a cell */
                o->sky = sky_ambient(e, p, fly_v3norm(fly_v3mk(-view.x, -view.y, view.z * 0.5f + 1.0f)));
                {
                    float dl = fly_v3len(dir);
                    fog_pair(e, v->pos, fly_v3scale(dir, 1.0f / (dl + 1e-5f)), dl,
                             &o->fogT, &o->fogS);
                }
            }
        }
    for (j = 0; j < n; ++j)
        for (i = 0; i < n; ++i) {
            const fly__wv *v00 = &wv[j * vn + i], *v10 = &wv[j * vn + i + 1];
            const fly__wv *v01 = &wv[(j + 1) * vn + i], *v11 = &wv[(j + 1) * vn + i + 1];
            if (v00->depth <= -0.4f && v10->depth <= -0.4f &&
                v01->depth <= -0.4f && v11->depth <= -0.4f) continue;
            float x0 = cx0 + i * step, y0 = cy0 + j * step;
            float mx = x0 + step * 0.5f - v->pos.x, my = y0 + step * 0.5f - v->pos.y;
            /* cull by the cell's outer edge; see terrain_ring for why */
            if (inner > 0.0f && fabsf(mx) + step * 0.5f < inner &&
                fabsf(my) + step * 0.5f < inner) continue;
            fly_v3 p00 = fly_v3mk(x0, y0, v00->z);
            fly_v3 p10 = fly_v3mk(x0 + step, y0, v10->z);
            fly_v3 p01 = fly_v3mk(x0, y0 + step, v01->z);
            fly_v3 p11 = fly_v3mk(x0 + step, y0 + step, v11->z);
            raster_water_tri(rt, v, e, p00, p10, p11, v00, v10, v11);
            raster_water_tri(rt, v, e, p00, p11, p01, v00, v11, v01);
        }
}

/* Is a patch of ground on screen at all?
 *
 * The three ground scatters below — woodland, blades, boulders — each walk a
 * disc of cells centred on the camera, and a disc is not what a camera sees.
 * A 16:9 frame at this field of view covers about a quarter of the compass, so
 * three quarters of every scatter was being placed, height-sampled, shaded and
 * fogged before `project` threw it away for being behind the eye: the shading
 * happens per vertex at the moment a triangle is built, and the rejection
 * happens after that. The disc is what the *world* looks like and the scatter
 * has to stay deterministic per cell, so the fix is not a smaller disc, it is
 * asking the frustum first.
 *
 * `r` bounds everything the patch can put in the air — the cell's own diagonal
 * plus what stands on it — and it is deliberately generous: a scatter that
 * culls a tree you can see is a wood that pops, and the whole saving here is
 * in the cells that are nowhere near the edge.
 *
 * Not used by the shadow cast. An occluder outside the frame casts into it,
 * and the caster walks its own cascade-centred region for exactly that
 * reason. */
/* The ambient contact ring a thing standing on the ground gets — defined with
 * the settlement kit, because that is where it was needed first, and declared
 * here because the scatter needs it too: a boulder and a tree stand on the
 * ground exactly the way a shed does. See prop_skirt. */
static void prop_skirt(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g, fly_v3 c, float hx, float hy, float yaw,
                       float reach, float depth);
/* The ground a scattered thing stands on, memoized — declared here for the same
 * reason prop_skirt is, and defined with the other heightfield memo. Every point
 * below is a fixed function of its world cell, so a cell that was in range last
 * frame asks for exactly the coordinates it asked for then. */
static float scatter_ground(const fly_world *w, float x, float y);

static int patch_visible(const fly__view *v, const fly_render_target *rt,
                         float x, float y, float z, float r) {
    return sphere_visible(v, rt->w, rt->h, fly_v3mk(x, y, z), r);
}

/* is this spot clear of settlements? (aprons stay mowed) */
static int clear_of_sites(const fly_world *w, float x, float y, float margin) {
    int li;
    for (li = 0; li < w->nloc; ++li) {
        float lx = x - w->loc[li].pos.e, ly = y - w->loc[li].pos.n;
        if (lx * lx + ly * ly < margin * margin) return 0;
    }
    return 1;
}

/* grass layer: wind-swaying blade tufts in several species — lush clumps,
 * dry golden grass, wildflowers, shoreline reeds — scattered densely near
 * the camera; deterministic per world cell */
static void draw_grass(fly_render_target *rt, const fly_game *g, const fly__env *e,
                       const fly__view *v) {
    const fly_world *w = &g->world;
    const float cell = 9.0f;
    const float radius = e->lod.grass;
    /* The blade layer is the most expensive thing per pixel of ground in the
     * frame — three or four triangles a tuft, thousands of tufts, all of them
     * under a metre across — so it is the first thing the budget rung gives
     * up, and the reach is how the rungs above it are priced. */
    if (radius <= 0.0f) return;
    if (v->pos.z - fly_world_ground(w, v->pos.x, v->pos.y) > 500.0f) return; /* too high to matter */
    int ix0 = (int)floorf((v->pos.x - radius) / cell), ix1 = (int)ceilf((v->pos.x + radius) / cell);
    int iy0 = (int)floorf((v->pos.y - radius) / cell), iy1 = (int)ceilf((v->pos.y + radius) / cell);
    float wspd = fly_v3len(fly_v3mk(e->wind.x, e->wind.y, 0));
    fly_v3 wdir = wspd > 0.5f ? fly_v3norm(fly_v3mk(e->wind.x, e->wind.y, 0)) : fly_v3mk(1, 0, 0);
    float day_l = 0.18f + 0.82f * e->day;
    int ix, iy;
    for (iy = iy0; iy <= iy1; ++iy)
        for (ix = ix0; ix <= ix1; ++ix) {
            uint32_t h1 = fly_hash2(w->seed + 81, ix, iy);
            /* how many of the eight get a tuft: half at the default rung, all
             * of them at the top, and the pattern is nested rather than
             * reshuffled, so raising the level thickens the same meadow */
            if ((int)(h1 & 7u) >= e->lod.grass_den) continue;
            float px = ((float)ix + 0.15f + (float)((h1 >> 3) & 15) / 22.0f) * cell;
            float py = ((float)iy + 0.15f + (float)((h1 >> 7) & 15) / 22.0f) * cell;
            float dx = px - v->pos.x, dy = py - v->pos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) continue;
            float gz = scatter_ground(w, px, py);
            if (gz > 900.0f) continue;
            int reed = gz > FLY_WATER_Z + 0.15f && gz < FLY_WATER_Z + 2.6f;
            if (gz < FLY_WATER_Z + 0.15f) continue;
            /* a tuft is a metre of nothing; ask the frustum before the second
             * heightfield sample, the site scan and the three blades */
            if (!patch_visible(v, rt, px, py, gz + 1.0f, 2.6f)) continue;
            /* keep off steep rock */
            if (fabsf(scatter_ground(w, px + 7.0f, py) - gz) > 2.6f) continue;
            if (!clear_of_sites(w, px, py, 95.0f)) continue;

            float region = grass_region(w->seed, px, py);
            int dry = region > 0.18f, heather = region < -0.25f;
            int flower = !reed && !dry && ((h1 >> 11) & 15u) == 3u;
            float tall = reed ? 1.3f + (float)((h1 >> 13) & 3) * 0.25f
                              : 0.45f + (float)((h1 >> 13) & 7) * 0.08f * (dry ? 1.5f : 1.0f);
            fly_v3 col = reed ? fly_v3mk(0.09f, 0.17f, 0.08f)
                        : dry ? fly_v3mk(0.42f, 0.34f, 0.13f)
                        : heather ? fly_v3mk(0.20f, 0.19f, 0.16f)
                                  : fly_v3mk(0.11f, 0.25f, 0.10f);
            /* under a canopy the tufts go the way the floor does, or a wood
               ends up with meadow-bright grass growing in its shade */
            col = fly_v3lerp(col, fly_v3mk(0.055f, 0.075f, 0.045f),
                             fly_world_forest_mask(w, px, py) * 0.62f);
            /* And what is growing where the ground is worked. A meadow tuft in
             * a standing crop is a weed, and in a ploughed field it is a
             * mistake: the blade layer takes the field's own colour and the
             * crop's own height, and on bare earth there is nothing to draw.
             * The parcel says which — see crop_colour, whose vegetation
             * channel is exactly "is anything standing here". */
            {
                fly_tilth ti;
                float cv;
                fly_v3 cc;
                fly_world_tilth(w, px, py, gz, &ti);
                if (ti.work > 0.15f) {
                    cc = crop_colour(ti.crop, &cv);
                    if (cv < 0.2f && ti.work > 0.45f) continue;
                    col = fly_v3lerp(col, fly_v3scale(cc, 0.72f), ti.work * cv);
                    tall *= 1.0f + 1.25f * cv * ti.work;
                    flower = 0;
                }
            }
            col = fly_v3scale(col, day_l * (0.85f + 0.3f * (float)((h1 >> 20) & 7) / 7.0f));
            uint32_t bladec = v3_to_rgb(col);
            /* wind sway: lean + flutter */
            float sway = fly_clampf(wspd / 13.0f, 0.06f, 0.85f) * tall *
                         (0.55f + 0.45f * sinf((float)e->time * (1.6f + (float)(h1 & 3) * 0.5f) +
                                               (float)((h1 >> 5) & 31)));
            float sx = wdir.x * sway, sy2 = wdir.y * sway;
            fly_v3 bp = fly_v3mk(px, py, gz);
            int nb = reed ? 3 : 2 + (int)(h1 & 1u);
            int b;
            for (b = 0; b < nb; ++b) {
                float ba = (float)b * 2.09f + (float)((h1 >> 9) & 7) * 0.8f;
                float bx = cosf(ba) * 0.22f, by = sinf(ba) * 0.22f;
                float wdt = reed ? 0.055f : 0.10f;
                raster_tri_view(rt, v,
                                fly_v3add(bp, fly_v3mk(bx - wdt, by, 0)),
                                fly_v3add(bp, fly_v3mk(bx + wdt, by, 0)),
                                fly_v3add(bp, fly_v3mk(bx * 2.2f + sx, by * 2.2f + sy2, tall)),
                                bladec);
            }
            if (flower) {
                static const uint32_t petals[3] = { FLY_RGB(235, 210, 90), FLY_RGB(230, 228, 224),
                                                    FLY_RGB(186, 120, 220) };
                uint32_t pc = petals[(h1 >> 16) & 3u ? ((h1 >> 16) & 3u) - 1u : 0u];
                fly_v3 tip = fly_v3add(bp, fly_v3mk(sx, sy2, tall + 0.08f));
                raster_tri_view(rt, v, fly_v3add(tip, fly_v3mk(-0.09f, 0, 0)),
                                fly_v3add(tip, fly_v3mk(0.09f, 0, 0)),
                                fly_v3add(tip, fly_v3mk(0, 0, 0.14f)), pc);
            }
        }
}

/* scattered boulders on rocky ground */
static void draw_boulders(fly_render_target *rt, const fly_game *g, const fly__env *e,
                          const fly__view *v) {
    const fly_world *w = &g->world;
    const float cell = 64.0f, radius = 850.0f;
    int ix0 = (int)floorf((v->pos.x - radius) / cell), ix1 = (int)ceilf((v->pos.x + radius) / cell);
    int iy0 = (int)floorf((v->pos.y - radius) / cell), iy1 = (int)ceilf((v->pos.y + radius) / cell);
    int ix, iy;
    for (iy = iy0; iy <= iy1; ++iy)
        for (ix = ix0; ix <= ix1; ++ix) {
            uint32_t h1 = fly_hash2(w->seed + 83, ix, iy);
            if ((h1 & 15u) != 3u) continue; /* sparse */
            float px = ((float)ix + 0.5f + (float)((h1 >> 4) & 7) / 16.0f) * cell;
            float py = ((float)iy + 0.5f + (float)((h1 >> 7) & 7) / 16.0f) * cell;
            float dx = px - v->pos.x, dy = py - v->pos.y;
            if (dx * dx + dy * dy > radius * radius) continue;
            float gz = scatter_ground(w, px, py);
            if (gz < FLY_WATER_Z + 1.0f) continue;
            if (!patch_visible(v, rt, px, py, gz + 1.6f, 4.6f)) continue;
            if (!clear_of_sites(w, px, py, 110.0f)) continue;
            float s = 0.8f + (float)((h1 >> 10) & 7) * 0.28f;
            float ang = (float)((h1 >> 13) & 7) * 0.8f;
            fly_v3 bp = fly_v3mk(px, py, gz - s * 0.15f);
            fly_v3 rock = fly_v3scale(fly_v3mk(0.30f, 0.28f, 0.26f),
                                      0.8f + 0.3f * (float)((h1 >> 16) & 7) / 7.0f);
            fly_v3 apex = fly_v3add(bp, fly_v3mk(cosf(ang) * s * 0.2f, sinf(ang) * s * 0.2f, s));
            fly_v3 c0 = fly_v3add(bp, fly_v3mk(cosf(ang) * s, sinf(ang) * s, 0));
            fly_v3 c1 = fly_v3add(bp, fly_v3mk(cosf(ang + 2.1f) * s * 0.85f, sinf(ang + 2.1f) * s * 0.85f, 0));
            fly_v3 c2 = fly_v3add(bp, fly_v3mk(cosf(ang + 4.2f) * s * 1.1f, sinf(ang + 4.2f) * s * 1.1f, 0));
            tri_shaded(rt, v, e, apex, c0, c1, rock, 1);
            tri_shaded(rt, v, e, apex, c1, c2, rock, 1);
            tri_shaded(rt, v, e, apex, c2, c0, fly_v3scale(rock, 0.85f), 1);
            /* the ambient contact, same as everything else that stands on the
             * ground: a boulder occludes the sky from the grass round its foot
             * whether or not there is a sun to cast a shadow. The skirt's own
             * projected-size gate keeps this to the near field. */
            prop_skirt(rt, v, e, g, fly_v3mk(px, py, gz), s * 0.8f, s * 0.8f, ang,
                       s * 0.9f, 0.45f);
        }
}

/* How much of a tree survives at this distance: 2 near, 1 mid, 0 far.
 * `reach` is the graphics level's multiplier on every band, so a higher rung
 * carries the detailed model further out rather than drawing a different
 * tree. */
static int tree_lod(float d2, float reach) {
    float r = reach > 0.05f ? reach : 0.05f;
    float d = d2 / (r * r);
    if (d < 240.0f * 240.0f) return 2;
    if (d < 850.0f * 850.0f) return 1;
    return 0;
}

/* A crown vertex, ragged in radius and in height.
 *
 * Both matter and the second one is the one that was missing. A ring jittered
 * only in radius still lies in a plane, and a surface lofted from a planar ring
 * to a point is a cone whatever the radii do — which is what a wood of these
 * looked like from inside it: flat-sided pyramids with a hard rim. Lifting and
 * dropping the vertices as well breaks the outline, which is the single
 * strongest cue that a mass is foliage and not a solid.
 *
 * `shift` walks the hash so neighbouring vertices are independent, and every
 * offset is a fraction of the tree's own size, so a sapling is as ragged as a
 * standard in proportion and neither is ragged in absolute metres. */
static void crown_pt(fly_v3 *out, fly_v3 c, float ang, float r, float s,
                     uint32_t h, int shift, int tall) {
    float jr = 0.72f + 0.52f * (float)((h >> (shift & 27)) & 3) / 3.0f;
    float jz = ((float)((h >> ((shift + 5) & 27)) & 7) / 7.0f - 0.5f) *
               (tall ? 0.26f : 0.15f) * s;
    *out = fly_v3add(c, fly_v3mk(cosf(ang) * r * jr, sinf(ang) * r * jr, jz));
}

/* How much lighter or darker one face of a crown is than its neighbours.
 * Light through a canopy is broken by the leaves in front of the leaves, and a
 * few per cent per face is what that looks like from outside; a crown lit by
 * one number reads as a painted solid. */
static float leaf_face_tint(uint32_t h, int f) {
    return 0.92f + 0.16f * (float)((h >> ((f * 3 + 9) & 27)) & 3) / 3.0f;
}

/* One tree, three species, three levels of detail.
 *
 * A forest needs all three: the nine-triangle model that reads as a spruce at
 * 40 m is invisible detail at 900, and the single triangle that reads as a
 * tree at 900 is a bowtie at 40. The old scatter drew one model at every
 * distance — two crossed triangles — which is why the near ground looked like
 * cardboard and the far ground looked bare.
 *
 * species: 0 conifer (tiered, tapering), 1 broadleaf (round crown on a bare
 * trunk), 2 snag (dead, bare, leaning). */
static void draw_tree_model(fly_render_target *rt, const fly__view *v, const fly__env *e,
                            fly_v3 bp, float s, int species, fly_v3 leaf, uint32_t h, int lod) {
    fly_v3 bark = fly_v3mk(0.20f, 0.14f, 0.09f);
    float lean = ((float)((h >> 22) & 7) - 3.5f) * 0.02f;
    /* Which way round the tree is standing.
     *
     * It used to be four positions — `(h & 3) * 0.4` on a face spacing of
     * 0.79 radians — and four is not a rotation, it is a small set of poses.
     * From the air that is exactly what a stand looked like: identical dots on
     * a grid, because the crowns were not merely the same shape but the same
     * shape pointing the same few ways. Sixty-four positions over the whole
     * turn costs the same one shift and is a rotation. */
    float spin = (float)((h >> 25) & 63u) * (6.2832f / 64.0f);
    fly_v3 top = fly_v3add(bp, fly_v3mk(lean * s, lean * s * 0.6f, s));

    if (lod == 0) { /* one triangle, canopy colour, no trunk worth a pixel */
        float cw = s * (species == 1 ? 0.40f : 0.32f);
        /* Still the canopy BRDF — a distant hillside of woodland is exactly
           where transmission and the down-sun surge do their work, and at one
           triangle a tree there is the cheapest place in the frame to buy it. */
        tri_leaf(rt, v, e, fly_v3add(bp, fly_v3mk(-cw, 0, s * 0.18f)),
                 fly_v3add(bp, fly_v3mk(cw, 0, s * 0.18f)), top, leaf,
                 fly_v3add(bp, fly_v3mk(0, 0, s * 0.45f)), 0.9f);
        return;
    }

    if (species == 2) { /* snag: a bare leaning trunk and two broken limbs */
        fly_v3 t2 = fly_v3add(bp, fly_v3mk(s * 0.16f, s * 0.10f, s * 0.72f));
        tri_shaded(rt, v, e, fly_v3add(bp, fly_v3mk(-s * 0.06f, 0, 0)),
                   fly_v3add(bp, fly_v3mk(s * 0.06f, 0, 0)), t2, bark, 1);
        tri_shaded(rt, v, e, fly_v3add(bp, fly_v3mk(0, -s * 0.06f, 0)),
                   fly_v3add(bp, fly_v3mk(0, s * 0.06f, 0)), t2, fly_v3scale(bark, 0.8f), 1);
        if (lod == 2) {
            fly_v3 m = fly_v3lerp(bp, t2, 0.6f);
            tri_shaded(rt, v, e, m, fly_v3add(m, fly_v3mk(s * 0.30f, s * 0.06f, s * 0.10f)),
                       fly_v3add(m, fly_v3mk(s * 0.26f, -s * 0.04f, s * 0.04f)),
                       fly_v3scale(bark, 0.9f), 1);
            tri_shaded(rt, v, e, m, fly_v3add(m, fly_v3mk(-s * 0.24f, s * 0.10f, s * 0.14f)),
                       fly_v3add(m, fly_v3mk(-s * 0.20f, s * 0.16f, s * 0.08f)),
                       fly_v3scale(bark, 0.75f), 1);
        }
        return;
    }

    { /* trunk: a three-sided taper, so it has a lit side and a shaded one */
        float r = s * (species == 1 ? 0.045f : 0.038f);
        float ha = species == 1 ? s * 0.46f : s * 0.30f; /* clear bole under a crown */
        fly_v3 apex = fly_v3add(bp, fly_v3mk(lean * s * 0.5f, lean * s * 0.3f, ha));
        int f;
        for (f = 0; f < 3; ++f) {
            float a0 = (float)f * 2.0944f, a1 = a0 + 2.0944f;
            tri_shaded(rt, v, e, fly_v3add(bp, fly_v3mk(cosf(a0) * r, sinf(a0) * r, 0)),
                       fly_v3add(bp, fly_v3mk(cosf(a1) * r, sinf(a1) * r, 0)), apex,
                       fly_v3scale(bark, 0.75f + 0.25f * (float)f / 2.0f), 1);
        }
    }

    if (species == 1) {
        /* Broadleaf: a crown with a waist, and a ragged one.
         *
         * The first cut hung a 0.36s ring at 0.71s with the apex at 0.98s — a
         * cone a quarter as tall as it was broad, which reads as a parasol.
         * Dropping the ring and raising the apex fixed the proportions and
         * left the shape: *one* ring is a diamond however it is proportioned,
         * because every point on the surface is on a straight line from the
         * ring to the apex. Standing in a wood you could see it — a field of
         * flat-sided pyramids with a hard rim and a visible flat underside.
         *
         * A crown needs a waist. Two rings, a shoulder and a belly, and the
         * silhouette curves between them. Both are ragged in radius *and* in
         * height, which is what actually breaks the outline: a lumpy edge is
         * the single strongest cue that a mass is foliage rather than a solid,
         * and it costs nothing but the jitter — the vertices were being placed
         * anyway. The near model is 24 triangles against 12; the far ones are
         * untouched, and past 240 m a crown is a dozen pixels and this is all
         * invisible. */
        /* The crown's profile, in units of the tree's own height: three rings
         * between a point underneath and the apex. Where the widest ring sits
         * is the whole shape — put it a quarter of the way up and the crown is
         * a mushroom, which is what the two-ring cut gave. Half way up and
         * slightly nearer the top than the bottom is a broadleaf. */
        static const float C[3][2] = { { 0.54f, 0.32f }, { 0.70f, 0.45f }, { 0.86f, 0.34f } };
        float cz = s * 0.40f;                 /* where the crown starts */
        float ct = s * 1.04f;                 /* and where it ends */
        fly_v3 hub = fly_v3add(bp, fly_v3mk(lean * s * 0.6f, lean * s * 0.35f, s * 0.70f));
        fly_v3 crown = fly_v3add(bp, fly_v3mk(lean * s, lean * s * 0.6f, ct));
        fly_v3 under = fly_v3add(bp, fly_v3mk(0, 0, cz));
        int f, nf = lod == 2 ? 8 : 4;
        if (lod < 2) {
            /* One ring for the middle distance, and it keeps the cheap inline
             * jitter rather than the crown vertex helper. Past 240 m a crown is
             * a dozen pixels; running the ragged placement over the thirteen
             * thousand trees the middle band carries cost six per cent of the
             * frame and bought a shape nobody at that range can resolve, which
             * is the wrong side of the trade this whole function is built on. */
            float cr = s * 0.42f;
            fly_v3 mid = fly_v3add(bp, fly_v3mk(lean * s * 0.5f, lean * s * 0.3f, s * 0.63f));
            for (f = 0; f < nf; ++f) {
                float a0 = (float)f * (6.2832f / (float)nf) + spin;
                float a1 = a0 + 6.2832f / (float)nf;
                float r0 = cr * (0.82f + 0.30f * (float)((h >> (f * 2 + 4)) & 3) / 3.0f);
                float r1 = cr * (0.82f + 0.30f * (float)((h >> (f * 2 + 6)) & 3) / 3.0f);
                fly_v3 p0 = fly_v3add(mid, fly_v3mk(cosf(a0) * r0, sinf(a0) * r0, 0));
                fly_v3 p1 = fly_v3add(mid, fly_v3mk(cosf(a1) * r1, sinf(a1) * r1, 0));
                tri_leaf(rt, v, e, p0, p1, crown, leaf, mid, 1.0f);
                tri_leaf(rt, v, e, p1, p0, under, fly_v3scale(leaf, 0.82f), mid, 0.72f);
            }
            return;
        }
        for (f = 0; f < nf; ++f) {
            float a0 = (float)f * (6.2832f / (float)nf) + spin;
            float a1 = a0 + 6.2832f / (float)nf;
            fly_v3 q[3][2];   /* [station][edge of this face] */
            float tf = leaf_face_tint(h, f);
            int r;
            for (r = 0; r < 3; ++r) {
                fly_v3 c = fly_v3add(bp, fly_v3mk(lean * s * C[r][0], lean * s * C[r][0] * 0.6f,
                                                  s * C[r][0]));
                crown_pt(&q[r][0], c, a0, s * C[r][1], s, h, f * 3 + r * 11, 1);
                crown_pt(&q[r][1], c, a1, s * C[r][1], s, h, (f + 1) * 3 + r * 11, 1);
            }
            /* Each face at its own value. A crown lit by one number is a solid;
               the light through a canopy is broken by the leaves in front of
               the leaves, and a few per cent of scatter per face is what that
               looks like from outside. */
            tri_leaf(rt, v, e, q[0][0], q[0][1], under, fly_v3scale(leaf, 0.74f * tf), hub, 0.64f);
            for (r = 0; r + 1 < 3; ++r) {
                float band = tf * (r == 0 ? 0.93f : 1.0f);
                tri_leaf(rt, v, e, q[r][0], q[r][1], q[r + 1][1],
                         fly_v3scale(leaf, band), hub, 0.90f + 0.05f * (float)r);
                tri_leaf(rt, v, e, q[r][0], q[r + 1][1], q[r + 1][0],
                         fly_v3scale(leaf, band * 0.98f), hub, 0.90f + 0.05f * (float)r);
            }
            tri_leaf(rt, v, e, q[2][0], q[2][1], crown, fly_v3scale(leaf, tf * 1.05f), hub, 1.0f);
        }
        return;
    }

    { /* Conifer: stacked skirts, each narrower and shorter than the one below.
       *
       * The skirts were two crossed quads apiece — a bowtie, and from inside a
       * stand you could see straight through the gap between the blades. A
       * skirt is a ring now, ragged at the tips the way a bough is, and the
       * leader carries a spike: a spruce is read by its point, and a spruce
       * whose top is a flat pair of triangles is a bush. Fifteen triangles at
       * the near LOD against six; the two-tier middle distance keeps the
       * crossed pair, where a whole tree is a few pixels wide. */
        int tiers = lod == 2 ? 3 : 2, t;
        for (t = 0; t < tiers; ++t) {
            float f0 = 0.16f + 0.27f * (float)t;              /* skirt base */
            float f1 = f0 + (tiers == 3 ? 0.46f : 0.62f);      /* skirt apex */
            float cw = s * (0.47f - 0.12f * (float)t);
            fly_v3 b0 = fly_v3add(bp, fly_v3mk(lean * s * f0, lean * s * f0 * 0.6f, s * f0));
            fly_v3 a0 = fly_v3add(bp, fly_v3mk(lean * s * f1, lean * s * f1 * 0.6f, s * f1));
            /* the skirt's centre is on the trunk at its own height, so the
               normal runs outward from the stem the way a branch does, and the
               lower skirts sit in more of their own shade */
            fly_v3 hub = fly_v3add(bp, fly_v3mk(lean * s * f0, lean * s * f0 * 0.6f,
                                                s * (f0 + f1) * 0.5f));
            float sao = 0.62f + 0.19f * (float)t;
            if (lod < 2) {
                tri_leaf(rt, v, e, fly_v3add(b0, fly_v3mk(-cw, 0, 0)),
                         fly_v3add(b0, fly_v3mk(cw, 0, 0)), a0, leaf, hub, sao);
                tri_leaf(rt, v, e, fly_v3add(b0, fly_v3mk(0, -cw, 0)),
                         fly_v3add(b0, fly_v3mk(0, cw, 0)), a0, leaf, hub, sao);
                continue;
            }
            {
                int f, nf = 5;
                for (f = 0; f < nf; ++f) {
                    float ang0 = (float)f * (6.2832f / (float)nf) + spin +
                                 (float)t * 0.5f;
                    float ang1 = ang0 + 6.2832f / (float)nf;
                    fly_v3 p0, p1;
                    float tf = leaf_face_tint(h, f + t * 5);
                    crown_pt(&p0, b0, ang0, cw, s, h, f * 3 + t * 7, 0);
                    crown_pt(&p1, b0, ang1, cw, s, h, (f + 1) * 3 + t * 7, 0);
                    tri_leaf(rt, v, e, p0, p1, a0, fly_v3scale(leaf, tf), hub, sao);
                }
            }
        }
        if (lod == 2) {
            /* the leader: the spike that says spruce */
            fly_v3 tip = fly_v3add(bp, fly_v3mk(lean * s * 1.06f, lean * s * 0.64f, s * 1.06f));
            fly_v3 nk = fly_v3add(bp, fly_v3mk(lean * s * 0.86f, lean * s * 0.52f, s * 0.86f));
            fly_v3 hub = fly_v3lerp(nk, tip, 0.4f);
            float lw = s * 0.10f;
            int f;
            for (f = 0; f < 3; ++f) {
                float a0 = (float)f * 2.0944f, a1 = a0 + 2.0944f;
                tri_leaf(rt, v, e, fly_v3add(nk, fly_v3mk(cosf(a0) * lw, sinf(a0) * lw, 0)),
                         fly_v3add(nk, fly_v3mk(cosf(a1) * lw, sinf(a1) * lw, 0)), tip,
                         fly_v3scale(leaf, 1.05f), hub, 0.95f);
            }
        }
        (void)top;
    }
}

/* A tree is one point to the air and to the cloud deck — see fog_anchor_begin.
 * The sphere is the model's own: it stands `s` tall and its crown reaches about
 * 0.6s across, and the tallest thing the scatter plants is a far-band clump at
 * s = 22 m, so the radius is inside FLY_FOG_ANCHOR_R at every rung and it is
 * the angular half of the bound that decides whether any given tree shares. */
/* The half of a tree an instance does not carry. `draw_tree_model` again, with
 * the leaves switched off — one model, three ways of asking it. */
static void draw_tree_bole(fly_render_target *rt, const fly__view *v, const fly__env *e,
                           fly_v3 bp, float s, int species, uint32_t h, int lod) {
    g_wood_bole_only = 1;
    draw_tree_model(rt, v, e, bp, s, species, fly_v3mk(1, 1, 1), h, lod);
    g_wood_bole_only = 0;
}

/* One instance list per species, level of detail and baked shape. The lists are
 * kept between frames — they refill from empty every frame and a scatter's
 * count wanders by a few per cent, so the allocation settles immediately. */
typedef struct {
    float *v;
    int n, cap;
    int overflow;
} fly__woodlist;

static fly__woodlist g_wood_list[3][3][FLY_WOOD_SHAPES];

static void wood_lists_reset(void) {
    int sp, lo, k;
    for (sp = 0; sp < 3; ++sp)
        for (lo = 0; lo < 3; ++lo)
            for (k = 0; k < FLY_WOOD_SHAPES; ++k) {
                g_wood_list[sp][lo][k].n = 0;
                g_wood_list[sp][lo][k].overflow = 0;
            }
}

/* Record one tree, or answer 0 and let the model draw it.
 *
 * A snag has no canopy at all — it is bare timber through `tri_shaded` — so it
 * is not an instance of anything and goes down the old path whole. */
static int wood_inst_add(const fly__env *e, fly_v3 bp, float s, int species,
                         fly_v3 leaf, uint32_t h, int lod) {
    fly__woodlist *L;
    float *o, ang;
    if (species < 0 || species > 1 || lod < 0 || lod > 2) return 0;
    L = &g_wood_list[species][lod][h & (FLY_WOOD_SHAPES - 1)];
    if (L->n + 1 > L->cap) {
        int want = L->cap ? L->cap * 2 : 256;
        float *nv = (float *)realloc(L->v, (size_t)want * FLY_WOOD_IFLOATS * sizeof(float));
        if (!nv) { L->overflow = 1; return 0; }
        L->v = nv;
        L->cap = want;
    }
    /* The turn is off the same bits the model's own `spin` uses, and it is on
     * top of the one baked into this shape rather than instead of it: eight
     * shapes, each free to point anywhere. */
    ang = (float)((h >> 25) & 63u) * (6.2832f / 64.0f);
    o = &L->v[(size_t)L->n * FLY_WOOD_IFLOATS];
    o[0] = bp.x; o[1] = bp.y; o[2] = bp.z;
    o[3] = s;
    o[4] = leaf.x; o[5] = leaf.y; o[6] = leaf.z;
    o[7] = cosf(ang); o[8] = sinf(ang);
    /* The height the traced occlusion window is read at. One lattice lookup a
     * tree rather than one a triangle, which is the same trade the fog anchor
     * makes and for the same reason: a crown is one point to a field this
     * smooth. */
    o[9] = tri_agl(e, fly_v3add(bp, fly_v3mk(0.0f, 0.0f, s * 0.55f)));
    ++L->n;
    return 1;
}

static void draw_tree(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 bp, float s, int species, fly_v3 leaf, uint32_t h, int lod) {
    /* The crown goes to the instance list when one is being collected, and the
     * bole below still goes through the ordinary capture — which is why this is
     * a flag on `draw_tree` and not on the scatter walk: the walk's frustum
     * test, its road verge, its species roll and its fade are all still doing
     * exactly what they did, and only the crown geometry moves. */
    if (g_wood_collect && wood_inst_add(e, bp, s, species, leaf, h, lod)) {
        fog_anchor_begin(fly_v3add(bp, fly_v3mk(0.0f, 0.0f, s * 0.55f)), s * 0.75f);
        draw_tree_bole(rt, v, e, bp, s, species, h, lod);
        fog_anchor_end();
        return;
    }
    fog_anchor_begin(fly_v3add(bp, fly_v3mk(0.0f, 0.0f, s * 0.55f)), s * 0.75f);
    draw_tree_model(rt, v, e, bp, s, species, leaf, h, lod);
    fog_anchor_end();
}

/* --- underbrush ----------------------------------------------------------
 *
 * A wood is not trunks standing on a lawn. What is actually under and around
 * one is scrub — bramble, gorse, hazel, whatever the wood is made of coming up
 * where the light gets in — and its absence is why the scatter read as a grid
 * of identical dots with nothing between them however good each tree was: the
 * eye reads the *spacing* when there is nothing at the intermediate scale to
 * read instead.
 *
 * A bush is a squashed dome on no trunk: three or four faces from a ragged ring
 * up to a low apex, through the same canopy BRDF the crowns use so it wraps and
 * transmits like foliage rather than like a green rock. It is near-field only —
 * a metre and a half of shrub is invisible past a couple of hundred metres and
 * there are more of them than there are trees — and it never casts, because a
 * shadow map with texels a tenth of a metre across has better things to draw
 * than a gorse bush. */
static void draw_bush_model(fly_render_target *rt, const fly__view *v, const fly__env *e,
                            fly_v3 bp, float s, fly_v3 leaf, uint32_t h) {
    /* Two rings and a crown, not one ring and an apex: a single ring lofted to
     * a point is a cone however its radii are jittered, and a field of cones a
     * metre and a half high does not read as scrub, it reads as traffic
     * management. The shoulder ring is the whole shape — wide, low, and only a
     * little narrower than the skirt under it, which is what makes the mass
     * round rather than pointed. */
    static const float C[2][2] = { { 0.22f, 0.78f }, { 0.62f, 0.60f } };  /* (height, radius) */
    fly_v3 apex = fly_v3add(bp, fly_v3mk(0, 0, s * 0.95f));
    fly_v3 hub = fly_v3add(bp, fly_v3mk(0, 0, s * 0.40f));
    float rot = (float)((h >> 17) & 63u) * (6.2832f / 64.0f);
    int f, nf = 5;
    for (f = 0; f < nf; ++f) {
        float a0 = rot + (float)f * (6.2832f / (float)nf);
        float a1 = a0 + 6.2832f / (float)nf;
        float tf = leaf_face_tint(h, f);
        fly_v3 q[2][2];
        int r;
        for (r = 0; r < 2; ++r) {
            fly_v3 c = fly_v3add(bp, fly_v3mk(0, 0, s * C[r][0]));
            crown_pt(&q[r][0], c, a0, s * C[r][1], s, h, f * 3 + r * 9, 0);
            crown_pt(&q[r][1], c, a1, s * C[r][1], s, h, (f + 1) * 3 + r * 9, 0);
        }
        /* the skirt takes less sky than the top does, the way it does on a
         * crown — a shrub lit by one number is a green stone */
        tri_leaf(rt, v, e, q[0][0], q[0][1], q[1][1], fly_v3scale(leaf, tf * 0.88f), hub, 0.62f);
        tri_leaf(rt, v, e, q[0][0], q[1][1], q[1][0], fly_v3scale(leaf, tf * 0.88f), hub, 0.62f);
        tri_leaf(rt, v, e, q[1][0], q[1][1], apex, fly_v3scale(leaf, tf), hub, 0.86f);
    }
}

/* and a bush is one point to the air the same way a tree is */
static void draw_bush(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 bp, float s, fly_v3 leaf, uint32_t h) {
    fog_anchor_begin(fly_v3add(bp, fly_v3mk(0.0f, 0.0f, s * 0.5f)), s * 0.8f);
    draw_bush_model(rt, v, e, bp, s, leaf, h);
    fog_anchor_end();
}

/* Procedural woodland: stands of conifer, broadleaf and dead timber on gentle,
 * low ground, thinning at the treeline, the shoreline and the edge of a
 * settlement's clearing, with scrub between them. */
static void draw_foliage(fly_render_target *rt, const fly_game *g, const fly__env *e,
                         const fly__view *v) {
    const fly_world *w = &g->world;
    const float cell = 44.0f;
    /* Casting, a wood is drawn round the near cascade rather than round the
     * eye, and drawn cheap.
     *
     * A forest that casts nothing is lit as though it were transparent: the
     * floor under a closed canopy took full sun, which is the single largest
     * lighting error left in the frame and no amount of albedo fixes it. The
     * whole 2.6 km scatter is far too much to put through the caster, so only
     * the near cascade's own footprint casts — that is the cascade with texels
     * a tenth of a metre across, the one where a crown's shadow is worth
     * having — and it casts the mid-detail model, because a shadow is a
     * silhouette and the third skirt of a spruce is not in it. */
    int casting = g_shadow_cast != NULL;
    float ox = casting ? g_shadow_cast->center[0].x : v->pos.x;
    float oy = casting ? g_shadow_cast->center[0].y : v->pos.y;
    float radius = casting ? g_shadow_cast->half[0] * 1.35f : 2600.0f * e->lod.trees;
    float fade = radius * 0.82f, fade2 = fade * fade;
    /* How far the scrub reaches. Priced off the grass rung rather than the tree
     * one because that is what it costs like — a great many small things close
     * to the camera — and so that the budget rung, which gives up the blade
     * layer entirely, gives this up with it. */
    float brush = casting ? 0.0f : e->lod.grass * 0.55f;
    int ix0 = (int)floorf((ox - radius) / cell), ix1 = (int)ceilf((ox + radius) / cell);
    int iy0 = (int)floorf((oy - radius) / cell), iy1 = (int)ceilf((oy + radius) / cell);
    int ix, iy;
    for (iy = iy0; iy <= iy1; ++iy)
        for (ix = ix0; ix <= ix1; ++ix) {
            float cx = ((float)ix + 0.5f) * cell, cy = ((float)iy + 0.5f) * cell;
            float dx = cx - ox, dy = cy - oy;
            float d2 = dx * dx + dy * dy, dens, gz0, region, conif;
            uint32_t h1;
            int lod, n, j, verge;
            if (d2 > radius * radius) continue;
            /* Where the wood is, and how far the ground is down, in one call:
             * the cheap octaves reject most of the map before the heightfield
             * is sampled at all. */
            dens = fly_world_forest(w, cx, cy, &gz0);
            if (dens <= 0.0f) continue;
            /* The cell's own footprint plus the tallest thing that can stand on
             * it and the relief the ground has across 44 m. Cheap here and
             * worth a great deal: everything below this line is a heightfield
             * sample per tree and a scattering integral per vertex. The caster
             * has no frustum — see patch_visible. */
            if (!casting && !patch_visible(v, rt, cx, cy, gz0 + 14.0f, 74.0f)) continue;
            /* Is a road through this cell? One bit for the whole cell, and only
             * the handful that answer yes pay for a distance per tree — a wood
             * growing across the carriageway is the one thing that gives a road
             * away as painted on, and checking every trunk in a 2.6 km scatter
             * against two thousand spans would cost more than the wood does. */
            verge = fly_road_near(&g->roads, fly_wpos_mk(cx, cy));

            h1 = fly_hash2(w->seed + 71, ix, iy);
            lod = casting ? 1 : tree_lod(d2, e->lod.trees);
            /* How many trees a closed-canopy cell carries, by distance. Ten in
             * a 44 m cell is a tree every fourteen metres, which is woodland;
             * four was an orchard. Far cells get fewer and bigger instead,
             * because past a kilometre a stand is a texture and no one counts
             * the trunks — the triangle budget belongs to the ground you can
             * actually see. Stochastic, so a thinning stand loses trees one at
             * a time rather than every cell in it emptying at the same
             * threshold. */
            n = (int)(dens * (casting ? 16.0f
                                      : lod == 2 ? 16.0f : lod == 1 ? 11.0f : 6.0f) +
                      (float)(h1 & 255u) / 255.0f);
            region = grass_region(w->seed, cx, cy);
            for (j = 0; j < n; ++j) {
                uint32_t h = fly_hash2(h1 ^ 0x9E3779B9u, ix * 7 + j, iy * 13 - j);
                float px = cx + ((float)((h >> 3) & 63) / 63.0f - 0.5f) * cell;
                float py = cy + ((float)((h >> 9) & 63) / 63.0f - 0.5f) * cell;
                float gz = lod > 0 ? scatter_ground(w, px, py) : gz0;
                if (casting && d2 > radius * radius) continue;
                float s, tintr;
                int species;
                fly_v3 leaf;
                if (gz < FLY_WATER_Z + 0.5f) continue;
                /* nothing grows on the road or on its shoulder */
                if (verge && fly_road_gap(&g->roads, fly_wpos_mk(px, py)) <
                             FLY_ROAD_CLEAR) continue;
                /* Conifer takes over with altitude and cold; dry country runs
                   to standing deadwood. Species is per tree, not per stand, so
                   a wood has a mix in it the way a wood does. */
                conif = fly_clampf((gz - 120.0f) / 420.0f, 0.05f, 0.95f);
                species = (h & 31u) == 5u ? 2
                          : ((float)((h >> 5) & 255) / 255.0f < conif ? 0 : 1);
                if (region > 0.30f && (h & 7u) == 3u) species = 2;
                tintr = (float)((h >> 16) & 7) / 7.0f;
                s = species == 0 ? 6.5f + tintr * 9.0f
                    : species == 1 ? 5.5f + tintr * 6.5f
                                   : 4.0f + tintr * 5.0f;
                /* a distant clump stands in for the trees its cell no longer
                   draws, so a wood does not thin out as you climb away from it */
                if (lod == 0) s *= 1.45f;
                /* and the last of them shrink away rather than stopping at a
                   circle: the scatter radius used to be a visible ring on the
                   ground, wooded on one side and bare on the other, which is
                   the one edge in the landscape that nothing in the world put
                   there */
                if (d2 > fade2) s *= 1.0f - (sqrtf(d2) - fade) / (radius - fade);
                leaf = species == 1
                           ? fly_v3lerp(fly_v3mk(0.13f, 0.26f, 0.09f),
                                        fly_v3mk(0.24f, 0.25f, 0.07f), tintr)
                           : fly_v3lerp(fly_v3mk(0.06f, 0.17f, 0.10f),
                                        fly_v3mk(0.11f, 0.22f, 0.09f), tintr);
                if (gz > 600.0f)
                    leaf = fly_v3lerp(leaf, fly_v3mk(0.12f, 0.16f, 0.12f),
                                      fly_smoothstepf(600.0f, 990.0f, gz));
                draw_tree(rt, v, e, fly_v3mk(px, py, gz), s, species, leaf, h, lod);
                /* --- where it meets the ground ------------------------------
                 *
                 * The one thing a tree in this renderer never had. The sun
                 * shadow map handles the sun, and on the overcast afternoon
                 * most of the gallery is shot on there is barely a sun to cast
                 * — but a stem and a canopy occlude most of the sky from the
                 * few metres of ground at their foot whatever the weather is
                 * doing, and without that gradient a wood is a set of models
                 * standing on a lawn. Settlement props have had the ambient
                 * skirt since they were built; the scatter never did.
                 *
                 * Near field only, and hard: `prop_skirt` costs a heightfield
                 * gradient and a terrain shade at each of its fifteen
                 * stations, and there are thousands of trees. Forty metres is
                 * about where the ring itself is a couple of pixels wide, so
                 * nothing is lost by stopping there — and the skirt's own
                 * projected-size gate would refuse it a few metres later
                 * anyway. */
                if (lod == 2 && !casting && d2 < 40.0f * 40.0f && species != 2)
                    prop_skirt(rt, v, e, g, fly_v3mk(px, py, gz), s * 0.16f, s * 0.16f,
                               0.0f, s * 0.20f, 0.42f);
            }
            /* --- and the scrub between them ---------------------------------
             *
             * Thickest where the canopy breaks — a closed wood is dark
             * underneath and an open edge is where the light gets in — so the
             * count peaks in the middle of the density range rather than at
             * the top of it, which is also where a stand most needs something
             * at an intermediate scale. Near field only: see draw_bush. */
            if (!casting && lod == 2 && brush > 0.0f) {
                int nb = (int)(26.0f * dens * (1.35f - dens) + (float)((h1 >> 8) & 63u) / 63.0f);
                for (j = 0; j < nb; ++j) {
                    uint32_t h = fly_hash2(h1 ^ 0x85EBCA6Bu, ix * 11 + j, iy * 5 - j);
                    float px = cx + ((float)((h >> 3) & 63) / 63.0f - 0.5f) * cell;
                    float py = cy + ((float)((h >> 9) & 63) / 63.0f - 0.5f) * cell;
                    float gz, bs;
                    fly_v3 leaf;
                    if ((px - v->pos.x) * (px - v->pos.x) +
                        (py - v->pos.y) * (py - v->pos.y) > brush * brush) continue;
                    gz = fly_world_ground(w, px, py);
                    if (gz < FLY_WATER_Z + 0.5f) continue;
                    if (verge && fly_road_gap(&g->roads, fly_wpos_mk(px, py)) <
                                 FLY_ROAD_CLEAR) continue;
                    bs = 1.0f + (float)((h >> 20) & 7) * 0.24f;
                    /* Scrub follows the region the meadow does — gorse on the
                     * dry side, hazel on the lush one — so it belongs to the
                     * ground it is on rather than to the wood above it. */
                    leaf = region > 0.18f ? fly_v3mk(0.125f, 0.165f, 0.055f)
                                          : fly_v3mk(0.065f, 0.150f, 0.058f);
                    /* scrub in a wood stands in the wood's own shade */
                    leaf = fly_v3scale(leaf, 1.0f - 0.25f * dens);
                    draw_bush(rt, v, e, fly_v3mk(px, py, gz), bs, leaf, h);
                }
            }
        }
}

/* ---------------- GPU environment pass ----------------
 *
 * The sky, the four terrain LOD rings and the three water rings as real GL
 * geometry: one depth-tested pass, one readback. This is the GPU rasterizer.
 *
 * The split of work follows cost. Heights, normals, crevice AO and the water
 * surface model are per-vertex (a few thousand invocations); albedo, the sun
 * shadow lookup and the fog blend are per-fragment, which is *more* than the
 * CPU rasterizer does — it interpolates one Gouraud colour per vertex — so the
 * GPU frame is the better-looking of the two, not merely the faster one.
 *
 * The readback carries linear radiance in rgb and view depth in alpha, so the
 * CPU renderer keeps drawing scatter, settlements and craft on top with a
 * z-buffer that already contains the GPU's terrain. Every failure path returns
 * non-zero and leaves rt untouched, so a machine with no GL driver simply runs
 * the CPU environment below. */

#define FLY_GPU_RING_HALF 24                        /* max cells from centre */
#define FLY_GPU_RING_VN (FLY_GPU_RING_HALF * 2 + 1) /* vertices per side */

/* one grid vertex is just its world xy; the vertex shader lifts it onto the
 * heightfield, which keeps the per-frame upload tiny */
static const char *FLY_ENV_GRID_VS_HEAD =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec2 aXY;\n"
    FLY_GLSL_ENV_UNIFORMS;

static const char *FLY_ENV_SKY_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec2 aNDC;\n"
    /* pinned to the far plane so terrain and water always win the depth test */
    "void main(){ gl_Position = vec4(aNDC, 1.0, 1.0); }\n";

static const char *FLY_ENV_SKY_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_PROJECT
    "void main(){\n"
    "  vec3 rd = fly_pixel_ray(gl_FragCoord.xy);\n"
    /* alpha overflows fp16 to +inf: the background is infinitely far, exactly
     * what fly_rt_clear writes, so CPU geometry composites over it */
    "  oColor = vec4(fly_sky(uCamPos, rd), 1e30);\n"
    "}\n";

/* The traced occlusion window and the read of it. Shared, because everything
 * that stands on the ground takes it now and not only the ground: an object
 * shaded against the open hemisphere inside a closed stand is lit as though it
 * were in a field, and the window has always been indexed by ground position
 * so the read was there for the asking. Twin of ao_at in fly_render.c, height
 * blend and all. */
#define FLY_ENV_AO_UNIFORMS \
    "uniform sampler2D uAO;\n"  /* traced sky visibility on the world lattice */ \
    "uniform int uAOOn;\n" \
    "uniform vec3 uAOCfg;\n"    /* lattice metres, window side, fade radius */ \
    "uniform float uAOHi;\n"    /* the height the second channel was traced at */

#define FLY_ENV_AO \
    "float fly_ao_at(vec2 p, float agl, float d){\n" \
    "  if (uAOOn == 0) return 1.0;\n"  /* the vertex stage applied the scalar */ \
    "  float t = clamp(agl/uAOHi, 0.0, 1.0);\n" \
    /* outside the traced window this is the scalar the tracer replaces, not
       open sky — see ao_at, whose comment carries the measurement. It lifts
       with height for the same reason the traced pair does. */ \
    "  float far = 1.0 - 0.42*fly_forest_mask(uint(uSeed), p)*(1.0 - t);\n" \
    "  float k = 1.0 - fly_smoothstep(uAOCfg.z*0.70, uAOCfg.z, d);\n" \
    "  if (k <= 0.0) return far;\n" \
    "  int N = int(uAOCfg.y);\n" \
    "  vec2 f = p / uAOCfg.x;\n" \
    "  ivec2 tc = ivec2(floor(f));\n" \
    "  vec2 w = f - vec2(tc);\n" \
    /* texelFetch with an explicit wrap rather than the sampler's filter: the
       window is toroidal and the texture clamps, and the CPU takes exactly
       these four taps with exactly these weights. Two channels: red is what
       the ground sees, green what a canopy's height above it sees. */ \
    "  vec2 s00 = texelFetch(uAO, ivec2(tc.x & (N-1), tc.y & (N-1)), 0).rg;\n" \
    "  vec2 s10 = texelFetch(uAO, ivec2((tc.x+1) & (N-1), tc.y & (N-1)), 0).rg;\n" \
    "  vec2 s01 = texelFetch(uAO, ivec2(tc.x & (N-1), (tc.y+1) & (N-1)), 0).rg;\n" \
    "  vec2 s11 = texelFetch(uAO, ivec2((tc.x+1) & (N-1), (tc.y+1) & (N-1)), 0).rg;\n" \
    "  vec2 b = mix(mix(s00,s10,w.x), mix(s01,s11,w.x), w.y);\n" \
    "  return mix(far, mix(b.x, b.y, t), k);\n" \
    "}\n"

/* uniforms only the terrain programs need */
#define FLY_ENV_TERRAIN_UNIFORMS \
    "uniform vec3 uSmRight, uSmUp, uSmFwd;\n" \
    "uniform vec3 uSmUmin, uSmVmin, uSmTexel;\n" \
    "uniform vec3 uSmSize;\n" \
    "uniform ivec3 uSmPcf;\n" \
    "uniform int uSmActive;\n" \
    "uniform sampler2D uSmNear, uSmMid, uSmFar;\n" \
    FLY_GLSL_CANOPY_UNIFORMS \
    "uniform float uEdge;\n"    /* central-difference spacing, = step*0.4 */ \
    "uniform int uPerPixel;\n"  /* 1 = sample the shadow map in the fragment stage */ \
    FLY_ENV_AO_UNIFORMS

/* What is split by LOD is the *shadow lookup*, and only that.
 *
 * It has to be: the shadow map's terrain is sampled far more finely than a
 * 950 m ring's triangles, so a coarse ring that samples it per pixel cuts
 * through the depth map and bands with acne. The near rings, whose cells are
 * smaller than a map texel, take the crisp per-pixel path; the far rings
 * resolve one visibility per vertex and interpolate it, which is soft, and
 * mountain-scale occlusion is soft anyway.
 *
 * Colour is *not* split, and used to be. Resolving albedo at the vertices of a
 * 240 m ring point-samples noise whose finest octave is twelve metres across:
 * not merely blurred but biased, because the mix that turns it into a colour is
 * nonlinear, and the far rings came out a different colour from the near ones
 * rather than a smoother version of them. From altitude that drew a hard square
 * on the ground exactly where the ring changed — the single most visible
 * artifact in the renderer, and it had been read as "distant terrain is
 * plainer" rather than as a bug. The fragment stage now always shades; the
 * fine albedo octaves fade on projected size, so the far rings converge to the
 * mean of what they can no longer resolve instead of to an arbitrary sample of
 * it. */
static const char *FLY_ENV_TERRAIN_VS_BODY =
    FLY_ENV_TERRAIN_UNIFORMS
    /* centroid, not the default: with multisampling a partly covered pixel is
     * shaded at its *centre*, which for a sliver — a pipe seen edge-on, a pier,
     * a tower's corner — lies outside the triangle, and the varyings come back
     * extrapolated. That put negative radiance in the HDR buffer and depths of
     * nonsense in its alpha, and one camera at one resolution tonemapped to a
     * black frame. Centroid sampling moves the evaluation to a point actually
     * inside the covered area. */
    "centroid out vec3 vPos;\n"
    "centroid out vec3 vNrm;\n"
    "centroid out float vShadow;\n" /* per-vertex sun visibility (far rings) */
    "centroid out float vAO;\n"
    "centroid out float vCloud;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_SHADOW
    FLY_GLSL_CANOPY
    FLY_GLSL_PROJECT
    "void main(){\n"
    "  float e = uEdge;\n"
    "  float h = fly_ground(aXY);\n"
    "  float hx0 = fly_ground(aXY - vec2(e, 0.0)), hx1 = fly_ground(aXY + vec2(e, 0.0));\n"
    "  float hy0 = fly_ground(aXY - vec2(0.0, e)), hy1 = fly_ground(aXY + vec2(0.0, e));\n"
    "  vec3 n = normalize(vec3(-(hx1 - hx0)/(2.0*e), -(hy1 - hy0)/(2.0*e), 1.0));\n"
    /* concave cells see less sky: the same four taps pay for crevice AO */
    "  float conc = (hx0 + hx1 + hy0 + hy1)*0.25 - h;\n"
    "  vec3 p = vec3(aXY, h);\n"
    "  vNrm = n;\n"
    "  vPos = p;\n"
    /* twin of the canopy-occlusion term in the CPU ring build: a stand is a
       lid on the sky, and how much sky the floor sees is not the same question
       as what the floor is made of */
    /* Crevice occlusion only when the traced term is live: the canopy half is
       read per fragment off the lattice, because a vertex is 26 m from its
       neighbour and the trees vary every 15.5. Twin of terrain_ring. */ \
    "  vAO = 1.0 - clamp(conc/(e*0.8), 0.0, 0.4);\n"
    "  if (uAOOn == 0) vAO *= 1.0 - 0.42*fly_forest_mask(uint(uSeed), aXY);\n"
    /* the cloud deck is a 5 km feature: interpolating it across a cell is
     * invisible and saves an fbm per fragment */
    "  vCloud = fly_cloud_shadow(p);\n"
    "  vec3 dir = p - uCamPos;\n"
    "  float dist = length(dir);\n"
    /* aerial perspective tints toward the horizon sky; that colour changes
     * slowly across the frame, so it is interpolated rather than re-marched */
    "  vShadow = 1.0;\n"
    "  if (uPerPixel == 0) {\n"
    "    float nd = dot(n, uSun);\n"
    "    vShadow = fly_shadow_sample(fly_sm_offset(p, nd < 0.0 ? -n : n, abs(nd)), abs(nd));\n"
    "  }\n"
    "  gl_Position = fly_project(p);\n"
    "}\n";

static const char *FLY_ENV_TERRAIN_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    FLY_ENV_TERRAIN_UNIFORMS
    "centroid in vec3 vPos;\n"
    "centroid in vec3 vNrm;\n"
    "centroid in float vShadow;\n"
    "centroid in float vAO;\n"
    "centroid in float vCloud;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_SHADOW
    FLY_GLSL_CANOPY
    FLY_GLSL_PROJECT
    FLY_ENV_AO
    "void main(){\n"
    "  vec3 dir = vPos - uCamPos;\n"
    "  float dist = length(dir);\n"
    "  float px = uPxScale/dist;\n"
    "  vec3 gn = normalize(vNrm);\n"  /* geometric: slope, and the shadow offset */
    "  vec3 n = gn;\n"
    "  vec3 mat;\n"
    "  vec3 alb = fly_terrain_surface(vPos.xy, vPos.z, 1.0 - gn.z, px, mat);\n"
    "  alb *= fly_terrain_detail(vPos.xy, mat, px, n);\n"
    "  float s = vShadow;\n"
    "  if (uPerPixel != 0) {\n"
    /* the shadow map is built from the heightfield, so its normal offset takes
       the heightfield's normal — a tussock tilt would push the lookup off the
       surface the map actually holds */
    "    float nd = dot(gn, uSun);\n"
    "    s = fly_shadow_sample(fly_sm_offset(vPos, nd < 0.0 ? -gn : gn, abs(nd)), abs(nd));\n"
    "  }\n"
    "  vec3 col = fly_apply_fog(fly_terrain_light(alb, mat, n, -dir/dist, vCloud*s,\n"
    "                                             vAO*fly_ao_at(vPos.xy, 0.0, dist)),\n"
    "                           uCamPos, dir, dist);\n"
    "  oColor = vec4(col, dot(dir, uCamFwd));\n"
    "}\n";

/* Water shades per fragment, not per vertex.
 *
 * Its rings are as coarse as the terrain's (26 m at the finest), and the wave
 * normal drives fresnel and the sun glint — both sharply non-linear — so
 * interpolating one colour per vertex broke the surface into large soft
 * triangles wherever water filled the near field. Only the two genuinely
 * expensive lookups stay per vertex: the reflected sky (a cloud march) and the
 * cloud deck, neither of which turns appreciably across a cell. Reflecting
 * about the mean surface rather than the perturbed one costs a few degrees of
 * sky direction and saves marching it per pixel. */
static const char *FLY_ENV_WATER_VS_BODY =
    "centroid out vec3 vPos;\n"
    "centroid out vec3 vSky;\n"   /* sky reflected about the flat surface */
    "centroid out float vDepth;\n" /* metres of water; negative above the surface */
    "centroid out float vCloud;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_PROJECT
    "void main(){\n"
    /* The still water only: the sea, and a lake at its own spill point. A
       river is a line forty metres across between vertices sixty metres apart
       and would break into dashes on this lattice, so it is drawn as the line
       it is — see draw_rivers, which both pipelines share. */
    "  float gz = fly_ground(aXY);\n"
    "  float wz = fly_still(aXY, gz);\n"
    "  vec3 p = vec3(aXY, wz);\n"
    "  vDepth = wz - gz;\n"
    "  vPos = p;\n"
    "  vCloud = fly_cloud_shadow(p);\n"
    "  vec3 view = normalize(uCamPos - p);\n"
    "  vSky = fly_sky_amb(p, normalize(vec3(-view.x, -view.y, view.z*0.5 + 1.0)));\n"
    "  vec3 dir = p - uCamPos;\n"
    "  gl_Position = fly_project(p);\n"
    "}\n";

static const char *FLY_ENV_WATER_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "centroid in vec3 vPos;\n"
    "centroid in vec3 vSky;\n"
    "centroid in float vDepth;\n"
    "centroid in float vCloud;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_PROJECT
    /* per-pixel shoreline: the CPU rasterizer clips whole triangles against the
     * same waterline, so both renderers follow the contour rather than the grid */
    "void main(){\n"
    "  if (vDepth < -0.4) discard;\n"
    "  vec3 col = fly_water_surface(uCamPos, vPos.xy, vPos.z, vDepth, vSky, vCloud);\n"
    "  vec3 dir = vPos - uCamPos;\n"
    "  float dist = length(dir);\n"
    "  col = fly_apply_fog(col, uCamPos, dir, dist);\n"
    "  oColor = vec4(col, dot(dir, uCamFwd));\n"
    "}\n";

/* Settlements, craft, rail, ground items and near-field scatter: the same
 * triangles the CPU rasterizer would fill, handed over as one buffer. Vertex
 * colours arrive already shaded (Gouraud lit/occluded pair) but *unfogged*; the
 * fragment stage cross-fades them by the shadow map, which is what the CPU
 * rasterizer does per pixel, and then puts the air in front of them. ndotl < 0
 * marks flat geometry — decals, markings, grass blades — that takes neither a
 * shadow lookup nor aerial perspective.
 *
 * The air moved here from the vertex colours, and the two reasons are worth
 * separating.
 *
 * The one that shows in the frame: it is the same move the terrain and the
 * water already made. A scattering integral shared across a triangle is right
 * for a crown and wrong for a carriageway running away from the eye, and the
 * fragment stage does not have to choose — it evaluates the integral where the
 * pixel is. Applying it after the shadow cross-fade rather than before is not
 * an approximation of the old order, it is *identical* to it: fog is affine in
 * the colour, so mix(d*T+S, l*T+S, s) and mix(d,l,s)*T + S are the same number.
 *
 * The one that shows in the budget: it takes the single largest item out of the
 * CPU's object build. Over closed canopy at 1280x720 the scattering integral
 * was 32 ms of a 124 ms build — a quarter of it, and more than roads,
 * settlements, rail, grass and boulders together — because a wood is tens of
 * thousands of small triangles and each one was paying for four Chapman columns
 * and a cloud tap.
 *
 * What it does *not* do is make the captured colour camera-independent, which
 * is what a geometry cache across frames would need. `tri_leaf` shades through
 * the canopy BRDF, whose transmission and down-sun surge are functions of the
 * view direction, and `tri_lit_n`'s specular lobe is too. Both would have to
 * move into GLSL before a chunk of wood could be built once and drawn twice. */
static const char *FLY_ENV_OBJ_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aLit;\n"
    "layout(location = 2) in vec3 aDark;\n"
    "layout(location = 3) in vec4 aNdl;\n"
    "layout(location = 4) in float aAgl;\n"
    FLY_GLSL_ENV_UNIFORMS
    "centroid out vec3 vPos;\n"
    "centroid out vec3 vLit;\n"
    "centroid out vec3 vDark;\n"
    "centroid out vec3 vNrm;\n"
    "centroid out float vNdl;\n"
    "centroid out float vAgl;\n"
    FLY_GLSL_PROJECT
    "void main(){\n"
    "  vPos = aPos; vLit = aLit; vDark = aDark; vNrm = aNdl.xyz; vNdl = aNdl.w;\n"
    "  vAgl = aAgl;\n"
    "  gl_Position = fly_project(aPos);\n"
    "}\n";

static const char *FLY_ENV_OBJ_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "uniform vec3 uSmRight, uSmUp, uSmFwd;\n"
    "uniform vec3 uSmUmin, uSmVmin, uSmTexel;\n"
    "uniform vec3 uSmSize;\n"
    "uniform ivec3 uSmPcf;\n"
    "uniform int uSmActive;\n"
    "uniform sampler2D uSmNear, uSmMid, uSmFar;\n"
    FLY_GLSL_CANOPY_UNIFORMS
    FLY_ENV_AO_UNIFORMS
    "centroid in vec3 vPos;\n"
    "centroid in vec3 vLit;\n"
    "centroid in vec3 vDark;\n"
    "centroid in vec3 vNrm;\n"
    "centroid in float vNdl;\n"
    "centroid in float vAgl;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_SHADOW
    FLY_GLSL_CANOPY
    FLY_GLSL_PROJECT
    FLY_ENV_AO
    "void main(){\n"
    "  vec3 dir = vPos - uCamPos;\n"
    "  float dist = length(dir);\n"
    "  vec3 col;\n"
    "  if (vNdl >= 0.0) {\n"
    "    float s = fly_shadow_sample(fly_sm_offset(vPos, normalize(vNrm), vNdl), vNdl);\n"
    /* vDark is the ambient half and vLit - vDark the direct half, so the
     * traced occlusion scales exactly the term it belongs to and leaves the
     * sun alone — the same split raster_tri_shadowed_raw makes, read at this
     * primitive's own height off the ground rather than at the ground's. */
    "    float ao = fly_ao_at(vPos.xy, vAgl, dist);\n"
    "    col = fly_apply_fog(vDark*ao + (vLit - vDark)*s, uCamPos, dir, dist);\n"
    "  } else {\n"
    "    col = vLit;\n"  /* flat geometry: one colour, no shadow and no air */
    "  }\n"
    "  oColor = vec4(col, dot(dir, uCamFwd));\n"
    "}\n";

/* ---- the wood ----
 *
 * The template mesh in tree units, one instance per tree. The vertex stage
 * turns the shape about its own axis, scales it, stands it on the ground and
 * works out the crown normal; the fragment stage runs the canopy BRDF, the
 * cascade lookup and the air. Nothing about a tree crosses the bus but the ten
 * floats that say which tree it is.
 *
 * The normal is `tri_leaf`'s, exactly: out from the shading hub, lifted toward
 * the sky, because a crown's outer surface faces up as well as out and a purely
 * radial normal leaves the underside lit from below. It is computed after the
 * rotation and the scale, both of which preserve it — a uniform scale drops out
 * of a normalize and a turn about z is orthonormal — so this is the same vector
 * the reference builds, not an approximation of it.
 *
 * The cascade wants the *geometric* normal instead, which a vertex stage cannot
 * know: it is a property of the triangle, not the corner. The fragment stage
 * can, off the derivatives of the position it is interpolating, which is where
 * `raster_tri_shadowed`'s cross product ends up anyway. */
static const char *FLY_ENV_WOOD_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec3 aPos;\n"    /* template, in tree units */
    "layout(location = 1) in vec3 aHub;\n"
    "layout(location = 2) in vec2 aShade;\n"  /* albedo multiplier, occlusion */
    "layout(location = 3) in vec4 aBase;\n"   /* instance: xyz base, w size */
    "layout(location = 4) in vec3 aLeaf;\n"
    "layout(location = 5) in vec3 aTurn;\n"   /* cos, sin, and the agl datum */
    FLY_GLSL_ENV_UNIFORMS
    "centroid out vec3 vPos;\n"
    "centroid out vec3 vNrm;\n"
    "centroid out vec3 vAlb;\n"
    "centroid out float vAO;\n"
    "centroid out float vAgl;\n"
    FLY_GLSL_PROJECT
    "vec3 fly_wood_place(vec3 u, vec4 base, vec3 turn){\n"
    "  vec3 r = vec3(u.x*turn.x - u.y*turn.y, u.x*turn.y + u.y*turn.x, u.z);\n"
    "  return base.xyz + r*base.w;\n"
    "}\n"
    "void main(){\n"
    "  vec3 p = fly_wood_place(aPos, aBase, aTurn);\n"
    "  vec3 hub = fly_wood_place(aHub, aBase, aTurn);\n"
    "  vec3 d = p - hub;\n"
    "  vec3 n = dot(d, d) > 1e-6 ? normalize(d) : vec3(0.0, 0.0, 1.0);\n"
    "  vPos = p;\n"
    "  vNrm = normalize(n + vec3(0.0, 0.0, 0.35));\n"
    "  vAlb = aLeaf * aShade.x;\n"
    "  vAO = aShade.y;\n"
    "  vAgl = aTurn.z + (p.z - aBase.z);\n"
    "  gl_Position = fly_project(p);\n"
    "}\n";

static const char *FLY_ENV_WOOD_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "uniform vec3 uSmRight, uSmUp, uSmFwd;\n"
    "uniform vec3 uSmUmin, uSmVmin, uSmTexel;\n"
    "uniform vec3 uSmSize;\n"
    "uniform ivec3 uSmPcf;\n"
    "uniform int uSmActive;\n"
    "uniform sampler2D uSmNear, uSmMid, uSmFar;\n"
    FLY_GLSL_CANOPY_UNIFORMS
    FLY_ENV_AO_UNIFORMS
    "centroid in vec3 vPos;\n"
    "centroid in vec3 vNrm;\n"
    "centroid in vec3 vAlb;\n"
    "centroid in float vAO;\n"
    "centroid in float vAgl;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_SHADOW
    FLY_GLSL_CANOPY
    FLY_GLSL_PROJECT
    FLY_ENV_AO
    "void main(){\n"
    /* (vegetation, f0, blinn exponent): leaves are waxy, so a little sheen.
       tri_leaf's `leaf_mat`, and it has to stay its twin. */
    "  const vec3 leaf_mat = vec3(1.0, 0.022, 12.0);\n"
    "  vec3 dir = vPos - uCamPos;\n"
    "  float dist = length(dir);\n"
    "  vec3 n = normalize(vNrm);\n"
    "  float cloud = fly_cloud_shadow(vPos);\n"
    "  vec3 lit  = fly_terrain_light(vAlb, leaf_mat, n, -dir/dist, cloud, vAO);\n"
    "  vec3 dark = fly_lit_surface(vAlb, n, 0.0, vAO);\n"
    "  float s = 1.0;\n"
    "  if (uSmActive != 0) {\n"
    /* the triangle's own normal, oriented toward the sun, exactly as the
       capture path orients its cross product before offsetting the lookup */
    "    vec3 gn = normalize(cross(dFdx(vPos), dFdy(vPos)));\n"
    "    float nd = dot(gn, uSun);\n"
    "    if (nd < 0.0) { gn = -gn; nd = -nd; }\n"
    "    s = fly_shadow_sample(fly_sm_offset(vPos, gn, nd), nd);\n"
    "  }\n"
    /* the traced occlusion scales the ambient half and leaves the sun alone —
       the split raster_tri_shadowed_raw makes, read at the crown's own height */
    "  float ao = fly_ao_at(vPos.xy, vAgl, dist);\n"
    "  vec3 col = fly_apply_fog(dark*ao + (lit - dark)*s, uCamPos, dir, dist);\n"
    "  oColor = vec4(col, dot(dir, uCamFwd));\n"
    "}\n";

/* ---- shadow-cascade build programs ----
 *
 * Two programs write one cascade: the terrain heightfield off a flat grid whose
 * height the vertex stage evaluates, and the scene's captured occluder
 * triangles. Both emit the same thing — light-space distance in red — so the
 * cascade a beauty-pass shader samples is byte-for-byte the layout the CPU
 * builder produced, and FLY_GLSL_SHADOW reads it unchanged. */
static const char *FLY_SM_CAST_TERRAIN_VS =
    FLY_GLSL_HEADER
    /* the grid arrives as integer cell indices and is therefore *static*: the
     * cascade's origin and spacing move every frame, but only as uniforms */
    "layout(location = 0) in vec2 aIJ;\n"
    "uniform int uSeed;\n"
    "uniform int uLocCount;\n"
    "uniform vec4 uLoc[28];\n"
    "uniform vec3 uCastGrid;\n" /* xy = corner, z = spacing */
    FLY_GLSL_CAST
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    "out float vD;\n"
    "void main(){\n"
    "  vec2 xy = uCastGrid.xy + aIJ*uCastGrid.z;\n"
    "  gl_Position = fly_cast_project(vec3(xy, fly_ground(xy)), vD);\n"
    "}\n";

static const char *FLY_SM_CAST_OBJ_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec3 aPos;\n"
    FLY_GLSL_CAST
    "out float vD;\n"
    "void main(){ gl_Position = fly_cast_project(aPos, vD); }\n";

static const char *FLY_SM_CAST_FS =
    FLY_GLSL_HEADER
    "in float vD;\n"
    "out vec4 oColor;\n"
    "void main(){ oColor = vec4(vD, 0.0, 0.0, 1.0); }\n";

/* everything that is not sky, terrain or water; shared by both pipelines */
/* The graphics level rides in `e->lod`, not in an argument: there is one
 * level a frame and every one of the dozen things below reads it, so passing
 * it down by hand is a dozen chances for one of them to be handed a different
 * one. */
static void draw_scene_objects(fly_render_target *rt, const fly_game *g, const fly__env *e,
                               const fly__view *v);
/* additive emissive splat, replayed once the frame's z-buffer exists */
static void light_splat(fly_render_target *rt, float sx, float sy, float z,
                        uint32_t col, float size);

/* the two grid programs share a vertex-shader prologue */
static char *env_vs_join(const char *body) {
    size_t a = strlen(FLY_ENV_GRID_VS_HEAD), b = strlen(body);
    char *s = (char *)malloc(a + b + 1);
    if (!s) return NULL;
    memcpy(s, FLY_ENV_GRID_VS_HEAD, a);
    memcpy(s + a, body, b + 1);
    return s;
}

/* World-space reach of a pad apron. Beyond it fly_ground is the raw
 * heightfield, which is what makes the cull below exact rather than an
 * approximation. It was this number written out a second time; it is
 * fly_world.h's own now, so the cull cannot quietly disagree with the grading
 * it is culling against. */
#define FLY_PAD_REACH FLY_PAD_BLEND_R

/* The shader's cut array and the world's are the same array; fly_glsl.h cannot
 * include fly_world.h to say so, so it is said here, where both are visible. */
typedef char fly__cut_cap_agrees[FLY_GLSL_MAX_CUT == FLY_CUT_MAX ? 1 : -1];
typedef char fly__river_cap_agrees[FLY_GLSL_MAX_RIVER == FLY_RIVER_MAX &&
                                   FLY_GLSL_MAX_RIVER_PT == FLY_RIVER_PT_MAX &&
                                   FLY_GLSL_MAX_LAKE == FLY_LAKE_MAX ? 1 : -1];

/* Upload the world data fly_ground walks — the pad aprons and the cuttings —
 * restricted to what a draw can actually reach.
 *
 * fly_ground walks uLoc once per invocation, and a vertex-stage loop over 28
 * sites is not free: on this host the aprons alone were 4.2 ms of the 15.3 ms
 * shadow-cascade pass, and the path tracer pays the loop again at every march
 * step. A site only bends the heightfield within FLY_PAD_REACH of its centre,
 * so restricting the array to the sites within `radius + locreach` of
 * (cx, cy) leaves every sampled height bit-identical while typically cutting
 * the loop to nothing. Pass radius < 0 for "the whole world".
 *
 * `locreach` is how far a site has to be able to reach to matter to the shader
 * being fed, and it belongs to the caller because the answer is not the same
 * for all of them: fly_ground stops at FLY_PAD_REACH, a shader that also calls
 * fly_canopy carries the settlement clearing, which reaches
 * FLY_CLEARING_REACH — five times as far — and one that shades the ground or
 * plants a wood carries the worked belt as well, which reaches
 * FLY_TILTH_REACH, five times further again. Culling to the pad's reach in
 * either of those hands the shader a site list with the clearing missing, and
 * it grows a wood across the airfield or ploughs the ground round the wrong
 * settlement. Widening it never moves a height: the pad loop's own guard skips
 * whatever the cull would have.
 *
 * `radius` must reach the *corner* of a square footprint, not its edge — a
 * grid that spans +-H reaches H*sqrt(2) diagonally, and culling to H quietly
 * drops a site whose apron the corner still touches. That showed up as the GPU
 * and CPU shadow cascades disagreeing on 12000 texels: same heightfield, one
 * of them missing a pad it should have been levelling.
 *
 * The cuttings are culled the same way and for the same reason. Their reach is
 * their own: a box `half` long and `wide` across, feathered on both, so the
 * circle that holds it is the diagonal of the feathered box. Dropping a cut
 * whose box the draw does not touch is exact — outside it the loop's own guard
 * would have skipped it anyway. */
/* The water on the land, uploaded whole.
 *
 * Not culled, where the aprons and the cuttings are, and the reason is the
 * shape of the thing rather than an oversight: a river is one polyline whose
 * stations are indexed by the river that owns them, so dropping the ones a
 * draw cannot reach would mean renumbering the rest — and there are sixty-four
 * stations in a world, against the twenty-eight sites and twenty-four boxes
 * that *are* culled because they are independent of each other. What does the
 * culling here is the per-river box the shader tests first: four comparisons
 * reject every station of every river for any point that is not near water,
 * which is very nearly every point of very nearly every frame.
 *
 * Left at their defaults these draw a planet with the valleys still in it and
 * no water in them, which `gpu.ground` would see as a five-metre disagreement
 * about the height of the ground. */
static void env_water_uniforms(fly_gpu_prog *p, const fly_game *g) {
    float river[FLY_GLSL_MAX_RIVER * 4], box[FLY_GLSL_MAX_RIVER * 4];
    float pt[FLY_GLSL_MAX_RIVER_PT * 4], lake[FLY_GLSL_MAX_LAKE * 4];
    const fly_world *w = &g->world;
    int i, nr = w->nriver < FLY_GLSL_MAX_RIVER ? w->nriver : FLY_GLSL_MAX_RIVER;
    int npt = w->nriver_pt < FLY_GLSL_MAX_RIVER_PT ? w->nriver_pt : FLY_GLSL_MAX_RIVER_PT;
    int nl = w->nlake < FLY_GLSL_MAX_LAKE ? w->nlake : FLY_GLSL_MAX_LAKE;
    memset(river, 0, sizeof river);
    memset(box, 0, sizeof box);
    memset(pt, 0, sizeof pt);
    memset(lake, 0, sizeof lake);
    for (i = 0; i < nr; ++i) {
        const fly_river *rv = &w->river[i];
        river[i * 4 + 0] = (float)rv->first;
        river[i * 4 + 1] = (float)rv->count;
        river[i * 4 + 2] = rv->bed;
        river[i * 4 + 3] = rv->batter;
        box[i * 4 + 0] = rv->x0;  box[i * 4 + 1] = rv->y0;
        box[i * 4 + 2] = rv->x1;  box[i * 4 + 3] = rv->y1;
    }
    for (i = 0; i < npt; ++i) {
        pt[i * 4 + 0] = w->river_pt[i].x;
        pt[i * 4 + 1] = w->river_pt[i].y;
        pt[i * 4 + 2] = w->river_pt[i].z;
        pt[i * 4 + 3] = w->river_pt[i].wide;
    }
    for (i = 0; i < nl; ++i) {
        lake[i * 4 + 0] = w->lake[i].x;
        lake[i * 4 + 1] = w->lake[i].y;
        lake[i * 4 + 2] = w->lake[i].r;
        lake[i * 4 + 3] = w->lake[i].z;
    }
    fly_gpu_set1i(p, "uRiverCount", nr);
    fly_gpu_set4fv(p, "uRiver", river, FLY_GLSL_MAX_RIVER);
    fly_gpu_set4fv(p, "uRiverBox", box, FLY_GLSL_MAX_RIVER);
    fly_gpu_set4fv(p, "uRiverPt", pt, FLY_GLSL_MAX_RIVER_PT);
    fly_gpu_set1i(p, "uLakeCount", nl);
    fly_gpu_set4fv(p, "uLake", lake, FLY_GLSL_MAX_LAKE);
}

static void env_ground_uniforms(fly_gpu_prog *p, const fly_game *g,
                                float cx, float cy, float radius, float locreach) {
    float loc[FLY_GLSL_MAX_LOC * 4];
    float cut[FLY_GLSL_MAX_CUT * 12];
    int i, n = 0, nc = 0, cap = g->world.nloc < FLY_GLSL_MAX_LOC ? g->world.nloc : FLY_GLSL_MAX_LOC;
    int ccap = g->world.ncut < FLY_GLSL_MAX_CUT ? g->world.ncut : FLY_GLSL_MAX_CUT;
    float reach = radius + locreach;
    memset(loc, 0, sizeof loc);
    memset(cut, 0, sizeof cut);
    for (i = 0; i < cap; ++i) {
        float dx = g->world.loc[i].pos.e - cx, dy = g->world.loc[i].pos.n - cy;
        if (radius >= 0.0f && (fabsf(dx) > reach || fabsf(dy) > reach)) continue;
        loc[n * 4 + 0] = g->world.loc[i].pos.e;
        loc[n * 4 + 1] = g->world.loc[i].pos.n;
        loc[n * 4 + 2] = g->world.loc[i].pad_z;
        /* fly_canopy's clearing, in the component fly_ground has no use for.
         * Never zero — it divides by it — and it is this site's own kind's
         * radius, so both sides thin the same wood by the same amount. */
        loc[n * 4 + 3] = fly_world_clearing(g->world.loc[i].kind);
        ++n;
    }
    for (i = 0; i < ccap; ++i) {
        const fly_cut *c = &g->world.cut[i];
        float span = hypotf(c->half + c->feather, c->wide + c->feather);
        float dx = c->x - cx, dy = c->y - cy;
        float *row = &cut[nc * 12];
        if (radius >= 0.0f && (fabsf(dx) > radius + span || fabsf(dy) > radius + span)) continue;
        row[0] = c->x;  row[1] = c->y;    row[2] = c->dx;      row[3] = c->dy;
        row[4] = c->half; row[5] = c->wide; row[6] = c->feather; row[7] = c->z0;
        row[8] = c->z1;
        ++nc;
    }
    fly_gpu_set1i(p, "uLocCount", n);
    fly_gpu_set4fv(p, "uLoc", loc, FLY_GLSL_MAX_LOC);
    fly_gpu_set1i(p, "uCutCount", nc);
    fly_gpu_set4fv(p, "uCut", cut, FLY_GLSL_MAX_CUT * 3);
    env_water_uniforms(p, g);
}

static void env_uniforms(fly_gpu_prog *p, const fly_game *g, const fly__env *e,
                         const fly__view *v, int w, int h) {
    fly_gpu_set1i(p, "uSeed", (int)g->world.seed);
    fly_gpu_set4f(p, "uChartFrame", g->world.frame.ux, g->world.frame.uy,
                     g->world.frame.sa, g->world.frame.ca);
    env_ground_uniforms(p, g, 0.0f, 0.0f, -1.0f, FLY_PAD_REACH);
    fly_gpu_set3f(p, "uSun", e->sun.x, e->sun.y, e->sun.z);
    fly_gpu_set3f(p, "uWind", e->wind.x, e->wind.y, e->wind.z);
    fly_gpu_set1f(p, "uDay", e->day);
    fly_gpu_set1f(p, "uDusk", e->dusk);
    fly_gpu_set1f(p, "uStorm", e->storm);
    fly_gpu_set1f(p, "uWet", e->wet);
    fly_gpu_set1f(p, "uCloudMean", e->cloud_mean);
    fly_gpu_set1f(p, "uCirrusMean", e->cirrus_mean);
    fly_gpu_set3f(p, "uCirrusLit", e->cirrus_lit.x, e->cirrus_lit.y, e->cirrus_lit.z);
    fly_gpu_set2f(p, "uCirrusDir", e->cirrus_dir.x, e->cirrus_dir.y);
    fly_gpu_set3f(p, "uCloudLit", e->cloud_lit.x, e->cloud_lit.y, e->cloud_lit.z);
    fly_gpu_set3f(p, "uCloudDark", e->cloud_dark.x, e->cloud_dark.y, e->cloud_dark.z);
    fly_gpu_set3f(p, "uSunLight", e->sunlight.x, e->sunlight.y, e->sunlight.z);
    { float sh[27]; int k; for (k = 0; k < 9; ++k) {
          sh[k * 3 + 0] = e->sh[k].x; sh[k * 3 + 1] = e->sh[k].y;
          sh[k * 3 + 2] = e->sh[k].z; }
      fly_gpu_set3fv(p, "uSH", sh, 9); }
    fly_gpu_set3f(p, "uGround", e->ground_lit.x, e->ground_lit.y, e->ground_lit.z);
    fly_gpu_set1f(p, "uTime", (float)e->time);
    fly_gpu_set3f(p, "uCamPos", v->pos.x, v->pos.y, v->pos.z);
    fly_gpu_set3f(p, "uCamFwd", v->fwd.x, v->fwd.y, v->fwd.z);
    fly_gpu_set3f(p, "uCamRight", v->right.x, v->right.y, v->right.z);
    fly_gpu_set3f(p, "uCamUp", v->up.x, v->up.y, v->up.z);
    fly_gpu_set2f(p, "uRes", (float)w, (float)h);
    fly_gpu_set2f(p, "uClip", v->ortho ? 0.001f : 0.35f, 40000.0f);
    fly_gpu_set1f(p, "uScale", v->sy);
    /* output pixels per unit: what the detail LOD fades on (see fly__env) */
    fly_gpu_set1f(p, "uPxScale", v->sy / (float)(v->aa > 0 ? v->aa : 1));
    /* and how far the chart has given way to the ball — see chart_gives_way */
    fly_gpu_set1f(p, "uBall", e->ball);
    fly_gpu_set1i(p, "uOrtho", v->ortho);
}

/* Build one LOD ring's grid: a snapped square of world xy, indexed skipping the
 * cells the next finer ring already covers (the CPU rings cull identically, so
 * the two renderers draw the same silhouette). */
static int env_ring_mesh(fly_gpu_mesh *m, const fly__view *v, float step, int half,
                         float inner) {
    static float verts[FLY_GPU_RING_VN * FLY_GPU_RING_VN * 2];
    static uint32_t idx[(FLY_GPU_RING_VN - 1) * (FLY_GPU_RING_VN - 1) * 6];
    const int attr[1] = { 2 };
    int n, vn, i, j, ni = 0;
    float cx0, cy0;
    if (half > FLY_GPU_RING_HALF) half = FLY_GPU_RING_HALF;
    n = half * 2;
    vn = n + 1;
    cx0 = floorf(v->pos.x / step) * step - (float)half * step;
    cy0 = floorf(v->pos.y / step) * step - (float)half * step;
    for (j = 0; j < vn; ++j)
        for (i = 0; i < vn; ++i) {
            verts[(j * vn + i) * 2 + 0] = cx0 + (float)i * step;
            verts[(j * vn + i) * 2 + 1] = cy0 + (float)j * step;
        }
    for (j = 0; j < n; ++j)
        for (i = 0; i < n; ++i) {
            float mx = cx0 + ((float)i + 0.5f) * step - v->pos.x;
            float my = cy0 + ((float)j + 0.5f) * step - v->pos.y;
            uint32_t a;
            /* Cull by the cell's outer edge, not its centre. A ring's guaranteed
             * coverage is step*(half-1) — its origin snaps to the step grid, so up
             * to one cell of the nominal extent can fall on the far side. Culling
             * by centre therefore drops coarse cells reaching half a cell past what
             * the finer ring covers, and the sky shows through the seam as a hard
             * line around every LOD boundary. */
            if (inner > 0.0f && fabsf(mx) + step * 0.5f < inner &&
                fabsf(my) + step * 0.5f < inner) continue;
            a = (uint32_t)(j * vn + i);
            idx[ni++] = a;
            idx[ni++] = a + 1;
            idx[ni++] = a + (uint32_t)vn + 1;
            idx[ni++] = a;
            idx[ni++] = a + (uint32_t)vn + 1;
            idx[ni++] = a + (uint32_t)vn;
        }
    if (ni == 0) return -1;
    return fly_gpu_mesh_upload(m, verts, vn * vn, 2, attr, 1, idx, ni);
}

/* Upload this frame's cascades once. Several programs sample them — terrain and
 * objects — and the texture is megabytes, so only the sampler bindings are
 * per-program; the pixels go up a single time. */
static fly_gpu_img *g_sm_img[FLY_SM_CASCADES];
static fly_gpu_img *g_ao_img;
/* Is this frame's radiance still in GL memory rather than in rt->hdr? Set only
 * by the pure raster path, which is the one that can finish a frame without
 * the CPU touching it; every stage from the volumetric correction to the
 * resolve asks this to know where to read. */
static int g_frame_resident;
/* the 1x1 image gpu_exposure leaves this frame's exposure in, or NULL */
static fly_gpu_img *g_exposure_img;

static void env_shadow_upload(const fly__env *e) {
    static const float far_away = 1e30f;
    const fly__shadow *sm = e->shadow;
    int c;
    /* a GPU-built map is already where the samplers want it — the cascades were
     * rendered into these very images and never came back to the CPU */
    if (sm && sm->active && sm->gpu) return;
    for (c = 0; c < FLY_SM_CASCADES; ++c) {
        if (!g_sm_img[c]) g_sm_img[c] = fly_gpu_img_create();
        if (!g_sm_img[c]) continue;
        if (sm && sm->active) fly_gpu_img_set(g_sm_img[c], sm->size[c], sm->size[c], sm->depth[c], 1);
        else fly_gpu_img_set(g_sm_img[c], 1, 1, &far_away, 1); /* keep it complete */
    }
}

/* light-space frame one cascade is being rendered in (see FLY_GLSL_CAST) */
static void cast_uniforms(fly_gpu_prog *p, const fly__shadow *sm, int c) {
    fly_gpu_set3f(p, "uCastRight", sm->right.x, sm->right.y, sm->right.z);
    fly_gpu_set3f(p, "uCastUp", sm->up.x, sm->up.y, sm->up.z);
    fly_gpu_set3f(p, "uCastFwd", sm->fwd.x, sm->fwd.y, sm->fwd.z);
    fly_gpu_set2f(p, "uCastOrigin", sm->umin[c], sm->vmin[c]);
    fly_gpu_set2f(p, "uCastRange", sm->dmin[c], sm->dmax[c]);
    fly_gpu_set1f(p, "uCastTexel", sm->texel[c]);
    fly_gpu_set1f(p, "uCastSize", (float)sm->size[c]);
}

/* point one program at the cascades already uploaded */
/* The occlusion window, for whichever program is about to read it. One place,
 * because the terrain and the objects now take the same numbers and a second
 * copy of the binding is a second thing to forget. */
static void env_ao_uniforms(fly_gpu_prog *p, int on) {
    if (on && fly_gpu_img_bind(p, "uAO", 3, g_ao_img) != 0) on = 0;
    fly_gpu_set1i(p, "uAOOn", on);
    fly_gpu_set3f(p, "uAOCfg", FLY_AO_LAT, (float)FLY_AO_N, FLY_AO_FILL);
    fly_gpu_set1f(p, "uAOHi", FLY_AO_HI);
}

static void env_shadow_uniforms(fly_gpu_prog *p, const fly__env *e) {
    const fly__shadow *sm = e->shadow;
    if (g_sm_img[0]) fly_gpu_img_bind(p, "uSmNear", 0, g_sm_img[0]);
    if (g_sm_img[1]) fly_gpu_img_bind(p, "uSmMid", 1, g_sm_img[1]);
    if (g_sm_img[2]) fly_gpu_img_bind(p, "uSmFar", 2, g_sm_img[2]);
    if (!sm || !sm->active) {
        fly_gpu_set1i(p, "uSmActive", 0);
        return;
    }
    fly_gpu_set1i(p, "uSmActive", 1);
    fly_gpu_set3i(p, "uSmPcf", sm->pcf[0], sm->pcf[1], sm->pcf[2]);
    fly_gpu_set3f(p, "uSmSize", (float)sm->size[0], (float)sm->size[1], (float)sm->size[2]);
    fly_gpu_set3f(p, "uSmRight", sm->right.x, sm->right.y, sm->right.z);
    fly_gpu_set3f(p, "uSmUp", sm->up.x, sm->up.y, sm->up.z);
    fly_gpu_set3f(p, "uSmFwd", sm->fwd.x, sm->fwd.y, sm->fwd.z);
    fly_gpu_set3f(p, "uSmUmin", sm->umin[0], sm->umin[1], sm->umin[2]);
    fly_gpu_set3f(p, "uSmVmin", sm->vmin[0], sm->vmin[1], sm->vmin[2]);
    fly_gpu_set3f(p, "uSmTexel", sm->texel[0], sm->texel[1], sm->texel[2]);
    /* what the wood does where the crowns are not cast — see canopy_sun */
    fly_gpu_set4f(p, "uCanopy", sm->center[0].x, sm->center[0].y,
                  sm->half[0] * 1.35f, FLY_CANOPY_H);
    fly_gpu_set3f(p, "uCanopyK", FLY_CANOPY_K, FLY_CANOPY_TOP, FLY_CANOPY_OFFSET);
}

/* Bring the published frame down into the render target's radiance and depth
 * planes. It is RGBA16F with view depth in alpha, and GL's rows run bottom-up,
 * so this indexes them in reverse rather than paying a separate flipping pass
 * over the whole buffer. */
static int frame_to_target(fly_render_target *rt) {
    static float *buf;
    static int buf_w, buf_h;
    int i, x, w = rt->w, h = rt->h;
    if (buf_w != w || buf_h != h) {
        float *nb = (float *)realloc(buf, (size_t)w * (size_t)h * 4 * sizeof(float));
        if (!nb) return -1;
        buf = nb;
        buf_w = w;
        buf_h = h;
    }
    if (fly_gpu_frame_read(buf, w, h) != 0) return -1;
    for (i = 0; i < h; ++i) {
        const float *row = &buf[(size_t)(h - 1 - i) * (size_t)w * 4];
        fly_v3 *dst = &rt->hdr[(size_t)i * (size_t)w];
        float *dz = &rt->depth[(size_t)i * (size_t)w];
        for (x = 0; x < w; ++x) {
            dst[x] = fly_v3mk(row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]);
            dz[x] = row[x * 4 + 3];
        }
    }
    return 0;
}

/* Returns 0 when the frame's sky, terrain, water and (with `with_objects`) the
 * scene's objects are drawn. `resident` keeps the result in GL memory instead
 * of reading it into rt->hdr and rt->depth — see the resident-frame notes in
 * fly_gpu.h for why that is the difference between a frame that fits in 16 ms
 * and one that cannot. */
/* ---- the wood's templates and its draw ----
 *
 * Eight shapes per species and level of detail, baked once from the model at
 * unit size with a white leaf colour. A template is a handful of kilobytes and
 * never changes, so it is uploaded on first use and then only the instance list
 * moves — which is the entire argument for doing this.
 *
 * The hashes are arbitrary and fixed: any eight distinct ones give eight
 * distinct trees, and pinning them is what makes a frame reproducible. */
static fly_gpu_mesh *g_wood_mesh[3][3][FLY_WOOD_SHAPES];
static int g_wood_nvert[3][3][FLY_WOOD_SHAPES];
static int g_wood_baked;
/* what the last frame's crowns came to, so `render.cost` can say what moved */
static long g_wood_verts;

static int wood_bake(void) {
    static fly__woodbuf tmpl;
    static uint32_t *idx;
    static int idx_cap;
    int sp, lo, k;
    if (g_wood_baked) return g_wood_baked > 0 ? 0 : -1;
    g_wood_baked = -1;
    for (sp = 0; sp < 2; ++sp)
        for (lo = 0; lo < 3; ++lo)
            for (k = 0; k < FLY_WOOD_SHAPES; ++k) {
                const int attr[3] = { 3, 3, 2 };
                uint32_t h = fly_hash2(0x5EED37u + (uint32_t)k,
                                       sp * 31 + lo, k * 7 + 3);
                int i, nv;
                tmpl.nvert = 0;
                tmpl.overflow = 0;
                g_wood_tmpl = &tmpl;
                draw_tree_model(NULL, NULL, NULL, fly_v3mk(0, 0, 0), 1.0f, sp,
                                fly_v3mk(1, 1, 1), h, lo);
                g_wood_tmpl = NULL;
                nv = tmpl.nvert;
                if (tmpl.overflow || nv <= 0) return -1;
                if (nv > idx_cap) {
                    uint32_t *ni = (uint32_t *)realloc(idx, (size_t)nv * sizeof(uint32_t));
                    if (!ni) return -1;
                    idx = ni;
                    for (i = idx_cap; i < nv; ++i) idx[i] = (uint32_t)i;
                    idx_cap = nv;
                }
                if (!g_wood_mesh[sp][lo][k]) g_wood_mesh[sp][lo][k] = fly_gpu_mesh_create();
                if (!g_wood_mesh[sp][lo][k]) return -1;
                if (fly_gpu_mesh_upload(g_wood_mesh[sp][lo][k], tmpl.v, nv,
                                        FLY_WOOD_TFLOATS, attr, 3, idx, nv) != 0) return -1;
                g_wood_nvert[sp][lo][k] = nv;
            }
    g_wood_baked = 1;
    return 0;
}

/* Hand over what the scatter collected. Returns the vertex count drawn, so the
 * cost measure can say what moved, or -1 if the pass has to be abandoned. */
static long wood_draw(fly_gpu_prog *prog, const fly_game *g, const fly__env *e,
                      const fly__view *v, int w, int h, int ao_on) {
    static const int iattr[3] = { 4, 3, 3 };
    long verts = 0;
    int sp, lo, k, any = 0;
    for (sp = 0; sp < 2 && !any; ++sp)
        for (lo = 0; lo < 3 && !any; ++lo)
            for (k = 0; k < FLY_WOOD_SHAPES; ++k)
                if (g_wood_list[sp][lo][k].n > 0) { any = 1; break; }
    if (!any) return 0;
    env_uniforms(prog, g, e, v, w, h);
    env_shadow_uniforms(prog, e);
    env_ao_uniforms(prog, ao_on);
    for (sp = 0; sp < 2; ++sp)
        for (lo = 0; lo < 3; ++lo)
            for (k = 0; k < FLY_WOOD_SHAPES; ++k) {
                fly__woodlist *L = &g_wood_list[sp][lo][k];
                fly_gpu_mesh *m = g_wood_mesh[sp][lo][k];
                if (L->n <= 0 || !m) continue;
                if (fly_gpu_mesh_instances(m, L->v, L->n, FLY_WOOD_IFLOATS,
                                           iattr, 3, 3) != 0) return -1;
                if (fly_gpu_mesh_draw_instanced(prog, m, L->n) != 0) return -1;
                verts += (long)L->n * (long)g_wood_nvert[sp][lo][k];
            }
    return verts;
}

static int raster_environment_gpu(fly_render_target *rt, const fly_game *g, const fly__env *e,
                                  const fly__view *v, int detail, int with_objects, int msaa,
                                  int resident) {
    static fly_gpu_mesh *sky_mesh, *ring[4], *wave[3], *obj_mesh;
    static fly__tribuf tris;
    static uint32_t *obj_idx;
    static int obj_idx_cap;
    static const float sky_quad[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    static const uint32_t sky_idx[3] = { 0, 1, 2 };
    static const int sky_attr[1] = { 2 };
    static const float clear[4] = { 0.0f, 0.0f, 0.0f, 1e30f };
    /* ring geometry, mirroring the CPU LOD ladder in raster_environment */
    const float step[4] = { 950.0f, 240.0f, 60.0f, 26.0f };
    fly_gpu_prog *sky, *terrain, *water, *object, *wood;
    int half[4], k, nring, nwave, i, ao_on, wood_gpu;

    if (fly_gpu_init() != 0) return -1;
    {
        static char *tvs, *wvs;
        if (!tvs) tvs = env_vs_join(FLY_ENV_TERRAIN_VS_BODY);
        if (!wvs) wvs = env_vs_join(FLY_ENV_WATER_VS_BODY);
        if (!tvs || !wvs) return -1;
        sky = fly_gpu_program_mesh(FLY_ENV_SKY_VS, FLY_ENV_SKY_FS);
        terrain = fly_gpu_program_mesh(tvs, FLY_ENV_TERRAIN_FS);
        water = fly_gpu_program_mesh(wvs, FLY_ENV_WATER_FS);
        object = fly_gpu_program_mesh(FLY_ENV_OBJ_VS, FLY_ENV_OBJ_FS);
        wood = fly_gpu_program_mesh(FLY_ENV_WOOD_VS, FLY_ENV_WOOD_FS);
    }
    if (!sky || !terrain || !water || !object) return -1;
    /* The crowns go down as instances when the program and the templates are
     * both there, and as ordinary captured triangles when either is not — see
     * the instance lattice above. Decided here, before the scene is walked,
     * because the walk is what has to know. */
    wood_gpu = with_objects && wood != NULL && wood_bake() == 0;
    if (with_objects && !obj_mesh) obj_mesh = fly_gpu_mesh_create();
    if (with_objects && !obj_mesh) return -1;
    if (!sky_mesh) {
        sky_mesh = fly_gpu_mesh_create();
        for (k = 0; k < 4; ++k) ring[k] = fly_gpu_mesh_create();
        for (k = 0; k < 3; ++k) wave[k] = fly_gpu_mesh_create();
    }
    if (!sky_mesh) return -1;
    for (k = 0; k < 4; ++k) if (!ring[k]) return -1;
    for (k = 0; k < 3; ++k) if (!wave[k]) return -1;
    half[0] = 20;
    half[1] = 22;
    half[2] = detail >= 1 ? 22 : 14;
    half[3] = 24;
    nring = detail >= 1 ? 4 : 3;
    nwave = detail >= 1 ? 3 : 2;
    /* from outside the air there is no near field to draw — see
     * near_field_worth_drawing, and raster_environment_cpu for the twin */
    if (!near_field_worth_drawing(e)) nring = nwave = 0;
    if (fly_gpu_mesh_upload(sky_mesh, sky_quad, 3, 2, sky_attr, 1, sky_idx, 3) != 0) return -1;

    phase(FLY_PHASE_TERRAIN);
    if (fly_gpu_pass_begin(rt->w, rt->h, clear, msaa) != 0) return -1;
    env_shadow_upload(e);
    env_uniforms(terrain, g, e, v, rt->w, rt->h);
    env_shadow_uniforms(terrain, e);
    /* the traced occlusion window, as the numbers the CPU already has rather
       than a second implementation of them in the shader */
    ao_on = 0;
    {
        static unsigned sent_rev;
        static int sent_any;
        unsigned rev = 0u;
        const float *tex = g_ao_target > 0 ? ao_texels(&rev) : NULL;
        if (tex) {
            /* unit 3: 0..2 are the shadow cascades and FLY_GPU_TEX_MAX (4) is
               the upload scratch unit, so 3 is the only free one. Both channels
               ride in one RG32F for exactly that reason. Binding out of range
               is *rejected*, which leaves uAO at its default 0 — the near
               cascade — and multiplies the terrain by shadow depths. */
            int ok;
            if (!g_ao_img) { g_ao_img = fly_gpu_img_create(); sent_any = 0; }
            ok = g_ao_img != NULL;
            /* the window only changes on frames that traced */
            if (ok && (!sent_any || sent_rev != rev)) {
                ok = fly_gpu_img_set(g_ao_img, FLY_AO_N, FLY_AO_N, tex, 2) == 0;
                sent_any = ok;
                sent_rev = rev;
            }
            ao_on = ok;
        }
        env_ao_uniforms(terrain, ao_on);
    }
    for (k = 0; k < nring; ++k) {
        /* each ring stops where its finer sibling starts */
        float inner = k + 1 < nring ? step[k + 1] * (float)(half[k + 1] - 1) : 0.0f;
        fly_gpu_set1f(terrain, "uEdge", step[k] * 0.4f);
        /* the near rings span a few hundred metres, so the apron list shrinks
         * to the sites they can actually touch (uEdge widens the reach by the
         * normal's central difference) */
        env_ground_uniforms(terrain, g, v->pos.x, v->pos.y,
                            (step[k] * (float)(half[k] + 1) + step[k] * 0.4f) * 1.4143f,
                            FLY_TILTH_REACH);
        /* the two near rings shade per pixel, matching raster_environment_cpu
         * (which drops to per-vertex everywhere at detail 0) */
        fly_gpu_set1i(terrain, "uPerPixel", detail >= 1 && k >= 2);
        if (env_ring_mesh(ring[k], v, step[k], half[k], inner) != 0) continue;
        if (fly_gpu_mesh_draw(terrain, ring[k]) != 0) return -1;
    }

    env_uniforms(water, g, e, v, rt->w, rt->h);
    for (k = 0; k < nwave; ++k) {
        int s = k + 1; /* water starts one ring in from the terrain ladder */
        float inner = s + 1 < 1 + nwave ? step[s + 1] * (float)(half[s + 1] - 1) : 0.0f;
        if (env_ring_mesh(wave[k], v, step[s], half[s], inner) != 0) continue;
        if (fly_gpu_mesh_draw(water, wave[k]) != 0) return -1;
    }

    if (with_objects) {
        /* Build and shade the scene's objects on the CPU exactly as before, but
         * append their triangles instead of filling pixels, then hand the lot
         * to the GPU inside this same depth-tested pass. */
        int nv;
        phase(FLY_PHASE_OBJECTS);
        tribuf_reset(&tris);
        if (wood_gpu) wood_lists_reset();
        g_capture = &tris;
        g_capture_lights = 1;
        g_wood_collect = wood_gpu;
        g_nlights = 0;
        draw_scene_objects(rt, g, e, v);
        g_wood_collect = 0;
        g_capture = NULL;
        g_capture_lights = 0;
        nv = tris.nvert;
        phase(FLY_PHASE_UPLOAD);
        /* any bail-out below falls back to the software path, which splats its
         * own lights as it draws — drop what was recorded so they cannot
         * replay onto a later frame */
        if (tris.overflow) { g_nlights = 0; return -1; }
        if (nv > 0) {
            const int attr[5] = { 3, 3, 3, 4, 1 };
            if (nv > obj_idx_cap) {
                uint32_t *ni = (uint32_t *)realloc(obj_idx, (size_t)nv * sizeof(uint32_t));
                if (!ni) { g_nlights = 0; return -1; }
                obj_idx = ni;
                for (i = obj_idx_cap; i < nv; ++i) obj_idx[i] = (uint32_t)i;
                obj_idx_cap = nv;
            }
            if (fly_gpu_mesh_upload(obj_mesh, tris.v, nv, FLY_OBJ_FLOATS, attr, 5,
                                    obj_idx, nv) != 0) { g_nlights = 0; return -1; }
            env_uniforms(object, g, e, v, rt->w, rt->h);
            env_shadow_uniforms(object, e);
            env_ao_uniforms(object, ao_on);
            if (fly_gpu_mesh_draw(object, obj_mesh) != 0) { g_nlights = 0; return -1; }
        }
        /* and the crowns, as instances of eight shapes rather than as a
         * quarter of a million shaded vertices */
        g_wood_verts = 0;
        if (wood_gpu) {
            long wv = wood_draw(wood, g, e, v, rt->w, rt->h, ao_on);
            if (wv < 0) { g_nlights = 0; return -1; }
            g_wood_verts = wv;
        }
    }

    /* Sky last, not first: it is pinned to the far plane, so every pixel the
     * ground already covers is rejected by depth before the cloud march runs. */
    phase(FLY_PHASE_TERRAIN);
    env_uniforms(sky, g, e, v, rt->w, rt->h);
    if (fly_gpu_mesh_draw(sky, sky_mesh) != 0) { g_nlights = 0; return -1; }

    /* Where the frame stops. `resident` leaves it in GL memory for the passes
     * that used to run off a read-back copy; anything else wants the buffers,
     * so it comes down here and the software pipeline takes over. */
    if (fly_gpu_frame_end() != 0) { g_nlights = 0; return -1; }
    if (resident) return 0;
    phase(FLY_PHASE_READBACK);
    if (frame_to_target(rt) != 0) { g_nlights = 0; return -1; }
    /* the emissive splats needed the finished z-buffer to test against */
    for (i = 0; i < g_nlights; ++i)
        light_splat(rt, g_lights[i].sx, g_lights[i].sy, g_lights[i].z,
                    g_lights[i].col, g_lights[i].size);
    g_nlights = 0;
    return 0;
}

static void raster_environment_cpu(fly_render_target *rt, const fly_game *g, const fly__env *e,
                                   const fly__view *v, int detail) {
    /* nothing here is instanced, and a stale count is worse than none */
    g_wood_verts = 0;
    /* and from outside the air the sky is the whole answer — see
     * near_field_worth_drawing */
    if (!near_field_worth_drawing(e)) { raster_sky(rt, e, v); return; }
    /* four LOD rings: an ultra-fine ring under the camera keeps ground
     * texture crisp at low level; each ring skips cells its finer sibling
     * already drew. The two fine rings sample the shadow map per pixel for
     * crisp local cast shadows; the far rings resolve one lookup per vertex
     * (mountain-scale occlusion is soft, so it costs less to spare it the
     * per-pixel work). Above the budget rung the *colour* is resolved per
     * pixel on all four — see raster_tri_ground.
     *
     * The ladder is unchanged, and that is a measured answer rather than an
     * omission. Once the per-vertex path took the mesh's own sampling rate
     * (see terrain_ring) the near field stopped carrying the 90 m patch field,
     * because 26 m cells give it three and a half samples a wavelength where
     * `detail_amount` wants five. A fifth ring at 9 m brings it back and
     * reaches 216 m; at any altitude the game is flown at, 216 m of ground is
     * the bottom edge of the frame, and an A/B pair over the same camera at
     * 150 m came out indistinguishable. Two and a half thousand vertices for
     * the strip under the nose is not the trade — and it is not the answer
     * either: what the near field wanted was per-pixel shading, which it now
     * has, at the pixel's own rate rather than the mesh's. */
    float s00 = fly__ring_step[0], s0 = fly__ring_step[1];
    float s1 = fly__ring_step[2], s2 = fly__ring_step[3];
    int h00 = fly__ring_half[0], h0 = detail >= 1 ? fly__ring_half[1] : 14;
    int h1 = fly__ring_half[2], h2 = fly__ring_half[3];
    int fine = detail >= 1;
    terrain_ring(rt, g, e, v, s2, h2, s1 * (h1 - 1), 0);
    terrain_ring(rt, g, e, v, s1, h1, s0 * (h0 - 1), 0);
    terrain_ring(rt, g, e, v, s0, h0, fine ? s00 * (h00 - 1) : 0.0f, fine);
    if (fine) terrain_ring(rt, g, e, v, s00, h00, 0.0f, 1);
    water_ring(rt, g, e, v, s1, h1, s0 * (h0 - 1));
    water_ring(rt, g, e, v, s0, h0, fine ? s00 * (h00 - 1) : 0.0f);
    if (fine) water_ring(rt, g, e, v, s00, h00, 0.0f);
    /* The sky goes in last, into whatever the ground did not cover.
     *
     * It used to be painted first over the whole frame and then buried, which
     * on a valley view meant computing the most expensive pixel in the renderer
     * — a scattering integral, a cloud deck and a planet limb — for two thirds
     * of a frame that would never show one. Nothing depended on the order:
     * aerial perspective is evaluated from the same model per vertex and does
     * not read the framebuffer, and the water's reflection samples the sky
     * function rather than the pixels above it. */
    raster_sky(rt, e, v);
}

/* GPU first, always — the CPU pipeline above is the reference implementation
 * and the fallback for machines with no GL driver. The near-field scatter is
 * still CPU geometry either way, and composites against whichever z-buffer the
 * environment left behind. */
static int g_env_gpu = 0;       /* did the last environment pass run on the GPU? */
static int g_env_force_cpu = 0; /* test hook, see fly_render_env_force_cpu */
static int g_force_readback = 0; /* test hook, see fly_render_force_readback */

const char *fly_render_env_backend(void) { return g_env_gpu ? "gpu" : "cpu"; }

long fly_render_instanced_verts(void) { return g_wood_verts; }

int fly_render_force_readback(int on) {
    int was = g_force_readback;
    g_force_readback = on != 0;
    return was;
}

int fly_render_env_force_cpu(int on) {
    int was = g_env_force_cpu;
    g_env_force_cpu = on != 0;
    return was;
}

static void raster_environment(fly_render_target *rt, const fly_game *g, const fly__env *e,
                               const fly__view *v, int detail, int msaa) {
    g_env_gpu = !g_env_force_cpu &&
                raster_environment_gpu(rt, g, e, v, detail, 0, msaa, 0) == 0;
    if (!g_env_gpu) raster_environment_cpu(rt, g, e, v, detail);
}

/* ---------------- settlements ---------------- */

/* defined with the craft models below */
static void draw_light(fly_render_target *rt, const fly__view *v, fly_v3 p,
                       uint32_t col, float size);
static void hdr_add_glare(fly_render_target *rt, float sx, float sy, float r, fly_v3 rad);

static fly_v3 yawp(fly_v3 base, float dx, float dy, float dz, float cy, float sy) {
    return fly_v3mk(base.x + dx * cy - dy * sy, base.y + dx * sy + dy * cy, base.z + dz);
}

/* Where a piece of a settlement actually stands (see fly_render.h).
 *
 * A line arriving at a settlement between two towers looks like it was drawn
 * after the buildings were; a line arriving down a corridor into an open apron
 * looks like the settlement was laid out around it, which is how it would be.
 * FLY_SITE_WALK is comfortably more than FLY_SHORE_SETBACK, so a piece whose
 * footprint merely overhangs the waterline still gets built.
 *
 * Two things can be wrong with a plot: it is in the water, or it is on the
 * rail's right of way.
 *
 * The water gets a short walk inland — a plot whose footprint merely overhangs
 * the waterline was surveyed a few metres out, and moving it is what a builder
 * would do. (The piece is otherwise just a box at fly_world_ground's answer,
 * and that answer is happily below the waterline; towers used to be drawn
 * standing in open water with the sea lapping their walls.) Past that the
 * answer is not a longer walk, it is that there is no plot. Dragging a tower
 * ninety metres in from the outer avenue to squat beside an inner one is how a
 * laid-out city turns back into a scatter — and it is not what a coastline does
 * to a street plan. The street plan simply stops at the shore.
 *
 * The right of way gets no walk at all. It is a clearing; the whole point of a
 * clearing is that nothing is standing in it. Shoving the piece sideways
 * instead put it down at whatever radius the push happened to reach, which is
 * the same scatter by another route — and near a terminus the push is a
 * hundred metres, enough to land a tower on the far side of the pad.
 *
 * Deterministic and side-effect free: the renderer calls it while building
 * geometry, including during the shadow-cast replay, so the two passes must
 * agree exactly. */
int fly_site_footing(const fly_game *g, fly_v3 base, float radius, fly_v3 *p) {
    const fly_rail_route *r = &g->rail_route;
    fly_wpos plot = fly_wpos_of(*p), q = plot;
    int i;
    if (!fly_world_settle(&g->world, fly_wpos_of(base), &q, &p->z)) return 0;
    if (hypotf(q.e - plot.e, q.n - plot.n) > FLY_SITE_WALK) return 0;
    /* Seat it on the lowest ground under its own footprint rather than on the
     * height at its centre. A box is a box: on any slope a single centre height
     * leaves the downhill corners standing in the air with daylight visible
     * under them, which is the one thing that gives a settlement away as pasted
     * on. Sinking to the minimum buries the uphill side instead, and a buried
     * corner is a foundation.
     *
     * Sixteen samples around the circle that circumscribes the footprint, since
     * the piece may be at any rotation and the corners are the extremes. The
     * minimum of a finite set of directions is not the minimum of the circle,
     * so the seat goes a further FLY_SITE_BURY down: at eight samples the
     * residual measured up to half a metre of daylight on ordinary ground, and
     * a foundation buried a hand's breadth deeper is invisible where a gap is
     * not. Only the boundary is sampled — ground dipping under the middle of a
     * building cannot be seen. */
    if (radius > 0.0f) {
        float lo = p->z;
        int a;
        for (a = 0; a < 16; ++a) {
            float th = (float)a * (FLY_PI / 8.0f);
            float gz = fly_world_ground(&g->world, q.e + cosf(th) * radius,
                                        q.n + sinf(th) * radius);
            if (gz < lo) lo = gz;
        }
        p->z = lo - FLY_SITE_BURY;
    }
    /* the landing deck stays clear whatever else is built */
    { float dx = q.e - base.x, dy = q.n - base.y;
      float keep = FLY_PAD_DECK_R + 18.0f;
      if (dx * dx + dy * dy < keep * keep) return 0; }
    /* And so does the road, for exactly the reason the rail's right of way
       does: the widening where it meets the settlement is the opening it
       arrives through, and a shed standing in it is a shed in the road. */
    if (fly_road_blocked(&g->roads, q)) return 0;
    for (i = 0; i + 1 < r->point_count; ++i) {
        fly_v3 a = r->points[i].pos, b = r->points[i + 1].pos;
        float ex = b.x - a.x, ey = b.y - a.y;
        float len2 = ex * ex + ey * ey, t, dx, dy;
        /* the terminus needs an apron, not just a corridor */
        float want = (i == 0 || i + 2 == r->point_count) ? FLY_RAIL_YARD : FLY_RAIL_ROW;
        if (len2 < 1e-4f) continue;
        t = fly_clampf(((q.e - a.x) * ex + (q.n - a.y) * ey) / len2, 0.0f, 1.0f);
        dx = q.e - (a.x + ex * t);
        dy = q.n - (a.y + ey * t);
        if (dx * dx + dy * dy < want * want) return 0;
    }
    p->x = q.e;
    p->y = q.n;
    return 1;
}

/* box building: gradient walls, distinct roof, optional window bands on all
 * faces (dark glass by day, warm scattered lights by night) */
static void draw_box2(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 base, float hx, float hy, float hgt, float yaw,
                      fly_v3 wall, fly_v3 roof, int windows, uint32_t wseed) {
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 b[4] = { yawp(base, -hx, -hy, 0, cy, sy), yawp(base, hx, -hy, 0, cy, sy),
                    yawp(base, hx, hy, 0, cy, sy), yawp(base, -hx, hy, 0, cy, sy) };
    fly_v3 t[4] = { yawp(base, -hx, -hy, hgt, cy, sy), yawp(base, hx, -hy, hgt, cy, sy),
                    yawp(base, hx, hy, hgt, cy, sy), yawp(base, -hx, hy, hgt, cy, sy) };
    int k;
    for (k = 0; k < 4; ++k) {
        int k2 = (k + 1) & 3;
        wall_quad(rt, v, e, b[k], b[k2], t[k2], t[k], wall, 1.0f);
    }
    tri_shaded(rt, v, e, t[0], t[1], t[2], roof, 0.95f);
    tri_shaded(rt, v, e, t[0], t[2], t[3], roof, 0.95f);

    if (!windows) return;
    int night = e->day < 0.45f;
    /* --- what the wall is made of -----------------------------------------
     *
     * The facade used to be one thing: a grid of flat, unlit, near-black
     * rectangles on every wall of every building in the world. Two separate
     * faults, and the loud one is not the pattern.
     *
     * The loud one is that the panes were drawn with `raster_tri_view` — a
     * constant colour, straight into the frame, with no lighting of any kind.
     * Glass is the *most* view-dependent surface a settlement has: what you see
     * in a window is the sky, and which piece of sky depends on where you are
     * standing. Painting it a constant made every tower a black-and-white
     * checkerboard that stayed exactly as bright in shadow, at dusk, in a
     * storm and head-on into the sun. Everything below goes through `tri_spec`
     * instead, so a window reflects the same sky the aircraft parked in front
     * of it does, and goes dark when the sun leaves it.
     *
     * The quiet one is that one pattern is not a city. Three constructions,
     * chosen off the building's own seed:
     *
     *  - CURTAIN: a glazed tower. Continuous ribbon glazing, storey by storey,
     *    with vertical mullions standing proud of it and the wall itself
     *    showing between the ribbons as the spandrel. This is also the
     *    *cheapest* of the three — one quad a storey a face instead of eight
     *    panes — which is why the towers, the tallest and most-drawn things in
     *    a settlement, get it.
     *  - PANEL: precast concrete. Punched openings, smaller and squarer than a
     *    ribbon, with a recessed joint line at every floor so the wall reads as
     *    stacked panels rather than as one poured surface.
     *  - CLAD: profiled metal. Vertical ribs the full height of the wall and a
     *    strip window per storey between them, which is what a shed, a hangar
     *    or a light-industrial block is actually built out of.
     *
     * The glass tint varies per building too, because a street of identical
     * glazing is the same failure one step further out. */
    enum { FACADE_CURTAIN = 0, FACADE_PANEL = 1, FACADE_CLAD = 2 };
    uint32_t fh = fly_hash2(wseed + 5701u, (int)(hgt * 4.0f), (int)(hx * 4.0f));
    int facade = hgt > 26.0f ? FACADE_CURTAIN
                             : (int)((fh >> 3) % 3u);
    /* Coated architectural glass: dark in its own right, and what you see in
     * it is almost entirely reflected. `spec` is the material's specular level
     * (see tri_lit_n), so 0.62 is a little brighter than an ordinary
     * dielectric, and the exponent is high because a window is flat. */
    fly_v3 glass = fly_v3lerp(fly_v3mk(0.030f, 0.048f, 0.062f),
                              fly_v3mk(0.052f, 0.056f, 0.044f),
                              (float)((fh >> 11) & 7u) / 7.0f);
    const float glass_spec = 0.62f, glass_gloss = 620.0f;
    /* Frames, mullions and joints are the wall's own colour moved a little,
     * never a contrast: an aluminium mullion against a concrete spandrel is a
     * few per cent of tone apart in life, and it was the *contrast* that made
     * the old grid read as a checkerboard rather than the grid. */
    fly_v3 frame = fly_v3scale(wall, 1.14f);
    fly_v3 joint = fly_v3scale(wall, 0.80f);
    float bdist = fly_v3dist(fly_v3add(base, fly_v3mk(0, 0, hgt * 0.5f)), v->pos) + 1e-3f;
    /* How tall the thing is on screen. Everything below is a decision about
     * how much of a facade the projection can carry, and none of it can be
     * made without this. */
    float hpx = e->pxscale * hgt / bdist;
    int rows, face;

    /* A base course and a parapet, which is most of what a wall needs to stop
     * being a rectangle. Both are one box, both are proud of the wall by a
     * hand's breadth, and between them they give a building a bottom and a top
     * — the two places a real facade always does something. Drawn without
     * windows of their own, which is also what stops this recursing. */
    if (hgt > 7.0f && hpx > 26.0f) {
        fly_v3 trim = fly_v3scale(wall, 0.72f);
        draw_box2(rt, v, e, base, hx + 0.35f, hy + 0.35f, 2.4f, yaw, trim,
                  fly_v3scale(trim, 1.06f), 0, 0);
        draw_box2(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, hgt - 1.1f)),
                  hx + 0.45f, hy + 0.45f, 1.1f, yaw, trim, roof, 0, 0);
    }

    /* Storeys are storeys. This used to divide whatever height it was given
     * into at most eight bands, so a 24 m block and an 88 m spire both got
     * eight rows of windows — which put 11 m between floors on the spire and
     * is why a city read as a row of slabs with a decal on it rather than as
     * buildings of different sizes. A floor is 3.6 m in a tower and in a shed,
     * and letting the count follow from that is the whole of what makes a
     * skyline have scale in it.
     *
     * What bounds it now is the projection rather than a constant: five pixels
     * a storey, below which a row of windows is a grey smear that costs two
     * triangles a pane. That is a cut *and* a saving — the old cap drew all
     * eight rows on a tower three kilometres away. */
    rows = (int)(hgt / 3.6f);
    {
        int lim = (int)(hpx / 5.0f);
        if (rows > lim) rows = lim;
    }
    if (rows > 26) rows = 26;
    if (rows < 1) {
        if (hpx < 9.0f) return;   /* not even one legible row */
        rows = 1;
    }
    /* One quad on a wall face, in that face's own (across, up) coordinates and
     * standing `ox` proud of it. Every piece of every facade below is one of
     * these, which is what keeps the three constructions to a few lines each. */
#define FACE_PT(da, dz, ox) (face == 0 ? yawp(base, (da), -hy - (ox), (dz), cy, sy) : \
                             face == 1 ? yawp(base, hx + (ox), (da), (dz), cy, sy) : \
                             face == 2 ? yawp(base, (da), hy + (ox), (dz), cy, sy) : \
                                         yawp(base, -hx - (ox), (da), (dz), cy, sy))
#define FACE_QUAD(a0, a1, z0, z1, ox, col, sh, sp, gl)                          \
    do {                                                                        \
        fly_v3 f0_ = FACE_PT(a0, z0, ox), f1_ = FACE_PT(a1, z0, ox);            \
        fly_v3 f2_ = FACE_PT(a1, z1, ox), f3_ = FACE_PT(a0, z1, ox);            \
        tri_spec(rt, v, e, f0_, f1_, f2_, col, sh, sp, gl);                     \
        tri_spec(rt, v, e, f0_, f2_, f3_, col, sh, sp, gl);                     \
    } while (0)
    /* A window whose lights are on is the one thing on a facade that emits
     * rather than reflects, so it stays an HDR splat rather than a material. */
#define FACE_GLOW(a0, a1, z0, z1, ox, rad)                                      \
    do {                                                                        \
        fly_v3 f0_ = FACE_PT(a0, z0, ox), f1_ = FACE_PT(a1, z0, ox);            \
        fly_v3 f2_ = FACE_PT(a1, z1, ox), f3_ = FACE_PT(a0, z1, ox);            \
        raster_tri_hdr1(rt, v, f0_, f1_, f2_, rad);                             \
        raster_tri_hdr1(rt, v, f0_, f2_, f3_, rad);                             \
    } while (0)

    float storey = hgt / (rows + 0.4f);
    for (face = 0; face < 4; ++face) {
        float half = (face & 1) ? hy : hx;
        int cols = (int)(half / 2.2f);
        int wlim = (int)(e->pxscale * half * 2.0f / bdist / 4.0f);
        int r, c;
        if (cols > wlim) cols = wlim;
        if (cols > 8) cols = 8;
        if (cols < 1) cols = 1;
        for (r = 0; r < rows; ++r) {
            float wz = (r + 0.55f) * storey;
            /* Ribbon glazing runs the width of the wall between its mullions;
             * a punched opening and a strip window are both narrower than
             * that. All three are a fraction of their own storey in height,
             * not of the whole building — tying it to the total is what once
             * made the windows on an eighty-metre tower as tall as a house. */
            float wh = fly_clampf(storey * (facade == FACADE_PANEL ? 0.40f : 0.46f),
                                  0.7f, 2.2f);
            if (facade == FACADE_CURTAIN) {
                float in = half - 0.30f;
                if (night) {
                    /* After dark the ribbon is a row of offices, some of them
                     * still working: the same segment count the daytime
                     * mullions divide it into, so the lit ones line up with
                     * the bays rather than floating between them. */
                    for (c = 0; c < cols; ++c) {
                        uint32_t hsh = fly_hash2(wseed + (uint32_t)face * 131u, r, c);
                        float a0 = -in + (float)c * (2.0f * in / cols) + 0.10f;
                        float a1 = -in + (float)(c + 1) * (2.0f * in / cols) - 0.10f;
                        if ((hsh & 3u) != 0)
                            FACE_GLOW(a0, a1, wz, wz + wh, 0.14f,
                                      fly_v3scale(fly_v3mk(2.0f, 1.45f, 0.62f),
                                                  0.8f + (float)((hsh >> 6) & 3) * 0.25f));
                        else
                            FACE_QUAD(a0, a1, wz, wz + wh, 0.14f, glass, 1.0f,
                                      glass_spec, glass_gloss);
                    }
                } else {
                    FACE_QUAD(-in, in, wz, wz + wh, 0.14f, glass, 1.0f,
                              glass_spec, glass_gloss);
                }
                /* the transom under the ribbon: the spandrel's own shadow line,
                 * and what stops a storey being a stripe of glass on a slab */
                FACE_QUAD(-in, in, wz - 0.16f, wz, 0.20f, joint, 0.85f, 0.10f, 12.0f);
            } else {
                for (c = 0; c < cols; ++c) {
                    uint32_t hsh = fly_hash2(wseed + (uint32_t)face * 131u, r, c);
                    float ctr = -half + (c + 0.5f) * (2.0f * half / cols);
                    float ww = facade == FACADE_CLAD
                                   ? fly_clampf(half / cols * 0.72f, 0.5f, 2.4f)
                                   : fly_clampf(half / cols * 0.42f, 0.4f, 1.3f);
                    /* Recessed, not applied: a punched window sits *behind* the
                     * wall plane and a metal one behind its ribs, so both go a
                     * few centimetres in rather than a hand's breadth out. That
                     * one sign is most of the difference between a hole in a
                     * wall and a tile stuck on it. */
                    float ox = facade == FACADE_CLAD ? 0.02f : -0.04f;
                    if (night && (hsh & 3u) != 0)
                        FACE_GLOW(ctr - ww, ctr + ww, wz, wz + wh, ox + 0.06f,
                                  fly_v3scale(fly_v3mk(2.0f, 1.45f, 0.62f),
                                              0.8f + (float)((hsh >> 6) & 3) * 0.25f));
                    else
                        FACE_QUAD(ctr - ww, ctr + ww, wz, wz + wh, ox, glass,
                                  0.9f, glass_spec, glass_gloss);
                    /* the head: a lintel or a window head, one line above the
                     * opening, which is what gives a punched wall its depth */
                    if (facade == FACADE_PANEL)
                        FACE_QUAD(ctr - ww - 0.14f, ctr + ww + 0.14f,
                                  wz + wh, wz + wh + 0.14f, 0.10f, frame, 1.0f, 0.12f, 20.0f);
                }
            }
            /* And the horizontal that says which construction this is: a
             * precast wall has a joint at every floor and a clad one does not. */
            if (facade == FACADE_PANEL && r > 0)
                FACE_QUAD(-half, half, wz - storey * 0.42f, wz - storey * 0.42f + 0.10f,
                          0.06f, joint, 0.8f, 0.05f, 8.0f);
        }
        /* The verticals, drawn once for the whole wall rather than per storey:
         * curtain-wall mullions and cladding ribs are continuous by
         * construction, and a facade whose verticals restart at every floor
         * reads as a grid again — which is the thing this is here to stop. */
        if (facade != FACADE_PANEL) {
            int nrib = facade == FACADE_CURTAIN ? cols : cols + 1;
            float wide = facade == FACADE_CURTAIN ? 0.16f : 0.26f;
            float span = facade == FACADE_CURTAIN ? half - 0.30f : half;
            for (c = 0; c <= nrib; ++c) {
                float a = -span + (float)c * (2.0f * span / nrib);
                FACE_QUAD(a - wide, a + wide, 0.4f, hgt - 0.4f, 0.22f,
                          frame, 1.0f, 0.16f, 26.0f);
            }
        }
    }
#undef FACE_GLOW
#undef FACE_QUAD
#undef FACE_PT
}

/* n-gon prism (tanks, silos, pylons) with a rimmed roof */
/* Round tube swept along an arbitrary axis: the rail pipe, and anything else
 * that is a cylinder rather than an upright prism.
 *
 * The faces go through tri_shaded rather than wall_quad on purpose. wall_quad
 * lays a vertical gradient from its first edge to its second, which is what a
 * building wall wants; on a tube that edge pair runs *along* the axis, so the
 * gradient shades one end of every span dark and the other light and the pipe
 * comes out banded and far too dark. tri_shaded takes the triangle's own
 * normal, which for a barrel is exactly the roll around it. */
/* One continuous tube swept along a polyline.
 *
 * Drawn span by span, a pipe comes apart. Each span builds its own ring of
 * vertices from its own axis, so at every joint the two rings disagree and the
 * barrel opens a wedge on the outside of the bend — a chain of sausages, not a
 * pipeline. Overlapping the spans hides it head-on and makes it worse in
 * profile. The fix is to build the rings, not the spans: one ring per point,
 * lying in the plane that bisects the two directions meeting there, so
 * consecutive quads share their edge exactly and the surface is closed by
 * construction however sharply the line turns.
 *
 * The ring's frame is carried forward from the previous one rather than
 * rebuilt from a fixed hint (parallel transport). Rebuilding it makes the tube
 * spin about its own axis whenever the direction crosses the hint, which on a
 * shaded barrel reads as a seam crawling along the pipe. The mitre also widens
 * each ring by 1/cos(half-angle) so the barrel keeps its diameter through a
 * bend instead of pinching.
 *
 * `radius` is per point, so the line can widen with distance without breaking
 * continuity. */
/* Sixteen, not ten. Ten is a fine barrel for a pipe read at a kilometre and it
 * is a visible decagon on the one the walking camera stands under, and on a
 * truck's wheel at four metres. The extra faces are only ever asked for by a
 * caller close enough to count the old ones. */
#define FLY_TUBE_SIDES_MAX 16

/* Closing the end of a tube. Flat, which is a cut face and what a wheel, a hub
 * or a length of pipe somebody sawed through actually has; or domed, which is
 * what a made end has — a guideway's terminus, a vessel, anything that was
 * formed rather than cut.
 *
 * The dome is rings of the tube's own frame, so it meets the barrel exactly:
 * band j sits r*sin(a) beyond the end along the outward axis and is r*cos(a)
 * across, the last one closing on the pole with a fan of triangles nobody can
 * count because they are a few centimetres each. The bands are priced off the
 * side count the caller already chose for the barrel — a five-sided pipe two
 * pixels wide has no business paying for four latitudes.
 *
 * `out` is the outward axis at that end, and the winding follows from it, so
 * the same code closes both ends. */
static void tube_dome(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 tip, fly_v3 out, fly_v3 u, fly_v3 w, float r, int sides,
                      int head, fly_v3 wall) {
    fly_v3 band[2][FLY_TUBE_SIDES_MAX];
    int bands = sides >= 12 ? 4 : (sides >= 8 ? 3 : 2);
    int j, k, cur = 0;
    for (k = 0; k < sides; ++k) {
        float a = (float)k / (float)sides * 2.0f * FLY_PI;
        band[0][k] = fly_v3add(tip, fly_v3add(fly_v3scale(u, cosf(a) * r),
                                              fly_v3scale(w, sinf(a) * r)));
    }
    for (j = 1; j <= bands; ++j) {
        float lat = (float)j / (float)bands * (FLY_PI * 0.5f);
        float rr = cosf(lat) * r, lift = sinf(lat) * r;
        fly_v3 pole = fly_v3add(tip, fly_v3scale(out, lift));
        for (k = 0; k < sides; ++k) {
            float a = (float)k / (float)sides * 2.0f * FLY_PI;
            band[1 - cur][k] = j == bands
                             ? pole
                             : fly_v3add(pole, fly_v3add(fly_v3scale(u, cosf(a) * rr),
                                                         fly_v3scale(w, sinf(a) * rr)));
        }
        for (k = 0; k < sides; ++k) {
            int k2 = (k + 1) % sides;
            fly_v3 a0 = band[cur][k], a1 = band[cur][k2];
            fly_v3 b0 = band[1 - cur][k], b1 = band[1 - cur][k2];
            if (head) {
                tri_shaded(rt, v, e, a0, b1, a1, wall, 1.0f);
                if (j < bands) tri_shaded(rt, v, e, a0, b0, b1, wall, 1.0f);
            } else {
                tri_shaded(rt, v, e, a0, a1, b1, wall, 1.0f);
                if (j < bands) tri_shaded(rt, v, e, a0, b1, b0, wall, 1.0f);
            }
        }
        cur = 1 - cur;
    }
}

static void draw_pipeline_capped(fly_render_target *rt, const fly__view *v, const fly__env *e,
                                 const fly_v3 *pts, const float *radius, int n, int sides,
                                 fly_v3 wall, int dome) {
    fly_v3 ring[2][FLY_TUBE_SIDES_MAX];
    fly_v3 u, w, ax;
    int i, k, cur = 0, have = 0;
    if (n < 2 || sides < 3) return;
    if (sides > FLY_TUBE_SIDES_MAX) sides = FLY_TUBE_SIDES_MAX;
    /* seed the transported frame from the first segment */
    ax = fly_v3norm(fly_v3sub(pts[1], pts[0]));
    u = fly_v3norm(fly_v3cross(ax, fabsf(ax.z) > 0.9f ? fly_v3mk(1, 0, 0) : fly_v3mk(0, 0, 1)));

    for (i = 0; i < n; ++i) {
        fly_v3 din = i > 0 ? fly_v3norm(fly_v3sub(pts[i], pts[i - 1])) : fly_v3zero();
        fly_v3 dout = i + 1 < n ? fly_v3norm(fly_v3sub(pts[i + 1], pts[i])) : fly_v3zero();
        fly_v3 axis = i == 0 ? dout : (i + 1 == n ? din : fly_v3norm(fly_v3add(din, dout)));
        float widen = 1.0f;
        if (i > 0 && i + 1 < n) {
            float c = fly_v3dot(din, axis);
            widen = c > 0.2f ? 1.0f / c : 5.0f;
        }
        /* re-orthogonalise the carried frame against the new axis */
        u = fly_v3sub(u, fly_v3scale(axis, fly_v3dot(u, axis)));
        if (fly_v3len(u) < 1e-4f)
            u = fly_v3cross(axis, fabsf(axis.z) > 0.9f ? fly_v3mk(1, 0, 0) : fly_v3mk(0, 0, 1));
        u = fly_v3norm(u);
        w = fly_v3cross(axis, u);
        for (k = 0; k < sides; ++k) {
            float a = (float)k / (float)sides * 2.0f * FLY_PI;
            float r = radius[i] * widen;
            ring[cur][k] = fly_v3add(pts[i], fly_v3add(fly_v3scale(u, cosf(a) * r),
                                                       fly_v3scale(w, sinf(a) * r)));
        }
        if (have) {
            for (k = 0; k < sides; ++k) {
                int k2 = (k + 1) % sides;
                tri_shaded(rt, v, e, ring[1 - cur][k], ring[1 - cur][k2], ring[cur][k2], wall, 1.0f);
                tri_shaded(rt, v, e, ring[1 - cur][k], ring[cur][k2], ring[cur][k], wall, 1.0f);
            }
        }
        /* End caps, so a terminus is the end of a pipe and not a hole. Domed
           where the caller asked for a formed end, and otherwise the flat disc
           a cut one has. */
        if (i == 0 || i == n - 1) {
            if (dome) {
                fly_v3 out = i == 0 ? fly_v3scale(axis, -1.0f) : axis;
                tube_dome(rt, v, e, pts[i], out, u, w, radius[i], sides, i == 0, wall);
            } else {
                for (k = 0; k < sides; ++k) {
                    int k2 = (k + 1) % sides;
                    if (i == 0)
                        tri_shaded(rt, v, e, pts[i], ring[cur][k], ring[cur][k2], wall, 0.72f);
                    else
                        tri_shaded(rt, v, e, pts[i], ring[cur][k2], ring[cur][k], wall, 0.72f);
                }
            }
        }
        cur = 1 - cur;
        have = 1;
    }
}

static void draw_pipeline(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          const fly_v3 *pts, const float *radius, int n, int sides,
                          fly_v3 wall) {
    draw_pipeline_capped(rt, v, e, pts, radius, n, sides, wall, 0);
}

/* Pier carrying a rail span: a column standing on the ground with a crosshead
 * the pipe rests on. The line used to float on a thin post every eighth span,
 * which from any distance read as a pipe hanging in the air; a pier under every
 * span, wide enough to see and with a visible bearing at the top, is what makes
 * it read as built rather than drawn. Over water the column carries on down to
 * the sea bed, so a crossing looks like a viaduct instead of a floating tube. */
static void draw_prism(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 base, float r, float hgt, int n, fly_v3 wall, fly_v3 roof);

static void draw_pier(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 top, float ground, float rad, float yaw, fly_v3 col) {
    float head = rad * 0.55f;             /* crosshead depth under the pipe */
    float shaft = top.z - rad - head - ground;
    fly_v3 base = fly_v3mk(top.x, top.y, ground);
    fly_v3 pad = fly_v3mk(0.44f, 0.43f, 0.41f);
    if (shaft <= 0.5f) return;            /* the pipe is already on the deck */
    /* The pad footing. A column that meets grass at a line is a column pushed
     * into the ground; what carries a thirteen-metre bent is a block of
     * concrete wider than the shaft, sunk into the ground and standing a
     * little out of it. It is two prisms and it is the difference between a
     * structure and a stick. */
    draw_prism(rt, v, e, fly_v3mk(base.x, base.y, ground - 0.55f), rad * 1.45f, 0.95f, 6,
               pad, fly_v3scale(pad, 1.12f));
    draw_prism(rt, v, e, base, rad * 0.75f, shaft, 6, col, fly_v3scale(col, 1.15f));
    /* the bearing: a beam across the line, so the join reads as a join */
    draw_box2(rt, v, e, fly_v3mk(top.x, top.y, ground + shaft), rad * 0.55f, rad * 1.7f,
              head, yaw, fly_v3scale(col, 1.25f), fly_v3scale(col, 1.4f), 0, 0);
}

static void draw_prism(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 base, float r, float hgt, int n, fly_v3 wall, fly_v3 roof) {
    int k;
    fly_v3 top = fly_v3add(base, fly_v3mk(0, 0, hgt));
    for (k = 0; k < n; ++k) {
        float a0 = (float)k / n * 2.0f * FLY_PI, a1 = (float)(k + 1) / n * 2.0f * FLY_PI;
        fly_v3 b0 = fly_v3add(base, fly_v3mk(cosf(a0) * r, sinf(a0) * r, 0));
        fly_v3 b1 = fly_v3add(base, fly_v3mk(cosf(a1) * r, sinf(a1) * r, 0));
        fly_v3 t0 = fly_v3mk(b0.x, b0.y, top.z), t1 = fly_v3mk(b1.x, b1.y, top.z);
        wall_quad(rt, v, e, b0, b1, t1, t0, wall, 1.0f);
        tri_shaded(rt, v, e, top, t0, t1, roof, 0.95f);
    }
}

static void draw_cone(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 base, float r, float hgt, int n, fly_v3 alb) {
    fly_v3 apex = fly_v3add(base, fly_v3mk(0, 0, hgt));
    int k;
    for (k = 0; k < n; ++k) {
        float a0 = (float)k / n * 2.0f * FLY_PI, a1 = (float)(k + 1) / n * 2.0f * FLY_PI;
        fly_v3 b0 = fly_v3add(base, fly_v3mk(cosf(a0) * r, sinf(a0) * r, 0));
        fly_v3 b1 = fly_v3add(base, fly_v3mk(cosf(a1) * r, sinf(a1) * r, 0));
        tri_shaded(rt, v, e, apex, b0, b1, alb, 1.0f);
    }
}

/* orange/white windsock pointing downwind */
static void draw_windsock(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          fly_v3 base) {
    fly_v3 pole = fly_v3mk(0.14f, 0.14f, 0.14f);
    draw_box2(rt, v, e, base, 0.18f, 0.18f, 6.5f, 0, pole, pole, 0, 0);
    fly_v3 top = fly_v3add(base, fly_v3mk(0, 0, 6.2f));
    float ws = fly_v3len(fly_v3mk(e->wind.x, e->wind.y, 0));
    fly_v3 wd = ws > 0.5f ? fly_v3norm(fly_v3mk(e->wind.x, e->wind.y, 0)) : fly_v3mk(1, 0, 0);
    float droop = fly_clampf(1.0f - ws / 14.0f, 0.0f, 0.8f);
    fly_v3 tip = fly_v3add(top, fly_v3add(fly_v3scale(wd, 3.4f * (1.0f - droop * 0.4f)),
                                          fly_v3mk(0, 0, -droop * 1.8f)));
    int k, n = 6;
    fly_v3 axis_a = fly_v3norm(fly_v3cross(wd, fly_v3mk(0, 0, 1)));
    for (k = 0; k < n; ++k) {
        float a0 = (float)k / n * 2.0f * FLY_PI, a1 = (float)(k + 1) / n * 2.0f * FLY_PI;
        fly_v3 r0 = fly_v3add(top, fly_v3add(fly_v3scale(axis_a, cosf(a0) * 0.75f),
                                             fly_v3mk(0, 0, sinf(a0) * 0.75f)));
        fly_v3 r1 = fly_v3add(top, fly_v3add(fly_v3scale(axis_a, cosf(a1) * 0.75f),
                                             fly_v3mk(0, 0, sinf(a1) * 0.75f)));
        fly_v3 alb = (k & 1) ? fly_v3mk(0.95f, 0.42f, 0.10f) : fly_v3mk(0.92f, 0.90f, 0.86f);
        tri_shaded(rt, v, e, tip, r0, r1, alb, 1.0f);
        (void)a1;
    }
}

/* raised landing deck: skirt, apron ring, dashed perimeter, inner circle,
 * H marking, pulsing edge lights and a windsock */
/* --- what a paved surface is actually like -------------------------------
 *
 * Every square metre of pavement in this world was one flat colour with one
 * specular level: the pad deck, the hardstands, the service tracks and the
 * roads. That is not what asphalt looks like from ten metres and it is very
 * much not what it looks like in the rain, which is the weather half of the
 * frames in the gallery.
 *
 * Two fields and one weather term, all evaluated in world space so that
 * nothing here follows the tessellation — a wear pattern keyed to the
 * triangles is a way of drawing the triangles.
 *
 *  - The patch field, at twenty-one metres. Pavement is laid in lands, cut
 *    open and made good again, and each of those is a slightly different mix
 *    with a slightly different age. That is what stops a large flat surface
 *    reading as a swatch, and twenty-one metres is coarse enough that a deck
 *    tessellated into three-metre segments samples it smoothly rather than
 *    faceting on it.
 *  - The aggregate, at two and a half. Much finer, and deliberately given a
 *    small amplitude: on the CPU rasterizer, which resolves one colour per
 *    triangle, this is at the edge of what the geometry can carry, and a
 *    fine field with a large amplitude is how a surface starts to sparkle.
 *  - And the rain. Water on asphalt is the textbook case of the wet surface:
 *    it darkens hard, because the pores fill, and it turns specular, because
 *    the film is flat where the surface is not. Both of those go a long way
 *    further here than on soil — a wet road is nearly black and mirrors the
 *    sky, which is exactly why a rainy frame with dry-looking tarmac in it
 *    reads as a picture with rain drawn on top.
 *
 * `grain` scales both fields, so a caller can ask for a smooth new apron or a
 * badly worn one, and 0 gets the flat colour back exactly. */
typedef struct {
    fly_v3 col;
    float spec;   /* specular level on tri_spec's 0..1 scale */
    float gloss;  /* Blinn exponent */
} fly__paving;

static fly__paving pave_surface(const fly__env *e, fly_v3 base, float x, float y,
                                float grain) {
    fly__paving s;
    s.col = base;
    s.spec = 0.16f;
    s.gloss = 10.0f;
    if (grain > 0.0f) {
        float patch = fly_noise2(e->seed + 51u, x / 21.0f, y / 21.0f);
        float agg = fly_noise2(e->seed + 52u, x / 2.5f, y / 2.5f);
        s.col = fly_v3scale(s.col, 1.0f + grain * (0.20f * patch + 0.07f * agg));
    }
    if (e->wet > 0.002f) {
        s.col = fly_v3scale(s.col, 1.0f - 0.46f * e->wet);
        s.spec = fly_lerpf(s.spec, 0.95f, e->wet);
        s.gloss = fly_lerpf(s.gloss, 420.0f, e->wet);
    }
    return s;
}

/* The same surface, drawn. Saves every caller repeating the centroid. */
static void tri_paved(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 a, fly_v3 b, fly_v3 c, fly_v3 base, float shade, float grain) {
    fly__paving s = pave_surface(e, base, (a.x + b.x + c.x) * (1.0f / 3.0f),
                                 (a.y + b.y + c.y) * (1.0f / 3.0f), grain);
    tri_spec(rt, v, e, a, b, c, s.col, shade, s.spec, s.gloss);
}

static void draw_pad(fly_render_target *rt, const fly__view *v, const fly__env *e,
                     fly_v3 center, float r, int idx) {
    float deck = 0.6f;
    fly_v3 top = fly_v3add(center, fly_v3mk(0, 0, deck));
    fly_v3 asphalt = fly_v3mk(0.135f, 0.145f, 0.16f);
    fly_v3 apron = fly_v3mk(0.175f, 0.185f, 0.20f);
    fly_v3 skirtc = fly_v3mk(0.09f, 0.10f, 0.115f);
    /* Tessellate the deck to its size on screen. A fixed 18 segments is a
     * visible decagon from the pad you are parked on — the rim reads as a
     * polygon and the painted rings inside it as a stop sign — while being
     * more than a 55 m disc needs from three kilometres up. Chord error is
     * r*(1 - cos(pi/seg)); solving that for half a pixel gives seg roughly
     * proportional to the square root of the projected radius, and the bounds
     * keep the near case honest and the far case cheap. `seg` stays a multiple
     * of four so the dashed perimeter (every other segment) closes evenly. */
    float dist = fly_v3dist(center, v->pos);
    float rpx = r * v->sy / (dist > 1.0f ? dist : 1.0f);
    int k, seg = (int)(4.0f * sqrtf(rpx > 1.0f ? rpx : 1.0f));
    seg = (seg + 3) & ~3;
    if (seg < 12) seg = 12;
    if (seg > 96) seg = 96;
    for (k = 0; k < seg; ++k) {
        float a0 = (float)k / seg * 2.0f * FLY_PI, a1 = (float)(k + 1) / seg * 2.0f * FLY_PI;
        fly_v3 e0 = fly_v3mk(cosf(a0), sinf(a0), 0), e1 = fly_v3mk(cosf(a1), sinf(a1), 0);
        fly_v3 g0 = fly_v3add(center, fly_v3scale(e0, r)), g1 = fly_v3add(center, fly_v3scale(e1, r));
        fly_v3 t0 = fly_v3add(top, fly_v3scale(e0, r)), t1 = fly_v3add(top, fly_v3scale(e1, r));
        wall_quad(rt, v, e, fly_v3add(g0, fly_v3mk(0, 0, -2.5f)), fly_v3add(g1, fly_v3mk(0, 0, -2.5f)),
                  t1, t0, skirtc, 0.9f);
        /* --- the deck, in bands ------------------------------------------
         *
         * It used to be two: a concrete apron ring and one triangle from the
         * rim to the centre for everything inside it. That single triangle is
         * why the deck was a swatch — there is no way to say anything about a
         * surface you have drawn as one facet fifty metres long, so the wear
         * field, the joints and the rubber all had nowhere to live.
         *
         * Four bands, and the radii are what a pad is: the touchdown circle in
         * the middle, where the tyres land and the rubber goes down; the
         * working surface around it; the run-off; and the concrete apron at the
         * rim, which is a different material from the asphalt inside it and is
         * meant to look like one. A band is two triangles a segment, so this
         * costs three more per segment than the old version and buys everything
         * below. */
        {
            static const float band[5] = { 0.0f, 0.26f, 0.52f, 0.82f, 1.0f };
            int q;
            for (q = 0; q < 4; ++q) {
                fly_v3 base = q == 3 ? apron : asphalt;
                fly_v3 p0 = fly_v3add(top, fly_v3scale(e0, r * band[q]));
                fly_v3 p1 = fly_v3add(top, fly_v3scale(e1, r * band[q]));
                fly_v3 p2 = fly_v3add(top, fly_v3scale(e1, r * band[q + 1]));
                fly_v3 p3 = fly_v3add(top, fly_v3scale(e0, r * band[q + 1]));
                /* Rubber. Every arrival puts a little more of its tyres on the
                 * same few metres of deck, and on a real pad that is the single
                 * most visible thing about the surface — a dark stain in the
                 * middle fading outward, not a uniform grey. Concrete does not
                 * get it: the apron is where aircraft are parked, not landed
                 * on. */
                if (q < 2)
                    base = fly_v3scale(base, q == 0 ? 0.62f : 0.84f);
                if (q == 0) {
                    tri_paved(rt, v, e, top, p2, p3, base, 0.95f, 0.5f);
                } else {
                    tri_paved(rt, v, e, p0, p1, p2, base, 0.95f, 1.0f);
                    tri_paved(rt, v, e, p0, p2, p3, base, 0.95f, 1.0f);
                }
            }
        }
        /* The joints. Pavement this size is laid in bays and every bay has an
         * edge, sealed and a shade darker than the surface it separates; a slab
         * with no joints in it is a slab nobody poured. Radial ones every
         * eighth segment so they stay a fixed number round the deck however
         * finely it is tessellated, and one circumferential at each band
         * change, which is where a real construction joint would be anyway. */
        if (seg >= 24 && (k % (seg / 8)) == 0) {
            float wj = 2.0f * FLY_PI / (float)seg * 0.14f;
            fly_v3 j0 = fly_v3mk(cosf(a0 - wj), sinf(a0 - wj), 0);
            fly_v3 j1 = fly_v3mk(cosf(a0 + wj), sinf(a0 + wj), 0);
            fly_v3 q0 = fly_v3add(top, fly_v3add(fly_v3scale(j0, r * 0.12f), fly_v3mk(0, 0, 0.02f)));
            fly_v3 q1 = fly_v3add(top, fly_v3add(fly_v3scale(j1, r * 0.12f), fly_v3mk(0, 0, 0.02f)));
            fly_v3 q2 = fly_v3add(top, fly_v3add(fly_v3scale(j1, r * 0.99f), fly_v3mk(0, 0, 0.02f)));
            fly_v3 q3 = fly_v3add(top, fly_v3add(fly_v3scale(j0, r * 0.99f), fly_v3mk(0, 0, 0.02f)));
            fly_v3 jc = fly_v3scale(asphalt, 0.55f);
            tri_spec(rt, v, e, q0, q1, q2, jc, 0.9f, 0.08f, 6.0f);
            tri_spec(rt, v, e, q0, q2, q3, jc, 0.9f, 0.08f, 6.0f);
        }
        if (k & 1) { /* dashed amber perimeter ring */
            fly_v3 r0 = fly_v3add(top, fly_v3scale(e0, r * 0.90f));
            fly_v3 r1 = fly_v3add(top, fly_v3scale(e1, r * 0.90f));
            fly_v3 r2 = fly_v3add(top, fly_v3scale(e1, r * 0.84f));
            fly_v3 r3 = fly_v3add(top, fly_v3scale(e0, r * 0.84f));
            uint32_t mk = FLY_RGB(212, 176, 84);
            raster_tri_view(rt, v, fly_v3add(r0, fly_v3mk(0, 0, 0.12f)), fly_v3add(r1, fly_v3mk(0, 0, 0.12f)),
                            fly_v3add(r2, fly_v3mk(0, 0, 0.12f)), mk);
            raster_tri_view(rt, v, fly_v3add(r0, fly_v3mk(0, 0, 0.12f)), fly_v3add(r2, fly_v3mk(0, 0, 0.12f)),
                            fly_v3add(r3, fly_v3mk(0, 0, 0.12f)), mk);
        }
        { /* thin white inner circle */
            fly_v3 r0 = fly_v3add(top, fly_v3scale(e0, r * 0.485f));
            fly_v3 r1 = fly_v3add(top, fly_v3scale(e1, r * 0.485f));
            fly_v3 r2 = fly_v3add(top, fly_v3scale(e1, r * 0.455f));
            fly_v3 r3 = fly_v3add(top, fly_v3scale(e0, r * 0.455f));
            uint32_t mk = FLY_RGB(196, 202, 208);
            raster_tri_view(rt, v, fly_v3add(r0, fly_v3mk(0, 0, 0.12f)), fly_v3add(r1, fly_v3mk(0, 0, 0.12f)),
                            fly_v3add(r2, fly_v3mk(0, 0, 0.12f)), mk);
            raster_tri_view(rt, v, fly_v3add(r0, fly_v3mk(0, 0, 0.12f)), fly_v3add(r2, fly_v3mk(0, 0, 0.12f)),
                            fly_v3add(r3, fly_v3mk(0, 0, 0.12f)), mk);
        }
    }
    { /* H marking */
        uint32_t mk = FLY_RGB(206, 212, 218);
        float hw = r * 0.030f, hl = r * 0.135f, gap = r * 0.080f, z = 0.14f;
#define HQ(x0, y0, x1, y1) \
        raster_tri_view(rt, v, fly_v3add(top, fly_v3mk(x0, y0, z)), fly_v3add(top, fly_v3mk(x1, y0, z)), \
                        fly_v3add(top, fly_v3mk(x1, y1, z)), mk); \
        raster_tri_view(rt, v, fly_v3add(top, fly_v3mk(x0, y0, z)), fly_v3add(top, fly_v3mk(x1, y1, z)), \
                        fly_v3add(top, fly_v3mk(x0, y1, z)), mk)
        HQ(-gap - hw, -hl, -gap + hw, hl);
        HQ(gap - hw, -hl, gap + hw, hl);
        HQ(-gap, -hw, gap, hw);
#undef HQ
    }
    for (k = 0; k < 9; ++k) { /* pulsing cyan edge lights */
        float a = (float)k / 9.0f * 2.0f * FLY_PI;
        float pulse = 0.6f + 0.4f * sinf((float)e->time * 2.2f + (float)k * 0.7f + (float)idx);
        int bright = (int)(fly_clampf((0.55f + (1.0f - e->day)) * pulse, 0, 1) * 255.0f);
        draw_light(rt, v, fly_v3add(top, fly_v3mk(cosf(a) * r * 0.95f, sinf(a) * r * 0.95f, 0.5f)),
                   FLY_RGBA(120, 225, 255, bright), 0.8f);
    }
    draw_windsock(rt, v, e, fly_v3add(center, fly_v3mk(r * 1.25f, -r * 0.45f, 0)));
}

/* tapered beacon mast with a pulsing site lamp */
static void draw_beacon(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        fly_v3 base, float hgt, uint32_t col, int idx) {
    fly_v3 steel = fly_v3mk(0.30f, 0.31f, 0.34f);
    draw_box2(rt, v, e, base, 1.1f, 1.1f, hgt * 0.45f, 0.4f, steel, steel, 0, 0);
    draw_box2(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, hgt * 0.45f)), 0.65f, 0.65f, hgt * 0.35f,
              0.4f, steel, steel, 0, 0);
    draw_box2(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, hgt * 0.8f)), 0.32f, 0.32f, hgt * 0.2f,
              0.4f, steel, steel, 0, 0);
    float pulse = 0.5f + 0.5f * sinf((float)e->time * 2.6f + (float)idx * 1.7f);
    int a = (int)(fly_clampf((0.5f + (1.0f - e->day) * 0.8f) * (0.4f + 0.6f * pulse), 0, 1) * 255.0f);
    draw_light(rt, v, fly_v3add(base, fly_v3mk(0, 0, hgt + 1.5f)),
               (col & 0x00FFFFFFu) | ((uint32_t)a << 24), 1.4f);
}

/* blinking red aviation warning light for tall structures */
static void draw_warning_light(fly_render_target *rt, const fly__view *v, const fly__env *e,
                               fly_v3 pos, int phase) {
    if (fmod(e->time + (double)phase * 0.37, 1.6) > 0.85) return;
    draw_light(rt, v, pos, FLY_RGBA(255, 46, 36, 235), 1.1f);
}

/* Should this object be built at all?
 *
 * In the beauty pass, anything outside the view cone is invisible. In the
 * shadow cast pass the camera cone means nothing — an off-screen tower still
 * throws a shadow into frame — so the bound there is the far cascade's
 * footprint, outside which a caster cannot reach the map. */
static int object_worth_drawing(const fly__view *v, const fly_render_target *rt,
                                fly_v3 centre, float radius) {
    if (g_shadow_cast) {
        const fly__shadow *sm = g_shadow_cast;
        int c = FLY_SM_CASCADES - 1;
        float dx = centre.x - sm->center[c].x, dy = centre.y - sm->center[c].y;
        float lim = sm->half[c] * 1.5f + radius;
        return dx * dx + dy * dy <= lim * lim;
    }
    return sphere_visible(v, rt->w, rt->h, centre, radius);
}

/* Bearing the rail arrives on, or 0 where it does not reach. Settlement layouts
 * index their avenues off this so the line comes in along one instead of
 * squeezing between two towers — the difference between a town with a railway
 * and a railway with a town in the way. */
/* ---------------- settlement detail kit ----------------
 *
 * A settlement used to be six or eight primitives on a bare hillside: four
 * tanks, ten towers, a mast. From the air that is a diagram of a place rather
 * than a place. What is missing is not size — the towers are eighty metres —
 * it is the small stuff that says somebody works here: a fence with a gate in
 * it, pallets stacked where a forklift left them, a lamp on a pole, a truck
 * that is somewhere different than it was a minute ago.
 *
 * So: a kit of props, each a handful of triangles, instanced by rule. Every one
 * of them is placed off the site's own RNG and its own axis, so a site is laid
 * out rather than sprinkled, and every one of them is culled by distance before
 * it is built — the far LOD band drops the lot and keeps the silhouette, which
 * is what the frame budget can afford.
 *
 * All of it is deterministic in the site seed and the clock, so two machines
 * looking at the same yard at the same moment see the same crates. */

/* how much detail a site at this range gets: 0 none, 1 the big props, 2 all */
static int site_detail(const fly__view *v, fly_v3 base) {
    float d = fly_v3dist(base, v->pos);
    return d < 900.0f ? 2 : d < 2600.0f ? 1 : 0;
}

/* A crate: a box with a lid seam and a darker base, which is enough at the
 * sizes these are ever seen. Cheaper than it looks — the seam is the roof
 * quad of a second, flatter box. */
static void prop_crate(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 p, float sx, float sy2, float h, float yaw, fly_v3 col) {
    draw_box2(rt, v, e, p, sx, sy2, h, yaw, col, fly_v3scale(col, 1.18f), 0, 0);
    draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, h)), sx * 1.06f, sy2 * 1.06f, 0.10f,
              yaw, fly_v3scale(col, 0.55f), fly_v3scale(col, 0.8f), 0, 0);
}

/* A drum, on its end. Six sides is plenty at a metre tall and the rim reads as
 * the rolling hoop. */
static void prop_drum(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 p, fly_v3 col) {
    draw_prism(rt, v, e, p, 0.42f, 1.15f, 6, col, fly_v3scale(col, 0.7f));
}

/* A run of fence: posts on a fixed pitch with two rails between them. The one
 * prop that does the most work — a boundary is what turns a scatter of sheds
 * into a yard, and the eye reads the line long before it reads what is inside
 * it. `gap` leaves an opening centred on a fraction along the run, which is
 * the gate. */
static void prop_fence(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 a, fly_v3 b, float gap_at, float gap_w) {
    fly_v3 d = fly_v3sub(b, a);
    float len = hypotf(d.x, d.y);
    float yaw = atan2f(d.y, d.x);
    fly_v3 post = fly_v3mk(0.20f, 0.19f, 0.17f);
    fly_v3 rail = fly_v3mk(0.26f, 0.25f, 0.23f);
    int n = (int)(len / 6.0f), i;
    if (n < 2) return;
    if (n > 20) n = 20;
    for (i = 0; i <= n; ++i) {
        float f = (float)i / (float)n;
        fly_v3 q;
        if (gap_w > 0.0f && fabsf(f - gap_at) * len < gap_w) continue;
        q = fly_v3lerp(a, b, f);
        draw_box2(rt, v, e, q, 0.13f, 0.13f, 1.9f, yaw, post, post, 0, 0);
    }
    for (i = 0; i < n; ++i) {
        float f0 = (float)i / (float)n, f1 = (float)(i + 1) / (float)n;
        float fm = (f0 + f1) * 0.5f;
        fly_v3 m;
        int r;
        if (gap_w > 0.0f && fabsf(fm - gap_at) * len < gap_w) continue;
        m = fly_v3lerp(a, b, fm);
        for (r = 0; r < 2; ++r)
            draw_box2(rt, v, e, fly_v3add(m, fly_v3mk(0, 0, 0.75f + (float)r * 0.75f)),
                      len / (float)n * 0.5f, 0.055f, 0.11f, yaw, rail, rail, 0, 0);
    }
}

/* One lamp in how many, at this range.
 *
 * A chain of lights is a rhythm rather than a count: what carries is that they
 * are evenly spaced and that they keep going, and past a few kilometres the eye
 * cannot resolve a sixty-metre pitch anyway. Thinning it with distance is what
 * makes lighting a way for its whole length cost about what lighting the near
 * two kilometres of it used to. Both the roads and the rail light their line
 * this way and both have the same reason to, so it is one ladder.
 *
 * Powers of two, and that is the whole of why the thinning does not read as
 * lamps going out: a lamp kept at one range is kept at every range beyond it,
 * so crossing a threshold takes half of them away and moves none of the rest.
 * An odd stride would renumber the survivors every time the camera moved. */
static int lamp_stride(float dist) {
    if (dist > 16000.0f) return 8;
    if (dist > 9000.0f) return 4;
    if (dist > 4200.0f) return 2;
    return 1;
}

/* --- what a lamp is made of ------------------------------------------------
 *
 * Two lightings, because the world has two ages of infrastructure in it and
 * they do not look alike. The roads and the yards are high-pressure sodium:
 * the orange every arterial road on earth was lit by, warm enough that the
 * pool is a different colour from anything the moon does. The guideway is the
 * newer thing and is lit the way anything built this decade is, in white with
 * a trace of blue — which is what makes a rail crossing a road read as two
 * systems meeting rather than as one lamp catalogue used twice.
 *
 * Linear radiance rather than an albedo, and that was the bug worth fixing
 * first: the pool used to be a *lit surface* — the ground's own colour, shaded
 * by whatever light was falling on it — which after dark is a dark disc. The
 * light a lamp throws is the one surface in the world that is not lit by the
 * sun, and lighting it by the sun after dark put a shadow under every
 * lamp-post in the world. */
#define FLY_LAMP_SODIUM fly_v3mk(1.00f, 0.66f, 0.30f)
#define FLY_LAMP_WHITE fly_v3mk(0.94f, 0.95f, 1.00f)

/* When this particular lamp comes on.
 *
 * A line of street lighting does not switch as one. Every column has its own
 * cell looking at its own patch of sky, they are years apart in age, and what
 * you actually see at dusk is a road lighting up raggedly over a few minutes.
 * The threshold is jittered off the column's own position, so the order is a
 * property of the world rather than of the frame — the same lamps come on
 * first every dusk, which is what makes it read as hardware and not as noise. */
static float lamp_dark(const fly__env *e, fly_v3 p) {
    uint32_t h = fly_hash2(0x1a5eu, (int32_t)p.x, (int32_t)p.y);
    float j = ((float)(h & 0xFFFFu) / 65536.0f - 0.5f) * 0.11f;
    return 1.0f - fly_smoothstepf(0.16f + j, 0.46f + j, e->day);
}

/* How far a road lantern is quoted at, and what it lays on the road under it.
 *
 * A column is 8.4 m to the lantern and stands beside a carriageway 15 m wide,
 * so a throw of about four mounting heights is what keeps a 62 m pitch from
 * having a dark half in the middle of it — which is the pitch the survey lays
 * and the reason a real road holds one at all. `lamp_light` is where the shape
 * of the throw is, and this is only how big it is and how bright. */
#define FLY_LAMP_MOUNT 8.4f
#define FLY_LAMP_REACH (FLY_LAMP_MOUNT * 4.1f)
#define FLY_LAMP_POWER 0.14f

/* A lamp on a pole: column, cranked arm, and a head that is a real emitter
 * after dark, throwing a real light out of it.
 *
 * It used to be handed the height and the grade of whatever it was standing
 * over, because the pool it laid was a flat shape that had to be told which
 * plane to lie in. It is not laid on anything now — `lamp_light` puts the light
 * on whatever the frame drew in front of the lantern — so a lamp needs to know
 * nothing at all about the ground it is lighting, which is also the right
 * answer for a lamp. */
static void prop_lamp(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 p, float yaw) {
    /* Dark and thin was the first version and it was wrong: from the air a
     * 7.5 m pole with a 0.8 m head on it is a black stick standing in grass,
     * and a field of them reads as litter rather than as lighting. Galvanised
     * rather than near-black, a plinth so it is installed in the ground instead
     * of pushed into it, and a head with enough of a cross-arm to be a shape at
     * two hundred metres. */
    fly_v3 steel = fly_v3mk(0.46f, 0.47f, 0.49f);
    fly_v3 conc = fly_v3mk(0.40f, 0.39f, 0.37f);
    float cy = cosf(yaw), sy = sinf(yaw);
    float h = 8.4f;
    fly_v3 head = yawp(p, 2.3f, 0, h, cy, sy);
    float dark = fly_clampf(lamp_dark(e, p), 0.0f, 1.0f);
    draw_prism(rt, v, e, p, 1.05f, 0.35f, 8, conc, fly_v3scale(conc, 0.8f));
    draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, 0.3f)), 0.30f, 0.30f, h - 0.3f, yaw,
              steel, fly_v3scale(steel, 1.12f), 0, 0);
    /* the cross-arm, cranked out over what it lights */
    draw_box2(rt, v, e, yawp(p, 1.2f, 0, h - 0.22f, cy, sy), 1.35f, 0.14f, 0.24f, yaw,
              steel, fly_v3scale(steel, 1.1f), 0, 0);
    draw_box2(rt, v, e, yawp(p, 0.55f, 0, h - 1.15f, cy, sy), 0.11f, 0.11f, 1.0f,
              yaw + 0.85f, steel, steel, 0, 0);   /* the stay under it */
    if (dark < 0.02f) {
        draw_box2(rt, v, e, head, 0.80f, 0.46f, 0.34f, yaw, steel,
                  fly_v3mk(0.72f, 0.72f, 0.68f), 0, 0);
        return;
    }
    draw_box2(rt, v, e, head, 0.80f, 0.46f, 0.34f, yaw, steel,
              fly_v3scale(steel, 1.1f), 0, 0);
    {
        /* The lantern's own glass, as an emitter, and the light out of it.
         *
         * The source is the glass rather than the column: a lantern is cranked
         * out over what it lights, so the light comes from over the carriageway
         * and the verge behind the post gets what spills back past the cutoff,
         * which is what a road lamp actually does. That used to be a hand-placed
         * offset on a painted ellipse; it is the geometry now.
         *
         * Aimed along the road, which is the arm's own cross-axis: the arm
         * reaches *across* the carriageway and the light goes *down* it. */
        fly_v3 glow = fly_v3scale(fly_v3mk(2.4f, 1.85f, 1.05f), dark);
        raster_tri_hdr1(rt, v, yawp(p, 1.58f, -0.40f, h - 0.03f, cy, sy),
                        yawp(p, 3.02f, -0.40f, h - 0.03f, cy, sy),
                        yawp(p, 3.02f, 0.40f, h - 0.03f, cy, sy), glow);
        raster_tri_hdr1(rt, v, yawp(p, 1.58f, -0.40f, h - 0.03f, cy, sy),
                        yawp(p, 3.02f, 0.40f, h - 0.03f, cy, sy),
                        yawp(p, 1.58f, 0.40f, h - 0.03f, cy, sy), glow);
        draw_light(rt, v, fly_v3add(head, fly_v3mk(0, 0, -0.3f)),
                   FLY_RGBA(255, 214, 150, (int)(215.0f * dark)), 1.15f);
        lamp_light(rt, v, e, fly_v3add(head, fly_v3mk(0, 0, -0.22f)),
                   yaw + FLY_PI * 0.5f, FLY_LAMP_MOUNT, FLY_LAMP_REACH,
                   FLY_LAMP_SODIUM, FLY_LAMP_POWER * dark);
    }
}

/* A flatbed: cab, bed, four wheels, and headlights after dark. Small enough
 * that the wheels are boxes and nobody is the wiser. */
/* where a track leaving the deck for `to` crosses the deck's edge */
static fly_v3 deck_edge(fly_v3 base, fly_v3 to) {
    float dx = to.x - base.x, dy = to.y - base.y;
    float d = hypotf(dx, dy);
    if (d < 1.0f) return base;
    return fly_v3mk(base.x + dx / d * FLY_PAD_DECK_R,
                    base.y + dy / d * FLY_PAD_DECK_R, base.z);
}

/* How far the drawn terrain sits above `fly_world_ground` here.
 *
 * The terrain is a mesh, and its finest ring has twenty-six metre cells; what
 * gets rasterised between four of its vertices is their bilinear interpolation,
 * not the heightfield. Over a concave patch — a valley floor, or the outer
 * shoulder of the smoothstep that grades a landing apron flat — that surface
 * sits *above* the true height, by the better part of a metre on the blend ring
 * around a pad. Anything laid on the ground at `fly_world_ground` plus a few
 * centimetres is therefore buried exactly where a settlement puts it, which is
 * what happened: the first service tracks vanished for their outer half and
 * looked like they stopped in a field.
 *
 * The excess is the height of the highest chord of a cell through this point,
 * which is what the mesh draws, sampled four ways because the mesh grid is
 * anchored on the camera and its phase is not knowable here. It is a smooth,
 * slowly varying field, so a caller may evaluate it once for a whole
 * cross-section rather than at every vertex. */
static float ground_bias_r(const fly_world *w, float x, float y, float r) {
    const float q = r * 0.70710678f;   /* r / sqrt(2), for the diagonals */
    float h = fly_world_ground(w, x, y);
    float c[4], top = h;
    int i;
    c[0] = 0.5f * (fly_world_ground(w, x - r, y) + fly_world_ground(w, x + r, y));
    c[1] = 0.5f * (fly_world_ground(w, x, y - r) + fly_world_ground(w, x, y + r));
    c[2] = 0.5f * (fly_world_ground(w, x - q, y - q) + fly_world_ground(w, x + q, y + q));
    c[3] = 0.5f * (fly_world_ground(w, x - q, y + q) + fly_world_ground(w, x + q, y - q));
    for (i = 0; i < 4; ++i) if (c[i] > top) top = c[i];
    return top - h;
}

static float ground_bias(const fly_world *w, float x, float y) {
    return ground_bias_r(w, x, y, 13.0f);  /* half the finest ring's cell */
}

/* --- the heightfield memos -------------------------------------------------
 *
 * Two questions are asked of the same settled heightfield over and over, so
 * there are two tables through one mechanism.
 *
 * `drawn_ground` is ten heightfield evaluations — one for the height and nine
 * for the chord above it — and the frame asks it for the same points over and
 * over. A road's cross-sections stand at fixed stations along a fixed survey,
 * so every station the last frame priced is priced again this frame at exactly
 * the same coordinates. Measured on the chase view of seed 4242 at 1280x720,
 * the road network asked 56,520 heightfield taps a frame to produce 5,892
 * vertices: a third of the whole object build, spent on ground that had not
 * moved since the world was made.
 *
 * So the answers are kept. Keyed on the exact bits of x and y and not on a
 * quantisation of them: a memo that rounds its key is a different function from
 * the one it stands in for, and one whose answers then depend on which points
 * happened to be asked first — which is not something a renderer required to be
 * deterministic for a given seed and camera can have. Exact keys make this an
 * optimisation with no visual term at all, so there is nothing to tune and
 * nothing to look at: the value a hit returns is the value a miss would have
 * computed, bit for bit.
 *
 * Direct-mapped and fixed-size, so it cannot grow, needs no eviction policy and
 * cannot be a leak; a collision is a miss and costs exactly what the function
 * always cost. The heightfield is settled before anything is drawn —
 * fly_road_build is the last thing that touches it — but which world these
 * heights belong to is recorded anyway, and a different one empties the table,
 * because a still of one world must not be able to read another's ground.
 *
 * Not thread-safe, and does not need to be: every caller is an object or prop
 * builder on the main thread. The path tracer, which is the one threaded part
 * of the renderer, marches the heightfield itself and never comes here.
 *
 * `scatter_ground` is the second question and the plain height: what a tree, a
 * boulder or a tuft of grass stands on. It repeats for the same reason one
 * scale out — the scatter is deterministic per world cell, so a cell that was
 * in range last frame asks for exactly the coordinates it asked for then — and
 * everything above holds for it word for word, which is why it goes through the
 * same table rather than a second copy of the argument.
 *
 * The size is set by how many distinct points a frame asks about, not by taste.
 * Closed canopy at the finest rung is 15,260 asks over about thirteen thousand
 * trees, so 65,536 slots run at 92% hits; 32,768 gives 84% and 131,072 gives
 * 96%, which is four times the memory for four points. */
#define FLY_MEMO_BITS 16
#define FLY_MEMO_N (1 << FLY_MEMO_BITS)

typedef struct {
    float x, y, z;
    int used;
} fly__memo_slot;

typedef struct {
    fly__memo_slot *slot;
    const fly_world *world;
    uint32_t seed;
    int nloc, ncut;
} fly__memo;

static uint32_t fly__memo_hash(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    a *= 0x9E3779B1u;
    b *= 0x85EBCA77u;
    a ^= a >> 15;
    b ^= b >> 13;
    a = (a + b) * 0xC2B2AE35u;
    return (a ^ (a >> 16)) & (uint32_t)(FLY_MEMO_N - 1);
}

static float fly__memo_get(fly__memo *m, const fly_world *w, float x, float y,
                           float (*fn)(const fly_world *, float, float)) {
    fly__memo_slot *s;
    if (!m->slot) {
        m->slot = (fly__memo_slot *)calloc(FLY_MEMO_N, sizeof *m->slot);
        /* no table is a slow renderer, never a broken one */
        if (!m->slot) return fn(w, x, y);
        m->world = NULL;
    }
    if (w != m->world || w->seed != m->seed || w->nloc != m->nloc ||
        w->ncut != m->ncut) {
        memset(m->slot, 0, (size_t)FLY_MEMO_N * sizeof *m->slot);
        m->world = w;
        m->seed = w->seed;
        m->nloc = w->nloc;
        m->ncut = w->ncut;
    }
    s = &m->slot[fly__memo_hash(x, y)];
    if (s->used && s->x == x && s->y == y) return s->z;
    s->x = x;
    s->y = y;
    s->z = fn(w, x, y);
    s->used = 1;
    return s->z;
}

static float drawn_ground_raw(const fly_world *w, float x, float y) {
    return fly_world_ground(w, x, y) + ground_bias(w, x, y);
}

/* Where a thing laid on the ground has to sit to be on top of it. */
static float drawn_ground(const fly_world *w, float x, float y) {
    static fly__memo m;
    return fly__memo_get(&m, w, x, y, drawn_ground_raw);
}

/* And the plain height, for the things that merely stand on it. */
static float scatter_ground(const fly_world *w, float x, float y) {
    static fly__memo m;
    return fly__memo_get(&m, w, x, y, fly_world_ground);
}

/* --- the shadow a thing casts by standing there ---------------------------
 *
 * Every prop in a settlement was set down on unbroken grass with a hard seam
 * where it met it. That seam is the single loudest "pasted on" cue an outdoor
 * scene has, and it is not the shadow map's job: the map handles the sun, and
 * on an overcast noon — which is most of the gallery — there is barely a sun
 * to cast. What is missing is the *ambient* half. A wall standing on grass
 * occludes most of the sky from the grass at its foot, and none of it a few
 * metres out, and that gradient is there whatever the weather is doing.
 *
 * So: a ring of ground-following triangles round the footprint, in the ground's
 * own colour and lit by the ground's own shading, with the ambient term ramped
 * from dark at the wall to open sky at the outer edge. It is a decal and not a
 * material — sample the terrain surface at each vertex and it matches whatever
 * is underneath, grass or dirt or the litter under a wood, at any time of day.
 *
 * It is skipped while casting, because a shadow decal that casts a shadow is
 * an object; and it is skipped at range, because past the point where the
 * whole building is a dozen pixels the ring is smaller than a pixel and all it
 * can do is cost heightfield taps. */
static void skirt_vertex(const fly_game *g, const fly__view *v, const fly__env *e,
                         float x, float y, float ao, fly_v3 *p, fly_v3 *lit, fly_v3 *dark) {
    const fly_world *w = &g->world;
    fly_v3 nrm = terrain_normal(w, x, y, 14.0f);
    fly_v3 mat, alb, dir, vdir;
    float dist, px;
    p->x = x;
    p->y = y;
    /* the same seat a service track takes, and a finger higher, so a skirt
     * laid over a track reads as shading on the track rather than z-fighting
     * with it */
    p->z = drawn_ground(w, x, y) + 0.16f;
    dir = fly_v3sub(*p, v->pos);
    dist = fly_v3len(dir);
    px = e->pxscale / (dist + 1e-3f);
    alb = terrain_surface(w, x, y, p->z, 1.0f - nrm.z, px, e->ball, e->wet, &mat);
    vdir = fly_v3scale(dir, -1.0f / (dist + 1e-5f));
    *lit = apply_fog(e, terrain_light(e, alb, mat, nrm, vdir, cloud_shadow(e, *p), ao),
                     v->pos, dir, dist);
    *dark = apply_fog(e, terrain_light(e, alb, mat, nrm, vdir, 0.0f, ao),
                      v->pos, dir, dist);
}

/* `hx`,`hy` is the footprint's half-extent in its own yawed frame, `reach` how
 * far the darkening carries past it, and `depth` how much sky the wall takes
 * at the foot of it. A round prop passes hx == hy and gets a disc. */
static void prop_skirt(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g, fly_v3 c, float hx, float hy, float yaw,
                       float reach, float depth) {
    enum { SEG = 14 };
    fly_v3 ip[SEG + 1], op[SEG + 1], il[SEG + 1], id[SEG + 1], ol[SEG + 1], od[SEG + 1];
    float cy = cosf(yaw), sy = sinf(yaw);
    float span = (hx > hy ? hx : hy) + reach;
    int i;
    if (g_shadow_cast) return;
    /* Worth drawing at all: a skirt is a soft gradient a couple of metres wide,
     * and a couple of metres at four hundred is a pixel and a half. */
    if (!object_worth_drawing(v, rt, c, span * 1.4f)) return;
    if (e->pxscale * span / (fly_v3dist(c, v->pos) + 1e-3f) < 26.0f) return;
    for (i = 0; i <= SEG; ++i) {
        float a = (float)(i % SEG) * (2.0f * FLY_PI / (float)SEG);
        float ux = cosf(a), uy = sinf(a);
        /* the footprint's own boundary in direction u, then the same point
         * pushed out along u — a rectangle grows into a rounded rectangle,
         * which is what a soft shadow round a building looks like anyway */
        float tx = fabsf(ux) > 1e-4f ? hx / fabsf(ux) : 1e9f;
        float ty = fabsf(uy) > 1e-4f ? hy / fabsf(uy) : 1e9f;
        float t = tx < ty ? tx : ty;
        float lx = ux * t, ly = uy * t;
        fly_v3 in = yawp(c, lx, ly, 0.0f, cy, sy);
        fly_v3 ou = yawp(c, lx + ux * reach, ly + uy * reach, 0.0f, cy, sy);
        skirt_vertex(g, v, e, in.x, in.y, 1.0f - depth, &ip[i], &il[i], &id[i]);
        skirt_vertex(g, v, e, ou.x, ou.y, 1.0f, &op[i], &ol[i], &od[i]);
    }
    for (i = 0; i < SEG; ++i) {
        int j = i + 1;
        if (e->shadow && e->shadow->active) {
            raster_tri_shadowed(rt, v, e, ip[i], ip[j], op[j], il[i], il[j], ol[j],
                                id[i], id[j], od[j], 0.0f);
            raster_tri_shadowed(rt, v, e, ip[i], op[j], op[i], il[i], ol[j], ol[i],
                                id[i], od[j], od[i], 0.0f);
        } else {
            raster_tri_hdr(rt, v, ip[i], ip[j], op[j], il[i], il[j], ol[j]);
            raster_tri_hdr(rt, v, ip[i], op[j], op[i], il[i], ol[j], ol[i]);
        }
    }
}

/* The ground a footprint stands on: the two numbers everything seated on the
 * ground here needs, and neither of them is the height at the centre.
 *
 * They come off *different* surfaces on purpose, and getting that wrong is how
 * the first version of the kerbed slab still floated. `hi` is the drawn mesh —
 * the bilinear surface the terrain is actually rasterised as, which stands
 * above the heightfield over a concave patch (see `ground_bias`) — because a
 * deck has to clear what is drawn or it is buried by it. `lo` is the
 * heightfield itself, because the underside has to get *below* what is drawn,
 * and the bias is a positive number: subtracting a margin from a lifted
 * minimum left the low corner of every slab up to three quarters of a metre in
 * the air with its own shadow under it.
 *
 * Nine samples — corners, edge midpoints and the middle — since a cell of the
 * drawn mesh is smaller than most of these footprints and the extremes are
 * therefore not necessarily at the corners. */
static void footprint_ground(const fly_game *g, fly_v3 c, float hx, float hy, float yaw,
                             float *lo, float *hi) {
    static const float ox[9] = { 0.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 0.0f };
    static const float oy[9] = { 0.0f, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 1.0f };
    float cy = cosf(yaw), sy = sinf(yaw);
    float top = -1e9f, bot = 1e9f;
    int i;
    for (i = 0; i < 9; ++i) {
        fly_v3 q = yawp(c, ox[i] * hx, oy[i] * hy, 0.0f, cy, sy);
        float raw = fly_world_ground(&g->world, q.x, q.y);
        float h = raw + ground_bias(&g->world, q.x, q.y);
        if (h > top) top = h;
        if (raw < bot) bot = raw;
    }
    if (lo) *lo = bot;
    if (hi) *hi = top;
}

/* A slab of pavement: level on top, and carried down into the ground on every
 * side so that it is a slab rather than a tile lying on the grass.
 *
 * The deck stands on the highest drawn ground under its own footprint, because
 * `fly_site_footing` does the opposite on purpose — it seats a *building* on
 * the lowest ground under it and buries it a further hand's breadth, since a
 * buried corner is a foundation and a floating one is not. Run a 0.75 m slab
 * through the same rule and the whole slab is the buried corner: the apron's
 * hardstand vanished completely, and the service track running to it looked
 * like it stopped in a field ten metres short of the hangar.
 *
 * Standing it on the highest corner is only half the answer, and the missing
 * half is what made every hardstand in the gallery read as a sheet of card
 * dropped on a lawn: a fixed 0.95 m box seated on the *high* corner has its
 * underside in mid-air over the low one, and on ordinary settlement ground
 * that gap is most of a metre of daylight with a hard shadow under it. So the
 * box reaches from a hand below the lowest ground under the footprint up to
 * the deck: on the flat that is a kerb, and on a fall it is the retaining
 * edge that put the pavement there — the same thing the road's deck does.
 *
 * Returns the level of the finished surface, so whatever stands on the slab
 * stands on it rather than on the ground it covered. */
static float pave(fly_render_target *rt, const fly__view *v, const fly__env *e,
                  const fly_game *g, fly_v3 c, float hx, float hy, float yaw,
                  fly_v3 col) {
    float lo, top, cyaw = cosf(yaw), syaw = sinf(yaw);
    footprint_ground(g, c, hx, hy, yaw, &lo, &top);
    /* A slab on the side of a hill is a slab, not a viaduct. Past a couple of
       metres the free edge stops reading as a kerb and starts reading as a
       wall somebody built, and burying the low corner of a large hardstand by
       the difference is much the lesser of the two wrongs. */
    if (top - lo > 2.6f) lo = top - 2.6f;
    prop_skirt(rt, v, e, g, fly_v3mk(c.x, c.y, top), hx, hy, yaw, 3.8f, 0.40f);
    draw_box2(rt, v, e, fly_v3mk(c.x, c.y, lo - 0.45f), hx, hy, top + 0.55f - (lo - 0.45f),
              yaw, col, fly_v3scale(col, 1.2f), 0, 0);
    /* --- the wearing course ------------------------------------------------
     *
     * The slab's own roof is one flat quad, which is the right thing for the
     * structure and the wrong thing for the surface: a hardstand you are
     * standing on is a bay of concrete with sawn joints in it, laid at
     * different times, stained where things stand and park. None of that can
     * be said on two triangles.
     *
     * So the top is re-laid in bays, close in only: this is a second surface
     * over the first and pure overdraw, and a hardstand thirty pixels across
     * has nothing to say. The bays are four metres, which is what a sawn joint
     * spacing actually is, and the joint is drawn as the *gap* between bays
     * showing the slab beneath rather than as its own geometry — a joint that
     * needs triangles is a joint that will z-fight. */
    {
        float deck = top + 0.55f;
        float span = hx > hy ? hx : hy;
        float dist = fly_v3dist(fly_v3mk(c.x, c.y, deck), v->pos) + 1e-3f;
        if (!g_shadow_cast && e->pxscale * span / dist > 30.0f) {
            int nx = (int)(hx * 0.5f), ny = (int)(hy * 0.5f), i, j;
            if (nx < 1) nx = 1;
            if (nx > 8) nx = 8;
            if (ny < 1) ny = 1;
            if (ny > 8) ny = 8;
            for (j = 0; j < ny; ++j)
                for (i = 0; i < nx; ++i) {
                    float x0 = -hx + (float)i * (2.0f * hx / nx) + 0.09f;
                    float x1 = -hx + (float)(i + 1) * (2.0f * hx / nx) - 0.09f;
                    float y0 = -hy + (float)j * (2.0f * hy / ny) + 0.09f;
                    float y1 = -hy + (float)(j + 1) * (2.0f * hy / ny) - 0.09f;
                    fly_v3 q0 = yawp(fly_v3mk(c.x, c.y, deck), x0, y0, 0.012f, cyaw, syaw);
                    fly_v3 q1 = yawp(fly_v3mk(c.x, c.y, deck), x1, y0, 0.012f, cyaw, syaw);
                    fly_v3 q2 = yawp(fly_v3mk(c.x, c.y, deck), x1, y1, 0.012f, cyaw, syaw);
                    fly_v3 q3 = yawp(fly_v3mk(c.x, c.y, deck), x0, y1, 0.012f, cyaw, syaw);
                    tri_paved(rt, v, e, q0, q1, q2, col, 0.95f, 1.0f);
                    tri_paved(rt, v, e, q0, q2, q3, col, 0.95f, 1.0f);
                }
        }
    }
    return top + 0.55f;
}

/* A stain on pavement: oil under where a machine stands, fuel round a bowser,
 * rubber where tyres turn. Cheap and worth its cost — a spotless apron is the
 * one thing that says nobody works here, and a dark patch under the parked
 * truck says the opposite in eight triangles.
 *
 * Drawn as a ragged disc rather than a circle, because a spill is not round,
 * and dark rather than black: what it is is the same surface with a lower
 * albedo and a slicker finish, which is exactly what `pave_surface` already
 * describes. Skipped while casting — a stain is a decal, not an object. */
static void prop_stain(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 at, float r, uint32_t seed) {
    enum { SEG = 9 };
    fly__paving s;
    fly_v3 rim[SEG + 1];
    int i;
    if (g_shadow_cast) return;
    if (!object_worth_drawing(v, rt, at, r * 1.5f)) return;
    if (e->pxscale * r / (fly_v3dist(at, v->pos) + 1e-3f) < 6.0f) return;
    s = pave_surface(e, fly_v3mk(0.055f, 0.052f, 0.050f), at.x, at.y, 0.0f);
    s.spec = fly_lerpf(0.45f, 0.95f, e->wet);   /* oil is slick wet or dry */
    s.gloss = fly_lerpf(60.0f, 420.0f, e->wet);
    for (i = 0; i <= SEG; ++i) {
        float a = (float)(i % SEG) * (2.0f * FLY_PI / (float)SEG);
        float jr = r * (0.62f + 0.38f * (float)(fly_hash2(seed, i % SEG, 0) & 7u) / 7.0f);
        rim[i] = fly_v3add(at, fly_v3mk(cosf(a) * jr, sinf(a) * jr, 0.0f));
    }
    for (i = 0; i < SEG; ++i)
        tri_spec(rt, v, e, at, rim[i], rim[i + 1], s.col, 0.95f, s.spec, s.gloss);
}

/* The concrete a building stands on.
 *
 * A wall that meets grass in a straight line is a wall somebody photographed
 * and pasted in; what a building on a site actually has under it is a slab
 * wider than itself, standing proud of the ground on the low side and cut into
 * it on the high one. That is this: the footing goes from below the lowest
 * ground under its own (widened) footprint up to a hand above the seat
 * `fly_site_footing` chose, and the caller then stands the building on the
 * number it returns rather than on the dirt.
 *
 * `margin` is how far the slab oversails the walls. It is a parameter because
 * a hut wants a hand's breadth of it and a fuel tank wants a bund.
 *
 * The skirt comes with it, so no caller has to remember both. */
static float prop_footing(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          const fly_game *g, fly_v3 c, float hx, float hy, float yaw,
                          float margin) {
    fly_v3 conc = fly_v3mk(0.42f, 0.41f, 0.39f);
    float ex = hx + margin, ey = hy + margin;
    float lo, hi, top;
    footprint_ground(g, c, ex, ey, yaw, &lo, &hi);
    /* The seat is what the caller was given and is already at or below the
       lowest ground under the walls; the slab has to clear the widened
       footprint too, and stand a hand above whichever is higher. */
    top = (c.z > hi ? c.z : hi) + 0.28f;
    if (top - lo > 7.0f) lo = top - 7.0f;   /* a plinth, not a retaining wall */
    prop_skirt(rt, v, e, g, fly_v3mk(c.x, c.y, hi), ex, ey, yaw, 4.2f, 0.60f);
    draw_box2(rt, v, e, fly_v3mk(c.x, c.y, lo - 0.50f), ex, ey, top - (lo - 0.50f), yaw,
              conc, fly_v3scale(conc, 1.14f), 0, 0);
    return top;
}

/* A graded track between two places at the site.
 *
 * The cheapest thing on this list and close to the most valuable. Buildings
 * standing on unbroken grass read as objects set down on a field however good
 * each one is; what makes them a settlement is that the ground between them has
 * been used. A ribbon of gravel following the terrain does that, and it is also
 * the only piece here that says which parts of a site talk to which.
 *
 * Cross-sections every ten metres, each vertex on the heightfield plus the
 * cross-section's own bias, lifted a hand's breadth. Ten metres is not a
 * quality compromise now that the bias is right: the surface being followed is
 * linear across a twenty-six metre cell, so more stations buy nothing and each
 * one costs nine heightfield taps. Both ends overrun by four metres into what
 * they serve — a track that stops at a wall reads as unfinished, and the wall
 * hides the overlap.
 *
 * The surface is the caller's: gravel between the sheds, and the road's own
 * black where the way is the last thirty metres of a carriageway. */
static void prop_track(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g, fly_v3 a, fly_v3 b, float wide, fly_v3 surface) {
    enum { STA = 26 };
    fly_v3 left[STA], right[STA];
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = hypotf(dx, dy), nx, ny, ex, ey;
    int i, n;
    if (len < 8.0f) return;
    nx = -dy / len * wide;
    ny = dx / len * wide;
    ex = dx / len * 4.0f;
    ey = dy / len * 4.0f;
    a = fly_v3mk(a.x - ex, a.y - ey, a.z);
    dx += ex * 2.0f;
    dy += ey * 2.0f;
    len += 8.0f;
    n = (int)(len / 10.0f);
    if (n < 2) n = 2;
    if (n > STA - 1) n = STA - 1;
    for (i = 0; i <= n; ++i) {
        float f = (float)i / (float)n;
        float x = a.x + dx * f, y = a.y + dy * f;
        float bias = ground_bias(&g->world, x, y) + 0.12f;
        left[i] = fly_v3mk(x - nx, y - ny,
                           fly_world_ground(&g->world, x - nx, y - ny) + bias);
        right[i] = fly_v3mk(x + nx, y + ny,
                            fly_world_ground(&g->world, x + nx, y + ny) + bias);
        /* bias is the cross-section's, not the vertex's: it is a smooth field
           and nine taps per vertex measured +7.3% against +5.7% for this */
    }
    for (i = 0; i < n; ++i) {
        tri_shaded(rt, v, e, left[i], right[i], right[i + 1], surface, 0.92f);
        tri_shaded(rt, v, e, left[i], right[i + 1], left[i + 1], surface, 0.92f);
    }
}

/* What a service track is surfaced with when nobody says otherwise. */
#define FLY_TRACK_GRAVEL fly_v3mk(0.23f, 0.21f, 0.185f)

/* --- lighting a way ------------------------------------------------------
 *
 * Where a lamp goes is not a decoration question, it is the whole of whether a
 * settlement reads as built. A lamp standing in open grass a hundred metres
 * from anything is litter — nobody digs a foundation and runs a cable out into
 * a field to light nothing — and a ring of six of them around a pad was
 * exactly that: the single loudest "scattered" cue in the gallery.
 *
 * A lamp lights something. So there is one way to place one here: give this
 * function the two ends of a way — a service track, the edge of a hardstand,
 * the run in from the road — and it puts a column every `pitch` metres down
 * the *side* of it, alternating so the cranked arms reach in over the surface
 * from both edges the way street lighting does. Nothing else in a settlement
 * places a lamp any more.
 *
 * Seated on the drawn ground rather than through `fly_site_footing`: the way
 * itself has already been sited, so a column standing beside it is on ground
 * something else vouched for, and a lamp that walked forty metres to find dry
 * land would no longer be beside the way it is lighting. Below the waterline
 * it is simply not installed. */
static void prop_way_lights(fly_render_target *rt, const fly__view *v, const fly__env *e,
                            const fly_game *g, fly_v3 a, fly_v3 b, float off, float pitch) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = hypotf(dx, dy), ux, uy;
    int i, n;
    if (len < 12.0f || pitch < 4.0f) return;
    ux = dx / len;
    uy = dy / len;
    n = (int)(len / pitch);
    if (n > 5) n = 5;   /* a run of six columns is a lit way; more is a fence of them */
    for (i = 0; i <= n; ++i) {
        /* Half a pitch in from each end, so a lamp never lands on top of
           whatever the way runs into. */
        float s = (len - (float)n * pitch) * 0.5f + (float)i * pitch;
        float side = (i & 1) ? off : -off;
        float x = a.x + ux * s - uy * side, y = a.y + uy * s + ux * side;
        float gz = fly_world_ground(&g->world, x, y);
        if (gz <= FLY_WATER_LEVEL) continue;
        /* the arm cranks in over the way, so the yaw is across it */
        prop_lamp(rt, v, e, fly_v3mk(x, y, drawn_ground(&g->world, x, y) - 0.12f),
                  atan2f(uy, ux) + ((i & 1) ? -FLY_PI * 0.5f : FLY_PI * 0.5f));
    }
}

/* A service way: the track, and the lighting that says it is used after dark.
 * One call, because a track and its lamps are the same decision and the two
 * drifting apart is how lamps ended up in fields. */
static void prop_way(fly_render_target *rt, const fly__view *v, const fly__env *e,
                     const fly_game *g, fly_v3 a, fly_v3 b, float wide, fly_v3 surface,
                     int lit) {
    prop_track(rt, v, e, g, a, b, wide, surface);
    if (lit) prop_way_lights(rt, v, e, g, a, b, wide + 1.9f, 52.0f);
}

/* A wheel: a barrel about the axle, with a paler hub set into it.
 *
 * These were boxes, and the reason they were boxes is in the note that used to
 * sit over prop_truck — "small enough that the wheels are boxes and nobody is
 * the wiser". That was true while the only place a truck was ever seen was from
 * an aeroplane. It stopped being true the moment there was a walking camera and
 * a road with a column coming down it: at four metres a cube is a cube, and a
 * vehicle standing on four of them is a toy. A barrel is ten triangles more and
 * it is the difference. */
static void prop_wheel(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 c, float yaw, float rad, float half, int fine) {
    fly_v3 tyre = fly_v3mk(0.075f, 0.072f, 0.08f);
    fly_v3 hub = fly_v3mk(0.30f, 0.30f, 0.31f);
    fly_v3 ax = fly_v3mk(-sinf(yaw), cosf(yaw), 0.0f);
    fly_v3 pts[2];
    float r[2];
    pts[0] = fly_v3sub(c, fly_v3scale(ax, half));
    pts[1] = fly_v3add(c, fly_v3scale(ax, half));
    r[0] = r[1] = rad;
    draw_pipeline(rt, v, e, pts, r, 2, fine ? 12 : 7, tyre);
    if (!fine) return;
    /* the hub, standing a little proud of the tyre so the disc reads as a
       wheel rather than as the flat end of a drum */
    pts[0] = fly_v3sub(c, fly_v3scale(ax, half * 1.06f));
    pts[1] = fly_v3add(c, fly_v3scale(ax, half * 1.06f));
    r[0] = r[1] = rad * 0.5f;
    draw_pipeline(rt, v, e, pts, r, 2, 8, hub);
}

/* --- a truck --------------------------------------------------------------
 *
 * The yard vehicle, and the thing a convoy is made of. It was five boxes: four
 * for the wheels, one for the bed and two for the cab and whatever was behind
 * it. That reads from the air, which is where it used to be seen, and it falls
 * apart everywhere else — the road frames put a column a dozen metres from the
 * camera and there is nothing there but rectangles.
 *
 * What a truck is made of, and roughly in the order the eye picks it up: a
 * chassis with wheels under it, a flat deck with drop-sides, a cab with glass
 * in it, and then the things that say which end is the front — grille, bumper,
 * lamps, a stack, mirrors. Each of those is one or two boxes and between them
 * they are the whole silhouette.
 *
 * Three ranges. Everything is drawn close in; the trim goes first, then the
 * bodywork detail, and what is left past four hundred metres is the same five
 * boxes it always was, which is all that survives the projection anyway. */
static void prop_truck(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 p, float yaw, fly_v3 col, int lit) {
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 bed = fly_v3scale(col, 0.55f);
    fly_v3 steel = fly_v3mk(0.30f, 0.30f, 0.32f);
    fly_v3 dark = fly_v3mk(0.13f, 0.13f, 0.14f);
    fly_v3 glass = fly_v3mk(0.06f, 0.085f, 0.11f);
    float dist = fly_v3dist(p, v->pos);
    int fine = dist < 130.0f, mid = dist < 420.0f;
    int w;
    /* the running gear */
    for (w = 0; w < 4; ++w)
        prop_wheel(rt, v, e, yawp(p, (w & 1) ? 1.85f : -1.70f, (w & 2) ? 1.00f : -1.00f,
                                  0.50f, cy, sy), yaw, 0.50f, 0.21f, fine);
    if (mid)   /* the chassis the deck is bolted to */
        for (w = 0; w < 2; ++w)
            draw_box2(rt, v, e, yawp(p, 0.05f, w ? 0.62f : -0.62f, 0.70f, cy, sy),
                      2.60f, 0.10f, 0.20f, yaw, dark, steel, 0, 0);
    /* the deck */
    draw_box2(rt, v, e, yawp(p, 0.0f, 0, 0.90f, cy, sy), 2.7f, 1.05f, 0.30f, yaw, bed,
              fly_v3scale(bed, 1.15f), 0, 0);
    if (mid) {   /* drop-sides and a tailgate, which is what makes it a flatbed */
        for (w = 0; w < 2; ++w)
            draw_box2(rt, v, e, yawp(p, 0.0f, w ? 1.00f : -1.00f, 1.20f, cy, sy),
                      2.68f, 0.07f, 0.40f, yaw, bed, fly_v3scale(bed, 1.2f), 0, 0);
        draw_box2(rt, v, e, yawp(p, -2.64f, 0, 1.20f, cy, sy), 0.07f, 1.02f, 0.40f, yaw,
                  bed, fly_v3scale(bed, 1.2f), 0, 0);
        /* mudguards over the back axle */
        for (w = 0; w < 2; ++w)
            draw_box2(rt, v, e, yawp(p, -1.70f, w ? 1.00f : -1.00f, 1.02f, cy, sy),
                      0.66f, 0.26f, 0.09f, yaw, dark, steel, 0, 0);
    }
    /* the cab */
    draw_box2(rt, v, e, yawp(p, 1.55f, 0, 1.20f, cy, sy), 1.05f, 0.98f, 1.25f, yaw,
              col, fly_v3scale(col, 0.8f), 0, 0);
    if (mid) {
        /* Glass, as a band right round the cab: a windscreen, two doors and a
           back light in one box, proud of the panel by a centimetre so it wins
           the depth test from every angle a truck is looked at from. */
        draw_box2(rt, v, e, yawp(p, 1.58f, 0, 1.92f, cy, sy), 1.00f, 1.00f, 0.42f, yaw,
                  glass, fly_v3scale(glass, 1.3f), 0, 0);
        /* a roof cap, so the cab has a top edge rather than a cut */
        draw_box2(rt, v, e, yawp(p, 1.52f, 0, 2.45f, cy, sy), 0.98f, 0.92f, 0.11f, yaw,
                  fly_v3scale(col, 0.75f), fly_v3scale(col, 0.9f), 0, 0);
        /* grille, bumper and the housings the lamps sit in */
        draw_box2(rt, v, e, yawp(p, 2.58f, 0, 1.32f, cy, sy), 0.07f, 0.80f, 0.72f, yaw,
                  dark, steel, 0, 0);
        draw_box2(rt, v, e, yawp(p, 2.62f, 0, 0.92f, cy, sy), 0.11f, 1.02f, 0.24f, yaw,
                  steel, fly_v3scale(steel, 1.2f), 0, 0);
        for (w = 0; w < 2; ++w)
            draw_box2(rt, v, e, yawp(p, 2.56f, w ? 0.68f : -0.68f, 1.22f, cy, sy),
                      0.08f, 0.20f, 0.26f, yaw, fly_v3mk(0.70f, 0.70f, 0.66f),
                      fly_v3mk(0.80f, 0.80f, 0.75f), 0, 0);
    }
    if (fine) {
        fly_v3 pts[2];
        float r[2];
        /* the stack, and the tank it feeds from */
        pts[0] = yawp(p, 0.62f, 1.02f, 1.20f, cy, sy);
        pts[1] = yawp(p, 0.62f, 1.02f, 2.95f, cy, sy);
        r[0] = 0.085f;
        r[1] = 0.075f;
        draw_pipeline(rt, v, e, pts, r, 2, 7, fly_v3mk(0.24f, 0.23f, 0.22f));
        pts[0] = yawp(p, -0.30f, -1.06f, 0.82f, cy, sy);
        pts[1] = yawp(p, 0.85f, -1.06f, 0.82f, cy, sy);
        r[0] = r[1] = 0.23f;
        draw_pipeline(rt, v, e, pts, r, 2, 8, fly_v3mk(0.42f, 0.43f, 0.44f));
        /* mirrors: an arm and a glass, one each side of the screen */
        for (w = 0; w < 2; ++w) {
            float side = w ? 1.12f : -1.12f;
            draw_box2(rt, v, e, yawp(p, 2.30f, side, 1.98f, cy, sy), 0.05f, 0.05f, 0.52f,
                      yaw, dark, dark, 0, 0);
            draw_box2(rt, v, e, yawp(p, 2.30f, side, 2.16f, cy, sy), 0.06f, 0.17f, 0.30f,
                      yaw, dark, steel, 0, 0);
        }
        /* a step under each door, which is the detail that says people get in */
        for (w = 0; w < 2; ++w)
            draw_box2(rt, v, e, yawp(p, 1.55f, w ? 1.02f : -1.02f, 0.62f, cy, sy),
                      0.42f, 0.10f, 0.07f, yaw, steel, fly_v3scale(steel, 1.2f), 0, 0);
    }
    if (lit) {
        /* What the headlamps land on. Two splats at the front of a cab say
         * "there is a vehicle there" and nothing else; what says a truck is
         * *driving* is the patch of road in front of it moving with it. Along
         * the way it is going, a dozen metres out, and dim enough to lose
         * against a lamp-lit stretch and to carry a dark one.
         *
         * A lantern hung over the road ahead of the cab rather than a beam out
         * of it. A dipped headlamp aims at the surface a dozen metres on and
         * everything the eye reads off it is *there* — the patch, its shape and
         * how it slides over a hump — so a source over the patch says the same
         * thing as a cone out of the lamp, in one term instead of a second
         * luminaire model that exists to be pointed sideways. */
        if (fly_v3dist(p, v->pos) < 1400.0f)
            lamp_light(rt, v, e, yawp(p, 10.5f, 0.0f, 4.6f, cy, sy), yaw,
                       4.6f, 17.0f, fly_v3mk(0.98f, 0.93f, 0.80f), 0.16f);
        draw_light(rt, v, yawp(p, 2.62f, 0.68f, 1.34f, cy, sy), FLY_RGBA(255, 240, 210, 220), 0.55f);
        draw_light(rt, v, yawp(p, 2.62f, -0.68f, 1.34f, cy, sy), FLY_RGBA(255, 240, 210, 220), 0.55f);
        draw_light(rt, v, yawp(p, -2.68f, 0.80f, 1.30f, cy, sy), FLY_RGBA(255, 60, 40, 190), 0.42f);
        draw_light(rt, v, yawp(p, -2.68f, -0.80f, 1.30f, cy, sy), FLY_RGBA(255, 60, 40, 190), 0.42f);
        if (mid)   /* marker lamps along the top of the cab */
            for (w = -1; w <= 1; ++w)
                draw_light(rt, v, yawp(p, 1.52f, (float)w * 0.62f, 2.60f, cy, sy),
                           FLY_RGBA(255, 196, 120, 170), 0.30f);
    }
}

/* A handrail along an edge: uprights and a top rail. What a deck is missing
 * when it reads as a slab with nothing to stop you walking off it. */
static void prop_railing(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         fly_v3 a, fly_v3 b) {
    fly_v3 d = fly_v3sub(b, a);
    float len = hypotf(d.x, d.y), yaw = atan2f(d.y, d.x);
    fly_v3 col = fly_v3mk(0.30f, 0.31f, 0.34f);
    int n = (int)(len / 3.2f), i;
    if (n < 2) return;
    if (n > 14) n = 14;
    for (i = 0; i <= n; ++i)
        draw_box2(rt, v, e, fly_v3lerp(a, b, (float)i / (float)n), 0.07f, 0.07f, 1.1f,
                  yaw, col, col, 0, 0);
    draw_box2(rt, v, e, fly_v3add(fly_v3scale(fly_v3add(a, b), 0.5f), fly_v3mk(0, 0, 1.05f)),
              len * 0.5f, 0.055f, 0.09f, yaw, col, col, 0, 0);
}

/* Steam or smoke leaving a stack: a stack of translucent quads that widen,
 * lean downwind and thin out with height. It goes through the blend list the
 * propeller uses, so it composites against the finished depth buffer and is
 * drawn by neither rasterizer — which is also why it costs nothing on the GPU
 * path beyond the triangles themselves. */
static void prop_plume(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 base, float r0, float rise, fly_v3 tint, float density) {
    int i, n = 7;
    float t = (float)e->time * 0.35f;
    (void)rt;
    for (i = 0; i < n; ++i) {
        float f = ((float)i + fmodf(t, 1.0f)) / (float)n;
        float rr = r0 * (1.0f + f * 3.4f);
        float z = base.z + f * rise;
        float drift = f * f * rise * 0.55f;
        float a = density * (1.0f - f) * (1.0f - f) * 0.55f;
        float wob = sinf(t * 1.7f + (float)i) * rr * 0.25f;
        fly_v3 c = fly_v3mk(base.x + e->wind.x * drift * 0.06f + wob,
                            base.y + e->wind.y * drift * 0.06f, z);
        int seg;
        if (a < 0.006f) continue;
        for (seg = 0; seg < 6; ++seg) {
            float a0 = (float)seg / 6.0f * 2.0f * FLY_PI + f * 1.3f;
            float a1 = (float)(seg + 1) / 6.0f * 2.0f * FLY_PI + f * 1.3f;
            fly_v3 q0 = fly_v3mk(c.x + cosf(a0) * rr, c.y + sinf(a0) * rr, c.z);
            fly_v3 q1 = fly_v3mk(c.x + cosf(a1) * rr, c.y + sinf(a1) * rr, c.z);
            blend_shaded(v, e, c, q0, q1, tint, a, a * 0.35f, a * 0.35f);
        }
    }
}

/* A shipping container: the unit of measurement for an industrial yard, and
 * the one prop big enough to read from the air. Ribbed sides, a darker end with
 * door furniture, and a slightly proud roof. Stacks. */
static void prop_container(fly_render_target *rt, const fly__view *v, const fly__env *e,
                           fly_v3 p, float yaw, fly_v3 col, int ribs) {
    float cy = cosf(yaw), sy = sinf(yaw);
    int i;
    draw_box2(rt, v, e, p, 3.05f, 1.22f, 2.60f, yaw, col, fly_v3scale(col, 1.15f), 0, 0);
    draw_box2(rt, v, e, yawp(p, 3.06f, 0, 0, cy, sy), 0.06f, 1.24f, 2.60f, yaw,
              fly_v3scale(col, 0.62f), fly_v3scale(col, 0.62f), 0, 0);
    if (!ribs) return;
    for (i = 0; i < 7; ++i) {   /* corrugation, as a few proud strips */
        float u = -2.6f + (float)i * 0.87f;
        draw_box2(rt, v, e, yawp(p, u, 0, 0.15f, cy, sy), 0.07f, 1.26f, 2.30f, yaw,
                  fly_v3scale(col, 0.80f), fly_v3scale(col, 0.80f), 0, 0);
    }
}

/* An open-fronted shed: the building a yard is actually for. Two side walls, a
 * back wall, a shallow gabled roof on trusses, and a dark opening across the
 * front — the opening is what makes it a building you could taxi into rather
 * than a closed box. */
static void prop_shed(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 p, float hx, float hy, float h, float yaw, fly_v3 col) {
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 dark = fly_v3scale(col, 0.30f);
    fly_v3 roof = fly_v3scale(col, 0.72f);
    int i;
    draw_box2(rt, v, e, yawp(p, 0, -hy, 0, cy, sy), hx, 0.35f, h, yaw, col, roof, 0, 0);
    draw_box2(rt, v, e, yawp(p, 0, hy, 0, cy, sy), hx, 0.35f, h, yaw, col, roof, 0, 0);
    draw_box2(rt, v, e, yawp(p, -hx, 0, 0, cy, sy), 0.35f, hy, h, yaw, col, roof, 0, 0);
    /* the ridge, as two pitched slabs, and a lintel over the opening */
    {
        fly_v3 a0 = yawp(p, -hx, -hy, h, cy, sy), a1 = yawp(p, hx, -hy, h, cy, sy);
        fly_v3 b0 = yawp(p, -hx, hy, h, cy, sy), b1 = yawp(p, hx, hy, h, cy, sy);
        fly_v3 r0 = yawp(p, -hx, 0, h + hy * 0.34f, cy, sy);
        fly_v3 r1 = yawp(p, hx, 0, h + hy * 0.34f, cy, sy);
        tri_shaded(rt, v, e, a0, a1, r1, roof, 1.0f);
        tri_shaded(rt, v, e, a0, r1, r0, roof, 1.0f);
        tri_shaded(rt, v, e, b0, r0, r1, fly_v3scale(roof, 0.86f), 1.0f);
        tri_shaded(rt, v, e, b0, r1, b1, fly_v3scale(roof, 0.86f), 1.0f);
        tri_shaded(rt, v, e, a0, r0, b0, fly_v3scale(col, 0.9f), 1.0f);
    }
    draw_box2(rt, v, e, yawp(p, hx - 0.3f, 0, h - 0.7f, cy, sy), 0.3f, hy, 0.7f, yaw,
              fly_v3scale(col, 0.55f), roof, 0, 0);
    /* the inside of the opening: dark, so it reads as depth rather than a face */
    draw_box2(rt, v, e, yawp(p, hx - 1.4f, 0, 0, cy, sy), 0.2f, hy * 0.94f, h - 0.9f, yaw,
              dark, dark, 0, 0);
    for (i = 0; i < 3; ++i)     /* roof trusses showing through the opening */
        draw_box2(rt, v, e, yawp(p, -hx + (float)(i + 1) * (hx * 0.5f), 0, h - 0.25f, cy, sy),
                  0.14f, hy, 0.2f, yaw, fly_v3scale(col, 0.5f), fly_v3scale(col, 0.5f), 0, 0);
}

/* A gantry: two legs, a girder across them and a hoist trolley that is at a
 * different place along the span each time you look. */
static void prop_gantry(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        fly_v3 p, float span, float h, float yaw, double time) {
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 steel = fly_v3mk(0.40f, 0.34f, 0.16f);
    float u = (float)sin(time * 0.13) * (span - 4.0f);
    int i;
    for (i = 0; i < 2; ++i) {
        float sx = i ? span : -span;
        draw_box2(rt, v, e, yawp(p, sx, -1.2f, 0, cy, sy), 0.5f, 0.5f, h, yaw, steel, steel, 0, 0);
        draw_box2(rt, v, e, yawp(p, sx, 1.2f, 0, cy, sy), 0.5f, 0.5f, h, yaw, steel, steel, 0, 0);
        draw_box2(rt, v, e, yawp(p, sx, 0, h * 0.55f, cy, sy), 0.22f, 1.6f, 0.22f, yaw,
                  steel, steel, 0, 0);
    }
    draw_box2(rt, v, e, yawp(p, 0, 0, h, cy, sy), span + 0.6f, 0.9f, 1.1f, yaw,
              steel, fly_v3scale(steel, 1.2f), 0, 0);
    draw_box2(rt, v, e, yawp(p, u, 0, h - 1.1f, cy, sy), 1.1f, 1.0f, 1.0f, yaw,
              fly_v3mk(0.28f, 0.28f, 0.30f), fly_v3mk(0.34f, 0.34f, 0.36f), 0, 0);
    draw_box2(rt, v, e, yawp(p, u, 0, h * 0.5f - 1.1f, cy, sy), 0.08f, 0.08f, h * 0.5f, yaw,
              fly_v3mk(0.18f, 0.18f, 0.18f), fly_v3mk(0.18f, 0.18f, 0.18f), 0, 0);
}

/* Find room for a group of pieces at a bearing off the site axis.
 *
 * `fly_site_footing` is the rule every building uses, and it has one property
 * that matters more for a group than for a box: it *walks* a plot to find
 * ground, up to `FLY_SITE_WALK`, and its keep-clear test then applies to the
 * walked centre only. A yard asked for at 132 m came back at 92 — legal by that
 * test, and with its far corner standing on the landing deck, which the pad's
 * own roundness gate caught. So the caller states how far its pieces reach back
 * toward the site, and a candidate whose reach crosses the deck's keep-clear is
 * refused rather than accepted at its centre.
 *
 * Bearings are tried in order and the first that fits wins, so the list is a
 * preference: the layout a site gets on open ground, then the fallbacks for one
 * hemmed in by the rail or a coast. A site with no room for a group does not
 * get one. */
static int site_plot(const fly_game *g, fly_v3 site, float axis, const float *turn,
                     int nturn, float r, float foot, float reach, fly_v3 *out,
                     float *yaw) {
    int i;
    for (i = 0; i < nturn; ++i) {
        float a2 = axis + turn[i];
        fly_v3 c = fly_v3add(site, fly_v3mk(cosf(a2) * r, sinf(a2) * r, 0));
        if (!fly_site_footing(g, site, foot, &c)) continue;
        if (hypotf(c.x - site.x, c.y - site.y) - reach < FLY_PAD_DECK_R + 18.0f) continue;
        *out = c;
        *yaw = a2 + FLY_PI;   /* the group turns to face the pad wherever it lands */
        return 1;
    }
    return 0;
}

/* The yard: a fenced compound with a gate facing the pad, lit at the corners,
 * with pallets and drums stacked inside it and a truck parked by the gate.
 *
 * This is the piece that makes a site read as laid out rather than scattered.
 * Everything in it is positioned in the yard's own frame and then rotated onto
 * the site axis, so the gate faces the apron at every site and the rows of
 * stock run parallel to the fence instead of at whatever angle a global
 * offset happened to give them. */
static int draw_yard(fly_render_target *rt, const fly__view *v, const fly__env *e,
                     const fly_game *g, fly_v3 site, float axis, uint32_t seed,
                     fly_v3 stock, int detail, fly_v3 *out) {
    /* Half extents. Smaller than the first version: the compound's own
     * half-diagonal is what has to clear the deck, so a 58x42 yard could not
     * stand closer than 145 m and was simply out of sight from the pad. At
     * 46x34 it clears at 112 and is part of the same view as the apron. */
    float hw = 46.0f, hh = 34.0f;
    /* Its own stream, not the caller's. The yard draws a few dozen numbers and
     * the per-kind code below it draws from the same generator, so sharing one
     * would re-roll every building in the world the first time this function
     * changed how many numbers it wants. */
    fly_rng rngv, *rng = &rngv;
    fly_v3 c;
    fly_v3 corner[4];
    float cy, sy, yaw = axis, yard_slab;
    int i;
    static const float turn[8] = { 2.30f, -2.30f, 1.55f, -1.55f, 2.95f, -2.95f,
                                   0.85f, -0.85f };
    fly_rng_seed(rng, seed);
    /* Fifty metres of clear ground, at whichever bearing has it. The reach is
     * the compound's own half-diagonal: a corner post is as much on the deck as
     * a wall would be. */
    if (!site_plot(g, site, axis, turn, 8, 146.0f, 44.0f, hypotf(hw, hh), &c, &yaw))
        return 0;
    cy = cosf(yaw);
    sy = sinf(yaw);
    for (i = 0; i < 4; ++i)
        corner[i] = yawp(c, (i == 0 || i == 3) ? -hw : hw, (i < 2) ? -hh : hh, 0, cy, sy);
    /* what the track outside should aim at: the gate, not the middle of the
       compound, which is behind a fence from every direction */
    *out = yawp(c, hw, 0.0f, 0.0f, cy, sy);
    /* Local +x points back at the pad, so the run between corners 1 and 2 is
     * the one the approach faces and the gate belongs in that one. The first
     * version put it in the run at local -y, which is a side wall, and left the
     * service track arriving at a fence. */
    prop_fence(rt, v, e, corner[0], corner[1], -1.0f, 0.0f);
    prop_fence(rt, v, e, corner[1], corner[2], 0.5f, 7.0f);
    prop_fence(rt, v, e, corner[2], corner[3], -1.0f, 0.0f);
    prop_fence(rt, v, e, corner[3], corner[0], -1.0f, 0.0f);
    for (i = 0; i < 4; ++i)
        prop_lamp(rt, v, e, corner[i],
                  yaw + (float)i * (FLY_PI * 0.5f) + FLY_PI * 0.75f);
    /* The mass, before the clutter: a shed along the back fence, a gantry
     * beside it, and container stacks in rows. These are what carry at three
     * hundred metres — a crate does not, and a yard of nothing but crates is a
     * texture rather than a place.
     *
     * All of it on hardstand. A container yard is the one place on a site that
     * cannot be grass — nothing that weighs thirty tonnes is set down on turf
     * — and the slab is also what turns nine boxes at a regular spacing into a
     * stacking area with an edge to it. */
    yard_slab = pave(rt, v, e, g, yawp(c, hw - 26.0f, -hh + 13.0f, 0.0f, cy, sy),
                     18.0f, 12.0f, yaw, fly_v3mk(0.26f, 0.26f, 0.26f));
    {
        fly_v3 gp = yawp(c, hw - 18.0f, hh - 15.0f, 0, cy, sy);
        gp.z = prop_footing(rt, v, e, g, gp, 13.0f, 11.0f, yaw, 1.6f);
        prop_gantry(rt, v, e, gp, 13.0f, 11.0f, yaw, e->time);
    }
    for (i = 0; i < 9; ++i) {
        int row = i / 3, colx = i % 3;
        fly_v3 q = yawp(c, hw - 34.0f + (float)colx * 8.0f, -hh + 10.0f + (float)row * 6.4f,
                        0, cy, sy);
        int stack = 1 + (int)(fly_rng_f01(rng) * 2.4f), k2;
        static const float hue[4][3] = { { 0.32f, 0.16f, 0.12f }, { 0.16f, 0.26f, 0.30f },
                                         { 0.30f, 0.28f, 0.14f }, { 0.22f, 0.20f, 0.22f } };
        const float *cc = hue[(int)(fly_rng_f01(rng) * 3.99f) & 3];
        q.z = yard_slab;
        for (k2 = 0; k2 < stack; ++k2)
            prop_container(rt, v, e, fly_v3add(q, fly_v3mk(0, 0, (float)k2 * 2.64f)),
                           yaw + FLY_PI * 0.5f, fly_v3mk(cc[0], cc[1], cc[2]), detail >= 2);
    }
    if (detail < 2) return 1;
    /* stock in rows along the fence, jittered but never through it */
    for (i = 0; i < 14; ++i) {
        float row = -hh + 7.0f + (float)(i / 7) * 12.0f;
        float col = -hw + 5.0f + (float)(i % 7) * 6.0f;
        fly_v3 q = yawp(c, col + fly_rng_span(rng, -1.6f, 1.6f),
                        row + fly_rng_span(rng, -1.4f, 1.4f), 0, cy, sy);
        float ry = yaw + fly_rng_span(rng, -0.18f, 0.18f);
        float roll = fly_rng_f01(rng);
        q.z = c.z;
        if (roll < 0.42f) {
            int stack = 1 + (int)(fly_rng_f01(rng) * 2.99f), k2;
            for (k2 = 0; k2 < stack; ++k2)
                prop_crate(rt, v, e, fly_v3add(q, fly_v3mk(0, 0, (float)k2 * 1.32f)),
                           1.5f, 1.1f, 1.2f, ry,
                           fly_v3scale(stock, 0.85f + 0.3f * fly_rng_f01(rng)));
        } else if (roll < 0.72f) {
            int d2, nd = 2 + (int)(fly_rng_f01(rng) * 3.99f);
            for (d2 = 0; d2 < nd; ++d2)
                prop_drum(rt, v, e,
                          fly_v3add(q, fly_v3mk((float)(d2 % 3) * 0.95f,
                                                (float)(d2 / 3) * 0.95f, 0)),
                          fly_v3mk(0.30f, 0.22f, 0.12f));
        } else if (roll < 0.80f) {
            prop_crate(rt, v, e, q, 3.4f, 1.3f, 2.6f, ry, fly_v3mk(0.26f, 0.28f, 0.30f));
        }
    }
    /* one parked, one working: the second is placed by the clock in
     * draw_site_traffic below */
    prop_truck(rt, v, e, yawp(c, -hw + 11.0f, -hh + 5.0f, 0, cy, sy), yaw + 1.6f,
               fly_v3mk(0.42f, 0.30f, 0.14f), e->day < 0.35f);
    return 1;
}

/* The apron: what stands immediately off the landing deck, at every site.
 *
 * The yard is the storage; this is the working end, and it is the part a pilot
 * ever sees close up. A hangar facing the pad with a concrete hardstand run out
 * toward it, the fuel installation beside it, masts along the edge, and
 * something parked. Placed just outside the deck's keep-clear and turned to
 * face the pad, so the opening looks at the aircraft rather than away from it.
 *
 * The hardstand does more work than it looks: a slab of pavement joining two
 * built things is what stops them reading as objects dropped on grass. */
static int draw_apron(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly_game *g, fly_v3 site, float axis, int detail,
                      fly_v3 *out) {
    static const float turn[6] = { 1.95f, -1.95f, 2.60f, -2.60f, 1.30f, -1.30f };
    fly_v3 c, hangar;
    float yaw, cy, sy, hard, bund;
    int i;
    /* The hardstand runs 37 m out of the hangar mouth toward the deck and the
     * mast line runs with it: that, not the hangar, is what has to clear. */
    if (!site_plot(g, site, axis, turn, 6, 128.0f, 26.0f, 40.0f, &c, &yaw)) return 0;
    *out = c;
    cy = cosf(yaw);
    sy = sinf(yaw);
    /* Pavement, run from the hangar mouth back toward the deck, and the hangar
     * itself on the slab that carries it. `pave` and `prop_footing` both hand
     * back the level of the surface they built, so what stands on concrete is
     * seated on the concrete rather than on the grass it covered — and `c.z`
     * stays what the survey gave it, so the loose clutter that is *not* on a
     * slab is not lifted along with the buildings. */
    hard = pave(rt, v, e, g, yawp(c, 15.0f, 0, 0.0f, cy, sy), 22.0f, 16.0f, yaw,
                fly_v3mk(0.29f, 0.29f, 0.30f));
    hangar = c;
    hangar.z = prop_footing(rt, v, e, g, c, 16.0f, 13.0f, yaw, 2.2f);
    prop_shed(rt, v, e, hangar, 16.0f, 13.0f, 9.5f, yaw, fly_v3mk(0.44f, 0.44f, 0.42f));
    /* fuel: two upright tanks in one bunded compound, with a bowser alongside */
    bund = pave(rt, v, e, g, yawp(c, 2.0f, -24.7f, 0.0f, cy, sy), 8.0f, 12.0f, yaw,
                fly_v3mk(0.30f, 0.29f, 0.27f));
    for (i = 0; i < 2; ++i)
        draw_prism(rt, v, e, yawp(c, 2.0f, -20.0f - (float)i * 9.5f, bund - c.z, cy, sy),
                   3.6f, 7.5f, 8,
                   fly_v3mk(0.52f, 0.50f, 0.46f), fly_v3mk(0.34f, 0.33f, 0.31f));
    prop_truck(rt, v, e, yawp(c, 14.0f, -19.0f, 0, cy, sy), yaw + 0.35f,
               fly_v3mk(0.50f, 0.16f, 0.12f), e->day < 0.35f);
    /* Where the work happens, the pavement shows it: under the bowser, on the
     * stand an aircraft is turned round on, and at the bund gate the drums are
     * rolled through. Three stains, at fixed offsets rather than scattered,
     * because each of them is under a specific thing that is standing there. */
    prop_stain(rt, v, e, yawp(c, 14.0f, -19.0f, bund - c.z + 0.02f, cy, sy), 2.6f, 7717u);
    prop_stain(rt, v, e, yawp(c, 20.0f, 1.5f, hard - c.z + 0.02f, cy, sy), 3.4f, 7723u);
    prop_stain(rt, v, e, yawp(c, 6.0f, -3.0f, hard - c.z + 0.02f, cy, sy), 2.1f, 7741u);
    /* masts down the pavement edge, standing on the pavement they light */
    for (i = 0; i < 3; ++i)
        prop_lamp(rt, v, e, yawp(c, 6.0f + (float)i * 14.0f, 14.4f, hard - c.z, cy, sy),
                  yaw - FLY_PI * 0.5f);
    if (detail < 2) return 1;
    prop_truck(rt, v, e, yawp(c, 24.0f, 6.0f, hard - c.z, cy, sy), yaw + 2.4f,
               fly_v3mk(0.20f, 0.34f, 0.42f), e->day < 0.35f);
    /* drums and pallets against the hangar wall — on the hangar's own slab,
       which is what they are stood against the wall of */
    for (i = 0; i < 5; ++i)
        prop_drum(rt, v, e, yawp(c, -13.0f + (float)i * 1.0f, 14.6f, hangar.z - c.z, cy, sy),
                  fly_v3mk(0.34f, 0.24f, 0.12f));
    prop_crate(rt, v, e, yawp(c, -6.0f, 14.4f, hangar.z - c.z, cy, sy), 1.8f, 1.3f, 1.4f, yaw,
               fly_v3mk(0.36f, 0.31f, 0.22f));
    prop_crate(rt, v, e, yawp(c, -6.0f, 14.4f, hangar.z - c.z + 1.5f, cy, sy),
               1.8f, 1.3f, 1.4f, yaw + 0.12f, fly_v3mk(0.33f, 0.29f, 0.21f));
    return 1;
}

/* The other side of the apron: a control tower and the block under it.
 *
 * Every place aircraft arrive at has one, it is the tallest thing that is not
 * industrial, and it is what stops a site reading as "a pad with a shed next to
 * it" — built mass on two sides of the deck instead of one. The cab is glazed
 * on all four faces with a railed balcony round it and a lit interior after
 * dark, which at night is the one window in a settlement that is always on. */
static int draw_tower_group(fly_render_target *rt, const fly__view *v, const fly__env *e,
                            const fly_game *g, fly_v3 site, float axis, int li, int detail,
                            fly_v3 *out) {
    fly_v3 c, cab, ob, tw;
    static const float turn[6] = { -1.05f, 1.05f, -0.45f, 0.45f, -1.75f, 1.75f };
    fly_v3 conc = fly_v3mk(0.46f, 0.45f, 0.43f);
    fly_v3 steel = fly_v3mk(0.33f, 0.34f, 0.37f);
    float yaw, cy, sy, th = 23.0f;
    int i;
    /* the balcony rail is the furthest thing in toward the pad, at 16.4 m */
    if (!site_plot(g, site, axis, turn, 6, 112.0f, 15.0f, 17.0f, &c, &yaw)) return 0;
    *out = c;
    cy = cosf(yaw);
    sy = sinf(yaw);
    /* The operations block and the tower out of one end of it, both on the raft
     * that carries them and joined by an apron the staff walk across — the two
     * were sitting on grass with a soft ring painted round the foot. */
    pave(rt, v, e, g, yawp(c, 1.0f, 0, 0.0f, cy, sy), 19.0f, 11.0f, yaw,
         fly_v3mk(0.31f, 0.31f, 0.31f));
    ob = yawp(c, -3.0f, 0, 0, cy, sy);
    tw = yawp(c, 8.5f, 0, 0, cy, sy);
    ob.z = prop_footing(rt, v, e, g, ob, 13.0f, 8.0f, yaw, 1.8f);
    tw.z = prop_footing(rt, v, e, g, tw, 3.6f, 3.6f, yaw, 2.4f);
    draw_box2(rt, v, e, ob, 13.0f, 8.0f, 5.4f, yaw, conc,
              fly_v3scale(conc, 0.78f), 1, (uint32_t)li * 977u + 11u);
    draw_box2(rt, v, e, tw, 3.6f, 3.6f, th, yaw, conc,
              fly_v3scale(conc, 0.8f), 1, (uint32_t)li * 61u + 3u);
    cab = fly_v3add(tw, fly_v3mk(0, 0, th));
    draw_box2(rt, v, e, cab, 5.6f, 5.6f, 0.5f, yaw, steel, steel, 0, 0);   /* balcony deck */
    for (i = 0; i < 4; ++i) {   /* the rail round it */
        float a0 = yaw + (float)i * (FLY_PI * 0.5f);
        float a1 = yaw + (float)(i + 1) * (FLY_PI * 0.5f);
        prop_railing(rt, v, e,
                     fly_v3mk(cab.x + cosf(a0 + 0.7854f) * 7.9f,
                              cab.y + sinf(a0 + 0.7854f) * 7.9f, cab.z + 0.5f),
                     fly_v3mk(cab.x + cosf(a1 + 0.7854f) * 7.9f,
                              cab.y + sinf(a1 + 0.7854f) * 7.9f, cab.z + 0.5f));
    }
    /* the glazed cab: walls dark, and after dark the inside is lit */
    {
        float dark = 1.0f - fly_smoothstepf(0.16f, 0.46f, e->day);
        fly_v3 glass = fly_v3lerp(fly_v3mk(0.05f, 0.07f, 0.09f),
                                  fly_v3mk(0.16f, 0.20f, 0.24f), e->day);
        draw_box2(rt, v, e, fly_v3add(cab, fly_v3mk(0, 0, 0.5f)), 4.2f, 4.2f, 3.4f, yaw,
                  glass, fly_v3scale(steel, 0.9f), 0, 0);
        if (dark > 0.02f) {
            fly_v3 lit = fly_v3scale(fly_v3mk(1.55f, 1.30f, 0.85f), dark);
            int f;
            for (f = 0; f < 4; ++f) {
                float a0 = yaw + (float)f * (FLY_PI * 0.5f);
                fly_v3 n0 = fly_v3mk(cosf(a0), sinf(a0), 0.0f);
                fly_v3 t0 = fly_v3mk(-n0.y, n0.x, 0.0f);
                fly_v3 o = fly_v3add(cab, fly_v3scale(n0, 4.25f));
                fly_v3 p0 = fly_v3add(fly_v3add(o, fly_v3scale(t0, -3.4f)), fly_v3mk(0, 0, 1.1f));
                fly_v3 p1 = fly_v3add(fly_v3add(o, fly_v3scale(t0, 3.4f)), fly_v3mk(0, 0, 1.1f));
                fly_v3 p2 = fly_v3add(p1, fly_v3mk(0, 0, 2.3f));
                fly_v3 p3 = fly_v3add(p0, fly_v3mk(0, 0, 2.3f));
                raster_tri_hdr1(rt, v, p0, p1, p2, lit);
                raster_tri_hdr1(rt, v, p0, p2, p3, lit);
            }
            draw_light(rt, v, fly_v3add(cab, fly_v3mk(0, 0, 2.2f)),
                       FLY_RGBA(255, 226, 170, (int)(180.0f * dark)), 1.6f);
        }
        draw_box2(rt, v, e, fly_v3add(cab, fly_v3mk(0, 0, 3.9f)), 4.8f, 4.8f, 0.45f, yaw,
                  steel, fly_v3scale(steel, 1.1f), 0, 0);
    }
    draw_warning_light(rt, v, e, fly_v3add(cab, fly_v3mk(0, 0, 5.6f)), li);
    if (detail < 2) return 1;
    prop_lamp(rt, v, e, yawp(c, -15.0f, 9.5f, 0, cy, sy), yaw + 2.2f);
    prop_truck(rt, v, e, yawp(c, -12.0f, -10.5f, 0, cy, sy), yaw + 0.2f,
               fly_v3mk(0.24f, 0.26f, 0.30f), e->day < 0.35f);
    prop_crate(rt, v, e, yawp(c, 2.0f, -10.0f, 0, cy, sy), 1.6f, 1.2f, 1.3f, yaw,
               fly_v3mk(0.32f, 0.30f, 0.26f));
    return 1;
}

/* --- the worked country ---------------------------------------------------
 *
 * What a settlement's fields carry that is not paint: the hedge along a
 * boundary, the steading standing in the corner of one, and the track that
 * joins it to the road.
 *
 * Its own pass rather than a branch inside the wood's, because it is the
 * opposite scatter — the wood plants where fly_world_forest is high and this
 * builds where fly_world_tilth is, and the two are exclusive by construction,
 * since a field is cleared ground (see forest_open). The lattice is walked the
 * way the wood's is and for the same reasons: one hash per cell, the cheap
 * field before the heightfield, and a projected-size gate before anything is
 * built.
 *
 * The hedge is why the pass exists at all. Enclosure is painted into the
 * ground by terrain_surface, and paint on its own reads as a pattern printed
 * on a lawn: what makes a boundary a boundary is that it stands up, throws a
 * shadow across the crop beside it and hides the foot of the field behind it.
 * A line of thorn is a handful of the bushes the scrub layer already draws,
 * and the parcel frame says exactly where they go — fly_world_parcel is split
 * out of the field for this, so a cell pays the settlement loop once and its
 * candidates pay only the frame.
 *
 * The steading is why the pass is worth its cost. A settlement is a pad, a
 * yard and a spire district, and then the world used to stop: there was
 * nothing at all between one and the next, and a country whose only buildings
 * are its airfields reads as an installation rather than as somewhere people
 * live. A barn, a byre and a fenced yard in the corner of a field is the whole
 * of the missing rung, and it is built out of props the settlements already
 * own. */

/* One cell in FLY_STEADING_ODDS carries a steading, on the FLY_STEADING_CELL
 * lattice, so the mean spacing is the pitch times the root of the odds — about
 * half a kilometre, which is a farm to every forty hectares and is what the
 * parcel sizes imply. */
#define FLY_STEADING_ODDS 32u
/* Nothing is built inside this of a settlement centre: past the clearing, past
 * the spire district, and past the ground the gateway and its avenue are laid
 * out on. A farm is what stands *outside* a town. */
#define FLY_STEADING_KEEPOUT 460.0f
/* Most points a cell's boundaries can want at the finest pitch: an 88 m cell
 * holds about eighty metres of hedge, which is forty at 2.2 m, and the cap is
 * there so an odd parcel frame cannot make the walk unbounded. */
#define FLY_HEDGE_MAX 64

/* Which way the field runs at a point: the bearing of the parcel boundary
 * nearest it, off a central difference of the boundary distance.
 *
 * A farm square to its own fields and a farm at a random angle to them are the
 * difference between a steading and a shed dropped on a pattern, and the frame
 * the fields are laid out on is a settlement's rather than the caller's — so
 * the honest way to ask is to measure it off the same function that drew them.
 * Four parcel evaluations, paid once per steading and never per hedge. */
static float parcel_bearing(const fly_world *w, int parish, float x, float y) {
    const float d = 6.0f;
    float c, e0, e1, e2, e3;
    fly_world_parcel(w, parish, x - d, y, &c, &e0);
    fly_world_parcel(w, parish, x + d, y, &c, &e1);
    fly_world_parcel(w, parish, x, y - d, &c, &e2);
    fly_world_parcel(w, parish, x, y + d, &c, &e3);
    /* the gradient points across the boundary, so the boundary runs at a right
     * angle to it; a gradient of nothing means the point is in the middle of a
     * parcel, where any bearing is as good as another */
    if (fabsf(e1 - e0) < 1e-4f && fabsf(e3 - e2) < 1e-4f) return 0.0f;
    return atan2f(e1 - e0, -(e3 - e2));
}

/* One farm: a barn, a byre at right angles to it, a fenced yard between them
 * with a gate in the side the track arrives on, and the clutter of a working
 * place. `big` builds the hamlet version — a second range and a longer yard —
 * which is what a cell rolls about one time in four. */
static void draw_steading(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          const fly_game *g, fly_v3 base, float yaw, uint32_t h, int big) {
    float cy = cosf(yaw), sy = sinf(yaw);
    /* Lime-washed stone through to weathered board, and a rusted tin roof on
     * the byre. Pale, and deliberately: a farm is the one building in the
     * world nobody paints in a faction's colours, and against a ploughed field
     * a dark one is a hole rather than a house. */
    fly_v3 timber = fly_v3lerp(fly_v3mk(0.46f, 0.42f, 0.34f), fly_v3mk(0.62f, 0.58f, 0.50f),
                               (float)((h >> 3) & 7) / 7.0f);
    fly_v3 tin = fly_v3lerp(fly_v3mk(0.45f, 0.30f, 0.20f), fly_v3mk(0.38f, 0.36f, 0.33f),
                            (float)((h >> 6) & 7) / 7.0f);
    float bh = 4.2f + (float)((h >> 9) & 3) * 0.5f;
    fly_v3 barn = yawp(base, -6.0f, 0.0f, 0.0f, cy, sy);
    fly_v3 byre = yawp(base, 6.5f, -7.0f, 0.0f, cy, sy);
    int i, n;
    barn.z = drawn_ground(&g->world, barn.x, barn.y);
    byre.z = drawn_ground(&g->world, byre.x, byre.y);
    prop_shed(rt, v, e, barn, 7.2f, 4.6f, bh, yaw, timber);
    prop_skirt(rt, v, e, g, barn, 7.2f, 4.6f, yaw, 3.0f, 0.55f);
    prop_shed(rt, v, e, byre, 4.4f, 3.0f, 3.0f, yaw + FLY_PI * 0.5f, tin);
    prop_skirt(rt, v, e, g, byre, 3.0f, 4.4f, yaw, 2.4f, 0.5f);
    if (big) {
        fly_v3 range = yawp(base, -4.0f, 11.5f, 0.0f, cy, sy);
        range.z = drawn_ground(&g->world, range.x, range.y);
        prop_shed(rt, v, e, range, 5.6f, 3.4f, 3.4f, yaw, timber);
        prop_skirt(rt, v, e, g, range, 5.6f, 3.4f, yaw, 2.6f, 0.5f);
    }
    /* The yard, and it is the piece that does the work: a scatter of sheds is
     * a depot and a boundary round them is a farm. Gated on the side the byre
     * leaves open, so the yard has a way in. */
    {
        float hx = 15.0f, hy = big ? 15.0f : 11.0f;
        fly_v3 c0 = yawp(base, -hx, -hy, 0.0f, cy, sy);
        fly_v3 c1 = yawp(base, hx, -hy, 0.0f, cy, sy);
        fly_v3 c2 = yawp(base, hx, hy, 0.0f, cy, sy);
        fly_v3 c3 = yawp(base, -hx, hy, 0.0f, cy, sy);
        c0.z = drawn_ground(&g->world, c0.x, c0.y);
        c1.z = drawn_ground(&g->world, c1.x, c1.y);
        c2.z = drawn_ground(&g->world, c2.x, c2.y);
        c3.z = drawn_ground(&g->world, c3.x, c3.y);
        prop_fence(rt, v, e, c0, c1, 0.5f, 5.0f);
        prop_fence(rt, v, e, c1, c2, 0.0f, 0.0f);
        prop_fence(rt, v, e, c2, c3, 0.0f, 0.0f);
        prop_fence(rt, v, e, c3, c0, 0.0f, 0.0f);
    }
    n = 2 + (int)((h >> 12) & 3u);
    for (i = 0; i < n; ++i) {
        uint32_t q = fly_hash2(h ^ 0x27D4EB2Fu, i, 3);
        float lx = ((float)((q >> 3) & 63) / 63.0f - 0.5f) * 18.0f;
        float ly = ((float)((q >> 11) & 63) / 63.0f - 0.5f) * 14.0f;
        fly_v3 p = yawp(base, lx, ly, 0.0f, cy, sy);
        p.z = drawn_ground(&g->world, p.x, p.y);
        if ((q & 1u) == 0u)
            prop_drum(rt, v, e, p, fly_v3mk(0.30f, 0.26f, 0.14f));
        else
            prop_crate(rt, v, e, p, 1.1f, 0.9f, 0.9f, yaw + (float)(q & 7u) * 0.2f,
                       fly_v3mk(0.34f, 0.30f, 0.22f));
    }
}

/* Where the farm in one lattice cell stands, or 0 for a cell with no farm.
 *
 * Split out of the pass and exported because *where* a thing may be built is a
 * claim about the world rather than about a frame, and a rule the test suite
 * re-derives is a rule nobody is holding: `world.tilth` walks this over a whole
 * chart and measures the footprint it hands back. It is also the only place
 * the rules are written down, so the drawn farm and the tested one cannot
 * drift apart.
 *
 * The rules are the ones a settlement's own architecture is held to. Dry
 * ground, and ground flat enough to build on. Off the carriageway and off the
 * guideway. Well outside the town whose fields these are, because a farm is
 * what stands beyond a town rather than inside it. And against a field
 * boundary rather than out in the middle of the crop, which is where a farm
 * with a lane to it actually sits. Five candidates in the cell and then no
 * farm at all — a plot that fails is not a plot shoved sideways, which is the
 * same answer fly_site_footing gives. */
int fly_steading_at(const fly_game *g, int ix, int iy, fly_steading *out) {
    const fly_world *w = &g->world;
    const float cell = FLY_STEADING_CELL;
    float cx = ((float)ix + 0.5f) * cell, cy = ((float)iy + 0.5f) * cell;
    /* The lattice roll first, because it is free and it is what makes this
     * cheap enough to ask of every cell the pass walks: thirty-one cells in
     * thirty-two are answered without touching the heightfield. */
    uint32_t h1 = fly_hash2(w->seed + 101, ix, iy);
    fly_tilth ti;
    int j;
    if ((h1 % FLY_STEADING_ODDS) != 9u) return 0;
    fly_world_tilth(w, cx, cy, fly_world_ground(w, cx, cy), &ti);
    if (ti.parish < 0 || ti.work < 0.30f) return 0;
    for (j = 0; j < 5; ++j) {
        uint32_t h = fly_hash2(h1 ^ 0x165667B1u, ix + j, iy - j);
        float px = cx + ((float)((h >> 3) & 63) / 63.0f - 0.5f) * cell;
        float py = cy + ((float)((h >> 9) & 63) / 63.0f - 0.5f) * cell;
        float crop, edge, gz, dist, sep, yaw;
        fly_wpos wp = fly_wpos_mk(px, py);
        int c, level = 1;
        fly_world_parcel(w, ti.parish, px, py, &crop, &edge);
        if (edge < 2.0f || edge > 12.0f) continue;
        gz = fly_world_ground(w, px, py);
        if (gz < FLY_WATER_Z + 2.0f) continue;
        if (!fly_world_ground_safe(w, px, py, 0.14f)) continue;
        if (!clear_of_sites(w, px, py, FLY_STEADING_KEEPOUT)) continue;
        if (fly_road_gap(&g->roads, wp) < FLY_STEADING_VERGE) continue;
        if (fly_rail_project(&g->rail_route, fly_v3mk(px, py, gz), &dist, &sep) &&
            sep < FLY_STEADING_VERGE + 8.0f) continue;
        /* And the whole yard, not only the point it is measured at.
         *
         * The slope test above is a central difference at the centre, which is
         * the ground under the barn door and says nothing about the ground
         * thirty metres away — and a farm is a yard, so a plot that passes it
         * on a break of slope puts a fence post two and a half metres in the
         * air. The four corners are asked directly, in the yard's own frame,
         * which is the same rule `world.siting` holds a settlement's
         * architecture to. */
        yaw = parcel_bearing(w, ti.parish, px, py);
        for (c = 0; c < 4 && level; ++c) {
            float ex = (c == 0 || c == 3) ? -FLY_STEADING_YARD : FLY_STEADING_YARD;
            float ey = c < 2 ? -FLY_STEADING_YARD : FLY_STEADING_YARD;
            float qx = px + ex * cosf(yaw) - ey * sinf(yaw);
            float qy = py + ex * sinf(yaw) + ey * cosf(yaw);
            float qz = fly_world_ground(w, qx, qy);
            if (qz <= FLY_WATER_Z || fabsf(qz - gz) > 2.0f) level = 0;
            if (fly_road_gap(&g->roads, fly_wpos_mk(qx, qy)) < FLY_ROAD_CLEAR) level = 0;
        }
        if (!level) continue;
        out->pos = wp;
        out->ground = gz;
        out->yaw = yaw;
        out->hash = h;
        out->big = (h & 3u) == 1u;
        return 1;
    }
    return 0;
}

static void draw_steadings(fly_render_target *rt, const fly_game *g, const fly__env *e,
                           const fly__view *v) {
    const fly_world *w = &g->world;
    const float cell = FLY_STEADING_CELL;
    int casting = g_shadow_cast != NULL;
    float ox = casting ? g_shadow_cast->center[0].x : v->pos.x;
    float oy = casting ? g_shadow_cast->center[0].y : v->pos.y;
    float radius = casting ? g_shadow_cast->half[0] * 1.35f : 2400.0f * e->lod.trees;
    /* A hedge is two metres of thorn and stops being worth a triangle long
     * before a barn does, so the two ranges are separate and the loop runs to
     * the wider of them. */
    float hedge_r = casting ? radius : 760.0f * e->lod.trees;
    int ix0 = (int)floorf((ox - radius) / cell), ix1 = (int)ceilf((ox + radius) / cell);
    int iy0 = (int)floorf((oy - radius) / cell), iy1 = (int)ceilf((oy + radius) / cell);
    int ix, iy;
    for (iy = iy0; iy <= iy1; ++iy)
        for (ix = ix0; ix <= ix1; ++ix) {
            float cx = ((float)ix + 0.5f) * cell, cy2 = ((float)iy + 0.5f) * cell;
            float dx = cx - ox, dy = cy2 - oy;
            float d2 = dx * dx + dy * dy, gz0;
            fly_tilth ti;
            uint32_t h1;
            int j;
            if (d2 > radius * radius) continue;
            gz0 = scatter_ground(w, cx, cy2);
            fly_world_tilth(w, cx, cy2, gz0, &ti);
            /* One answer for the cell: everything below asks the parcel frame
             * per candidate and the settlement loop not at all. */
            if (ti.parish < 0 || ti.work < 0.30f) continue;
            if (!casting && !patch_visible(v, rt, cx, cy2, gz0 + 6.0f, 74.0f)) continue;
            h1 = fly_hash2(w->seed + 101, ix, iy);

            if (d2 < hedge_r * hedge_r) {
                fly_wpos line[FLY_HEDGE_MAX];
                /* A bush is about a metre and a half across and a hedge is a
                 * solid thing, so the pitch is what makes the line close up
                 * rather than read as a row of shrubs — and it opens again
                 * with distance on the same power-of-two ladder the road
                 * lighting thins on, so a hedge two kilometres off costs an
                 * eighth of what the one under the wheels does and keeps the
                 * bushes it had rather than renumbering them. */
                int stride = lamp_stride(sqrtf(d2));
                int nb = fly_world_hedge(w, ti.parish, cx, cy2, cell * 0.5f,
                                         2.2f * (float)stride, line, FLY_HEDGE_MAX);
                for (j = 0; j < nb; ++j) {
                    float px = line[j].e, py = line[j].n, gz, s;
                    uint32_t h = fly_hash2(h1 ^ 0xC2B2AE35u, (int)px, (int)py);
                    fly_v3 leaf;
                    /* Thinned by how worked the ground is, so a hedge fades
                     * out with the enclosure it encloses rather than ending at
                     * the cell where the belt does. */
                    if ((float)(h & 255u) / 255.0f > ti.work) continue;
                    gz = scatter_ground(w, px, py);
                    if (gz < FLY_WATER_Z + 0.5f) continue;
                    if (fly_road_near(&g->roads, fly_wpos_mk(px, py)) &&
                        fly_road_gap(&g->roads, fly_wpos_mk(px, py)) < FLY_ROAD_CLEAR)
                        continue;
                    s = 1.25f + (float)((h >> 20) & 7) * 0.14f;
                    leaf = fly_v3mk(0.070f, 0.130f, 0.052f);
                    /* One standard in sixteen: a hedge left to itself grows
                     * trees out of it, and a line of them is what carries a
                     * boundary at the range the bushes have already gone. */
                    if ((h & 15u) == 3u)
                        draw_tree(rt, v, e, fly_v3mk(px, py, gz), 5.5f + s * 2.0f, 1,
                                  fly_v3mk(0.11f, 0.22f, 0.08f), h,
                                  tree_lod(d2, e->lod.trees));
                    else
                        draw_bush(rt, v, e, fly_v3mk(px, py, gz), s, leaf, h);
                }
            }

            /* And the farm, if this cell has one. Where it stands is
             * fly_steading_at's answer and not this loop's — see there. */
            {
                fly_steading st;
                fly_v3 base;
                float dist, sep;
                if (!fly_steading_at(g, ix, iy, &st)) continue;
                base = fly_wpos_at(st.pos, st.ground);
                draw_steading(rt, v, e, g, base, st.yaw, st.hash, st.big);
                /* The lane in. A farm the road does not reach is a model
                 * standing in a field; the track is what makes it somewhere
                 * somebody drives to. Only when a carriageway is close enough
                 * that one straight run is an honest answer — past that the
                 * lane would want a survey of its own, which is a road. */
                if (!casting) {
                    int r = fly_road_project(&g->roads, base, &dist, &sep);
                    if (r >= 0 && sep < FLY_STEADING_LANE) {
                        fly_v3 on, along;
                        if (fly_road_eval(&g->roads.road[r], dist, &on, &along))
                            prop_track(rt, v, e, g, base, on, 2.4f, FLY_TRACK_GRAVEL);
                    }
                }
            }
        }
}

/* --- the gate ------------------------------------------------------------
 *
 * Where the road arrives, which until now was nowhere.
 *
 * `fly_road` stops a carriageway FLY_PAD_DECK_R + 30 metres short of the site
 * centre, so that the last stretch is the settlement's rather than the
 * network's — and then nothing was built on that stretch, so every road in the
 * world ran out of deck thirty metres from the apron and simply stopped, a
 * black ribbon with a sheared end lying in unbroken grass. From the air that is
 * not a road serving a place; it is a road somebody forgot to finish, and it
 * was the loudest thing wrong with the settlements.
 *
 * What is here is the missing thirty metres and the reason for it:
 *
 *  - the run in, surfaced like the road and carried onto the pad's apron, so
 *    a carriageway ends at the deck it serves instead of at a field;
 *  - a checkpoint on the boundary — barrier, cabin, kerbs — because a road
 *    into a place where aircraft are kept ends at a control, and a barrier is
 *    the one prop that says which side of a line you are on;
 *  - a parking lot beside it, marked out in bays, because a checkpoint with
 *    nothing behind it is a gate in a fence with no field;
 *  - and lighting down the run and round the lot, which is the whole of the
 *    lamp question: these are lamps that light something.
 *
 * All of it in the gate's own frame — +x from the terminus toward the pad —
 * so the layout is the same at every settlement, on any bearing, and none of
 * it can drift out of the right of way the road already keeps clear of
 * buildings (FLY_ROAD_GATE, which is why the lot may stand where it does).
 *
 * Drawn per road, and a settlement has at most FLY_ROAD_DEGREE_MAX of them. */
static void draw_gateway(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         const fly_game *g, fly_v3 base, int li, int detail) {
    fly_v3 tar = fly_v3mk(0.155f, 0.155f, 0.16f);
    fly_v3 conc = fly_v3mk(0.42f, 0.41f, 0.39f);
    fly_v3 kerb = fly_v3mk(0.50f, 0.49f, 0.46f);
    int at[FLY_ROAD_DEGREE_MAX], n = fly_road_at(&g->roads, li, at, FLY_ROAD_DEGREE_MAX);
    int r;
    for (r = 0; r < n; ++r) {
        fly_v3 tip, in, gate, lot;
        float yaw, cy, sy, lotz;
        if (!fly_road_end(&g->roads, at[r], li, &tip, &in)) continue;
        yaw = atan2f(in.y, in.x);
        cy = cosf(yaw);
        sy = sinf(yaw);
        if (!object_worth_drawing(v, rt, tip, 60.0f)) continue;

        /* The run in: from the toe of the carriageway's own ramp onto the
           deck's rim. `deck_edge` is where a way leaving the pad for `tip`
           crosses it, so the two ends of this are the two ends of the gap. */
        gate = fly_v3mk(tip.x + in.x * FLY_ROAD_RAMP, tip.y + in.y * FLY_ROAD_RAMP, tip.z);
        prop_way(rt, v, e, g, gate, deck_edge(base, tip), FLY_ROAD_HALF, tar, 1);

        /* The checkpoint, on the carriageway at the terminus: a kerb block
         * either side of the opening, the cabin behind the left one, and the
         * arm across the road. The arm is down — a barrier that is up is a
         * barrier you cannot see from anywhere but alongside. */
        {
            fly_v3 lk = yawp(tip, 1.0f, FLY_ROAD_HALF + 1.6f, 0, cy, sy);
            fly_v3 rk = yawp(tip, 1.0f, -FLY_ROAD_HALF - 1.6f, 0, cy, sy);
            float lz = pave(rt, v, e, g, lk, 3.0f, 2.2f, yaw, conc);
            float rz = pave(rt, v, e, g, rk, 3.0f, 2.2f, yaw, conc);
            lk.z = lz;
            rk.z = rz;
            draw_box2(rt, v, e, lk, 2.2f, 1.7f, 3.1f, yaw,
                      fly_v3mk(0.46f, 0.45f, 0.42f), fly_v3mk(0.28f, 0.27f, 0.25f),
                      1, (uint32_t)li * 313u + (uint32_t)r * 17u + 5u);
            draw_box2(rt, v, e, rk, 1.0f, 1.0f, 1.4f, yaw, kerb,
                      fly_v3scale(kerb, 1.1f), 0, 0);
            /* The arm: a boom from the hinge post on the cabin side across the
               inbound lane, in the red and white it is painted in everywhere
               else in the world. It stops at the centre line, because the far
               lane is the one traffic leaves by. */
            draw_box2(rt, v, e, yawp(tip, 1.0f, FLY_ROAD_HALF * 0.5f, 2.0f, cy, sy),
                      0.16f, FLY_ROAD_HALF * 0.5f - 0.5f, 0.18f, yaw,
                      fly_v3mk(0.62f, 0.14f, 0.11f), fly_v3mk(0.80f, 0.78f, 0.74f), 0, 0);
            draw_box2(rt, v, e, yawp(tip, 1.0f, FLY_ROAD_HALF - 0.6f, 0.0f, cy, sy),
                      0.22f, 0.22f, 2.1f, yaw, kerb, kerb, 0, 0);
            /* one column, over the opening, on the cabin's own slab. Two put a
               pole either side of a fifteen-metre gap and read as a gantry. */
            prop_lamp(rt, v, e, lk, yaw - FLY_PI * 0.5f);
        }

        if (detail < 2) continue;

        /* The lot: hardstand back down the road and off to one side, inside
         * the right of way and so on ground nothing else may build on. Which
         * side alternates with the road index, so a site with three gates does
         * not stack three lots on the same flank. */
        {
            float side = (r & 1) ? -1.0f : 1.0f;
            int b;
            lot = yawp(tip, -26.0f, side * 26.0f, 0.0f, cy, sy);
            lotz = pave(rt, v, e, g, lot, 18.0f, 12.0f, yaw, fly_v3mk(0.235f, 0.235f, 0.24f));
            lot.z = lotz;
            /* bays, painted across the short axis in two ranks off a spine */
            for (b = 0; b < 12; ++b) {
                float bx = -17.0f + (float)(b % 6) * 6.8f;
                float by = (b < 6) ? 4.2f : -4.2f;
                fly_v3 q0 = yawp(lot, bx, by - 5.6f, 0.05f, cy, sy);
                fly_v3 q1 = yawp(lot, bx + 0.30f, by - 5.6f, 0.05f, cy, sy);
                fly_v3 q2 = yawp(lot, bx + 0.30f, by + 5.6f, 0.05f, cy, sy);
                fly_v3 q3 = yawp(lot, bx, by + 5.6f, 0.05f, cy, sy);
                uint32_t mk = FLY_RGB(178, 176, 162);
                raster_tri_view(rt, v, q0, q1, q2, mk);
                raster_tri_view(rt, v, q0, q2, q3, mk);
            }
            /* the spur off the carriageway into it, and the light over it */
            prop_way(rt, v, e, g, yawp(tip, -26.0f, side * (FLY_ROAD_HALF + 1.0f), tip.z,
                                       cy, sy),
                     lot, 4.5f, tar, 0);
            prop_lamp(rt, v, e, yawp(lot, 0.0f, side * 12.0f, 0.0f, cy, sy),
                      yaw - FLY_PI * 0.5f * side);
            prop_truck(rt, v, e, yawp(lot, -10.6f, 4.2f, 0.0f, cy, sy),
                       yaw + FLY_PI * 0.5f,
                       fly_v3mk(0.30f, 0.34f, 0.22f), e->day < 0.35f);
            prop_truck(rt, v, e, yawp(lot, 3.0f, -4.2f, 0.0f, cy, sy),
                       yaw - FLY_PI * 0.5f,
                       fly_v3mk(0.44f, 0.26f, 0.14f), e->day < 0.35f);
            /* and a fence along the back of it, so the lot has an edge */
            prop_fence(rt, v, e, yawp(lot, -20.0f, side * 13.5f, 0.0f, cy, sy),
                       yawp(lot, 20.0f, side * 13.5f, 0.0f, cy, sy), -1.0f, 0.0f);
        }
    }
}

/* Traffic: a service vehicle on a circuit of the apron, placed by the clock.
 *
 * The single cheapest thing that makes a place look inhabited is something in
 * it that is somewhere else a minute later. It is a function of time and the
 * site index alone — no state, no simulation — so every client agrees about
 * where the truck is without anybody sending it. */
static void draw_site_traffic(fly_render_target *rt, const fly__view *v, const fly__env *e,
                              fly_v3 base, float axis, int li) {
    float period = 96.0f + (float)((li * 7) % 5) * 11.0f;
    float u = (float)fmod(e->time * 0.35 + (double)li * 13.0, (double)period) / period;
    float ang = u * 2.0f * FLY_PI;
    float rr = 118.0f + 9.0f * sinf(ang * 3.0f);
    fly_v3 p = fly_v3add(base, fly_v3mk(cosf(ang + axis) * rr, sinf(ang + axis) * rr, 0));
    /* the heading is the tangent, so it faces the way it is going */
    float head = ang + axis + FLY_PI * 0.5f;
    p.z = base.z;
    prop_truck(rt, v, e, p, head, fly_v3mk(0.52f, 0.46f, 0.20f), e->day < 0.35f);
}

/* The bearing a settlement is laid out on: the direction infrastructure arrives
 * from. The rail first where there is one, the road otherwise — two sites in
 * the world have a railhead and every site the network reaches has a road, so
 * without the second clause almost every settlement in the world would be laid
 * out on due east while its own road came in over a corner of the yard. Where
 * neither reaches, zero, which is what it always was. */
static float site_axis(const fly_game *g, fly_v3 base) {
    const fly_rail_route *r = &g->rail_route;
    float best = 1e30f, ang = 0.0f;
    int i;
    for (i = 0; i < r->point_count; ++i) {
        float dx = r->points[i].pos.x - base.x, dy = r->points[i].pos.y - base.y;
        float d = dx * dx + dy * dy;
        if (d < best && d > 1.0f) { best = d; ang = atan2f(dy, dx); }
    }
    if (best < 400.0f * 400.0f) return ang;
    for (i = 0; i < g->world.nloc; ++i) {
        float bearing;
        if (fabsf(g->world.loc[i].pos.e - base.x) > 1.0f ||
            fabsf(g->world.loc[i].pos.n - base.y) > 1.0f) continue;
        if (fly_road_bearing(&g->roads, i, &bearing)) return bearing;
        break;
    }
    return 0.0f;
}

/* --- the banner mast -----------------------------------------------------
 *
 * Who holds this place, said in the one language an aircraft at two thousand
 * feet can read: a colour on a pole.
 *
 * Everything else factions do to the world is a number on a screen. This is
 * the part that is *in* the world, and it is deliberately the tallest thing at
 * a settlement that is not a city tower — thirty-two metres, on the site's own
 * axis, so it stands clear of the deck and lines up with the pad and the yard
 * gate like everything else here does.
 *
 * Three things carry, at three ranges. The masthead lamp is emissive and
 * drawn at any distance the site is drawn at, so from the far side of a valley
 * a pinprick of Foundry rust or Concord blue says whose ground you are looking
 * at. The pennant resolves a couple of kilometres out and gives the colour
 * area instead of a point. Close in, the cloth moves — a travelling wave along
 * its length, off the shared clock so the shadow pass and the beauty pass
 * agree about where the fabric is.
 *
 * A contested site flies a second, smaller pennant underneath in the
 * challenger's colours, and its masthead strobes. That is a fight visible from
 * the air before any of it is visible on a chart, which is the point: the
 * player should be able to fly somewhere and see that it is changing hands. */
/* Where the mast stands: off the deck on the opposite bearing from the beacon,
 * and walked back onto dry ground like every other piece of settlement
 * architecture — a coastal site would otherwise fly its colours from the sea
 * bed. One function, so the thing that draws it and the thing that points a
 * camera at it cannot disagree about where it is. */
static int banner_mast(const fly_game *g, fly_v3 base, float ax, fly_v3 *out) {
    float ca = cosf(ax), sa = sinf(ax);
    fly_v3 mast = base;
    mast.x += ca * -86.0f - sa * 18.0f;
    mast.y += sa * -86.0f + ca * 18.0f;
    if (!fly_site_footing(g, base, 2.0f, &mast)) return 0;
    *out = mast;
    return 1;
}

float fly_site_axis(const fly_game *g, int loc) {
    const fly_location *L;
    if (!g || loc < 0 || loc >= g->world.nloc) return 0.0f;
    L = &g->world.loc[loc];
    return site_axis(g, fly_wpos_at(L->pos, fly_world_ground(&g->world, L->pos.e, L->pos.n)));
}

int fly_site_banner(const fly_game *g, int loc, fly_v3 *out) {
    fly_v3 base;
    const fly_location *L;
    if (!g || loc < 0 || loc >= g->world.nloc || !out) return 0;
    L = &g->world.loc[loc];
    base = fly_wpos_at(L->pos, fly_world_ground(&g->world, L->pos.e, L->pos.n));
    return banner_mast(g, base, site_axis(g, base), out);
}

static void draw_banner(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        const fly_game *g, fly_v3 base, float ax, int li) {
    const fly_location *L = &g->world.loc[li];
    int owner = (int)L->owner;
    int contested = fly_faction_contested(&g->world, li);
    int challenger = FLY_FACTION_FREE;
    fly_v3 mast = base;
    fly_v3 steel = fly_v3mk(0.44f, 0.45f, 0.47f);
    fly_v3 conc = fly_v3mk(0.40f, 0.39f, 0.37f);
    fly_v3 col = fly_faction_mark(owner);
    float H = 32.0f;
    float ca = cosf(ax), sa = sinf(ax);
    uint32_t lamp;

    if (!banner_mast(g, base, ax, &mast)) return;
    if (!object_worth_drawing(v, rt, fly_v3add(mast, fly_v3mk(0, 0, H * 0.5f)), H)) return;

    {   /* the lamp: colour of whoever holds it, strobing while it is argued
         * over. Drawn before the geometry so a site too far away for the mast
         * still says who owns it. */
        float pulse = contested
                          ? (fmod(e->time * 1.6 + (double)li * 0.31, 1.0) < 0.45 ? 1.0f : 0.15f)
                          : 0.55f + 0.45f * sinf((float)e->time * 0.9f + (float)li * 2.1f);
        int a = (int)(fly_clampf((0.45f + (1.0f - e->day) * 0.75f) * pulse, 0, 1) * 255.0f);
        lamp = FLY_RGBA((int)(fly_clampf(col.x, 0, 1) * 255.0f),
                        (int)(fly_clampf(col.y, 0, 1) * 255.0f),
                        (int)(fly_clampf(col.z, 0, 1) * 255.0f), a);
        draw_light(rt, v, fly_v3add(mast, fly_v3mk(0, 0, H + 1.2f)), lamp, 1.9f);
    }
    if (fly_v3dist(mast, v->pos) > 4200.0f && !g_shadow_cast) return;

    /* The plaza under it. A thirty-two metre mast standing in a meadow is a
       flagpole somebody planted on the way past; what it wants is the small
       square of paving every ceremonial mast in the world stands in. */
    mast.z = pave(rt, v, e, g, mast, 5.5f, 5.5f, ax, fly_v3mk(0.33f, 0.32f, 0.31f));
    draw_prism(rt, v, e, mast, 1.5f, 0.6f, 8, conc, fly_v3scale(conc, 0.8f));
    draw_box2(rt, v, e, fly_v3add(mast, fly_v3mk(0, 0, 0.55f)), 0.24f, 0.24f, H - 0.55f,
              ax, steel, fly_v3scale(steel, 1.1f), 0, 0);

    if (contested) challenger = fly_faction_challenger(&g->world, li, NULL);

    {   /* The cloth: hoist at the mast, flying along the site axis, tapering to
         * a tail, with a wave running down it. Two passes rather than a loop
         * over an array, because the second pennant only exists sometimes.
         *
         * The field is the power's *mark* and the border its countermark —
         * see fly_faction_mark for why a flag cannot simply be the hull
         * colour, and why the chart, the radar and this all read the same one
         * colour per power. */
        int fi;
        for (fi = 0; fi < (challenger != FLY_FACTION_FREE ? 2 : 1); ++fi) {
            fly_v3 fc = fly_faction_mark(fi ? challenger : owner);
            fly_v3 ft = fly_faction_countermark(fi ? challenger : owner);
            float top = fi ? H - 11.5f : H - 1.1f;
            float len = fi ? 7.0f : 13.0f;
            float drop = fi ? 3.0f : 5.4f;
            int seg = 6, k;
            for (k = 0; k < seg; ++k) {
                float u0 = (float)k / (float)seg, u1 = (float)(k + 1) / (float)seg;
                float w0 = sinf(u0 * 7.0f - (float)e->time * 3.1f + (float)li) * 0.55f * u0;
                float w1 = sinf(u1 * 7.0f - (float)e->time * 3.1f + (float)li) * 0.55f * u1;
                /* a pennant is a triangle by the time it reaches the tail */
                float d0 = drop * (1.0f - u0 * 0.72f), d1 = drop * (1.0f - u1 * 0.72f);
                fly_v3 a0 = fly_v3mk(mast.x + ca * (u0 * len) - sa * w0,
                                     mast.y + sa * (u0 * len) + ca * w0, mast.z + top);
                fly_v3 a1 = fly_v3mk(mast.x + ca * (u1 * len) - sa * w1,
                                     mast.y + sa * (u1 * len) + ca * w1, mast.z + top);
                fly_v3 b0 = fly_v3mk(a0.x, a0.y, a0.z - d0);
                fly_v3 b1 = fly_v3mk(a1.x, a1.y, a1.z - d1);
                /* the last third of the cloth is the border, in the trim */
                wall_quad(rt, v, e, b0, b1, a1, a0, u0 > 0.66f ? ft : fc, 1.0f);
            }
        }
    }
}

static void draw_location(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          const fly_game *g, int li) {
    const fly_location *L = &g->world.loc[li];
    fly_v3 base = fly_wpos_at(L->pos, fly_world_ground(&g->world, L->pos.e, L->pos.n));
    float d = fly_v3dist(base, v->pos);
    if (d > 17000.0f) return;
    /* pad, beacon and the tallest architecture fit inside this */
    if (!object_worth_drawing(v, rt, fly_v3add(base, fly_v3mk(0, 0, 60.0f)), 330.0f)) return;
    uint32_t bcol = L->kind == FLY_LOC_CITY ? FLY_RGB(255, 190, 90) :
                    L->kind == FLY_LOC_RUINS ? FLY_RGB(190, 110, 255) : FLY_RGB(110, 220, 255);
    /* No contact skirt on the deck, and it is the one thing in a settlement
     * that does not want one. Everything else stands on ground that was there
     * before it; a pad's apron is graded flat to it by fly_world_ground out to
     * 75 m and blended back to the terrain by 150, so there is no seam at the
     * rim to soften — the earthworks are already in the heightfield.
     *
     * Drawing one anyway put ground-following geometry a hand's breadth over
     * that apron, which is inside the 0.15..1.4 m window `render.pad` scans to
     * find where the deck ends: the scan ran off the rim onto the skirt and
     * reported the terrain's roughness as the deck's — 10.4 px of rim ripple
     * against a bound of 0.5, at the one altitude where a pad fills the frame.
     */
    draw_pad(rt, v, e, base, FLY_PAD_DECK_R, li);
    draw_beacon(rt, v, e, fly_v3add(base, fly_v3mk(72, 26, 0)), 24.0f, bcol, li);
    /* Before the range cut, not after it: the flag is the one piece of a
     * settlement that has to be readable from further away than the buildings
     * are, because it is the answer to a question you ask before deciding
     * whether to fly down there at all. */
    draw_banner(rt, v, e, g, base, site_axis(g, base), li);
    if (d > 9000.0f) return; /* architecture pops in closer */

    fly_rng rng;
    fly_rng_seed(&rng, g->seed * 131u + (uint32_t)li * 7u + 3u);
    int k;
    int det = site_detail(v, base);
    float ax = site_axis(g, base);

    /* Everything below is per-kind. Everything here is what every settlement
     * has because somebody works at it: a lit apron, a fenced yard with stock
     * in it, and a vehicle going round. Laid out on the site's own axis, so the
     * yard gate and the pad face each other at every site. */
    if (det >= 1) {
        /* The pad's own lighting, and it stands on the pad's own apron rather
         * than out in the field. This used to be a ring of six columns at the
         * deck radius plus twenty-six metres, on whatever the ground happened
         * to be there — six lamps in unbroken grass, lighting grass, which is
         * the single thing that made every settlement in the gallery read as
         * scattered. Four now, at the rim itself where `fly_world_ground`
         * grades the apron flat, each on a hardstand of its own so it is a
         * fitting on a paved edge rather than a post in a meadow.
         *
         * Twenty-two metres out and not seven: `fly_site_footing` keeps
         * everything off the deck out to FLY_PAD_DECK_R + 18, so a lamp asked
         * for inside that is not built at all — the first version of this
         * removed six lamps and installed none. */
        for (k = 0; k < 4; ++k) {
            float a2 = ax + FLY_PI * 0.25f + (float)k * (FLY_PI * 0.5f);
            fly_v3 q = fly_v3add(base, fly_v3mk(cosf(a2) * (FLY_PAD_DECK_R + 22.0f),
                                                sinf(a2) * (FLY_PAD_DECK_R + 22.0f), 0));
            if (!fly_site_footing(g, base, 1.2f, &q)) continue;
            q.z = pave(rt, v, e, g, q, 3.4f, 3.4f, a2, fly_v3mk(0.27f, 0.27f, 0.28f));
            prop_lamp(rt, v, e, q, a2 + FLY_PI);
        }
        {
            fly_v3 ac = base, tc = base, yc = base;
            int ha = draw_apron(rt, v, e, g, base, ax, det, &ac);
            int ht = draw_tower_group(rt, v, e, g, base, ax, li, det, &tc);
            int hy = draw_yard(rt, v, e, g, base, ax,
                               g->seed * 2657u + (uint32_t)li * 41u + 7u,
                               fly_v3mk(0.34f, 0.30f, 0.24f), det, &yc);
            /* The ground between them, used, and lit down its own length. Drawn
               after the groups so their own pavement and plinths win the depth
               test where they overlap. */
            if (ha) prop_way(rt, v, e, g, deck_edge(base, ac), ac, 5.0f,
                             FLY_TRACK_GRAVEL, 1);
            if (ht) prop_way(rt, v, e, g, deck_edge(base, tc), tc, 4.0f,
                             FLY_TRACK_GRAVEL, 1);
            if (hy) prop_way(rt, v, e, g, ha ? ac : deck_edge(base, yc), yc, 4.5f,
                             FLY_TRACK_GRAVEL, 1);
        }
        draw_gateway(rt, v, e, g, base, li, det);
        draw_site_traffic(rt, v, e, base, ax, li);
    }

    switch (L->kind) {
    case FLY_LOC_CITY: {
        /* Spire district: two concentric blocks on radial avenues, with the
         * avenues indexed off the rail's approach so the line arrives down one
         * of them rather than between two towers. Placed on a lattice with only
         * a little jitter — the previous free-for-all in radius read as scatter,
         * and a city is the one settlement kind that should look laid out. */
        int n = 10;
        float axis = site_axis(g, base);
        for (k = 0; k < n; ++k) {
            float ang = axis + ((float)k + 0.5f) / n * 2.0f * FLY_PI + fly_rng_span(&rng, -0.06f, 0.06f);
            float rr = ((k & 1) ? 130.0f : 235.0f) + fly_rng_span(&rng, -14, 14);
            fly_v3 p = fly_v3add(base, fly_v3mk(cosf(ang) * rr, sinf(ang) * rr, 0));
            float h1 = fly_rng_span(&rng, 24, 88);
            float bx = fly_rng_span(&rng, 8, 15), by = fly_rng_span(&rng, 8, 15);
            if (!fly_site_footing(g, base, hypotf(bx, by), &p)) continue;
            fly_v3 wall = fly_v3mk(0.28f + fly_rng_f01(&rng) * 0.10f, 0.29f, 0.33f);
            fly_v3 roof = fly_v3scale(wall, 0.6f);
            uint32_t ws = (uint32_t)(fly_rng_u64(&rng) & 0xFFFF);
            /* A tower's podium: the wider slab it comes out of, which is what
               a city block has at pavement level and what stops eighty metres
               of glass meeting a lawn in a straight line. */
            p.z = prop_footing(rt, v, e, g, p, bx, by, ang, 2.6f);
            draw_box2(rt, v, e, p, bx, by, h1, ang, wall, roof, 1, ws);
            if (h1 > 46) { /* setback tier + crown */
                draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, h1)), bx * 0.62f, by * 0.62f,
                          h1 * 0.38f, ang, wall, roof, 1, ws + 7u);
                draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, h1 * 1.38f)), 0.8f, 0.8f,
                          fly_rng_span(&rng, 6, 13), ang, fly_v3mk(0.42f, 0.36f, 0.28f),
                          fly_v3mk(0.42f, 0.36f, 0.28f), 0, 0);
                draw_warning_light(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, h1 * 1.38f + 14)), k);
            }
        }
        for (k = 0; k < 6; ++k) { /* low blocks filling the gaps between avenues */
            float ang = axis + ((float)k / 6.0f) * 2.0f * FLY_PI + fly_rng_span(&rng, -0.05f, 0.05f);
            float rr = 185.0f + fly_rng_span(&rng, -18, 18);
            fly_v3 p = fly_v3add(base, fly_v3mk(cosf(ang) * rr, sinf(ang) * rr, 0));
            float lx, ly, lh;
            if (!fly_site_footing(g, base, 21.6f, &p)) continue;
            lx = fly_rng_span(&rng, 10, 18);
            ly = fly_rng_span(&rng, 8, 12);
            lh = fly_rng_span(&rng, 6, 14);
            p.z = prop_footing(rt, v, e, g, p, lx, ly, ang, 1.6f);
            draw_box2(rt, v, e, p, lx, ly, lh, ang, fly_v3mk(0.32f, 0.30f, 0.27f),
                      fly_v3mk(0.22f, 0.21f, 0.19f), 1, (uint32_t)(fly_rng_u64(&rng) & 0xFFFF));
        }
        break;
    }
    case FLY_LOC_REFINERY: {
        fly_v3 tank = fly_v3mk(0.44f, 0.42f, 0.40f);
        fly_v3 rim = fly_v3mk(0.30f, 0.28f, 0.26f);
        fly_v3 pipe = fly_v3mk(0.24f, 0.24f, 0.26f);
        fly_v3 tp[4];
        int dry[4];
        for (k = 0; k < 4; ++k) {
            tp[k] = fly_v3add(base, fly_v3mk(95.0f + (k & 1) * 34.0f, -55.0f + (k >> 1) * 36.0f, 0));
            dry[k] = fly_site_footing(g, base, 13.0f, &tp[k]);
            if (!dry[k]) continue;
            /* the bund: a tank stands in a concrete tray that would hold what
               is in it if the tank let go, and it is wider than the tank */
            tp[k].z = prop_footing(rt, v, e, g, tp[k], 13.0f, 13.0f, 0.0f, 3.4f);
            draw_prism(rt, v, e, tp[k], 13.0f, 11.0f, 10, tank, rim);
            if (det < 2) continue;
            {   /* a tank is a drum with a walkway round the top and a ladder up
                   the side; without them it is a shape, with them it is plant */
                fly_v3 top = fly_v3add(tp[k], fly_v3mk(0, 0, 11.0f));
                int q;
                /* Plate courses. A storage tank is rolled plate welded in
                   strakes, and each course is thinner than the one below it
                   because it carries less head — so the seams are the one
                   piece of detail that also says which way is up. Two rings
                   proud of the wall by a hand, which at eleven metres is the
                   difference between a drum and a painted cylinder. */
                for (q = 1; q <= 2; ++q)
                    draw_prism(rt, v, e,
                               fly_v3add(tp[k], fly_v3mk(0, 0, (float)q * 3.4f)),
                               13.25f, 0.34f, 10, rim, fly_v3scale(rim, 1.15f));
                for (q = 0; q < 8; ++q) {
                    float a0 = (float)q / 8.0f * 2.0f * FLY_PI;
                    float a1 = (float)(q + 1) / 8.0f * 2.0f * FLY_PI;
                    prop_railing(rt, v, e,
                                 fly_v3add(top, fly_v3mk(cosf(a0) * 12.6f, sinf(a0) * 12.6f, 0)),
                                 fly_v3add(top, fly_v3mk(cosf(a1) * 12.6f, sinf(a1) * 12.6f, 0)));
                }
                for (q = 0; q < 9; ++q)   /* the ladder's rungs */
                    draw_box2(rt, v, e,
                              fly_v3add(tp[k], fly_v3mk(12.9f, 0, 0.9f + (float)q * 1.15f)),
                              0.28f, 0.42f, 0.09f, 0.0f, rim, rim, 0, 0);
                draw_box2(rt, v, e, fly_v3add(tp[k], fly_v3mk(13.1f, 0.42f, 5.4f)),
                          0.07f, 0.07f, 10.6f, 0.0f, rim, rim, 0, 0);
                draw_box2(rt, v, e, fly_v3add(tp[k], fly_v3mk(13.1f, -0.42f, 5.4f)),
                          0.07f, 0.07f, 10.6f, 0.0f, rim, rim, 0, 0);
            }
        }
        for (k = 0; k < 3; ++k) { /* pipe runs between the tanks that stand */
            fly_v3 a, b, mid;
            float len, ang2;
            if (!dry[k] || !dry[k + 1]) continue;
            a = fly_v3add(tp[k], fly_v3mk(0, 0, 4.0f));
            b = fly_v3add(tp[k + 1], fly_v3mk(0, 0, 4.0f));
            mid = fly_v3scale(fly_v3add(a, b), 0.5f);
            len = fly_v3dist(a, b) * 0.5f;
            ang2 = atan2f(b.y - a.y, b.x - a.x);
            draw_box2(rt, v, e, fly_v3mk(mid.x, mid.y, mid.z - 0.5f), len, 0.5f, 1.0f, ang2,
                      pipe, pipe, 0, 0);
        }
        { /* cracking tower + flare stack with a flickering flame */
            fly_v3 p = fly_v3add(base, fly_v3mk(-95, 45, 0));
            fly_v3 st = fly_v3add(base, fly_v3mk(-70, 70, 0));
            if (fly_site_footing(g, base, 4.5f, &p)) {
                p.z = prop_footing(rt, v, e, g, p, 4.5f, 4.5f, 0.0f, 2.6f);
                draw_prism(rt, v, e, p, 4.5f, 30.0f, 8, fly_v3mk(0.36f, 0.33f, 0.31f), rim);
            }
            if (!fly_site_footing(g, base, 1.4f, &st)) break;
            st.z = prop_footing(rt, v, e, g, st, 1.4f, 1.4f, 0.0f, 2.2f);
            draw_prism(rt, v, e, st, 1.4f, 38.0f, 6, pipe, pipe);
            uint32_t fh = fly_hash2(e->seed, (int32_t)(e->time * 9.0), 5);
            float fl = 2.6f + (float)(fh & 7) * 0.5f;
            fly_v3 fb = fly_v3add(st, fly_v3mk(0, 0, 38.0f));
            fly_v3 flame = fly_v3mk(5.0f, 2.2f, 0.55f); /* HDR emitter: blooms */
            raster_tri_hdr1(rt, v, fly_v3add(fb, fly_v3mk(-1.5f, 0, 0)), fly_v3add(fb, fly_v3mk(1.5f, 0, 0)),
                            fly_v3add(fb, fly_v3mk(0.4f * (float)((fh >> 4) & 3) - 0.6f, 0, fl)), flame);
            raster_tri_hdr1(rt, v, fly_v3add(fb, fly_v3mk(0, -1.5f, 0)), fly_v3add(fb, fly_v3mk(0, 1.5f, 0)),
                            fly_v3add(fb, fly_v3mk(0, 0.4f * (float)(fh & 3) - 0.6f, fl)), flame);
            draw_light(rt, v, fly_v3add(fb, fly_v3mk(0, 0, fl * 0.6f)), FLY_RGBA(255, 170, 70, 200), 1.3f);
            /* what a flare actually leaves behind, and the cracking tower's
               own steam: the thing that says the plant is running */
            prop_plume(rt, v, e, fly_v3add(fb, fly_v3mk(0, 0, fl)), 2.4f, 90.0f,
                       fly_v3mk(0.18f, 0.17f, 0.17f), 1.0f);
            if (det >= 1)
                prop_plume(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, 30.0f)), 3.0f, 62.0f,
                           fly_v3mk(0.82f, 0.83f, 0.86f), 0.75f);
        }
        break;
    }
    case FLY_LOC_MINE: {
        fly_v3 timber = fly_v3mk(0.30f, 0.24f, 0.18f);
        fly_v3 p = fly_v3add(base, fly_v3mk(100, 0, 0));
        if (fly_site_footing(g, base, 7.8f, &p)) {
            fly_v3 b0 = fly_v3add(base, fly_v3mk(52, 46, 0));
            /* a headframe carries the whole shaft's load: it stands on a raft,
               with the hardstand the ore comes off run out around it */
            pave(rt, v, e, g, p, 13.0f, 11.0f, 0.5f, fly_v3mk(0.28f, 0.26f, 0.24f));
            p.z = prop_footing(rt, v, e, g, p, 5.5f, 5.5f, 0.5f, 2.4f);
            draw_box2(rt, v, e, p, 5.5f, 5.5f, 19.0f, 0.5f, timber, fly_v3scale(timber, 0.7f), 0, 0);
            draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, 19.0f)), 7.0f, 4.0f, 3.5f, 0.5f,
                      fly_v3mk(0.36f, 0.30f, 0.24f), fly_v3scale(timber, 0.6f), 0, 0);
            if (fly_site_footing(g, base, 3.0f, &b0)) { /* inclined conveyor to the heaps */
                fly_v3 a0 = fly_v3add(p, fly_v3mk(4, 3, 15));
                fly_v3 side;
                b0.z += 2.0f;
                side = fly_v3scale(fly_v3norm(fly_v3cross(fly_v3sub(b0, a0), fly_v3mk(0, 0, 1))), 1.4f);
                wall_quad(rt, v, e, fly_v3sub(a0, side), fly_v3add(a0, side), fly_v3add(b0, side),
                          fly_v3sub(b0, side), fly_v3mk(0.26f, 0.24f, 0.22f), 1.0f);
            }
        }
        for (k = 0; k < 3; ++k) { /* ore heaps */
            fly_v3 h = fly_v3add(base, fly_v3mk(42.0f + k * 17.0f, 52.0f + (k & 1) * 14.0f, 0));
            if (!fly_site_footing(g, base, 8.0f - (float)k, &h)) continue;
            draw_cone(rt, v, e, h, 8.0f - k, 5.5f - k, 8, fly_v3mk(0.23f, 0.19f, 0.15f));
            if (k == 0 && det >= 1)   /* dust off the heap the conveyor feeds */
                prop_plume(rt, v, e, fly_v3add(h, fly_v3mk(0, 0, 5.0f)), 3.2f, 26.0f,
                           fly_v3mk(0.44f, 0.38f, 0.30f), 0.55f);
        }
        break;
    }
    case FLY_LOC_RUINS: {
        fly_v3 stone = fly_v3mk(0.34f, 0.31f, 0.38f);
        for (k = 0; k < 7; ++k) {
            float ang = fly_rng_span(&rng, 0, 2 * FLY_PI);
            fly_v3 p = fly_v3add(base, fly_v3mk(cosf(ang) * fly_rng_span(&rng, 75, 160),
                                                sinf(ang) * fly_rng_span(&rng, 75, 160), 0));
            if (!fly_site_footing(g, base, 3.1f, &p)) continue;
            if (fly_rng_f01(&rng) < 0.65f) {
                float ch = fly_rng_span(&rng, 5, 18);
                draw_prism(rt, v, e, p, 2.2f, ch, 6, stone, fly_v3scale(stone, 0.8f));
                if (fly_rng_f01(&rng) < 0.5f)
                    draw_cone(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, ch)), 2.2f,
                              fly_rng_span(&rng, 1, 3), 6, fly_v3scale(stone, 0.9f));
            } else {
                draw_box2(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, -0.8f)),
                          fly_rng_span(&rng, 5, 9), 2.0f, 2.4f, ang, stone,
                          fly_v3scale(stone, 0.85f), 0, 0);
            }
        }
        for (k = 0; k < 3; ++k) { /* wall fragments */
            float ang = fly_rng_span(&rng, 0, 2 * FLY_PI);
            fly_v3 p = fly_v3add(base, fly_v3mk(cosf(ang) * 120.0f, sinf(ang) * 120.0f, 0));
            if (!fly_site_footing(g, base, 16.0f, &p)) continue;
            draw_box2(rt, v, e, p, fly_rng_span(&rng, 8, 16), 1.2f, fly_rng_span(&rng, 4, 9),
                      ang + 0.7f, fly_v3scale(stone, 0.9f), fly_v3scale(stone, 0.7f), 0, 0);
        }
        { /* the monolith: dark slab with a glowing rune seam */
            fly_v3 p = fly_v3add(base, fly_v3mk(-95, -60, 0));
            if (!fly_site_footing(g, base, 3.9f, &p)) break;
            draw_box2(rt, v, e, p, 3.6f, 1.4f, 26.0f, 0.9f, fly_v3mk(0.09f, 0.08f, 0.12f),
                      fly_v3mk(0.07f, 0.06f, 0.10f), 0, 0);
            float glow = 0.55f + 0.45f * sinf((float)e->time * 0.9f + (float)li);
            fly_v3 rune = fly_v3scale(fly_v3mk(1.4f, 0.55f, 2.6f), 0.4f + 1.6f * glow);
            float cy2 = cosf(0.9f), sy2 = sinf(0.9f);
            raster_tri_hdr1(rt, v, yawp(p, -0.5f, -1.55f, 4, cy2, sy2), yawp(p, 0.5f, -1.55f, 4, cy2, sy2),
                            yawp(p, 0.2f, -1.55f, 22, cy2, sy2), rune);
            raster_tri_hdr1(rt, v, yawp(p, -0.5f, -1.55f, 4, cy2, sy2), yawp(p, 0.2f, -1.55f, 22, cy2, sy2),
                            yawp(p, -0.3f, -1.55f, 22, cy2, sy2), rune);
            draw_light(rt, v, fly_v3add(p, fly_v3mk(0, 0, 27.5f)), FLY_RGBA(190, 110, 255, (int)(200 * glow)), 1.2f);
        }
        break;
    }
    case FLY_LOC_SKYPORT: {
        fly_v3 steel = fly_v3mk(0.34f, 0.36f, 0.42f);
        fly_v3 deckc = fly_v3mk(0.40f, 0.42f, 0.48f);
        fly_v3 dp = fly_v3add(base, fly_v3mk(0, 120, 0));
        float dh = 46.0f;
        if (!fly_site_footing(g, base, 26.0f, &dp)) break;
        /* The four legs, each on the pad block it needs to be standing on
         * rather than pushed into the turf, and the whole footprint hard-
         * standing between them: a forty-six metre column carries a deck the
         * size of a football pitch, and what is under it is not lawn. */
        pave(rt, v, e, g, dp, 26.0f, 15.0f, 0.0f, fly_v3mk(0.30f, 0.30f, 0.31f));
        for (k = 0; k < 4; ++k) {
            fly_v3 leg = fly_v3add(dp, fly_v3mk((k & 1) ? 20.0f : -20.0f,
                                                (k & 2) ? 10.0f : -10.0f, 0));
            leg.z = prop_footing(rt, v, e, g, leg, 3.2f, 3.2f, 0.0f, 2.4f);
            draw_prism(rt, v, e, leg, 3.2f, dh - (leg.z - dp.z), 6, steel, steel);
        }
        draw_box2(rt, v, e, fly_v3add(dp, fly_v3mk(0, 0, dh)), 34.0f, 18.0f, 3.5f, 0.0f,
                  deckc, fly_v3scale(deckc, 0.85f), 0, 0);
        draw_box2(rt, v, e, fly_v3add(dp, fly_v3mk(-24, 0, dh + 3.5f)), 3.0f, 3.0f, 16.0f, 0.0f,
                  steel, steel, 0, 0);
        draw_box2(rt, v, e, fly_v3add(dp, fly_v3mk(-24, 0, dh + 19.5f)), 4.6f, 4.6f, 3.4f, 0.0f,
                  fly_v3mk(0.10f, 0.16f, 0.20f), fly_v3scale(steel, 0.8f), 0, 0);
        draw_warning_light(rt, v, e, fly_v3add(dp, fly_v3mk(-24, 0, dh + 24.5f)), li);
        if (det >= 1) {
            /* the deck gets an edge you could stand at, a stair down to the
               ground, and a dish that is pointing somewhere different each
               time you look at it */
            fly_v3 dk = fly_v3add(dp, fly_v3mk(0, 0, dh + 3.5f));
            fly_v3 c0 = fly_v3add(dk, fly_v3mk(-34, -18, 0)), c1 = fly_v3add(dk, fly_v3mk(34, -18, 0));
            fly_v3 c2 = fly_v3add(dk, fly_v3mk(34, 18, 0)), c3 = fly_v3add(dk, fly_v3mk(-34, 18, 0));
            float sl = (float)e->time * 0.11f + (float)li;
            fly_v3 dishp = fly_v3add(dk, fly_v3mk(24.0f, -9.0f, 0));
            int st;
            prop_railing(rt, v, e, c0, c1);
            prop_railing(rt, v, e, c1, c2);
            prop_railing(rt, v, e, c2, c3);
            prop_railing(rt, v, e, c3, c0);
            /* The stair. Treads alone read as a dotted line of floating tiles
             * from anywhere but alongside, so the stringers under them and the
             * handrail over them are what make it a flight rather than a hint
             * of one — and they are the parts that carry at range. */
            {
                float run = 16.0f, rise = dh + 3.5f;
                fly_v3 top = fly_v3add(dp, fly_v3mk(30.0f, -22.0f, rise));
                fly_v3 foot = fly_v3add(dp, fly_v3mk(30.0f + run, -22.0f, 0.0f));
                int sd, q;
                /* draw_box2 has no pitch, so each rake is a run of short
                   segments stepped down the slope — eight reads as one line */
                for (sd = 0; sd < 2; ++sd) {
                    float oy = sd ? 1.7f : -1.7f;
                    for (q = 0; q < 8; ++q) {
                        float f = ((float)q + 0.5f) / 8.0f;
                        fly_v3 s0 = fly_v3lerp(top, foot, f);
                        draw_box2(rt, v, e, fly_v3mk(s0.x, s0.y + oy, s0.z - 0.55f),
                                  run / 16.0f + 0.14f, 0.14f, 0.55f, 0.0f,
                                  fly_v3scale(steel, 0.8f), fly_v3scale(steel, 0.9f), 0, 0);
                        draw_box2(rt, v, e, fly_v3mk(s0.x, s0.y + oy, s0.z + 1.0f),
                                  run / 16.0f + 0.14f, 0.07f, 0.10f, 0.0f,
                                  steel, steel, 0, 0);
                        if (!(q & 1))   /* a stanchion every other step */
                            draw_box2(rt, v, e, fly_v3mk(s0.x, s0.y + oy, s0.z),
                                      0.07f, 0.07f, 1.05f, 0.0f, steel, steel, 0, 0);
                    }
                }
                for (st = 0; st < 14; ++st) {
                    float f = (float)st / 13.0f;
                    fly_v3 s0 = fly_v3lerp(top, foot, f);
                    draw_box2(rt, v, e, s0, 0.9f, 1.7f, 0.16f, 0.0f, steel,
                              fly_v3scale(steel, 1.1f), 0, 0);
                }
            }
            draw_box2(rt, v, e, fly_v3add(dishp, fly_v3mk(0, 0, 1.4f)), 0.5f, 0.5f, 2.8f,
                      0.0f, steel, steel, 0, 0);
            draw_cone(rt, v, e, fly_v3add(dishp, fly_v3mk(cosf(sl) * 1.2f, sinf(sl) * 1.2f, 4.2f)),
                      2.6f, 1.5f, 8, fly_v3mk(0.62f, 0.63f, 0.66f));
        }
        for (k = 0; k < 6; ++k) { /* deck edge lights */
            float pulse = 0.6f + 0.4f * sinf((float)e->time * 2.0f + (float)k);
            int a = (int)(fly_clampf((0.5f + (1.0f - e->day)) * pulse, 0, 1) * 230.0f);
            draw_light(rt, v, fly_v3add(dp, fly_v3mk(-34.0f + (float)k * 13.6f, 18.5f, dh + 4.2f)),
                       FLY_RGBA(120, 225, 255, a), 0.8f);
        }
        break;
    }
    default: { /* outpost: huts with pitched roofs, drums, mast, solar array */
        fly_v3 hut = fly_v3mk(0.33f, 0.31f, 0.26f);
        fly_v3 roof = fly_v3mk(0.45f, 0.30f, 0.20f);
        fly_v3 row0 = base, row1 = base;
        int rows = 0;
        for (k = 0; k < 4; ++k) {
            fly_v3 p = fly_v3add(base, fly_v3mk(80.0f + (k & 1) * 26.0f, 34.0f - (k >> 1) * 30.0f, 0));
            if (!fly_site_footing(g, base, 7.9f, &p)) continue;
            float hh = fly_rng_span(&rng, 3.0f, 4.2f);
            float yaw2 = 0.3f * (float)k;
            /* Huts were the one kind that stood on bare turf with not even a
               contact shadow: four boxes and four roofs on a lawn. Each gets
               its slab and its step out of the door. */
            p.z = prop_footing(rt, v, e, g, p, 6.5f, 4.5f, yaw2, 1.5f);
            if (!rows) row0 = p;
            row1 = p;
            ++rows;
            draw_box2(rt, v, e, p, 6.5f, 4.5f, hh, yaw2, hut, hut, 1, (uint32_t)k * 13u);
            float cy2 = cosf(yaw2), sy2 = sinf(yaw2);
            fly_v3 r0 = yawp(p, -6.8f, 0, hh + 2.2f, cy2, sy2);
            fly_v3 r1 = yawp(p, 6.8f, 0, hh + 2.2f, cy2, sy2);
            tri_shaded(rt, v, e, r0, r1, yawp(p, 6.8f, 4.8f, hh - 0.2f, cy2, sy2), roof, 1);
            tri_shaded(rt, v, e, r0, yawp(p, 6.8f, 4.8f, hh - 0.2f, cy2, sy2),
                       yawp(p, -6.8f, 4.8f, hh - 0.2f, cy2, sy2), roof, 1);
            tri_shaded(rt, v, e, r1, r0, yawp(p, -6.8f, -4.8f, hh - 0.2f, cy2, sy2), roof, 1);
            tri_shaded(rt, v, e, r1, yawp(p, -6.8f, -4.8f, hh - 0.2f, cy2, sy2),
                       yawp(p, 6.8f, -4.8f, hh - 0.2f, cy2, sy2), roof, 1);
        }
        /* the path between the huts, so a row of four is a row rather than
           four objects at the same spacing */
        if (rows > 1) prop_way(rt, v, e, g, row0, row1, 2.6f, FLY_TRACK_GRAVEL, 1);
        { /* fuel drums, all three on the one hardstand rather than in the grass */
            fly_v3 d0 = fly_v3add(base, fly_v3mk(68.5f, -32.0f, 0));
            if (fly_site_footing(g, base, 7.6f, &d0)) {
                float dz = pave(rt, v, e, g, d0, 8.0f, 3.6f, 0.0f,
                                fly_v3mk(0.30f, 0.29f, 0.27f));
                for (k = 0; k < 3; ++k)
                    draw_prism(rt, v, e,
                               fly_v3mk(d0.x - 4.5f + (float)k * 4.5f, d0.y, dz),
                               1.6f, 2.4f, 7, fly_v3mk(0.45f, 0.20f, 0.12f),
                               fly_v3mk(0.30f, 0.14f, 0.09f));
            }
        }
        { /* comms mast + strobe, tilted solar array */
            fly_v3 p = fly_v3add(base, fly_v3mk(58, -48, 0));
            fly_v3 sp = fly_v3add(base, fly_v3mk(96, -20, 0));
            if (fly_site_footing(g, base, 0.7f, &p)) {
                p.z = prop_footing(rt, v, e, g, p, 0.6f, 0.6f, 0.0f, 1.5f);
                draw_box2(rt, v, e, p, 0.5f, 0.5f, 16.0f, 0, fly_v3mk(0.5f, 0.5f, 0.55f),
                          fly_v3mk(0.5f, 0.5f, 0.55f), 0, 0);
                draw_warning_light(rt, v, e, fly_v3add(p, fly_v3mk(0, 0, 17.0f)), li);
            }
            if (fly_site_footing(g, base, 2.0f, &sp)) {
                sp.z = prop_footing(rt, v, e, g, sp, 5.4f, 3.4f, 0.0f, 1.2f);
                wall_quad(rt, v, e, fly_v3add(sp, fly_v3mk(-5, -3, 1.0f)), fly_v3add(sp, fly_v3mk(5, -3, 1.0f)),
                          fly_v3add(sp, fly_v3mk(5, 3, 4.2f)), fly_v3add(sp, fly_v3mk(-5, 3, 4.2f)),
                          fly_v3mk(0.06f, 0.10f, 0.22f), 1.0f);
            }
        }
        break;
    }
    }
}

/* Capture one settlement into its column grid — see "what a settlement
 * occludes". Runs `draw_location` as a caster, so what lands in the grid is
 * exactly the geometry that would cast a shadow: no decals, no glass, no
 * translucent discs, and none of the near-field detail gates, which a caster
 * already bypasses. Nothing is drawn; every primitive is diverted at the same
 * seam the shadow pass uses.
 *
 * The dummy cascade exists for `object_worth_drawing`, which asks a caster for
 * a distance from the coarsest cascade's centre rather than a frustum test.
 * Centred on the site with a half-extent that reaches past the widest clearing,
 * so the answer is the site's own geometry and nothing about a camera. */
static void site_occ_build(const fly_game *g, int li) {
    fly__siteocc *o = &g_site_occ[li];
    const fly_location *L = &g->world.loc[li];
    fly__shadow dummy;
    fly__shadow *save_cast = g_shadow_cast;
    fly_render_target rt;
    fly__view wide;
    fly__env e;
    int i, c;
    if (o->built) return;
    if (!o->top) {
        o->top = (float *)malloc((size_t)FLY_OCC_N * FLY_OCC_N * sizeof(float));
        if (!o->top) return;
    }
    for (i = 0; i < FLY_OCC_N * FLY_OCC_N; ++i) o->top[i] = -1e30f;
    o->ox = L->pos.e - FLY_OCC_N * FLY_OCC_LAT * 0.5f;
    o->oy = L->pos.n - FLY_OCC_N * FLY_OCC_LAT * 0.5f;

    memset(&dummy, 0, sizeof dummy);
    dummy.active = 1;
    for (c = 0; c < FLY_SM_CASCADES; ++c) {
        dummy.center[c] = fly_v3mk(L->pos.e, L->pos.n, 0.0f);
        dummy.half[c] = FLY_OCC_N * FLY_OCC_LAT;
    }
    /* `draw_location` measures a distance from the view to decide whether the
     * site is worth drawing at all, and that one is not on the caster's path.
     * A view standing at the site answers it the same way every time, whoever
     * is looking and from where. */
    memset(&wide, 0, sizeof wide);
    wide.right = fly_v3mk(1, 0, 0);
    wide.up = fly_v3mk(0, 0, 1);
    wide.fwd = fly_v3mk(0, 1, 0);
    wide.pos = fly_v3mk(L->pos.e, L->pos.n,
                        fly_world_ground(&g->world, L->pos.e, L->pos.n));
    wide.sx = wide.sy = 1.0f;
    wide.aa = 1;

    /* The capture makes its own everything, and that is the point rather than
     * a convenience: what a settlement occludes is a property of the world, so
     * it must not depend on the frame that happened to ask for it. Its own
     * view, and the top graphics rung whatever the frame is running — a site
     * captured at low detail and read back at high would put the quality
     * setting into a cache that outlives it. */
    if (fly_rt_init(&rt, 8, 8) != 0) return;
    e = make_env(g, &wide);
    e.lod = lod_for(FLY_QUALITY_ULTRA);
    g_shadow_cast = &dummy;
    g_occ_build = o;
    draw_location(&rt, &wide, &e, g, li);
    g_occ_build = NULL;
    g_shadow_cast = save_cast;
    fly_rt_free(&rt);
    o->built = 1;
    g_occ_any = 1;
}

/* Capture whatever sites the occlusion window is about to trace against.
 *
 * Only the ones it can reach: the window fills within FLY_AO_FILL of the
 * camera and a ray from it looks FLY_AO_R further, so anything past that
 * cannot change a single sample. On the usual frame — over open country, or
 * high enough that no lattice point is being filled at all — this captures
 * nothing and costs a loop over the site list.
 *
 * Dropped and rebuilt when the world underneath changes. The grids are a pure
 * function of the seed and the site, so nothing else can invalidate them —
 * a settlement does not move, and what it builds does not depend on who is
 * looking or on what the player has done to it. */
static void site_occ_sync(const fly_game *g, float x, float y) {
    const float reach = FLY_AO_FILL + FLY_AO_R + FLY_OCC_N * FLY_OCC_LAT * 0.5f;
    int li;
    if (g_occ_game != g || g_occ_seed != g->world.seed) {
        fly_render_ao_reset();
        g_occ_game = g;
        g_occ_seed = g->world.seed;
    }
    for (li = 0; li < g->world.nloc; ++li) {
        float dx = g->world.loc[li].pos.e - x, dy = g->world.loc[li].pos.n - y;
        if (g_site_occ[li].built) continue;
        if (fabsf(dx) > reach || fabsf(dy) > reach) continue;
        site_occ_build(g, li);
    }
}

/* ---------------- craft models ---------------- */

typedef struct {
    fly_v3 X, Y, Z, P;
} fly__frame;

static fly_v3 fp(const fly__frame *f, float x, float y, float z) {
    return fly_v3add(f->P, fly_v3add(fly_v3scale(f->X, x),
                                     fly_v3add(fly_v3scale(f->Y, y), fly_v3scale(f->Z, z))));
}

/* small body-frame box (module pods, tanks, plating) */
static void model_box(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly__frame *f, float cx, float cy, float cz,
                      float hx, float hy, float hz, fly_v3 alb, float spec, float gloss) {
    fly_v3 p[8];
    int i;
    for (i = 0; i < 8; ++i)
        p[i] = fp(f, cx + ((i & 1) ? hx : -hx), cy + ((i & 2) ? hy : -hy),
                  cz + ((i & 4) ? hz : -hz));
    static const int q[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 2, 6, 4 },
                                 { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 } };
    for (i = 0; i < 6; ++i) {
        tri_spec(rt, v, e, p[q[i][0]], p[q[i][1]], p[q[i][2]], alb, 1, spec, gloss);
        tri_spec(rt, v, e, p[q[i][0]], p[q[i][2]], p[q[i][3]], alb, 1, spec, gloss);
    }
}

/* wing-mounted pod with a forward nose (gun pods, drop tanks) */
static void model_pod(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly__frame *f, float cx, float cy, float cz,
                      float len, float r, fly_v3 alb, int barrel) {
    model_box(rt, v, e, f, cx, cy, cz, len, r, r, alb, 0.5f, 30.0f);
    tri_spec(rt, v, e, fp(f, cx + len + r * 2.2f, cy, cz), fp(f, cx + len, cy - r, cz + r),
             fp(f, cx + len, cy + r, cz + r), alb, 1, 0.5f, 30.0f);
    tri_spec(rt, v, e, fp(f, cx + len + r * 2.2f, cy, cz), fp(f, cx + len, cy + r, cz - r),
             fp(f, cx + len, cy - r, cz - r), alb, 1, 0.5f, 30.0f);
    if (barrel)
        model_box(rt, v, e, f, cx + len + r * 2.0f, cy, cz, r * 2.6f, 0.06f, 0.06f,
                  fly_v3mk(0.08f, 0.09f, 0.10f), 0.8f, 60.0f);
}

/* ---------------- solid parts ----------------
 *
 * Every part of an aircraft used to be a bare triangle: a fin was one triangle
 * standing on edge, a tailplane one delta, a wing two. Three things follow from
 * that and all three showed on the studio plates.
 *
 *  - A surface with no thickness seen edge-on is a line, so the fin vanished
 *    out of the side elevation and both wings vanished head-on.
 *  - One triangle has one normal. A wing that should catch the sun along the
 *    top and sit in its own shade underneath was a single flat tone from every
 *    angle, and no amount of work in the shading model can fix a shape that
 *    has only one face to shade.
 *  - A surface with no edge has no silhouette to catch a rim of sky, and that
 *    grazing rim is most of what makes painted metal read as metal rather than
 *    as coloured paper.
 *
 * So the parts are solids. Three builders cover both airframes: a hull lofted
 * through a run of rings, an aerofoil section to make the rings out of for
 * anything meant to fly, and a round tube for the struts and wheels. Nothing
 * here is a special case of an aeroplane — the same three build the drone.
 *
 * Every ring lays its points at half-step angles, so none of them lands on the
 * y = 0 centreline and the two halves of a body are the same vertices with y
 * negated. That is not tidiness: a ring with a vertex *on* the centreline
 * rounds one side of the nose a quarter-step differently from the other, which
 * is small enough to survive review and large enough to see. */

/* One station of a body: an ellipse with its own half-depth above and below
 * the centre line, so a fuselage can be flat-bottomed and round-backed. */
static void body_ring(const fly__frame *f, float x, float z, float hy,
                      float hup, float hdn, int n, fly_v3 *out) {
    int k;
    for (k = 0; k < n; ++k) {
        float a = ((float)k + 0.5f) * (2.0f * FLY_PI / (float)n);
        float ca = cosf(a), sa = sinf(a);
        out[k] = fp(f, x, hy * sa, z + (ca >= 0.0f ? hup : hdn) * ca);
    }
}

/* The half-thickness of a symmetric aerofoil at chord fraction `u`, normalised
 * to 1 at its thickest.
 *
 * This is the NACA four-digit thickness distribution, which is a formula and
 * not a taste: a square-root nose, so the leading edge has a real radius
 * instead of a corner, and a polynomial tail that runs out fine. It replaced an
 * eight-entry table, and the reason is the table's shape rather than its size —
 * a table can only be resampled between the points it has, so asking for a
 * finer section got more vertices along the same straight lines. The nose is
 * where all the curvature is and where a faceted wing shows first, and a table
 * cannot put a point there that it does not already have.
 *
 * The trailing edge is closed off at zero rather than at the two per cent the
 * polynomial leaves, because the loft treats coincident points as a collapsed
 * edge and a fine trailing edge is what these surfaces have. */
static float foil_thick(float u) {
    float s = sqrtf(u);
    float y = 2.969f * s - 1.260f * u - 3.516f * u * u +
              2.843f * u * u * u - 1.015f * u * u * u * u;
    return y > 0.0f ? y : 0.0f;
}

/* An aerofoil section as points around the chord: a rounded nose, thickest
 * around 30% back, and a fine trailing edge. Symmetric, because none of these
 * surfaces is cambered and camber would need a normal per vertex to be worth
 * the points it costs.
 *
 * Index 0 is the leading edge and index n/2 the trailing edge, so k and n-k are
 * the same point reflected through the chord plane. That is the symmetry the
 * loft's diagonal flip is built on — see loft_hull — and it is why `n` is
 * forced even.
 *
 * The chord stations are cosine-spaced, which puts the points where the
 * curvature is: uniform spacing spends half of a sixteen-point section on the
 * flat run aft of mid-chord and still rounds the nose with two.
 *
 * `up` is squared off against the chord before it is used, so a swept or
 * tapered panel comes out as thick as it is and not thicker; its *length* is
 * kept, which is what lets a caller hand in a scaled frame axis and have the
 * thickness scale with the rest of the model. */
static void foil_ring(fly_v3 le, fly_v3 te, fly_v3 up, float half, int n, fly_v3 *out) {
    fly_v3 ch = fly_v3sub(te, le);
    float cc = fly_v3dot(ch, ch), ul = fly_v3len(up);
    int m, k;
    if (n < 4) n = 4;
    n &= ~1;
    m = n / 2;
    if (cc > 1e-9f) up = fly_v3sub(up, fly_v3scale(ch, fly_v3dot(up, ch) / cc));
    up = fly_v3scale(fly_v3norm(up), ul);
    for (k = 0; k < n; ++k) {
        int j = k <= m ? k : n - k;            /* fold onto the upper surface */
        float sign = k <= m ? 1.0f : -1.0f;
        float u = 0.5f * (1.0f - cosf(FLY_PI * (float)j / (float)m));
        float t = (j == 0 || j == m) ? 0.0f : foil_thick(u) * sign;
        out[k] = fly_v3add(le, fly_v3add(fly_v3scale(ch, u),
                                         fly_v3scale(up, t * half)));
    }
}

/* The surface normal at every point of a lofted grid, from the grid itself.
 *
 * A loft is a parametric surface sampled on a rectangular lattice, so the
 * normal at a sample is the cross product of its two tangents — central
 * differences around the ring and along the hull, one-sided at the ends. That
 * is the *surface's* normal rather than an average of the facets standing in
 * for it, which matters at a collapsed ring: a nose cone's tip has no ring to
 * difference and every facet meeting there has a different normal, so it takes
 * the direction out of the hull's own axis instead and the nose stays smooth
 * rather than picking up a star.
 *
 * Outward is decided per station against that station's centre, which is what
 * makes it right for a shape that is not convex end to end — a fuselage with a
 * waist gets an outward normal at the waist, where a single hull centroid would
 * hand it an inward one. */
static void loft_normals(const fly_v3 *ring, int ns, int n, fly_v3 *out) {
    int i, k;
    for (i = 0; i < ns; ++i) {
        const fly_v3 *r = ring + (size_t)i * n;
        const fly_v3 *pr = ring + (size_t)(i > 0 ? i - 1 : i) * n;
        const fly_v3 *nx = ring + (size_t)(i + 1 < ns ? i + 1 : i) * n;
        fly_v3 c = fly_v3zero();
        for (k = 0; k < n; ++k) c = fly_v3add(c, r[k]);
        c = fly_v3scale(c, 1.0f / (float)n);
        for (k = 0; k < n; ++k) {
            int km = k == 0 ? n - 1 : k - 1, kp = k + 1 == n ? 0 : k + 1;
            fly_v3 du = fly_v3sub(r[kp], r[km]);
            fly_v3 dv = fly_v3sub(nx[k], pr[k]);
            fly_v3 nn = fly_v3cross(du, dv);
            fly_v3 rad = fly_v3sub(r[k], c);
            float l = fly_v3len(nn);
            if (l < 1e-9f) {
                /* a collapsed ring, or a station where the hull does not move:
                 * fall back to the direction out of the axis, and to the axis
                 * itself at a tip where even that is nothing */
                nn = rad;
                if (fly_v3dot(nn, nn) < 1e-12f) nn = fly_v3sub(r[k], pr[k]);
                if (fly_v3dot(nn, nn) < 1e-12f) nn = fly_v3mk(0, 0, 1);
            } else {
                nn = fly_v3scale(nn, 1.0f / l);
            }
            if (fly_v3dot(nn, rad) < 0.0f) nn = fly_v3scale(nn, -1.0f);
            out[(size_t)i * n + k] = fly_v3norm(nn);
        }
    }
}

/* Loft a closed hull through `ns` rings of `n` points and cap both ends. A
 * ring collapsed to a point turns its bay into a fan, so a nose cone, the
 * barrel behind it and the tail cone all come out of one call.
 *
 * The barrel is smooth-shaded off `loft_normals` and the two caps are not, and
 * that split is the shape rather than a shortcut: the side of a hull is a
 * curved surface sampled coarsely, and the end of one is a flat disc that
 * genuinely has one normal. Smoothing across the rim would round off an edge
 * that is really there — a wheel would lose its tread line, a tank its ends. */
static void loft_hull(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly_v3 *ring, int ns, int n, fly_v3 alb,
                      float spec, float gloss) {
    fly_v3 nrm[FLY_LOFT_MAX * FLY_RING_N];
    int i, k, end;
    int smooth = !g_shadow_cast && ns >= 2 && ns <= FLY_LOFT_MAX && n <= FLY_RING_N;
    if (smooth) loft_normals(ring, ns, n, nrm);
    for (i = 0; i + 1 < ns; ++i) {
        const fly_v3 *a = ring + (size_t)i * n, *b = ring + (size_t)(i + 1) * n;
        const fly_v3 *na = nrm + (size_t)i * n, *nb = nrm + (size_t)(i + 1) * n;
        for (k = 0; k < n; ++k) {
            int k2 = k + 1 == n ? 0 : k + 1;
            fly_v3 q[3];
            /* Which way the quad is split has to mirror with the ring, or the
             * model is lopsided by a triangle everywhere. Every ring here has
             * index k and index n-1-k as mirror partners, so quad k pairs with
             * quad n-2-k — and a single diagonal maps to the *other* diagonal
             * under that. Two non-planar triangulations of the same four
             * corners have two different face normals, and a face normal is
             * what the specular is evaluated against, so the canopy came back
             * with a highlight 118/255 brighter on one side than the other.
             * Flipping the diagonal across the centreline makes the pair agree.
             *
             * The two quads that straddle the centreline are their own mirror
             * partners and neither of their diagonals lies in the plane, so
             * they cannot be split symmetrically at all. They sit along the
             * spine and the keel where the surface is flattest and the two
             * diagonals very nearly coincide, which is why what is left is
             * under a fifth of a unit. */
#define FLY_LOFT_TRI(P0, P1, P2, N0, N1, N2)                                  \
            do {                                                              \
                if (smooth) { q[0] = (N0); q[1] = (N1); q[2] = (N2); }         \
                tri_lit_n(rt, v, e, (P0), (P1), (P2), smooth ? q : NULL,       \
                          alb, 1, spec, gloss);                                \
            } while (0)
            if (k * 2 < n) {
                if (fly_v3dist(a[k], a[k2]) > 1e-4f)
                    FLY_LOFT_TRI(a[k], a[k2], b[k2], na[k], na[k2], nb[k2]);
                if (fly_v3dist(b[k], b[k2]) > 1e-4f)
                    FLY_LOFT_TRI(a[k], b[k2], b[k], na[k], nb[k2], nb[k]);
            } else {
                if (fly_v3dist(a[k], a[k2]) > 1e-4f)
                    FLY_LOFT_TRI(a[k], a[k2], b[k], na[k], na[k2], nb[k]);
                if (fly_v3dist(b[k], b[k2]) > 1e-4f)
                    FLY_LOFT_TRI(a[k2], b[k2], b[k], na[k2], nb[k2], nb[k]);
            }
#undef FLY_LOFT_TRI
        }
    }
    for (end = 0; end < 2; ++end) {
        const fly_v3 *r = ring + (size_t)(end && ns > 1 ? ns - 1 : 0) * n;
        fly_v3 c = fly_v3zero();
        for (k = 0; k < n; ++k) c = fly_v3add(c, r[k]);
        c = fly_v3scale(c, 1.0f / (float)n);
        for (k = 0; k < n; ++k) {
            int k2 = k + 1 == n ? 0 : k + 1;
            if (fly_v3dist(r[k], r[k2]) > 1e-4f)
                tri_spec(rt, v, e, c, r[k], r[k2], alb, 1, spec, gloss);
        }
        if (ns < 2) break;
    }
}

/* A body: rings of (x, z, half-width, half-depth up, half-depth down) lofted
 * along the frame's x axis. */
static void model_body(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly__frame *f, const float (*st)[5], const int *sel,
                       int ns, int n, fly_v3 alb, float spec, float gloss) {
    fly_v3 ring[FLY_LOFT_MAX * FLY_RING_N];
    int i;
    if (ns > FLY_LOFT_MAX) ns = FLY_LOFT_MAX;
    if (n > FLY_RING_N) n = FLY_RING_N;
    for (i = 0; i < ns; ++i) {
        const float *s = st[sel ? sel[i] : i];
        body_ring(f, s[0], s[1], s[2], s[3], s[4], n, ring + (size_t)i * n);
    }
    loft_hull(rt, v, e, ring, ns, n, alb, spec, gloss);
}

/* A flying surface: an aerofoil section at every station, lofted and closed. */
typedef struct { fly_v3 le, te; float half; } fly__foilst;

static void model_foil(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly__foilst *st, int ns, fly_v3 up, int n,
                       fly_v3 alb, float spec, float gloss) {
    fly_v3 ring[FLY_LOFT_MAX * FLY_FOIL_N];
    int i;
    if (ns > FLY_LOFT_MAX) ns = FLY_LOFT_MAX;
    if (n > FLY_FOIL_N) n = FLY_FOIL_N;
    for (i = 0; i < ns; ++i)
        foil_ring(st[i].le, st[i].te, up, st[i].half, n, ring + (size_t)i * n);
    loft_hull(rt, v, e, ring, ns, n, alb, spec, gloss);
}

/* A round tube between two points: gear legs, rotor arms, skids and masts —
 * and the wheels, which are short fat ones with their axis across the aircraft.
 *
 * `across` is the body's own sideways axis, and passing it is what makes a
 * tube on the centreline symmetric. The cross-section is laid out with one
 * basis vector along `across` and the other square to it, so mirroring the
 * body negates exactly one of them and the half-step ring maps onto itself —
 * for an even `n`. Built off an arbitrary basis instead, a five-sided nose-leg
 * came out 43/255 different between its two halves, which is what an odd
 * polygon straddling a mirror plane looks like. A tube whose axis *is* the
 * sideways one (a wheel) has its ring in the plane of symmetry already, so the
 * degenerate case needs no special care beyond not dividing by zero. */
static void model_tube(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 a, fly_v3 b, float r, int n, fly_v3 across,
                       fly_v3 alb, float spec, float gloss) {
    fly_v3 ring[2 * FLY_RING_N], ax = fly_v3sub(b, a), u, w;
    int k;
    if (n > FLY_RING_N) n = FLY_RING_N;
    if (fly_v3dot(ax, ax) < 1e-8f) return;
    ax = fly_v3norm(ax);
    w = fly_v3sub(across, fly_v3scale(ax, fly_v3dot(across, ax)));
    if (fly_v3dot(w, w) < 1e-6f) {   /* the axis is the sideways one */
        w = fabsf(ax.z) < 0.9f ? fly_v3mk(0, 0, 1) : fly_v3mk(1, 0, 0);
        w = fly_v3cross(ax, w);
    }
    w = fly_v3norm(w);
    u = fly_v3cross(w, ax);
    for (k = 0; k < n; ++k) {
        float t = ((float)k + 0.5f) * (2.0f * FLY_PI / (float)n);
        fly_v3 o = fly_v3add(fly_v3scale(u, cosf(t) * r), fly_v3scale(w, sinf(t) * r));
        ring[k] = fly_v3add(a, o);
        ring[n + k] = fly_v3add(b, o);
    }
    loft_hull(rt, v, e, ring, 2, n, alb, spec, gloss);
}

/* How much of an aircraft is worth building at the size it lands on screen.
 * The near model is a few hundred triangles and a busy circuit can have a
 * dozen aircraft in it, so the section counts come down with the pixels: the
 * silhouette survives to the last step, and the control surfaces, the gear and
 * the rounding go first. The shadow pass takes the middle one at any distance,
 * because a caster contributes a silhouette and nothing else. */
static int craft_lod(const fly__view *v, const fly__env *e, fly_v3 p, float size) {
    float px;
    if (g_shadow_cast) return 1;
    /* pxscale is pixels per world unit at one metre under perspective and
     * pixels per world unit outright under an orthographic camera, which is
     * what the studio plates use — divide by the range in one case and not in
     * the other, or every plate is drawn at the coarsest model there is */
    px = v->ortho ? size * e->pxscale
                  : size * e->pxscale / (fly_v3dist(p, v->pos) + 1e-3f);
    return px > 150.0f ? 2 : px > 40.0f ? 1 : 0;
}

/* The half span the model is drawn to, in metres. It is the airframe's own
 * span — the models used to be one fixed size, so a 16.5 m Condor and an 8.2 m
 * Wasp were the same aeroplane with the same wings, and the studio plate said
 * so in a caption directly above the picture contradicting it. */
static float plane_half_span(const fly_airframe *af) {
    float s = af->wing_span > 1.0f ? af->wing_span * 0.5f : 5.5f;
    return (af->visual_mask & FLY_VIS_LONGWING) ? s * 1.30f : s;
}

/* The body frame with the model's scale folded into its axes: every coordinate
 * in the models below is in reference metres — the plane is drawn for a 5.5 m
 * half-span, the drone for a 1 m rotor arm — and fp() returns world. Anything
 * that is a genuine world length (a tube radius, the rotor disc, the height of
 * the gear off the ground) has to say so, and does. */
static void craft_frame(const fly_craft *c, float sc, fly__frame *f) {
    f->X = fly_v3scale(fly_qrot(c->ori, fly_v3mk(1, 0, 0)), sc);
    f->Y = fly_v3scale(fly_qrot(c->ori, fly_v3mk(0, 1, 0)), sc);
    f->Z = fly_v3scale(fly_qrot(c->ori, fly_v3mk(0, 0, 1)), sc);
    f->P = c->pos;
}

/* The wing planform, read at a spanwise fraction: leading and trailing edge
 * station in x, the dihedral in z, and the section's half-thickness. The
 * panel, the aileron cut-out and the wingtip nav light all come out of this,
 * so they cannot drift apart the way three separate sets of literals did. */
static void wing_at(float u, float *xle, float *xte, float *z, float *half) {
    static const float K[3][5] = { { 0.00f, 1.55f, -0.78f, 0.26f, 0.200f },
                                   { 0.45f, 1.16f, -0.62f, 0.38f, 0.140f },
                                   { 1.00f, 0.72f, -0.42f, 0.56f, 0.055f } };
    int i = u <= K[1][0] ? 0 : 1;
    float t = fly_clampf((u - K[i][0]) / (K[i + 1][0] - K[i][0]), 0.0f, 1.0f);
    *xle = fly_lerpf(K[i][1], K[i + 1][1], t);
    *xte = fly_lerpf(K[i][2], K[i + 1][2], t);
    *z = fly_lerpf(K[i][3], K[i + 1][3], t);
    *half = fly_lerpf(K[i][4], K[i + 1][4], t);
}

/* Where the aileron lives: outboard fraction of span, and its chord. The wing
 * gives up exactly this much trailing edge over that stretch, so the two
 * together are the planform above and the control surface is *in* the wing
 * rather than bolted on behind it. */
#define FLY_AIL_U0 0.56f
#define FLY_AIL_U1 0.97f
#define FLY_AIL_C 0.44f

/* A propeller, and what a camera does to one.
 *
 * The first version of this drew an opaque plate over the nose: `tri_shaded`'s
 * last argument is an ambient-occlusion term, not an alpha, and neither
 * rasterizer blends. The version after that avoided alpha rather than solving
 * it — thin opaque ghost blades and an opaque band around the rim — and what
 * came out was a bicycle wheel: a black tyre with black spokes, bolted to every
 * aircraft in the gallery. A prop is a see-through thing and there is no way to
 * draw one honestly without something that blends, so `blend_shaded` above is
 * that something.
 *
 * With alpha available the shape falls out of one number. Time-average the
 * blade over the exposure: at radius r a blade of chord c occupies c/r radians,
 * and smearing it across `sweep` radians of arc leaves each point of that arc
 * covered for c/(r*sweep) of the time. That is the opacity. Three things follow
 * that the wheel got backwards:
 *
 *  - It falls as 1/r. A real disc is densest at the root and nearly clear at
 *    the tip, because the same blade has more circumference to cover out there.
 *    An opaque band at the rim is the exact inverse of a propeller.
 *  - It is small. Two blades and a realistic planform put the tip near 2% and
 *    the root near 15% — a spinning prop is very close to invisible, which is
 *    what a photograph of one shows.
 *  - It closes smoothly. At rest `sweep` is nothing and c/(r*sweep) saturates
 *    to a solid blade; wind it up and the arcs lengthen, thin out and meet.
 *    There is no threshold anywhere and nothing to switch between.
 *
 * The blades under it are real geometry — tapered, twisted, rounded at the tip
 * — because at a walking idle the eye has time to look, and a triangle from the
 * hub to a point is a spoke. They fade out as the arcs take over. */

/* Blade chord as a fraction of the tip radius: a narrow shank at the root,
 * widest about a third out, tapering away at the tip. */
static float rotor_chord(float u) {
    u = fly_clampf(u, 0.0f, 1.0f);
    return 0.050f + 0.085f * sinf(FLY_PI * powf(u, 0.62f));
}

/* Legibility, and the one number here that is not physics. The time-average
 * above is right and it renders a prop you can barely find at fifty pixels
 * across, so the coverage is scaled by this and clamped. Set by eye against the
 * gallery stills; the physical figure is this over 2.1. */
#define FLY_ROTOR_GAIN 2.1f
#define FLY_ROTOR_MAXA 0.55f

/* one blade, as geometry: a twisted tapered ribbon from shank to tip */
static void rotor_blade(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        fly_v3 hub, fly_v3 ax, fly_v3 ay, fly_v3 axis,
                        float r0, float r1, fly_v3 col, float ba) {
    enum { NSPAN = 6 };
    float ca = cosf(ba), sa = sinf(ba);
    fly_v3 along = fly_v3add(fly_v3scale(ax, ca), fly_v3scale(ay, sa));
    fly_v3 across = fly_v3add(fly_v3scale(ax, -sa), fly_v3scale(ay, ca));
    fly_v3 le[NSPAN + 1], te[NSPAN + 1];
    int i;
    for (i = 0; i <= NSPAN; ++i) {
        float u = (float)i / (float)NSPAN;
        float r = r0 + (r1 - r0) * u;
        float ch = rotor_chord(u) * r1 * 0.5f;
        /* coarse pitch at the root, fine at the tip: a blade has to meet the
         * same airflow at every radius and the tip is going much faster */
        float pitch = 0.90f - 0.58f * u;
        fly_v3 mid = fly_v3add(hub, fly_v3scale(along, r));
        fly_v3 w = fly_v3scale(across, ch * cosf(pitch));
        fly_v3 d = fly_v3scale(axis, ch * sinf(pitch));
        le[i] = fly_v3add(mid, fly_v3add(w, d));
        te[i] = fly_v3sub(mid, fly_v3add(w, d));
    }
    for (i = 0; i < NSPAN; ++i) {
        tri_shaded(rt, v, e, le[i], te[i], te[i + 1], col, 1);
        tri_shaded(rt, v, e, le[i], te[i + 1], le[i + 1], col, 1);
    }
}

static void rotor_ghosts(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         fly_v3 hub, fly_v3 ax, fly_v3 ay, float r0, float r1,
                         fly_v3 col, float blur, float phase, int blades) {
    fly_v3 axis = fly_v3norm(fly_v3cross(ax, ay));
    /* Painted tips. Nearly every light aircraft has them and on a turning prop
     * they are the only part of the disc the eye reliably catches — which is
     * also the honest way to get a visible rim, as paint rather than as a
     * black band that no propeller has. */
    fly_v3 tipc = fly_v3mk(0.62f, 0.50f, 0.06f);
    float sweep = blur * (2.0f * FLY_PI / (float)blades);
    /* How many pixels across the disc lands. The smear is composited on the
     * CPU in every pipeline, so the traffic in a busy sky pays for it: a disc
     * three pixels wide gets one ring and a quarter of the arc segments, and
     * one too small to see gets nothing at all. The spinner above is a handful
     * of triangles and stays — it is what says an aircraft has an engine. */
    float rpx = r1 / (fly_v3len(fly_v3sub(hub, v->pos)) + 1e-3f) * e->pxscale;
    int NRING = rpx > 26.0f ? 7 : rpx > 9.0f ? 4 : 2;
    int NARC = rpx > 26.0f ? 10 : rpx > 9.0f ? 5 : 3;
    int b, k, s;
    if (g_shadow_cast) return;   /* the disc is air; the blades below cast */

    /* The spinner. A see-through disc needs something solid at the middle to
     * be attached to, and the hub is where a prop is genuinely opaque. */
    {
        fly_v3 tipp = fly_v3add(hub, fly_v3scale(axis, r0 * 1.15f));
        for (s = 0; s < 8; ++s) {
            float a0 = (float)s / 8.0f * 2.0f * FLY_PI;
            float a1 = (float)(s + 1) / 8.0f * 2.0f * FLY_PI;
            fly_v3 p0 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, cosf(a0) * r0 * 0.62f),
                                                 fly_v3scale(ay, sinf(a0) * r0 * 0.62f)));
            fly_v3 p1 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, cosf(a1) * r0 * 0.62f),
                                                 fly_v3scale(ay, sinf(a1) * r0 * 0.62f)));
            tri_spec(rt, v, e, tipp, p0, p1, fly_v3scale(col, 1.6f), 1, 0.5f, 30.0f);
        }
    }

    /* The blades, fading out as their own smear takes over from them. */
    if (blur < 0.34f)
        for (b = 0; b < blades; ++b)
            rotor_blade(rt, v, e, hub, ax, ay, axis, r0, r1, col,
                        phase + (float)b * (2.0f * FLY_PI / (float)blades));

    if (sweep <= 1e-4f || rpx < 1.5f) return;

    /* The smear: one arc per blade, spanning what it swept. Opacity is carried
     * per vertex, because it is a 1/r gradient and a band of flat triangles
     * across it draws the concentric steps rather than the falloff. The rings
     * are bunched toward the rim so the painted tip gets a band of its own
     * rather than a seventh of the radius. */
    for (k = 0; k < NRING; ++k) {
        float u0 = 1.0f - powf(1.0f - (float)k / (float)NRING, 1.5f);
        float u1 = 1.0f - powf(1.0f - (float)(k + 1) / (float)NRING, 1.5f);
        float ra = r0 + (r1 - r0) * u0, rb = r0 + (r1 - r0) * u1;
        float scale = FLY_ROTOR_GAIN / sweep;
        float ain, aout;
        fly_v3 cc = k >= NRING - 1 ? tipc : col;
        /* while the blades are still drawn, the smear is what is left of the
         * exposure rather than all of it, or the two would sum to double */
        if (blur < 0.34f) scale *= fly_smoothstepf(0.0f, 0.34f, blur);
        ain = rotor_chord(u0) * r1 / ra * scale;
        aout = rotor_chord(u1) * r1 / rb * scale;
        /* the outermost band is paint on a blade that is running out of chord,
         * so it fades to nothing at the very tip instead of ending on an edge */
        if (k == NRING - 1) aout *= 0.35f;
        if (ain > FLY_ROTOR_MAXA) ain = FLY_ROTOR_MAXA;
        if (aout > FLY_ROTOR_MAXA) aout = FLY_ROTOR_MAXA;
        for (b = 0; b < blades; ++b) {
            float base = phase + (float)b * (2.0f * FLY_PI / (float)blades);
            for (s = 0; s < NARC; ++s) {
                float t0 = base + sweep * (float)s / (float)NARC;
                float t1 = base + sweep * (float)(s + 1) / (float)NARC;
                float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
                fly_v3 i0 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, c0 * ra),
                                                     fly_v3scale(ay, s0 * ra)));
                fly_v3 i1 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, c1 * ra),
                                                     fly_v3scale(ay, s1 * ra)));
                fly_v3 o0 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, c0 * rb),
                                                     fly_v3scale(ay, s0 * rb)));
                fly_v3 o1 = fly_v3add(hub, fly_v3add(fly_v3scale(ax, c1 * rb),
                                                     fly_v3scale(ay, s1 * rb)));
                blend_shaded(v, e, i0, i1, o1, cc, ain, ain, aout);
                blend_shaded(v, e, i0, o1, o0, cc, ain, aout, aout);
            }
        }
    }
}

/* Where an aeroplane's propeller disc is: the hub in the model's own reference
 * metres and the disc radius in world metres.
 *
 * One function because two callers need the same answer and the second one is
 * a test. `render.cine` measures the disc off a rendered frame, and to do that
 * it has to know where on screen the disc is; it had the geometry copied out,
 * with the radius as the flat 1.9 m it used to be on every airframe. When the
 * radius became a function of the span and the undercarriage the copy was left
 * behind, so every sample the gate took landed in clear sky past the tip and
 * what it measured was the sky's own gradient. It read "the propeller darkens
 * nothing" as a pass for as long as two negative numbers stayed in the right
 * order. Geometry a test has to know is geometry the renderer should hand it. */
static void prop_disc_geom(const fly_airframe *af, fly_v3 *hub, float *radius) {
    float half = plane_half_span(af);
    float sc = half / 5.5f;
    /* Prop clearance: sized to the airframe, then clipped to leave a quarter
     * of a metre under the tip at rest — see the propeller in
     * draw_plane_model, which is the other caller. */
    float pr = 1.15f * sc, lim = af->gear_height - 0.25f + 0.15f * sc;
    if (pr > lim) pr = lim;
    if (pr < 0.35f * sc) pr = 0.35f * sc;
    if (hub) *hub = fly_v3mk(6.55f, 0.0f, 0.15f);
    if (radius) *radius = pr;
}

static void draw_plane_model(fly_render_target *rt, const fly__view *v, const fly__env *e,
                             const fly_craft *c, const fly_airframe *af,
                             const fly_controls *in, fly_v3 tint, fly_v3 trim, double time) {
    uint32_t vm = af->visual_mask;
    float half = plane_half_span(af);
    float sc = half / 5.5f;                  /* the model is drawn for a 5.5 m half-span */
    int lod = craft_lod(v, e, c->pos, half * 2.0f);
    /* Two ladders meet here and both have to be obeyed: how big the aircraft
     * lands on screen (`lod`) and how much the graphics level is willing to
     * spend on it at all (`e->lod`). Taking the smaller is what stops a distant
     * aeroplane costing an ultra hull and a near one costing more than the low
     * level promised. */
    int nb = lod >= 2 ? e->lod.ring : lod >= 1 ? 8 : 6;  /* points around a body ring */
    int nf = lod >= 1 ? e->lod.foil : 4;                 /* points around a section */
    fly__frame f;
    fly_v3 uX = fly_qrot(c->ori, fly_v3mk(1, 0, 0));
    fly_v3 uY = fly_qrot(c->ori, fly_v3mk(0, 1, 0));
    fly_v3 uZ = fly_qrot(c->ori, fly_v3mk(0, 0, 1));
    fly_v3 fus = tint;
    fly_v3 wingc = fly_v3scale(tint, 0.78f);
    fly_v3 ctrl = fly_v3scale(tint, 0.70f);
    fly_v3 dark = fly_v3mk(0.10f, 0.11f, 0.13f);
    fly_v3 metal = fly_v3mk(0.32f, 0.33f, 0.35f);
    /* Rarity paint. A module that rolled well is a different piece of kit and
     * looks like one from outside: works get brighter and harder as the tier
     * climbs, from grey factory issue through anodised to a one-off's polished
     * finish. It rides on the fitted geometry only — the airframe underneath
     * is whatever the operator bought, and pretending otherwise would make a
     * good hardpoint repaint the wings. */
    float rare = (float)af->visual_rarity / (float)(FLY_ITEM_RARITY_COUNT - 1);
    fly_v3 works = fly_v3lerp(fly_v3mk(0.16f, 0.17f, 0.19f),
                              fly_v3mk(0.62f, 0.55f, 0.24f), rare);
    fly_v3 plate = fly_v3lerp(fly_v3mk(0.55f, 0.56f, 0.58f),
                              fly_v3mk(0.80f, 0.72f, 0.42f), rare);
    float shine = 0.5f + 0.9f * rare, hard = 30.0f + 70.0f * rare;
    /* the gear line, in frame units: nothing hung under the belly may go below
     * it, and the undercarriage is built down to it */
    float gz = -af->gear_height / (sc > 1e-3f ? sc : 1.0f), belly = gz + 0.12f;
    int side, i;
    (void)uX;
    craft_frame(c, sc, &f);

    /* Fuselage: a round-backed, flat-sided body from the spinner to the tail
     * post. The old one was a four-sided prism, which is why every aircraft in
     * the gallery had a crease running its full length at the waterline. */
    {
        static const float S[8][5] = {   /* x, z, half-width, half-up, half-down */
            {  6.40f, 0.15f, 0.030f, 0.030f, 0.030f },
            {  5.55f, 0.15f, 0.300f, 0.285f, 0.260f },
            {  4.30f, 0.16f, 0.620f, 0.560f, 0.520f },
            {  2.20f, 0.15f, 0.950f, 0.700f, 0.700f },
            {  0.00f, 0.17f, 0.950f, 0.720f, 0.720f },
            { -2.60f, 0.25f, 0.750f, 0.600f, 0.600f },
            { -4.00f, 0.40f, 0.420f, 0.360f, 0.330f },
            { -4.70f, 0.55f, 0.090f, 0.120f, 0.100f } };
        static const int FAR[5] = { 0, 2, 3, 5, 7 };
        model_body(rt, v, e, &f, S, lod >= 1 ? NULL : FAR, lod >= 1 ? 8 : 5, nb,
                   fus, 0.55f, 26.0f);
    }

    /* Canopy: a glasshouse sitting on the deck, not three flat panes. Its
     * underside is inside the fuselage, so the join needs no seam. */
    if (lod >= 1) {
        static const float S[4][5] = {
            {  3.70f, 0.55f, 0.26f, 0.32f, 0.02f },
            {  2.20f, 0.60f, 0.60f, 0.64f, 0.02f },
            {  0.40f, 0.60f, 0.58f, 0.58f, 0.02f },
            { -0.90f, 0.64f, 0.26f, 0.22f, 0.02f } };
        model_body(rt, v, e, &f, S, NULL, 4, nb, dark, 1.30f, 110.0f);
    } else {
        tri_spec(rt, v, e, fp(&f, 3.6f, 0, 0.85f), fp(&f, 1.6f, 0.58f, 1.22f),
                 fp(&f, 1.6f, -0.58f, 1.22f), dark, 1, 1.30f, 110.0f);
    }

    /* Wings, ailerons and anything hung under them. Built one side at a time
     * from the same planform with y negated, so the two are exact mirrors. */
    {
        static const float UW[5] = { 0.00f, 0.45f, FLY_AIL_U0, FLY_AIL_U0, 1.00f };
        static const float UF[3] = { 0.00f, 0.50f, 1.00f };
        const float *uw = lod >= 1 ? UW : UF;
        int nw = lod >= 1 ? 5 : 3;
        float ail = in ? in->roll * 0.42f : 0.0f;
        for (side = -1; side <= 1; side += 2) {
            float sy = (float)side;
            fly__foilst st[5];
            for (i = 0; i < nw; ++i) {
                float xle, xte, z, th;
                wing_at(uw[i], &xle, &xte, &z, &th);
                if (lod >= 1 && i >= 3) xte += FLY_AIL_C;   /* the aileron cut-out */
                st[i].le = fp(&f, xle, 5.5f * uw[i] * sy, z);
                st[i].te = fp(&f, xte, 5.5f * uw[i] * sy, z);
                st[i].half = th;
            }
            model_foil(rt, v, e, st, nw, f.Z, nf, wingc, 0.30f, 14.0f);
            if (lod >= 1) {   /* the aileron fills the cut-out and hinges at it */
                float defl = ail * sy, cd = cosf(defl), sd = sinf(defl);
                fly__foilst a[2];
                for (i = 0; i < 2; ++i) {
                    float u = i ? FLY_AIL_U1 : FLY_AIL_U0, xle, xte, z, th;
                    wing_at(u, &xle, &xte, &z, &th);
                    a[i].le = fp(&f, xte + FLY_AIL_C, 5.5f * u * sy, z);
                    a[i].te = fp(&f, xte + FLY_AIL_C * (1.0f - cd), 5.5f * u * sy,
                                 z - FLY_AIL_C * sd);
                    a[i].half = th * 0.58f;
                }
                model_foil(rt, v, e, a, 2, f.Z, nf, ctrl, 0.30f, 14.0f);
            }
            /* slat strip along the leading edge (STOL kit) */
            if ((vm & FLY_VIS_SLATS) && lod >= 1) {
                fly__foilst s2[2];
                float xle, xte, z, th;
                wing_at(0.12f, &xle, &xte, &z, &th);
                s2[0].le = fp(&f, xle + 0.34f, 5.5f * 0.12f * sy, z + 0.06f);
                s2[0].te = fp(&f, xle - 0.10f, 5.5f * 0.12f * sy, z + 0.02f);
                s2[0].half = 0.055f;
                wing_at(0.94f, &xle, &xte, &z, &th);
                s2[1].le = fp(&f, xle + 0.26f, 5.5f * 0.94f * sy, z + 0.05f);
                s2[1].te = fp(&f, xle - 0.08f, 5.5f * 0.94f * sy, z + 0.02f);
                s2[1].half = 0.030f;
                model_foil(rt, v, e, s2, 2, f.Z, nf, fly_v3mk(0.75f, 0.76f, 0.78f),
                           0.4f, 22.0f);
            }
            if (vm & FLY_VIS_GUNPOD)
                model_pod(rt, v, e, &f, 0.6f, 2.6f * sy, -0.20f, 0.9f, 0.22f, works, 1);
            if (vm & FLY_VIS_DROPTANK)
                model_pod(rt, v, e, &f, 0.2f, 3.6f * sy, -0.30f, 1.3f, 0.34f, plate, 0);
            /* --- the armed loadout ---------------------------------------
             *
             * Five bits with names in fly_sim.h and, until this, nothing on the
             * other end of them — the same defect the spaceframe conversion had
             * and was fixed for. What an aeroplane is carrying should be
             * visible from outside it: somebody deciding whether to pick a
             * fight can see the rails, and a player who has just spent two
             * thousand tokens should be able to look at what they bought.
             *
             * Each is drawn where its own module says it is. Rockets and
             * missiles hang under the wing where the pylons are, the dispenser
             * sits in the belly because it is a bay part, and the emitter is a
             * barrel out along the leading edge because it is the one weapon
             * with nothing to eject. */
            if (vm & FLY_VIS_ROCKETPOD) {
                /* a blunt tube of tubes: fat, short, and unmistakably not a
                 * fuel tank at the range where the two would otherwise agree */
                model_pod(rt, v, e, &f, 0.1f, 3.1f * sy, -0.34f, 0.78f, 0.30f, works, 0);
                if (lod >= 1)
                    model_tube(rt, v, e, fp(&f, 0.50f, 3.1f * sy, -0.34f),
                               fp(&f, 0.86f, 3.1f * sy, -0.34f), 0.26f * sc, 6, f.Y,
                               fly_v3mk(0.10f, 0.10f, 0.11f), 0.25f, 8.0f);
            }
            if (vm & FLY_VIS_MISSILE) {
                /* a rail with a slim round on it, and a nose that reads as a
                 * seeker head rather than as more pod */
                model_tube(rt, v, e, fp(&f, 1.05f, 3.35f * sy, -0.24f),
                           fp(&f, -0.75f, 3.35f * sy, -0.24f), 0.115f * sc, 6, f.Y,
                           fly_v3lerp(fly_v3mk(0.80f, 0.80f, 0.82f), plate, rare),
                           shine, hard);
                model_tube(rt, v, e, fp(&f, 1.05f, 3.35f * sy, -0.24f),
                           fp(&f, 1.34f, 3.35f * sy, -0.24f), 0.075f * sc, 6, f.Y,
                           fly_v3mk(0.16f, 0.15f, 0.17f), 0.7f, 60.0f);
                if (lod >= 1)
                    model_box(rt, v, e, &f, 0.15f, 3.35f * sy, -0.10f, 0.9f, 0.05f, 0.10f,
                              works, 0.3f, 12.0f);
            }
            if (vm & FLY_VIS_EMITTER)
                model_tube(rt, v, e, fp(&f, 1.20f, 2.15f * sy, -0.05f),
                           fp(&f, 2.80f, 2.15f * sy, -0.05f), 0.085f * sc, 6, f.Y,
                           fly_v3mk(0.34f, 0.36f, 0.42f), 0.85f, 90.0f);
        }
    }

    /* fuselage-mounted modules */
    if (vm & FLY_VIS_TURBO)
        for (side = -1; side <= 1; side += 2)
            model_box(rt, v, e, &f, 4.6f, 0.45f * (float)side, 0.85f, 0.7f, 0.14f, 0.14f,
                      dark, 0.6f, 40.0f);
    /* Anything slung under the belly is placed off the gear line rather than
     * at a fixed depth, or it drags: a cargo pod at -0.95 hung 0.2 m through
     * the apron on a Skylark and further on a Condor, whose gear is longer in
     * metres but shorter in the model's own units. */
    if (vm & FLY_VIS_CARGOPOD)
        model_box(rt, v, e, &f, 0.1f, 0, belly + 0.45f, 2.2f, 0.85f, 0.45f,
                  fly_v3lerp(fly_v3scale(tint, 0.6f), plate, rare * 0.6f), 0.2f + shine * 0.3f,
                  10.0f + hard * 0.4f);
    if (vm & FLY_VIS_BELLYTANK)
        model_box(rt, v, e, &f, 0.2f, 0, belly + 0.38f, 1.8f, 0.55f, 0.38f,
                  plate, shine, hard);
    /* The bay's two: a rack of bombs under the belly, and a dispenser flush
     * with it. The dispenser is deliberately small and hard to mistake for
     * cargo — what it is is a box of tubes pointing down and back. */
    if (vm & FLY_VIS_BOMBRACK) {
        model_box(rt, v, e, &f, 0.1f, 0, belly + 0.30f, 1.9f, 0.62f, 0.16f,
                  works, 0.25f, 10.0f);
        for (side = -1; side <= 1; side += 2)
            model_tube(rt, v, e, fp(&f, 0.95f, 0.30f * (float)side, belly + 0.02f),
                       fp(&f, -0.85f, 0.30f * (float)side, belly + 0.02f),
                       0.17f * sc, 6, f.Y, fly_v3mk(0.30f, 0.31f, 0.28f), 0.2f, 8.0f);
    }
    if (vm & FLY_VIS_DISPENSER)
        model_box(rt, v, e, &f, -1.35f, 0, belly + 0.34f, 0.62f, 0.48f, 0.20f,
                  fly_v3mk(0.24f, 0.24f, 0.26f), 0.35f, 14.0f);
    if (vm & FLY_VIS_RADARDOME) {
        model_box(rt, v, e, &f, 0.8f, 0, 1.5f, 0.12f, 0.12f, 0.3f, dark, 0.3f, 20.0f);
        model_box(rt, v, e, &f, 0.8f, 0, 1.95f, 0.75f, 0.75f, 0.22f,
                  fly_v3lerp(fly_v3mk(0.72f, 0.73f, 0.75f), plate, rare), shine, hard);
    }
    if (vm & FLY_VIS_ANTENNA)
        model_tube(rt, v, e, fp(&f, -2.2f, 0, 1.10f), fp(&f, -2.2f, 0, 2.30f),
                   0.045f * sc, 6, f.Y, fly_v3mk(0.6f, 0.6f, 0.62f), 0.5f, 30.0f);
    if (vm & FLY_VIS_ARMOR)
        for (side = -1; side <= 1; side += 2)
            model_box(rt, v, e, &f, 1.2f, 1.05f * (float)side, 0.15f, 2.4f, 0.07f, 0.55f,
                      works, 0.3f + shine * 0.4f, 16.0f + hard * 0.5f);

    /* --- the spaceframe conversion -----------------------------------------
     *
     * Five bits that had names in `fly_sim.h` and nothing on the other end of
     * them: FLY_VIS_ROCKET through FLY_VIS_CABIN were set by the modules,
     * carried through the fit and saved with the airframe, and then no
     * geometry read them. The gallery's own caption for the airframe plates
     * says a loadout is visible on the hull, and for the one build that costs
     * more than a second aeroplane it was not — "Skylark T1, loadout B: Rocket
     * Motor, RCS Thrusters, Propellant Tank, Pressure Cabin, Ablative Shield"
     * over a picture of a stock Skylark.
     *
     * Each of the five is drawn where its own module says it is: the motor is
     * in the tail because it is the engine slot, the tank is in the belly
     * because it is the bay and the hold has gone, and the shield is the
     * underside because it is the hull. Nothing here is a new builder — the
     * same lofted body, box and tube the rest of the aeroplane is made of. */
    if (vm & FLY_VIS_ROCKET) {
        /* Chamber, throat and bell, aft of the tail post. A rocket that ended
         * flush with the fuselage would be an engine you have to be told
         * about; the point of the silhouette is that it hangs out the back. */
        static const float S[4][5] = {
            { -4.10f, 0.42f, 0.33f, 0.33f, 0.33f },
            { -5.15f, 0.42f, 0.26f, 0.26f, 0.26f },
            { -5.70f, 0.42f, 0.48f, 0.48f, 0.48f },
            { -6.35f, 0.42f, 0.72f, 0.72f, 0.72f } };
        model_body(rt, v, e, &f, S, NULL, 4, nb, works, 0.4f + shine * 0.5f,
                   20.0f + hard * 0.6f);
        /* the throat, seen up the bell: nothing else on the aeroplane is this
         * dark, and at a distance it is what tells the bell from a tailcone */
        model_box(rt, v, e, &f, -5.15f, 0, 0.42f, 0.10f, 0.24f, 0.24f,
                  fly_v3mk(0.05f, 0.05f, 0.06f), 0.1f, 8.0f);
    }
    if (vm & FLY_VIS_PROPTANK) {
        /* A pressure vessel where the hold was: domed both ends, on the
         * centreline, sitting on the gear line like everything else slung
         * underneath. Its length is most of the fuselage, because the
         * propellant outmasses the airframe carrying it. */
        float bz = belly + 0.62f;
        const float S[5][5] = {
            {  2.90f, bz, 0.18f, 0.18f, 0.18f },
            {  2.20f, bz, 0.60f, 0.60f, 0.60f },
            {  0.00f, bz, 0.66f, 0.66f, 0.66f },
            { -2.20f, bz, 0.58f, 0.58f, 0.58f },
            { -2.95f, bz, 0.16f, 0.16f, 0.16f } };
        model_body(rt, v, e, &f, S, NULL, 5, nb, plate, shine, hard);
    }
    if (vm & FLY_VIS_RCS) {
        /* Four clusters, at the two ends of the longest lever arms the
         * airframe has. Attitude with no air is a couple, so they are in
         * mirrored pairs fore and aft rather than anywhere convenient. */
        for (side = -1; side <= 1; side += 2) {
            float sy = (float)side;
            model_box(rt, v, e, &f, 3.85f, 0.62f * sy, 0.42f, 0.26f, 0.20f, 0.18f,
                      works, 0.5f + shine * 0.4f, 30.0f + hard * 0.5f);
            model_box(rt, v, e, &f, -3.30f, 0.52f * sy, 0.72f, 0.24f, 0.18f, 0.16f,
                      works, 0.5f + shine * 0.4f, 30.0f + hard * 0.5f);
        }
    }
    if (vm & FLY_VIS_SHIELD) {
        /* The aeroshell is the outer mould line, not a panel bolted to one:
         * a wide flat underside from the nose to behind the wing, dark
         * because ablative is dark and because it is the one surface on the
         * aeroplane that is meant to come back a different colour. */
        model_box(rt, v, e, &f, 1.30f, 0, belly + 0.16f, 3.30f, 1.05f, 0.11f,
                  fly_v3mk(0.13f, 0.11f, 0.10f), 0.15f, 8.0f);
        model_box(rt, v, e, &f, 4.10f, 0, belly + 0.34f, 1.05f, 0.55f, 0.10f,
                  fly_v3mk(0.13f, 0.11f, 0.10f), 0.15f, 8.0f);
    }
    if (vm & FLY_VIS_CABIN) {
        /* A pressure hull over the glasshouse: a domed fairing and the collar
         * it seals against. What a sealed cabin looks like from outside is
         * less window, not more. */
        const float S[4][5] = {
            {  3.30f, 0.62f, 0.30f, 0.30f, 0.02f },
            {  2.30f, 0.62f, 0.66f, 0.62f, 0.02f },
            {  0.30f, 0.62f, 0.64f, 0.60f, 0.02f },
            { -1.10f, 0.62f, 0.24f, 0.22f, 0.02f } };
        model_body(rt, v, e, &f, S, NULL, 4, nb,
                   fly_v3lerp(fly_v3mk(0.66f, 0.67f, 0.70f), plate, rare), shine, hard);
        model_tube(rt, v, e, fp(&f, 2.30f, 0, 1.24f), fp(&f, 0.30f, 0, 1.22f),
                   0.09f * sc, 6, f.Y, metal, 0.6f, 40.0f);
    }

    /* Tailplane and a full-span elevator, one half at a time. */
    {
        float elev = in ? -in->pitch * 0.42f : 0.0f;
        float ce = cosf(elev), se = sinf(elev);
        for (side = -1; side <= 1; side += 2) {
            float sy = (float)side;
            fly__foilst st[2];
            st[0].le = fp(&f, -3.15f, 0.26f * sy, 0.62f);
            st[0].te = fp(&f, -4.20f, 0.26f * sy, 0.62f);
            st[0].half = 0.105f;
            st[1].le = fp(&f, -3.85f, 2.35f * sy, 0.72f);
            st[1].te = fp(&f, -4.42f, 2.35f * sy, 0.72f);
            st[1].half = 0.045f;
            model_foil(rt, v, e, st, 2, f.Z, nf, wingc, 0.30f, 14.0f);
            if (lod >= 1) {
                fly__foilst el[2];
                el[0].le = st[0].te;
                el[0].te = fp(&f, -4.20f - 0.52f * ce, 0.26f * sy, 0.62f + 0.52f * se);
                el[0].half = 0.055f;
                el[1].le = st[1].te;
                el[1].te = fp(&f, -4.42f - 0.46f * ce, 2.35f * sy, 0.72f + 0.46f * se);
                el[1].half = 0.028f;
                model_foil(rt, v, e, el, 2, f.Z, nf, ctrl, 0.30f, 14.0f);
            }
        }
    }

    /* Fin and rudder. The fin is the part the old model gave away most
     * cheaply — one triangle in the y = 0 plane, so from either side of the
     * aircraft it was a hairline and from dead astern it did not exist. */
    {
        float rud = in ? in->yaw * 0.42f : 0.0f;
        float cr = cosf(rud), sr = sinf(rud);
        fly__foilst st[3];
        st[0].le = fp(&f, -2.85f, 0, 0.62f);
        st[0].te = fp(&f, -4.55f, 0, 0.60f);
        st[0].half = 0.115f;
        st[1].le = fp(&f, -3.70f, 0, 1.58f);
        st[1].te = fp(&f, -4.62f, 0, 1.58f);
        st[1].half = 0.078f;
        st[2].le = fp(&f, -4.16f, 0, 2.45f);
        st[2].te = fp(&f, -4.66f, 0, 2.45f);
        st[2].half = 0.038f;
        if (lod < 1) st[1] = st[2];
        /* The fin is the flash. It is the one surface an aeroplane presents
         * broadside from almost every angle a player sees another aeroplane
         * from, so it is where a livery has to put its second colour: hull
         * colour alone separates four powers and not seven, and the pair that
         * come closest — Foundry rust and Corsair crimson — are told apart
         * here and nowhere else. */
        model_foil(rt, v, e, st, lod >= 1 ? 3 : 2, f.Y, nf, trim, 0.34f, 18.0f);
        if (lod >= 1) {
            fly__foilst rd[2];
            rd[0].le = fp(&f, -4.55f, 0, 0.66f);
            rd[0].te = fp(&f, -4.55f - 0.55f * cr, 0.55f * sr, 0.66f);
            rd[0].half = 0.060f;
            rd[1].le = fp(&f, -4.66f, 0, 2.40f);
            rd[1].te = fp(&f, -4.66f - 0.42f * cr, 0.42f * sr, 2.40f);
            rd[1].half = 0.030f;
            model_foil(rt, v, e, rd, 2, f.Y, nf, ctrl, 0.30f, 14.0f);
        }
    }

    /* Undercarriage. The aircraft has stood on nothing at all until now: the
     * sim already parks the body origin `gear_height` above the ground, so
     * there was a metre and a half of daylight under every aeroplane on every
     * apron in the gallery. The legs are drawn to that number rather than to a
     * guess, so the wheels touch whatever the airframe says they should. */
    if (lod >= 1) {
        float wr = 0.30f, ww = 0.13f;
        int nw = lod >= 2 ? 10 : 6;
        model_tube(rt, v, e, fp(&f, 4.30f, 0, -0.34f), fp(&f, 4.30f, 0, gz + wr),
                   0.055f * sc, 6, f.Y, metal, 0.5f, 30.0f);
        model_tube(rt, v, e, fp(&f, 4.30f, -ww, gz + wr), fp(&f, 4.30f, ww, gz + wr),
                   wr * sc, nw, f.Y, dark, 0.25f, 12.0f);
        for (side = -1; side <= 1; side += 2) {
            float sy = (float)side;
            model_tube(rt, v, e, fp(&f, 0.55f, 0.55f * sy, -0.50f),
                       fp(&f, 0.55f, 1.45f * sy, gz + wr), 0.065f * sc, 6, f.Y,
                       metal, 0.5f, 30.0f);
            model_tube(rt, v, e, fp(&f, 0.55f, (1.45f - ww) * sy, gz + wr),
                       fp(&f, 0.55f, (1.45f + ww) * sy, gz + wr), wr * sc, nw, f.Y,
                       dark, 0.25f, 12.0f);
        }
    }

    /* The propeller.
     *
     * Two blades frozen at whatever angle the frame caught them is what a
     * still photograph of a stopped engine looks like, and the eye reads a
     * turning prop as a disc long before it can count blades. Above a walking
     * idle the blades give way to a translucent annulus — the time-average of
     * the blade sweeping past, which is what motion blur *is* — with the
     * blades kept underneath at low revs so a stopped or ticking-over engine
     * still shows what it has. */
    {
        float rpm = c->throttle * 38.0f + 4.0f;
        float a = (float)fmod(time * rpm, 2.0 * FLY_PI);
        float blur = fly_smoothstepf(9.0f, 20.0f, rpm);
        /* Prop clearance. The disc used to be a flat 1.9 m in radius on every
         * airframe, which on a Skylark put the tip 0.55 m *underground* when
         * it was parked — invisible for as long as the aeroplane had no
         * undercarriage and floated over the apron, and the first thing you
         * see once it stands on wheels. Sized to the airframe and then clipped
         * to leave a quarter of a metre under the tip at rest, in
         * `prop_disc_geom`, which is also where `render.cine` reads it from. */
        fly_v3 hubref;
        float pr;
        prop_disc_geom(af, &hubref, &pr);
        rotor_ghosts(rt, v, e, fp(&f, hubref.x, hubref.y, hubref.z), uY, uZ,
                     0.26f * sc, pr, dark, blur, a, 2);
    }
}

/* The drone. Same three builders: a lofted pod, tubes for the arms and the
 * skids, and the rotor smear the aeroplane's propeller uses. It is drawn to
 * its own span too — at a fixed 2.3 m arm the Dragonfly was six and a half
 * metres across a plate captioned "span 1.6m". */
static void draw_drone_model(fly_render_target *rt, const fly__view *v, const fly__env *e,
                             const fly_craft *c, const fly_airframe *af, fly_v3 tint,
                             fly_v3 trim, double time) {
    uint32_t vm = af->visual_mask;
    /* the span is motor to motor across the diagonal, so one arm is half of it
     * over root two; everything below is in units of that arm */
    float span = af->wing_span > 0.4f ? af->wing_span : 1.6f;
    float sc = span * 0.35355f;
    int lod = craft_lod(v, e, c->pos, span);
    int nb = lod >= 2 ? e->lod.ring : lod >= 1 ? 8 : 6;
    fly__frame f;
    fly_v3 uX = fly_qrot(c->ori, fly_v3mk(1, 0, 0));
    fly_v3 uY = fly_qrot(c->ori, fly_v3mk(0, 1, 0));
    fly_v3 dark = fly_v3mk(0.10f, 0.11f, 0.13f);
    fly_v3 metal = fly_v3mk(0.32f, 0.33f, 0.35f);
    /* A quad has no fin, so its livery's second colour goes on the arms —
     * the part that reads as four bright spokes against the ground from
     * directly above, which is how a drone is usually seen. */
    fly_v3 arm = fly_v3lerp(fly_v3scale(tint, 0.72f), trim, 0.55f);
    float gz = -af->gear_height / (sc > 1e-3f ? sc : 1.0f), belly = gz + 0.10f;
    int k;
    craft_frame(c, sc, &f);

    /* pod: a closed, symmetric body. The old one was five loose triangles with
     * a face on the port side that had no starboard twin and an open tail. */
    {
        static const float S[5][5] = {
            {  1.10f, 0.02f, 0.10f, 0.10f, 0.08f },
            {  0.80f, 0.02f, 0.38f, 0.34f, 0.30f },
            {  0.20f, 0.02f, 0.54f, 0.44f, 0.40f },
            { -0.60f, 0.02f, 0.52f, 0.42f, 0.38f },
            { -1.00f, 0.04f, 0.22f, 0.24f, 0.20f } };
        model_body(rt, v, e, &f, S, NULL, 5, nb, tint, 0.45f, 22.0f);
    }
    /* glossy sensor turret under the nose */
    if (lod >= 1) {
        static const float S[3][5] = {
            {  0.98f, -0.16f, 0.10f, 0.10f, 0.10f },
            {  0.86f, -0.21f, 0.20f, 0.18f, 0.20f },
            {  0.68f, -0.18f, 0.13f, 0.13f, 0.13f } };
        model_body(rt, v, e, &f, S, NULL, 3, nb, dark, 1.20f, 90.0f);
    }

    for (k = 0; k < 4; ++k) {
        float bx = (k & 1) ? 1.0f : -1.0f, by = (k & 2) ? 1.0f : -1.0f;
        float rlen = ((vm & FLY_VIS_LONGWING) ? 0.78f : 0.62f) * sc;
        float rpm = c->throttle * 55.0f + 6.0f;
        float a = (float)fmod(time * rpm, 2.0 * FLY_PI) + (float)k * 1.3f;
        /* a rotor turns faster than a propeller and is never seen from far
         * enough away to count its blades, so it smears sooner */
        float blur = fly_smoothstepf(11.0f, 24.0f, rpm);
        model_tube(rt, v, e, fp(&f, bx * 0.40f, by * 0.40f, 0.06f),
                   fp(&f, bx, by, 0.24f), 0.085f * sc, lod >= 1 ? 6 : 4, f.Y,
                   arm, 0.4f, 20.0f);
        if (lod >= 1)   /* motor can */
            model_tube(rt, v, e, fp(&f, bx, by, 0.20f), fp(&f, bx, by, 0.40f),
                       0.155f * sc, 6, f.Y, metal, 0.6f, 40.0f);
        rotor_ghosts(rt, v, e, fp(&f, bx, by, 0.44f), uX, uY,
                     0.16f * sc, rlen, dark, blur, a, 2);
    }

    /* Skids: two rails and four legs, standing the pod exactly `gear_height`
     * off the ground. The old pair were flat triangles that reached 0.75 m
     * below a body the sim parks 0.4 m up, so the drone stood in the dirt. */
    if (lod >= 1) {
        int side;
        for (side = -1; side <= 1; side += 2) {
            float sy = (float)side, r = 0.06f;
            fly_v3 a0 = fp(&f, 0.78f, 0.66f * sy, gz + r);
            fly_v3 a1 = fp(&f, -0.82f, 0.66f * sy, gz + r);
            model_tube(rt, v, e, a0, a1, r * sc, 6, f.Y, metal, 0.5f, 30.0f);
            model_tube(rt, v, e, fp(&f, 0.42f, 0.30f * sy, -0.32f),
                       fp(&f, 0.42f, 0.66f * sy, gz + r), 0.05f * sc, 6, f.Y,
                       metal, 0.5f, 30.0f);
            model_tube(rt, v, e, fp(&f, -0.50f, 0.30f * sy, -0.32f),
                       fp(&f, -0.50f, 0.66f * sy, gz + r), 0.05f * sc, 6, f.Y,
                       metal, 0.5f, 30.0f);
        }
    }

    /* slung and saddle modules */
    if (vm & FLY_VIS_GUNPOD)
        model_pod(rt, v, e, &f, 0.20f, 0, belly + 0.13f, 0.42f, 0.13f,
                  fly_v3mk(0.16f, 0.17f, 0.19f), 1);
    if (vm & FLY_VIS_DROPTANK)
        for (k = -1; k <= 1; k += 2)
            model_box(rt, v, e, &f, -0.25f, 0.68f * (float)k, 0.12f, 0.50f, 0.16f, 0.16f,
                      fly_v3mk(0.55f, 0.56f, 0.58f), 0.6f, 40.0f);
    if (vm & FLY_VIS_CARGOPOD)
        model_box(rt, v, e, &f, 0, 0, belly + 0.22f, 0.62f, 0.40f, 0.22f,
                  fly_v3scale(tint, 0.6f), 0.2f, 10.0f);
    if (vm & FLY_VIS_BELLYTANK)
        model_box(rt, v, e, &f, 0, 0, belly + 0.20f, 0.50f, 0.28f, 0.20f,
                  fly_v3mk(0.55f, 0.56f, 0.58f), 0.6f, 40.0f);
    /* The armed loadout at silhouette scale. Only the two that change the
     * outline enough to be worth a primitive at this range: rockets and
     * missiles under the wing say "this one is armed" from further out than a
     * gun pod does, which is exactly the judgement the far LOD is for. The
     * dispenser and the emitter are inside the noise here and are left to the
     * near model. */
    if (vm & (FLY_VIS_ROCKETPOD | FLY_VIS_MISSILE))
        for (k = -1; k <= 1; k += 2)
            model_box(rt, v, e, &f, -0.10f, 0.60f * (float)k, -0.06f,
                      (vm & FLY_VIS_MISSILE) ? 0.44f : 0.30f, 0.10f, 0.10f,
                      fly_v3mk(0.30f, 0.31f, 0.33f), 0.4f, 20.0f);
    if (vm & FLY_VIS_BOMBRACK)
        model_box(rt, v, e, &f, 0, 0, belly + 0.16f, 0.52f, 0.24f, 0.12f,
                  fly_v3mk(0.30f, 0.31f, 0.28f), 0.2f, 8.0f);
    if (vm & FLY_VIS_RADARDOME)
        model_box(rt, v, e, &f, -0.25f, 0, 0.42f, 0.28f, 0.28f, 0.10f,
                  fly_v3mk(0.72f, 0.73f, 0.75f), 0.7f, 50.0f);
    if (vm & FLY_VIS_ANTENNA)
        model_tube(rt, v, e, fp(&f, -0.80f, 0, 0.24f), fp(&f, -0.80f, 0, 0.86f),
                   0.030f * sc, 6, f.Y, fly_v3mk(0.6f, 0.6f, 0.62f), 0.5f, 30.0f);
    if (vm & FLY_VIS_ARMOR)
        for (k = -1; k <= 1; k += 2)
            model_box(rt, v, e, &f, 0, 0.58f * (float)k, 0.06f, 0.72f, 0.04f, 0.24f,
                      fly_v3mk(0.15f, 0.16f, 0.18f), 0.3f, 16.0f);
}

/* The glare around a small bright source, as an additive HDR splat.
 *
 * It used to be two flat discs — one of radius r at 3.2 and one of 2r at 0.35,
 * each with a one-pixel coverage skirt — and from anywhere near a lamp-post
 * that is exactly what it looked like: a hard bright coin inside a slightly
 * larger hard dim one, with a step between them. A sphere painted on the frame.
 *
 * What is actually around a bright light is a veiling glare, and it has no
 * edge anywhere: the eye's own scatter, the camera's, and the air's between all
 * fall off as a power of the angle rather than stopping. FLY_GLARE_CORE is that
 * profile — a Cauchy squared, 1/(1+t)^2 in t = (d/r)^2 — which integrates over
 * the plane to exactly pi*r^2 per unit amplitude, so the two lobes below carry
 * the same energy the two discs did while ending in a gradient instead of a
 * rim. The wide lobe is the haze around a street lamp on a damp night; the
 * narrow one is the lamp.
 *
 * Truncated at FLY_GLARE_SPAN core radii, with a pixel of linear window at the
 * cut, because a power law never reaches zero and a frame has to end somewhere.
 * Twin of the same arithmetic in FLY_SPLAT_FS. */
#define FLY_GLARE_CORE 3.55f     /* amplitude of the narrow lobe */
#define FLY_GLARE_HALO 0.17f     /* ...and of the wide one */
#define FLY_GLARE_WIDE 2.4f      /* how many core radii wide that one is */
#define FLY_GLARE_SPAN 5.0f      /* where the splat is cut off */
static void hdr_add_glare(fly_render_target *rt, float sx, float sy, float r, fly_v3 rad) {
    float R = r * FLY_GLARE_SPAN + 1.5f;
    float ir = 1.0f / (r * r), ih = 1.0f / (r * FLY_GLARE_WIDE * r * FLY_GLARE_WIDE);
    int minx = (int)floorf(sx - R), maxx = (int)ceilf(sx + R);
    int miny = (int)floorf(sy - R), maxy = (int)ceilf(sy + R);
    int x, y;
    for (y = miny; y <= maxy; ++y) {
        if (y < 0 || y >= rt->h) continue;
        for (x = minx; x <= maxx; ++x) {
            float dx, dy, d2, tc, th, w;
            int idx;
            if (x < 0 || x >= rt->w) continue;
            dx = (float)x + 0.5f - sx;
            dy = (float)y + 0.5f - sy;
            d2 = dx * dx + dy * dy;
            if (d2 > R * R) continue;
            tc = 1.0f + d2 * ir;
            th = 1.0f + d2 * ih;
            w = (FLY_GLARE_CORE / (tc * tc) + FLY_GLARE_HALO / (th * th)) *
                fly_clampf(R - sqrtf(d2), 0.0f, 1.0f);
            idx = y * rt->w + x;
            rt->hdr[idx] = fly_v3add(rt->hdr[idx], fly_v3scale(rad, w));
        }
    }
}

/* depth-tested emissive light sprite (nav lights, strobes): true HDR
 * emitters, so the bloom pass makes them glow */
static void light_splat(fly_render_target *rt, float sx, float sy, float z,
                        uint32_t col, float size) {
    int ix = (int)sx, iy = (int)sy;
    if (ix < 0 || iy < 0 || ix >= rt->w || iy >= rt->h) return;
    if (rt->depth[iy * rt->w + ix] + 6.0f < z) return; /* occluded */
    float a = (float)(col >> 24) / 255.0f;
    fly_v3 lin = fly_v3scale(lin_from_u32(col), a);
    /* The ceiling is what a glare does as you close on a lamp: it grows. Held
       at 2.2 px it stopped growing at fifty metres, so the last fifty were a
       bead of fixed size that read as a painted dot rather than as a light you
       were walking up to. */
    float r = fly_clampf(110.0f / z, 0.7f, 3.2f) * size;
    hdr_add_glare(rt, sx, sy, r, lin);
}

static void draw_light(fly_render_target *rt, const fly__view *v, fly_v3 p,
                       uint32_t col, float size) {
    if (g_shadow_cast) return; /* emitters cast no shadow */
    float sx, sy, z;
    if (!project(v, rt->w, rt->h, p, &sx, &sy, &z)) return;
    /* A lamp is a point source, and `light_splat` floors its radius at most of
     * a pixel so that a beacon at range does not flicker in and out with the
     * sampling. That floor has no far end, which is how twenty-four aircraft
     * over the settled world came out as a scatter of white specks on a planet
     * seen from a hundred and fifty kilometres up — spots with nothing behind
     * them, on ground the near field no longer even draws.
     *
     * A navigation lamp is a few tens of watts. Real ones are called at five to
     * ten kilometres in clear air and nothing sees one at twenty-five, so that
     * is where this ends: unchanged over everything a lamp is for — a circuit,
     * an approach, a settlement across a valley — and gone by the time the
     * aircraft carrying it is itself under a pixel. */
    {
        float d = fly_v3dist(p, v->pos);
        float k = 1.0f - fly_smoothstepf(9000.0f, 26000.0f, d);
        if (k <= 0.004f) return;
        if (k < 1.0f)
            col = (col & 0x00FFFFFFu) |
                  ((uint32_t)((float)(col >> 24) * k + 0.5f) << 24);
    }
    /* during capture the z-buffer these test against does not exist yet, so
     * record the splat and replay it once the frame has been read back */
    if (g_capture_lights) {
        if (g_nlights < FLY_LIGHT_MAX) {
            fly__lightsplat *o = &g_lights[g_nlights++];
            o->sx = sx; o->sy = sy; o->z = z; o->size = size; o->col = col;
        }
        return;
    }
    light_splat(rt, sx, sy, z, col, size);
}

/* A navigation lamp of a given colour, scaled so every lamp on the aircraft
 * puts out the same luminance whatever colour it is.
 *
 * They are one rating of lamp in two colours and they have to read as a pair:
 * left untouched, sRGB (60,255,90) carries 0.73 of luminance against
 * (255,40,40)'s 0.23, so the starboard light came out three times brighter
 * than its port twin — and being a true HDR emitter it also bloomed three
 * times as far. That is the sort of asymmetry the eye catches immediately and
 * cannot name, which is exactly why it is worth fixing rather than arguing
 * about. `want` is the luminance both are held to. */
static uint32_t nav_lamp(int r, int g, int b, float want) {
    fly_v3 lin;
    r += (int)((255 - r) * 0.32f); g += (int)((255 - g) * 0.32f); b += (int)((255 - b) * 0.32f);
    lin = lin_from_u32(FLY_RGBA(r, g, b, 255));
    float y = 0.2126f * lin.x + 0.7152f * lin.y + 0.0722f * lin.z;
    float a = fly_clampf(want / (y > 1e-4f ? y : 1e-4f), 0.0f, 1.0f);
    return FLY_RGBA(r, g, b, (int)(a * 255.0f + 0.5f));
}

/* One horizontal ring of a body of revolution about the frame's z axis: the
 * disc's own primitive, the way `body_ring` is the fuselage's. Half-step
 * angles, for the reason given where those are laid out — and the same
 * convention, so k and n-1-k are mirror partners across y = 0 and `loft_hull`
 * splits the quads it straddles the way it expects to. */
static void disc_ring(const fly__frame *f, float r, float z, int n, fly_v3 *out) {
    int k;
    for (k = 0; k < n; ++k) {
        float a = ((float)k + 0.5f) * (2.0f * FLY_PI / (float)n);
        out[k] = fp(f, cosf(a) * r, sinf(a) * r, z);
    }
}

/* The saucer.
 *
 * The airframe already says what it is: `antigrav` is the one number in the
 * flight model with no engineering behind it, and it is nonzero on exactly one
 * thing. Drawn as an aeroplane, the only thing in the world that does not fly
 * with wings was a Skylark hanging motionless a hundred and fifty kilometres
 * up, which is the one place in the game where a familiar silhouette is the
 * wrong answer.
 *
 * So: a lens with a dome, lofted about the vertical, and no aerofoil anywhere
 * on it. Nothing about the shape is a special case of the aircraft builders —
 * `loft_hull` closes it and `tri_spec` shades it, the same two calls a
 * fuselage goes through — and its axis is the only difference. It is drawn to
 * `wing_span` like everything else, so the disc is as wide as the airframe
 * claims to be rather than a size chosen here.
 *
 * The rim lamps run round it in a cycle rather than blinking on a schedule
 * like a strobe. That is the whole read at distance, where the model is six
 * pixels across: an aircraft's lights are a fixed red, a fixed green and a
 * white flash, and this one is a wave going round a circle. */
static void draw_saucer_model(fly_render_target *rt, const fly__view *v, const fly__env *e,
                              const fly_craft *c, const fly_airframe *af, fly_v3 tint,
                              double time) {
    /* radius, z — in units of the disc's own radius.
     *
     * Two lofts and not one, split at the rim. loft_hull carries at most eight
     * stations and averages its normals across every one of them, so a single
     * profile through the rim gets both: too few stations to curve, and a rim
     * whose crease is smoothed away into the two facets either side of it. A
     * saucer's rim is a real edge — it is the one hard line on the object — so
     * splitting there costs nothing and buys twelve stations of curve.
     *
     * What this replaced was six stations for the whole hull, three of them
     * above the rim, which is a hexagonal section: from any angle you could
     * count the creases down the dish. */
    static const float LO[6][2] = {          /* the underside, centre outward */
        { 0.00f, -0.30f }, { 0.26f, -0.283f }, { 0.50f, -0.238f },
        { 0.72f, -0.171f }, { 0.90f, -0.093f }, { 1.00f,  0.000f } };
    static const float UP[6][2] = {          /* the rim, back in to the collar */
        { 1.00f,  0.000f }, { 0.90f,  0.052f }, { 0.74f,  0.098f },
        { 0.58f,  0.132f }, { 0.44f,  0.155f }, { 0.34f,  0.168f } };
    static const float DM[5][2] = {          /* the dome */
        { 0.34f,  0.160f }, { 0.31f,  0.216f }, { 0.25f,  0.264f },
        { 0.15f,  0.300f }, { 0.00f,  0.318f } };
    float rad = (af->wing_span > 1.0f ? af->wing_span : 22.0f) * 0.5f;
    int lod = craft_lod(v, e, c->pos, rad * 2.0f);
    /* Its own segment count, not the shared body ring.
     *
     * A fuselage is a tube read from the side and ten sides is plenty of it; a
     * saucer is a disc read broadside and edge-on, and ten sides on a disc is a
     * decagon you can count in the gallery plate. There is one of these in a
     * world and it is the thing a player flies over to look at, so it gets the
     * full ring at close range whatever rung the rest of the frame is on —
     * about six hundred triangles for the whole object, against tens of
     * thousands in the terrain mesh under it. */
    int n = lod >= 2 ? FLY_RING_N : lod >= 1 ? 12 : 8;
    fly_v3 ring[6 * FLY_RING_N];
    fly__frame f;
    /* Barely tinted. The caller's colour is a faction livery and a saucer does
     * not have one; enough of it survives to keep two contacts apart and not
     * enough to make it a painted aeroplane. */
    fly_v3 hull = fly_v3lerp(fly_v3mk(0.22f, 0.23f, 0.26f), tint, 0.14f);
    int i, k;
    craft_frame(c, rad, &f);

    /* Hard and glossy: whatever it is made of is not painted aluminium, and
     * the specular is most of what says so at the size it is usually seen. */
    for (i = 0; i < 6; ++i) disc_ring(&f, LO[i][0], LO[i][1], n, ring + (size_t)i * n);
    loft_hull(rt, v, e, ring, 6, n, fly_v3scale(hull, 0.82f), 1.15f, 120.0f);
    for (i = 0; i < 6; ++i) disc_ring(&f, UP[i][0], UP[i][1], n, ring + (size_t)i * n);
    loft_hull(rt, v, e, ring, 6, n, hull, 1.35f, 140.0f);

    /* The rim itself: a band a thousandth of a radius deep, in a darker metal.
     * A knife edge between two lofts is geometrically an edge and reads as
     * nothing, because there is no facet on it to catch the light. Give it one
     * and the object gets a bright line all the way round, which is what says
     * the dish is a machined thing rather than a lens flare. */
    if (lod >= 1) {
        fly_v3 band[2 * FLY_RING_N];
        disc_ring(&f, 1.002f, -0.016f, n, band);
        disc_ring(&f, 1.002f, 0.016f, n, band + n);
        loft_hull(rt, v, e, band, 2, n, fly_v3scale(hull, 1.45f), 2.2f, 220.0f);
    }

    /* The belly plate: a shallow disc set into the underside, which is the
     * side you see when one of these is holding station above you. Without it
     * the underside is one unbroken dish thirty metres across and reads as a
     * shape rather than as a machine. */
    if (lod >= 1) {
        fly_v3 plate[2 * FLY_RING_N];
        disc_ring(&f, 0.30f, -0.292f, n, plate);
        disc_ring(&f, 0.34f, -0.276f, n, plate + n);
        loft_hull(rt, v, e, plate, 2, n, fly_v3scale(hull, 0.58f), 0.9f, 80.0f);
    }

    if (lod >= 1) {
        /* The collar the dome stands in, and the dome. The collar is proud of
         * the hull by a hair, so the dome has a base rather than growing out
         * of the plating, and the join is a shadow line instead of a seam. */
        fly_v3 col[2 * FLY_RING_N], dome[5 * FLY_RING_N];
        disc_ring(&f, 0.360f, 0.150f, n, col);
        disc_ring(&f, 0.345f, 0.178f, n, col + n);
        loft_hull(rt, v, e, col, 2, n, fly_v3scale(hull, 0.66f), 1.1f, 90.0f);
        for (i = 0; i < 5; ++i) disc_ring(&f, DM[i][0], DM[i][1], n, dome + (size_t)i * n);
        loft_hull(rt, v, e, dome, 5, n, fly_v3mk(0.42f, 0.62f, 0.70f), 1.8f, 180.0f);
    }

    /* The rim lamps, and the one underneath. Twelve where the model is worth
     * twelve and eight beyond that: the lights are what carries at range and
     * the geometry is what does not, so they are the last thing that should
     * thin out. */
    {
        const int NL = lod >= 2 ? 12 : 8;
        float phase = (float)fmod(time * 1.1, 1.0);
        for (k = 0; k < NL; ++k) {
            float a = (float)k * (2.0f * FLY_PI / (float)NL);
            float u = (float)k / (float)NL - phase;
            float lit;
            u -= floorf(u);
            lit = 0.18f + 0.82f * fly_clampf(1.0f - u * 4.0f, 0.0f, 1.0f);
            draw_light(rt, v, fp(&f, cosf(a) * 0.99f, sinf(a) * 0.99f, -0.01f),
                       nav_lamp(255, 170, 60, 0.22f * lit), 1.1f);
        }
        draw_light(rt, v, fp(&f, 0.0f, 0.0f, -0.30f),
                   nav_lamp(120, 255, 200, 0.30f), 2.0f);
    }
}

/* --- livery ---------------------------------------------------------------
 *
 * What an aircraft is painted, and it is the single largest thing factions do
 * to a frame. Every aeroplane in the world that was not the player's used to
 * be drawn the same dark red, so the sky was "you, and twenty strangers"; it
 * is now seven liveries and the unaligned, and a player who has flown for an
 * hour can tell at two kilometres whether the thing crossing the valley is
 * somebody they are at war with.
 *
 * The player is the exception and stays orange while unaligned. That orange is
 * the one colour in the game that means "this one is you", and handing it to
 * the Free Reaches would cost more than it bought — but the moment an oath is
 * taken the hull repaints in the power's colours, which is the most direct
 * statement the renderer can make about a decision the player took on a pad. */
static fly_v3 craft_livery(int faction, int is_player) {
    if (is_player && faction == FLY_FACTION_FREE) return fly_v3mk(0.78f, 0.42f, 0.20f);
    return fly_faction_colour(faction);
}

static fly_v3 craft_trim(int faction, int is_player) {
    if (is_player && faction == FLY_FACTION_FREE) return fly_v3mk(0.92f, 0.72f, 0.28f);
    return fly_faction_trim(faction);
}

static void draw_craft(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_craft *c, const fly_airframe *af, const fly_controls *in,
                       fly_v3 tint, fly_v3 trim, double time) {
    int drone = af->kind == FLY_CRAFT_DRONE;
    float span = af->wing_span > (drone ? 0.4f : 1.0f) ? af->wing_span : (drone ? 1.6f : 11.0f);
    fly__frame f;
    fly_v3 port, stbd, strobe;
    float night = 0.6f + (1.0f - e->day) * 0.4f;
    if (!object_worth_drawing(v, rt, c->pos, span * 0.8f + 4.0f)) return;
    /* Whatever is flying this is not doing it with wings, and the model says
     * so off the same field the flight model does. Its lights are its own and
     * it takes none of the three below: a saucer showing a red port lamp is a
     * saucer complying with an air law nobody up there has heard of. */
    if (af->antigrav > 0.5f) { draw_saucer_model(rt, v, e, c, af, tint, time); return; }
    if (drone) draw_drone_model(rt, v, e, c, af, tint, trim, time);
    else draw_plane_model(rt, v, e, c, af, in, tint, trim, time);

    /* Navigation lights: red to port, green to starboard, white tail strobe.
     * The wingtip pair is read out of the same planform the wing is built
     * from, at the same span, so the lamps sit on the metal and the two are at
     * mirrored points rather than at two sets of literals that used to be a
     * metre and a half adrift of the tip they belonged to. */
    if (drone) {
        craft_frame(c, span * 0.35355f, &f);
        /* on the forward arms, outboard and below the motor can so the lamp is
         * not sitting inside it */
        port = fp(&f, 1.06f, 1.06f, 0.12f);
        stbd = fp(&f, 1.06f, -1.06f, 0.12f);
        strobe = fp(&f, -1.0f, 0, 0.28f);
    } else {
        float halfs = plane_half_span(af), xle, xte, z, th;
        craft_frame(c, halfs / 5.5f, &f);
        wing_at(1.0f, &xle, &xte, &z, &th);
        port = fp(&f, xte + 0.10f, 5.5f, z + 0.03f);
        stbd = fp(&f, xte + 0.10f, -5.5f, z + 0.03f);
        strobe = fp(&f, -4.66f, 0, 2.45f);
    }
    draw_light(rt, v, port, nav_lamp(255, 40, 40, 0.20f * night), 1.0f);
    draw_light(rt, v, stbd, nav_lamp(60, 255, 90, 0.20f * night), 1.0f);
    if (fmod(time, 1.3) < 0.08)
        draw_light(rt, v, strobe, FLY_RGBA(255, 255, 255, 255), 1.5f);
}

/* ---------------- weather overlays ---------------- */

/* Both of these are overlays on the finished picture rather than parts of the
 * scene, so they take the image they are drawn onto: at any rung above the
 * first that is the *output* frame, not the supersampled one the scene was
 * drawn at. A streak measured in supersamples is a shorter, denser streak once
 * the box filter has been over it, which made the rain quietly change
 * character with the graphics level. */
static void draw_rain_img(fly_img *im, const fly__env *e, float precip) {
    if (precip < 0.05f) return;
    int n = (int)(precip * 130.0f) * (im->w / 320 + 1);
    uint32_t frame = (uint32_t)(e->time * 12.0);
    float tilt = fly_clampf(e->wind.x * 0.02f, -0.6f, 0.6f) * (float)im->h * 0.03f;
    int i;
    for (i = 0; i < n; ++i) {
        uint32_t h1 = fly_hash2(e->seed + 91, (int32_t)i, (int32_t)frame);
        float x = (float)(h1 % (uint32_t)im->w);
        float y = (float)((h1 >> 12) % (uint32_t)im->h);
        float len = 8.0f + (float)(h1 >> 28);
        fly_img_aa_line(im, x, y, x + tilt, y + len, 1.0f,
                        FLY_RGBA(180, 200, 220, (int)(70 * precip)));
    }
}

static void draw_lightning_img(fly_img *im, const fly__env *e) {
    if (e->storm < 0.55f) return;
    uint32_t slot = (uint32_t)(e->time / 0.9);
    uint32_t h = fly_hash2(e->seed + 97, (int32_t)slot, 7);
    if ((h & 15u) > (uint32_t)(e->storm * 4.0f)) return;
    float phase = (float)fmod(e->time, 0.9) / 0.9f;
    if (phase > 0.22f) return;
    int a = (int)(120.0f * (1.0f - phase / 0.22f));
    fly_img_fill_rect(im, 0, 0, im->w, im->h, FLY_RGBA(235, 240, 255, a));
}

/* ---------------- volumetric sun shafts ----------------
 *
 * The air between the camera and the world is lit, and until now it was lit
 * unconditionally: `fly__scatter` integrates the in-scattered sunlight along a
 * ray and asks nothing about whether that stretch of air is in the sun. A
 * mountain therefore shaded its own slope and not the haze in front of it, a
 * hangar shaded the apron and not the dust over it, and the one thing that
 * makes a low sun in a valley read as a low sun in a valley — a solid beam
 * where the air is lit next to a dull one where it is not — could not happen
 * at all.
 *
 * This is that term, and the shape of it is worth being precise about because
 * it is the difference between a light shaft and a lens flare. It **only ever
 * takes light away**.
 *
 * How much it is worth depends entirely on what is standing in the light. Over
 * open country it is a ten-thousandth of the frame — not a failure, just the
 * honest amount of in-scatter that a few kilometres of clear air contributes to
 * a pixel, shadowed or not. Put a city's towers in front of a low sun and the
 * deepest correction is thirty times that. Measuring it somewhere with nothing
 * in it measures the sky. The frame has already added the fully-lit in-scatter, so
 * what is missing is the part of it that never happened, and the beams are
 * what is left standing when the air around them goes down. Adding a bright
 * streak instead would put energy into the frame that no light source emitted,
 * and would put it there in the shape of the screen rather than the shape of
 * the world — which is what the post chain's radial gather does, deliberately
 * and separately, as a lens effect.
 *
 * Three things keep it affordable.
 *
 *  - It marches the near field only. Shafts are a contrast between two parcels
 *    of air a few hundred metres apart; past the reach below the shadow map has
 *    no cascade to answer with and the analytic average is the right answer
 *    anyway. The far part of the path keeps its unshadowed in-scatter, weighted
 *    in by the air it actually crosses, so a ray that ends in space is barely
 *    touched and a ray that ends on a hillside is fully corrected.
 *  - It runs on a half-resolution grid and is bilinearly resampled. The field
 *    is smooth by construction — it is an integral over hundreds of metres of
 *    air — so the only thing full resolution would buy is four times the cost.
 *  - Each sample dithers its march by a hash of its own pixel, so the step
 *    count shows as a little noise rather than as the ring of hard shells that
 *    an aligned march puts around every occluder. The hash is a pure function
 *    of the pixel, so the frame stays reproducible.
 *
 * It runs after the environment and the objects and before the resolve, off
 * the finished depth buffer, which is what makes it identical whichever
 * pipeline drew the frame — there is no GLSL copy of this to drift.
 *
 * What it marches is the sun shadow map, so the occluders are terrain,
 * settlements, the rail, the aircraft and the woodland — everything that
 * casts. The cloud deck is not marched here because the analytic in-scatter
 * already gates itself on it; the two are different halves of the same term,
 * not two versions of it.
 */

#define FLY_SHAFT_REACH 4200.0f  /* how far down the ray the march looks, m */

/* what the last pass did, for fly_render_shaft_stats */
static double g_shaft_sum, g_shaft_worst;
static long g_shaft_n;

/* The lattice the march runs on: one ray per two *output* pixels, whatever the
 * supersampling.
 *
 * The stride used to be two render-target pixels, which quietly made this pass
 * one of the things supersampling multiplies: at ssaa 2 it marched four times
 * as many rays for the same picture and at ssaa 3, nine. It should not be.
 * Supersampling is there to resolve edges, and this term has none to resolve —
 * it is bilinearly upsampled out of a lattice on its way into the frame, so
 * every ray beyond the one per two output pixels that ssaa 1 asks for is
 * interpolated back away by the box filter.
 *
 * Shared with the GLSL port below, which is the only way the two can be held
 * to producing the same field. */
static void shaft_lattice(int w, int h, int aa, int *hw, int *hh, int *st) {
    int s = 2 * (aa > 1 ? aa : 1);
    *st = s;
    *hw = (w + s - 1) / s;
    *hh = (h + s - 1) / s;
}

static void volumetric_shafts(fly_render_target *rt, const fly__env *e, const fly__view *v) {
    static fly_v3 *buf;
    static int bw, bh;
    int steps = e->lod.shafts;
    int st, hw, hh, i, j, x, y;
    g_shaft_sum = g_shaft_worst = 0.0;
    g_shaft_n = 0;
    if (steps <= 0) return;
    if (!e->shadow || !e->shadow->active || !e->shadow->depth[0]) return;
    /* No beam, no shafts: the correction is a fraction of the sun's own
     * in-scatter and there is none of it once the sun is down. */
    if (e->sun.z < 0.02f) return;
    shaft_lattice(rt->w, rt->h, v->aa, &hw, &hh, &st);
    if (hw != bw || hh != bh) {
        fly_v3 *nb = (fly_v3 *)realloc(buf, (size_t)hw * (size_t)hh * sizeof *buf);
        if (!nb) return;
        buf = nb;
        bw = hw;
        bh = hh;
    }
    for (j = 0; j < hh; ++j)
        for (i = 0; i < hw; ++i) {
            int px = i * st < rt->w ? i * st : rt->w - 1;
            int py = j * st < rt->h ? j * st : rt->h - 1;
            fly_v3 ro = v->pos, dv, rd, S;
            float len, depth, dist, span, seen = 0.0f, wsum = 0.0f, jit;
            float ox = ((float)px - rt->w * 0.5f) / v->sx;
            float oy = (rt->h * 0.5f - (float)py) / v->sy;
            int k;
            buf[(size_t)j * hw + i] = fly_v3zero();
            if (v->ortho) {
                ro = fly_v3add(ro, fly_v3add(fly_v3scale(v->right, ox),
                                             fly_v3scale(v->up, oy)));
                rd = v->fwd;
                len = 1.0f;
            } else {
                dv = fly_v3add(v->fwd, fly_v3add(fly_v3scale(v->right, ox),
                                                 fly_v3scale(v->up, oy)));
                len = fly_v3len(dv);
                rd = fly_v3scale(dv, 1.0f / len);
            }
            /* A ray climbing out of the ground layer is out of the shadow map
             * within a few hundred metres, and what shafts it does carry come
             * off the cloud deck, which the analytic in-scatter already gates.
             * Marching it buys noise and costs the whole sky. */
            if (rd.z > 0.35f) continue;
            depth = rt->depth[(size_t)py * rt->w + px];
            dist = depth >= 1e29f ? FLY_SHAFT_REACH : depth * len;
            span = dist < FLY_SHAFT_REACH ? dist : FLY_SHAFT_REACH;
            if (span < 8.0f) continue;
            jit = (float)(fly_hash2(e->seed ^ 0x5f3au, px, py) & 1023u) / 1024.0f;
            for (k = 0; k < steps; ++k) {
                float t = span * ((float)k + jit) / (float)steps;
                fly_v3 p = fly_v3add(ro, fly_v3scale(rd, t));
                /* the air's own weight: what scatters is proportional to how
                 * much of it there is, so the dense near end counts for more */
                float w = expf(-(p.z > 0.0f ? p.z : 0.0f) / FLY_SCAT_HR);
                /* The shadow map only. What the cloud deck lets through is
                 * already in `S` — fly__scatter gates its in-scatter on a tap
                 * of the deck — and multiplying it in again here would remove
                 * the same light twice, which is how a physically shaped
                 * correction turns into a hole under every cloud. */
                seen += w * shadow_beam(e->shadow, p);
                wsum += w;
            }
            if (wsum <= 0.0f) continue;
            ++g_shaft_n;
            /* Nothing stood between this air and the sun, so the correction is
             * exactly zero and the in-scatter it would have been scaling is a
             * number nobody reads. Skipping it is not an approximation — the
             * taps are 0 or 1 and `seen` accumulates the same weights in the
             * same order as `wsum`, so the two are bit-equal on a lit ray and
             * the product is a true zero — and out of doors under a high sun
             * it is most of the frame: the pass is called volumetric *shadows*
             * and it is now priced on the shadows rather than on the volume. */
            if (seen >= wsum) continue;
            fly__scatter(e, ro, rd, span, NULL, &S);
            {
                fly_v3 d = fly_v3scale(S, seen / wsum - 1.0f);
                double m = (double)(d.x + d.y + d.z) / 3.0;
                buf[(size_t)j * hw + i] = d;
                g_shaft_sum += m;
                if (m < g_shaft_worst) g_shaft_worst = m;
            }
        }
    for (y = 0; y < rt->h; ++y) {
        float fy = (float)y / (float)st;
        int j0 = (int)fy, j1 = j0 + 1 < hh ? j0 + 1 : hh - 1;
        float ty = fy - (float)j0;
        for (x = 0; x < rt->w; ++x) {
            float fx = (float)x / (float)st;
            int i0 = (int)fx, i1 = i0 + 1 < hw ? i0 + 1 : hw - 1;
            float tx = fx - (float)i0;
            fly_v3 a = fly_v3lerp(buf[(size_t)j0 * hw + i0], buf[(size_t)j0 * hw + i1], tx);
            fly_v3 b = fly_v3lerp(buf[(size_t)j1 * hw + i0], buf[(size_t)j1 * hw + i1], tx);
            fly_v3 d = fly_v3lerp(a, b, ty);
            fly_v3 *o = &rt->hdr[(size_t)y * rt->w + x];
            /* A term that removes light must not remove more than there is:
             * a negative radiance is a NaN out of the resolve's log-average
             * exposure and a black frame out of that. */
            o->x = o->x + d.x > 0.0f ? o->x + d.x : 0.0f;
            o->y = o->y + d.y > 0.0f ? o->y + d.y : 0.0f;
            o->z = o->z + d.z > 0.0f ? o->z + d.z : 0.0f;
        }
    }
}

/* ---- the same correction, run where the frame is ----
 *
 * A port of the pass above, and it exists for one reason: the CPU version
 * needs the finished depth buffer and the shadow cascades in main memory, and
 * insisting on that is what pinned a whole frame to the wrong side of the bus.
 * It forced the cascades to be rasterized on the CPU at every level that asks
 * for shafts — the two expensive ones — and it forced the radiance buffer to
 * be read back before anything could be added to it.
 *
 * The march is the same march: same lattice, same jitter, same weights, same
 * early outs, and the same "only ever takes light away" clamp. What changes is
 * where the two inputs come from. View depth is the alpha the environment pass
 * already writes into the frame, and the cascades are the images the GPU
 * builder already left in GL memory, so the pass reads what is in front of it
 * instead of asking for a copy. The CPU version stays the reference and still
 * runs whenever the environment did. */
#define FLY_GLSL_SHAFT_REACH "4200.0"

static const char *FLY_SHAFT_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "uniform vec3 uSmRight, uSmUp, uSmFwd;\n"
    "uniform vec3 uSmUmin, uSmVmin, uSmTexel;\n"
    "uniform vec3 uSmSize;\n"
    "uniform ivec3 uSmPcf;\n"
    "uniform int uSmActive;\n"
    "uniform sampler2D uSmNear, uSmMid, uSmFar;\n"
    "uniform sampler2D uFrame;\n"   /* the resident frame; alpha is view depth */
    "uniform vec3 uShaftGrid;\n"    /* lattice width, height, stride in pixels */
    "uniform int uShaftSteps;\n"
    "uniform int uShaftSeed;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    FLY_GLSL_SHADOW
    "void main(){\n"
    "  int hw = int(uShaftGrid.x), hh = int(uShaftGrid.y), st = int(uShaftGrid.z);\n"
    "  int W = int(uRes.x), H = int(uRes.y);\n"
    /* the lattice is indexed in the image's top-down rows, like the CPU's */
    "  int i = int(gl_FragCoord.x), j = hh - 1 - int(gl_FragCoord.y);\n"
    "  int px = i*st < W ? i*st : W - 1, py = j*st < H ? j*st : H - 1;\n"
    "  float ox = (float(px) - float(W)*0.5)/uScale;\n"
    "  float oy = (float(H)*0.5 - float(py))/uScale;\n"
    "  vec3 ro = uCamPos, rd; float len;\n"
    /* alpha is not read by the composite; it says whether this lattice point
       was corrected at all, which is what fly_render_shaft_stats counts */
    "  oColor = vec4(0.0);\n"
    "  if (hw < 1) return;\n"
    "  if (uOrtho != 0) { ro += uCamRight*ox + uCamUp*oy; rd = uCamFwd; len = 1.0; }\n"
    "  else { vec3 dv = uCamFwd + uCamRight*ox + uCamUp*oy;\n"
    "         len = length(dv); rd = dv/len; }\n"
    /* a ray climbing out of the ground layer leaves the map within a few
       hundred metres — see volumetric_shafts */
    "  if (rd.z > 0.35) return;\n"
    "  float depth = texelFetch(uFrame, ivec2(px, H - 1 - py), 0).a;\n"
    "  float dist = depth >= 1e29 ? " FLY_GLSL_SHAFT_REACH " : depth*len;\n"
    "  float span = min(dist, " FLY_GLSL_SHAFT_REACH ");\n"
    "  if (span < 8.0) return;\n"
    "  float jit = float(fly_hash2(uint(uShaftSeed), px, py) & 1023u)/1024.0;\n"
    "  float seen = 0.0, wsum = 0.0;\n"
    "  for (int k = 0; k < uShaftSteps; ++k) {\n"
    "    float t = span*(float(k) + jit)/float(uShaftSteps);\n"
    "    vec3 p = ro + rd*t;\n"
    "    float w = exp(-max(p.z, 0.0)/8000.0);\n"
    "    seen += w*fly_shadow_beam(p);\n"
    "    wsum += w;\n"
    "  }\n"
    /* nothing stood in the light, so the correction is a true zero */
    "  if (wsum <= 0.0 || seen >= wsum) return;\n"
    "  vec3 T, S;\n"
    "  fly_scatter(ro, rd, span, T, S);\n"
    "  oColor = vec4(S*(seen/wsum - 1.0), 1.0);\n"
    "}\n";

/* The correction laid into the frame, bilinearly upsampled off the lattice
 * exactly as the CPU pass does — and, when there is no correction to lay in, a
 * plain copy. The copy is not waste: the passes after this one have to read
 * the frame's own view depth while they write over it, and a texture cannot be
 * both at once, so this is where the frame moves to its other target. */
static const char *FLY_FRAME_FS =
    FLY_GLSL_HEADER
    "uniform sampler2D uFrame, uShaft;\n"
    "uniform vec2 uSize;\n"
    "uniform vec3 uShaftGrid;\n"
    "uniform int uShaftOn;\n"
    "out vec4 oColor;\n"
    "vec3 fly_shaft_at(int i, int j){\n"
    "  int hh = int(uShaftGrid.y);\n"
    "  return texelFetch(uShaft, ivec2(i, hh - 1 - j), 0).rgb;\n"
    "}\n"
    "void main(){\n"
    "  ivec2 c = ivec2(gl_FragCoord.xy);\n"
    "  vec4 f = texelFetch(uFrame, c, 0);\n"
    "  if (uShaftOn == 0) { oColor = f; return; }\n"
    "  int hw = int(uShaftGrid.x), hh = int(uShaftGrid.y);\n"
    "  float st = uShaftGrid.z;\n"
    "  int y = int(uSize.y) - 1 - c.y;\n"
    "  float fx = float(c.x)/st, fy = float(y)/st;\n"
    "  int i0 = int(fx), j0 = int(fy);\n"
    "  int i1 = i0 + 1 < hw ? i0 + 1 : hw - 1, j1 = j0 + 1 < hh ? j0 + 1 : hh - 1;\n"
    "  float tx = fx - float(i0), ty = fy - float(j0);\n"
    "  vec3 a = mix(fly_shaft_at(i0, j0), fly_shaft_at(i1, j0), tx);\n"
    "  vec3 b = mix(fly_shaft_at(i0, j1), fly_shaft_at(i1, j1), tx);\n"
    /* a term that removes light must not remove more than there is */
    "  oColor = vec4(max(f.rgb + mix(a, b, ty), vec3(0.0)), f.a);\n"
    "}\n";

/* ---- emissive splats and the translucent pass, over the resident frame ----
 *
 * Both used to run on the CPU off the read-back frame, and both are small:
 * a few dozen nav lamps and a couple of thousand triangles of propeller disc,
 * fireball and plume. They are here because of what they were holding up, not
 * because of what they cost — a frame cannot stay on the GPU while two of its
 * stages insist on a pointer to it.
 *
 * Neither uses a depth buffer. Multisampling stores depth per sample and there
 * is no defined way to resolve that into something a later pass can read, so
 * both test against the view depth in the frame's own alpha — which is what
 * the CPU versions compare against too, in the same units. */
static const char *FLY_SPLAT_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec2 aPix;\n"   /* corner, image pixels (top-down) */
    "layout(location = 1) in vec3 aCtr;\n"   /* centre x, y and its view depth */
    "layout(location = 2) in vec4 aCol;\n"   /* linear radiance, disc radius */
    "uniform vec2 uRes;\n"
    "flat out vec3 vCtr;\n"
    "flat out vec4 vCol;\n"
    "void main(){\n"
    "  vCtr = aCtr; vCol = aCol;\n"
    "  gl_Position = vec4(aPix.x/uRes.x*2.0 - 1.0, 1.0 - aPix.y/uRes.y*2.0, 0.0, 1.0);\n"
    "}\n";

static const char *FLY_SPLAT_FS =
    FLY_GLSL_HEADER
    "uniform vec2 uRes;\n"
    "uniform sampler2D uFrame;\n"
    "flat in vec3 vCtr;\n"
    "flat in vec4 vCol;\n"
    "out vec4 oColor;\n"
    "void main(){\n"
    "  int W = int(uRes.x), H = int(uRes.y);\n"
    "  int cx = int(vCtr.x), cy = int(vCtr.y);\n"
    "  if (cx < 0 || cy < 0 || cx >= W || cy >= H) discard;\n"
    /* the occlusion test is the centre texel's, as in light_splat: a lamp is a
       point source and the glare around it is its glow, not its silhouette */
    "  if (texelFetch(uFrame, ivec2(cx, H - 1 - cy), 0).a + 6.0 < vCtr.z) discard;\n"
    "  float dx = gl_FragCoord.x - vCtr.x, dy = (uRes.y - gl_FragCoord.y) - vCtr.y;\n"
    "  float d2 = dx*dx + dy*dy, r = vCol.w;\n"
    "  float R = r*" FLY_GLSL_GLARE_SPAN " + 1.5;\n"
    "  if (d2 > R*R) discard;\n"
    /* the twin of hdr_add_glare: two Cauchy-squared lobes, windowed at the cut */
    "  float rh = r*" FLY_GLSL_GLARE_WIDE ";\n"
    "  float tc = 1.0 + d2/(r*r), th = 1.0 + d2/(rh*rh);\n"
    "  float w = (" FLY_GLSL_GLARE_CORE "/(tc*tc) + " FLY_GLSL_GLARE_HALO "/(th*th))\n"
    "          * clamp(R - sqrt(d2), 0.0, 1.0);\n"
    /* alpha zero: the frame's alpha is its view depth and an additive pass has
       no business changing it (see fly_gpu_frame_overlay) */
    "  oColor = vec4(vCol.rgb*w, 0.0);\n"
    "}\n";

/* ---- the lamps, as a deferred pass over the finished frame ----
 *
 * Twin of lamp_flush, and the same arithmetic per fragment. The quad is the
 * lamp's screen circle and carries nothing but the lamp; the surface it lights
 * is read back out of the frame — its view depth is in the alpha, and the ray
 * through the fragment is the camera basis, so the world point the frame drew
 * there is one multiply-add away. That is the whole trick: a light that lands
 * on the frame's own geometry needs no geometry of its own. */
static const char *FLY_LAMP_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec2 aPix;\n"   /* corner, image pixels (top-down) */
    "layout(location = 1) in vec3 aPos;\n"   /* the lantern, in world */
    "layout(location = 2) in vec4 aAim;\n"   /* the way it throws (xy), reach, spare */
    "layout(location = 3) in vec3 aRad;\n"   /* linear radiance at the nadir */
    "uniform vec2 uRes;\n"
    "flat out vec3 vPos;\n"
    "flat out vec4 vAim;\n"
    "flat out vec3 vRad;\n"
    "void main(){\n"
    "  vPos = aPos; vAim = aAim; vRad = aRad;\n"
    "  gl_Position = vec4(aPix.x/uRes.x*2.0 - 1.0, 1.0 - aPix.y/uRes.y*2.0, 0.0, 1.0);\n"
    "}\n";

static const char *FLY_LAMP_FS =
    FLY_GLSL_HEADER
    FLY_GLSL_ENV_UNIFORMS
    "uniform sampler2D uFrame;\n"
    "flat in vec3 vPos;\n"
    "flat in vec4 vAim;\n"
    "flat in vec3 vRad;\n"
    "out vec4 oColor;\n"
    "void main(){\n"
    "  float zs = texelFetch(uFrame, ivec2(gl_FragCoord.xy), 0).a;\n"
    "  float reach = vAim.z;\n"
    "  float zl = dot(vPos - uCamPos, uCamFwd);\n"
    "  float dz = zs - zl;\n"
    /* the sky included: its depth is further off than any reach */
    "  if (dz > reach || dz < -reach) discard;\n"
    "  float ux = (gl_FragCoord.x - uRes.x*0.5)/uScale;\n"
    "  float uy = (gl_FragCoord.y - uRes.y*0.5)/uScale;\n"
    "  vec3 off = uCamRight*ux + uCamUp*uy;\n"
    "  vec3 s = uOrtho != 0 ? uCamPos + off + uCamFwd*zs\n"
    "                       : uCamPos + (uCamFwd + off)*zs;\n"
    "  vec3 dv = s - vPos;\n"
    "  float q2 = dot(dv, dv);\n"
    "  if (q2 >= reach*reach) discard;\n"
    "  float inv = inversesqrt(q2 + 1e-6);\n"
    "  float hh = -dv.z*inv;\n"
    "  if (hh <= 0.0) discard;\n"
    "  float a = (dv.x*vAim.x + dv.y*vAim.y)*inv;\n"
    "  float b = (dv.y*vAim.x - dv.x*vAim.y)*inv;\n"
    "  float lat = a*a + b*b*" FLY_GLSL_LAMP_ACROSS ";\n"
    "  float gain = (1.0 + " FLY_GLSL_LAMP_WING "*lat*lat)*hh/(q2 + " FLY_GLSL_LAMP_SOFT ");\n"
    "  float t = 1.0 - q2/(reach*reach);\n"
    /* alpha zero: the frame's alpha is its view depth and an additive pass has
       no business changing it (see fly_gpu_frame_overlay) */
    "  oColor = vec4(vRad*(gain*t*t), 0.0);\n"
    "}\n";

static const char *FLY_BLEND_VS =
    FLY_GLSL_HEADER
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec4 aCol;\n"   /* radiance, coverage */
    FLY_GLSL_ENV_UNIFORMS
    "out vec4 vCol;\n"
    "out float vDepth;\n"
    FLY_GLSL_PROJECT
    "void main(){\n"
    "  vCol = aCol;\n"
    "  vDepth = dot(aPos - uCamPos, uCamFwd);\n"
    "  gl_Position = fly_project(aPos);\n"
    "}\n";

static const char *FLY_BLEND_FS =
    FLY_GLSL_HEADER
    "uniform sampler2D uFrame;\n"
    "in vec4 vCol;\n"
    "in float vDepth;\n"
    "out vec4 oColor;\n"
    "void main(){\n"
    "  if (vDepth >= texelFetch(uFrame, ivec2(gl_FragCoord.xy), 0).a) discard;\n"
    "  oColor = vCol;\n"
    "}\n";

/* The lattice image the march writes and the composite reads. One per context,
 * grown with the frame, and never read back — unless something asked. */
static fly_gpu_img *g_shaft_img;
static int g_shaft_probe;

int fly_render_shaft_probe(int on) {
    int was = g_shaft_probe;
    g_shaft_probe = on != 0;
    return was;
}

/* The numbers fly_render_shaft_stats reports, taken off the marched lattice.
 * The software pass accumulates them as it goes, for nothing; this one has to
 * bring the lattice down to answer, which is why it is a hook and not the
 * default. It is a few hundred kilobytes — the lattice, not the frame. */
static void shaft_stats_from_img(int hw, int hh) {
    static float *px;
    static int cap;
    int n = hw * hh * 4, i;
    if (n > cap) {
        float *nb = (float *)realloc(px, (size_t)n * sizeof(float));
        if (!nb) return;
        px = nb;
        cap = n;
    }
    if (fly_gpu_img_read(g_shaft_img, px) != 0) return;
    for (i = 0; i < hw * hh; ++i) {
        double m = ((double)px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
        if (px[i * 4 + 3] < 0.5f) continue; /* the march skipped this point */
        ++g_shaft_n;
        g_shaft_sum += m;
        if (m < g_shaft_worst) g_shaft_worst = m;
    }
}

/* Returns 1 when a correction was marched into g_shaft_img, 0 when this frame
 * needs none, negative when GL refused. */
static int gpu_shafts(const fly_game *g, const fly__env *e, const fly__view *v, int w, int h) {
    fly_gpu_prog *p;
    int hw, hh, st, steps = e->lod.shafts;
    g_shaft_sum = g_shaft_worst = 0.0;
    g_shaft_n = 0;
    if (steps <= 0) return 0;
    if (!e->shadow || !e->shadow->active) return 0;
    if (e->sun.z < 0.02f) return 0;
    shaft_lattice(w, h, v->aa, &hw, &hh, &st);
    p = fly_gpu_program(FLY_SHAFT_FS);
    if (!p) return -1;
    if (!g_shaft_img) g_shaft_img = fly_gpu_img_create();
    if (!g_shaft_img) return -1;
    if (fly_gpu_img_set(g_shaft_img, hw, hh, NULL, 4) != 0) return -1;
    env_uniforms(p, g, e, v, w, h);
    env_shadow_uniforms(p, e);
    /* unit 3: 0..2 are the cascades and 4 is the upload scratch unit */
    if (fly_gpu_frame_bind(p, "uFrame", 3) != 0) return -1;
    fly_gpu_set3f(p, "uShaftGrid", (float)hw, (float)hh, (float)st);
    fly_gpu_set1i(p, "uShaftSteps", steps);
    fly_gpu_set1i(p, "uShaftSeed", (int)(e->seed ^ 0x5f3au));
    if (fly_gpu_img_draw(p, g_shaft_img) != 0) return -1;
    if (g_shaft_probe) shaft_stats_from_img(hw, hh);
    return 1;
}

/* Move the frame to its other target, adding the correction on the way. */
static int gpu_frame_correct(const fly__view *v, int w, int h, int shafts) {
    fly_gpu_prog *p = fly_gpu_program(FLY_FRAME_FS);
    int hw, hh, st;
    if (!p) return -1;
    shaft_lattice(w, h, v->aa, &hw, &hh, &st);
    if (fly_gpu_frame_swap() != 0) return -1;
    if (fly_gpu_frame_bind(p, "uFrame", 0) != 0) return -1;
    fly_gpu_set2f(p, "uSize", (float)w, (float)h);
    fly_gpu_set1i(p, "uShaftOn", shafts);
    fly_gpu_set3f(p, "uShaftGrid", (float)hw, (float)hh, (float)st);
    if (shafts && g_shaft_img && fly_gpu_img_bind(p, "uShaft", 1, g_shaft_img) != 0) return -1;
    return fly_gpu_frame_draw(p);
}

/* The emissive splats, the lamps and the translucent queue, laid over the frame
 * that is already there. All three read the frame's alpha for their depth test,
 * so all three write the target the correction pass just filled and sample the
 * one it read. Consumes g_nlights, g_nlamp and g_nblend, as their CPU twins do.
 *
 * In that order, and the order matters once: the first two are added and the
 * last is mixed, so the translucent pass has to go over a frame whose lighting
 * has already been finished. */
static int gpu_overlay(const fly_game *g, const fly__env *e, const fly__view *v, int w, int h) {
    static fly_gpu_mesh *splat_mesh, *lamp_mesh, *blend_mesh;
    static float *vbuf;
    static uint32_t *ibuf;
    static int vbuf_cap, ibuf_cap;
    const int splat_attr[3] = { 2, 3, 4 };
    const int lamp_attr[4] = { 2, 3, 4, 3 };
    const int blend_attr[2] = { 3, 4 };
    fly_gpu_prog *sp, *lp, *bp;
    int i, rc = 0;

    if (g_nlights <= 0 && g_nlamp <= 0 && g_nblend <= 0) return 0;
    sp = fly_gpu_program_mesh(FLY_SPLAT_VS, FLY_SPLAT_FS);
    lp = fly_gpu_program_mesh(FLY_LAMP_VS, FLY_LAMP_FS);
    bp = fly_gpu_program_mesh(FLY_BLEND_VS, FLY_BLEND_FS);
    if (!sp || !lp || !bp) return -1;
    if (!splat_mesh) splat_mesh = fly_gpu_mesh_create();
    if (!lamp_mesh) lamp_mesh = fly_gpu_mesh_create();
    if (!blend_mesh) blend_mesh = fly_gpu_mesh_create();
    if (!splat_mesh || !lamp_mesh || !blend_mesh) return -1;
    /* one scratch buffer for all three: the splats are 9 floats over 4
     * vertices, the lamps 12 over 4 and the translucent triangles 7 over 3, so
     * the lamps set the ceiling */
    {
        int want_v = g_nlights * 4 * 9;
        int want_i = g_nlights * 6;
        if (g_nlamp * 4 * 12 > want_v) want_v = g_nlamp * 4 * 12;
        if (g_nlamp * 6 > want_i) want_i = g_nlamp * 6;
        if (g_nblend * 3 * 7 > want_v) want_v = g_nblend * 3 * 7;
        if (g_nblend * 3 > want_i) want_i = g_nblend * 3;
        if (want_v > vbuf_cap) {
            float *nv = (float *)realloc(vbuf, (size_t)want_v * sizeof(float));
            if (!nv) return -1;
            vbuf = nv;
            vbuf_cap = want_v;
        }
        if (want_i > ibuf_cap) {
            uint32_t *ni = (uint32_t *)realloc(ibuf, (size_t)want_i * sizeof(uint32_t));
            if (!ni) return -1;
            ibuf = ni;
            ibuf_cap = want_i;
        }
    }

    if (g_nlights > 0) {
        int n = 0, ni = 0;
        for (i = 0; i < g_nlights; ++i) {
            const fly__lightsplat *L = &g_lights[i];
            float a = (float)(L->col >> 24) / 255.0f;
            fly_v3 lin = fly_v3scale(lin_from_u32(L->col), a);
            float r = fly_clampf(110.0f / L->z, 0.7f, 3.2f) * L->size;
            /* the quad has to cover the whole glare out to where it is cut
             * off — see hdr_add_glare */
            float R = r * FLY_GLARE_SPAN + 1.5f;
            int c;
            const float cx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
            const float cy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
            for (c = 0; c < 4; ++c) {
                float *o = &vbuf[(size_t)(n + c) * 9];
                o[0] = L->sx + cx[c] * R;
                o[1] = L->sy + cy[c] * R;
                o[2] = L->sx; o[3] = L->sy; o[4] = L->z;
                o[5] = lin.x; o[6] = lin.y; o[7] = lin.z; o[8] = r;
            }
            ibuf[ni++] = (uint32_t)n; ibuf[ni++] = (uint32_t)(n + 1);
            ibuf[ni++] = (uint32_t)(n + 2);
            ibuf[ni++] = (uint32_t)n; ibuf[ni++] = (uint32_t)(n + 2);
            ibuf[ni++] = (uint32_t)(n + 3);
            n += 4;
        }
        if (fly_gpu_mesh_upload(splat_mesh, vbuf, n, 9, splat_attr, 3, ibuf, ni) != 0) rc = -1;
        if (rc == 0) {
            fly_gpu_set2f(sp, "uRes", (float)w, (float)h);
            if (fly_gpu_frame_bind(sp, "uFrame", 0) != 0) rc = -1;
        }
        if (rc == 0 && fly_gpu_frame_overlay(FLY_GPU_BLEND_ADD) != 0) rc = -1;
        if (rc == 0 && fly_gpu_mesh_draw(sp, splat_mesh) != 0) rc = -1;
    }

    if (rc == 0 && g_nlamp > 0) {
        int n = 0, ni = 0;
        for (i = 0; i < g_nlamp; ++i) {
            const fly__lamp *L = &g_lamp[i];
            float rect[4];
            int c;
            const float cx[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
            const float cy[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            if (!lamp_rect(v, w, h, L, rect)) continue;
            for (c = 0; c < 4; ++c) {
                float *o = &vbuf[(size_t)(n + c) * 12];
                o[0] = rect[0] + cx[c] * (rect[2] - rect[0]);
                o[1] = rect[1] + cy[c] * (rect[3] - rect[1]);
                o[2] = L->p.x; o[3] = L->p.y; o[4] = L->p.z;
                o[5] = L->ax.x; o[6] = L->ax.y; o[7] = L->reach; o[8] = 0.0f;
                o[9] = L->rad.x; o[10] = L->rad.y; o[11] = L->rad.z;
            }
            ibuf[ni++] = (uint32_t)n; ibuf[ni++] = (uint32_t)(n + 1);
            ibuf[ni++] = (uint32_t)(n + 2);
            ibuf[ni++] = (uint32_t)n; ibuf[ni++] = (uint32_t)(n + 2);
            ibuf[ni++] = (uint32_t)(n + 3);
            n += 4;
        }
        if (n > 0) {
            if (fly_gpu_mesh_upload(lamp_mesh, vbuf, n, 12, lamp_attr, 4, ibuf, ni) != 0)
                rc = -1;
            if (rc == 0) {
                env_uniforms(lp, g, e, v, w, h);
                if (fly_gpu_frame_bind(lp, "uFrame", 0) != 0) rc = -1;
            }
            if (rc == 0 && fly_gpu_frame_overlay(FLY_GPU_BLEND_ADD) != 0) rc = -1;
            if (rc == 0 && fly_gpu_mesh_draw(lp, lamp_mesh) != 0) rc = -1;
        }
    }

    if (rc == 0 && g_nblend > 0) {
        int n = 0;
        for (i = 0; i < g_nblend; ++i) {
            const fly__blendtri *t = &g_blend[i];
            int k;
            for (k = 0; k < 3; ++k) {
                float *o = &vbuf[(size_t)(n + k) * 7];
                o[0] = t->p[k].x; o[1] = t->p[k].y; o[2] = t->p[k].z;
                o[3] = t->col.x; o[4] = t->col.y; o[5] = t->col.z;
                o[6] = t->alpha[k];
            }
            ibuf[n] = (uint32_t)n;
            ibuf[n + 1] = (uint32_t)(n + 1);
            ibuf[n + 2] = (uint32_t)(n + 2);
            n += 3;
        }
        if (fly_gpu_mesh_upload(blend_mesh, vbuf, n, 7, blend_attr, 2, ibuf, n) != 0) rc = -1;
        if (rc == 0) {
            env_uniforms(bp, g, e, v, w, h);
            if (fly_gpu_frame_bind(bp, "uFrame", 0) != 0) rc = -1;
        }
        if (rc == 0 && fly_gpu_frame_overlay(FLY_GPU_BLEND_OVER) != 0) rc = -1;
        if (rc == 0 && fly_gpu_mesh_draw(bp, blend_mesh) != 0) rc = -1;
    }
    fly_gpu_frame_overlay(FLY_GPU_BLEND_OFF);
    g_nlights = 0;
    g_nlamp = 0;
    g_nblend = 0;
    return rc;
}

/* Everything between the geometry and the post chain, without the frame
 * leaving GL memory. Leaves the finished frame published for the resolve. */
static int gpu_frame_finish(const fly_game *g, const fly__env *e, const fly__view *v,
                            int w, int h) {
    int shafts = gpu_shafts(g, e, v, w, h);
    if (shafts < 0) return -1;
    /* The correction pass runs whenever anything still has to be laid into the
     * frame, because it is also what moves the frame off the target those
     * stages need to read. With nothing left to do it is skipped and the
     * geometry pass's own target is what the resolve reads. */
    if (shafts || g_nlights > 0 || g_nlamp > 0 || g_nblend > 0) {
        if (gpu_frame_correct(v, w, h, shafts) != 0) return -1;
        if (gpu_overlay(g, e, v, w, h) != 0) return -1;
        if (fly_gpu_frame_swap() != 0) return -1;
    }
    return 0;
}

/* ---------------- HDR resolve: exposure, bloom, shafts, ACES ------------- */

/* ---- GPU post chain: bright pass, separable blur, composite ----
 *
 * Three programs over two quarter-resolution float images. The CPU resolve
 * below stays the reference — this is a port of it, and the sun-shaft gather,
 * the blocky bloom upsample and the dither are reproduced rather than
 * improved, so the two backends agree.
 *
 * Row order: the uploaded HDR image is top-down (fly_img order) while GL's
 * framebuffer origin is bottom-left, so anything indexing it flips explicitly.
 * The bloom chain is rendered *and* sampled, so it stays in GL order. */

#define FLY_POST_UNIFORMS \
    "uniform sampler2D uHdr, uBloom, uExp;\n" \
    "uniform vec2 uSize, uBSize;\n" \
    "uniform float uExposure;\n" \
    /* Where the exposure comes from. A frame that stayed on the GPU has no
     * buffer for the CPU to take a log-average of, so it is reduced there and
     * left in a 1x1 image the two post programs sample — which also spares the
     * frame a readback in the middle of itself just to learn one number. */ \
    "uniform int uExpOn;\n" \
    /* and which way up the source is: an uploaded fly_img runs top-down, a
     * resident GL frame bottom-up, and every index into it goes through here */ \
    "uniform int uHdrFlip;\n" \
    "float fly_exposure(){\n" \
    "  return uExpOn != 0 ? texelFetch(uExp, ivec2(0, 0), 0).r : uExposure;\n" \
    "}\n" \
    "vec3 fly_hdr_at(int px, int pyImg){\n" \
    "  int H = int(uSize.y);\n" \
    "  return texelFetch(uHdr, ivec2(px, uHdrFlip != 0 ? pyImg : H - 1 - pyImg), 0).rgb;\n" \
    "}\n"

/* Auto-exposure, reduced where the frame is.
 *
 * A port of the log-average in hdr_resolve, over the same every-third-pixel
 * lattice, in two steps: a tile pass that sums log-luminance into a small
 * image, and a 1x1 pass that finishes the average and applies the same clamp.
 * The tree sum is if anything more accurate than the scalar one it copies —
 * a million terms into one float loses more than 256 partial sums do. */
#define FLY_EXPOSE_TILES 16

static const char *FLY_EXPOSE_FS =
    FLY_GLSL_HEADER
    "uniform sampler2D uFrame;\n"
    "uniform vec2 uSize;\n"
    "uniform int uTiles;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  int bx = int(gl_FragCoord.x), by = int(gl_FragCoord.y);\n"
    "  int W = int(uSize.x), H = int(uSize.y);\n"
    "  int nx = (W + 2)/3, ny = (H + 2)/3;\n"
    "  int x0 = nx*bx/uTiles, x1 = nx*(bx + 1)/uTiles;\n"
    "  int y0 = ny*by/uTiles, y1 = ny*(by + 1)/uTiles;\n"
    "  float s = 0.0, n = 0.0;\n"
    "  for (int j = y0; j < y1; ++j)\n"
    "    for (int i = x0; i < x1; ++i) {\n"
    "      vec3 c = texelFetch(uFrame, ivec2(i*3, H - 1 - j*3), 0).rgb;\n"
    "      s += log(dot(c, vec3(0.2126, 0.7152, 0.0722)) + 1e-4);\n"
    "      n += 1.0;\n"
    "    }\n"
    "  o = vec4(s, n, 0.0, 1.0);\n"
    "}\n";

static const char *FLY_EXPOSE_AVG_FS =
    FLY_GLSL_HEADER
    "uniform sampler2D uTile;\n"
    "uniform int uTiles;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  float s = 0.0, n = 0.0;\n"
    "  for (int j = 0; j < uTiles; ++j)\n"
    "    for (int i = 0; i < uTiles; ++i) {\n"
    "      vec2 t = texelFetch(uTile, ivec2(i, j), 0).xy;\n"
    "      s += t.x; n += t.y;\n"
    "    }\n"
    "  float avg = exp(s/max(n, 1.0));\n"
    /* night stays night and noon stays bright — the twin of hdr_resolve */
    "  o = vec4(clamp(0.30/(avg + 1e-4), 0.30, 1.9), avg, 0.0, 1.0);\n"
    "}\n";

static const char *FLY_POST_BRIGHT_FS =
    FLY_GLSL_HEADER
    FLY_POST_UNIFORMS
    "out vec4 o;\n"
    "void main(){\n"
    "  int bx = int(gl_FragCoord.x), byImg = int(uBSize.y) - 1 - int(gl_FragCoord.y);\n"
    "  int W = int(uSize.x), H = int(uSize.y);\n"
    "  vec3 acc = vec3(0.0);\n"
    "  int cnt = 0;\n"
    "  for (int j = 0; j < 4; ++j)\n"
    "    for (int i = 0; i < 4; ++i) {\n"
    "      int px = bx*4 + i, py = byImg*4 + j;\n"
    "      if (px >= W || py >= H) continue;\n"
    "      vec3 c = fly_hdr_at(px, py)*fly_exposure();\n"
    "      float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    /* only true emitters and the sun bloom */
    "      if (l > 1.38) acc += c*((l - 1.38)/l);\n"
    "      ++cnt;\n"
    "    }\n"
    "  o = vec4(cnt > 0 ? acc/float(cnt) : vec3(0.0), 1.0);\n"
    "}\n";

/* One 9-tap pass per axis instead of two 5-tap box passes: convolving a
 * 5-box with itself gives exactly [1,2,3,4,5,4,3,2,1]/25, and separable blurs
 * commute, so H9.V9 reproduces the CPU's H5.V5.H5.V5 with half the passes.
 * (Only the clamped border differs, by a fraction of a bloom texel.) */
static const char *FLY_POST_BLUR_FS =
    FLY_GLSL_HEADER
    FLY_POST_UNIFORMS
    "uniform ivec2 uDir;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  ivec2 c = ivec2(gl_FragCoord.xy), hi = ivec2(uBSize) - ivec2(1);\n"
    "  vec3 sum = vec3(0.0);\n"
    "  for (int k = -4; k <= 4; ++k) {\n"
    "    float wgt = 5.0 - abs(float(k));\n"
    "    sum += texelFetch(uBloom, clamp(c + uDir*k, ivec2(0), hi), 0).rgb*wgt;\n"
    "  }\n"
    "  o = vec4(sum/25.0, 1.0);\n"
    "}\n";

static const char *FLY_POST_FS =
    FLY_GLSL_HEADER
    FLY_POST_UNIFORMS
    "uniform vec2 uSunPix;\n"  /* sun position in image pixels */
    "uniform vec2 uOutSize;\n" /* the *output* frame; uSize is what was drawn */
    "uniform int uAA;\n"       /* supersample factor between the two */
    "uniform float uShaft;\n"
    "uniform int uPost;\n"
    "uniform vec3 uGrade;\n"
    "uniform float uSat;\n"
    "out vec4 o;\n"
    FLY_GLSL_NOISE
    "float fly_aces1(float x){\n"
    "  return clamp((x*(2.51*x + 0.03))/(x*(2.43*x + 0.59) + 0.14), 0.0, 1.0);\n"
    "}\n"
    "vec3 fly_bloom_at(int px, int pyImg){\n"
    "  ivec2 hi = ivec2(uBSize) - ivec2(1);\n"
    "  int bx = clamp(px/4, 0, hi.x), by = clamp(pyImg/4, 0, hi.y);\n"
    "  return texelFetch(uBloom, ivec2(bx, hi.y - by), 0).rgb;\n"
    "}\n"
    /* ...and the same buffer read as the continuous field it stands for. The
       bloom is built at a quarter of the frame, so a nearest tap paints it on
       in four-by-four blocks — invisible on a sun that fills a quarter of the
       sky and impossible to miss around a lamp, where the halo is a dozen
       pixels across and came out as a staircase of squares around it. A bloom
       texel's centre is a pixel and a half into the four it covers, which is
       what the offset is; the tap is the twin of bloom_bilinear. */
    "vec3 fly_bloom_lin(int px, int pyImg){\n"
    "  ivec2 hi = ivec2(uBSize) - ivec2(1);\n"
    "  float u = (float(px) - 1.5)*0.25, w = (float(pyImg) - 1.5)*0.25;\n"
    "  int x0 = int(floor(u)), y0 = int(floor(w));\n"
    "  float tx = u - float(x0), ty = w - float(y0);\n"
    "  int x1 = clamp(x0 + 1, 0, hi.x), y1 = clamp(y0 + 1, 0, hi.y);\n"
    "  x0 = clamp(x0, 0, hi.x); y0 = clamp(y0, 0, hi.y);\n"
    "  vec3 a = mix(texelFetch(uBloom, ivec2(x0, hi.y - y0), 0).rgb,\n"
    "               texelFetch(uBloom, ivec2(x1, hi.y - y0), 0).rgb, tx);\n"
    "  vec3 b = mix(texelFetch(uBloom, ivec2(x0, hi.y - y1), 0).rgb,\n"
    "               texelFetch(uBloom, ivec2(x1, hi.y - y1), 0).rgb, tx);\n"
    "  return mix(a, b, ty);\n"
    "}\n"
    /* One supersample, resolved to its own 8-bit code, dither and all. The
     * box filter below averages these rather than the radiance behind them,
     * because that is what fly_img_downsample averages and because the dither
     * is the point: it carries sub-code precision across the f^2 samples,
     * which is what keeps a smooth gradient smooth at output size. */
    "vec3 fly_resolve_px(int fx, int y, float ex){\n"
    "  vec3 c = fly_hdr_at(fx, y)*ex;\n"
    "  if (uPost != 0) {\n"
    "    c += fly_bloom_lin(fx, y)*0.6;\n"
    /* crepuscular shafts: radial gather of the bright mask toward the sun */
    "    if (uShaft > 0.001) {\n"
    "      float sh = 0.0;\n"
    "      for (int si = 1; si <= 5; ++si) {\n"
    "        float t = float(si)/6.0;\n"
    "        vec3 g = fly_bloom_at(int(float(fx) + (uSunPix.x - float(fx))*t),\n"
    "                              int(float(y)  + (uSunPix.y - float(y))*t));\n"
    "        sh += (g.r*0.5 + g.g*0.35)*(1.0 - t*0.7);\n"
    "      }\n"
    "      sh *= uShaft*0.30;\n"
    "      c += vec3(sh, sh*0.9, sh*0.7);\n"
    "    }\n"
    "  }\n"
    /* filmic tonemap to display, gamma encode, vignette */
    /* biome grade: twin of grade_apply in fly_render.c */
    "  c *= uGrade;\n"
    "  { float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    c = vec3(l) + (c - vec3(l))*uSat; }\n"
    "  vec3 d = vec3(sqrt(fly_aces1(c.r)), sqrt(fly_aces1(c.g)), sqrt(fly_aces1(c.b)));\n"
    "  if (uPost != 0) {\n"
    "    vec2 ctr = uSize*0.5;\n"
    "    vec2 dd = vec2(float(fx), float(y)) - ctr;\n"
    "    d *= 1.0 - dot(dd, dd)/dot(ctr, ctr)*0.20;\n"
    "  }\n"
    /* triangular-PDF dither, matching the CPU resolve texel for texel */
    "  uint h1 = fly_hash2(0x1111u, fx, y), h2 = fly_hash2(0x2222u, fx, y),\n"
    "       h3 = fly_hash2(0x3333u, fx, y);\n"
    "  vec3 dit = vec3(float(h1 & 0xffffu) - float(h1 >> 16),\n"
    "                  float(h2 & 0xffffu) - float(h2 >> 16),\n"
    "                  float(h3 & 0xffffu) - float(h3 >> 16))/65535.0;\n"
    "  return clamp(floor(d*255.0 + dit + 0.5), vec3(0.0), vec3(255.0));\n"
    "}\n"
    "void main(){\n"
    "  int aa = uAA > 1 ? uAA : 1;\n"
    "  int ox = int(gl_FragCoord.x), oy = int(uOutSize.y) - 1 - int(gl_FragCoord.y);\n"
    "  float ex = fly_exposure();\n"
    "  if (aa == 1) { o = vec4(fly_resolve_px(ox, oy, ex)/255.0, 1.0); return; }\n"
    /* the box average fly_img_downsample would have taken, rounded the same
     * way — truncating it loses a systematic half-code per channel */
    "  vec3 sum = vec3(0.0);\n"
    "  for (int j = 0; j < aa; ++j)\n"
    "    for (int i = 0; i < aa; ++i)\n"
    "      sum += fly_resolve_px(ox*aa + i, oy*aa + j, ex);\n"
    "  float f2 = float(aa*aa);\n"
    "  o = vec4(floor((sum + floor(f2*0.5))/f2)/255.0, 1.0);\n"
    "}\n";

/* Where the sun lands on screen and how hard the post chain's radial gather
 * pulls toward it. Off-screen and below the horizon both come back as zero
 * strength, so a caller can use the numbers unconditionally. */
static void sun_on_screen(const fly__env *e, const fly__view *v, int w, int h, int post,
                          float *sx, float *sy, float *str) {
    float zc = fly_v3dot(e->sun, v->fwd);
    *sx = *sy = *str = 0.0f;
    if (!post || zc <= 0.05f || e->day <= 0.08f) return;
    *sx = (float)w * 0.5f + fly_v3dot(e->sun, v->right) / zc * v->sx;
    *sy = (float)h * 0.5f - fly_v3dot(e->sun, v->up) / zc * v->sy;
    *str = e->day * (1.0f - e->storm * 0.85f) * fly_clampf(zc, 0, 1) * 0.5f;
}

/* Bind the frame the post chain is to read, on unit 0, and say which way up it
 * is: the resident GL frame as it stands, or rt->hdr uploaded when the
 * software pipeline drew it. Returns negative on failure. */
static int post_bind_source(fly_gpu_prog *p, fly_gpu_img *hdr) {
    if (g_frame_resident) {
        fly_gpu_set1i(p, "uHdrFlip", 0);
        return fly_gpu_frame_bind(p, "uHdr", 0);
    }
    fly_gpu_set1i(p, "uHdrFlip", 1);
    return fly_gpu_img_bind(p, "uHdr", 0, hdr);
}

/* The exposure this frame is resolved at, left in a 1x1 image for the post
 * programs to sample. Only the resident path needs it — a frame the CPU holds
 * has already been averaged in hdr_resolve. */
static int gpu_exposure(fly_render_target *rt) {
    static fly_gpu_img *tile, *one;
    fly_gpu_prog *red = fly_gpu_program(FLY_EXPOSE_FS);
    fly_gpu_prog *avg = fly_gpu_program(FLY_EXPOSE_AVG_FS);
    const int T = FLY_EXPOSE_TILES;
    if (!red || !avg) return -1;
    if (!tile) { tile = fly_gpu_img_create(); one = fly_gpu_img_create(); }
    if (!tile || !one) return -1;
    if (fly_gpu_img_set(tile, T, T, NULL, 4) != 0) return -1;
    if (fly_gpu_img_set(one, 1, 1, NULL, 4) != 0) return -1;
    fly_gpu_set2f(red, "uSize", (float)rt->w, (float)rt->h);
    fly_gpu_set1i(red, "uTiles", T);
    if (fly_gpu_frame_bind(red, "uFrame", 0) != 0) return -1;
    if (fly_gpu_img_draw(red, tile) != 0) return -1;
    fly_gpu_set1i(avg, "uTiles", T);
    if (fly_gpu_img_bind(avg, "uTile", 0, tile) != 0) return -1;
    if (fly_gpu_img_draw(avg, one) != 0) return -1;
    g_exposure_img = one;
    return 0;
}

/* One tap of the bloom buffer, read as the continuous field it stands for.
 *
 * The bloom is built at a quarter of the frame, and the composite used to read
 * it with a nearest tap — which paints it on in four-by-four blocks. On a sun
 * bloom that fills a quarter of the sky nobody could see it; around a lamp,
 * where the halo is a dozen pixels across, it was a staircase of squares round
 * every light in the world, and it is exactly the sort of thing that stops a
 * light reading as light. A bloom texel's centre is a pixel and a half into the
 * four it covers, which is where the offset comes from. Twin of fly_bloom_lin.
 *
 * The shaft gather below still taps it nearest: that is a five-sample radial
 * smear of a blurred mask over hundreds of pixels, and it has nothing to gain. */
static void bloom_bilinear(const float *bloom, int bw, int bh, int px, int py,
                           float *out) {
    float u = ((float)px - 1.5f) * 0.25f, w = ((float)py - 1.5f) * 0.25f;
    int x0 = (int)floorf(u), y0 = (int)floorf(w), x1, y1, c;
    float tx = u - (float)x0, ty = w - (float)y0;
    x1 = x0 + 1 < bw ? (x0 + 1 < 0 ? 0 : x0 + 1) : bw - 1;
    y1 = y0 + 1 < bh ? (y0 + 1 < 0 ? 0 : y0 + 1) : bh - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 >= bw) x0 = bw - 1;
    if (y0 >= bh) y0 = bh - 1;
    for (c = 0; c < 3; ++c) {
        float a = fly_lerpf(bloom[(y0 * bw + x0) * 3 + c],
                            bloom[(y0 * bw + x1) * 3 + c], tx);
        float b = fly_lerpf(bloom[(y1 * bw + x0) * 3 + c],
                            bloom[(y1 * bw + x1) * 3 + c], tx);
        out[c] = fly_lerpf(a, b, ty);
    }
}

/* Returns 0 when `out` holds the resolved frame; non-zero means nothing was
 * written and the caller must run the CPU resolve. `out` may be smaller than
 * the render target by `aa`, in which case the box downsample happens in the
 * final program rather than as a pass over the picture afterwards. */
static int hdr_resolve_gpu(fly_render_target *rt, fly_img *out, float exposure,
                           float sun_sx, float sun_sy, float shaft, int post,
                           fly_v3 grade, float sat, int aa) {
    static fly_gpu_img *hdr, *bloom_a, *bloom_b;
    fly_gpu_prog *bright, *blur, *final;
    int bw = (rt->w + 3) / 4, bh = (rt->h + 3) / 4, pass;
    int expose_on;

    if (fly_gpu_init() != 0) return -1;
    bright = fly_gpu_program(FLY_POST_BRIGHT_FS);
    blur = fly_gpu_program(FLY_POST_BLUR_FS);
    final = fly_gpu_program(FLY_POST_FS);
    if (!bright || !blur || !final) return -1;
    if (!hdr) {
        hdr = fly_gpu_img_create();
        bloom_a = fly_gpu_img_create();
        bloom_b = fly_gpu_img_create();
    }
    if (!hdr || !bloom_a || !bloom_b) return -1;

    g_exposure_img = NULL;
    if (g_frame_resident && gpu_exposure(rt) != 0) return -1;
    expose_on = g_exposure_img != NULL;
    /* fly_v3 is three contiguous floats, so the HDR buffer uploads as-is */
    if (!g_frame_resident &&
        fly_gpu_img_set(hdr, rt->w, rt->h, (const float *)rt->hdr, 3) != 0) return -1;
    if (fly_gpu_img_set(bloom_a, bw, bh, NULL, 4) != 0) return -1;
    if (fly_gpu_img_set(bloom_b, bw, bh, NULL, 4) != 0) return -1;

    if (post) {
        fly_gpu_set2f(bright, "uSize", (float)rt->w, (float)rt->h);
        fly_gpu_set2f(bright, "uBSize", (float)bw, (float)bh);
        fly_gpu_set1f(bright, "uExposure", exposure);
        fly_gpu_set1i(bright, "uExpOn", expose_on);
        if (expose_on && fly_gpu_img_bind(bright, "uExp", 2, g_exposure_img) != 0) return -1;
        if (post_bind_source(bright, hdr) != 0) return -1;
        if (fly_gpu_img_draw(bright, bloom_a) != 0) return -1;
        fly_gpu_set2f(blur, "uBSize", (float)bw, (float)bh);
        for (pass = 0; pass < 2; ++pass) {
            fly_gpu_img *src = pass ? bloom_b : bloom_a;
            fly_gpu_img *dst = pass ? bloom_a : bloom_b;
            fly_gpu_set2i(blur, "uDir", pass ? 0 : 1, pass ? 1 : 0);
            if (fly_gpu_img_bind(blur, "uBloom", 1, src) != 0) return -1;
            if (fly_gpu_img_draw(blur, dst) != 0) return -1;
        }
        /* the vertical pass lands back on bloom_a */
    }

    fly_gpu_set2f(final, "uSize", (float)rt->w, (float)rt->h);
    fly_gpu_set2f(final, "uOutSize", (float)out->w, (float)out->h);
    fly_gpu_set1i(final, "uAA", aa);
    fly_gpu_set2f(final, "uBSize", (float)bw, (float)bh);
    fly_gpu_set1f(final, "uExposure", exposure);
    fly_gpu_set1i(final, "uExpOn", expose_on);
    fly_gpu_set2f(final, "uSunPix", sun_sx, sun_sy);
    fly_gpu_set1f(final, "uShaft", post ? shaft : 0.0f);
    fly_gpu_set1i(final, "uPost", post != 0);
    fly_gpu_set3f(final, "uGrade", grade.x, grade.y, grade.z);
    fly_gpu_set1f(final, "uSat", sat);
    if (expose_on && fly_gpu_img_bind(final, "uExp", 2, g_exposure_img) != 0) return -1;
    if (post_bind_source(final, hdr) != 0) return -1;
    if (fly_gpu_img_bind(final, "uBloom", 1, bloom_a) != 0) return -1;
    return fly_gpu_draw(final, out);
}

/* Resolve the frame to 8 bits. Returns 1 when `out` was written directly —
 * which is what the GPU chain does, box filter and all — and 0 when the
 * software resolve filled rt->img at render size and the caller still owes the
 * downsample. `out` is the frame the picture is wanted at; it is rt->img when
 * nothing is supersampled. */
static int hdr_resolve(fly_render_target *rt, fly_img *out, const fly__env *e,
                       const fly__view *v, int post, int studio) {
    int w = rt->w, h = rt->h;
    int aa = out && out->w > 0 ? rt->w / out->w : 1;
    int x, y;
    float exposure = 1.0f;

    if (!out) { out = &rt->img; aa = 1; }
    if (aa < 1) aa = 1;

    /* A resident frame has nowhere else to go: the radiance is in GL memory
     * and the software resolve cannot read it. It reduces its own exposure
     * there too — see gpu_exposure. If the chain fails, the frame comes down
     * and the software resolve finishes it rather than the picture being
     * quietly wrong. */
    if (g_frame_resident) {
        float ssx = 0, ssy = 0, str = 0;
        sun_on_screen(e, v, w, h, post, &ssx, &ssy, &str);
        if (hdr_resolve_gpu(rt, out, 1.0f, ssx, ssy, str, post, e->grade, e->sat, aa) == 0)
            return 1;
        if (frame_to_target(rt) != 0) return 1; /* nothing left to try */
        g_frame_resident = 0;
    }

    /* auto-exposure: log-average scene luminance, clamped so night stays
     * night and noon stays bright */
    {
        float logsum = 0.0f;
        int n = 0;
        for (y = 0; y < h; y += 3)
            for (x = 0; x < w; x += 3) {
                logsum += logf(v3_lum(rt->hdr[y * w + x]) + 1e-4f);
                ++n;
            }
        /* Studio plates are mostly flat backdrop, so auto-exposure keys off
         * that and slams to the ceiling, blowing out every lit surface. Pin it
         * instead so all four views of a plate are exposed identically. */
        exposure = studio ? 1.0f
                          : fly_clampf(0.30f / (expf(logsum / (float)(n > 0 ? n : 1)) + 1e-4f),
                                       0.30f, 1.9f);
    }

    {
        /* the sun's screen position and shaft strength, shared by both paths */
        float ssx = 0, ssy = 0, str = 0;
        sun_on_screen(e, v, w, h, post, &ssx, &ssy, &str);
        /* The post chain has a real fixed cost — an HDR upload, four
         * framebuffer switches and a readback — so below roughly a tenth of a
         * megapixel the scalar C resolve wins. Measured crossover on llvmpipe:
         * 320x180 is 2.6 ms faster on the CPU, 320x180 at ssaa2 is 15 ms
         * faster on the GPU. */
        if (!g_env_force_cpu && w * h >= 120000 &&
            hdr_resolve_gpu(rt, out, exposure, ssx, ssy, str, post, e->grade, e->sat, aa) == 0)
            return 1;
    }

    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    float *bloom = NULL;
    if (post) {
        bloom = (float *)calloc((size_t)bw * (size_t)bh * 3, sizeof(float));
        if (bloom) {
            /* bright pass in exposed linear space */
            for (y = 0; y < bh; ++y)
                for (x = 0; x < bw; ++x) {
                    fly_v3 acc = fly_v3zero();
                    int i, j, cnt = 0;
                    for (j = 0; j < 4; ++j)
                        for (i = 0; i < 4; ++i) {
                            int px = x * 4 + i, py = y * 4 + j;
                            if (px >= w || py >= h) continue;
                            fly_v3 c = fly_v3scale(rt->hdr[py * w + px], exposure);
                            float l = v3_lum(c);
                            if (l > 1.38f) /* only true emitters and the sun bloom */
                                acc = fly_v3add(acc, fly_v3scale(c, (l - 1.38f) / l));
                            ++cnt;
                        }
                    if (cnt) {
                        bloom[(y * bw + x) * 3 + 0] = acc.x / cnt;
                        bloom[(y * bw + x) * 3 + 1] = acc.y / cnt;
                        bloom[(y * bw + x) * 3 + 2] = acc.z / cnt;
                    }
                }
            /* two separable blur passes */
            float *tmp = (float *)malloc((size_t)bw * (size_t)bh * 3 * sizeof(float));
            if (tmp) {
                int pass;
                for (pass = 0; pass < 2; ++pass) {
                    for (y = 0; y < bh; ++y)
                        for (x = 0; x < bw; ++x) {
                            int c;
                            for (c = 0; c < 3; ++c) {
                                float sum = 0;
                                int k;
                                for (k = -2; k <= 2; ++k) {
                                    int xx = x + k;
                                    if (xx < 0) xx = 0;
                                    if (xx >= bw) xx = bw - 1;
                                    sum += bloom[(y * bw + xx) * 3 + c];
                                }
                                tmp[(y * bw + x) * 3 + c] = sum / 5.0f;
                            }
                        }
                    for (y = 0; y < bh; ++y)
                        for (x = 0; x < bw; ++x) {
                            int c;
                            for (c = 0; c < 3; ++c) {
                                float sum = 0;
                                int k;
                                for (k = -2; k <= 2; ++k) {
                                    int yy = y + k;
                                    if (yy < 0) yy = 0;
                                    if (yy >= bh) yy = bh - 1;
                                    sum += tmp[(yy * bw + x) * 3 + c];
                                }
                                bloom[(y * bw + x) * 3 + c] = sum / 5.0f;
                            }
                        }
                }
                free(tmp);
            }
        }
    }

    /* crepuscular sun shafts: radial gather of the bright mask */
    float sun_sx = 0, sun_sy = 0, shaft_str = 0;
    if (post && bloom) {
        float zc = fly_v3dot(e->sun, v->fwd);
        if (zc > 0.05f && e->day > 0.08f) {
            sun_sx = (float)w * 0.5f + fly_v3dot(e->sun, v->right) / zc * v->sx;
            sun_sy = (float)h * 0.5f - fly_v3dot(e->sun, v->up) / zc * v->sy;
            shaft_str = e->day * (1.0f - e->storm * 0.85f) * fly_clampf(zc, 0, 1) * 0.5f;
        }
    }

    float cx = w * 0.5f, cy = h * 0.5f;
    float inv_r2 = 1.0f / (cx * cx + cy * cy);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            fly_v3 c = fly_v3scale(rt->hdr[y * w + x], exposure);
            if (bloom) {
                float bl[3];
                bloom_bilinear(bloom, bw, bh, x, y, bl);
                c.x += bl[0] * 0.6f;
                c.y += bl[1] * 0.6f;
                c.z += bl[2] * 0.6f;
                if (shaft_str > 0.001f) {
                    float sh = 0;
                    int si;
                    for (si = 1; si <= 5; ++si) {
                        float t = (float)si / 6.0f;
                        int gx = (int)((float)x + (sun_sx - (float)x) * t) / 4;
                        int gy = (int)((float)y + (sun_sy - (float)y) * t) / 4;
                        if (gx < 0) gx = 0;
                        if (gy < 0) gy = 0;
                        if (gx >= bw) gx = bw - 1;
                        if (gy >= bh) gy = bh - 1;
                        sh += (bloom[(gy * bw + gx) * 3 + 0] * 0.5f +
                               bloom[(gy * bw + gx) * 3 + 1] * 0.35f) * (1.0f - t * 0.7f);
                    }
                    sh *= shaft_str * 0.30f;
                    c.x += sh; c.y += sh * 0.9f; c.z += sh * 0.7f;
                }
            }
            /* filmic tonemap to display, gamma encode, vignette */
            c = grade_apply(c, e->grade, e->sat);
            float r = sqrtf(aces1(c.x)), g2 = sqrtf(aces1(c.y)), b = sqrtf(aces1(c.z));
            if (post) {
                float dx = (float)x - cx, dy = (float)y - cy;
                float vig = 1.0f - (dx * dx + dy * dy) * inv_r2 * 0.20f;
                r *= vig; g2 *= vig; b *= vig;
            }
            /* Per-pixel triangular-PDF dither (±1 LSB, independent per channel):
             * decorrelates the 8-bit quantization so smooth sky / haze / fog and
             * soft shadow gradients stop banding. Deterministic per pixel, and
             * the SSAA box-downsample averages the noise away to a clean ramp. */
            uint32_t h1 = fly_hash2(0x1111u, x, y), h2 = fly_hash2(0x2222u, x, y),
                     h3 = fly_hash2(0x3333u, x, y);
            float dr = ((float)(h1 & 0xffffu) - (float)(h1 >> 16)) / 65535.0f;
            float dg = ((float)(h2 & 0xffffu) - (float)(h2 >> 16)) / 65535.0f;
            float db = ((float)(h3 & 0xffffu) - (float)(h3 >> 16)) / 65535.0f;
            int ri = (int)(r * 255.0f + dr + 0.5f), gi = (int)(g2 * 255.0f + dg + 0.5f),
                bi = (int)(b * 255.0f + db + 0.5f);
            ri = ri < 0 ? 0 : (ri > 255 ? 255 : ri);
            gi = gi < 0 ? 0 : (gi > 255 ? 255 : gi);
            bi = bi < 0 ? 0 : (bi > 255 ? 255 : bi);
            rt->img.px[y * w + x] = FLY_RGB(ri, gi, bi);
        }
    free(bloom);
    return 0;
}

/* ---------------- path tracer ---------------- */

static float trace_terrain(const fly_world *w, fly_v3 ro, fly_v3 rd, float tmax) {
    float t = 2.0f;
    float last_dh = ro.z - fly_world_ground(w, ro.x, ro.y);
    float last_t = 0.0f;
    while (t < tmax) {
        fly_v3 p = fly_v3add(ro, fly_v3scale(rd, t));
        float dh;
        /* Above the ceiling and still climbing: there is nothing left to hit.
         *
         * A miss costs more than a hit and there are a lot of them. A ray that
         * finds ground stops when it does — 41 steps on a horizon camera,
         * measured — while one that does not walks its whole 26 km reach at
         * 144 steps and answers "sky", and each of those steps is a 400 ns
         * evaluation of a five-octave planetary field. Two thirds of the march
         * work in a hybrid frame was sky rays proving the sky was empty.
         *
         * This is a termination, not a skip: it returns exactly what the march
         * below was going to return, so every frame is unchanged to the bit.
         * It cannot be turned into a *skip* forward to the ceiling crossing,
         * because the step size is a function of the clearance at each step
         * and the hit is interpolated between the last two — jumping ahead
         * would move the reported distance. See FLY_GROUND_CEILING. */
        if (rd.z >= 0.0f && p.z > FLY_GROUND_CEILING) return -1.0f;
        dh = p.z - fly_world_ground(w, p.x, p.y);
        if (dh < 0.0f) {
            float tt = last_t + (t - last_t) * (last_dh / (last_dh - dh + 1e-6f));
            return tt;
        }
        last_dh = dh;
        last_t = t;
        /* The step floor grows with distance. A ray skimming the ground stays a
         * few metres above it the whole way, so a fixed 3 m floor makes the
         * march take thousands of steps to cross a valley — and the GLSL twin,
         * which has to bound its loop, ran out and reported "no hit". That
         * painted sky where the mountain was: at night, on a snow slope, the
         * star field came through the terrain, which is what "the sky in place
         * of the snow" looks like. The floor only binds when dh is small, so a
         * ray still approaching the ground steps by dh*0.4 as before.
         *
         * The floor is held under the ceiling, and that one line is the bug
         * `render.night` is named for. The floor grows and the ceiling does
         * not, so past 9850 m the floor overtook it and this was calling
         * fly_clampf with lo > hi — which returns the *floor*. The effect was
         * precisely backwards: at 20 km a ray ten metres off the ground, the
         * one case that needs the finest step, strode 403 m, while the ceiling
         * that exists to stop exactly that stopped applying at all. One ridge
         * crest per frame got stepped over, the marcher reported no hit, and
         * the star field came through the mountain — the same symptom the
         * comment above describes, from the other end.
         *
         * It is worse than a wrong number in the GLSL twin, where clamp() with
         * a reversed range is undefined: `min(max(x,lo),hi)` gives 200 and
         * `max(min(x,hi),lo)` gives 403, so the two backends were entitled to
         * disagree about where the ground is. */
        {
            float ceil_step = 200.0f;
            float floor_step = 3.0f + t * 0.02f;
            if (floor_step > ceil_step) floor_step = ceil_step;
            t += fly_clampf(dh * 0.4f, floor_step, ceil_step);
        }
    }
    return -1.0f;
}

static fly_v3 cosine_dir(fly_v3 n, fly_rng *rng) {
    float u1 = fly_rng_f01(rng), u2 = fly_rng_f01(rng);
    float r = sqrtf(u1), phi = 2.0f * FLY_PI * u2;
    fly_v3 tang = fly_v3norm(fly_v3cross(n, fabsf(n.z) < 0.9f ? fly_v3mk(0, 0, 1) : fly_v3mk(1, 0, 0)));
    fly_v3 bit = fly_v3cross(n, tang);
    return fly_v3add(fly_v3add(fly_v3scale(tang, r * cosf(phi)), fly_v3scale(bit, r * sinf(phi))),
                     fly_v3scale(n, sqrtf(1.0f - u1)));
}

/* Jittered sun direction: soft area-light shadows.
 *
 * The disc is `FLY_SM_SUN_TAN`, the one the shadow map's penumbra is sized
 * from, so the tracer's soft edge and the rasterizer's are the same light
 * rather than two independent guesses that happen to look similar. */
static fly_v3 sun_jitter(const fly__env *e, fly_rng *rng) {
    fly_v3 s = e->sun;
    if (!rng) return s;
    fly_v3 tang = fly_v3norm(fly_v3cross(s, fly_v3mk(0, 0, 1)));
    fly_v3 bit = fly_v3cross(s, tang);
    float a = fly_rng_f01(rng) * 2.0f * FLY_PI, r = fly_rng_f01(rng) * FLY_SM_SUN_TAN;
    return fly_v3norm(fly_v3add(s, fly_v3add(fly_v3scale(tang, cosf(a) * r),
                                             fly_v3scale(bit, sinf(a) * r))));
}

/* `primary` marks a ray straight from the camera. It is deliberately not the
 * same thing as `depth_left`: the top-level call starts with two bounces left,
 * so keying "may I see stars?" off depth_left let the *first* bounce still
 * count as primary and put a star back into the indirect light. Two different
 * questions, two arguments. */
/* `hit_t` reports the terrain march this call made, for a caller that would
 * otherwise repeat it. NULL when nobody is asking — every recursive call
 * below. It is the terrain distance and not the shaded surface's: a ray that
 * came back off the water plane still reports the ground the march found (or
 * -1), which is what pt_environment's depth needs. */
static fly_v3 pt_shade(const fly_world *w, const fly__env *e, fly_v3 ro, fly_v3 rd,
                       fly_rng *rng, int depth_left, int primary, float *hit_t) {
    float t = trace_terrain(w, ro, rd, 26000.0f);
    if (hit_t) *hit_t = t;
    /* The water plane is intersected in its own right, not only when the
     * terrain march happens to land under it: a ray grazing out across a lake
     * can run past the march's reach and return "no hit", and keying water off
     * that made those rays render as sky — distant water broke into dashes
     * wherever neighbouring rays disagreed about finding ground. */
    {
        float tw = rd.z < -1e-4f ? (FLY_WATER_Z - ro.z) / rd.z : -1.0f;
        int li;
        /* A lake is the same intersection at its own level, and it needs its
         * own because it is above the sea's: a ray descending onto one crosses
         * the lake's plane first, and the disc and the ground under it are what
         * say whether that crossing is water. */
        for (li = 0; li < w->nlake; ++li) {
            const fly_lake *L = &w->lake[li];
            float tl = rd.z < -1e-4f ? (L->z - ro.z) / rd.z : -1.0f;
            float dx, dy;
            if (tl <= 0.0f || tl >= 26000.0f) continue;
            if (tw > 0.0f && tl > tw) continue;
            dx = ro.x + rd.x * tl - L->x;
            dy = ro.y + rd.y * tl - L->y;
            if (dx * dx + dy * dy > L->r * L->r) continue;
            {   float lx = ro.x + rd.x * tl, ly = ro.y + rd.y * tl;
                float lg = fly_world_ground(w, lx, ly);
                if (lg >= fly_world_water_at(w, lx, ly, lg)) continue; }
            tw = tl;
        }
        if (tw > 0.0f && tw < 26000.0f && (t < 0.0f || tw < t)) {
            fly_v3 wp = fly_v3add(ro, fly_v3scale(rd, tw));
            /* The reflection is traced rather than sampled off the sky dome —
             * that is what the tracer is for — but everything else about the
             * surface comes from the same function the rasterizers use, so a
             * coast is the same coast in all three modes. */
            float a1 = fly_noise2(e->seed + 51, wp.x / 46.0f + (float)e->time * 0.31f,
                                  wp.y / 39.0f - (float)e->time * 0.22f);
            fly_v3 n = fly_v3norm(fly_v3mk(a1 * 0.08f, a1 * 0.05f, 1.0f));
            fly_v3 refl = fly_v3sub(rd, fly_v3scale(n, 2.0f * fly_v3dot(rd, n)));
            fly_v3 rc = depth_left > 0 ? pt_shade(w, e, fly_v3add(wp, fly_v3mk(0, 0, 0.5f)),
                                                 fly_v3norm(refl), rng, 0, 0, NULL)
                                       : sky_ambient(e, wp, fly_v3norm(refl));
            float wdepth = wp.z - fly_world_ground(w, wp.x, wp.y);
            fly_v3 col = water_surface(e, ro, wp.x, wp.y, wp.z, wdepth, rc, cloud_shadow(e, wp));
            return apply_fog(e, col, ro, rd, tw);
        }
    }
    /* And the rivers, which are not planes and cannot be intersected as ones.
     *
     * A channel is a line in a valley: a ray that finds it has already found
     * the ground under it, because the bed of a river is terrain like anything
     * else. So the march answers first, and if what it landed on is under the
     * local surface the crossing is bisected out of the segment the ray has
     * already flown — fourteen halvings, which puts the surface inside a
     * centimetre over any segment this can be asked about, against the
     * hundred-odd steps the march itself spends getting there. */
    if (t > 0.0f) {
        fly_v3 hp = fly_v3add(ro, fly_v3scale(rd, t));
        float hg = fly_world_ground(w, hp.x, hp.y);
        if (fly_world_water_at(w, hp.x, hp.y, hg) > hg + 0.02f &&
            ro.z > fly_world_water(w, ro.x, ro.y)) {
            float lo = 0.0f, hi = t;
            fly_v3 wp;
            int it;
            for (it = 0; it < 14; ++it) {
                float mid = (lo + hi) * 0.5f;
                fly_v3 q = fly_v3add(ro, fly_v3scale(rd, mid));
                if (q.z > fly_world_water(w, q.x, q.y)) lo = mid; else hi = mid;
            }
            wp = fly_v3add(ro, fly_v3scale(rd, hi));
            wp.z = fly_world_water(w, wp.x, wp.y);
            {
                float a1 = fly_noise2(e->seed + 51, wp.x / 46.0f + (float)e->time * 0.31f,
                                      wp.y / 39.0f - (float)e->time * 0.22f);
                fly_v3 n = fly_v3norm(fly_v3mk(a1 * 0.08f, a1 * 0.05f, 1.0f));
                fly_v3 refl = fly_v3sub(rd, fly_v3scale(n, 2.0f * fly_v3dot(rd, n)));
                fly_v3 rc = depth_left > 0 ? pt_shade(w, e, fly_v3add(wp, fly_v3mk(0, 0, 0.5f)),
                                                     fly_v3norm(refl), rng, 0, 0, NULL)
                                           : sky_ambient(e, wp, fly_v3norm(refl));
                float wdepth = wp.z - fly_world_ground(w, wp.x, wp.y);
                fly_v3 col = water_surface(e, ro, wp.x, wp.y, wp.z, wdepth, rc,
                                           cloud_shadow(e, wp));
                return apply_fog(e, col, ro, rd, hi);
            }
        }
    }
    /* A primary ray sees the sky; a bounce or a reflection gets the ambient
     * one. A point sample of the star field used as a light source multiplies a
     * star into the albedo of whatever it lands on. */
    if (t < 0.0f) return primary ? sky_radiance(e, ro, rd) : sky_ambient(e, ro, rd);
    fly_v3 p = fly_v3add(ro, fly_v3scale(rd, t));

    /* The tracer shades per pixel, so it takes the same material and the same
     * near-field relief the raster path's fragment stage does. `gn` stays the
     * heightfield normal: it is what the slope, the shadow ray offset and the
     * cosine bounce are defined against. */
    fly_v3 gn = terrain_normal(w, p.x, p.y, 14.0f);
    float slope = 1.0f - gn.z;
    fly_v3 mat, n = gn;
    fly_v3 alb = terrain_surface(w, p.x, p.y, p.z, slope, e->pxscale / (t + 1e-3f), e->ball,
                                 e->wet, &mat);
    alb = fly_v3mul(alb, terrain_detail(w, p.x, p.y, mat, e->pxscale / t, &n));

    fly_v3 col = fly_v3scale(alb, 0.04f + 0.05f * e->day);
    /* direct sun with soft shadow + cloud shadow */
    fly_v3 sj = sun_jitter(e, rng);
    float ndl = fly_v3dot(gn, sj);
    if (ndl > 0.0f && sj.z > 0.0f) {
        fly_v3 sp = fly_v3add(p, fly_v3scale(gn, 3.0f));
        if (trace_terrain(w, sp, sj, 9000.0f) < 0.0f) {
            fly_v3 vdir = fly_v3scale(rd, -1.0f);
            fly_v3 lit = terrain_light(e, alb, mat, n, vdir, cloud_shadow(e, p), 1.0f);
            fly_v3 amb = terrain_light(e, alb, mat, n, vdir, 0.0f, 1.0f);
            col = fly_v3add(col, fly_v3sub(lit, amb));
        }
    }
    /* indirect bounces */
    if (depth_left > 0 && rng) {
        fly_v3 bd = cosine_dir(gn, rng);
        fly_v3 bounce = pt_shade(w, e, fly_v3add(p, fly_v3scale(gn, 3.0f)), bd, rng,
                                 depth_left - 1, 0, NULL);
        col = fly_v3add(col, fly_v3scale(fly_v3mul(alb, bounce), 0.6f));
    }
    return apply_fog(e, col, ro, rd, t);
}


/* ---------------- GPU path tracer ---------------- */

/* One fullscreen program: each pixel traces the terrain, shades it and folds
 * in sky, cloud shadow and fog exactly as pt_shade does on the CPU. Sample
 * jitter, the sun's area-light jitter and the cosine bounce all come from a
 * per-pixel hash so the frame stays deterministic. */
static const char *FLY_PT_FS =
    FLY_GLSL_HEADER
    /* The shared block, not a hand-written copy of most of it.
     *
     * This program used to declare its own list, and a list is a thing that
     * goes stale: `uWet` was added to the shading model, `FLY_GLSL_SHADE`
     * started reading it, and this program stopped compiling — silently,
     * because `pt_environment_gpu` returns -1 on a failed program and the CPU
     * tracer is the documented fallback. Every traced frame in the project has
     * been the fallback since. The block exists so the environment programs
     * cannot drift apart like this; the tracer is one of them. */
    FLY_GLSL_ENV_UNIFORMS
    "uniform int uSamples;\n"
    "out vec4 oColor;\n"
    FLY_GLSL_NOISE
    FLY_GLSL_WORLD
    FLY_GLSL_SHADE
    /* per-pixel stream of uniforms in [0,1) */
    "uint g_rng;\n"
    "float rnd(){ g_rng = g_rng*1664525u + 1013904223u; return float((g_rng >> 8) & 0xFFFFFFu)/16777216.0; }\n"
    "void main(){\n"
    "  vec2 frag = gl_FragCoord.xy;\n"
    "  g_rng = fly_hash2(uint(uSeed)*977u + 3u, int(frag.x), int(frag.y));\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int s = 0; s < uSamples; ++s) {\n"
    "    float jx = uSamples > 1 ? rnd() - 0.5 : 0.0;\n"
    "    float jy = uSamples > 1 ? rnd() - 0.5 : 0.0;\n"
    "    vec3 rd = normalize(uCamFwd\n"
    "        + uCamRight*((frag.x + jx - uRes.x*0.5)/uScale)\n"
    "        + uCamUp   *((frag.y + jy - uRes.y*0.5)/uScale));\n"
    /* jittered sun disc: soft area-light shadows, mirroring sun_jitter */
    "    vec3 tang = normalize(cross(uSun, vec3(0.0,0.0,1.0)));\n"
    "    vec3 bit = cross(uSun, tang);\n"
    "    float a = rnd()*6.28318530718, r = rnd()*0.028;\n"
    "    vec3 sj = normalize(uSun + tang*(cos(a)*r) + bit*(sin(a)*r));\n"
    /* cosine-ish bounce direction; fly_pt_shade flips it into the hemisphere */
    "    float ba = rnd()*6.28318530718, bz = sqrt(rnd());\n"
    "    float br = sqrt(max(1.0 - bz*bz, 0.0));\n"
    "    vec3 bd = normalize(vec3(cos(ba)*br, sin(ba)*br, bz));\n"
    "    acc += fly_pt_shade(uCamPos, rd, sj, bd);\n"
    "  }\n"
    /* float target: linear radiance survives un-clamped, and alpha carries the
     * primary hit's view depth so the CPU no longer has to re-march it */
    "  vec3 rd0 = normalize(uCamFwd\n"
    "      + uCamRight*((frag.x - uRes.x*0.5)/uScale)\n"
    "      + uCamUp   *((frag.y - uRes.y*0.5)/uScale));\n"
    "  float th = fly_trace_terrain(uCamPos, rd0, 26000.0);\n"
    "  float vdepth = th > 0.0 ? th*dot(rd0, uCamFwd) : 1e30;\n"
    "  oColor = vec4(acc/float(uSamples), vdepth);\n"
    "}\n";

/* Past this, a view depth means "nothing there".
 *
 * Not a comparison against the 1e30 the software rasterizer writes. Every GPU
 * pass carries view depth in an RGBA16F alpha, where 1e30 saturates to fp16's
 * 65504 — so a test for 1e29 answers "there is ground sixty-five kilometres
 * away" on every sky pixel of every frame a GPU drew, which is the difference
 * between the rule below working and doing nothing at all. No ray in this
 * renderer reaches past 26 km, so 40 is background whoever wrote it. */
#define FLY_DEPTH_FAR 40000.0f

/* Render the traced environment on the GPU. Returns 0 on success; any failure
 * leaves rt untouched so the caller can fall back to the CPU tracer. */
static int pt_environment_gpu(fly_render_target *rt, const fly_game *g, const fly__env *e,
                              const fly__view *v, int samples, float blend) {
    static float *buf;
    static int img_w, img_h;
    fly_gpu_prog *prog;
    int x, y;

    if (fly_gpu_init() != 0) return -1;
    prog = fly_gpu_program(FLY_PT_FS);
    if (!prog) return -1;
    if (img_w != rt->w || img_h != rt->h) {
        float *nb = (float *)realloc(buf, (size_t)rt->w * (size_t)rt->h * 4 * sizeof(float));
        if (!nb) return -1;
        buf = nb;
        img_w = rt->w;
        img_h = rt->h;
    }

    fly_gpu_set1i(prog, "uSeed", (int)g->world.seed);
    fly_gpu_set4f(prog, "uChartFrame", g->world.frame.ux, g->world.frame.uy,
                        g->world.frame.sa, g->world.frame.ca);
    /* the whole world, unculled: a path is free to march anywhere in it */
    env_ground_uniforms(prog, g, 0.0f, 0.0f, -1.0f, FLY_PAD_REACH);
    fly_gpu_set3f(prog, "uSun", e->sun.x, e->sun.y, e->sun.z);
    fly_gpu_set3f(prog, "uWind", e->wind.x, e->wind.y, e->wind.z);
    fly_gpu_set1f(prog, "uDay", e->day);
    fly_gpu_set1f(prog, "uDusk", e->dusk);
    fly_gpu_set1f(prog, "uStorm", e->storm);
    fly_gpu_set1f(prog, "uWet", e->wet);
    fly_gpu_set1f(prog, "uCloudMean", e->cloud_mean);
    fly_gpu_set1f(prog, "uCirrusMean", e->cirrus_mean);
    fly_gpu_set3f(prog, "uCirrusLit", e->cirrus_lit.x, e->cirrus_lit.y, e->cirrus_lit.z);
    fly_gpu_set2f(prog, "uCirrusDir", e->cirrus_dir.x, e->cirrus_dir.y);
    fly_gpu_set3f(prog, "uCloudLit", e->cloud_lit.x, e->cloud_lit.y, e->cloud_lit.z);
    fly_gpu_set3f(prog, "uCloudDark", e->cloud_dark.x, e->cloud_dark.y, e->cloud_dark.z);
    fly_gpu_set3f(prog, "uSunLight", e->sunlight.x, e->sunlight.y, e->sunlight.z);
    { float sh[27]; int k; for (k = 0; k < 9; ++k) {
          sh[k * 3 + 0] = e->sh[k].x; sh[k * 3 + 1] = e->sh[k].y;
          sh[k * 3 + 2] = e->sh[k].z; }
      fly_gpu_set3fv(prog, "uSH", sh, 9); }
    fly_gpu_set3f(prog, "uGround", e->ground_lit.x, e->ground_lit.y, e->ground_lit.z);
    fly_gpu_set1f(prog, "uTime", (float)e->time);
    fly_gpu_set3f(prog, "uCamPos", v->pos.x, v->pos.y, v->pos.z);
    fly_gpu_set3f(prog, "uCamFwd", v->fwd.x, v->fwd.y, v->fwd.z);
    fly_gpu_set3f(prog, "uCamRight", v->right.x, v->right.y, v->right.z);
    fly_gpu_set3f(prog, "uCamUp", v->up.x, v->up.y, v->up.z);
    fly_gpu_set2f(prog, "uRes", (float)rt->w, (float)rt->h);
    fly_gpu_set1f(prog, "uScale", v->sy);
    fly_gpu_set1f(prog, "uPxScale", v->sy / (float)(v->aa > 0 ? v->aa : 1));
    fly_gpu_set1i(prog, "uSamples", samples < 1 ? 1 : samples);
    if (fly_gpu_draw_hdr(prog, buf, rt->w, rt->h) != 0) return -1;

    /* rgb is linear radiance, a is the primary hit's view depth */
    for (y = 0; y < rt->h; ++y)
        for (x = 0; x < rt->w; ++x) {
            int idx = y * rt->w + x;
            const float *px = &buf[(size_t)idx * 4];
            fly_v3 c = fly_v3mk(px[0], px[1], px[2]);
            /* what the rasterizer made of this pixel, read before it goes */
            float raster_z = rt->depth[idx];
            float b = blend;
            rt->depth[idx] = px[3];
            /* Where the two halves disagree about what is *there*, the half
             * that found something wins — see pt_environment below, which is
             * where this rule is argued and where it used to live alone. That
             * was the whole of the bug: the reference tracer was fixed and the
             * GPU one, which is the tracer that runs on every machine with a
             * GL driver, went on averaging a star into a mountainside. */
            if (px[3] < FLY_DEPTH_FAR && raster_z > FLY_DEPTH_FAR) b = 1.0f;
            else if (px[3] > FLY_DEPTH_FAR && raster_z < FLY_DEPTH_FAR) b = 0.0f;
            rt->hdr[idx] = b >= 1.0f ? c
                         : b <= 0.0f ? rt->hdr[idx]
                                     : fly_v3lerp(rt->hdr[idx], c, b);
        }
    return 0;
}

/* One row of the traced environment: everything a worker thread does, and
 * everything it touches is indexed by its own y. */
static void pt_row(fly_render_target *rt, const fly_game *g, const fly__env *e,
                   const fly__view *v, int samples, float blend, int y) {
    int x, s;
    for (x = 0; x < rt->w; ++x) {
        fly_rng rng;
        fly_v3 acc = fly_v3zero();
        /* One primary march, reused rather than repeated. At one sample the ray
         * below and the rd0 the depth is taken from are the same ray to the bit
         * — the jitter is a literal zero — so marching it again was a second
         * full traversal of the height field for an answer already in hand, and
         * it was a quarter of the frame. */
        float primary_t = -1.0f, *want_t = samples == 1 ? &primary_t : NULL;
        /* Randomness seeded from the pixel rather than drawn off one stream
         * running the length of the frame. Two reasons that are the same
         * reason. The GLSL tracer has always seeded per fragment (fly_hash2, in
         * FLY_PT_FS — a fragment shader has nowhere to keep a stream), so this
         * is the two backends agreeing about how a sample gets chosen. And a
         * pixel whose randomness depends only on where it is can be rendered in
         * any order, which is what lets the rows be split across threads and
         * still come out identical on every run and at every thread count. */
        fly_rng_seed(&rng, fly_hash2(g->seed * 977u + 3u, x, y));
        for (s = 0; s < samples; ++s) {
            float jx = samples > 1 ? fly_rng_f01(&rng) - 0.5f : 0.0f;
            float jy = samples > 1 ? fly_rng_f01(&rng) - 0.5f : 0.0f;
            fly_v3 rd = fly_v3norm(fly_v3add(v->fwd,
                            fly_v3add(fly_v3scale(v->right, ((float)x + jx - rt->w * 0.5f) / v->sx),
                                      fly_v3scale(v->up, (rt->h * 0.5f - (float)y - jy) / v->sy))));
            acc = fly_v3add(acc, pt_shade(&g->world, e, v->pos, rd, &rng, 2, 1,
                                          s == 0 ? want_t : NULL));
        }
        acc = fly_v3scale(acc, 1.0f / (float)samples);
        int idx = y * rt->w + x;
        fly_v3 rd0 = fly_v3norm(fly_v3add(v->fwd,
                         fly_v3add(fly_v3scale(v->right, ((float)x - rt->w * 0.5f) / v->sx),
                                   fly_v3scale(v->up, (rt->h * 0.5f - (float)y) / v->sy))));
        /* Above one sample the rays carry jitter and none of them is rd0,
         * so the depth still needs its own march. */
        float t = want_t ? primary_t
                         : trace_terrain(&g->world, v->pos, rd0, 26000.0f);
        /* What the rasterizer made of this pixel, read before it is
         * overwritten. The two halves have to be compared, not merely
         * averaged — see below. */
        float raster_z = rt->depth[idx];
        float b = blend;
        rt->depth[idx] = t > 0.0f ? t * fly_v3dot(rd0, v->fwd) : 1e30f;
        /* --- where the two halves disagree about what is there ---------
         *
         * The blend exists to combine two renderings of *the same surface*:
         * both pipelines are linear radiance, so mixing them is meaningful
         * exactly as far as they are looking at the same thing. Where one
         * found geometry and the other found sky they are not, and an
         * average of ground and sky is neither.
         *
         * It happens over a real band of the frame rather than at an odd
         * pixel. The rastered terrain rings run out around twenty
         * kilometres and the march reaches twenty-six, so every hybrid
         * frame has a six-kilometre annulus in which the raster half is
         * sky and the traced half is ground — and 40% of that sky was
         * being mixed into the mountain.
         *
         * By day it is a slight wash toward the haze that nobody would
         * report. At night the sky at that range is not smooth: it has
         * stars in it, and a star is a sub-pixel point sample, so a single
         * one landing in the annulus came through at four tenths strength
         * as an isolated bright blue pixel on a dark slope. That is the
         * pixel `render.night` has failed on, and the aspect's name for it
         * — the sky showing through the ground — turns out to be the exact
         * and literal description. What it was *not* is a marcher that
         * stepped over a ridge: measured, the pixel is present in the pure
         * raster path and absent from the pure traced one, and the march
         * finds the same ground at 21.9 km for it as for its neighbours.
         *
         * So where they disagree, the half that found something wins.
         * There is nothing to average. */
        /* FLY_DEPTH_FAR rather than 1e29: in a mixed frame the raster
         * half may well have come off the GPU, and its sky reads 65504. */
        if (t > 0.0f && raster_z > FLY_DEPTH_FAR) b = 1.0f;        /* only the tracer sees it */
        else if (t <= 0.0f && raster_z < FLY_DEPTH_FAR) b = 0.0f;  /* only the rasterizer does */
        rt->hdr[idx] = b >= 1.0f ? acc
                     : b <= 0.0f ? rt->hdr[idx]
                                 : fly_v3lerp(rt->hdr[idx], acc, b);
    }
}

/* The rows, over as many cores as there are.
 *
 * Rows are handed out interleaved rather than in contiguous blocks because the
 * work is not evenly spread down the frame: the top of a flying shot is sky,
 * where every ray walks its whole reach and finds nothing, and the bottom is
 * ground, where they stop early — a three-to-one difference in march steps,
 * measured. Contiguous blocks would hand one thread the expensive half of that
 * and leave the rest waiting on it; every nth row gives them all the same
 * mixture, with no scheduler and nothing to synchronise.
 *
 * There is no synchronisation and none is needed: a row writes only its own
 * span of hdr and depth, and reads nothing another row writes. */
typedef struct {
    fly_render_target *rt;
    const fly_game *g;
    const fly__env *e;
    const fly__view *v;
    int samples, first, stride;
    float blend;
} pt_band;

static void pt_band_run(pt_band *b) {
    int y;
    for (y = b->first; y < b->rt->h; y += b->stride)
        pt_row(b->rt, b->g, b->e, b->v, b->samples, b->blend, y);
}

#ifdef FLY_THREADS
#ifdef _WIN32
static unsigned __stdcall pt_thread(void *p) { pt_band_run((pt_band *)p); return 0; }
#else
static void *pt_thread(void *p) { pt_band_run((pt_band *)p); return NULL; }
#endif

/* How many rows to run at once. Capped hard: the bands are stack-allocated and
 * a machine reporting something absurd should not take the renderer with it. */
#define FLY_PT_BANDS_MAX 32

static int pt_bands_wanted(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    return n > FLY_PT_BANDS_MAX ? FLY_PT_BANDS_MAX : n;
}
#endif /* FLY_THREADS */

static void pt_rows(fly_render_target *rt, const fly_game *g, const fly__env *e,
                    const fly__view *v, int samples, float blend) {
#ifdef FLY_THREADS
    int n = pt_bands_wanted();
    if (n > rt->h) n = rt->h;
    if (n > 1) {
        pt_band bands[FLY_PT_BANDS_MAX];
#ifdef _WIN32
        HANDLE th[FLY_PT_BANDS_MAX];
#else
        pthread_t th[FLY_PT_BANDS_MAX];
#endif
        int i, started = 0;
        for (i = 0; i < n; ++i) {
            bands[i].rt = rt; bands[i].g = g; bands[i].e = e; bands[i].v = v;
            bands[i].samples = samples; bands[i].blend = blend;
            bands[i].first = i; bands[i].stride = n;
        }
        /* The caller's thread takes band 0 rather than waiting on n others. */
        for (i = 1; i < n; ++i) {
#ifdef _WIN32
            th[i] = (HANDLE)_beginthreadex(NULL, 0, pt_thread, &bands[i], 0, NULL);
            if (!th[i]) break;
#else
            if (pthread_create(&th[i], NULL, pt_thread, &bands[i]) != 0) break;
#endif
            ++started;
        }
        /* A thread that would not start is not a failure: its band is simply
         * run here, so the frame comes out the same on a machine that has run
         * out of threads as on one that has not. */
        for (i = 1 + started; i < n; ++i) pt_band_run(&bands[i]);
        pt_band_run(&bands[0]);
        for (i = 1; i <= started; ++i) {
#ifdef _WIN32
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
#else
            pthread_join(th[i], NULL);
#endif
        }
        return;
    }
#endif /* FLY_THREADS */
    {
        pt_band one;
        one.rt = rt; one.g = g; one.e = e; one.v = v;
        one.samples = samples; one.blend = blend;
        one.first = 0; one.stride = 1;
        pt_band_run(&one);
    }
}

static void pt_environment(fly_render_target *rt, const fly_game *g, const fly__env *e,
                           const fly__view *v, int samples, float blend) {
    /* GPU first; the CPU tracer below stays the reference and the fallback.
     *
     * Two things this did not do. The force-to-CPU hook binds here as well, and
     * it did not: every other pipeline honours it — the rasterized environment,
     * the resident frame, the GPU resolve — while the tracer went to the GPU
     * regardless, so a test that asked for the reference frame was handed the
     * GPU one under that name.
     *
     * And when the tracer is the *whole* environment, which is what a blend of
     * one means, it says which pipeline drew it. `fly_render_env_backend` was
     * left holding the answer from whatever frame ran before, and without it a
     * caller cannot tell a GPU frame from a fallback — which matters because a
     * host can build some of these programs and refuse others. llvmpipe takes
     * every environment program and will not build `FLY_PT_FS`, so on a machine
     * with a software GL driver the gallery's path-traced pair was the same
     * frame twice, and `write_manifest`'s no-frame-twice rule failed on it. A
     * mixed frame keeps reporting its rasterized half, which is what drew the
     * sky, the terrain and the ground the trace is blended over. */
    int gpu = !g_env_force_cpu &&
              pt_environment_gpu(rt, g, e, v, samples, blend) == 0;
    if (blend >= 1.0f) g_env_gpu = gpu;
    if (gpu) return;
    pt_rows(rt, g, e, v, samples, blend);
}

static void draw_ground_items(fly_render_target *rt, const fly__view *v, const fly__env *e,
                              const fly_game *g) {
    int i;
    for (i = 0; i < FLY_GROUND_ITEM_MAX; ++i) if (g->ground_items[i].active) {
        const fly_ground_item *item = &g->ground_items[i];
        fly_v3 base = fly_v3add(item->pos, fly_v3mk(0, 0, -0.3f));
        if (!object_worth_drawing(v, rt, base, 1.6f)) continue;
        draw_box2(rt, v, e, base, 0.55f, 0.35f, 0.55f, item->yaw,
                  fly_v3mk(0.22f, 0.48f, 0.62f), fly_v3mk(0.55f, 0.82f, 0.92f), 0, 0);
        draw_prism(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, 0.55f)), 0.15f, 0.45f, 6,
                   fly_v3mk(0.9f, 0.55f, 0.12f), fly_v3mk(1.0f, 0.8f, 0.25f));
    }
}

/* Pack a linear colour and an alpha the way `lin_from_u32` will read it back:
   draw_light's colour argument is a display-space uint32 from an era of literal
   FLY_RGBA constants, and a fireball's colour is computed. */
static uint32_t u32_from_lin(fly_v3 lin, float a) {
    float r = sqrtf(fly_clampf(lin.x, 0.0f, 1.0f));
    float g = sqrtf(fly_clampf(lin.y, 0.0f, 1.0f));
    float b = sqrtf(fly_clampf(lin.z, 0.0f, 1.0f));
    return FLY_RGBA((int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f),
                    (int)(b * 255.0f + 0.5f),
                    (int)(fly_clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

/* A camera-facing disc of radiance: dense at the middle, gone at the rim.
 *
 * Both fire and smoke are volumes seen from outside, and neither has an
 * orientation of its own — a billboard is the honest primitive. Eight segments
 * is enough that the silhouette does not read as a polygon at the sizes these
 * reach, and cheap enough to spend a dozen of them on one wreck. */
static void puff(const fly__view *v, const fly__env *e, fly_v3 c, float r,
                 fly_v3 rad, float core, float rim) {
    fly_v3 R = fly_v3scale(v->right, r), U = fly_v3scale(v->up, r);
    int k;
    for (k = 0; k < 8; ++k) {
        float a0 = (float)k / 8.0f * 2.0f * (float)FLY_PI;
        float a1 = (float)(k + 1) / 8.0f * 2.0f * (float)FLY_PI;
        fly_v3 p0 = fly_v3add(c, fly_v3add(fly_v3scale(R, cosf(a0)), fly_v3scale(U, sinf(a0))));
        fly_v3 p1 = fly_v3add(c, fly_v3add(fly_v3scale(R, cosf(a1)), fly_v3scale(U, sinf(a1))));
        blend_radiance(v, e, p0, p1, c, rad, rim, rim, core);
    }
}

/* An airframe coming apart.
 *
 * Three things at three rates, because one flash reads as a muzzle and not as
 * an aeroplane exploding. A fireball that blooms in a fifth of a second and is
 * out inside one; sparks thrown clear on ballistic arcs that outlive it; and
 * smoke that goes on rising and spreading for five seconds after there is
 * nothing left burning. The salvage is not drawn here — those are real ground
 * items with real physics, thrown by the crash, and they land where they land.
 *
 * How many lobes each of the three is made of is the graphics level's to
 * choose, and the hash is indexed by the lobe number rather than reshuffled, so
 * raising the level thickens the same explosion instead of drawing a different
 * one. A plume is a volume approximated by a sum of billboards: more of them is
 * more of the volume, not a different shape.
 *
 * The fire is emissive and the smoke is lit from above, both queued as
 * radiance rather than as albedo. Lighting a puff off a normal aimed at the
 * camera — which is what the existing translucent path does, correctly, for a
 * propeller disc — gave a backlit plume 0.009 of linear grey: drawn, and
 * completely invisible. Everything is derived from `age` and the blast's own
 * hash, so there is no particle state to step and both backends agree because
 * neither is storing anything. */
/* --- ordnance in flight ----------------------------------------------------
 *
 * Small, fast and brief, so it is drawn as light and smoke rather than as
 * geometry: at two hundred metres a missile is a couple of pixels and a trail,
 * and the trail is the part that carries the information. Which is also the
 * gameplay requirement — a round you cannot see coming is one you cannot break
 * from, and the smoke is what tells you where it came from and how hard it is
 * turning.
 *
 * Everything is derived from the round's own `age` and `seed`, so there is no
 * particle state to step and both backends agree because neither is storing
 * anything — the arrangement `draw_blasts` already uses.
 *
 * Three characters, and they are meant to read apart at a glance:
 *
 *   a motor burning is a hard white-hot point with a dense trail behind it;
 *   a coasting round is a dull point and a thinning trail, which is what tells
 *     you it has spent its energy and can be out-turned;
 *   a flare is orange, falling, and far brighter than either — it has to be,
 *     because being the brightest thing in the sky is its entire function and
 *     the picture should say so.
 */
static void draw_ordnance(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          const fly_game *g) {
    int i, k;
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        const fly_ord *o = &g->ord[i];
        float burning;
        int trail;
        if (!o->active) continue;
        if (!object_worth_drawing(v, rt, o->pos, 40.0f)) continue;
        burning = o->burn > 0.0f ? 1.0f : 0.0f;

        if (o->decoy_heat > 0.0f) {
            /* A flare. Radiance well over one so the tonemap keeps it as fire
             * rather than as a pale disc, and it dims as it burns down — which
             * is the same curve the seeker is reading, so what the player sees
             * and what the missile sees are the same thing. */
            float life = fly_clampf(o->decoy_heat / 7.5f, 0.0f, 1.0f);
            fly_v3 hot = fly_v3scale(fly_v3mk(6.0f, 2.4f, 0.5f), 0.25f + 0.75f * life);
            puff(v, e, o->pos, 0.9f + 1.4f * (1.0f - life), hot, 0.95f, 0.0f);
            for (k = 1; k < 5; ++k) {
                float t = (float)k * 0.09f;
                fly_v3 at = fly_v3add(o->pos, fly_v3scale(o->vel, -t));
                at.z += 9.80665f * 0.5f * t * t;
                puff(v, e, at, 0.5f + 1.1f * (float)k * 0.25f,
                     fly_v3scale(fly_v3mk(0.30f, 0.26f, 0.24f), 1.0f),
                     fly_clampf(0.42f * life * (1.0f - (float)k * 0.2f), 0.0f, 1.0f), 0.0f);
            }
            continue;
        }
        if (o->decoy_radar > 0.0f) {
            /* Chaff: no fire in it at all, just a cloud that blooms and hangs.
             * It has to look nothing like a flare, because they answer
             * different threats and a player who confuses them spends the
             * wrong magazine. */
            float life = fly_clampf(o->decoy_radar / 6.0f, 0.0f, 1.0f);
            for (k = 0; k < e->lod.puffs; ++k) {
                uint32_t hh = fly_hash2(o->seed, k, 5);
                float ux = ((float)(hh & 255u) / 255.0f - 0.5f);
                float uy = ((float)((hh >> 8) & 255u) / 255.0f - 0.5f);
                float uz = ((float)((hh >> 16) & 255u) / 255.0f - 0.5f);
                float grow = 1.0f + o->age * 3.4f;
                fly_v3 at = fly_v3add(o->pos, fly_v3mk(ux * grow * 2.0f, uy * grow * 2.0f,
                                                       uz * grow * 1.4f));
                puff(v, e, at, 0.8f * grow,
                     lit_surface(e, fly_v3mk(0.72f, 0.74f, 0.78f), fly_v3mk(0, 0, 1),
                                 cloud_shadow(e, o->pos), 1.0f),
                     fly_clampf(0.55f * life, 0.0f, 1.0f), 0.0f);
            }
            continue;
        }

        /* A round with a warhead. The body is a point of light; a bomb has no
         * motor and gets none, which is exactly the distinction a player needs
         * from a mile away — one of these things is chasing somebody and one is
         * merely falling. */
        if (!fly_ord_is_ballistic(o->kind)) {
            fly_v3 hot = burning > 0.0f ? fly_v3mk(7.0f, 4.2f, 1.6f)
                                        : fly_v3mk(1.1f, 0.55f, 0.22f);
            puff(v, e, o->pos, burning > 0.0f ? 0.85f : 0.45f, hot, 0.92f, 0.0f);
        }
        draw_light(rt, v, o->pos, FLY_RGBA(255, 236, 210, 235), 0.30f);

        /* And the trail, which is the part that matters. Laid along where the
         * round has actually been rather than straight back along its velocity:
         * a seeker pulling six g leaves a curved trail, and that curve is the
         * clearest picture in the game of a missile running out of the energy
         * to keep turning. Approximated by integrating the round's own drag and
         * gravity backwards, which costs nothing and is right to within a
         * metre over the second and a half that is drawn. */
        trail = burning > 0.0f ? 14 : 9;
        {
            fly_v3 at = o->pos, vel = o->vel;
            float step = 0.10f;
            for (k = 0; k < trail; ++k) {
                float age = (float)k * step;
                float fade;
                if (age > o->age) break;
                at = fly_v3add(at, fly_v3scale(vel, -step));
                fade = 1.0f - (float)k / (float)trail;
                fade *= burning > 0.0f ? 0.62f : 0.30f;
                puff(v, e, at, 0.55f + 0.30f * (float)k,
                     lit_surface(e, fly_v3mk(0.58f, 0.58f, 0.60f), fly_v3mk(0, 0, 1),
                                 cloud_shadow(e, at), 1.0f),
                     fly_clampf(fade, 0.0f, 1.0f), 0.0f);
            }
        }
    }
}

static void draw_blasts(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        const fly_game *g) {
    int b;
    for (b = 0; b < FLY_BLAST_MAX; ++b) {
        const fly_blast *bl = &g->blasts[b];
        float t, sz, cloud;
        fly_v3 smoke_lit;
        int k;
        if (!bl->active) continue;
        t = bl->age;
        sz = bl->size;
        if (!object_worth_drawing(v, rt, bl->pos, sz * 6.0f)) continue;
        cloud = cloud_shadow(e, bl->pos);
        /* Smoke is lit from above, not from wherever the camera happens to be.
           Doing it once per blast also keeps a plume one colour throughout
           rather than flickering per puff. */
        /* Soot, not steam. A 0.5 albedo lit from above in daylight comes out a
           pale grey and reads as fog sitting on the field; what burns off an
           airframe is black. */
        smoke_lit = lit_surface(e, fly_v3mk(0.225f, 0.213f, 0.205f), fly_v3mk(0, 0, 1),
                                cloud, 1.0f);

        /* --- fireball: bright, brief, lumpy rather than a clean sphere --- */
        if (t < 1.15f) {
            float grow = 0.28f + 1.9f * t;   /* a tight core that blooms outward */
            float fade = fly_clampf(1.0f - t / 1.15f, 0.0f, 1.0f);
            float heat = fade * fade * fade * fade;   /* white-hot is brief */
            /* white-hot, through orange, to a dull red as it cools. Radiance
               well above 1 so it survives the tonemap as *fire* rather than as
               a pale disc — this is the one thing in the frame that is its own
               light source. */
            /* Saturated well past what the tonemap can hold, so the fire keeps
               its colour where a paler mix goes to white paper. */
            fly_v3 hot = fly_v3lerp(fly_v3mk(3.4f, 0.42f, 0.04f),
                                    fly_v3mk(9.0f, 3.4f, 0.65f), heat);
            for (k = 0; k < e->lod.puffs; ++k) {
                uint32_t h = fly_hash2(bl->seed, k, 11);
                float ux = ((float)(h & 255u) / 255.0f - 0.5f);
                float uy = ((float)((h >> 8) & 255u) / 255.0f - 0.5f);
                float uz = ((float)((h >> 16) & 255u) / 255.0f - 0.5f);
                fly_v3 at = fly_v3add(bl->pos,
                    fly_v3mk(ux * sz * grow * 1.1f, uy * sz * grow * 1.1f,
                             uz * sz * grow * 0.7f + t * sz * 0.8f));
                /* Fire is opaque. Fading the *opacity* on heat left the late
                   fireball at a third and you could read the tree line through
                   it; it thins on age instead, and burns out rather than
                   dissolving. */
                puff(v, e, at, sz * grow * (0.55f + 0.18f * ux), hot,
                     fly_clampf(0.50f + 0.47f * fade, 0.0f, 0.97f), 0.0f);
            }
            /* No splat halo. `draw_light` is a disc of fixed screen radius
               with a hard edge — exactly what a nav light wants, and exactly
               wrong over something already emitting its own geometry: per lobe
               it drew four white circles laid on the fire, and one big one drew
               a single blown-out coin. The fireball's radiance is well above
               one, so the post chain's bloom spreads it for free and spreads it
               in the shape of the fire rather than in the shape of a disc. */
        }

        /* --- sparks: they outlive the flash, which is what sells the scale --- */
        if (t < 2.4f) {
            float fade = fly_clampf(1.0f - t / 2.4f, 0.0f, 1.0f);
            uint32_t rgba = u32_from_lin(fly_v3mk(1.0f, 0.55f, 0.14f), fade * 0.95f);
            for (k = 0; k < e->lod.puffs * 2; ++k) {
                uint32_t h = fly_hash2(bl->seed ^ 0x5bf03635u, k, 3);
                float a = (float)(h & 1023u) / 1024.0f * 2.0f * (float)FLY_PI;
                float sp = 9.0f + (float)((h >> 10) & 63u) / 63.0f * 26.0f;
                float up = 7.0f + (float)((h >> 16) & 63u) / 63.0f * 18.0f;
                fly_v3 at = fly_v3add(bl->pos,
                    fly_v3mk(cosf(a) * sp * t, sinf(a) * sp * t,
                             up * t - 0.5f * 9.80665f * t * t));
                if (at.z < fly_world_ground(&g->world, at.x, at.y)) continue;
                draw_light(rt, v, at, rgba, 0.34f);
            }
        }

        /* --- smoke: rises, spreads, thins, and is the last thing left --- */
        {
            float fade = fly_clampf(1.0f - t / FLY_BLAST_LIFE, 0.0f, 1.0f);
            float bloom = fly_clampf(t / 0.35f, 0.0f, 1.0f);  /* fire first, then smoke */
            /* Smoke reads by *occluding*, not by tinting: a dark puff at half
               opacity over bright grass is a smudge nobody notices. Opaque in
               the middle, soft at the rim, and thinning on a curve rather than
               linearly so the plume is still there at three seconds. */
            float alpha = powf(fade, 0.7f) * bloom * 0.88f;
            /* soot darkens the plume as it cools and thickens */
            fly_v3 rad = fly_v3scale(smoke_lit,
                                     fly_clampf(1.0f - t * 0.13f, 0.35f, 1.0f));
            if (alpha > 0.01f) {
                for (k = 0; k < e->lod.puffs + 2; ++k) {
                    uint32_t h = fly_hash2(bl->seed ^ 0x27d4eb2fu, k, 7);
                    float ux = ((float)(h & 255u) / 255.0f - 0.5f);
                    float uy = ((float)((h >> 8) & 255u) / 255.0f - 0.5f);
                    float lag = (float)((h >> 16) & 255u) / 255.0f * 0.55f;
                    float tt = t - lag, r;
                    fly_v3 c;
                    if (tt <= 0.0f) continue;
                    r = sz * (0.42f + tt * 0.52f);
                    c = fly_v3add(bl->pos,
                        fly_v3mk(ux * sz * 1.1f + e->wind.x * tt * 0.55f,
                                 uy * sz * 1.1f + e->wind.y * tt * 0.55f,
                                 sz * 0.45f + tt * (2.9f + ux * 0.9f)));
                    puff(v, e, c, r, rad, alpha * (1.0f - lag * 0.5f), 0.0f);
                }
            }
        }
    }
}

/* Points in the drawn line, as against the surveyed one. The survey's stations
 * are FLY_RAIL_POINT_MAX; the extra room is for the arcs between the ones near
 * enough to be drawn as arcs, and only a couple of kilometres' worth of the
 * route is ever that close at once. */
#define FLY_RAIL_DRAW_MAX 1024

/* --- lighting the guideway -------------------------------------------------
 *
 * The same decision the roads got, for the same reason and drawn the same way:
 * a line the player is meant to be able to follow across the country has to be
 * followable after dark, and the rail had nothing at all. It is also the line
 * the player *rides*, which is the stronger half of the argument — the ride
 * camera looks straight down the guideway for the whole journey, and at night
 * that was a black pipe against black ground with the horizon somewhere.
 *
 * A mast on the flank of the pipe rather than a column beside it: the line is
 * five to thirteen metres up on piers, and a lamp standing on the ground under
 * a viaduct lights the field the viaduct crosses. So the lighting is part of
 * the structure, cranked in over the crown from alternating sides the way the
 * road's arms reach over the carriageway — and high enough over the crown that
 * the pod passes under it, which is what sets the mast height rather than
 * anything about how far the light throws.
 *
 * Ranges as the road's: the whole mast close in, the bare stalk beyond that,
 * and past the point where there is no structure worth drawing, the light
 * itself, running off down the line to wherever the route goes. */
#define FLY_RAIL_LAMP_PITCH 68.0f
#define FLY_RAIL_LAMP_NEAR 2200.0f
#define FLY_RAIL_LAMP_FAR 26000.0f
/* Where the lamp goes, and both numbers are set by the railcar rather than by
 * anything about lighting: the vehicle is a metre and a quarter above the crown
 * and a metre either side of the axis, so the mast stands outboard of that and
 * the head hangs over it. A mast on the shoulder of the barrel — which is where
 * the first cut of this put it — is a mast the railcar drives straight through,
 * about once every fifteen seconds of a journey.
 *
 * Which is why the mast is bracketed off the *underside* of the barrel and not
 * off its side: below the pod's saddle there is nothing in the way, and a
 * cantilever from under the deck is what a viaduct's lighting is carried on
 * anyway. In multiples of the pipe's own radius, so a wider pipe carries its
 * lighting further out instead of inside itself. */
#define FLY_RAIL_LAMP_LIFT (2.30f)          /* head, above the crown */
#define FLY_RAIL_LAMP_OUT (1.40f)           /* mast, in barrel radii off the axis */
#define FLY_RAIL_LAMP_DROP (2.00f)          /* bracket, in barrel radii under the crown */

static void rail_lamp(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 crown, float yaw, float side, float pipe_r, float dist) {
    fly_v3 steel = fly_v3mk(0.44f, 0.45f, 0.47f);
    float cy = cosf(yaw), sy = sinf(yaw);
    float out = pipe_r * FLY_RAIL_LAMP_OUT, drop = pipe_r * FLY_RAIL_LAMP_DROP;
    fly_v3 head = yawp(crown, 0.0f, side * 0.18f, FLY_RAIL_LAMP_LIFT, cy, sy);
    /* White, where the roads are sodium, and its own dusk like theirs — see
       FLY_LAMP_WHITE and lamp_dark. The guideway is the newest thing in the
       world and is lit like it. */
    float on = fly_clampf(lamp_dark(e, crown), 0.0f, 1.0f);
    if (dist < FLY_RAIL_LAMP_NEAR) {
        /* the mast, standing on a bracket that reaches out from under the
           barrel and buried in it at the inboard end */
        draw_box2(rt, v, e, yawp(crown, 0.0f, side * out, -drop, cy, sy),
                  0.14f, 0.14f, drop + FLY_RAIL_LAMP_LIFT, yaw, steel,
                  fly_v3scale(steel, 1.12f), 0, 0);
        if (dist < 420.0f) {
            draw_box2(rt, v, e, yawp(crown, 0.0f, side * out * 0.5f, -drop, cy, sy),
                      0.11f, out * 0.5f + 0.14f, 0.22f, yaw, steel,
                      fly_v3scale(steel, 1.1f), 0, 0);
            /* the arm over the crown, and the head on the end of it */
            draw_box2(rt, v, e, yawp(crown, 0.0f, side * (out * 0.5f + 0.09f),
                                     FLY_RAIL_LAMP_LIFT - 0.14f, cy, sy),
                      0.10f, out * 0.5f + 0.09f, 0.13f, yaw, steel,
                      fly_v3scale(steel, 1.1f), 0, 0);
            draw_box2(rt, v, e, yawp(crown, 0.0f, side * 0.18f,
                                     FLY_RAIL_LAMP_LIFT - 0.05f, cy, sy),
                      0.32f, 0.26f, 0.20f, yaw, steel,
                      on > 0.02f ? fly_v3mk(1.42f, 1.46f, 1.60f)
                                 : fly_v3mk(0.72f, 0.72f, 0.68f), 0, 0);
        }
    }
    if (on <= 0.02f) return;
    /* The crown it is there for, and the field underneath.
     *
     * The second is the one that reads from outside — a guideway crossing dark
     * country at night is a line of light with a soft band of ground under it —
     * and it is what the line looked like it had switched off from anywhere but
     * the rider's seat. It used to need the ground's own height and grade taken
     * either side of the spill, because a flat ellipse laid on a hillside puts
     * its far half inside the hill. A lamp does not have to be told where the
     * ground is any more: the light is thrown from the lantern and falls on
     * whatever the frame drew, so a viaduct over a slope lights the slope and
     * one over a river lights the water.
     *
     * Quoted at the drop it actually hangs at, which on a viaduct is the fall to
     * the field and not a fixed mounting: the power is the brightness directly
     * under the lantern, so a taller pier spreads the same brightness over a
     * reach that grows with it rather than dimming to nothing. Below a road
     * column's, because it is spill and not a pool. */
    if (dist < 2400.0f) {
        float gz = drawn_ground(e->world, crown.x, crown.y);
        float fall = crown.z - pipe_r - gz;
        if (fall > 1.0f && fall < 30.0f)
            lamp_light(rt, v, e, head, yaw, fall, 14.0f + fall * 1.7f,
                       FLY_LAMP_WHITE, 0.10f * on);
    }
    draw_light(rt, v, head, FLY_RGBA(226, 236, 255, (int)(205.0f * on)),
               dist < FLY_RAIL_LAMP_NEAR ? 1.05f : 0.80f);
}

/* The lamps whose chainage falls inside span `i`, placed by distance along the
 * route the way the road's are and for the same reason: the pitch is 68 m of
 * line wherever the survey put its stations, so a mast never crawls along the
 * pipe as the camera moves. Seated on the drawn curve rather than on the chord
 * across it, which is the same rule the piers follow — a mast on the chord
 * meets the barrel a hand's breadth inside it. */
/* Where the k-th lamp on the guideway hangs: the crown of the barrel at that
 * chainage, which side the mast is on, and which way the line is heading there.
 * The road's rule exactly (see road_lamp_at) on the rail's own pitch, and one
 * function for the same reason — the drawing and the probe must agree about
 * where a lamp is or the test is checking its own arithmetic. */
static int rail_lamp_at(const fly_rail_route *r, int k, float pipe_r,
                        fly_v3 *crown, float *yaw, float *side, float *chain_out) {
    float chain = FLY_RAIL_LAMP_PITCH * 0.5f + (float)k * FLY_RAIL_LAMP_PITCH;
    int n = r->point_count, lo = 0, hi, i;
    if (k < 0 || n < 2 || chain > r->length) return 0;
    if (n > FLY_RAIL_POINT_MAX) n = FLY_RAIL_POINT_MAX;
    hi = n - 2;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (r->points[mid].distance <= chain) lo = mid; else hi = mid - 1;
    }
    i = lo;
    {   float d0 = r->points[i].distance, d1 = r->points[i + 1].distance;
        fly_v3 c, d;
        if (d1 - d0 < 1e-3f) return 0;
        /* Seated on the drawn curve rather than on the chord across it, which
           is the same rule the piers follow — a mast on the chord meets the
           barrel a hand's breadth inside it. Through fly_line, which is where
           the curve a laid line takes is decided, so the mast, the pier, the
           barrel and the railcar are all on one answer. */
        if (!fly_line_span(r->points, n, FLY_LINE_CHORD, i,
                           (chain - d0) / (d1 - d0), &c, NULL)) return 0;
        d = fly_v3sub(r->points[i + 1].pos, r->points[i].pos);
        c.z += pipe_r;
        if (crown) *crown = c;
        if (yaw) *yaw = atan2f(d.y, d.x); }
    if (side) *side = (k & 1) ? 1.0f : -1.0f;
    if (chain_out) *chain_out = chain;
    return 1;
}

static void rail_lamps(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_rail_route *r, int i, int n, float pipe_r,
                       float dist, float dark) {
    float d0 = r->points[i].distance, d1 = r->points[i + 1].distance;
    int k, k0, k1, stride = lamp_stride(dist);
    (void)n;
    /* No occluders. A sixteen-centimetre mast throws a shadow nobody can pick
       out from the pipe's own, and there are several hundred of them in a
       cascade — which is the same trade the road's lighting makes by sitting
       inside a `draw_roads` that leaves the shadow pass at the door. */
    if (g_shadow_cast) return;
    if (d1 - d0 < 1e-3f) return;
    k0 = (int)ceilf((d0 - FLY_RAIL_LAMP_PITCH * 0.5f) / FLY_RAIL_LAMP_PITCH);
    k1 = (int)ceilf((d1 - FLY_RAIL_LAMP_PITCH * 0.5f) / FLY_RAIL_LAMP_PITCH) - 1;
    if (k0 < 0) k0 = 0;
    for (k = k0; k <= k1; ++k) {
        fly_v3 c;
        float yaw, side, ld;
        if (stride > 1 && (k % stride)) continue;
        if (!rail_lamp_at(r, k, pipe_r, &c, &yaw, &side, NULL)) continue;
        ld = fly_v3dist(c, v->pos);
        if (ld > (dark > 0.02f ? FLY_RAIL_LAMP_FAR : FLY_RAIL_LAMP_NEAR)) continue;
        if (!object_worth_drawing(v, rt, c, 6.0f)) continue;
        rail_lamp(rt, v, e, c, yaw, side, pipe_r, ld);
    }
}

/* --- the railcar -----------------------------------------------------------
 *
 * What the player rides, and for the length of a journey the only thing in
 * frame that is not landscape — the ride camera sits seven metres behind it and
 * four above, so the pod fills the lower third of every frame of it. It was one
 * box: a 4.4 m brick in two flat colours, with no front, no back, nothing to
 * say which way it was going and nothing to say anybody was inside it.
 *
 * So it is a vehicle now, built the way `prop_truck` is — a handful of boxes
 * that make a silhouette, with the trim priced off range so the ones that
 * resolve to nothing are not drawn at all. What it has to say, in order of how
 * far away it still says it: which end is the front (a stepped nose and the
 * lights on it, against a blunter tail); that it rides the pipe rather than
 * floating over it (the
 * saddle down both flanks and the shoes gripping the barrel); that it carries
 * people (a window band right round, lit from inside after dark); and that it
 * is a made thing (roof cap, rubbing strake, grab rails).
 *
 * Everything is measured from the pipe's own axis, so the whole vehicle moves
 * with the barrel radius rather than agreeing with it by coincidence. */
static void draw_railcar(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         fly_v3 axis, float yaw, float pipe_r, float dark) {
    fly_v3 hull = fly_v3mk(0.50f, 0.18f, 0.10f);
    fly_v3 roof = fly_v3mk(0.85f, 0.42f, 0.16f);
    fly_v3 steel = fly_v3mk(0.30f, 0.30f, 0.32f);
    fly_v3 shade = fly_v3mk(0.16f, 0.16f, 0.17f);
    fly_v3 glass = fly_v3mk(0.07f, 0.10f, 0.13f);
    float cy = cosf(yaw), sy = sinf(yaw);
    float floor_z = pipe_r * 0.86f;   /* the underside of the body, on the crown */
    float dist = fly_v3dist(axis, v->pos);
    int fine = dist < 140.0f, mid = dist < 460.0f;
    int k;
    /* The saddle: two plates down the flanks of the barrel, and the shoes that
       run on it. This is the part that makes the thing a railcar rather than a
       crate somebody left on a pipe. */
    for (k = 0; k < 2; ++k) {
        float side = k ? 1.0f : -1.0f;
        draw_box2(rt, v, e, yawp(axis, -0.15f, side * pipe_r * 0.80f, -pipe_r * 0.55f,
                                 cy, sy), 1.95f, 0.09f, floor_z + pipe_r * 0.55f, yaw,
                  steel, fly_v3scale(steel, 1.25f), 0, 0);
        if (mid) {
            int f;
            /* The shoes that run on the barrel, outboard of the plates so they
               read as running gear rather than as more of the same slab. */
            for (f = 0; f < 2; ++f)
                draw_box2(rt, v, e, yawp(axis, f ? 1.62f : -1.92f, side * pipe_r * 0.86f,
                                         -pipe_r * 0.66f, cy, sy),
                          0.46f, 0.15f, 0.54f, yaw, shade, fly_v3scale(steel, 1.3f), 0, 0);
        }
    }
    /* The body, and a cowl on each end of it. They are kept low — a full-height
       block on the back is what the first cut of this had, and from the ride
       camera, which looks at the back of this thing for the whole journey, it
       turned the rear into three flat bands with nothing to read. Low enough
       and the end face above it is free for a screen. */
    draw_box2(rt, v, e, fly_v3add(axis, fly_v3mk(0, 0, floor_z)), 2.20f, 1.00f, 1.16f,
              yaw, hull, roof, 0, 0);
    draw_box2(rt, v, e, yawp(axis, 2.38f, 0.0f, floor_z + 0.04f, cy, sy), 0.22f, 0.88f,
              0.54f, yaw, hull, roof, 0, 0);
    draw_box2(rt, v, e, yawp(axis, -2.34f, 0.0f, floor_z + 0.04f, cy, sy), 0.19f, 0.90f,
              0.46f, yaw, fly_v3scale(hull, 0.85f), roof, 0, 0);
    if (mid) {
        /* Glass right round, proud of the panel by a centimetre so it wins the
           depth test from every angle — the same trick the truck's cab uses.
           Lit from inside after dark: an unlit box on a lit line reads as
           freight, and this one is carrying the player. */
        fly_v3 lit = dark > 0.02f ? fly_v3mk(0.95f, 0.80f, 0.52f) : glass;
        draw_box2(rt, v, e, fly_v3add(axis, fly_v3mk(0, 0, floor_z + 0.46f)),
                  2.02f, 1.02f, 0.46f, yaw, lit, fly_v3scale(lit, 1.3f), 0, 0);
        /* the screen at each end, above its cowl — the same glass, and what
           makes the two end faces something other than a painted panel */
        draw_box2(rt, v, e, yawp(axis, 2.24f, 0.0f, floor_z + 0.50f, cy, sy),
                  0.05f, 0.82f, 0.46f, yaw, lit, fly_v3scale(lit, 1.3f), 0, 0);
        draw_box2(rt, v, e, yawp(axis, -2.24f, 0.0f, floor_z + 0.48f, cy, sy),
                  0.05f, 0.78f, 0.44f, yaw, lit, fly_v3scale(lit, 1.3f), 0, 0);
        /* The pillars between the windows. One box crosses the whole car and
           does both flanks at once, and without them the glass is a letterbox
           slot rather than the two saloon windows and a door it is meant to be
           — which is the difference between a vehicle and a container. */
        for (k = -1; k <= 1; k += 2)
            draw_box2(rt, v, e, yawp(axis, (float)k * 0.74f, 0.0f, floor_z + 0.44f,
                                     cy, sy), 0.09f, 1.03f, 0.50f, yaw, hull,
                      fly_v3scale(hull, 1.2f), 0, 0);
        /* the roof cap, and the strake along the flanks at floor level */
        draw_box2(rt, v, e, fly_v3add(axis, fly_v3mk(0, 0, floor_z + 1.14f)),
                  2.12f, 0.94f, 0.12f, yaw, fly_v3scale(roof, 0.8f), roof, 0, 0);
        draw_box2(rt, v, e, fly_v3add(axis, fly_v3mk(0, 0, floor_z + 0.16f)),
                  2.24f, 1.04f, 0.10f, yaw, shade, steel, 0, 0);
    }
    if (fine) {
        fly_v3 pts[2];
        float rr[2];
        /* a grab rail down each side of the roof, which is the detail that says
           somebody gets on and off this thing */
        for (k = 0; k < 2; ++k) {
            float side = k ? 0.72f : -0.72f;
            pts[0] = yawp(axis, -1.80f, side, floor_z + 1.34f, cy, sy);
            pts[1] = yawp(axis, 1.80f, side, floor_z + 1.34f, cy, sy);
            rr[0] = rr[1] = 0.045f;
            draw_pipeline(rt, v, e, pts, rr, 2, 6, steel);
        }
        /* the lamp housings, whether or not they are lit */
        for (k = 0; k < 2; ++k)
            draw_box2(rt, v, e, yawp(axis, 2.58f, k ? 0.54f : -0.54f, floor_z + 0.14f,
                                     cy, sy), 0.06f, 0.19f, 0.24f, yaw,
                      fly_v3mk(0.70f, 0.70f, 0.66f), fly_v3mk(0.80f, 0.80f, 0.75f), 0, 0);
    }
    /* Running lights, and they are on in daylight too: this is a vehicle on a
       line nobody can steer off, and what a headlight is for here is being seen
       coming rather than seeing. */
    for (k = 0; k < 2; ++k) {
        float side = k ? 0.54f : -0.54f;
        draw_light(rt, v, yawp(axis, 2.62f, side, floor_z + 0.24f, cy, sy),
                   FLY_RGBA(255, 244, 220, 225), 0.62f);
        draw_light(rt, v, yawp(axis, -2.52f, side * 1.35f, floor_z + 0.30f, cy, sy),
                   FLY_RGBA(255, 60, 40, 195), 0.44f);
    }
}

/* --- carrying the line over a road -----------------------------------------
 *
 * Where a road and the guideway cross, something has to be built, and until now
 * nothing was: the pipe ran over the carriageway on whatever piers the spacing
 * happened to put there, one of them as often as not standing in the near-side
 * lane. A road under a viaduct is not a coincidence of two lines, it is a
 * structure — the span is widened to clear the right of way, the two bents
 * either side of it are moved out to the verges, and the whole thing is lit,
 * because the one place a driver needs to see the road is the place with a
 * bridge over it.
 *
 * The survey already decided where these are and left a headroom under the
 * barrel to build to (see fly_road.h's crossings, FLY_ROAD_UNDER_CLEAR and the
 * corridor toll that makes a crossing square and rare in the first place). What
 * is here is only the structure, drawn from that record.
 *
 * The legs stand on the guideway's own line rather than square to the road,
 * because that is where the load is: they are the two piers that would have
 * been there anyway, moved apart until they clear the carriageway. How far
 * apart is the road's half width and a margin, divided by the sine of the
 * crossing angle — a square crossing needs the least, and a skew one needs a
 * longer span, which is exactly why a skew crossing is the expensive thing the
 * survey is priced to avoid. */
#define FLY_RAIL_PORTAL_CLEAR (FLY_ROAD_HALF + 5.0f)
#define FLY_RAIL_PORTAL_MIN 13.0f
#define FLY_RAIL_PORTAL_MAX 46.0f
#define FLY_RAIL_PORTAL_DRAW 2600.0f

/* Half the clear span this crossing needs, in metres along the guideway. */
static float rail_portal_half(const fly_road_cross *x) {
    float s = sinf(x->angle);
    float half = FLY_RAIL_PORTAL_CLEAR / (s > 0.28f ? s : 0.28f);
    if (half < FLY_RAIL_PORTAL_MIN) half = FLY_RAIL_PORTAL_MIN;
    if (half > FLY_RAIL_PORTAL_MAX) half = FLY_RAIL_PORTAL_MAX;
    return half;
}

/* Is this point inside a crossing's span — the stretch of guideway that is
 * carried on the portal and has no bent under it? Asked by the piers, and by
 * the road's own lighting, which must not put a column where the structure is.
 * `pad` is how much room the asker needs beyond the span itself. */
static int rail_portal_inside(const fly_game *g, float x, float y, float pad) {
    int c;
    for (c = 0; c < g->roads.cross_count; ++c) {
        const fly_road_cross *k = &g->roads.cross[c];
        float half = rail_portal_half(k) + pad;
        if (hypotf(x - k->pos.x, y - k->pos.y) < half) return 1;
    }
    return 0;
}

/* One leg: a blade pier on a spread footing, with the corbel the beam sits on.
 * Blade rather than round, and square to the line rather than to the road,
 * because it is a pier under a beam and not a column under a point — and
 * because a flat face is what carries the crossing's lighting and its hazard
 * marking. */
static void rail_portal_leg(fly_render_target *rt, const fly__view *v, const fly__env *e,
                            fly_v3 top, float ground, float yaw, fly_v3 col,
                            int up_close) {
    fly_v3 pad = fly_v3mk(0.44f, 0.43f, 0.41f);
    float h = top.z - ground;
    if (h <= 1.0f) return;
    draw_prism(rt, v, e, fly_v3mk(top.x, top.y, ground - 0.7f), 2.15f, 1.15f, 6,
               pad, fly_v3scale(pad, 1.1f));
    draw_box2(rt, v, e, fly_v3mk(top.x, top.y, ground), 0.62f, 1.55f, h * 0.82f, yaw,
              col, fly_v3scale(col, 1.12f), 0, 0);
    /* the shaft narrows and the corbel widens: the two together are what makes
       a leg read as carrying something rather than as a post under a pipe */
    draw_box2(rt, v, e, fly_v3mk(top.x, top.y, ground + h * 0.82f), 0.52f, 1.35f,
              h * 0.18f - 0.35f > 0.1f ? h * 0.18f - 0.35f : 0.1f, yaw,
              fly_v3scale(col, 1.06f), fly_v3scale(col, 1.15f), 0, 0);
    draw_box2(rt, v, e, fly_v3mk(top.x, top.y, top.z - 0.35f), 0.95f, 1.75f, 0.35f, yaw,
              fly_v3scale(col, 1.2f), fly_v3scale(col, 1.34f), 0, 0);
    if (!up_close) return;
    /* and the bearings under the beam, one each side of the leg's own axis,
       which is the detail that says the deck is sitting on this rather than
       growing out of it */
    {   int s;
        fly_v3 steel = fly_v3mk(0.30f, 0.31f, 0.33f);
        float cy = cosf(yaw), sy = sinf(yaw);
        for (s = -1; s <= 1; s += 2)
            draw_box2(rt, v, e, yawp(top, 0.0f, (float)s * 0.85f, 0.0f, cy, sy),
                      0.34f, 0.30f, 0.16f, yaw, steel, fly_v3scale(steel, 1.2f), 0, 0);
    }
}

static void draw_rail_portal(fly_render_target *rt, const fly__view *v, const fly__env *e,
                             const fly_game *g, const fly_road_cross *x, float pipe_r) {
    fly_v3 conc = fly_v3mk(0.52f, 0.51f, 0.48f);
    fly_v3 steel = fly_v3mk(0.44f, 0.45f, 0.47f);
    float half = rail_portal_half(x);
    float cy = cosf(x->yaw), sy = sinf(x->yaw);
    float dist = fly_v3dist(x->pos, v->pos);
    fly_v3 leg[2], seat[2];
    float road_yaw = x->yaw + FLY_PI * 0.5f;
    int s, up_close = dist < 700.0f;
    if (dist > FLY_RAIL_PORTAL_DRAW) return;
    if (!object_worth_drawing(v, rt, x->pos, half + 20.0f)) return;
    /* which way the road runs under it, so the abutments and the light the
       lanterns throw lie along the carriageway rather than across it */
    if (x->road >= 0 && x->road < g->roads.count) {
        fly_v3 tan;
        if (fly_road_span(&g->roads.road[x->road], x->station, x->t, NULL, &tan) &&
            hypotf(tan.x, tan.y) > 1e-4f)
            road_yaw = atan2f(tan.y, tan.x);
    }
    /* The two seats, on the guideway's own line and at its own height there.
     * Taken from the route rather than extrapolated off the crossing, because
     * the line is on a grade and on a curve through here and a beam hung level
     * off the crossing's height would meet the barrel at one end and miss it at
     * the other. */
    for (s = 0; s < 2; ++s) {
        float off = (s ? 1.0f : -1.0f) * half;
        fly_v3 q = fly_v3mk(x->pos.x + cy * off, x->pos.y + sy * off, x->deck);
        fly_v3 axis;
        float d, sep;
        if (fly_rail_project(&g->rail_route, q, &d, &sep) == 0 &&
            fly_rail_eval(&g->rail_route, d, &axis, NULL) == 0) seat[s] = axis;
        else seat[s] = q;
        leg[s] = seat[s];
    }
    {
        fly_v3 mid = fly_v3scale(fly_v3add(seat[0], seat[1]), 0.5f);
        float run = fly_v3dist(seat[0], seat[1]) * 0.5f;
        float beam = 0.62f;                       /* how deep the girder is */
        float top = mid.z - pipe_r;               /* the barrel sits on it */
        float yaw = atan2f(seat[1].y - seat[0].y, seat[1].x - seat[0].x);
        for (s = 0; s < 2; ++s)
            rail_portal_leg(rt, v, e, fly_v3mk(leg[s].x, leg[s].y, top - beam),
                            drawn_ground(&g->world, leg[s].x, leg[s].y), yaw, conc,
                            up_close);
        /* The girder, and the haunches over the seats. A beam of one depth from
           end to end is a plank; a beam that deepens where it is supported is
           the shape bending moment actually asks for, and it is two boxes. */
        draw_box2(rt, v, e, fly_v3mk(mid.x, mid.y, top - beam), run, pipe_r * 1.45f,
                  beam, yaw, fly_v3scale(conc, 0.94f), fly_v3scale(conc, 1.18f), 0, 0);
        for (s = 0; s < 2; ++s)
            draw_box2(rt, v, e, fly_v3mk(seat[s].x, seat[s].y, top - beam - 0.34f),
                      run * 0.26f, pipe_r * 1.30f, 0.34f, yaw,
                      fly_v3scale(conc, 0.90f), fly_v3scale(conc, 1.05f), 0, 0);
        if (up_close) {
            /* the parapet either side of the barrel: what stops the crossing
               reading as a pipe balanced on a shelf */
            for (s = -1; s <= 1; s += 2)
                draw_box2(rt, v, e,
                          fly_v3mk(mid.x - sinf(yaw) * (float)s * pipe_r * 1.30f,
                                   mid.y + cosf(yaw) * (float)s * pipe_r * 1.30f, top),
                          run, 0.16f, 0.72f, yaw, steel, fly_v3scale(steel, 1.2f), 0, 0);
            /* and the hazard band on the leading face of the girder, which is
               the one piece of a bridge every driver under it has looked at */
            for (s = -1; s <= 1; s += 2) {
                fly_v3 band = fly_v3mk(mid.x - sinf(yaw) * (float)s * (pipe_r * 1.45f + 0.06f),
                                       mid.y + cosf(yaw) * (float)s * (pipe_r * 1.45f + 0.06f),
                                       top - beam + 0.10f);
                draw_box2(rt, v, e, band, run * 0.92f, 0.06f, 0.30f, yaw,
                          fly_v3mk(0.72f, 0.62f, 0.16f), fly_v3mk(0.78f, 0.68f, 0.20f), 0, 0);
            }
        }
        /* --- the road under it -------------------------------------------
         *
         * An abutment wall each side of the carriageway, which is what a road
         * passing under a structure has: the batter of the cutting is held
         * back off the running lanes rather than spilling into them, and the
         * wall is what the crossing is read against from a truck's height. */
        if (up_close) {
            float rc = cosf(road_yaw), rs = sinf(road_yaw);
            for (s = -1; s <= 1; s += 2) {
                fly_v3 w0 = fly_v3mk(x->pos.x - rs * (float)s * (FLY_ROAD_HALF + 1.1f),
                                     x->pos.y + rc * (float)s * (FLY_ROAD_HALF + 1.1f),
                                     x->pos.z - FLY_ROAD_DECK);
                draw_box2(rt, v, e, w0, 11.0f, 0.42f, 1.35f, road_yaw,
                          fly_v3scale(conc, 0.88f), fly_v3scale(conc, 1.0f), 0, 0);
            }
        }
        /* --- and it is lit -----------------------------------------------
         *
         * Both legs carry a lantern under the beam, aimed down the road. A
         * crossing is the one stretch of carriageway with something solid
         * standing beside it, and an unlit one is a black gap in a lit road —
         * which is exactly what the first version of this looked like once the
         * road's own columns were moved out of the structure. */
        {
            float on = fly_clampf(lamp_dark(e, x->pos), 0.0f, 1.0f);
            if (on > 0.02f) {
                for (s = 0; s < 2; ++s) {
                    fly_v3 lamp = fly_v3mk(leg[s].x, leg[s].y, top - beam - 1.1f);
                    float drop = lamp.z - x->pos.z;
                    draw_box2(rt, v, e, lamp, 0.34f, 0.52f, 0.26f, yaw, steel,
                              fly_v3mk(1.5f, 1.5f, 1.6f), 0, 0);
                    draw_light(rt, v, lamp, FLY_RGBA(232, 238, 255, (int)(210.0f * on)),
                               dist < 900.0f ? 1.10f : 0.85f);
                    /* Each lantern throws its own, down the road it is over,
                       from where it actually hangs — which is what puts the
                       light under the beam and out both portals instead of one
                       ellipse painted across the deck between them. */
                    if (drop > 1.0f)
                        lamp_light(rt, v, e, lamp, road_yaw, drop, drop * 3.6f + 8.0f,
                                   FLY_LAMP_WHITE, 0.09f * on);
                }
            }
        }
    }
}

static void draw_rail(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly_game *g) {
    /* The pipe. It used to be a 0.56 m wide, 0.36 m tall flat box in near-black
     * drawn one span at a time — at any distance a sub-pixel dark ribbon that
     * broke into a dashed line of crawling fragments, and at any bend a chain
     * of separate sausages. It is one continuous mitred tube now (see
     * draw_pipeline), wide enough to read as infrastructure, galvanised so it
     * separates from both grass and a sea reflecting the sky, and never thinner
     * on screen than about two output pixels so the silhouette stays whole to
     * the horizon. Over-wide in the far distance is the right trade: a line you
     * can follow to the horizon beats a physically exact one that dissolves. */
    static fly_v3 pts[FLY_RAIL_DRAW_MAX];
    static float rad[FLY_RAIL_DRAW_MAX];
    const fly_rail_route *r = &g->rail_route;
    /* The survey's own number, not a second opinion about it: everything that
       passes under the line clears the underside of this barrel. */
    const float pipe_r = FLY_RAIL_PIPE_R;
    fly_v3 steel = fly_v3mk(0.50f, 0.53f, 0.58f);
    fly_v3 pylon = fly_v3mk(0.34f, 0.36f, 0.40f);
    int i, n = r->point_count, sides, m = 0;
    float near_dist = 1e30f;
    float dark = 1.0f - fly_smoothstepf(0.16f, 0.46f, e->day);

    if (n < 2) return;
    if (n > FLY_RAIL_POINT_MAX) n = FLY_RAIL_POINT_MAX;
    /* --- the drawn line ---------------------------------------------------
     *
     * The survey's stations, and the arcs between them. Both halves of the
     * survey want the arc, and the *profile* wanted it first: a taut line over
     * the ground is straight wherever the ground lets it be and changes
     * gradient wherever it touches down, so the deck arrives at a crest on one
     * grade and leaves on another — and a mitred tube through those two
     * stations is a visible bend in a pipe, in the one piece of geometry the
     * rail camera spends the whole journey looking straight down.
     *
     * The plan wants it too now that the line winds. It used to be held to a
     * four-kilometre radius, which is a line that does not really turn and
     * whose chords are its arcs; at the fifteen hundred metres a meandering
     * route holds, a 75 m chord sits half a metre inside its own curve, and a
     * chain of them is a pipe drawn as a polygon.
     *
     * fly_rail_grade rounds the gradient breaks off into a vertical curve;
     * drawing the stations chord to chord throws that away again and puts the
     * corner back as a facet. So a span near enough to see is drawn as the
     * curve through its stations, and one far enough that the whole tube is two
     * pixels wide is drawn as the single segment it resolves to.
     *
     * Centripetal, and the termini are why. The survey's stations are evenly
     * spaced everywhere except at its two ends, where the trim that stops the
     * line short of a settlement leaves a stub five metres long against a
     * neighbour seventy-five — and on that the uniform form takes a forty-metre
     * tangent across a five-metre span, so the curve overshot the terminus and
     * came back through itself. A mitred tube through a reversal opens every
     * ring to the widening cap: the last thing on the line was a splayed fan of
     * rings five times the pipe's width, at the one place a rider ends up
     * standing. The bound is the fix, not a shorter step. */
    for (i = 0; i + 1 < n; ++i) {
        fly_v3 a = r->points[i].pos, b = r->points[i + 1].pos;
        float d0 = fly_v3dist(a, v->pos), d1 = fly_v3dist(b, v->pos);
        float dist = d0 < d1 ? d0 : d1;
        int sub = dist < 300.0f ? 4 : (dist < 1200.0f ? 2 : 1), s;
        /* Every remaining station has to keep its own point whatever happens —
           a line that ran out of buffer and stopped would be a rail that ends
           in mid-air — so the extra samples are only taken while there is room
           left over after one apiece. */
        if (m + sub + (n - 1 - i) >= FLY_RAIL_DRAW_MAX - 1) sub = 1;
        if (d0 < near_dist) near_dist = d0;
        for (s = 0; s < sub; ++s) {
            float t = (float)s / (float)sub;
            float pd, minr;
            pts[m] = a;
            if (sub > 1) fly_line_span(r->points, n, FLY_LINE_CHORD, i, t, &pts[m], NULL);
            pd = fly_v3dist(pts[m], v->pos);
            /* world radius covering ~2 output pixels at this depth */
            minr = (v->ortho ? 1.0f : pd) * (float)v->aa / v->sy;
            rad[m] = pipe_r > minr ? pipe_r : minr;
            ++m;
        }
    }
    {   /* the far terminus, which no span starts at */
        float pd = fly_v3dist(r->points[n - 1].pos, v->pos);
        float minr = (v->ortho ? 1.0f : pd) * (float)v->aa / v->sy;
        pts[m] = r->points[n - 1].pos;
        rad[m] = pipe_r > minr ? pipe_r : minr;
        ++m;
        if (pd < near_dist) near_dist = pd;
    }
    /* a coarser barrel far away is free: past a kilometre it is two pixels
       across and the extra faces resolve to nothing */
    sides = near_dist < 400.0f ? 14 : (near_dist < 1400.0f ? 9 : 5);
    /* Domed at both ends. A guideway is a formed thing that stops in a yard,
       not a length of pipe somebody cut: the flat disc read as a lid stuck on
       the end, and it is the one piece of the line the rider walks up to. */
    draw_pipeline_capped(rt, v, e, pts, rad, m, sides, steel, 1);

    /* The piers, with a crosshead the tube sits on.
     *
     * There used to be one at each surveyed station and nowhere else, and the
     * survey's stations are a hundred and twenty metres apart: a hundred and
     * twenty metres of unsupported pipe standing thirteen metres up, which is
     * not a viaduct, it is a pipe hanging in the sky with an occasional stick
     * under it. From the ground — which is where the walking camera and the
     * whole `rail_approach` frame are — the eye reads the span before it reads
     * anything else and the structure simply does not carry.
     *
     * So the span is subdivided: a bent every FLY_RAIL_BENT metres, seated on
     * the same curve the barrel is drawn along rather than on the chord across
     * it, so an intermediate pier meets the underside of the tube exactly where
     * the tube is. It used to lerp, which was right while the tube was a chain
     * of chords and puts a pier's crosshead a hand's breadth inside the barrel
     * now that it is not. Beyond a couple of kilometres the line reads on its
     * own and the piers are CPU geometry resolving to nothing, and past a
     * kilometre the intermediates are gone too and only the stations keep a
     * pier. */
    for (i = 0; i + 1 < n; ++i) {
        fly_v3 p = r->points[i].pos, q = r->points[i + 1].pos;
        fly_v3 d = fly_v3sub(q, p);
        float span = hypotf(d.x, d.y);
        float yaw = atan2f(d.y, d.x);
        float dist = fly_v3dist(p, v->pos);
        float minr = (v->ortho ? 1.0f : dist) * (float)v->aa / v->sy;
        float pr = pipe_r > minr ? pipe_r : minr;
        int b, bents;
        /* The lighting runs the whole route, which is well past the range the
           structure under it is worth drawing at — so it is asked before the
           piers give up rather than after. */
        if (dist < (dark > 0.02f ? FLY_RAIL_LAMP_FAR : FLY_RAIL_LAMP_NEAR))
            rail_lamps(rt, v, e, r, i, n, pipe_r, dist, dark);
        if (dist > 2600.0f) continue;
        if (pr > pipe_r * 1.6f) pr = pipe_r * 1.6f;
        bents = dist < 1100.0f && span > FLY_RAIL_BENT ? (int)(span / FLY_RAIL_BENT) : 1;
        if (bents > 8) bents = 8;
        for (b = 0; b < bents; ++b) {
            fly_v3 c = p;
            fly_line_span(r->points, n, FLY_LINE_CHORD, i,
                          (float)b / (float)bents, &c, NULL);
            if (!object_worth_drawing(v, rt, c, pr * 3.0f + 12.0f)) continue;
            /* Not in the road. Where the line crosses a carriageway the span is
               carried by the portal's two legs out on the verges, and a bent on
               the regular spacing through there is a pier standing in the
               near-side lane — which is what was there before. */
            if (rail_portal_inside(g, c.x, c.y, 0.0f)) continue;
            /* on the ground the mesh draws rather than the ground the height
               function reports: a footing pad seated on the second is a pad
               with its top edge under the grass — see drawn_ground */
            draw_pier(rt, v, e, c, drawn_ground(&g->world, c.x, c.y), pr, yaw, pylon);
        }
    }
    /* the far terminus, which is the one station no span starts at */
    {
        fly_v3 tip = r->points[n - 1].pos;
        float dist = fly_v3dist(tip, v->pos);
        float minr = (v->ortho ? 1.0f : dist) * (float)v->aa / v->sy;
        float pr = pipe_r > minr ? pipe_r : minr;
        fly_v3 d = fly_v3sub(tip, r->points[n - 2].pos);
        if (pr > pipe_r * 1.6f) pr = pipe_r * 1.6f;
        if (dist <= 2600.0f && object_worth_drawing(v, rt, tip, pr * 3.0f + 12.0f))
            draw_pier(rt, v, e, tip, drawn_ground(&g->world, tip.x, tip.y),
                      pr, atan2f(d.y, d.x), pylon);
    }

    /* And the structures where the line crosses a road. Drawn from the survey's
       own record of the crossings rather than found here — see fly_road.h. */
    {   int c;
        for (c = 0; c < g->roads.cross_count; ++c)
            draw_rail_portal(rt, v, e, g, &g->roads.cross[c], pipe_r);
    }

    /* The railcar rides on the crown of the pipe. It used to be centred on the
     * pipe's own axis, which buried it inside the barrel, and the way that was
     * made visible was to stop drawing the barrel for a couple of points either
     * side of it. At the spacing the route uses that is a quarter-mile hole in
     * the line, directly ahead of the one camera pointed down it: from the
     * rider's seat the rail simply was not there. Sitting it on top costs
     * nothing and the line stays whole. */
    if (g->mode == FLY_MODE_RAIL)
        draw_railcar(rt, v, e, g->rail.pos, atan2f(g->rail.tangent.y, g->rail.tangent.x),
                     pipe_r, dark);
}

/* The end of a road, made into an end.
 *
 * A carriageway is a slab standing on its own kerb, and a slab that simply
 * stops is a slab with a sheer face across it — half a metre to a metre and a
 * half of black wall facing whoever is walking up to it, which is what every
 * terminus in the world looked like: a strip of tarmac cut off with scissors
 * and dropped in a field.
 *
 * So the last few metres ramp: the deck falls from its own level to the ground
 * over `RUN` metres past the terminus, narrowing as it goes, with the kerb
 * closing down to nothing either side. That is what a road does where it meets
 * an unmade surface, and it is also the thing the gate then arrives at — the
 * settlement's run-in starts at the tip of this rather than at a step.
 *
 * `ux`,`uy` is the unit heading *out* of the road at this end, so the ramp is
 * always drawn away from the carriageway and never over the top of it. */
static void road_ramp(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly_game *g, fly_v3 tip, float ux, float uy, float half) {
    const float RUN = FLY_ROAD_RAMP;
    fly_v3 wall = fly_v3mk(0.27f, 0.245f, 0.20f);
    fly_v3 metal = fly_v3mk(0.145f, 0.145f, 0.15f);
    float nx = -uy, ny = ux;
    fly_v3 a0, a1, b0, b1;
    float tipz, toe = drawn_ground(&g->world, tip.x + ux * RUN, tip.y + uy * RUN) + 0.10f;
    float tw = half * 0.72f;
    /* Never uphill: a ramp that climbs out of the road is a hump. Where the
       ground beyond the terminus is above the deck the cut face is terrain's
       business and all this may do is lie flat on it. */
    tipz = tip.z;
    if (toe > tipz) toe = tipz;
    a0 = fly_v3mk(tip.x + nx * half, tip.y + ny * half, tipz);
    a1 = fly_v3mk(tip.x - nx * half, tip.y - ny * half, tipz);
    b0 = fly_v3mk(tip.x + ux * RUN + nx * tw, tip.y + uy * RUN + ny * tw, toe);
    b1 = fly_v3mk(tip.x + ux * RUN - nx * tw, tip.y + uy * RUN - ny * tw, toe);
    tri_shaded(rt, v, e, a0, a1, b1, metal, 0.94f);
    tri_shaded(rt, v, e, a0, b1, b0, metal, 0.94f);
    /* the kerb, tapering into the ground with the deck it holds up */
    {
        float g0 = drawn_ground(&g->world, a0.x, a0.y);
        float g1 = drawn_ground(&g->world, a1.x, a1.y);
        if (g0 > tipz - FLY_ROAD_DECK) g0 = tipz - FLY_ROAD_DECK;
        if (g1 > tipz - FLY_ROAD_DECK) g1 = tipz - FLY_ROAD_DECK;
        if (tipz - g0 > 8.0f) g0 = tipz - 8.0f;
        if (tipz - g1 > 8.0f) g1 = tipz - 8.0f;
        wall_quad(rt, v, e, fly_v3mk(a0.x, a0.y, g0), fly_v3mk(b0.x, b0.y, toe - 0.12f),
                  b0, a0, wall, 0.95f);
        wall_quad(rt, v, e, fly_v3mk(b1.x, b1.y, toe - 0.12f), fly_v3mk(a1.x, a1.y, g1),
                  a1, b1, wall, 0.95f);
    }
}

/* --- the roads ------------------------------------------------------------
 *
 * A carriageway laid on the ground, draped like `prop_track`'s yard surfacing
 * and for the same reason — the road's own centreline is smooth, the ground
 * under it is not, and a flat ribbon over a cross-slope buries one edge and
 * floats the other. Each edge takes the higher of the terrain under it and the
 * centreline, so the surface is a road on an embankment rather than a decal
 * sinking into a hillside.
 *
 * It used to be two triangles a span, straight from one surveyed station to the
 * next, and that was fine while a road *was* two dozen straight lines: a chord
 * across a span of a line that is not turning is the line. It is not fine now
 * that the network winds. At the survey's 90 m pitch and a 600 m radius every
 * station is a nine-degree crease, and a run of them reads as exactly what it
 * is — a polygon pretending to be a curve, with a flat spot every ninety metres
 * all the way to the horizon.
 *
 * So a span is drawn as the arc it is. `fly_road_span` is the one definition of
 * where the road goes; this walks it in sub-steps chosen by how big the span is
 * on screen, and the same sub-steps carry the kerb, the bank and the paint, so
 * nothing on the road disagrees with anything else about where the road is.
 *
 * Across, too. A carriageway is cambered — it sheds water off a crown down the
 * middle — and near enough to see, the deck is two quads wide with that crown
 * between them. Far enough that the whole road is four pixels, it is the one
 * flat ribbon it resolves to anyway.
 *
 * The width is held to about two output pixels the way the rail's pipe is. A
 * physically exact 10 m road is sub-pixel at four kilometres and dissolves into
 * a dashed line of crawling fragments, which is the one thing a road must not
 * do: the whole point of drawing the network is that from three thousand feet
 * you can see where the ground traffic goes and follow it to the next
 * settlement. Over-wide in the far distance is the right trade.
 *
 * Nothing here is a shadow caster. A ribbon lying on the terrain casts onto the
 * terrain it is lying on, which is a few thousand triangles of replay for a
 * shadow nobody can see. */

/* The camber the deck is drawn with is FLY_ROAD_CAMBER, and it lives in
 * fly_road.h with the rest of the carriageway's shape: whatever puts a vehicle
 * on the deck has to sit on the same surface the deck is drawn as.
 *
 * One cross-section of the carriageway: where the centreline is, which way is
 * across it, and where the bank either side of it comes to ground. */
typedef struct {
    fly_v3 c;          /* on the deck, at the crown */
    float nx, ny;      /* unit, across the road, to the left of travel */
    float half;        /* half the drawn width */
    fly_v3 toe[2];     /* left then right, where the batter meets the ground */
} road_xs;

/* The toe of the bank, found by walking outward from the deck edge until the
 * ground comes up to meet the batter. Two steps is enough at these heights and
 * it is what turns a vertical face into tipped material: the taller the fill,
 * the further out it spreads.
 *
 * In a cutting there is nothing to tip: the ground beside the deck is above it,
 * the clamp below pins the toe at one slab's depth and the walk stops there.
 * The cut face itself is terrain — fly_world_ground carries it — so all this
 * has to draw is the kerb. */
static fly_v3 road_toe(const fly_world *w, fly_v3 edge, float ox, float oy) {
    float gz = fly_world_ground(w, edge.x, edge.y);
    float hgt, spread, bx = edge.x, by = edge.y;
    int it;
    if (gz > edge.z - FLY_ROAD_DECK) gz = edge.z - FLY_ROAD_DECK;
    if (edge.z - gz > 40.0f) gz = edge.z - 40.0f;   /* a viaduct, not a bank */
    hgt = edge.z - gz;
    for (it = 0; it < 2; ++it) {
        spread = hgt * FLY_ROAD_BATTER;
        bx = edge.x + ox * spread;
        by = edge.y + oy * spread;
        gz = fly_world_ground(w, bx, by);
        if (gz > edge.z - FLY_ROAD_DECK) gz = edge.z - FLY_ROAD_DECK;
        if (edge.z - gz > 40.0f) gz = edge.z - 40.0f;
        hgt = edge.z - gz;
    }
    return fly_v3mk(bx, by, gz);
}

/* A point on the deck, `u` of the way from the left edge (-1) to the right
 * (+1). The crown is a parabola across, which is what a camber is. */
static fly_v3 road_deck(const road_xs *x, float u, float lift) {
    return fly_v3mk(x->c.x + x->nx * x->half * u, x->c.y + x->ny * x->half * u,
                    x->c.z - FLY_ROAD_CAMBER * u * u + lift);
}

/* --- and keeping it out of the hillside ----------------------------------
 *
 * The survey lays the deck a slab above the highest ground within the
 * formation, measured at each surveyed station: see `road_shelf`. That is the
 * right rule and it is not quite enough to draw with, for two reasons that
 * both come down to the drawn terrain not being the height function.
 *
 * The mesh is bilinear between vertices up to a cell apart, so the surface
 * that gets rasterised stands above `fly_world_ground` by up to about what the
 * ground varies across a cell — `ground_bias`, which every other thing laid on
 * the ground here already goes through. The shelf's ring is measured about the
 * *centreline*, so the excess at the deck's own edge, half a carriageway out,
 * is nobody's business; and the stations are 60 m apart, so what the ground
 * does between two of them is nobody's either. Measured across five seeds:
 * about three deck samples in a thousand stand under the drawn ground, by up
 * to 1.25 m — which is not a hairline, it is a hummock coming up through the
 * tarmac, and it is worst exactly where a road crosses rolling country.
 *
 * So the drawn deck is lifted wherever the drawn ground would come through it.
 * Not the surveyed deck: the profile, the gradients and the earthworks are the
 * survey's answer and this does not get to move them. It is the same
 * correction `drawn_ground` applies to a service track, applied to the one
 * surface a road is, and away from the handful of places it bites it changes
 * nothing at all.
 *
 * Both edges and the crown are asked, and asked of `drawn_ground` rather than
 * of the height function: the edges are a camber below the crown and it is the
 * low side of a cross-slope that gets buried, and the excess the mesh carries
 * at the edge is not the excess it carries at the centreline. Sampling the
 * middle and sharing its answer across the section was the first cut of this
 * and it left three quarters of the burials in place — the bias is smooth, but
 * not over the seven and a half metres out to the kerb. */
static float road_bed(const fly_world *w, fly_v3 c, float nx, float ny, float half) {
    float gc = drawn_ground(w, c.x, c.y);
    float gl = drawn_ground(w, c.x - nx * half, c.y - ny * half);
    float gr = drawn_ground(w, c.x + nx * half, c.y + ny * half);
    float need = gl > gr ? gl : gr;
    need += FLY_ROAD_CAMBER;      /* the edge sits a camber under the crown */
    if (gc > need) need = gc;
    /* clear of the ground rather than coincident with it: two surfaces at the
       same height are a z-fighting band down the middle of the carriageway */
    return need + 0.05f;
}

/* Where the carriageway is `t` of the way along span `i`, and everything the
 * drawing needs about it there. `bank` asks for the toes, which cost four
 * heightfield taps and are wanted only close in. */
static void road_section(const fly_game *g, const fly__view *v, const fly_road *rd,
                         int i, float t, int bank, road_xs *x) {
    fly_v3 tan;
    float len, dist, bed;
    fly_road_span(rd, i, t, &x->c, &tan);
    len = hypotf(tan.x, tan.y);
    x->nx = len > 1e-4f ? -tan.y / len : 0.0f;
    x->ny = len > 1e-4f ? tan.x / len : 1.0f;
    dist = fly_v3dist(x->c, v->pos);
    x->half = (v->ortho ? 1.0f : dist) * (float)v->aa / v->sy;
    if (x->half < FLY_ROAD_HALF) x->half = FLY_ROAD_HALF;
    /* The lift is measured across the real carriageway, never across the
       widened one: past a few kilometres the drawn deck is held to a couple of
       output pixels rather than to ten metres, and a road that lifted itself
       clear of ground half a lane-width outside its own edge would climb the
       hillside as the camera flew away from it. */
    bed = road_bed(&g->world, x->c, x->nx, x->ny,
                   x->half < FLY_ROAD_HALF ? x->half : FLY_ROAD_HALF);
    if (x->c.z < bed) x->c.z = bed;
    if (bank) {
        fly_v3 el = road_deck(x, -1.0f, 0.0f), er = road_deck(x, 1.0f, 0.0f);
        x->toe[0] = road_toe(&g->world, el, -x->nx, -x->ny);
        x->toe[1] = road_toe(&g->world, er, x->nx, x->ny);
    } else {
        x->toe[0] = x->toe[1] = x->c;
    }
}

/* How many pieces a span is drawn in. The span is a fixed 90 m of world, so
 * this is only ever a question about how much of the screen it covers. */
static int road_steps(float dist) {
    if (dist < 260.0f) return 8;
    if (dist < 700.0f) return 5;
    if (dist < 1800.0f) return 3;
    if (dist < 5000.0f) return 2;
    return 1;
}

/* --- lighting the road ----------------------------------------------------
 *
 * The network is lit end to end, both sides, the whole way between every pair
 * of settlements. That is a decision about what these roads *are*: they are the
 * arteries the freight moves on, the thing a settlement exists at the end of,
 * and the one piece of infrastructure the player is meant to be able to follow
 * across the country. A dark ribbon does that in daylight and disappears
 * completely at night, which is exactly when following it matters most.
 *
 * Three ranges, because a lamp is three different things depending on how far
 * away it is and drawing the near one everywhere would cost more than the
 * terrain does:
 *
 *   Close in it is the column the settlements already use — plinth, shaft,
 *   cranked arm, and after dark a real emitter with a pool of light under it.
 *   Beyond that it is a bare shaft and its head: at four hundred metres the
 *   plinth is a pixel and the arm is two, and what carries is the rhythm of
 *   uprights down the verge.
 *   Beyond *that* there is no column worth drawing at all, and after dark what
 *   is left is the only part that ever mattered from the air — the light
 *   itself, a chain of them running off over the dark ground, which is what
 *   tells you where the road went.
 *
 * And that last range runs as far as the road does. It used to stop at nine
 * kilometres while the carriageway itself was drawn to twenty-six, so from
 * altitude at night the lit chain ended in mid-country and a grey ribbon
 * carried on into the dark — which is the one thing the lighting is for read
 * backwards: the road you can follow stops being followable exactly where it
 * gets too far away to see any other way. The end of it is `draw_light`'s own
 * range instead, which fades a lamp out over the last few kilometres and drops
 * it at twenty-six, so a road is lit for as long as it is drawn and no lamp is
 * ever a speck with nothing under it. The pitch thins with distance to pay for
 * it: at sixteen kilometres one lamp in eight is half a kilometre of road
 * apart and still a dozen pixels between neighbours.
 *
 * Alternating sides, so the arms reach in over the carriageway from both verges
 * the way road lighting does, and standing on the road's own formation rather
 * than on the terrain beside it. That is not a shortcut, it is where a lamp
 * goes: a road crossing a hollow on a six-metre embankment carries its lighting
 * along the shoulder of the embankment, and a column seated on the field the
 * bank was built out of would have its head below the carriageway it is
 * supposed to be lighting. Which is also why the offset is barely outside the
 * paved edge — a metre further out and the plinth is standing on the batter. */
#define FLY_ROAD_LAMP_PITCH 62.0f
#define FLY_ROAD_LAMP_OFF (FLY_ROAD_HALF + 0.8f)
/* How far a lamp is worth drawing as a column, and how far as a light. The
 * second is the road's own draw range: past it there is no road under the
 * lamp. */
#define FLY_ROAD_LAMP_NEAR 2600.0f
#define FLY_ROAD_LAMP_FAR 26000.0f

static void road_lamp(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 base, float yaw, float dist) {
    fly_v3 steel = fly_v3mk(0.46f, 0.47f, 0.49f);
    float cy = cosf(yaw), sy = sinf(yaw), h = 8.4f;
    fly_v3 head = yawp(base, 2.3f, 0, h, cy, sy);
    /* Its own dusk, not the frame's: a road does not light up all at once. See
       lamp_dark. How far the chain is drawn is still the caller's `dark`,
       because that is a question about the frame rather than about this lamp. */
    float on = fly_clampf(lamp_dark(e, base), 0.0f, 1.0f);
    /* The full column close in; a shaft and its head while those are still more
       than a pixel each; nothing but the light beyond that. */
    if (dist < 520.0f) { prop_lamp(rt, v, e, base, yaw); return; }
    if (dist < FLY_ROAD_LAMP_NEAR) {
        draw_box2(rt, v, e, base, 0.30f, 0.30f, h, yaw, steel,
                  fly_v3scale(steel, 1.12f), 0, 0);
        draw_box2(rt, v, e, yawp(base, 1.2f, 0, h - 0.22f, cy, sy), 1.35f, 0.14f, 0.24f,
                  yaw, steel, fly_v3scale(steel, 1.1f), 0, 0);
    }
    if (on <= 0.02f) return;
    /* And the light itself, which is what makes a road at night a road.
     *
     * A splat at the head alone is a dot in the dark: from two thousand feet a
     * lit carriageway is a chain of overlapping patches *on the ground*, and
     * the ground under them is the only thing saying there is a road there at
     * all. It used to stop at fourteen hundred metres, because past that a pool
     * cost the same sixteen triangles of the shared blend list it cost at
     * twenty. A deferred light costs its own screen circle instead — 40 m of
     * reach at four kilometres is a dozen pixels — so it runs out to where the
     * chain thins to one lamp in four and the lit road no longer has to end in
     * mid-country and carry on as a chain of specks. */
    lamp_light(rt, v, e, fly_v3add(head, fly_v3mk(0, 0, -0.22f)), yaw + FLY_PI * 0.5f,
               FLY_LAMP_MOUNT, FLY_LAMP_REACH, FLY_LAMP_SODIUM, FLY_LAMP_POWER * on);
    draw_light(rt, v, head, FLY_RGBA(255, 214, 150, (int)(215.0f * on)),
               dist < FLY_ROAD_LAMP_NEAR ? 1.15f : 0.85f);
}

/* The lamps whose chainage falls inside span `i`. Placed by distance along the
 * road rather than per station, so the pitch is 62 m of carriageway wherever
 * the survey happened to put its points and a column never crawls along the
 * verge as the camera moves. */
/* Where the k-th lamp on a road stands, and which way its arm is cranked.
 *
 * By chainage from the start of the road, so the pitch is 62 m of carriageway
 * wherever the survey happened to put its stations and a column never crawls
 * along the verge as the camera moves. Alternating sides, so the arms reach in
 * over the carriageway from both verges the way road lighting does.
 *
 * One function, because two things ask: the drawing, which asks for the lamps
 * on the span it is drawing, and `fly_render_lamp_probe`, which asks for all of
 * them. A probe that computed the placement a second time would be a test of
 * its own arithmetic rather than of the road's lighting. Returns 0 past the end
 * of the road. */
static int road_lamp_at(const fly_game *g, const fly_road *rd, int k,
                        fly_v3 *base, float *yaw, float *chain_out) {
    float chain = FLY_ROAD_LAMP_PITCH * 0.5f + (float)k * FLY_ROAD_LAMP_PITCH;
    float side = (k & 1) ? 1.0f : -1.0f;
    fly_v3 c, tan;
    float len, nx, ny, bed;
    int lo = 0, hi = rd->point_count - 2, i;
    if (k < 0 || rd->point_count < 2 || chain > rd->length) return 0;
    /* the span this chainage falls on; the distances are monotonic, so it is a
       bisection rather than a walk down a thousand stations */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (rd->points[mid].distance <= chain) lo = mid; else hi = mid - 1;
    }
    i = lo;
    {   float d0 = rd->points[i].distance, d1 = rd->points[i + 1].distance;
        if (d1 - d0 < 1e-3f) return 0;
        if (!fly_road_span(rd, i, (chain - d0) / (d1 - d0), &c, &tan)) return 0; }
    len = hypotf(tan.x, tan.y);
    if (len < 1e-4f) return 0;
    nx = -tan.y / len;
    ny = tan.x / len;
    /* On the formation, and on the *drawn* deck's formation: where the
       carriageway has been lifted clear of a hummock the shoulder beside it
       comes up with it, or the column it carries is standing in a hole of its
       own road's making. */
    bed = road_bed(&g->world, c, nx, ny, FLY_ROAD_HALF);
    if (base) *base = fly_v3mk(c.x + nx * side * FLY_ROAD_LAMP_OFF,
                               c.y + ny * side * FLY_ROAD_LAMP_OFF,
                               (c.z > bed ? c.z : bed) - FLY_ROAD_DECK);
    /* The arm cranks back in *over the carriageway*, so the yaw is the bearing
     * from the column toward the centreline.
     *
     * It used to be that bearing turned ninety degrees, which is along the
     * road: every lamp on every road in the world reached its arm down the
     * verge instead of over the traffic, and — since the pool of light is laid
     * under the head — lit the grass beside the road rather than the road. The
     * column drawing hid it, because a cross-arm seen end-on from a moving
     * aeroplane is a cross-arm; what gave it away was the pool, the moment the
     * pool became a shape instead of a disc. */
    if (yaw) *yaw = atan2f(-side * ny, -side * nx);
    if (chain_out) *chain_out = chain;
    return 1;
}

static void road_lamps(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g, const fly_road *rd, int i, float dist,
                       float dark) {
    float d0 = rd->points[i].distance, d1 = rd->points[i + 1].distance;
    int k, k0, k1, stride = lamp_stride(dist);
    if (d1 - d0 < 1e-3f) return;
    /* Half-open on purpose: the lamp that lands exactly on a station belongs to
       the span after it and not to both, or two columns stand in one hole. */
    k0 = (int)ceilf((d0 - FLY_ROAD_LAMP_PITCH * 0.5f) / FLY_ROAD_LAMP_PITCH);
    k1 = (int)ceilf((d1 - FLY_ROAD_LAMP_PITCH * 0.5f) / FLY_ROAD_LAMP_PITCH) - 1;
    if (k0 < 0) k0 = 0;
    for (k = k0; k <= k1; ++k) {
        fly_v3 base;
        float yaw, ld;
        if (stride > 1 && (k % stride)) continue;
        if (!road_lamp_at(g, rd, k, &base, &yaw, NULL)) continue;
        ld = fly_v3dist(base, v->pos);
        if (ld > (dark > 0.02f ? FLY_ROAD_LAMP_FAR : FLY_ROAD_LAMP_NEAR)) continue;
        /* Not under the bridge: the crossing is lit off the portal's own legs,
           and a column on the regular pitch through there stands in the
           structure carrying the line over it. */
        if (rail_portal_inside(g, base.x, base.y, 4.0f)) continue;
        if (!object_worth_drawing(v, rt, base, 12.0f)) continue;
        road_lamp(rt, v, e, base, yaw, ld);
    }
}

/* --- the line beside the road ----------------------------------------------
 *
 * The worked country is a parish: a field, a hedge, a steading and a lane, all
 * of it hung off a settlement's own clearing radius and all of it gone within
 * a couple of kilometres of the town that works it. What that leaves is the
 * stretch between two parishes — most of the length of every road in the world
 * — where the belt has run out and the frame is a carriageway across open
 * ground with nothing built anywhere in it.
 *
 * What belongs there is not another parish. It is the thing that belongs to a
 * *corridor* rather than to a place, and the cheapest of those is a
 * transmission line following the road. It costs nothing to route, which is
 * the whole reason it is this and not the quarry the same entry asked for: the
 * polyline is already surveyed, already bounded in curvature, already clear of
 * the water and already keeping its distance from the guideway. A line beside
 * it is a mast on a fixed pitch of that chainage and a span of wire between
 * two of them — the road lighting's own shape, placed by the same rule (see
 * road_lamp_at) and for the same reason, so a tower never crawls along the
 * verge as the camera moves either.
 *
 * The one question it actually poses is what a wire costs at range, and the
 * answer is that a conductor is five centimetres and stops being a triangle
 * long before the mast carrying it does. So the two are priced apart. Both are
 * held to a floor of about an output pixel — below that a chain of sub-pixel
 * triangles misses pixel centres and a line comes out dashed and crawling
 * rather than thin, which is the same widening `draw_rail` gives its barrel —
 * but the wire is given up at FLY_ROAD_WIRE_FAR while the mast carries on to
 * FLY_ROAD_PYLON_NEAR. Which is what a line looks like from an aeroplane: the
 * towers march off into the distance and the wires between them go first.
 *
 * Every road carries one, on one side for its whole length, because that is
 * what a grid is — a road with a line beside it and the next road without one
 * is not two kinds of country, it is a line somebody forgot to draw. */

/* How far apart the masts stand, and how far off the centreline.
 *
 * Two hundred and twenty metres is a lattice tower's span rather than a pole's
 * — a pole line is a mast every eighty and reads as a fence from the air —
 * and it is deliberately not a multiple of the lamp pitch: a tower landing on
 * the same chainage as a column every third span is a rhythm the eye picks out
 * of a moving frame.
 *
 * The offset clears the right of way (FLY_ROAD_ROW) by more than the lower
 * arm reaches, so no conductor hangs over a carriageway and no tower stands in
 * the corridor a settlement's architecture is already kept out of. */
#define FLY_ROAD_PYLON_PITCH 220.0f
#define FLY_ROAD_PYLON_OFF (FLY_ROAD_ROW + 12.0f)
/* How far a mast is drawn, and how far the wires between them are. See the
 * note above: these are one decision about what a line costs at range, and the
 * gap between them is the band where the towers carry the line on their own. */
#define FLY_ROAD_PYLON_NEAR 3400.0f
#define FLY_ROAD_WIRE_FAR 1600.0f
/* The thickest member in the lattice: a leg. It is the constant the tower's
 * two builds are chosen between on — see pylon_fine. */
#define FLY_PYLON_LEG 0.24f
/* The tower itself, in its own frame: the spread of the feet, the waist the
 * legs splay from, the two arm levels and the earth peak. Written down once
 * because the conductors hang off the arms and the arms off the body, and one
 * of them moving alone is a wire fastened to nothing. */
#define FLY_PYLON_FOOT 3.10f
#define FLY_PYLON_WAIST 9.5f
#define FLY_PYLON_ARM_LO 15.5f
#define FLY_PYLON_ARM_HI 20.0f
#define FLY_PYLON_PEAK 25.0f

/* The five conductors a tower carries, where they are fastened, in the mast's
 * own frame: x along the line, y across it, z off its base. Two on the lower
 * arm, two on the upper, and the earth wire on the peak.
 *
 * One table, because the drawing hangs an insulator off exactly these points
 * and the span hangs a catenary off them: two opinions about where a wire is
 * fastened is a wire that misses its own tower. The sag beside it is the drop
 * at mid-span, and the earth wire's is smaller because it is strung tighter —
 * which is what puts it above the phases in the middle of a span as well as at
 * the tower, where the whole point of it is that it is the highest thing. */
#define FLY_PYLON_WIRES 5
static const fly_v3 g_pylon_hang[FLY_PYLON_WIRES] = {
    { 0.0f, -5.20f, FLY_PYLON_ARM_LO - 1.5f },
    { 0.0f,  5.20f, FLY_PYLON_ARM_LO - 1.5f },
    { 0.0f, -3.90f, FLY_PYLON_ARM_HI - 1.5f },
    { 0.0f,  3.90f, FLY_PYLON_ARM_HI - 1.5f },
    { 0.0f,  0.00f, FLY_PYLON_PEAK }
};
static const float g_pylon_sag[FLY_PYLON_WIRES] = { 4.6f, 4.6f, 4.2f, 4.2f, 2.6f };

/* The world radius a thin member has to be drawn at to survive the pixel grid:
 * about an output pixel at this depth. The guideway's barrel takes the same
 * widening for the same reason (see draw_rail) — under it the triangles fall
 * between pixel centres and what should be a thin continuous line becomes a
 * dashed one that crawls as the camera moves. */
static float pixel_floor(const fly__view *v, float dist) {
    float sy = v->sy > 1e-4f ? v->sy : 1e-4f;
    return (v->ortho ? 1.0f : dist) * (float)v->aa / sy * 0.5f;
}

/* Which build of a tower this frame gets, and it is a question about pixels
 * rather than about metres: the lattice is worth drawing exactly as long as its
 * members are wider than the pixel grid can hold. Past that every one of them
 * would be held at the floor, and four legs and their bracing each drawn a
 * pixel wide is a bar of ink several times wider than the tower it stands for
 * — where one column of boxes at the same floor is the honest silhouette, and
 * a sixth of the triangles.
 *
 * On projected size and never on distance, which is the one lesson the ground
 * detail has already paid for (see detail_amount, and the dead end it is
 * written against): the same member is half a pixel at 320 wide and three at
 * 1920, so a range tuned at one resolution strips the other bare. */
static int pylon_fine(const fly__view *v, float dist) {
    return pixel_floor(v, dist) < FLY_PYLON_LEG;
}

/* Where wire `w` is fastened on the mast standing at `base` and heading `yaw`. */
static fly_v3 pylon_hang(fly_v3 base, float yaw, int w) {
    fly_v3 h = g_pylon_hang[w];
    return yawp(base, h.x, h.y, h.z, cosf(yaw), sinf(yaw));
}

/* One member of the lattice: a three-sided tube between two points, which is
 * the cheapest thing in the kit that can lean. A leg splays to its own footing
 * and a brace runs across a panel diagonally, and neither of those is a box. */
static void pylon_member(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         fly_v3 a, fly_v3 b, float r, float floor_r, fly_v3 col) {
    fly_v3 p[2];
    float rr[2];
    if (fly_v3dist(a, b) < 0.05f) return;
    p[0] = a;
    p[1] = b;
    /* A belt is a third of a leg, so the two go sub-pixel at different ranges
       and only the floor keeps the thin ones from dashing out of a tower whose
       legs are still solid. */
    rr[0] = rr[1] = r > floor_r ? r : floor_r;
    draw_pipeline(rt, v, e, p, rr, 2, 3, col);
}

/* One span of conductor: the parabola a hung wire actually takes between two
 * fastenings, which at these sag-to-span ratios is the catenary to within a
 * few centimetres and is one multiply instead of a cosh.
 *
 * Sampled at seven points, which is what it takes for the curve to read as a
 * curve rather than as a bent stick at the bottom of the sag, and drawn as a
 * three-sided tube held at the pixel floor. `sag` is the drop at mid-span and
 * it is the caller's because a span is not always one pitch: a line skips the
 * mast it may not build in a river and the wire crosses the gap instead, and a
 * span twice as long sags four times as far — the parabola's own scaling, so a
 * crossing hangs like a crossing. */
static void pylon_wire(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       fly_v3 a, fly_v3 b, float sag, float dist) {
    enum { N = 7 };
    fly_v3 p[N];
    float r[N], rad = 0.055f, floor_r = pixel_floor(v, dist);
    int i;
    if (floor_r > rad) rad = floor_r;
    for (i = 0; i < N; ++i) {
        float t = (float)i / (float)(N - 1);
        p[i] = fly_v3lerp(a, b, t);
        p[i].z -= sag * 4.0f * t * (1.0f - t);
        r[i] = rad;
    }
    draw_pipeline(rt, v, e, p, r, N, 3, fly_v3mk(0.12f, 0.12f, 0.13f));
}

/* A tower.
 *
 * Two builds of the same envelope, chosen by `pylon_fine`. While the members
 * are wider than a pixel it is the lattice: four legs off four separate
 * footings, the panels between them belted and braced, two arms across the line
 * and an insulator string hanging off each tip. Once they are not it is the
 * envelope itself — a tapered stack of boxes and the two arms — for the reason
 * written over pylon_fine.
 *
 * The legs are the reason a tower can stand anywhere the road goes. Each is
 * seated on its own footing rather than on one height taken at the centre: on
 * a break of slope the difference across a six-metre base is metres, and a
 * tower on one number has a leg in the air on one side and buried on the
 * other. It is the rule `fly_steading_at` learned the same way. */
static void draw_pylon(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g, fly_v3 base, float yaw, float dist) {
    fly_v3 steel = fly_v3mk(0.44f, 0.45f, 0.47f);
    fly_v3 shade = fly_v3scale(steel, 0.82f);
    fly_v3 glass = fly_v3mk(0.70f, 0.72f, 0.74f);
    float cy = cosf(yaw), sy = sinf(yaw);
    float floor_r = pixel_floor(v, dist);
    int q, w;
    fly_v3 foot[4], waist[4], lo[4], hi[4];
    fly_v3 peak = yawp(base, 0.0f, 0.0f, FLY_PYLON_PEAK, cy, sy);
    if (!pylon_fine(v, dist)) {
        /* The envelope: one column of boxes, each no narrower than the floor,
           and the two arms across it. Stacked on one axis, so what the floor
           widens is the column's own ink rather than four members' worth.
           One height tap rather than the lattice's four: the base carries down
           to the ground under the middle of the tower, which on the slopes a
           column of boxes is ever seen from is the whole of the difference. */
        static const float lvl[4] = { 0.0f, FLY_PYLON_WAIST, FLY_PYLON_ARM_LO,
                                      FLY_PYLON_ARM_HI };
        static const float top[4] = { FLY_PYLON_WAIST, FLY_PYLON_ARM_LO,
                                      FLY_PYLON_ARM_HI, FLY_PYLON_PEAK };
        static const float half[4] = { 1.55f, 0.86f, 0.66f, 0.42f };
        float sink = drawn_ground(&g->world, base.x, base.y) - base.z - 0.4f;
        if (sink > 0.0f) sink = 0.0f;
        for (q = 0; q < 4; ++q) {
            float hw = half[q] > floor_r ? half[q] : floor_r;
            float z0 = q ? lvl[q] : sink;
            draw_box2(rt, v, e, yawp(base, 0.0f, 0.0f, z0, cy, sy), hw, hw,
                      top[q] - z0, yaw, steel, shade, 0, 0);
        }
        for (q = 0; q < 2; ++q) {
            float az = q ? FLY_PYLON_ARM_HI : FLY_PYLON_ARM_LO;
            float ah = q ? 3.90f : 5.20f;
            float th = FLY_PYLON_LEG > floor_r ? FLY_PYLON_LEG : floor_r;
            draw_box2(rt, v, e, yawp(base, 0.0f, 0.0f, az - th, cy, sy), th, ah, th,
                      yaw, steel, shade, 0, 0);
        }
        return;
    }
    /* The lattice. The four corners at each level first, and the ground each
       leg meets; then the legs off their own footings, the uprights carrying
       the two arm levels and the peak, and the belts and braces that close
       each panel — which are what make the thing read as a frame rather than
       as four sticks in a bundle. */
    for (q = 0; q < 4; ++q) {
        float ex = (q & 1) ? FLY_PYLON_FOOT : -FLY_PYLON_FOOT;
        float ey = (q & 2) ? FLY_PYLON_FOOT : -FLY_PYLON_FOOT;
        foot[q] = yawp(base, ex, ey, 0.0f, cy, sy);
        foot[q].z = drawn_ground(&g->world, foot[q].x, foot[q].y) - 0.15f;
        waist[q] = yawp(base, ex * 0.40f, ey * 0.40f, FLY_PYLON_WAIST, cy, sy);
        lo[q] = yawp(base, ex * 0.33f, ey * 0.33f, FLY_PYLON_ARM_LO, cy, sy);
        hi[q] = yawp(base, ex * 0.25f, ey * 0.25f, FLY_PYLON_ARM_HI, cy, sy);
    }
    for (q = 0; q < 4; ++q) {
        pylon_member(rt, v, e, foot[q], waist[q], FLY_PYLON_LEG, floor_r, steel);
        pylon_member(rt, v, e, waist[q], lo[q], 0.17f, floor_r, steel);
        pylon_member(rt, v, e, lo[q], hi[q], 0.14f, floor_r, steel);
        pylon_member(rt, v, e, hi[q], peak, 0.11f, floor_r, steel);
    }
    for (q = 0; q < 4; ++q) {
        int n = (q + 1) & 3;
        /* the two faces that share a corner, belted at each level */
        pylon_member(rt, v, e, waist[q], waist[n], 0.10f, floor_r, shade);
        pylon_member(rt, v, e, lo[q], lo[n], 0.10f, floor_r, shade);
        pylon_member(rt, v, e, hi[q], hi[n], 0.09f, floor_r, shade);
        /* and one diagonal per panel per face, alternating which way it leans
           so the bracing zigzags up the tower the way a real one does */
        pylon_member(rt, v, e, (q & 1) ? foot[q] : foot[n],
                     (q & 1) ? waist[n] : waist[q], 0.09f, floor_r, shade);
        pylon_member(rt, v, e, (q & 1) ? waist[n] : waist[q],
                     (q & 1) ? lo[q] : lo[n], 0.08f, floor_r, shade);
        pylon_member(rt, v, e, (q & 1) ? lo[q] : lo[n],
                     (q & 1) ? hi[n] : hi[q], 0.07f, floor_r, shade);
    }
    /* The arms, and the stays that hold them up. An arm cantilevered off a
       body with nothing above it is a plank nailed to a post. */
    for (q = 0; q < 2; ++q) {
        float az = q ? FLY_PYLON_ARM_HI : FLY_PYLON_ARM_LO;
        float ah = q ? 3.90f : 5.20f;
        int s;
        pylon_member(rt, v, e, yawp(base, 0.0f, -ah, az, cy, sy),
                     yawp(base, 0.0f, ah, az, cy, sy), 0.15f, floor_r, steel);
        for (s = 0; s < 2; ++s) {
            float side = s ? ah : -ah;
            pylon_member(rt, v, e, yawp(base, 0.0f, side, az, cy, sy),
                         yawp(base, 0.0f, side * 0.20f, az + 3.2f, cy, sy), 0.08f,
                         floor_r, shade);
        }
    }
    /* The insulator strings. Four of them, because the earth wire on the peak
       is earthed to the steel and hangs off nothing. Pale, and they are the one
       part of a tower that is not steel-coloured. */
    for (w = 0; w < FLY_PYLON_WIRES - 1; ++w) {
        fly_v3 h = pylon_hang(base, yaw, w);
        fly_v3 arm = fly_v3mk(h.x, h.y, h.z + 1.5f);
        pylon_member(rt, v, e, arm, h, 0.17f, floor_r, glass);
    }
}

/* Where the k-th mast of the line beside `rd` stands, which way it faces, and
 * whether one is installed there at all.
 *
 * By chainage from the start of the road, the way the lamps are (see
 * road_lamp_at) and for the same reason: the pitch is 220 m of carriageway
 * wherever the survey happened to put its stations, so a tower never crawls
 * along the verge as the camera moves. One side for the whole road, drawn from
 * the road's own termini rather than from its index, so the line is a property
 * of the world and not of the order the network was laid in — a line that
 * changed sides halfway along is two lines meeting.
 *
 * The base is the *highest* of the four footings and the installation rule
 * asks the lowest, which is the same pair of questions `fly_steading_at` asks
 * of its yard: nothing standing in water, and nothing left in the air.
 *
 * `stands` is 0 where the placement lands somewhere a mast may not be built —
 * in water, inside a settlement's own clearing, or inside the structure
 * carrying the guideway over the road. That is a hole in the chain rather than
 * a tower shoved sideways, which is the answer `fly_site_footing` gives too,
 * and the span either side of it simply gets longer. Returns 0 past the end of
 * the road, so a caller walks k upward until it stops. */
static int road_pylon_at(const fly_game *g, const fly_road *rd, int k, fly_v3 *base,
                         float *yaw, int *stands, float *chain_out) {
    float chain = FLY_ROAD_PYLON_PITCH * 0.5f + (float)k * FLY_ROAD_PYLON_PITCH;
    fly_v3 c, tan;
    float len, nx, ny, ux, uy, side, x, y, top = -1e30f;
    int lo = 0, hi = rd->point_count - 2, i, q, dry = 1;
    if (k < 0 || rd->point_count < 2 || chain > rd->length) return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (rd->points[mid].distance <= chain) lo = mid; else hi = mid - 1;
    }
    i = lo;
    {   float d0 = rd->points[i].distance, d1 = rd->points[i + 1].distance;
        if (d1 - d0 < 1e-3f) return 0;
        if (!fly_road_span(rd, i, (chain - d0) / (d1 - d0), &c, &tan)) return 0; }
    len = hypotf(tan.x, tan.y);
    if (len < 1e-4f) return 0;
    ux = tan.x / len;
    uy = tan.y / len;
    nx = -uy;
    ny = ux;
    side = (fly_hash2(g->world.seed + 47u, rd->from, rd->to) & 1u) ? 1.0f : -1.0f;
    x = c.x + nx * side * FLY_ROAD_PYLON_OFF;
    y = c.y + ny * side * FLY_ROAD_PYLON_OFF;
    for (q = 0; q < 4; ++q) {
        float ex = (q & 1) ? FLY_PYLON_FOOT : -FLY_PYLON_FOOT;
        float ey = (q & 2) ? FLY_PYLON_FOOT : -FLY_PYLON_FOOT;
        float fx = x + ex * ux - ey * uy, fy = y + ex * uy + ey * ux;
        float fz = drawn_ground(&g->world, fx, fy);
        if (fz > top) top = fz;
        if (fly_world_wet(&g->world, fx, fy)) dry = 0;
    }
    if (base) *base = fly_v3mk(x, y, top);
    /* Square to the line, so the arms reach across it and the conductors run
       along it. */
    if (yaw) *yaw = atan2f(uy, ux);
    if (chain_out) *chain_out = chain;
    if (stands)
        *stands = dry && clear_of_sites(&g->world, x, y, FLY_STEADING_KEEPOUT) &&
                  !rail_portal_inside(g, x, y, FLY_ROAD_PYLON_OFF) &&
                  /* And out of every road's right of way, which the offset
                     cannot answer for on its own: the run is set out from its
                     own road and a junction puts another one under it. Asked
                     through the network's own predicate rather than by
                     distance, so a mast is held to the rule every piece of
                     settlement architecture is held to — which also stops the
                     run inside the wider apron at a terminus, where the road
                     arrives through an opening kept clear for it. */
                  !fly_road_blocked(&g->roads, fly_wpos_mk(x, y));
    return 1;
}

/* The next mast forward that actually stands, within `max` pitches, and how
 * many pitches away it is — 0 for none, which is where a run ends. */
static int road_pylon_next(const fly_game *g, const fly_road *rd, int k, int max,
                           fly_v3 *pos, float *yaw) {
    int j;
    for (j = 1; j <= max; ++j) {
        int stands;
        if (!road_pylon_at(g, rd, k + j, pos, yaw, &stands, NULL)) return 0;
        if (stands) return j;
    }
    return 0;
}

/* The masts whose chainage falls inside span `i`, and the wire out of each.
 *
 * Half-open on the span the same way the lamps are, so a mast landing exactly
 * on a station belongs to the span after it and not to both. The chain thins
 * on the same power-of-two ladder (`lamp_stride`) and for the same reason —
 * and the wires are unaffected by it, because they are given up at
 * FLY_ROAD_WIRE_FAR and the ladder's first rung is well past that: a span
 * whose wires are drawn is a span with every one of its masts still in. */
static void road_pylons(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        const fly_game *g, const fly_road *rd, int i, float dist) {
    float d0 = rd->points[i].distance, d1 = rd->points[i + 1].distance;
    int k, k0, k1, stride = lamp_stride(dist);
    if (d1 - d0 < 1e-3f) return;
    k0 = (int)ceilf((d0 - FLY_ROAD_PYLON_PITCH * 0.5f) / FLY_ROAD_PYLON_PITCH);
    k1 = (int)ceilf((d1 - FLY_ROAD_PYLON_PITCH * 0.5f) / FLY_ROAD_PYLON_PITCH) - 1;
    if (k0 < 0) k0 = 0;
    for (k = k0; k <= k1; ++k) {
        fly_v3 base, next;
        float yaw, nyaw, md;
        int stands, span, w;
        if (stride > 1 && (k % stride)) continue;
        if (!road_pylon_at(g, rd, k, &base, &yaw, &stands, NULL)) continue;
        if (!stands) continue;
        md = fly_v3dist(base, v->pos);
        if (md > FLY_ROAD_PYLON_NEAR) continue;
        if (object_worth_drawing(v, rt, fly_v3add(base, fly_v3mk(0, 0, FLY_PYLON_PEAK * 0.5f)),
                                 FLY_PYLON_PEAK))
            draw_pylon(rt, v, e, g, base, yaw, md);
        if (md > FLY_ROAD_WIRE_FAR) continue;
        /* Three pitches of reach, which is the longest gap the placement can
           leave: FLY_ROAD_FORD is 300 m of water and a crossing's structure is
           narrower than that. */
        span = road_pylon_next(g, rd, k, 3, &next, &nyaw);
        if (!span) continue;
        /* The span is culled on its own bounds and not on its near tower's: a
           wire is two hundred metres of geometry hung off a twelve-metre
           object, so a sphere round the mast leaves the middle of every span
           at the edge of the frame undrawn — which is a line that comes apart
           exactly where the camera is panning. */
        {   fly_v3 mid = fly_v3scale(fly_v3add(base, next), 0.5f);
            mid.z += FLY_PYLON_ARM_LO;
            if (!object_worth_drawing(v, rt, mid,
                                      fly_v3dist(base, next) * 0.5f + FLY_PYLON_PEAK))
                continue; }
        for (w = 0; w < FLY_PYLON_WIRES; ++w)
            pylon_wire(rt, v, e, pylon_hang(base, yaw, w), pylon_hang(next, nyaw, w),
                       g_pylon_sag[w] * (float)(span * span), md);
    }
}

static void draw_roads(fly_render_target *rt, const fly__view *v, const fly__env *e,
                       const fly_game *g) {
    fly_v3 metal = fly_v3mk(0.145f, 0.145f, 0.15f);
    fly_v3 wall = fly_v3mk(0.27f, 0.245f, 0.20f);   /* the made edge: tipped fill */
    fly_v3 paint = fly_v3mk(0.62f, 0.60f, 0.50f);
    float dark = 1.0f - fly_smoothstepf(0.16f, 0.46f, e->day);
    int r, i;
    if (g_shadow_cast) return;
    for (r = 0; r < g->roads.count; ++r) {
        const fly_road *rd = &g->roads.road[r];
        /* The whole road first, by its own bounds. Two dozen roads of a
           hundred spans each is two thousand distance tests per frame to
           discover that most of them are on the other side of the map; the
           bounds answer that for one test apiece. */
        if (v->pos.x < rd->lo.x - 26000.0f || v->pos.x > rd->hi.x + 26000.0f) continue;
        if (v->pos.y < rd->lo.y - 26000.0f || v->pos.y > rd->hi.y + 26000.0f) continue;
        for (i = 0; i + 1 < rd->point_count; ++i) {
            fly_v3 a = rd->points[i].pos, b = rd->points[i + 1].pos;
            fly_v3 mid = fly_v3scale(fly_v3add(a, b), 0.5f);
            float len = hypotf(b.x - a.x, b.y - a.y), dist = fly_v3dist(mid, v->pos);
            int steps, bank, across, s;
            road_xs prev, cur;
            if (len < 1.0f || dist > 26000.0f) continue;
            /* the arc bulges off the chord, so the sphere the cull tests has to
               be the span's own reach plus room for the swing and the banks */
            if (!object_worth_drawing(v, rt, mid, len * 0.5f + 30.0f)) continue;
            steps = road_steps(dist);
            /* The kerb and the bank, close in only: at two kilometres the deck
               is four pixels wide and its edge is inside them. */
            bank = dist < 2600.0f;
            /* and the crown, while the road is wide enough to have one */
            across = dist < 1800.0f ? 2 : 1;
            road_section(g, v, rd, i, 0.0f, bank, &prev);
            for (s = 0; s < steps; ++s) {
                int k;
                road_section(g, v, rd, i, (float)(s + 1) / (float)steps, bank, &cur);
                for (k = 0; k < across; ++k) {
                    float u0 = -1.0f + 2.0f * (float)k / (float)across;
                    float u1 = -1.0f + 2.0f * (float)(k + 1) / (float)across;
                    fly_v3 p0 = road_deck(&prev, u0, 0.0f), p1 = road_deck(&prev, u1, 0.0f);
                    fly_v3 p2 = road_deck(&cur, u1, 0.0f), p3 = road_deck(&cur, u0, 0.0f);
                    /* The carriageway is paving, and it gets what paving gets:
                     * the lands it was laid in, the aggregate showing through,
                     * and — the half that matters most, since half the frames
                     * in this world have weather in them — a surface that goes
                     * dark and mirrors the sky when it is wet. A dry-looking
                     * road under falling rain is the loudest thing in a rainy
                     * frame. */
                    tri_paved(rt, v, e, p0, p1, p2, metal, 0.94f, 1.0f);
                    tri_paved(rt, v, e, p0, p2, p3, metal, 0.94f, 1.0f);
                }
                /* --- the shoulders --------------------------------------
                 *
                 * A carriageway that meets grass at its own kerb line is a
                 * ribbon laid on a field. What a road actually has beside it is
                 * a made-up verge — the graded gravel the machines ran on,
                 * which is where the drainage is and where anything that stops
                 * pulls over. It is also the piece that makes the road look
                 * *built*, because it is the only part of the cross-section
                 * that is neither the surface nor the landscape.
                 *
                 * Drawn only where the kerb is (see `bank`), for the same
                 * reason: at two kilometres the whole deck is four pixels and
                 * a metre of gravel each side is a quarter of one.
                 *
                 * Laid *inside* the formation, over the outer fifth of the
                 * deck, rather than as a flange hanging off the kerb: the deck
                 * stands on the highest ground under its own width, so anything
                 * built outboard of the kerb line is over whatever the hillside
                 * is doing there and would float above it half the time. A
                 * strip on the slab is a hard shoulder, which is the same thing
                 * and cannot come off the ground. */
                if (bank) {
                    static const fly_v3 grav = { 0.26f, 0.24f, 0.21f };
                    int side;
                    for (side = 0; side < 2; ++side) {
                        float ui = side ? 0.80f : -0.80f, uo = side ? 1.0f : -1.0f;
                        fly_v3 i0 = road_deck(&prev, ui, 0.02f), i1 = road_deck(&cur, ui, 0.02f);
                        fly_v3 o0 = road_deck(&prev, uo, 0.02f), o1 = road_deck(&cur, uo, 0.02f);
                        tri_paved(rt, v, e, i0, i1, o1, grav, 0.90f, 1.0f);
                        tri_paved(rt, v, e, i0, o1, o0, grav, 0.90f, 1.0f);
                    }
                }
                /* The edge, and it is the point of the whole thing: the deck
                 * stands on the highest ground under its own width, so on a
                 * slope one side of it is a wall. Drawn as a skirt down to the
                 * ground under each edge and never shorter than the slab, so a
                 * road on the flat still has a kerb and a road on a hillside
                 * has the retaining wall that put it there. */
                if (bank) {
                    fly_v3 l0 = road_deck(&prev, -1.0f, 0.0f), l1 = road_deck(&cur, -1.0f, 0.0f);
                    fly_v3 r0 = road_deck(&prev, 1.0f, 0.0f), r1 = road_deck(&cur, 1.0f, 0.0f);
                    wall_quad(rt, v, e, prev.toe[1], cur.toe[1], r1, r0, wall, 0.95f);
                    wall_quad(rt, v, e, cur.toe[0], prev.toe[0], l0, l1, wall, 0.95f);
                }
                /* Two lanes, said the way a road says it. Painted only where it
                   can be seen: a centre line at a kilometre is a sub-pixel
                   stripe on a four-pixel road and reads as noise on the
                   surface. Laid on the crown, which is where the line between
                   two carriageways goes. */
                if (dist < 900.0f) {
                    float w0 = 0.36f / (prev.half > 0.1f ? prev.half : 1.0f);
                    float w1 = 0.36f / (cur.half > 0.1f ? cur.half : 1.0f);
                    fly_v3 c0 = road_deck(&prev, -w0, 0.03f), c1 = road_deck(&prev, w0, 0.03f);
                    fly_v3 c2 = road_deck(&cur, w1, 0.03f), c3 = road_deck(&cur, -w1, 0.03f);
                    tri_shaded(rt, v, e, c0, c1, c2, paint, 0.96f);
                    tri_shaded(rt, v, e, c0, c2, c3, paint, 0.96f);
                }
                prev = cur;
            }
            if (dist < (dark > 0.02f ? FLY_ROAD_LAMP_FAR : FLY_ROAD_LAMP_NEAR + 100.0f))
                road_lamps(rt, v, e, g, rd, i, dist, dark);
            /* And the line beside it. A pitch further than the towers are
               drawn at, because the span's midpoint is what `dist` measures
               and a mast at either end of it is half a span nearer. */
            if (dist < FLY_ROAD_PYLON_NEAR + FLY_ROAD_PYLON_PITCH)
                road_pylons(rt, v, e, g, rd, i, dist);
        }
        /* Both termini, close in. Every road has two, and neither of them is a
           settlement's business — a road ends where it ends whether anybody
           has discovered the place it serves or not. */
        if (rd->point_count >= 2) {
            static const int end[2][2] = { { 0, 1 }, { -1, -2 } };
            int k;
            for (k = 0; k < 2; ++k) {
                int ti = end[k][0] < 0 ? rd->point_count + end[k][0] : end[k][0];
                int bi = end[k][1] < 0 ? rd->point_count + end[k][1] : end[k][1];
                fly_v3 tip = rd->points[ti].pos, back = rd->points[bi].pos;
                float dx = tip.x - back.x, dy = tip.y - back.y, len = hypotf(dx, dy);
                float bed;
                if (len < 1.0f) continue;
                if (fly_v3dist(tip, v->pos) > 2600.0f) continue;
                if (!object_worth_drawing(v, rt, tip, 30.0f)) continue;
                /* The ramp starts where the deck ends, and the deck may have
                   been lifted to clear the ground — see road_bed. Taking the
                   surveyed tip instead puts a step across the one piece of road
                   every arrival on foot walks up. */
                bed = road_bed(&g->world, tip, -dy / len, dx / len, FLY_ROAD_HALF);
                if (tip.z < bed) tip.z = bed;
                road_ramp(rt, v, e, g, tip, dx / len, dy / len, FLY_ROAD_HALF);
            }
        }
    }
}

/* One vehicle of a column. The cab is the yard truck every settlement already
 * has; what a tier adds is what it is carrying and what it is carrying it in —
 * a sheeted load, then plate over the load, then a mount on the roof of the
 * lead vehicle. The silhouette is the tier, which is the point: what is coming
 * up the road should be readable before the guns are. */
static void convoy_vehicle(fly_render_target *rt, const fly__view *v, const fly__env *e,
                           fly_v3 p, float yaw, int tier, int lead,
                           fly_v3 col, int lit) {
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 load = fly_v3scale(col, 0.66f);
    fly_v3 plate = fly_v3mk(0.26f, 0.26f, 0.27f);
    fly_v3 rope = fly_v3mk(0.20f, 0.19f, 0.17f);
    /* The deck the load stands on, and the top of the cab the mount stands on.
       Written down once rather than as four literals, because the two of them
       are prop_truck's business and a tier that disagreed with it by ten
       centimetres put a gun mount hovering over a lorry. */
    const float deck = 1.20f, roof = 2.56f;
    float dist = fly_v3dist(p, v->pos);
    int mid = dist < 420.0f;
    float lh = 0.95f + 0.22f * (float)tier;
    int k;
    prop_truck(rt, v, e, p, yaw, col, lit);
    if (tier >= 1) {
        draw_box2(rt, v, e, yawp(p, -0.4f, 0, deck, cy, sy), 1.35f, 0.95f, lh, yaw,
                  load, fly_v3scale(load, 0.7f), 0, 0);
        /* The bows the sheet is stretched over, and the ropes across it. A
           sheeted load without them is a crate; with them it is cargo somebody
           tied down, and it is three boxes. */
        if (mid) {
            for (k = -1; k <= 1; ++k)
                draw_box2(rt, v, e, yawp(p, -0.4f + (float)k * 0.92f, 0, deck + lh - 0.06f,
                                         cy, sy), 0.07f, 0.98f, 0.10f, yaw, rope,
                          fly_v3scale(rope, 1.2f), 0, 0);
            draw_box2(rt, v, e, yawp(p, -0.4f, 0, deck + lh * 0.45f, cy, sy),
                      1.38f, 0.98f, 0.06f, yaw, rope, fly_v3scale(rope, 1.2f), 0, 0);
        }
    }
    if (tier >= 2) {
        draw_box2(rt, v, e, yawp(p, -0.4f, 0, deck - 0.02f, cy, sy), 1.5f, 1.06f, 0.28f,
                  yaw, plate, fly_v3scale(plate, 1.15f), 0, 0);
        /* corner posts, so the plate reads as bolted on rather than painted */
        if (mid)
            for (k = 0; k < 4; ++k)
                draw_box2(rt, v, e, yawp(p, -0.4f + ((k & 1) ? 1.44f : -1.44f),
                                         (k & 2) ? 1.02f : -1.02f, deck - 0.02f, cy, sy),
                          0.09f, 0.09f, 0.34f, yaw, fly_v3scale(plate, 0.7f), plate, 0, 0);
        if (lead) {   /* the mount that answers, on the roof of the first one */
            draw_box2(rt, v, e, yawp(p, 1.5f, 0, roof, cy, sy), 0.5f, 0.5f, 0.42f, yaw,
                      plate, fly_v3scale(plate, 1.2f), 0, 0);
            draw_box2(rt, v, e, yawp(p, 2.3f, 0, roof + 0.31f, cy, sy), 0.9f, 0.09f, 0.14f,
                      yaw, fly_v3mk(0.14f, 0.14f, 0.15f), fly_v3mk(0.18f, 0.18f, 0.19f), 0, 0);
            if (mid) {   /* a shield in front of whoever is behind it */
                draw_box2(rt, v, e, yawp(p, 1.86f, 0, roof + 0.18f, cy, sy), 0.07f, 0.46f,
                          0.40f, yaw, fly_v3scale(plate, 0.85f), plate, 0, 0);
                draw_box2(rt, v, e, yawp(p, 1.5f, 0, roof - 0.10f, cy, sy), 0.62f, 0.62f,
                          0.10f, yaw, fly_v3scale(plate, 0.7f), plate, 0, 0);
            }
        }
    }
}

/* --- the shore, where it is worked ------------------------------------------
 *
 * A quay is the one piece of architecture in this world that stands in the
 * water on purpose, and it is built out of pieces that already existed: the
 * rail's pier is the pile under it, the yard's gantry is the crane on it and
 * the yard's containers are what is stacked beside that. What is new is the
 * deck between them and the fact that all of it is laid along one bearing —
 * fly_sea's quay, root to head — so a harbour is a line running out from the
 * land rather than a scatter of things near water.
 *
 * The piles are what make it read as built. A deck sitting on the sea with
 * nothing under it is a jetty drawn by somebody who had not looked at one, and
 * what is under this one is `draw_pier` — the guideway's own bent, footing pad,
 * shaft and crosshead, which is exactly what carries a deck over water and was
 * already written for carrying one. A bay every twenty-two metres, and each of
 * them takes its bents down to whatever the drawn ground is beneath. */
#define FLY_QUAY_BAY 22.0f
#define FLY_QUAY_DRAW_R 9000.0f
#define FLY_BUOY_DRAW_R 6000.0f

static void draw_quay(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      const fly_game *g, const fly_quay *q) {
    float dx = q->head.x - q->root.x, dy = q->head.y - q->root.y;
    float run = hypotf(dx, dy), yaw = atan2f(dy, dx);
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 root = q->root, head = q->head;
    fly_v3 conc = fly_v3mk(0.46f, 0.45f, 0.43f);
    fly_v3 pile = fly_v3mk(0.38f, 0.37f, 0.35f);
    float deck_z = FLY_WATER_LEVEL + FLY_QUAY_DECK;
    int bays, k, detail;
    if (run < 8.0f) return;
    if (!object_worth_drawing(v, rt, head, run * 0.5f + 30.0f)) return;
    detail = fly_v3dist(head, v->pos) < 2600.0f ? 2 : 1;
    bays = (int)(run / FLY_QUAY_BAY) + 1;
    if (bays > 40) bays = 40;
    /* The deck, in bays rather than as one slab: a 400 m quad is a quad the
       rasterizer shades with one normal and one distance, and the whole point
       of the thing is that it recedes. */
    for (k = 0; k < bays; ++k) {
        float u0 = (float)k / (float)bays, u1 = (float)(k + 1) / (float)bays;
        float mid = (u0 + u1) * 0.5f;
        fly_v3 c = fly_v3mk(fly_lerpf(root.x, head.x, mid), fly_lerpf(root.y, head.y, mid),
                            deck_z);
        draw_box2(rt, v, e, c, run / (float)bays * 0.5f, FLY_QUAY_HALF, 0.9f, yaw,
                  fly_v3scale(conc, 0.86f), conc, 0, 0);
        /* the bents under it, both sides, down to whatever is beneath — on the
           ground the mesh draws rather than the ground the height function
           reports, which is the rule the guideway's piers follow and for the
           same reason: a footing seated on the second is a footing with its
           top edge under the sand */
        {   int side;
            for (side = -1; side <= 1; side += 2) {
                fly_v3 f = yawp(c, 0.0f, (float)side * (FLY_QUAY_HALF - 1.4f), deck_z,
                                cy, sy);
                draw_pier(rt, v, e, f, drawn_ground(&g->world, f.x, f.y), 0.85f,
                          yaw + FLY_PI * 0.5f, pile);
            }
        }
    }
    /* The head: a crane over the berth, boxes waiting beside it, and the
       bollards a hull ties to. */
    {
        fly_v3 gp = fly_v3mk(fly_lerpf(root.x, head.x, 0.86f),
                             fly_lerpf(root.y, head.y, 0.86f), deck_z + 0.9f);
        /* Its legs stand on the deck rather than out over the water, so the
           span is the deck's half width and not the crane's own preference. */
        prop_gantry(rt, v, e, gp, FLY_QUAY_HALF - 1.6f, 13.0f, yaw + FLY_PI * 0.5f,
                    e->time);
    }
    if (detail >= 2) {
        static const float hue[3][3] = { { 0.32f, 0.16f, 0.12f }, { 0.16f, 0.26f, 0.30f },
                                         { 0.30f, 0.28f, 0.14f } };
        for (k = 0; k < 6; ++k) {
            const float *cc = hue[k % 3];
            float u = 0.36f + (float)(k / 2) * 0.14f;
            fly_v3 c = fly_v3mk(fly_lerpf(root.x, head.x, u), fly_lerpf(root.y, head.y, u),
                                deck_z + 0.9f);
            fly_v3 b = yawp(c, 0.0f, (k & 1) ? 3.4f : -3.4f, 0.0f, cy, sy);
            prop_container(rt, v, e, b, yaw, fly_v3mk(cc[0], cc[1], cc[2]), 0);
        }
        for (k = 0; k < 6; ++k) {
            float u = 0.30f + (float)k * 0.13f;
            fly_v3 c = fly_v3mk(fly_lerpf(root.x, head.x, u), fly_lerpf(root.y, head.y, u),
                                deck_z + 0.9f);
            draw_prism(rt, v, e, yawp(c, 0.0f, FLY_QUAY_HALF - 0.7f, 0.0f, cy, sy),
                       0.34f, 0.75f, 6, fly_v3mk(0.20f, 0.20f, 0.21f),
                       fly_v3mk(0.26f, 0.26f, 0.27f));
        }
    }
    /* Lamps along it, on the same argument as the road's: a working quay after
       dark is a lit one, and a jetty with no light on it is a place nothing
       ties up at. */
    for (k = 0; k < 4; ++k) {
        float u = 0.22f + (float)k * 0.24f;
        fly_v3 c = fly_v3mk(fly_lerpf(root.x, head.x, u), fly_lerpf(root.y, head.y, u),
                            deck_z + 0.9f);
        prop_lamp(rt, v, e, yawp(c, 0.0f, -(FLY_QUAY_HALF - 1.1f), 0.0f, cy, sy),
                  yaw + FLY_PI * 0.5f);
    }
    /* And the landward end: the deck has to arrive at ground rather than stop
       over it, so the last bay is carried back into the bank on fill. */
    {
        float bed = drawn_ground(&g->world, root.x, root.y);
        if (deck_z - bed > 0.4f)
            draw_box2(rt, v, e, fly_v3mk(root.x, root.y, bed), 7.0f, FLY_QUAY_HALF * 0.9f,
                      deck_z - bed, yaw, fly_v3scale(conc, 0.78f), conc, 0, 0);
    }
}

/* A lateral mark: a float, a topmark and, after dark, the light that is the
 * whole reason it is out there. Red to port and green to starboard of the
 * fairway, which is the convention a pair of them exists to state — see
 * fly_sea_buoy, where the sides alternate. */
static void draw_buoy(fly_render_target *rt, const fly__view *v, const fly__env *e,
                      fly_v3 p, int port, double time, int k) {
    fly_v3 col = port ? fly_v3mk(0.52f, 0.10f, 0.09f) : fly_v3mk(0.10f, 0.44f, 0.16f);
    /* It rides the swell rather than standing on it: a mark bolted to a flat
       plane is the one thing on the water that would say the water is not
       moving. Off the shared clock, so the shadow pass and the beauty pass put
       it in the same place. */
    float bob = sinf((float)time * 0.9f + (float)k * 1.7f) * 0.35f;
    fly_v3 base = fly_v3mk(p.x, p.y, FLY_WATER_LEVEL - 0.5f + bob);
    if (!object_worth_drawing(v, rt, base, 4.0f)) return;
    draw_prism(rt, v, e, base, 1.15f, 2.1f, 6, col, fly_v3scale(col, 1.3f));
    draw_box2(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, 2.1f)), 0.16f, 0.16f, 1.6f, 0.0f,
              fly_v3mk(0.24f, 0.24f, 0.24f), fly_v3mk(0.28f, 0.28f, 0.28f), 0, 0);
    draw_prism(rt, v, e, fly_v3add(base, fly_v3mk(0, 0, 3.4f)), 0.55f, 0.7f, 4,
               fly_v3scale(col, 1.15f), fly_v3scale(col, 1.4f));
    {   float dark = fly_clampf(lamp_dark(e, base), 0.0f, 1.0f);
        /* Occulting, not steady: a mark that is on all night is a lamp, and
           what tells a light on the water apart from a light on the shore is
           that it goes out. */
        float phase = fmodf((float)time * 0.5f + (float)k * 0.31f, 1.0f);
        if (dark > 0.02f && phase > 0.22f)
            draw_light(rt, v, fly_v3add(base, fly_v3mk(0, 0, 3.8f)),
                       port ? FLY_RGBA(255, 60, 50, (int)(220.0f * dark))
                            : FLY_RGBA(60, 255, 100, (int)(220.0f * dark)), 1.0f);
    }
}

/* The whole shore, and what marks the water off it. */
static void draw_harbour(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         const fly_game *g) {
    int i, k;
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_quay *q = fly_sea_quay(&g->lanes, i);
        if (!q) continue;
        /* A quay belongs to a settlement and is only there once the settlement
           is: an undiscovered site does not draw its own architecture and must
           not draw a harbour either. */
        if (!g->world.loc[i].discovered) continue;
        if (fly_v3dist(q->head, v->pos) > FLY_QUAY_DRAW_R) continue;
        draw_quay(rt, v, e, g, q);
    }
    for (i = 0; i < g->lanes.count; ++i) {
        const fly_lane *ln = &g->lanes.lane[i];
        fly_v3 p;
        int port;
        for (k = 0; fly_sea_buoy(ln, k, &p, &port); ++k) {
            if (fly_v3dist(p, v->pos) > FLY_BUOY_DRAW_R) continue;
            draw_buoy(rt, v, e, p, port, e->time, k + i * 31);
        }
    }
}

/* --- and one vessel of a column at sea --------------------------------------
 *
 * The same job as convoy_vehicle and the same rule about it: the silhouette is
 * the tier, because what is coming up the lane should be readable before the
 * guns are. A lighter is a low open tow, a coaster has its house aft and one
 * hold, a freighter carries boxes on deck, and an ore carrier is a long black
 * hull with a mount on it.
 *
 * Everything is built from the deck plane rather than from the waterline, and
 * the waterline is FLY_WATER_LEVEL and nothing else: the sea is a plane, a
 * hull floats on it, and a ship whose deck was draped on the bed would be a
 * ship aground. The one thing that reads at every range is the wake — a hull
 * at eight kilometres is a few dark pixels and the white behind it is what
 * says the pixels are moving.
 *
 * `lit` is the whole night silhouette: the masthead and the sidelights are
 * emitters and they are what a ship is at night, since nothing else on the
 * water is lit at all. */
static void convoy_vessel(fly_render_target *rt, const fly__view *v, const fly__env *e,
                          fly_v3 p, float yaw, int tier, int lead,
                          fly_v3 col, int lit) {
    /* Half-length, half-beam, freeboard and the height of the house, by rung.
       A hull is the one thing in this world whose *size* is the tier — a
       lighter is thirty metres and a bulk carrier is ninety — so the table is
       here rather than scaled off one shape. */
    static const float DIM[4][4] = {
        /*  half-len  half-beam  freeboard  house */
        {   15.0f,     4.6f,      1.7f,     2.6f },
        {   26.0f,     6.4f,      3.0f,     6.2f },
        {   34.0f,     8.0f,      3.8f,     7.4f },
        {   44.0f,    10.2f,      4.6f,     8.6f }
    };
    const float *d = DIM[tier < 0 ? 0 : tier > 3 ? 3 : tier];
    float hl = d[0], hb = d[1], fb = d[2], hh = d[3];
    float cy = cosf(yaw), sy = sinf(yaw);
    fly_v3 deck = fly_v3mk(p.x, p.y, FLY_WATER_LEVEL);
    fly_v3 hull = fly_v3scale(col, 0.62f);
    fly_v3 upper = fly_v3mk(0.86f, 0.85f, 0.81f);
    fly_v3 rust = fly_v3mk(0.32f, 0.20f, 0.14f);
    float dist = fly_v3dist(p, v->pos);
    int mid = dist < 2600.0f;
    int k;
    /* The hull: a box for the body and a wedge for the bow. Two triangles buy
       the whole difference between a ship and a crate — a square bow reads as
       a barge from any angle, which is exactly what the bottom rung is. */
    draw_box2(rt, v, e, fly_v3add(deck, fly_v3mk(0, 0, -0.6f)), hl * 0.86f, hb, fb + 0.6f,
              yaw, hull, fly_v3scale(hull, 1.22f), 0, 0);
    if (tier > 0) {
        fly_v3 stem = yawp(deck, hl, 0.0f, fb, cy, sy);
        fly_v3 keel = yawp(deck, hl * 0.86f, 0.0f, -0.6f, cy, sy);
        fly_v3 sh0 = yawp(deck, hl * 0.86f, -hb, fb, cy, sy);
        fly_v3 sh1 = yawp(deck, hl * 0.86f, hb, fb, cy, sy);
        fly_v3 b0 = yawp(deck, hl * 0.86f, -hb, -0.6f, cy, sy);
        fly_v3 b1 = yawp(deck, hl * 0.86f, hb, -0.6f, cy, sy);
        tri_shaded(rt, v, e, stem, sh0, sh1, fly_v3scale(hull, 1.1f), 1.0f);
        tri_shaded(rt, v, e, stem, b0, sh0, hull, 1.0f);
        tri_shaded(rt, v, e, stem, sh1, b1, hull, 1.0f);
        tri_shaded(rt, v, e, stem, keel, b0, fly_v3scale(hull, 0.8f), 1.0f);
        tri_shaded(rt, v, e, stem, b1, keel, fly_v3scale(hull, 0.8f), 1.0f);
    }
    /* The house, aft, and the funnel over it. Windows on, because a lit
       wheelhouse at two in the morning is the only warm thing on the water. */
    if (tier > 0) {
        fly_v3 house = yawp(deck, -hl * 0.58f, 0.0f, fb, cy, sy);
        draw_box2(rt, v, e, house, hb * 0.62f, hb * 0.82f, hh, yaw, upper,
                  fly_v3scale(upper, 0.86f), 1, (uint32_t)(tier * 977u + 13u));
        draw_box2(rt, v, e, yawp(deck, -hl * 0.58f, 0.0f, fb + hh, cy, sy),
                  hb * 0.5f, hb * 0.66f, 1.5f, yaw, upper, fly_v3scale(upper, 0.9f), 0, 0);
        draw_prism(rt, v, e, yawp(deck, -hl * 0.80f, 0.0f, fb, cy, sy),
                   hb * 0.34f, hh * 0.92f, 8, rust, fly_v3mk(0.12f, 0.12f, 0.12f));
    }
    /* What it is carrying, on deck, forward of the house. Hatch coamings on
       the two middle rungs and boxes on the freighter, which is the one rung
       whose load is visible from directly overhead. */
    if (tier == 1 || tier == 3) {
        int n = tier == 1 ? 2 : 4;
        for (k = 0; k < n; ++k) {
            float u = hl * (0.42f - (float)k * (0.62f / (float)n));
            draw_box2(rt, v, e, yawp(deck, u, 0.0f, fb, cy, sy), hl * 0.11f, hb * 0.72f,
                      1.4f, yaw, fly_v3scale(hull, 0.8f), fly_v3mk(0.30f, 0.29f, 0.26f), 0, 0);
        }
    }
    if (tier == 2 && mid) {
        static const float hue[3][3] = { { 0.32f, 0.16f, 0.12f }, { 0.16f, 0.26f, 0.30f },
                                         { 0.30f, 0.28f, 0.14f } };
        for (k = 0; k < 6; ++k) {
            const float *cc = hue[k % 3];
            fly_v3 q = yawp(deck, hl * (0.44f - (float)(k / 2) * 0.30f),
                            (k & 1) ? hb * 0.42f : -hb * 0.42f, fb + 0.2f, cy, sy);
            prop_container(rt, v, e, q, yaw, fly_v3mk(cc[0], cc[1], cc[2]), 0);
        }
    }
    if (tier == 0) {   /* the tow: a low deck with a sheeted load lashed to it */
        draw_box2(rt, v, e, fly_v3add(deck, fly_v3mk(0, 0, fb)), hl * 0.62f, hb * 0.74f,
                  1.9f, yaw, fly_v3scale(col, 0.7f), fly_v3scale(col, 0.5f), 0, 0);
    }
    /* The mast, and the mount on the rungs that have one — the same shape as
       the column's, for the same reason: what answers should look like what
       answers. */
    if (tier > 0)
        draw_box2(rt, v, e, yawp(deck, hl * 0.16f, 0.0f, fb, cy, sy), 0.22f, 0.22f,
                  hh + 4.5f, yaw, fly_v3mk(0.30f, 0.29f, 0.27f),
                  fly_v3mk(0.34f, 0.33f, 0.31f), 0, 0);
    if (tier >= 2 && lead) {
        fly_v3 plate = fly_v3mk(0.26f, 0.26f, 0.27f);
        fly_v3 mount = yawp(deck, hl * 0.66f, 0.0f, fb, cy, sy);
        draw_box2(rt, v, e, mount, 1.5f, 1.5f, 1.2f, yaw, plate, fly_v3scale(plate, 1.2f), 0, 0);
        draw_box2(rt, v, e, yawp(deck, hl * 0.66f + 2.1f, 0.0f, fb + 1.05f, cy, sy),
                  2.2f, 0.16f, 0.24f, yaw, fly_v3mk(0.14f, 0.14f, 0.15f),
                  fly_v3mk(0.18f, 0.18f, 0.19f), 0, 0);
    }
    /* The wake, and it is the thing that says the hull is moving: at eight
     * kilometres she is a few dark pixels and the white behind her is what
     * makes them a ship rather than a rock.
     *
     * In lengths rather than in metres, tapering to nothing, and fading as it
     * goes — a wake is foam where the water was just thrown and disturbed
     * water for a long way after, so it has to arrive at the sea's own colour
     * rather than stop. The first cut was one bright wedge six lengths long
     * and it read as a concrete strip laid on the water, which is what an
     * opaque triangle of constant albedo on a flat surface always reads as.
     *
     * Laid a hand's breadth over the surface so it wins the depth test against
     * the lattice the sea is drawn on — and skipped in the shadow pass, since
     * what a sheet of foam six centimetres over the water occludes is the
     * water six centimetres under it. */
    if (!g_shadow_cast) {
        fly_v3 foam = fly_v3mk(0.74f, 0.78f, 0.80f);
        fly_v3 far = fly_v3mk(0.30f, 0.40f, 0.46f);   /* about what open water is */
        int n = mid ? 4 : 2;
        for (k = 0; k < n; ++k) {
            float t0 = (float)k / (float)n, t1 = (float)(k + 1) / (float)n;
            float run = hl * 3.2f;
            float u0 = -hl * 0.86f - run * t0, u1 = -hl * 0.86f - run * t1;
            float w0 = hb * 0.95f * (1.0f - t0), w1 = hb * 0.95f * (1.0f - t1);
            fly_v3 col = fly_v3lerp(foam, far, t0 * 0.85f);
            fly_v3 a = yawp(deck, u0, -w0, 0.06f, cy, sy);
            fly_v3 b = yawp(deck, u0, w0, 0.06f, cy, sy);
            fly_v3 c = yawp(deck, u1, w1, 0.06f, cy, sy);
            fly_v3 d2 = yawp(deck, u1, -w1, 0.06f, cy, sy);
            tri_shaded(rt, v, e, a, b, c, col, 1.0f);
            tri_shaded(rt, v, e, a, c, d2, col, 1.0f);
        }
        /* and the bow wave, which is what says which end is the front */
        if (mid) {
            fly_v3 f0 = yawp(deck, hl * 1.02f, 0.0f, 0.06f, cy, sy);
            fly_v3 f1 = yawp(deck, hl * 0.34f, -hb * 1.35f, 0.06f, cy, sy);
            fly_v3 f2 = yawp(deck, hl * 0.34f, hb * 1.35f, 0.06f, cy, sy);
            tri_shaded(rt, v, e, f0, f1, f2, foam, 1.0f);
        }
    }
    if (!lit) return;
    /* Navigation lights: red to port, green to starboard, and a masthead over
       both. The one thing on this water that is visible at night. */
    draw_light(rt, v, yawp(deck, hl * 0.16f, 0.0f, fb + hh + 4.6f, cy, sy),
               FLY_RGBA(255, 246, 214, 210), 1.0f);
    draw_light(rt, v, yawp(deck, hl * 0.30f, -hb, fb + 1.6f, cy, sy),
               FLY_RGBA(255, 70, 60, 190), 0.8f);
    draw_light(rt, v, yawp(deck, hl * 0.30f, hb, fb + 1.6f, cy, sy),
               FLY_RGBA(70, 255, 110, 190), 0.8f);
}

/* The columns themselves, on the road and on the water. Culled by range like
 * every other prop: a convoy twelve kilometres away is four sub-pixel boxes,
 * and the way it is on is still drawn.
 *
 * A hull is drawn further out than a lorry and that is not a preference: a
 * coaster is sixty metres long against a truck's nine, so the range at which
 * it stops being worth a triangle is a long way past the range a column does.
 * The projected-size gate below still has the last word on both. */
static void draw_convoys(fly_render_target *rt, const fly__view *v, const fly__env *e,
                         const fly_game *g) {
    int c, i;
    for (c = 0; c < FLY_CONVOY_MAX; ++c) {
        const fly_convoy *cv = &g->convoys[c];
        fly_way way;
        fly_v3 col;
        int night;
        if (!cv->active) continue;
        if (fly_v3dist(cv->pos, v->pos) > (cv->sea ? 12000.0f : 5200.0f)) continue;
        if (!(cv->sea ? fly_sea_as_way(&g->lanes, cv->line, &way)
                      : fly_road_as_way(&g->roads, cv->line, &way))) continue;
        /* A power's freight wears its colours; everybody else's is the ochre a
           working truck is painted — or, afloat, the grey a working hull is —
           because unaligned is a flag nobody flies and `fly_faction_colour`
           quite correctly has no opinion about it. */
        col = cv->faction == FLY_FACTION_FREE
            ? (cv->sea ? fly_v3mk(0.34f, 0.36f, 0.38f) : fly_v3mk(0.52f, 0.46f, 0.20f))
            : fly_faction_colour(cv->faction);
        night = e->day < 0.35f;
        for (i = 0; i < cv->trucks; ++i) {
            fly_v3 p;
            float yaw, deck;
            if (!fly_convoy_truck(cv, &way, i, &p, &yaw)) continue;
            if (!object_worth_drawing(v, rt, p, cv->sea ? 40.0f : 6.0f)) continue;
            if (cv->sea) {
                convoy_vessel(rt, v, e, p, yaw, (int)cv->tier, i == 0, col, night);
                continue;
            }
            /* On the deck as drawn rather than as surveyed. The two agree
               everywhere the road is not standing on a hummock the drawn
               terrain pushed up through it — see road_bed — and where they do
               not, a truck left at the surveyed height is a truck buried to the
               axles in its own carriageway. The lift is one slab over the drawn
               ground, which is where the surface of a road on natural ground
               is; on an embankment the survey is higher and wins. */
            deck = drawn_ground(&g->world, p.x, p.y) + FLY_ROAD_DECK;
            if (p.z < deck) p.z = deck;
            convoy_vehicle(rt, v, e, p, yaw, (int)cv->tier, i == 0, col, night);
        }
    }
}

/* --- the rivers ----------------------------------------------------------
 *
 * Still water is a plane and is drawn off the terrain lattice; a river is a
 * line and is drawn as one, and the reason is the arithmetic. The finest water
 * lattice steps 26 m and the coarse one 240; a channel is forty to fifty
 * metres across. Sampled on that lattice a river is caught by some cells and
 * missed by the ones between, so what comes out is a chain of dashes that
 * changes as the camera moves — and a cell with one corner in the channel and
 * the next outside it would run the surface from the river's own level down to
 * the sea across a single triangle.
 *
 * So the ribbon is built off the polyline the survey published: a cross
 * section at every station, seven columns wide, spanning the whole footprint
 * the channel was cut over. It is deliberately *wider* than the water, because
 * the waterline is not this function's to find — every column carries the
 * depth at its own foot and `raster_water_raw` drops the pixels where that
 * goes negative, so the shore follows the terrain's own contour exactly the
 * way the sea's does. What this has to guarantee is only that the drawn
 * surface is never above the drawn ground, which the survey guarantees by
 * putting the surface at the floor of the section.
 *
 * Everything else about the surface — waves, Fresnel, glint, the bed showing
 * through, the surf at the edge — is `water_surface`, which is the sea's, so a
 * river is the same water as the coast. */
/* Columns across the ribbon. The shoreline is clipped per pixel off the
 * interpolated depth, so this is what decides how finely that contour is
 * resolved rather than whether the shore is there at all: seven columns
 * over a two-hundred-metre section put the waterline on a thirty-metre
 * lattice, which reads as a canal with square ends. */
#define FLY_RIVER_COLS 15
#define FLY_RIVER_DRAW_R 9000.0f

static void river_section(const fly_world *w, const fly__env *e, const fly__view *v,
                          fly_v3 c, float nx, float ny, float wz, float half,
                          fly_v3 *pos, fly__wv *wv) {
    int i;
    /* Onto the ground as it is *drawn*, not as the height function reports it.
     *
     * A river surface is below the ground on both sides of it by construction,
     * so out where the terrain is drawn through a 240 m ring the mesh spans
     * the whole channel and buries it: the ribbon loses the depth test and the
     * river comes apart into a chain of pools with the reaches between them
     * missing — which is exactly what it did, and what the path tracer, which
     * has no rings, drew as one unbroken line. So the section is lifted onto
     * the coarse mesh the way a road deck is (see ground_bias), by the ring's
     * own cell. The *depth* stays the real one, so the shoreline and the
     * shallows are the water's and only the surface it is painted on is the
     * ring's. */
    float dx = fabsf(c.x - v->pos.x), dy = fabsf(c.y - v->pos.y);
    float cell = ring_cell(dx > dy ? dx : dy);
    /* Over the whole cell rather than half of it: the mesh's vertices are on
     * its own lattice and not on this section, so the highest ground it can be
     * carrying over a channel is a cell away in the worst case, not half. */
    float lift = ground_bias_r(w, c.x, c.y, cell * 0.9f);
    for (i = 0; i < FLY_RIVER_COLS; ++i) {
        float u = -1.0f + 2.0f * (float)i / (float)(FLY_RIVER_COLS - 1);
        float x = c.x + nx * half * u, y = c.y + ny * half * u;
        float gz = fly_world_ground(w, x, y);
        fly_v3 p, view, dir;
        float dl;
        /* The surface off the field itself, so the ribbon and the world agree
         * about where the water is to the centimetre: level across the floor
         * and let down under the ground on the bank — see FLY_RIVER_EDGE. That
         * is what makes the outer columns come out with a depth of exactly
         * zero at the waterline and negative past it, which is what the
         * fragment stage clips the shore on. */
        wz = fly_world_water_at(w, x, y, gz);
        wv[i].depth = wz - gz;
        if (gz + lift > wz) wz = gz + lift;
        p = fly_v3mk(x, y, wz);
        pos[i] = p;
        wv[i].z = wz;
        wv[i].cloud = cloud_shadow(e, p);
        view = fly_v3norm(fly_v3sub(v->pos, p));
        wv[i].sky = sky_ambient(e, p, fly_v3norm(fly_v3mk(-view.x, -view.y,
                                                          view.z * 0.5f + 1.0f)));
        dir = fly_v3sub(p, v->pos);
        dl = fly_v3len(dir);
        fog_pair(e, v->pos, fly_v3scale(dir, 1.0f / (dl + 1e-5f)), dl,
                 &wv[i].fogT, &wv[i].fogS);
    }
}

static void draw_rivers(fly_render_target *rt, const fly__view *v, const fly__env *e,
                        const fly_game *g) {
    const fly_world *w = &g->world;
    int r;
    if (g_shadow_cast) return;  /* water casts no shadow */
    for (r = 0; r < w->nriver; ++r) {
        const fly_river *rv = &w->river[r];
        int i;
        for (i = rv->first; i + 1 < rv->first + rv->count; ++i) {
            const fly_river_pt *a = &w->river_pt[i], *b = &w->river_pt[i + 1];
            float ex = b->x - a->x, ey = b->y - a->y, len = hypotf(ex, ey);
            float nx, ny, mx = (a->x + b->x) * 0.5f, my = (a->y + b->y) * 0.5f;
            float dist = hypotf(mx - v->pos.x, my - v->pos.y);
            float wide = a->wide > b->wide ? a->wide : b->wide;
            float half = wide + fly_river_batter(wide), step;
            int subs, k;
            fly_v3 pa[FLY_RIVER_COLS], pb[FLY_RIVER_COLS];
            fly__wv wa[FLY_RIVER_COLS], wb[FLY_RIVER_COLS];
            if (len < 1e-3f) continue;
            if (dist > FLY_RIVER_DRAW_R + len) continue;
            if (!object_worth_drawing(v, rt, fly_v3mk(mx, my, (a->z + b->z) * 0.5f),
                                      len * 0.5f + half)) continue;
            nx = -ey / len;
            ny = ex / len;
            /* Along the span, the air decides the tessellation and not the
               water: the surface is linear between stations, but the aerial
               perspective across a kilometre of it is not, and it is resolved
               per vertex the way the water ring resolves it. */
            step = fly_clampf(dist * 0.05f, 60.0f, 400.0f);
            subs = (int)(len / step) + 1;
            if (subs > 24) subs = 24;
            river_section(w, e, v, fly_v3mk(a->x, a->y, 0), nx, ny, a->z, half, pa, wa);
            for (k = 1; k <= subs; ++k) {
                float t = (float)k / (float)subs;
                int c;
                river_section(w, e, v, fly_v3mk(fly_lerpf(a->x, b->x, t),
                                                fly_lerpf(a->y, b->y, t), 0),
                              nx, ny, fly_lerpf(a->z, b->z, t), half, pb, wb);
                for (c = 0; c + 1 < FLY_RIVER_COLS; ++c) {
                    if (wa[c].depth <= -0.4f && wa[c + 1].depth <= -0.4f &&
                        wb[c].depth <= -0.4f && wb[c + 1].depth <= -0.4f) continue;
                    raster_water_tri(rt, v, e, pa[c], pa[c + 1], pb[c + 1],
                                     &wa[c], &wa[c + 1], &wb[c + 1]);
                    raster_water_tri(rt, v, e, pa[c], pb[c + 1], pb[c],
                                     &wa[c], &wb[c + 1], &wb[c]);
                }
                memcpy(pa, pb, sizeof pa);
                memcpy(wa, wb, sizeof wa);
            }
        }
    }
}

static void draw_scene_objects(fly_render_target *rt, const fly_game *g, const fly__env *e,
                               const fly__view *v) {
    int i;
    /* Everything standing on the ground goes with the ground: from above the
     * near field these are sub-pixel geometry inside a square that is no
     * longer drawn. Craft and their effects below are not near field — the
     * aeroplane is the subject of every frame from orbit. */
    int near = near_field_worth_drawing(e);
    if (near) {
        /* Everything below prices itself off the graphics level rather than
         * being switched off by it. A bald planet is not a cheap frame, it is
         * a different world, and the budget rung is supposed to be the same
         * world drawn for less: the wood thins and pulls in, the blade layer
         * goes, and the boulders — which are three triangles each and scatter
         * on a 64 m lattice — stay at every level because they cost nothing
         * and they are what gives bare rock its scale. */
        draw_boulders(rt, g, e, v);
        draw_foliage(rt, g, e, v);
        draw_grass(rt, g, e, v);
    }
    if (near) {
        /* Roads first, so the settlements' own pavement and plinths win the
           depth test where a carriageway runs into a yard. */
        /* Before the roads, so a farm lane gives way to the carriageway it
           joins where the two overlap, and after the wood, whose ground the
           fields have already taken. */
        draw_steadings(rt, g, e, v);
        /* Before the roads and after the wood, because a crossing is a road
           over water and the carriageway has to win the depth test where it
           does. */
        draw_rivers(rt, v, e, g);
        draw_roads(rt, v, e, g);
        /* After the roads and before the settlements: a quay is out on the
           water and overlaps neither, but it is the same kind of thing as a
           carriageway — infrastructure between the places rather than at one —
           and it belongs in the same band of the draw order. */
        draw_harbour(rt, v, e, g);
        for (i = 0; i < g->world.nloc; ++i)
            if (g->world.loc[i].discovered) draw_location(rt, v, e, g, i);
        draw_rail(rt, v, e, g);
        draw_convoys(rt, v, e, g);
        draw_ground_items(rt, v, e, g);
    }
    draw_blasts(rt, v, e, g);
    draw_ordnance(rt, v, e, g);
    if (!g->crash_handled && g->active_airframe >= 0 && g->active_airframe < g->owned_count &&
        g->owned[g->active_airframe].status == FLY_AIRFRAME_USABLE)
        draw_craft(rt, v, e, &g->player.craft, &g->player.airframe, &g->player.controls,
                   craft_livery(g->player.faction, 1), craft_trim(g->player.faction, 1),
                   g->time_s);
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active)
            draw_craft(rt, v, e, &g->pilots[i].craft, &g->pilots[i].airframe, NULL,
                       craft_livery(g->pilots[i].faction, 0),
                       craft_trim(g->pilots[i].faction, 0), g->time_s);
}

/* The cascades are megabytes each and rebuilt every frame; allocating and
 * faulting them in each time cost more than rasterizing into them. They are
 * kept between frames and only resized when --detail changes the map size. */
static float *g_sm_store[FLY_SM_CASCADES];
/* Per cascade, not one shared figure: the cascades are no longer all the same
 * size, and a single cached length made allocating the small one free the large
 * ones that had already been filled this frame — every pointer the build was
 * still holding, dangling. */
static int g_sm_store_n[FLY_SM_CASCADES];

static float *shadow_alloc(int c, int n) {
    if (g_sm_store_n[c] != n) {
        free(g_sm_store[c]);
        g_sm_store[c] = NULL;
        g_sm_store_n[c] = 0;
    }
    if (!g_sm_store[c]) {
        g_sm_store[c] = (float *)malloc((size_t)n * sizeof(float));
        if (!g_sm_store[c]) return NULL;
        g_sm_store_n[c] = n;
    }
    return g_sm_store[c];
}

static void shadow_free(fly__shadow *sm) {
    int c;
    for (c = 0; c < FLY_SM_CASCADES; ++c) sm->depth[c] = NULL;
    sm->active = 0;
}

#define FLY_SM_TERRAIN_MAX 161 /* terrain sample grid resolution + 1 */

/* rasterize the terrain heightfield over one cascade's world footprint into
 * its depth map (rolling two rows so no full grid is materialized) */
static void shadow_cast_terrain(fly__shadow *sm, int c, const fly_world *w,
                                float cx, float cy, float H, int cells) {
    static fly_v3 pr[FLY_SM_TERRAIN_MAX], cr[FLY_SM_TERRAIN_MAX];
    if (cells > FLY_SM_TERRAIN_MAX - 1) cells = FLY_SM_TERRAIN_MAX - 1;
    /* Snapped to a world lattice of `step`, for the same reason the light-plane
     * origin is snapped to its texels: a caster grid that slides with the
     * cascade centre samples different points of the heightfield every frame,
     * so the recorded silhouette wobbles even though the map it is written into
     * does not. It went unnoticed while the sea floor was a flat plane — there
     * was nothing under the coast to resample — and showed up the moment the
     * bathymetry arrived, as a climbing camera crawling ten times worse than a
     * translating one. */
    float step = 2.0f * H / (float)cells;
    float bx = floorf((cx - H) / step) * step, by = floorf((cy - H) / step) * step;
    int vn = cells + 1, i, j;
    for (i = 0; i < vn; ++i) {
        float x = bx + (float)i * step;
        pr[i] = fly_v3mk(x, by, fly_world_ground(w, x, by));
    }
    for (j = 1; j < vn; ++j) {
        float y = by + (float)j * step;
        for (i = 0; i < vn; ++i) {
            float x = bx + (float)i * step;
            cr[i] = fly_v3mk(x, y, fly_world_ground(w, x, y));
        }
        for (i = 0; i < cells; ++i) {
            sm_raster_tri(sm, c, pr[i], pr[i + 1], cr[i + 1]);
            sm_raster_tri(sm, c, pr[i], cr[i + 1], cr[i]);
        }
        memcpy(pr, cr, (size_t)vn * sizeof(fly_v3));
    }
}

/* Rasterize every cascade on the CPU: clear, scan-convert the heightfield, then
 * scan-convert the occluders the replay recorded. The reference build, and the
 * fallback whenever no GL backend finished the map. */
static int shadow_build_cpu(fly__shadow *sm, const fly_game *g) {
    int c;
    for (c = 0; c < FLY_SM_CASCADES; ++c) {
        int i, n = sm->size[c] * sm->size[c];
        sm->depth[c] = shadow_alloc(c, n);
        if (!sm->depth[c]) { shadow_free(sm); return -1; }
        for (i = 0; i < n; ++i) sm->depth[c][i] = 1e30f;
        shadow_cast_terrain(sm, c, &g->world, sm->center[c].x, sm->center[c].y,
                            sm->half[c], sm->cells);
    }
    shadow_replay_cpu(sm);
    sm->gpu = 0;
    return 0;
}

/* Render both cascades into the images the beauty pass samples, and leave them
 * there. The heightfield never reaches the CPU: a static grid of cell indices
 * is lifted onto it by the vertex stage, exactly as the terrain rings are, so
 * the per-frame cost is two draw calls and a handful of uniforms. */
static int shadow_build_gpu(fly__shadow *sm, const fly_game *g) {
    static fly_gpu_mesh *grid, *occ;
    static uint32_t *occ_idx;
    static int occ_idx_cap, grid_cells;
    static int cast_nc = 1; /* R32F unless the driver refuses to render it */
    const float clear[4] = { 1e30f, 0.0f, 0.0f, 1.0f };
    const int grid_attr[1] = { 2 };
    const int occ_attr[1] = { 3 };
    fly_gpu_prog *pt, *po;
    int c, ntri = g_smcast_n / 9;

    if (fly_gpu_init() != 0) return -1;
    pt = fly_gpu_program_mesh(FLY_SM_CAST_TERRAIN_VS, FLY_SM_CAST_FS);
    po = fly_gpu_program_mesh(FLY_SM_CAST_OBJ_VS, FLY_SM_CAST_FS);
    if (!pt || !po) return -1;
    if (!grid) grid = fly_gpu_mesh_create();
    if (!occ) occ = fly_gpu_mesh_create();
    if (!grid || !occ) return -1;

    /* the grid carries cell indices, not world positions, so it only changes
     * when --detail moves the tessellation */
    if (grid_cells != sm->cells) {
        static float *gv;
        static uint32_t *gi;
        int vn = sm->cells + 1, i, j, k = 0;
        float *nv = (float *)realloc(gv, (size_t)vn * vn * 2 * sizeof(float));
        uint32_t *ni = (uint32_t *)realloc(gi, (size_t)sm->cells * sm->cells * 6 * sizeof(uint32_t));
        if (!nv || !ni) { free(nv ? nv : gv); gv = NULL; gi = NULL; grid_cells = 0; return -1; }
        gv = nv;
        gi = ni;
        for (j = 0; j < vn; ++j)
            for (i = 0; i < vn; ++i) {
                gv[(j * vn + i) * 2 + 0] = (float)i;
                gv[(j * vn + i) * 2 + 1] = (float)j;
            }
        for (j = 0; j < sm->cells; ++j)
            for (i = 0; i < sm->cells; ++i) {
                uint32_t a = (uint32_t)(j * vn + i);
                gi[k++] = a; gi[k++] = a + 1; gi[k++] = a + (uint32_t)vn + 1;
                gi[k++] = a; gi[k++] = a + (uint32_t)vn + 1; gi[k++] = a + (uint32_t)vn;
            }
        if (fly_gpu_mesh_upload(grid, gv, vn * vn, 2, grid_attr, 1, gi, k) != 0) return -1;
        grid_cells = sm->cells;
    }

    if (ntri > 0) {
        int nv = ntri * 3, i;
        if (nv > occ_idx_cap) {
            uint32_t *ni = (uint32_t *)realloc(occ_idx, (size_t)nv * sizeof(uint32_t));
            if (!ni) return -1;
            occ_idx = ni;
            for (i = occ_idx_cap; i < nv; ++i) occ_idx[i] = (uint32_t)i;
            occ_idx_cap = nv;
        }
        if (fly_gpu_mesh_upload(occ, g_smcast, nv, 3, occ_attr, 1, occ_idx, nv) != 0) return -1;
    }

    for (c = 0; c < FLY_SM_CASCADES; ++c) {
        float step = 2.0f * sm->half[c] / (float)sm->cells;
        /* same lattice snap as shadow_cast_terrain, so the two builds agree */
        float gx = floorf((sm->center[c].x - sm->half[c]) / step) * step;
        float gy = floorf((sm->center[c].y - sm->half[c]) / step) * step;
        if (!g_sm_img[c]) g_sm_img[c] = fly_gpu_img_create();
        if (!g_sm_img[c]) return -1;
        if (fly_gpu_pass_begin_img(g_sm_img[c], sm->size[c], sm->size[c], cast_nc, clear) != 0) {
            /* a driver that will not render single-channel float still renders
             * RGBA32F; the samplers only ever read .r either way */
            if (cast_nc == 4) return -1;
            cast_nc = 4;
            if (fly_gpu_pass_begin_img(g_sm_img[c], sm->size[c], sm->size[c], 4, clear) != 0) return -1;
        }
        /* the cast programs want the heightfield and the light frame, nothing
         * about the camera or the sky; the grid never samples a pad apron
         * outside its own footprint */
        fly_gpu_set1i(pt, "uSeed", (int)g->world.seed);
        fly_gpu_set4f(pt, "uChartFrame", g->world.frame.ux, g->world.frame.uy,
                      g->world.frame.sa, g->world.frame.ca);
        /* the cascades' own grid is the one caller that also runs fly_canopy,
         * so its site list has to reach the clearing rather than the apron */
        env_ground_uniforms(pt, g, sm->center[c].x, sm->center[c].y,
                            sm->half[c] * 1.4143f, FLY_TILTH_REACH);
        cast_uniforms(pt, sm, c);
        fly_gpu_set3f(pt, "uCastGrid", gx, gy, step);
        if (fly_gpu_mesh_draw(pt, grid) != 0) { fly_gpu_pass_end_img(); return -1; }
        if (ntri > 0) {
            cast_uniforms(po, sm, c);
            if (fly_gpu_mesh_draw(po, occ) != 0) { fly_gpu_pass_end_img(); return -1; }
        }
        if (fly_gpu_pass_end_img() != 0) return -1;
    }
    for (c = 0; c < FLY_SM_CASCADES; ++c) sm->depth[c] = NULL;
    sm->gpu = 1;
    return 0;
}

/* Build the two-cascade sun shadow map for this frame: fit each cascade around
 * the point the camera looks at, replay the settlement, craft, rail and
 * ground-item geometry through the shared primitives in cast mode so their true
 * silhouettes are recorded, then rasterize terrain plus occluders into the
 * cascades — on the GPU when `use_gpu` and a backend is there, else on the CPU.
 * The two produce the same map in the same units; only where it lives differs. */
static void shadow_build(fly__shadow *sm, const fly_game *g, const fly__env *e,
                         fly_render_target *rt, const fly__view *v, int detail,
                         int use_gpu) {
    int c, li;
    memset(sm, 0, sizeof *sm);
    if (e->sun.z <= 0.06f) return; /* sun on/below the horizon: no direct light to occlude */

    fly_v3 L = fly_v3norm(fly_v3scale(e->sun, -1.0f)); /* light travel direction */
    fly_v3 hint = fabsf(L.z) > 0.98f ? fly_v3mk(1, 0, 0) : fly_v3mk(0, 0, 1);
    fly_v3 right = fly_v3norm(fly_v3cross(hint, L));
    fly_v3 up = fly_v3norm(fly_v3cross(L, right));
    sm->fwd = L;
    sm->right = right;
    sm->up = up;
    sm->size[0] = sm->size[1] = detail >= 2 ? 1024 : (detail >= 1 ? 768 : 512);
    sm->size[2] = sm->size[0] / 2;
    /* PCF radius *beyond* the bilinear 2x2 base: 0 gives a 2x2 filter (four
     * taps, continuous), 1 a 4x4, 2 a 6x6. Spent where it shows: the near
     * cascade's texels are a tenth of a metre, so a wider kernel there buys a
     * soft contact shadow for taps that each cover very little ground, while
     * the far cascade's metres-wide texels are already softer than anything a
     * kernel would add. */
    sm->pcf[0] = detail >= 2 ? 2 : (detail >= 1 ? 1 : 0);
    sm->pcf[1] = detail >= 2 ? 1 : 0;
    sm->pcf[2] = 0;
    /* Fewer cells than the two-cascade build used, and finer terrain shadows
     * for it. The grid is rasterized once per cascade over that cascade's own
     * footprint, so it is what a cascade costs — 160 cells over three cascades
     * measured +29% on a frame — but the footprints are now six times smaller,
     * and 112 cells over the near cascade's 190 m is a 1.7 m step where 160
     * cells over the old 3 km one was 19 m. Cheaper and eleven times finer
     * where it shows. */
    sm->cells = detail >= 2 ? 112 : (detail >= 1 ? 96 : 72);
    if (sm->cells > FLY_SM_TERRAIN_MAX - 1) sm->cells = FLY_SM_TERRAIN_MAX - 1;

    /* Extent and centre distance are one decision, not two.
     *
     * Fitting every cascade around a single focus point — the terrain the
     * camera is looking at, up to five kilometres out — meant that looking at
     * the horizon put the *crisp* cascade on a distant hillside and left the
     * ground under the aircraft to the coarse one. Each cascade is instead
     * centred a fraction of its own extent along the view: the small one covers
     * what is close, the large one covers what is far, and neither is asked to
     * do the other's job. The centre stops at the terrain the view actually
     * hits, so looking into a hillside does not push a cascade through it, and
     * it scales with how horizontal the view is, so looking straight down puts
     * every cascade under the camera where it belongs. */
    float t = trace_terrain(&g->world, v->pos, v->fwd, 12000.0f);
    if (t <= 1.0f) t = 12000.0f;
    fly_v3 fh = fly_v3mk(v->fwd.x, v->fwd.y, 0.0f);
    float horiz = fly_v3len(fh);
    fh = horiz > 1e-4f ? fly_v3scale(fh, 1.0f / horiz) : fly_v3mk(1.0f, 0.0f, 0.0f);
    float agl = v->pos.z - fly_world_ground(&g->world, v->pos.x, v->pos.y);
    if (agl < 0.0f) agl = 0.0f;
    /* The near cascade grows with altitude only slowly: from high up more ground
     * is on screen, but the ground that needs sub-pixel texels is still the
     * ground nearest the camera. */
    float half[FLY_SM_CASCADES];
    half[0] = fly_clampf(70.0f + agl * 0.20f, 95.0f, 520.0f);
    half[1] = fly_clampf(half[0] * 5.0f, 480.0f, 2600.0f);
    half[2] = fly_clampf(half[1] * 3.6f, 2200.0f, 9000.0f);

    for (c = 0; c < FLY_SM_CASCADES; ++c) {
        /* Quantise the extent before anything is fitted to it.
         *
         * The light-plane span works out to a function of H and the sun alone —
         * the corners below are all offsets from the focus, so the focus
         * cancels — which means a stepped H gives a texel size that holds
         * perfectly still while the camera moves at a fixed altitude. That is
         * what makes the origin snapping further down mean anything: snapping to
         * a lattice whose spacing changes every frame snaps to nothing. Twelve
         * steps per octave costs at most six percent of the resolution. */
        float H = exp2f(ceilf(log2f(half[c]) * 12.0f) / 12.0f);
        fly_v3 focus = fly_v3add(v->pos, fly_v3scale(fh, fminf(H * 0.65f, t) * horiz));
        focus.z = fly_world_ground(&g->world, focus.x, focus.y);
        sm->center[c] = focus;
        /* fit the light-plane square to the world AABB (ground relief plus room
         * for towers/craft above it), keeping square texels */
        float zlo = focus.z - H * 0.5f - 40.0f, zhi = focus.z + H * 0.55f + 190.0f;
        float umn = 1e30f, umx = -1e30f, vmn = 1e30f, vmx = -1e30f;
        float dmn = 1e30f, dmx = -1e30f;
        int a, b, d;
        for (a = 0; a < 2; ++a)
            for (b = 0; b < 2; ++b)
                for (d = 0; d < 2; ++d) {
                    fly_v3 p = fly_v3mk(focus.x + (a ? H : -H), focus.y + (b ? H : -H), d ? zhi : zlo);
                    float pu = fly_v3dot(p, right), pv = fly_v3dot(p, up);
                    /* The depth interval is fitted to the *world's* height
                     * range, not to this band. zlo/zhi are a guess at the
                     * relief a cascade will contain, which is fine for sizing
                     * the light-plane square but wrong as a clip plane: 520 m
                     * of ground can drop further than the guess allows, and
                     * only the GPU build clips, so the two silently diverged —
                     * 12000 texels of caster missing from the near cascade at
                     * a coastal site. Sea bed (which now really is below sea level) to the tallest tower on the
                     * tallest ridge costs a wider depth buffer and nothing
                     * else: 24 bits over 40 km still resolves millimetres. */
                    float pd0 = fly_v3dot(fly_v3mk(p.x, p.y, -260.0f), L);
                    float pd1 = fly_v3dot(fly_v3mk(p.x, p.y, 3000.0f), L);
                    if (pu < umn) umn = pu;
                    if (pu > umx) umx = pu;
                    if (pv < vmn) vmn = pv;
                    if (pv > vmx) vmx = pv;
                    if (pd0 < dmn) dmn = pd0;
                    if (pd1 < dmn) dmn = pd1;
                    if (pd0 > dmx) dmx = pd0;
                    if (pd1 > dmx) dmx = pd1;
                }
        float span = fmaxf(umx - umn, vmx - vmn) * 1.02f;
        sm->texel[c] = span / (float)sm->size[c];
        /* Snap the origin to the texel lattice. The map is rebuilt from world
         * geometry every frame, so when the grid lands on the same world texels
         * each time the stored depths do too, and a shadow edge stops wobbling
         * as the camera moves. It did not matter with the old wide cascades —
         * measured at 0.0001 rms, which is why it was not there — but a near
         * cascade with half-metre texels re-quantises its occluders visibly:
         * 0.007 rms of visibility on a camera creeping a third of a metre per
         * frame, i.e. the sharper edges bought back as shimmer. */
        sm->umin[c] = floorf(((umn + umx) * 0.5f - span * 0.5f) / sm->texel[c]) * sm->texel[c];
        sm->vmin[c] = floorf(((vmn + vmx) * 0.5f - span * 0.5f) / sm->texel[c]) * sm->texel[c];
        sm->half[c] = H;
        /* a little more for a craft above the tallest ridge, and for the pad
         * skirts that hang below the sea bed */
        sm->dmin[c] = dmn - 600.0f;
        sm->dmax[c] = dmx + 120.0f;
    }

    /* Replay solid scene geometry as occluders. Screen writes are gated off and
     * the primitives hand their triangles to shadow_cast_tri, so whichever
     * builder runs below sees the same silhouettes the beauty pass will draw. */
    g_smcast_n = 0;
    g_smcast_overflow = 0;
    g_shadow_cast = sm;
    for (li = 0; li < g->world.nloc; ++li)
        if (g->world.loc[li].discovered) draw_location(rt, v, e, g, li);
    draw_foliage(rt, g, e, v);
    draw_steadings(rt, g, e, v);
    draw_rail(rt, v, e, g);
    draw_ground_items(rt, v, e, g);
    if (!g->crash_handled && g->active_airframe >= 0 && g->active_airframe < g->owned_count &&
        g->owned[g->active_airframe].status == FLY_AIRFRAME_USABLE)
        draw_craft(rt, v, e, &g->player.craft, &g->player.airframe, &g->player.controls,
                   craft_livery(g->player.faction, 1), craft_trim(g->player.faction, 1),
                   g->time_s);
    for (li = 0; li < FLY_MAX_PILOTS; ++li)
        if (g->pilots[li].active)
            draw_craft(rt, v, e, &g->pilots[li].craft, &g->pilots[li].airframe, NULL,
                       craft_livery(g->pilots[li].faction, 0),
                       craft_trim(g->pilots[li].faction, 0), g->time_s);
    g_shadow_cast = NULL;

    /* Widen the depth interval to whatever the replay actually produced.
     *
     * Only the GPU build has clip planes, so an occluder outside them vanishes
     * from one cascade and not the other — 12000 texels' worth, in the case
     * that found this. Fitting the interval to a box around the cascade's
     * footprint is guesswork twice over: the light-plane square is wider than
     * the footprint, so geometry beyond it still lands on the map, and the box
     * says nothing about how tall or deep that geometry is. One pass over the
     * captured triangles answers both exactly and costs microseconds. */
    for (li = 0; li + 2 < g_smcast_n; li += 3) {
        float d = g_smcast[li] * L.x + g_smcast[li + 1] * L.y + g_smcast[li + 2] * L.z;
        for (c = 0; c < FLY_SM_CASCADES; ++c) {
            if (d - 4.0f < sm->dmin[c]) sm->dmin[c] = d - 4.0f;
            if (d + 4.0f > sm->dmax[c]) sm->dmax[c] = d + 4.0f;
        }
    }

    if (use_gpu && !g_smcast_overflow && shadow_build_gpu(sm, g) == 0) {
        sm->active = 1;
        return;
    }
    if (shadow_build_cpu(sm, g) != 0) return;
    sm->active = 1;
}


int fly_render_terrain_sun_probe(const fly_game *g, const fly_cam *cam, int detail,
                                 const fly_v3 *pts, const fly_v3 *nrm, float *vis, int n) {
    fly__view v = make_view(cam, 64, 64);
    fly__env e = make_env(g, &v);
    fly_render_target rt;
    fly__shadow sm;
    int i;
    if (fly_rt_init(&rt, 8, 8) != 0) return -1;
    fly_rt_clear(&rt, 0);
    shadow_build(&sm, g, &e, &rt, &v, detail, 0);
    if (!sm.active) { shadow_free(&sm); fly_rt_free(&rt); return -1; }
    e.shadow = &sm;
    for (i = 0; i < n; ++i) {
        float nd = fly_v3dot(nrm[i], e.sun);
        vis[i] = terrain_sun(&e, pts[i], nrm[i], nd < 0.0f ? -nd : nd, 0.0f);
    }
    shadow_free(&sm);
    fly_rt_free(&rt);
    return 0;
}

int fly_render_shadow_probe(const fly_game *g, const fly_cam *cam, int detail,
                            const fly_v3 *pts, const float *ndotl, const fly_v3 *nrm,
                            float *vis, int n,
                            float *const *depth, int *size, int *pcf,
                            fly_v3 *right, fly_v3 *up, fly_v3 *fwd,
                            float *umin, float *vmin, float *texel) {
    fly__view v = make_view(cam, 64, 64);
    fly__env e = make_env(g, &v);
    fly_render_target rt;
    fly__shadow sm;
    int i, rc = 0;
    if (fly_rt_init(&rt, 8, 8) != 0) return -1;
    fly_rt_clear(&rt, 0);
    /* the probe compares the GLSL lookup against the C one, so it wants the
     * reference map in memory rather than a GPU-resident cascade */
    shadow_build(&sm, g, &e, &rt, &v, detail, 0);
    if (!sm.active) { rc = -1; goto done; }
    for (i = 0; i < n; ++i) {
        fly_v3 p = nrm ? sm_offset(&sm, pts[i], nrm[i], ndotl[i]) : pts[i];
        vis[i] = shadow_sample(&sm, p, ndotl[i]);
    }
    if (right) *right = sm.right;
    if (up) *up = sm.up;
    if (fwd) *fwd = sm.fwd;
    for (i = 0; i < FLY_SM_CASCADES; ++i) {
        if (size) size[i] = sm.size[i];
        if (pcf) pcf[i] = sm.pcf[i];
        if (depth && depth[i])
            memcpy(depth[i], sm.depth[i], (size_t)sm.size[i] * sm.size[i] * sizeof(float));
        if (umin) umin[i] = sm.umin[i];
        if (vmin) vmin[i] = sm.vmin[i];
        if (texel) texel[i] = sm.texel[i];
    }
done:
    shadow_free(&sm);
    fly_rt_free(&rt);
    return rc;
}

int fly_render_shadow_cascade_probe(const fly_game *g, const fly_cam *cam, int detail,
                                    float *gpu_c, float *cpu_c, int *size, int cascade) {
    fly__view v = make_view(cam, 64, 64);
    fly__env e = make_env(g, &v);
    fly_render_target rt;
    fly__shadow sm;
    int rc = -1;
    if (cascade < 0 || cascade >= FLY_SM_CASCADES) return -1;
    if (fly_rt_init(&rt, 8, 8) != 0) return -1;
    fly_rt_clear(&rt, 0);
    shadow_build(&sm, g, &e, &rt, &v, detail, 1);
    if (!sm.active || !sm.gpu) goto done; /* no GL backend: nothing to compare */
    if (size) *size = sm.size[cascade];
    if (fly_gpu_img_read(g_sm_img[cascade], gpu_c) != 0) goto done;
    /* rebuild the very same cascade parameters through the reference path */
    shadow_build(&sm, g, &e, &rt, &v, detail, 0);
    if (!sm.active || sm.gpu) goto done;
    memcpy(cpu_c, sm.depth[cascade], (size_t)sm.size[cascade] * sm.size[cascade] * sizeof(float));
    rc = 0;
done:
    shadow_free(&sm);
    fly_rt_free(&rt);
    return rc;
}

int fly_render_detail_probe(const fly_game *g, float x, float y, fly_v3 mat,
                            float px_per_m, fly_v3 *n, fly_v3 *tint) {
    fly_v3 nn = n ? fly_v3norm(*n) : fly_v3mk(0.0f, 0.0f, 1.0f);
    fly_v3 t;
    if (!g) return -1;
    t = terrain_detail(&g->world, x, y, mat, px_per_m, &nn);
    if (n) *n = nn;
    if (tint) *tint = t;
    return 0;
}

int fly_render_material_probe(const fly_game *g, fly_v3 p, fly_v3 nrm, fly_v3 vdir,
                              float shadow, float px_per_m,
                              fly_v3 *mat, fly_v3 *albedo, fly_v3 *radiance) {
    fly__env e = make_env(g, NULL);
    fly_v3 m, alb;
    if (!g) return -1;
    alb = terrain_surface(&g->world, p.x, p.y, p.z, 1.0f - nrm.z, px_per_m, e.ball, e.wet, &m);
    if (mat) *mat = m;
    if (albedo) *albedo = alb;
    if (radiance) *radiance = terrain_light(&e, alb, m, nrm, fly_v3norm(vdir), shadow, 1.0f);
    return 0;
}

/* --- atmosphere test hook ---
 *
 * The scattering model, evaluated directly. The sky, the colour of the sun and
 * what distance does to a surface are one integral now, so a test can hold the
 * whole atmosphere to physical statements — blue overhead, red along a long
 * path, and light conserved as one turns into the other — instead of reading
 * three tuned gradients back out of a tonemapped frame. */
float fly_render_planet_probe(fly_v3 ro, fly_v3 rd, fly_v3 *normal) {
    return fly__planet_hit(ro, fly_v3norm(rd), normal);
}

float fly_render_air_probe(float H, float z0, float dz, float dist) {
    return scat_thick(H, z0, dz, dist);
}

int fly_render_brdf_probe(float gloss, float f0, float ndl, float ndv,
                          float ndh, float vdh, float *lobe, float *env) {
    float a = ggx_alpha(gloss);
    if (lobe) {
        float fcap = 1.0f - a;
        *lobe = ggx_sun(fly_clampf(ndl, 0.0f, 1.0f), fly_clampf(ndv, 0.0f, 1.0f),
                        fly_clampf(ndh, 0.0f, 1.0f), a) * schlick_f(f0, fcap, vdh);
    }
    if (env) *env = ggx_env(f0, ndv, a);
    return 0;
}

void fly_render_shaft_stats(double *mean, double *worst, long *touched) {
    if (mean) *mean = g_shaft_n ? g_shaft_sum / (double)g_shaft_n : 0.0;
    if (worst) *worst = g_shaft_worst;
    if (touched) *touched = g_shaft_n;
}

int fly_render_atmos_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                           fly_v3 *sky, fly_v3 *transmit, fly_v3 *inscat, fly_v3 *sun) {
    fly__env e;
    fly_v3 d;
    if (!g) return -1;
    e = make_env(g, NULL);
    if (sun) *sun = e.sun;
    d = fly_v3norm(rd);
    if (sky) *sky = sky_ambient(&e, ro, d);
    if (transmit || inscat) fly__scatter(&e, ro, d, dist, transmit, inscat);
    return 0;
}

int fly_render_cloud_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                           float *cover, fly_v3 *color) {
    fly__env e;
    fly_v3 c = fly_v3zero();
    float a;
    if (!g) return -1;
    e = make_env(g, NULL);
    a = cloud_slab(&e, ro, fly_v3norm(rd), dist, &c);
    if (cover) *cover = a;
    if (color) *color = c;
    return 0;
}

int fly_render_cirrus_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                            float *cover, fly_v3 *color) {
    fly__env e;
    fly_v3 c = fly_v3zero();
    float a;
    if (!g) return -1;
    e = make_env(g, NULL);
    a = cirrus_sheet(&e, ro, fly_v3norm(rd), dist, &c);
    if (cover) *cover = a;
    if (color) *color = c;
    return 0;
}

int fly_render_cloud_sun_probe(const fly_game *g, const fly_v3 *pts, float *sun,
                               float *deck, float *ice, int n) {
    fly__env e;
    int i;
    if (!g || !pts || n <= 0) return -1;
    e = make_env(g, NULL);
    for (i = 0; i < n; ++i) {
        fly_v3 p = pts[i];
        if (sun) sun[i] = cloud_shadow(&e, p);
        /* the two layers on their own, so a test can say which one it is
           looking at and what the composition did to the pair */
        if (deck) {
            float t = (FLY_CLOUD_Z - p.z) / (e.sun.z > 0.02f ? e.sun.z : 1.0f);
            deck[i] = (e.sun.z > 0.02f && p.z < FLY_CLOUD_Z)
                          ? cloud_cover(&e, p.x + e.sun.x * t, p.y + e.sun.y * t)
                          : 0.0f;
        }
        if (ice) {
            float t = (FLY_CIRRUS_Z - p.z) / (e.sun.z > 0.02f ? e.sun.z : 1.0f);
            ice[i] = (e.sun.z > 0.02f && p.z < FLY_CIRRUS_Z)
                         ? cirrus_cover(&e, p.x + e.sun.x * t, p.y + e.sun.y * t)
                         : 0.0f;
        }
    }
    return 0;
}

int fly_render_prop_disc(const fly_airframe *af, fly_v3 *hub, float *radius) {
    fly_v3 h;
    if (!af) return -1;
    prop_disc_geom(af, &h, radius);
    /* the model's reference metres times the model's own scale, so a caller
       gets a body-frame offset it can add to a position without knowing that
       the plane is drawn for a 5.5 m half-span */
    if (hub) *hub = fly_v3scale(h, plane_half_span(af) / 5.5f);
    return 0;
}

int fly_render_sky_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, unsigned opts,
                         fly_v3 *sky) {
    fly__env e;
    unsigned f = 0u;
    if (!g || !sky) return -1;
    if (opts & FLY_RENDER_SKY_NOSUN) f |= FLY_SKY_NOSUN;
    if (opts & FLY_RENDER_SKY_NOCLOUD) f |= FLY_SKY_NOCLOUD;
    if (opts & FLY_RENDER_SKY_STARS) f |= FLY_SKY_STARS;
    e = make_env(g, NULL);
    *sky = sky_sample(&e, ro, fly_v3norm(rd), f);
    return 0;
}

int fly_render_surface_probe(const fly_game *g, fly_v3 n, fly_v3 albedo, float ao,
                             fly_v3 *lit, fly_v3 *shaded) {
    fly__env e;
    if (!g) return -1;
    e = make_env(g, NULL);
    n = fly_v3norm(n);
    if (lit) *lit = lit_surface(&e, albedo, n, 1.0f, ao);
    if (shaded) *shaded = lit_surface(&e, albedo, n, 0.0f, ao);
    return 0;
}

int fly_render_sun_beam(const fly_game *g, fly_v3 ro, fly_v3 *beam) {
    fly__view vw;
    fly__env e;
    if (!g || !beam) return -1;
    memset(&vw, 0, sizeof vw);
    vw.pos = ro;
    vw.fwd = fly_v3mk(1, 0, 0);
    vw.right = fly_v3mk(0, 1, 0);
    vw.up = fly_v3mk(0, 0, 1);
    vw.sy = vw.sx = 623.0f;
    vw.aa = 1;
    e = make_env(g, &vw);
    *beam = e.sunlight;
    return 0;
}

int fly_render_irradiance_probe(const fly_game *g, fly_v3 ro, fly_v3 n,
                                fly_v3 *irr, fly_v3 *ground) {
    fly__view vw;
    fly__env e;
    if (!g) return -1;
    memset(&vw, 0, sizeof vw);
    vw.pos = ro;
    vw.fwd = fly_v3mk(1, 0, 0);
    vw.right = fly_v3mk(0, 1, 0);
    vw.up = fly_v3mk(0, 0, 1);
    vw.sy = vw.sx = 623.0f;
    vw.aa = 1;
    e = make_env(g, &vw);
    if (irr) *irr = sky_irradiance(&e, fly_v3norm(n));
    if (ground) *ground = e.ground_lit;
    return 0;
}

int fly_render_irradiance_exact(const fly_game *g, fly_v3 ro, fly_v3 n, fly_v3 *irr) {
    fly__view vw;
    fly__env e;
    fly_v3 acc = fly_v3zero(), nn;
    const int N = 4096;
    int i;
    if (!g || !irr) return -1;
    memset(&vw, 0, sizeof vw);
    vw.pos = ro;
    vw.fwd = fly_v3mk(1, 0, 0);
    vw.right = fly_v3mk(0, 1, 0);
    vw.up = fly_v3mk(0, 0, 1);
    vw.sy = vw.sx = 623.0f;
    vw.aa = 1;
    e = make_env(g, &vw);
    nn = fly_v3norm(n);
    for (i = 0; i < N; ++i) {
        float z = ((float)i + 0.5f) / (float)N;
        float r = sqrtf(1.0f - z * z);
        float ph = (float)i * 2.39996323f;
        fly_v3 d = fly_v3mk(r * cosf(ph), r * sinf(ph), z);
        float c = fly_v3dot(d, nn);
        if (c <= 0.0f) continue;
        acc = fly_v3add(acc, fly_v3scale(sky_sample(&e, ro, d, FLY_SKY_NOSUN), c));
    }
    /* 2pi/N is the hemisphere's measure; 1/pi is the diffuse BRDF, which the
     * fit also carries, so the two are directly comparable */
    *irr = fly_v3scale(acc, (2.0f * FLY_PI / (float)N) / FLY_PI * FLY_SKY_IRR);
    return 0;
}

/* --- cinematography test hooks ---
 *
 * The grade this frame would apply, and the regional grass field it is partly
 * built from. Both public for the same reason the atmosphere probe is: a grade
 * is checked against bounds, and reading bounds back out of a tonemapped frame
 * measures the tonemap. */
int fly_render_grade_probe(const fly_game *g, const fly_cam *cam, fly_v3 *grade, float *sat) {
    fly__view v;
    fly__env e;
    if (!g || !cam) return -1;
    v = make_view(cam, 64, 64);
    e = make_env(g, &v);
    if (grade) *grade = e.grade;
    if (sat) *sat = e.sat;
    return 0;
}

float fly_render_grass_region_probe(const fly_game *g, float x, float y) {
    return g ? grass_region(g->world.seed, x, y) : 0.0f;
}

float fly_render_ground_drawn(const fly_world *w, float x, float y) {
    return w ? drawn_ground(w, x, y) : 0.0f;
}

float fly_render_ground_scatter(const fly_world *w, float x, float y) {
    return w ? scatter_ground(w, x, y) : 0.0f;
}

int fly_render_road_deck_probe(const fly_game *g, int road, int span, float t,
                               fly_v3 *centre, fly_v3 *across) {
    const fly_road *rd;
    fly_v3 c, tan;
    float len, nx, ny, bed;
    if (!g || road < 0 || road >= g->roads.count) return 0;
    rd = &g->roads.road[road];
    if (span < 0 || span + 1 >= rd->point_count) return 0;
    if (!fly_road_span(rd, span, t, &c, &tan)) return 0;
    len = hypotf(tan.x, tan.y);
    if (len < 1e-4f) return 0;
    nx = -tan.y / len;
    ny = tan.x / len;
    /* the drawing's own rule, so what comes back is the deck as drawn rather
       than an idealised one */
    bed = road_bed(&g->world, c, nx, ny, FLY_ROAD_HALF);
    if (c.z < bed) c.z = bed;
    if (centre) *centre = c;
    if (across) *across = fly_v3mk(nx, ny, 0.0f);
    return 1;
}

int fly_render_lamp_probe(const fly_game *g, int line, int k,
                          fly_v3 *pos, float *chain) {
    if (!g) return 0;
    if (line < 0) {
        fly_v3 crown;
        if (!rail_lamp_at(&g->rail_route, k, FLY_RAIL_PIPE_R, &crown, NULL, NULL, chain))
            return 0;
        if (pos) *pos = crown;
        return 1;
    }
    if (line >= g->roads.count) return 0;
    return road_lamp_at(g, &g->roads.road[line], k, pos, NULL, chain);
}

int fly_render_pylon_probe(const fly_game *g, int road, int k, fly_v3 *pos,
                           float *chain, int *stands) {
    if (!g || road < 0 || road >= g->roads.count) return 0;
    return road_pylon_at(g, &g->roads.road[road], k, pos, NULL, stands, chain);
}

/* ---------------- frame ---------------- */

/* Hand the resolved picture over at output size. The software resolve fills
 * the render target at render size, so anything supersampled still owes a box
 * filter here; the GPU chain has already done it inside its last pass. */
/* The z-buffer's starting value, for whichever pipeline is about to write into
 * it.
 *
 * Only depth needs one: the HDR buffer is fully written by whichever
 * environment pass runs (or by the studio backdrop), and the 8-bit image is
 * fully written by the resolve. Clearing all three cost ~18 MB of writes a
 * frame for nothing.
 *
 * And only the pipelines that rasterize in C need it at all, which is why this
 * is a call at the head of each of them rather than one at the top of the
 * frame. A resident GPU frame never touches this buffer — its depth test runs
 * against a GL attachment, and every pass over the result from the volumetric
 * correction to the resolve stays on that side of the bus — so clearing it was
 * a write over the whole frame that nothing then read: 8 MB at 1080p, and 33 MB
 * at the rung that supersamples by two, which is more than the frame budget on
 * its own. A frame that comes back down is fine either way, because
 * frame_to_target writes every depth it read. */
static void depth_reset(fly_render_target *rt) {
    int k, npx = rt->w * rt->h;
    for (k = 0; k < npx; ++k) rt->depth[k] = 1e30f;
}

static void frame_out(fly_img *out, const fly_img *src, int aa) {
    if (!out || out == src) return;
    if (aa <= 1)
        memcpy(out->px, src->px, sizeof(uint32_t) * (size_t)out->w * (size_t)out->h);
    else fly_img_downsample(out, src, aa);
}

/* The frame, drawn into `rt` and resolved into `out`.
 *
 * `out` is the picture the caller asked for and `rt` is what the renderer drew
 * it at, which differ by the supersample factor. Passing both down is what
 * lets the resolve fold the box downsample into its own last pass: at the top
 * rung that is a 33 MB readback instead of a 132 MB one plus a pass over eight
 * million pixels on the way out. A NULL `out` means the caller wants the
 * render target itself — its radiance, its depth and its picture at render
 * size — which is the older, slower contract fly_render_frame still keeps. */
static void render_frame(fly_render_target *rt, fly_img *out, const fly_game *g,
                         const fly_cam *cam, const fly_render_opts *opts) {
    fly__view v;
    fly__env e;
    fly__shadow sm;
    fly_img *final = out ? out : &rt->img;
    phase_begin();
    phase(FLY_PHASE_SETUP);
    v = make_view(cam, rt->w, rt->h);
    v.aa = opts->ssaa > 1 ? opts->ssaa : 1;
    e = make_env(g, &v);
    e.lod = lod_for(opts->detail);
    ao_begin(g, opts->ao_target, opts->ao_budget);
    int use_shadow = opts->mode == FLY_RENDER_RASTER || opts->mode == FLY_RENDER_MIX;

    g_nblend = 0; /* a frame that bailed early must not leak its disc into this one */
    g_nlamp = 0;
    g_frame_resident = 0;
    if (opts->studio) {
        /* Studio plate: the airframe alone on a flat backdrop. No sky, terrain,
         * water, scatter, settlements or shadow map, so the silhouette and the
         * fitted geometry can be read without the world behind them. */
        fly_v3 back = fly_v3mk(0.052f, 0.058f, 0.070f);
        int n = rt->w * rt->h, k;
        depth_reset(rt);
        for (k = 0; k < n; ++k) rt->hdr[k] = back;
        draw_craft(rt, &v, &e, &g->player.craft, &g->player.airframe, &g->player.controls,
                   craft_livery(g->player.faction, 1), craft_trim(g->player.faction, 1),
                   g->time_s);
        lamp_flush(rt, &v);
        blend_flush(rt, &v);
        phase(FLY_PHASE_RESOLVE);
        if (!hdr_resolve(rt, out, &e, &v, opts->post, 1)) frame_out(out, &rt->img, v.aa);
        phase_end();
        return;
    }
    ao_warm(g, &v);
    if (use_shadow) {
        phase(FLY_PHASE_SHADOW);
        /* The cascades can stay on the GPU only when nothing on the CPU will
         * sample them — which is exactly the pure raster path, where the
         * scene's objects are captured and shaded by the fragment stage, the
         * volumetric correction marches in GLSL, and nothing else asks. The
         * traced modes rasterize their objects, so they need the map in
         * memory; so does a raster frame the software pipeline ends up
         * drawing, and that one is rebuilt below when it happens. */
        shadow_build(&sm, g, &e, rt, &v, opts->detail,
                     opts->mode == FLY_RENDER_RASTER && !g_env_force_cpu &&
                     !g_force_readback);
        e.shadow = &sm;
    }
    if (opts->mode == FLY_RENDER_RASTER) {
        /* One depth-tested GL pass for the whole frame: sky, terrain, water and
         * the scene's objects — and then the correction, the splats and the
         * translucent pass over the result, without any of it coming back. */
        int keep = !g_force_readback;
        g_env_gpu = !g_env_force_cpu &&
                    raster_environment_gpu(rt, g, &e, &v, opts->detail, 1,
                                           opts->msaa, keep) == 0;
        if (g_env_gpu && keep) {
            phase(FLY_PHASE_FINISH);
            g_env_gpu = gpu_frame_finish(g, &e, &v, rt->w, rt->h) == 0;
        }
        g_frame_resident = g_env_gpu && keep;
        if (!g_env_gpu) {
            /* Whatever the abandoned pass had already queued is queued twice
             * once the software pipeline draws the same objects again. */
            g_nblend = 0;
            g_nlights = 0;
            g_nlamp = 0;
            /* the software pipeline reads the cascades directly, so a map left
             * in GL textures has to be rebuilt in memory before it draws */
            if (sm.gpu) shadow_build(&sm, g, &e, rt, &v, opts->detail, 0);
            phase(FLY_PHASE_TERRAIN);
            depth_reset(rt);
            raster_environment_cpu(rt, g, &e, &v, opts->detail);
            phase(FLY_PHASE_OBJECTS);
            draw_scene_objects(rt, g, &e, &v);
        } else if (!out) {
            /* the caller wants the buffers as well as the picture, so this is
             * where the frame comes down — after everything that was going to
             * be added to it, and still only once */
            phase(FLY_PHASE_READBACK);
            frame_to_target(rt);
        }
    } else {
        /* Traced modes blend over the environment before objects go down, so
         * those stay on the software rasterizer. */
        phase(FLY_PHASE_TERRAIN);
        depth_reset(rt);
        if (opts->mode == FLY_RENDER_MIX) raster_environment(rt, g, &e, &v, opts->detail, opts->msaa);
        pt_environment(rt, g, &e, &v, opts->pt_samples,
                       opts->mode == FLY_RENDER_PT ? 1.0f : opts->pt_blend);
        phase(FLY_PHASE_OBJECTS);
        draw_scene_objects(rt, g, &e, &v);
    }

    /* The air and the translucent pass, now that there is a depth buffer to
     * work against — on the frame the software pipeline drew. A resident frame
     * has had both already, in GLSL, inside gpu_frame_finish; running them
     * again here would draw a second propeller disc over the first. */
    if (!g_frame_resident) {
        phase(FLY_PHASE_FINISH);
        /* Shafts before the translucent pass, so a propeller disc or a plume
         * seen through a shaft is composited over air that has already been
         * shadowed rather than over air that is about to be. */
        volumetric_shafts(rt, &e, &v);
        /* The lamps, before the translucent pass: a lamp lights the surfaces
         * the frame drew, and a plume or a propeller disc in front of one is
         * seen against a road that has already been lit rather than against a
         * road that is about to be. See lamp_flush. */
        lamp_flush(rt, &v);
        /* Whatever had to be seen through, composited against the z-buffer the
         * frame just finished. See blend_shaded. */
        blend_flush(rt, &v);
    }

    phase(FLY_PHASE_RESOLVE);
    if (!hdr_resolve(rt, out, &e, &v, opts->post, 0)) frame_out(out, &rt->img, v.aa);
    phase(FLY_PHASE_OVERLAY);
    /* Weather is an overlay on the finished picture, so it is drawn at the size
     * the picture is actually seen at rather than at the size it was sampled
     * at: a rain streak is eight to twenty-three *screen* pixels long, and
     * measuring it in supersamples made the rain shorter and denser at every
     * rung above the first. */
    draw_rain_img(final, &e, g->weather.precip);
    draw_lightning_img(final, &e);
    if (use_shadow) shadow_free(&sm);
    phase_end();
}

void fly_render_frame(fly_render_target *rt, const fly_game *g, const fly_cam *cam,
                      const fly_render_opts *opts) {
    render_frame(rt, NULL, g, cam, opts);
}

int fly_render_frame_aa(fly_img *out, const fly_game *g, const fly_cam *cam,
                        const fly_render_opts *opts) {
    /* The supersampled target is tens of megabytes; allocating and faulting it
     * in per frame cost more than several render passes. Keep it between calls
     * and resize only when the frame size changes. */
    static fly_render_target rt;
    int f = opts->ssaa > 1 ? opts->ssaa : 1;
    int w = out->w * f, h = out->h * f;
    if (rt.w != w || rt.h != h) {
        fly_rt_free(&rt);
        if (fly_rt_init(&rt, w, h) != 0) {
            rt.w = rt.h = 0;
            return -1;
        }
    }
    render_frame(&rt, out, g, cam, opts);
    return 0;
}
