/* fly_world: seeded procedural world — terrain, locations, weather regions,
 * resources and a supply/demand market economy. Everything derives
 * deterministically from the world seed; discovery state is the only
 * mutable exploration data. */
#ifndef FLY_WORLD_H
#define FLY_WORLD_H

#include <stdint.h>

#include "fly_faction.h"
#include "fly_math.h"
#include "fly_rng.h"
#include "fly_sim.h"

#define FLY_WORLD_HALF 30000.0f /* world spans +-30 km around the origin */
/* No ground anywhere on any planet stands above this, and that is a fact about
 * the field rather than a measurement of one seed. Summing the ceiling of every
 * term in ground_raw:
 *
 *   continent_z    <=  -3400 + 3400 + 450          =  450 m
 *   the constant                                   =  120 m
 *   base * 260     <=  FLY_NOISE3_GAIN * 260       =  301 m
 *   mountains*2400 <=  ridge^2 * smoothstep * orogeny * 2400, all three <= 1
 *                                                  = 2400 m
 *   detail         <=  (9 + 2.2) * FLY_NOISE3_GAIN =   13 m
 *                                                    -------
 *                                                     3284 m
 *
 * and neither of the two things fly_world_ground does afterwards can raise it:
 * an apron interpolates toward a pad_z that is itself a ground_raw sample, and
 * a cutting is written as a min. Rounded up, with the slack left visible.
 *
 * What it is for: a ray already above the ceiling and still climbing cannot
 * meet the ground, so the height-field march can stop rather than stepping to
 * its 26 km reach to find nothing — see trace_terrain, its GLSL twin in
 * fly_glsl.h, and `world.ceiling`, which holds the arithmetic above honest. */
#define FLY_GROUND_CEILING 3300.0f
#define FLY_MAX_LOC 28
#define FLY_WATER_LEVEL 2.0f     /* terrain below this is under water */
#define FLY_PAD_MIN_DRY 3.0f     /* pad aprons graded at least this high (dry) */
#define FLY_PAD_DECK_R 55.0f     /* drawn landing-deck radius; the rail stops clear of it */
#define FLY_SHORE_SETBACK 28.0f  /* structures keep this much dry ground around them */
/* Where a closed stand's crown mass sits, in metres above the ground it grows
 * on. The scatter's mean tree is 9.3 m and its crown hangs between 0.4 and
 * 1.04 of that, so this is the height a sun ray has to climb to be clear of
 * the leaves. See fly_world_canopy and canopy_sun in fly_render.c. */
#define FLY_CANOPY_H 5.8f
/* How much of the sun a closed canopy holds back, as the exponent of the
 * density: exp(-FLY_CANOPY_K) is what reaches the floor under a closed stand.
 *
 * Measured, not chosen. The scatter's own crowns are cast into the near
 * cascade and nowhere else, so the answer they give over 220 points of closed
 * canopy — divided by the same points with the foliage caster switched off, to
 * take the terrain's own shadow out of it — is the number this has to
 * reproduce: 0.84 of the sun, over seven sun elevations on seed 4242.
 * `render.shadow` measures the pair and holds the two within a few per cent. */
#define FLY_CANOPY_K 0.174f
/* Where the crown layer ends: the scatter's mean tree is 9.3 m and its crown
 * tops out at 1.04 of its height. A surface standing this far above the ground
 * is above the leaves and takes the full sun; one on the floor takes all of
 * FLY_CANOPY_K. Between the two it is linear, for the same reason the traced
 * occlusion window is linear between its two heights: visibility through a
 * canopy really falls off something nearer exponentially, and neither of these
 * has the samples to say which. */
#define FLY_CANOPY_TOP 9.7f
/* How far a settlement's clearing reaches before it stops thinning the wood at
 * all: the widest fly_world_clearing radius times the 2.53 its edge fades out
 * over. Anything that evaluates the woodland field against a *culled* list of
 * sites — the GPU, which cannot walk the whole array — has to keep every site
 * within this of the ground it is sampling, or it grows a wood where the
 * airfield is. `world.siting` holds the widest clearing under the number. */
#define FLY_CLEARING_REACH (330.0f * 2.53f)
/* How far out a pad grades the ground it stands on: flat to FLY_PAD_FLAT_R and
 * blended back into the natural terrain by FLY_PAD_BLEND_R — see
 * fly_world_ground, which is where the grading is applied, and fly_glsl.h,
 * which is where the shader does the same arithmetic. Out here rather than in
 * fly_world.c because the approach is flown against it: a terrain floor has
 * something to protect the aeroplane from everywhere except inside this ring,
 * and inside it the only thing a floor can do is hold the aeroplane off its
 * own apron. */
#define FLY_PAD_FLAT_R 75.0f
#define FLY_PAD_BLEND_R 150.0f

/* --- the shape of an approach --------------------------------------------
 *
 * One description of what flying into a settlement looks like, held here
 * because two flight laws fly it — the player's autopilot in fly_game and the
 * fleet's own in fly_pilot — and a pad that is approachable to one and not to
 * the other is a bug nobody can see from either file.
 *
 * The slope is the gradient of the descent: a straight line from the pad out
 * to wherever it meets the cruise altitude. It is not a taste. An aeroplane on
 * short final is at about 0.80 of vref and may not touch down faster than
 * 3.2 m/s without the flight model billing it as an arrival, so a slope steeper
 * than that ratio is one the aeroplane is forbidden to fly; one in thirteen is
 * the starting airframe's, and 0.075 leaves a little in hand.
 *
 * The clearance is what the profile holds over anything standing between the
 * aeroplane and the pad. Ten or twelve metres is the number a terrain floor is
 * worth on short final: enough that the tracking lag cannot put the wheels in
 * the ground, small enough that it does not hold the aeroplane over its own
 * apron.
 *
 * The final is how much of the approach is flown on one course. Four kilometres
 * at the slope above is three hundred metres of height to lose, which is the
 * descent a leg arrives with — so a final is the whole of the descent and the
 * course chosen for it is the whole answer to "which way in". */
#define FLY_APPROACH_SLOPE 0.075f
#define FLY_APPROACH_CLEAR 12.0f
#define FLY_APPROACH_FINAL 4000.0f
/* How many courses a site is surveyed on: sixteen, so the answers are 22.5
 * degrees apart. Finer buys nothing — the thing being avoided is a ridge, which
 * is kilometres wide at four kilometres out — and every one of them is a
 * terrain scan paid for at world-gen. */
#define FLY_APPROACH_DIRS 16

typedef enum {
    FLY_RES_FUEL,    /* refined fuel / charge, keeps you flying */
    FLY_RES_PARTS,   /* spare parts, repairs airframe wear */
    FLY_RES_ORE,     /* raw ore from mines */
    FLY_RES_ALLOY,   /* refined alloy */
    FLY_RES_DATA,    /* data cores from ruins, high value */
    FLY_RES_LUX,     /* luxuries, high value, fragile demand */
    FLY_RES_COUNT
} fly_resource;

typedef enum {
    FLY_LOC_CITY,     /* consumes everything, produces luxuries */
    FLY_LOC_OUTPOST,  /* consumes fuel/luxuries, produces parts */
    FLY_LOC_MINE,     /* produces ore, consumes fuel/parts */
    FLY_LOC_REFINERY, /* ore -> alloy + fuel */
    FLY_LOC_RUINS,    /* produces data cores; dangerous */
    FLY_LOC_SKYPORT,  /* high-altitude trade hub */
    FLY_LOC_KIND_COUNT
} fly_loc_kind;

/* access gates: airframe capabilities required to land there */
#define FLY_GATE_VTOL 0x1u      /* pad-only, needs vtol airframe */
#define FLY_GATE_HIGHALT 0x2u   /* needs service ceiling above the site */
#define FLY_GATE_STORM 0x4u     /* inside the storm belt, needs weather rating */
#define FLY_GATE_LONGRANGE 0x8u /* remote, needs fuel range */

/* --- a place on the chart, as against a distance across it ---------------
 *
 * `fly_wpos` is where something is. `fly_v2` is how far one thing is from
 * another. On a flat map those are the same type and subtraction turns the
 * first into the second; on a map of a ball they are not, because the chart
 * comes back on itself and two positions have no single difference — the way
 * out and the way round are both real and only one of them is shorter.
 *
 * So they are different types, and the members are deliberately not `x` and
 * `y`. A struct with matching member names would still let every `a.x - b.x`
 * in the codebase compile, and those are exactly the lines that are wrong: the
 * compiler cannot see a subtraction it is happy to perform. Renaming them to
 * `e` and `n` — metres east and north of the chart's centre, which is what
 * they have always been — makes every one of those sites a build error, which
 * is the only reliable way to find all of them.
 *
 * `fly_world_delta` is the difference, and `fly_world_dist` its length. */
typedef struct { float e, n; } fly_wpos;

static inline fly_wpos fly_wpos_mk(float e, float n) { fly_wpos p; p.e = e; p.n = n; return p; }
/* the chart part of a world position, and the world position over a chart one */
static inline fly_wpos fly_wpos_of(fly_v3 p) { return fly_wpos_mk(p.x, p.y); }
static inline fly_v3 fly_wpos_at(fly_wpos p, float z) { return fly_v3mk(p.e, p.n, z); }

/* --- the chart's centre, in the form the ball needs ----------------------
 *
 * Living somewhere other than the planet's pole is a *rotation of the sphere*,
 * and it has to be, because that is the only operation that leaves the terrain
 * a function of the point it is at.
 *
 * It used to be an addition to the chart coordinates before the mapping —
 * `sphere_dir(p + origin)` — which is a translation of a chart, and a chart is
 * not a plane. That was wrong twice. It stretched the world: the chart's
 * tangential scale at the origin's own radius is sin(A)/A, so a world centred
 * two radians out had its features squeezed by more than half in one direction
 * and not at all in the other, differently for every seed. And it put a seam
 * back into ground that was supposed to be seamless by construction, because a
 * position folded at the antipode and the same position unfolded are the same
 * point on the ball but different chart coordinates, so adding the origin to
 * each of them lands somewhere different: sixteen metres of cliff across two
 * metres of ground, measured, and exactly the kind of thing nobody finds by
 * flying and everybody finds eventually.
 *
 * A rotation has neither problem. It carries the pole to the origin's point,
 * takes distances and angles with it, and depends on nothing but where the
 * sample is on the ball.
 *
 * `ux, uy` is the bearing of the origin from the pole and `sa, ca` the sine and
 * cosine of the great-circle angle out to it; between them they are Rodrigues'
 * rotation about the axis across that bearing. Kept as four floats rather than
 * recomputed per sample because fly_world_ground is the hottest function in the
 * engine, and uploaded to the shaders as `uChartFrame`. */
typedef struct { float ux, uy, sa, ca; } fly_chart_frame;

typedef struct {
    char name[24];
    fly_loc_kind kind;
    fly_wpos pos;
    float elev;        /* site elevation (gates; skyports report their deck) */
    float pad_z;       /* terrain height the pad apron is leveled to */
    /* What each of FLY_APPROACH_DIRS courses into this pad costs, in metres of
     * ground standing above the approach slope that ends on it — see
     * fly_world_approach_survey, which fills it in once the last earthwork is
     * in the ground, and fly_world_approach_aim, which is how a flight law
     * reads it. Derived from the terrain, so it is regenerated with the world
     * and never saved. */
    float approach[FLY_APPROACH_DIRS];
    uint32_t gates;
    int discovered;
    float stock[FLY_RES_COUNT];  /* units on hand */
    float target[FLY_RES_COUNT]; /* desired stock (demand anchor) */
    float prod[FLY_RES_COUNT];   /* units produced per game-hour */
    float cons[FLY_RES_COUNT];   /* units consumed per game-hour */
    /* --- who holds it ---
     *
     * A property of the place, next to `discovered` and `stock`, because that
     * is what it is: it changes while the world runs and it saves with the
     * world. `owner` is a fly_faction and FLY_FACTION_FREE means nobody has
     * taken it; `grip` is how firmly, 0..1, and reaching zero under pressure
     * is what hands it to somebody else; `push` is what each power is
     * currently exerting here, decaying, so a site remembers a campaign for a
     * few hours and not for ever. The rules that move all three live in
     * fly_faction.c and nothing else may write them. */
    unsigned char owner;
    float grip;
    float push[FLY_FACTION_COUNT];
} fly_location;

/* --- cuttings -------------------------------------------------------------
 *
 * The one place the terrain is deformed by something other than a landing pad.
 *
 * A road holds a gradient by building embankments across the hollows and
 * cutting through the ridges. The embankments are geometry — the road draws its
 * own battered bank — but a cutting is not: it is an absence, and you cannot
 * draw an absence over a heightfield that is still there. So the heightfield
 * has to know, and this is what it knows.
 *
 * Cuts only, never fill, and that is what keeps this bounded. A cut lowers the
 * ground and can never raise it, so nothing here can invent terrain, put a
 * shelf under a bank the road already draws, or disagree with the fill the
 * profile solved for. It also means a world needs very few of them: an
 * embankment is free and a cutting is not, so the surveyor spends its budget
 * only where the alternative is a crest the road would otherwise go over at
 * whatever gradient the ridge happens to have.
 *
 * Bounded because the GPU has to agree. `fly_world_ground` has a twin in GLSL
 * — the terrain's vertices are lifted in the vertex shader — so anything the
 * height function consults has to be something a shader can walk per
 * invocation. The pads are already uploaded that way, as a uniform array of
 * 28; this is the same mechanism with an oriented box in place of a disc, and
 * the count is small for the same reason theirs is.
 *
 * Each is a box in plan, aligned to the line, with the formation height ramped
 * along it: `z0` at -half and `z1` at +half. `wide` is the half width taken
 * down to formation and `feather` how far past that the cut is blended back
 * into the hillside, which is the batter of the side slope. */
#define FLY_CUT_MAX 24
typedef struct {
    float x, y;        /* centre, chart metres */
    float dx, dy;      /* unit vector along the line */
    float half;        /* half length */
    float wide;        /* half width taken to formation */
    float feather;     /* blended back to the hillside over this */
    float z0, z1;      /* formation height at the two ends */
} fly_cut;

/* --- water on the land ----------------------------------------------------
 *
 * The other thing the heightfield is deformed by, and the only one that is
 * also drawn wet.
 *
 * A river is a line laid across the ground, surveyed the way the rail and the
 * roads are (fly_river.c), and it deforms the terrain for the same reason a
 * cutting does: a watercourse that is drawn and not cut lies on a hillside.
 * So the channel is a corridor the heightfield knows about — a chain of
 * stations with the water surface and the channel width at each — and
 * `fly_world_ground` takes the ground down to the bed between them.
 *
 * A lake is not cut at all, and that is the whole difference between the two.
 * A lake is a basin the descent dead-ended in, filled to the lowest lip it
 * could find: the ground inside it is *already* below that level, so all a
 * lake has to say is where the surface is and how far it reaches. The disc is
 * a bound and not a shape — the water is drawn wherever the ground inside the
 * disc is under the stage, so the shoreline is the terrain's own contour.
 *
 * Bounded for the reason the cuttings are bounded: `fly_world_ground` has a
 * GLSL twin, and everything the height function consults has to be something a
 * shader can walk per invocation. One vec4 a station is what makes the budget
 * affordable at all — see fly_glsl.h, where the same loop is written again.
 *
 * These are the counts a *uniform array* can carry, not the counts the survey
 * would like: the same wall the cuttings are up against, and the reason
 * TODO.md's "More earthworks than a uniform array holds" is what lifts both. */
#define FLY_RIVER_MAX 4        /* watercourses in a world */
#define FLY_RIVER_PT_MAX 64    /* stations, shared out between them */
#define FLY_LAKE_MAX 6
/* The least a river may be, in stations and in metres: shorter than this it is
 * a ditch nobody would navigate by, and it is not worth a station of a budget
 * this small. */
#define FLY_RIVER_MIN_PT 5
#define FLY_RIVER_MIN_LEN 4000.0f
/* How far past the flooded ground a lake's disc reaches. The disc has to hold
 * every wet point with room to spare on all sides, because the renderer draws
 * still water off a lattice and a cell with one wet corner and one corner
 * outside the disc would interpolate the surface from the lake down to the
 * sea across a single cell. The coarsest water lattice steps 240 m; this is
 * that with something in hand. */
#define FLY_LAKE_MARGIN FLY_LAKE_EDGE
/* --- where a river's water stops -----------------------------------------
 *
 * The channel is cut over the whole footprint — the floor, then the bank
 * blended back into the hillside — and the water inside it is *not* level all
 * the way to the edge of that. It is level across the floor and then let down
 * to just under the ground over the outer part of the bank, so that the field
 * meets the terrain instead of ending on it.
 *
 * This is the difference between a river and a wall of water. The water field
 * has to stop somewhere, and past the footprint it answers the sea's level;
 * if it were the river's own level right up to that boundary then wherever the
 * bank happens to lie below the surface — which on a heightfield carrying nine
 * metres of relief at a two-hundred-metre wavelength is often — the drawn
 * water would end in a cliff of itself with the country falling away behind
 * it. Let down to under the ground instead, the waterline is wherever the two
 * cross, which is a contour of the terrain, and `fly_world_surface` — the one
 * thing anything actually stands on — is continuous across it.
 *
 * FLY_RIVER_EDGE is where the letting-down starts, as a fraction of the bank;
 * FLY_RIVER_DRY is how far under the ground it ends up, which only has to be
 * enough that the renderer counts it as dry. */
#define FLY_RIVER_EDGE 0.35f
/* And the same, for a lake: the width of the band at the rim of its disc that
 * the level is let down over, in metres. A lake is a disc round a basin and a
 * basin is not a disc, so the rim is where the two disagree — the level is on
 * its way under the ground by the time it gets there. The band is exactly the
 * margin the disc is drawn past the flooded ground, so the letting-down
 * happens outside the water and never inside it. */
#define FLY_LAKE_EDGE 450.0f
#define FLY_RIVER_DRY 0.5f

/* --- the shape of the channel --------------------------------------------
 *
 * The bank is a function of the channel it holds rather than a number the
 * river carries: a watercourse six metres across at its head and twenty-six at
 * its mouth would otherwise be cut inside one footprint sized for the mouth,
 * which up in the hills is a two-hundred-metre trough with a stream in the
 * middle of it. `fly_river.batter` is therefore the *widest* the bank gets —
 * what the bounding box and the span test have to allow for — and this is what
 * the ground is actually graded over at each point along it.
 *
 * FLY_RIVER_LIP is the other half of the shape, and it is what stops a channel
 * reading as a flooded trough. The cut profile does not run flat to the bed
 * across the whole footprint; it rises from the bed at the edge of the floor
 * to a lip above the water by the time it reaches the top of the bank, so the
 * waterline lands part way up the bank instead of wherever the blend happens
 * to cross. The carve is still a min, so where the hillside is already lower
 * than the profile nothing is raised — a lip is a shape the cut may leave, not
 * ground it may invent. */
#define FLY_RIVER_BATTER_K 2.0f
#define FLY_RIVER_BATTER_C 26.0f
#define FLY_RIVER_LIP 3.0f
static inline float fly_river_batter(float wide) {
    return FLY_RIVER_BATTER_K * wide + FLY_RIVER_BATTER_C;
}

typedef struct {
    float x, y;    /* station, chart metres */
    float z;       /* the water surface here */
    float wide;    /* half width of the channel floor */
} fly_river_pt;

typedef struct {
    int first, count;      /* its stations in world.river_pt */
    float bed;             /* how far the channel floor sits under the surface */
    float batter;          /* the widest bank it has; see fly_river_batter */
    float x0, y0, x1, y1;  /* everything it touches, so a point can miss it cheaply */
    int lake;              /* the lake it ends in, or -1 when it reaches the sea */
} fly_river;

typedef struct {
    float x, y;  /* centre */
    float r;     /* the disc the flooded ground is inside */
    float z;     /* the spill point, which is the surface */
} fly_lake;

/* Tagged rather than an anonymous typedef so fly_faction.h — which fly_world.h
 * includes for the fields above — can forward declare it and take a world
 * without the two headers including each other. */
typedef struct fly_world {
    uint32_t seed;
    /* Where the chart's centre sits on the ball. The seed says what the planet
     * looks like; this says where on it you live, and world-gen picks it so
     * that home has a coast and is not walled in — see pick_origin in
     * fly_world.c for why that has to be chosen rather than inherited. */
    fly_wpos origin;
    /* the same thing as the rotation the terrain is sampled through, worked
     * out once — see fly_chart_frame */
    fly_chart_frame frame;
    int nloc;
    fly_location loc[FLY_MAX_LOC];
    float storm_belt_y;  /* center of the permanent storm belt */
    float storm_belt_w;
    /* Who is speaking to whom. World state like the sites are, and mutable
     * like they are: see fly_faction.h. */
    fly_diplomacy dip;
    /* Earthworks the network asked for. Empty until the roads are laid, which
     * is deliberate: a road is surveyed against the ground as it was found, and
     * only then does the ground learn about the road. See fly_cut. */
    int ncut;
    fly_cut cut[FLY_CUT_MAX];
    /* The water on the land, and unlike the cuttings it is there from the
     * moment the world exists: a river is something the country was found
     * with, and both networks are surveyed against ground that already has
     * one. See fly_river.c, and fly_world_gen, which lays them last of all
     * the things that are properties of the planet. */
    int nriver, nriver_pt, nlake;
    fly_river river[FLY_RIVER_MAX];
    fly_river_pt river_pt[FLY_RIVER_PT_MAX];
    fly_lake lake[FLY_LAKE_MAX];
} fly_world;

void fly_world_gen(fly_world *w, uint32_t seed);

/* --- the chart, and the ball under it -----------------------------------
 *
 * Azimuthal equidistant, centred on the chart origin. A point at chart radius
 * d and bearing theta is the point at *great-circle* distance d from the
 * origin along that bearing — so radial distance is exact everywhere and the
 * only distortion is tangential, at sin(d/R)/(d/R): 0.042% across the sixty
 * kilometres the game is played in.
 *
 * Chosen over the obvious equirectangular longitude/latitude for three
 * reasons. Every direction loops at 2*pi*R rather than only east. The
 * distortion in the content band is smaller (0.042% against 0.125%). And it
 * has one singularity, the antipode at pi*R, rather than two poles at pi*R/2 —
 * where an equirectangular chart collapses a whole line to a point and the
 * ground stops moving under an aircraft flying along it.
 *
 * `fly_world_sphere_dir` is the mapping terrain will be sampled through, so a
 * chart that comes back on itself lands on the same noise and has no seam.
 * `fly_world_chart_wrap` folds a position that has gone past the antipode back
 * into the disc — d -> 2*pi*R - d, bearing + 180 degrees — which is continuous
 * on the sphere and a jump across the chart, the same shape of thing an
 * x-wrap would be. */
fly_v3 fly_world_sphere_dir(float x, float y);
fly_wpos fly_world_chart_wrap(fly_wpos p);
/* half the circumference: chart radius of the antipode */
#define FLY_CHART_ANTIPODE (FLY_PI * FLY_PLANET_R)

/* --- measuring across the chart -----------------------------------------
 *
 * `fly_world_delta` is the way from `from` to `to`: a chart-frame displacement
 * whose length is the *great-circle* distance between the two and whose
 * direction is the heading to steer out of `from` to get there. `fly_world_dist`
 * is that length on its own, and the two agree by construction.
 *
 * Both are measured on the ball, which is what makes them right where the
 * chart is not. Two things go wrong with a straight line in chart coordinates.
 * Past the antipode the chart folds, so the short way between two places can be
 * a line that leaves one edge of the disc and arrives at the other, and a plane
 * measurement reads the long way round or a distance that does not exist.
 * Nearer in, the chart's tangential scale is sin(d/R)/(d/R) rather than one, so
 * a distance measured across the chart is short by that much — 0.042% over the
 * sixty kilometres the game is played in, which is nothing, and a third of the
 * way to the antipode, which is not.
 *
 * The near field is where almost every caller lives and it is the case that
 * has to stay boring: at these ranges the answer is the chart subtraction to
 * four decimal places, and this is a correction, not a change.
 *
 * Adding a delta back onto a position is `fly_wpos_step`, which folds — but the
 * chart is not a vector space, so only small steps are exact. A simulation step
 * is small; a route leg is not, and wants stepping or re-measuring. */
fly_v2 fly_world_delta(fly_wpos to, fly_wpos from);
float fly_world_dist(fly_wpos a, fly_wpos b);
fly_wpos fly_wpos_step(fly_wpos p, fly_v2 d);

fly_chart_frame fly_world_frame(fly_wpos origin);
/* the direction on the ball a chart position points at, through that frame:
 * fly_world_sphere_dir turned by where the chart's centre is. The terrain is a
 * function of this and nothing else, which is what makes it seamless. There is
 * a GLSL twin in fly_glsl.h. */
fly_v3 fly_world_ball_dir(fly_chart_frame f, float x, float y);

/* --- the planet under the chart -----------------------------------------
 *
 * Three fields that are functions of a direction on the ball and nothing else,
 * which is what makes them properties of the planet rather than of the map
 * drawn on it: turn the chart's centre anywhere and the same place keeps the
 * same sea, the same climate and the same snow. `fly_world_ball_dir` is how a
 * chart position becomes one of those directions.
 *
 * `fly_world_continent` is the field the ground is built on: positive is
 * continent, negative is sea floor, and it crosses zero at the shoreline. The
 * terrain adds its own relief on top, so the coast is where the two agree
 * rather than where this alone says.
 *
 * The other two are climate, and only the renderer reads them today — the limb
 * from orbit and the snow line under the wheels. They live here because
 * world-gen reads them too: home is chosen temperate and watered, so that the
 * green coast on the ball is the green coast you take off from.
 *
 * Each has an exact GLSL twin in fly_glsl.h. */
float fly_world_continent(uint32_t seed, fly_v3 u);
float fly_world_aridity(uint32_t seed, fly_v3 u);   /* 0 watered .. 1 desert */
float fly_world_snowline(uint32_t seed, fly_v3 u);  /* m; below sea level at the caps */

/* terrain elevation (m); also usable as the sim's fly_ground_fn */
float fly_world_ground(const fly_world *w, float x, float y);
/* the bed: may be below the water, which is how the renderer knows how deep
 * the water is */
float fly_world_surface(const fly_world *w, float x, float y);
float fly_world_ground_cb(void *world, float x, float y);

/* --- how high the water stands here --------------------------------------
 *
 * `fly_world_water` is the whole wet field: FLY_WATER_LEVEL out over the sea,
 * a lake's spill point inside its disc, and a river's own surface inside its
 * channel. It says where the water *would* be and not whether there is any —
 * over dry ground it answers the sea level under it, which is nowhere near.
 * `fly_world_wet` is the question almost every caller actually has: is there
 * water standing on this ground. `fly_world_surface` is what a thing rests on,
 * which is the higher of the two.
 *
 * `fly_world_still` is the same field with the rivers left out. It exists
 * because the two are drawn by different means and only one of them is a plane
 * a lattice can be compared against: still water is sampled off the terrain
 * lattice, where a channel forty metres across between vertices sixty metres
 * apart would break into dashes, so a river is drawn as the line it is. See
 * water_ring and draw_rivers in fly_render.c.
 *
 * Both are discontinuous at the outer edge of a channel or a disc, and that is
 * deliberate rather than tolerated: the carve is blended to nothing by the
 * same edge, so the ground there is the hillside as found and stands above the
 * surface either way. What is continuous is `fly_world_surface`, which is the
 * only one of the three anything stands on.
 *
 * There are GLSL twins of both in fly_glsl.h. */
float fly_world_water(const fly_world *w, float x, float y);
/* The same fields, given the ground height the caller already has. Every call
 * inside a channel or a lake needs it — see FLY_RIVER_EDGE, which is how both
 * of them stop — and almost every caller has just taken it, for the same
 * reason fly_world_canopy and fly_world_tilth take one. `fly_world_water` is
 * the first of these with the lookup done for you. */
float fly_world_water_at(const fly_world *w, float x, float y, float gz);
float fly_world_still(const fly_world *w, float x, float y, float gz);
int fly_world_wet(const fly_world *w, float x, float y);

/* And the level of the water body here, as the body *has* it, without the
 * letting-down at its edge: the sea's level out at sea, a lake's spill point
 * anywhere inside its disc, a river's own surface anywhere inside its channel.
 *
 * The difference from `fly_world_water` is what each is for. The water field
 * is the drawn surface and has to meet the ground where it ends, or the water
 * is a wall; this is the number the *ground* is described against — where the
 * sand under a waterline goes, how far up the bank the wet band reaches, where
 * the treeline starts, how low a field may be ploughed. Given the faded field
 * those all key off a level that is a fixed distance under the ground for the
 * whole width of the edge band, so a lake came out ringed by four hundred
 * metres of beach with no trees on it. */
float fly_world_shore(const fly_world *w, float x, float y);

/* Woodland density at a point: 0 open ground, 1 closed canopy.
 *
 * A property of the world, not of the renderer that draws it. The scatter
 * reads it to place stands, and a test reads it to tell a meadow from a wood
 * — which it has to be able to do, because "a clearing renders with some
 * colour in it" and "the inside of a spruce crown renders green" are the same
 * measurement taken forty metres apart.
 *
 * `ground_out`, when given, receives the terrain height at the same point.
 * The two travel together on purpose: the density falls off at the treeline
 * and the shoreline, so it needs the height anyway, and the caller almost
 * always wants it too. Callers that get 0 back can skip their own lookup —
 * the cheap noise rejected the point before the heightfield was touched. */
float fly_world_forest(const fly_world *w, float x, float y, float *ground_out);

/* How far a settlement of this kind clears the ground around it, in metres:
 * the radius inside which nothing grows at all. The woodland thins from there
 * out to about two and a half times it, so the number is the bare radius and
 * not the whole clearing.
 *
 * It is a property of the settlement rather than of the renderer because the
 * renderer is not the only thing that has to agree about it — the scatter reads
 * it through fly_world_forest, and a test reads it to ask whether a city's
 * outer ring of towers is standing in a wood. What it is set from is what each
 * kind actually builds: a city lays its spire district out on rings at 130 and
 * 235 m and an outpost is a shed and a mast, and one radius for both left trees
 * growing between the towers and across the approach.
 */
float fly_world_clearing(int kind);

/* Just the noise the woodland field is built on, before any of the falloffs
 * that need the heightfield or the settlement list. Split out because the
 * terrain shader needs the same field to know where to lay needle litter, and
 * a shader cannot walk the location array — so the two agree on the part they
 * can both evaluate rather than each carrying its own copy of the octaves.
 * There is a GLSL twin of exactly this in fly_glsl.h. */
float fly_world_forest_mask(const fly_world *w, float x, float y);

/* How tall a stand is, for anything that has to treat a wood as a solid rather
 * than as trees: the height above `gz` at which a wooded column stops passing
 * sunlight, and 0 where nothing grows.
 *
 * The height is an argument because every caller has just taken it. Pass
 * `fly_world_ground(w, x, y)` — the drawn ground, aprons and cuttings folded
 * in, which is what the trees are planted on.
 *
 * It is not the height of a tree. The scatter's mean tree is 9.3 m tall and
 * its crowns close over about a third of the ground beneath them, so a solid
 * lid at tree height would shade three times the ground a wood actually
 * shades. FLY_CANOPY_H is where the *crown mass* is, which is the height a lid
 * has to sit at to cast what the trees cast — measured against the trees
 * themselves, and gated by `world.canopy`. Density scales it, so an edge
 * thins to nothing rather than ending at a wall.
 *
 * There is a GLSL twin in fly_glsl.h (`fly_canopy`), minus the slope cut that
 * `fly_world_forest` ends on — see forest_open in fly_world.c. */
float fly_world_canopy(const fly_world *w, float x, float y, float gz);

/* --- the worked ground ----------------------------------------------------
 *
 * How far a settlement's fields reach, in units of that settlement's own
 * clearing radius, and therefore the one place the shape of the belt is
 * written down. In units of the clearing rather than in metres because that
 * is the measure the world already keeps a settlement's size in: a city
 * builds out to 330 m and works the ground for two and a half kilometres
 * beyond it, an outpost builds out to 170 and works a kilometre, and one
 * ratio says both.
 *
 * FLY_TILTH_IN is where the fields start — just outside the built clearing,
 * because nobody ploughs the apron — and FLY_TILTH_FULL where they are fully
 * established. FLY_TILTH_FADE to FLY_TILTH_OUT is where they give out into
 * open country, and the outer bound is the one the edge noise moves, so no
 * settlement's fields end on a circle. */
#define FLY_TILTH_IN 1.05f
#define FLY_TILTH_FULL 1.60f
#define FLY_TILTH_FADE 4.60f
#define FLY_TILTH_OUT 8.40f
/* The least the edge noise can shrink the outer bound to, so the furthest a
 * field can be from the settlement that works it is FLY_TILTH_OUT divided by
 * this. Named because two things need it: the field itself, and every cull of
 * the location array feeding a shader that evaluates the field. */
#define FLY_TILTH_RAGGED 0.74f
/* Widest any settlement's fields reach, in metres: the largest clearing
 * (a city's) at the furthest the bounds above allow. Anything that evaluates
 * the worked field against a *culled* list of sites — the GPU, which cannot
 * walk the whole array — has to keep every site within this of the ground it
 * is sampling, the same rule and for the same reason as FLY_CLEARING_REACH.
 * `world.tilth` holds the widest belt under the number. */
#define FLY_TILTH_REACH (330.0f * FLY_TILTH_OUT / FLY_TILTH_RAGGED)
/* How big a field is, in metres across its short axis, and the ratio of the
 * long axis to it. One parcel size per settlement rather than one per world:
 * the size is drawn from noise at the settlement's own position, so two
 * neighbouring parishes enclose their ground differently and the pattern does
 * not tile across the map. */
#define FLY_PARCEL_MIN 118.0f
#define FLY_PARCEL_VAR 116.0f
#define FLY_PARCEL_LONG 1.55f

/* What the worked ground is at a point.
 *
 * `work` is how strongly it is farmed, 0 open country to 1 field, and it is
 * the field the rest of the world reads: the woodland is thinned by it (a
 * field is cleared ground), the terrain shader tints and encloses by it, and
 * the scatter plants hedges and builds steadings by it.
 *
 * `crop` is one value for a whole parcel — which crop is standing in it — and
 * `edge` how far, in metres, the point is from the parcel's boundary. Both
 * are zero wherever `work` is, and neither is computed there: the parcel frame
 * is five more noise taps on top of the belt's two, and open country must not
 * pay them.
 *
 * `gz` is the ground height and is an argument for the same reason
 * fly_world_canopy takes one: every caller has just taken it, and a second
 * heightfield sample is the one expense this must not have.
 *
 * There is a GLSL twin in fly_glsl.h (`fly_tilth`), and `gpu.tilth` holds the
 * two together. */
typedef struct {
    int parish;  /* the settlement whose ground this is, or -1 */
    float work;
    float crop;
    float edge;
} fly_tilth;
void fly_world_tilth(const fly_world *w, float x, float y, float gz, fly_tilth *out);

/* The parcel at a point on a named parish's frame, without asking whose ground
 * it is.
 *
 * The scatter walks a lattice a cell at a time and every candidate in a cell
 * has the same answer to "whose fields are these", so paying the site loop per
 * candidate is paying for something already in hand — and the scatter tests
 * many candidates per cell against the parcel boundary to lay a hedge along
 * it. `parish` is `fly_tilth.parish` from a call at the cell; out of range it
 * answers 0 and 0.
 *
 * No GLSL twin: the shaders take the whole field in one call and never walk a
 * boundary. */
void fly_world_parcel(const fly_world *w, int parish, float x, float y,
                      float *crop, float *edge);

/* Where the parcel boundaries crossing a square cell actually run: points
 * along them, `pitch` metres apart, for anything that has to *build* a hedge
 * rather than paint one.
 *
 * A hedge is a line and `fly_world_parcel` answers with a distance, and the
 * two are not the same problem. Keeping scattered candidates that land near a
 * boundary is rejection sampling against a band a few metres wide inside a
 * parcel two hundred across — twenty-odd thrown away for every one kept — and
 * a hedge wants a bush every few metres, so the arithmetic does not come out.
 * The boundary is walked instead.
 *
 * `cx`, `cy` and `half` are the cell, and every point comes back inside it:
 * the walk is phase-locked to the parcel grid rather than to the caller's
 * lattice, so two neighbouring cells neither plant the same bush twice nor
 * leave a gap between them. Returns how many points were written, never more
 * than `max`. */
int fly_world_hedge(const fly_world *w, int parish, float cx, float cy, float half,
                    float pitch, fly_wpos *out, int max);

fly_v3 fly_world_ground_normal(const fly_world *w, float x, float y);
int fly_world_ground_safe(const fly_world *w, float x, float y, float max_slope);

/* Settle a structure planted near `site` onto dry ground.
 *
 * A settlement's architecture reaches a few hundred metres out from its pad,
 * and the coastline does not care: a candidate position can land in open water,
 * where the naive answer — fly_world_ground, which is happily below the
 * waterline — puts a building in the sea. This walks the candidate back along
 * its bearing toward the site until its footing is dry, so the piece keeps its
 * direction from the centre instead of vanishing. The pad apron is graded to at
 * least FLY_PAD_MIN_DRY, so moving inward always converges unless the whole
 * bearing is submerged, which is the 0 return.
 *
 * `p` carries the candidate in and the settled position out; `z` receives the
 * ground height there. Deterministic and side-effect free: the renderer calls
 * it while building geometry, including during the shadow-cast replay, so the
 * two passes must agree exactly. */
int fly_world_settle(const fly_world *w, fly_wpos site, fly_wpos *p, float *z);

/* --- which way in ---------------------------------------------------------
 *
 * A pad is not equally approachable from every side, and a straight-in approach
 * is the assumption that it is. Where a rise stands between the aeroplane and
 * the pad, the terrain floor every flight law carries holds it high until it is
 * too close to descend, and the only thing left is a go-around — flown again
 * from the same direction, with the same rise, for as many attempts as the
 * patience setting allows. Nine of them was standing in for an approach that
 * turns.
 *
 * So each site is surveyed on sixteen courses and remembers what each one
 * costs: the worst height, in metres, that ground on that final stands above
 * the slope an aeroplane would fly down it. Zero is a way in. It is measured
 * once, at world-gen, after the roads have finished moving the heightfield —
 * `fly_world_ground` is expensive and a flight law runs several times a second
 * for every aircraft in the sky, so the terrain question is asked where it can
 * be asked once rather than where it is needed.
 *
 * `fly_world_approach_aim` is the whole of the interface for a flight law: give
 * it where the aircraft is, and it answers where to fly — the pad itself when
 * the aeroplane is on a usable course for it, and a point out on the chosen
 * final when it is not, so the track bends onto the approach instead of
 * arriving across it. `along` comes back with how much final is left beyond
 * that point, which is what a descent profile needs to know: the distance still
 * to fly is to the aim point and then along the final, never the straight line
 * to the pad. */
void fly_world_approach_survey(fly_world *w);
float fly_world_approach_course(const fly_world *w, int loc, float inbound);
fly_wpos fly_world_approach_aim(const fly_world *w, int loc, fly_wpos from, float *along);

/* weather field at a position and game time (seconds) */
void fly_world_weather(const fly_world *w, fly_v3 pos, double time_s, fly_weather *out);
/* --- how dangerous a place is, 0..1 ---
 *
 * Hostile activity, and the one number the loot economy is hung off. Built out
 * of things that were already true about the map rather than a field painted
 * on top: distance from where everyone starts, a noise field of pirate
 * country, how deep into the permanent storm belt you are, and how much kit
 * the nearest site demands before it will let you land.
 *
 * One field, not two, and that is the design: the places that breed pirates
 * and the places that drop good gear have to be the same places, or the ladder
 * is decoration. The good ground is behind the gates, so the gear you need to
 * farm it is the gear you farm. */
float fly_world_danger(const fly_world *w, float x, float y);

/* --- economy --- */
const char *fly_resource_name(fly_resource r);
float fly_resource_base_price(fly_resource r); /* tokens per unit */
float fly_resource_mass(fly_resource r);       /* kg per unit */
/* local price from supply vs demand: scarce = expensive */
float fly_world_price(const fly_world *w, int loc, fly_resource r);
/* advance production/consumption by dt game-hours */
void fly_world_economy_step(fly_world *w, float dt_hours);

/* --- queries --- */
int fly_world_nearest(const fly_world *w, fly_wpos pos, int discovered_only);
/* mark locations within radius of pos discovered; returns count newly found */
int fly_world_discover(fly_world *w, fly_wpos pos, float radius);
/* can this airframe land at loc? returns 0 ok, else the blocking gate bit */
uint32_t fly_world_gate_check(const fly_world *w, int loc, const fly_airframe *af);

/* Danger as an item level, 1..99: what anything rolled here comes out at. */
int fly_world_item_level(const fly_world *w, float x, float y);
const char *fly_gate_name(uint32_t gate);
const char *fly_loc_kind_name(fly_loc_kind k);

#endif /* FLY_WORLD_H */
