#include "fly_world.h"

#include "fly_river.h"

#include <stdio.h>
#include <string.h>

/* ---------------- terrain ---------------- */

/* --- where the land is ---------------------------------------------------
 *
 * The relief below is a 9 km field, and a 9 km field on a 600 km ball has one
 * consequence that only shows from outside the atmosphere: the sea is a
 * scatter of ponds. It has to be, because the only thing that puts a point
 * under water is the rolling term dipping 120 m below its own mean, and that
 * happens in patches the size of the patches — 2.3% of the planet, in spots a
 * few kilometres across. From orbit that reads as speckle rather than as a
 * world, and no amount of shading fixes it: a planet with no ocean basins in
 * it does not look like one from any distance.
 *
 * So there is a second field under the first, and it does one job — it says
 * whether a place is sea floor or continent. Everything that was already here
 * rides on top of it unchanged, which is what keeps the ground you fly over
 * the same ground: the relief still has its own 9 km rolling, its own ridges
 * and its own mountains, and it is now sitting either on a coastal plain or
 * three kilometres down.
 *
 * Five octaves, from 250 km down to 16 km, and the count is the difference
 * between a coastline and a curve. The relief below runs from 9 km down and
 * the continental field's first octave is 250 km across, so with two or three
 * octaves there is nothing at all in the spectrum between them: the shore
 * follows a smooth contour of a smooth field, wandering by the ±6 km the 9 km
 * relief can push it, which from a hundred and fifty kilometres up is four
 * pixels of wobble on an arc drawn with a compass. The missing band is the
 * one every real coastline is made of — headlands, bays, peninsulas, the sea
 * reaching a hundred kilometres inland — so it is filled here and the ground
 * carries an unbroken spectrum from a quarter of the planet down to 46 m.
 *
 * The curve from sea floor to land is deliberately steep. A continental slope
 * that took the whole range of the field to descend would put the shoreline
 * wherever the 9 km relief happened to cross it, which is an archipelago the
 * width of a continent; at 34 km of elevation per unit of the field the ±380 m
 * the relief can move takes the coast ±6 km, so a coast is a coast with
 * islands off it rather than a smear. Measured over eight seeds this leaves
 * 73.4% of the planet under water — Earth is 71% — in a handful of continents
 * rather than one (`world.planet`).
 *
 * Every constant here is repeated in the GLSL twin (fly_glsl.h) and there is
 * no per-world sea level: the cut is a property of the field, so the two
 * renderers agree by construction rather than through a uniform. */
#define FLY_CONT_SCALE 2.4f  /* cycles of the continental field per radian */
#define FLY_CONT_SEA 0.17f   /* the value of it the shoreline sits at */

float fly_world_continent(uint32_t seed, fly_v3 u) {
    return fly_fbm3(seed + 3, u.x * FLY_CONT_SCALE, u.y * FLY_CONT_SCALE,
                    u.z * FLY_CONT_SCALE, 5) - FLY_CONT_SEA;
}

/* The hypsometric curve: abyssal plain, continental slope, coastal plain
 * rising inland. Two smoothsteps rather than one because the shelf and the
 * interior are different slopes — 34 km of elevation per unit across the
 * shore, 1.1 across the continent — and a single ramp cannot be both.
 *
 * The first ramp lands on zero rather than above it, so what a coast comes
 * ashore onto is the relief's own 120 m of rolling and not that plus a step.
 * It used to arrive at 300 m, which with the interior rise and the mountains
 * on top put the mean of all land at 780 m — every continent a plateau, and
 * from orbit every continent the colour of bare rock, because the treeline
 * ran through the middle of it. */
static float continent_z(float c) {
    return -3400.0f + 3400.0f * fly_smoothstepf(-0.13f, 0.0f, c) +
           450.0f * fly_smoothstepf(0.0f, 0.40f, c);
}

/* --- and where its mountains are ----------------------------------------
 *
 * Mountains were a 15 km ridged field gated by a 26 km one, which is a fair
 * description of what a range looks like and no description at all of where
 * ranges are. Scattered evenly over every continent at 26 km, they came out as
 * a rash: from orbit, thousands of short bright threads above the treeline
 * with no direction to any of them, and nothing that reads as a chain.
 *
 * Ranges are not scattered on a planet, they are built at the edges of
 * continents and where two of them meet, and both of those are things the
 * continental field already knows. Just inland of the shore is where the
 * mountains that face an ocean stand — the Andes, the Cascades, Japan, the
 * Southern Alps — and the deep interior is where the collisions are. Between
 * them lies the low ground, which is where a continent's plains and its rivers
 * are, and it is most of it.
 *
 * So this is a function of `c` and costs nothing: no new octave, no new field,
 * two smoothsteps on a number the terrain has already computed. What it buys
 * is that the snow line has something to trace. Snow follows relief, relief
 * now follows the coast, and a cap on a chain that runs a thousand kilometres
 * down the seaward edge of a continent is what snow looks like from outside a
 * planet — the rash was never about the snow. */
static float orogeny(float c) {
    float coastal = fly_smoothstepf(-0.01f, 0.12f, c) *
                    (1.0f - fly_smoothstepf(0.16f, 0.40f, c));
    float interior = fly_smoothstepf(0.34f, 0.60f, c);
    return fly_clampf(coastal + interior, 0.0f, 1.0f);
}

/* --- and what kind of land it is ----------------------------------------
 *
 * Two fields, both functions of the direction on the ball and nothing else,
 * because that is what a climate is: the noise volume's z is the planet's own
 * axis, so |u.z| is the sine of the latitude and the bands fall where they
 * fall rather than being painted on relative to wherever the chart is centred.
 *
 * Aridity is a noise field plus the two things that actually make deserts.
 * Air that rises at the equator comes down dry at about 25 degrees, which is
 * where most of Earth's are; and rain comes off the sea, so the far interior
 * of a continent is dry whatever latitude it is at, which is where the rest of
 * them are. The noise is what stops the planet being striped: three octaves
 * rather than one, because a single 350 km octave modulating a smooth function
 * of latitude still reads as a smooth function of latitude. Together they put
 * about a sixth of the land in desert, in the two places a desert belongs —
 * measured, 0.34 aridity in the horse latitudes against 0.02 at the equator
 * and 0.31 in the interiors against 0.11 on the coasts — and they leave the
 * coasts green, which is where the game is played.
 *
 * The snow line is one number for both land and sea, which is the whole reason
 * it is written this way: where it falls below the waterline the *sea* is
 * above it, and the polar caps come out of the same expression as the snow on
 * a mountain instead of being a second rule about latitude. It crosses zero at
 * |u.z| = 0.86, so a cap reaches about 59 degrees.
 *
 * The noise on it is not decoration. A snow line that is a pure function of
 * latitude puts the edge of a polar cap on a circle of latitude, drawn with a
 * compass across an ocean, and runs a level contour round every mountain in
 * the temperate zone. Two octaves at 200 km move it ±320 m, which is ±2
 * degrees of latitude at the cap and a ragged treeline everywhere else — the
 * shape a snow line has, for the reason it has it: how much falls varies from
 * one place to the next.
 *
 * 2050 m at the equator is not a free constant: it is the midpoint of the band
 * `terrain_surface` already lays snow over (1850 to 2250), so the mountain
 * that is white from orbit is the mountain that is white from the cockpit. The
 * ground shader does not read this yet and at the latitudes the game is played
 * at it does not need to — the settled world is inside 37 degrees on every
 * seed, where this is 2050 to the metre. Polar ground is the gap, and it is a
 * per-pixel cost in the hottest shading path rather than an oversight; see
 * TODO, `the planet from space`. */
float fly_world_aridity(uint32_t seed, fly_v3 u) {
    float t = (fabsf(u.z) - 0.40f) / 0.20f;
    float n = 0.5f + 0.5f * fly_fbm3(seed + 61, u.x * 1.7f, u.y * 1.7f, u.z * 1.7f, 3);
    float inland = fly_smoothstepf(0.10f, 0.42f, fly_world_continent(seed, u));
    return fly_clampf(n + 0.55f * expf(-t * t) + 0.30f * inland - 0.68f, 0.0f, 1.0f);
}

float fly_world_snowline(uint32_t seed, fly_v3 u) {
    return 2050.0f - 2400.0f * fly_smoothstepf(0.60f, 0.93f, fabsf(u.z)) +
           320.0f * fly_fbm3(seed + 63, u.x * 3.0f, u.y * 3.0f, u.z * 3.0f, 2);
}

/* natural terrain, before settlements grade their aprons flat
 *
 * Sampled on the ball rather than on the chart. `fly_world_sphere_dir` turns
 * a chart position into a direction on the planet and the noise is read in
 * the volume that direction points into, so a chart that comes back on itself
 * lands on the same noise and there is no seam anywhere — by construction,
 * because going all the way round arrives at the same point in the noise
 * domain by arriving at the same point. 2D noise cannot give that: it is not
 * periodic, and the join shows as a wall of discontinuous ground.
 *
 * Each scale is `FLY_PLANET_R / <the old chart divisor>`, so a feature that
 * spanned 9 km of chart now spans 9 km of great circle and the terrain has
 * the same character at the same sizes. Only the realization moved.
 *
 * `f` carries where the chart's centre sits on the ball, as the rotation that
 * puts it there — see fly_chart_frame in fly_world.h for why it has to be a
 * rotation and not an offset added to x and y. */
static float ground_raw(uint32_t seed, fly_chart_frame f, float x, float y) {
    fly_v3 u = fly_world_ball_dir(f, x, y);
    /* the continent this is on, or the basin it is at the bottom of */
    float c = fly_world_continent(seed, u);
    float bs = FLY_PLANET_R / 9000.0f;
    float bx = u.x * bs, by = u.y * bs, bz = u.z * bs;
    float base = fly_fbm3(seed, bx, by, bz, 5);           /* rolling ground */
    float ridge = 1.0f - fabsf(fly_noise3(seed + 77, bx * 0.6f, by * 0.6f, bz * 0.6f)); /* ridged mountains */
    /* what a range looks like, times where ranges are — see orogeny */
    float mountains = ridge * ridge *
                      fly_smoothstepf(0.1f, 0.7f, fly_noise3(seed + 5, bx * 0.35f, by * 0.35f, bz * 0.35f)) *
                      orogeny(c);
    float h = continent_z(c) + 120.0f + base * 260.0f + mountains * 2400.0f;
    /* micro-relief: mid- and fine-scale undulation, faded out near the
     * waterline so shores stay smooth */
    float d1 = FLY_PLANET_R / 210.0f, d2 = FLY_PLANET_R / 46.0f;
    float detail = fly_noise3(seed + 9, u.x * d1, u.y * d1, u.z * d1) * 9.0f +
                   fly_noise3(seed + 10, u.x * d2, u.y * d2, u.z * d2) * 2.2f;
    h += detail * fly_smoothstepf(3.0f, 40.0f, h);
    /* The sea floor is terrain too, and it used to be clamped flat at zero.
     *
     * That put a level bed exactly FLY_WATER_LEVEL under the whole ocean, so
     * every depth-driven term in the water shading — the deep tint, the light
     * the bed returns, the surf band — behaved as though the entire sea were
     * two metres of beach. A coastline came out as a pale shelf running to the
     * horizon with no gradient anywhere in it, and no amount of tuning the
     * water could fix a depth field that was constant.
     *
     * The relief already descends perfectly well on its own: this seed reaches
     * -70 m at its deepest and passes -2 m about a hundred metres off the
     * beach, which is the gradient the shore needs. Nothing here needed adding,
     * only the clamp needed removing. Collision clamps at the surface instead —
     * see fly_world_surface. */
    return h;
}

fly_v3 fly_world_sphere_dir(float x, float y) {
    float d = sqrtf(x * x + y * y);
    float a = d / FLY_PLANET_R;          /* great-circle angle from the origin */
    float sa = sinf(a), ca = cosf(a);
    float ux, uy;
    if (d < 1e-6f) return fly_v3mk(0.0f, 0.0f, 1.0f);
    ux = x / d; uy = y / d;
    return fly_v3mk(sa * ux, sa * uy, ca);
}

fly_chart_frame fly_world_frame(fly_wpos o) {
    float d = sqrtf(o.e * o.e + o.n * o.n);
    fly_chart_frame f;
    f.ux = d > 1e-6f ? o.e / d : 1.0f;
    f.uy = d > 1e-6f ? o.n / d : 0.0f;
    f.sa = sinf(d / FLY_PLANET_R);
    f.ca = cosf(d / FLY_PLANET_R);
    return f;
}

/* Rodrigues about k = (-uy, ux, 0) — the axis across the origin's bearing —
 * through the angle whose sine and cosine the frame carries. Sending the pole
 * (0,0,1) through it gives (sa*ux, sa*uy, ca), which is the origin's own
 * direction, so the frame is the rotation it says it is. */
fly_v3 fly_world_ball_dir(fly_chart_frame f, float x, float y) {
    fly_v3 v = fly_world_sphere_dir(x, y);
    float kv = f.ux * v.y - f.uy * v.x;          /* k . v */
    float w = kv * (1.0f - f.ca);
    return fly_v3mk(v.x * f.ca + f.ux * v.z * f.sa - f.uy * w,
                    v.y * f.ca + f.uy * v.z * f.sa + f.ux * w,
                    v.z * f.ca - (f.ux * v.x + f.uy * v.y) * f.sa);
}

fly_wpos fly_world_chart_wrap(fly_wpos p) {
    float x = p.e, y = p.n;
    float d = sqrtf(x * x + y * y), nd;
    fly_wpos out;
    float c = 2.0f * FLY_CHART_ANTIPODE;   /* one full lap */
    if (d < FLY_CHART_ANTIPODE) return p;
    /* Fold repeatedly so a position far outside still lands in the disc: past
     * the antipode you are coming back, and past the origin you are going out
     * again on the far side. */
    nd = fmodf(d, c);
    if (nd > FLY_CHART_ANTIPODE) nd = c - nd;
    /* Crossing the antipode reverses the bearing; crossing the origin does not.
     * floor(d / antipode) counts the crossings, and its parity is which. */
    {
        float sgn = fmodf(floorf(d / FLY_CHART_ANTIPODE), 2.0f) == 0.0f ? 1.0f : -1.0f;
        out.e = x / d * nd * sgn;
        out.n = y / d * nd * sgn;
    }
    return out;
}

float fly_world_dist(fly_wpos a, fly_wpos b) {
    fly_v3 ua = fly_world_sphere_dir(a.e, a.n);
    fly_v3 ub = fly_world_sphere_dir(b.e, b.n);
    /* atan2 of the sine against the cosine, not acos of the cosine. The angle
     * matters at both ends and acos is badly conditioned at both: within a
     * rounding of 1 it loses most of its digits, which is where every caller in
     * the settled world lives, and within a rounding of -1 it does it again,
     * which is the antipode. Measured, the second one showed: a four-kilometre
     * step near the fold came back as 4005 m. The cross product costs three
     * multiplies and is exact everywhere. */
    return atan2f(fly_v3len(fly_v3cross(ua, ub)), fly_v3dot(ua, ub)) * FLY_PLANET_R;
}

/* The way from one place to another: how far, and which way to point.
 *
 * Both are questions about the ball. The distance is the great-circle one; the
 * direction is the great circle's own initial bearing, written in the chart's
 * local frame at `from` so that a caller which has only ever thought in chart
 * coordinates keeps working without knowing any of this.
 *
 * That frame is the whole of the arithmetic below. At a point r out from the
 * centre the chart's two local axes are the sphere's — outward along the great
 * circle from the centre, and across it — with the second stretched by
 * a/sin(a), a = r/R, because the chart draws a circle of great-circle radius r
 * with a circumference of 2*pi*r where the sphere gives it 2*pi*R*sin(a). So
 * the tangent to the great circle toward `to` is resolved in the sphere's frame
 * and re-expressed in the chart's, which is one scale factor.
 *
 * At the centre there is no such frame, and none is needed: the chart is
 * azimuthal equidistant about that point, so the place at chart (e, n) is
 * exactly hypot(e, n) away in exactly that direction, and the delta is the
 * position. That is not a special case bolted on, it is the definition. */
fly_v2 fly_world_delta(fly_wpos to, fly_wpos from) {
    fly_v3 ua = fly_world_sphere_dir(to.e, to.n);
    fly_v3 ub = fly_world_sphere_dir(from.e, from.n);
    float rb = sqrtf(from.e * from.e + from.n * from.n);
    float d = fly_world_dist(to, from);
    fly_v2 out;
    out.x = out.y = 0.0f;
    if (d < 1e-4f) return out;
    if (rb < 1e-3f) { out.x = to.e; out.y = to.n; return out; }
    {
        float a = rb / FLY_PLANET_R, sa = sinf(a), ca = cosf(a);
        float ux = from.e / rb, uy = from.n / rb;
        fly_v3 sr = fly_v3mk(ca * ux, ca * uy, -sa);   /* outward from the centre */
        fly_v3 st = fly_v3mk(-uy, ux, 0.0f);           /* across it */
        fly_v3 w = fly_v3norm(fly_v3sub(ua, fly_v3scale(ub, fly_v3dot(ua, ub))));
        float k = sa > 1e-6f ? a / sa : 1.0f;
        float wr = fly_v3dot(w, sr), wt = fly_v3dot(w, st) * k;
        float l = sqrtf(wr * wr + wt * wt);
        if (l < 1e-9f) return out;     /* the antipode: every way is the way */
        wr = d * wr / l; wt = d * wt / l;
        out.x = wr * ux - wt * uy;
        out.y = wr * uy + wt * ux;
    }
    return out;
}

fly_wpos fly_wpos_step(fly_wpos p, fly_v2 d) {
    return fly_world_chart_wrap(fly_wpos_mk(p.e + d.x, p.n + d.y));
}

/* Where the chart's centre sits on the ball, chosen so that home is somewhere
 * worth putting a settlement.
 *
 * The terrain's largest features — the ridge field at 15 km and the gate that
 * decides where mountains are at all at 26 km — are the size of the playable
 * world, so which landscape a seed gets is very nearly one draw of one number.
 * Measured over twelve seeds on the chart this replaced, five came out as
 * habitable coastal lowland and the rest as highland with no sea in the
 * settled area at all: means of 452, 467 and 893 m, and worlds the autopilot
 * flies into. The four seeds the suite happens to use were three good draws
 * and one mountain, which is why nothing ever caught it.
 *
 * That was never a property of the sampling, so switching to the ball did not
 * introduce it — it only re-rolled the dice and drew badly. What the ball adds
 * is somewhere to put the fix: a chart centred anywhere on a whole planet can
 * be centred on a habitable place instead of wherever it landed.
 *
 * "Habitable" is deliberately loose — enough water to have a coast, not so
 * much that home is an island, low enough ground to build on, and high enough
 * ground somewhere in it to be worth flying over. Everything inside those
 * bounds is left to vary, because the point is a world worth flying, not
 * twelve copies of one.
 *
 * The floor under the tallest point is the newest of them and it is there
 * because the continents brought orogeny with them: mountains are built at the
 * edges of continents now rather than scattered evenly, and a chart centred on
 * a coast can land on the flat side of one. Without the floor a third of the
 * seeds came out as a plain with a 250 m rise in it — nothing to fly through,
 * and the gallery's alpine vista a hillside. With it, home is the coastal
 * lowland the settlements need with the range that made the coast behind it.
 *
 * With continents in the ground the odds changed and the search had to change
 * with them. Two thirds of the planet is now open ocean and the coast is a
 * line rather than a scatter, so a blind spiral of 400 candidates found a
 * habitable box one or two times in four hundred and on some seeds not at all.
 * The fix is not more full evaluations — each is 576 samples of a
 * thirteen-octave field — it is to ask the cheap question first. The
 * continental field says whether a point is on the shelf before any relief is
 * sampled, so the spiral is ten times as long and all but about one candidate
 * in forty is rejected for a fbm3 and a noise3. Measured, that is a third of
 * the old cost and finds a qualifying home on every seed tried.
 *
 * The two climate terms are in the cheap filter for the same reason they are
 * in the shading: home is where the player sees this world from the ground,
 * and a temperate, watered coast is what the ground shading draws. Putting the
 * chart's centre inside the ice cap would leave the limb white where the
 * terrain under the wheels is a green meadow. */
#define FLY_HOME_HALF 15000.0f   /* the settled area, which is what has to work */
#define FLY_HOME_GRID 24
#define FLY_HOME_SPIRAL 4000     /* candidates visited */
#define FLY_HOME_FULL 240        /* of which at most this many are fully measured */

/* The wet fraction, the mean of the *dry* ground and the tallest point.
 *
 * The mean is over land only, and that is a correction the continents forced:
 * with an ocean three kilometres deep in the box, a plain mean is a measure of
 * how much sea is in shot and says nothing at all about whether the land is
 * flyable. What the settled world needs is that the ground it is built on is
 * low, which is exactly the mean over the points that are ground. */
static void home_stats(uint32_t seed, fly_chart_frame f, float *wet, float *dry, float *hi) {
    int i, j, n = 0, w = 0, d = 0;
    float s = 0.0f, mx = -1e30f;
    for (j = 0; j < FLY_HOME_GRID; ++j)
        for (i = 0; i < FLY_HOME_GRID; ++i) {
            float t = 2.0f * FLY_HOME_HALF / (float)(FLY_HOME_GRID - 1);
            float h = ground_raw(seed, f, -FLY_HOME_HALF + (float)i * t,
                                 -FLY_HOME_HALF + (float)j * t);
            ++n;
            if (h < FLY_WATER_LEVEL) ++w;
            else { s += h; ++d; }
            if (h > mx) mx = h;
        }
    *wet = (float)w / (float)n;
    *dry = d ? s / (float)d : 0.0f;
    *hi = mx;
}

static float over(float v) { return v > 0.0f ? v : 0.0f; }

static fly_wpos pick_origin(uint32_t seed) {
    /* A golden-angle spiral out to the antipode visits the whole planet
     * without clustering, and the seed rotates it so two worlds do not walk
     * the same path. The first candidate that qualifies wins; if a planet is
     * somehow uniformly hostile, the least unsuitable one does — and a
     * candidate that was measured in full always beats one that was rejected
     * on the cheap terms, which is what the thousand in the score is for. */
    fly_wpos best = fly_wpos_mk(0.0f, 0.0f);
    float best_score = -1e30f;
    int k, full = 0;
    for (k = 0; k < FLY_HOME_SPIRAL; ++k) {
        float t = ((float)k + 0.5f) / (float)FLY_HOME_SPIRAL;
        float rad = FLY_CHART_ANTIPODE * sqrtf(t);   /* equal-area over the disc */
        float ang = (float)k * 2.39996323f + (float)(seed % 1000u) * 0.001f;
        fly_wpos o = fly_wpos_mk(rad * cosf(ang), rad * sinf(ang));
        fly_chart_frame f = fly_world_frame(o);
        fly_v3 u = fly_world_ball_dir(f, 0.0f, 0.0f);
        float shore = continent_z(fly_world_continent(seed, u));
        float arid = fly_world_aridity(seed, u);
        float wet, dry, hi, score;
        /* temperate, watered, and within reach of a shoreline */
        score = -(over(fabsf(u.z) - 0.60f) * 100.0f + over(arid - 0.35f) * 100.0f +
                  over(-350.0f - shore) * 0.01f + over(shore - 320.0f) * 0.01f);
        if (score < 0.0f || full >= FLY_HOME_FULL) {
            score -= 1000.0f;
            if (score > best_score) { best_score = score; best = o; }
            continue;
        }
        ++full;
        home_stats(seed, f, &wet, &dry, &hi);
        if (wet > 0.04f && wet < 0.32f && dry < 300.0f && hi > 600.0f && hi < 1800.0f)
            return o;
        /* how far outside the bounds this candidate is, negated */
        score = -(over(0.04f - wet) * 200.0f + over(wet - 0.32f) * 200.0f +
                  over(dry - 300.0f) * 0.01f + over(600.0f - hi) * 0.004f +
                  over(hi - 1800.0f) * 0.002f);
        if (score > best_score) { best_score = score; best = o; }
    }
    return best;
}

/* Where a point falls on one river station's span: how far off the channel's
 * centreline it is, and — through `mix` — where along the span it landed, so
 * the caller can take the surface and the width from the two ends. Returns 0
 * when the span is nowhere near, which is the answer for almost every point in
 * almost every world.
 *
 * The bounding test is the span's own box widened by `pad`, and it is the
 * whole reason this is affordable: a station is two comparisons to reject, and
 * the river's box above it rejects sixty-four of them at once. The GLSL twin
 * in fly_glsl.h is this function line for line. */
static int river_span(const fly_river_pt *a, const fly_river_pt *b, float pad,
                      float x, float y, float *dist, float *mix) {
    float ex = b->x - a->x, ey = b->y - a->y;
    float rx = x - a->x, ry = y - a->y;
    float len2 = ex * ex + ey * ey, t, cx, cy;
    float lo, hi, reach = (a->wide > b->wide ? a->wide : b->wide) + pad;
    lo = a->x < b->x ? a->x : b->x;  hi = a->x > b->x ? a->x : b->x;
    if (x < lo - reach || x > hi + reach) return 0;
    lo = a->y < b->y ? a->y : b->y;  hi = a->y > b->y ? a->y : b->y;
    if (y < lo - reach || y > hi + reach) return 0;
    if (len2 < 1e-3f) return 0;
    t = fly_clampf((rx * ex + ry * ey) / len2, 0.0f, 1.0f);
    cx = rx - ex * t;
    cy = ry - ey * t;
    *dist = sqrtf(cx * cx + cy * cy);
    *mix = t;
    return 1;
}

/* The channel, taken out of the ground the same way a cutting is: a weight
 * that reaches 1 across the floor and falls to nothing at the top of the bank,
 * and a min, so a river can only ever lower ground and never invent a bed
 * standing above the hillside it runs through. */
static float river_carve(const fly_world *w, float x, float y, float h) {
    int r, i;
    for (r = 0; r < w->nriver; ++r) {
        const fly_river *rv = &w->river[r];
        if (x < rv->x0 || x > rv->x1 || y < rv->y0 || y > rv->y1) continue;
        for (i = rv->first; i + 1 < rv->first + rv->count; ++i) {
            const fly_river_pt *a = &w->river_pt[i], *b = &w->river_pt[i + 1];
            float d, t, wide, bt, z, k;
            if (!river_span(a, b, rv->batter, x, y, &d, &t)) continue;
            wide = fly_lerpf(a->wide, b->wide, t);
            bt = fly_river_batter(wide);
            k = fly_smoothstepf(wide + bt, wide, d);
            if (k <= 0.0f) continue;
            /* The cut profile: the floor at the bottom, rising to a lip over
             * the water by the top of the bank — see FLY_RIVER_LIP. Blended
             * back into the hillside by `k`, and taken as a min, so the lip is
             * something the cut may leave standing and never ground it
             * invents. */
            z = fly_lerpf(a->z, b->z, t);
            z += -rv->bed + (rv->bed + FLY_RIVER_LIP) *
                            fly_smoothstepf(wide, wide + bt, d);
            z = fly_lerpf(h, z, k);
            if (z < h) h = z;
        }
    }
    return h;
}

float fly_world_still(const fly_world *w, float x, float y, float gz) {
    float wz = FLY_WATER_LEVEL;
    int i;
    for (i = 0; i < w->nlake; ++i) {
        const fly_lake *L = &w->lake[i];
        float dx = x - L->x, dy = y - L->y, d, z;
        if (fabsf(dx) > L->r || fabsf(dy) > L->r) continue;
        d = sqrtf(dx * dx + dy * dy);
        if (d > L->r) continue;
        /* Let down to under the ground over the outer band of the disc, the
         * same way a channel's is over the outer part of its bank: the disc is
         * a bound on a basin that is not a disc, and a level carried flat to
         * the edge of it is a wall of water wherever the two disagree. */
        z = fly_lerpf(gz - FLY_RIVER_DRY, L->z,
                      fly_smoothstepf(L->r, L->r - FLY_LAKE_EDGE, d));
        if (z > wz) wz = z;
    }
    return wz;
}

float fly_world_water_at(const fly_world *w, float x, float y, float gz) {
    float wz = fly_world_still(w, x, y, gz);
    int r, i;
    for (r = 0; r < w->nriver; ++r) {
        const fly_river *rv = &w->river[r];
        if (x < rv->x0 || x > rv->x1 || y < rv->y0 || y > rv->y1) continue;
        for (i = rv->first; i + 1 < rv->first + rv->count; ++i) {
            const fly_river_pt *a = &w->river_pt[i], *b = &w->river_pt[i + 1];
            float d, t, z, wide, bt, edge;
            if (!river_span(a, b, rv->batter, x, y, &d, &t)) continue;
            wide = fly_lerpf(a->wide, b->wide, t);
            bt = fly_river_batter(wide);
            if (d > wide + bt) continue;
            /* Level across the floor and let down to under the ground over the
             * outer part of the bank — see FLY_RIVER_EDGE, which is where a
             * river's water stops and why it stops that way. */
            edge = fly_smoothstepf(wide + bt, wide + FLY_RIVER_EDGE * bt, d);
            z = fly_lerpf(gz - FLY_RIVER_DRY, fly_lerpf(a->z, b->z, t), edge);
            if (z > wz) wz = z;
        }
    }
    return wz;
}

float fly_world_shore(const fly_world *w, float x, float y) {
    float wz = FLY_WATER_LEVEL;
    int r, i;
    for (i = 0; i < w->nlake; ++i) {
        const fly_lake *L = &w->lake[i];
        float dx = x - L->x, dy = y - L->y;
        if (fabsf(dx) > L->r || fabsf(dy) > L->r) continue;
        if (dx * dx + dy * dy > L->r * L->r) continue;
        if (L->z > wz) wz = L->z;
    }
    for (r = 0; r < w->nriver; ++r) {
        const fly_river *rv = &w->river[r];
        if (x < rv->x0 || x > rv->x1 || y < rv->y0 || y > rv->y1) continue;
        for (i = rv->first; i + 1 < rv->first + rv->count; ++i) {
            const fly_river_pt *a = &w->river_pt[i], *b = &w->river_pt[i + 1];
            float d, t, z, wide;
            if (!river_span(a, b, rv->batter, x, y, &d, &t)) continue;
            wide = fly_lerpf(a->wide, b->wide, t);
            if (d > wide + fly_river_batter(wide)) continue;
            z = fly_lerpf(a->z, b->z, t);
            if (z > wz) wz = z;
        }
    }
    return wz;
}

float fly_world_water(const fly_world *w, float x, float y) {
    /* The ground only where there is water to let down against it: over the
     * open country and the open sea this is four comparisons a river and a
     * disc test a lake, and the heightfield is never touched. */
    int i;
    for (i = 0; i < w->nriver; ++i) {
        const fly_river *rv = &w->river[i];
        if (x >= rv->x0 && x <= rv->x1 && y >= rv->y0 && y <= rv->y1)
            return fly_world_water_at(w, x, y, fly_world_ground(w, x, y));
    }
    for (i = 0; i < w->nlake; ++i) {
        float dx = x - w->lake[i].x, dy = y - w->lake[i].y;
        if (dx * dx + dy * dy <= w->lake[i].r * w->lake[i].r)
            return fly_world_water_at(w, x, y, fly_world_ground(w, x, y));
    }
    return FLY_WATER_LEVEL;
}


int fly_world_wet(const fly_world *w, float x, float y) {
    float h = fly_world_ground(w, x, y);
    return h <= fly_world_water_at(w, x, y, h);
}

float fly_world_ground(const fly_world *w, float x, float y) {
    float h = ground_raw(w->seed, w->frame, x, y);
    int i;
    /* The rivers first, because they are what the country was found with: the
     * pads are graded into ground that already has its valleys, and the roads
     * are surveyed against the result. Nothing overlaps in practice — the
     * survey refuses to bring a channel anywhere near a pad — so the order is
     * a statement about which is a property of the planet rather than an
     * arithmetic dependency. */
    if (w->nriver > 0) h = river_carve(w, x, y, h);
    for (i = 0; i < w->nloc; ++i) {
        const fly_location *L = &w->loc[i];
        float dx = x - L->pos.e, dy = y - L->pos.n;
        if (fabsf(dx) > FLY_PAD_BLEND_R || fabsf(dy) > FLY_PAD_BLEND_R) continue;
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= FLY_PAD_BLEND_R) continue;
        float t = fly_smoothstepf(FLY_PAD_BLEND_R, FLY_PAD_FLAT_R, d);
        h = fly_lerpf(h, L->pad_z, t);
    }
    /* The cuttings, after the aprons and never above them: a cut takes material
     * away and that is all it can do. Written as a min rather than as a blend
     * so that a cut which happens to fall on ground already below its formation
     * — a hollow inside the box, the far end of a ramp — does nothing at all
     * rather than filling it in. See fly_cut. The GLSL twin in fly_glsl.h is
     * this loop line for line; gpu.ground holds the two together. */
    for (i = 0; i < w->ncut; ++i) {
        const fly_cut *c = &w->cut[i];
        float rx = x - c->x, ry = y - c->y;
        float u = rx * c->dx + ry * c->dy;
        float v = rx * -c->dy + ry * c->dx;
        float au = u < 0.0f ? -u : u, av = v < 0.0f ? -v : v;
        float wu, wv, t, z;
        if (au > c->half + c->feather || av > c->wide + c->feather) continue;
        wu = fly_smoothstepf(c->half + c->feather, c->half, au);
        wv = fly_smoothstepf(c->wide + c->feather, c->wide, av);
        if (wu * wv <= 0.0f) continue;
        t = c->half > 1e-3f ? (u / c->half + 1.0f) * 0.5f : 0.5f;
        z = fly_lerpf(c->z0, c->z1, fly_clampf(t, 0.0f, 1.0f));
        z = fly_lerpf(h, z, wu * wv);
        if (z < h) h = z;
    }
    return h;
}

/* What a thing standing or flying here would rest on: the terrain, or the water
 * surface where the terrain is under it. Distinct from fly_world_ground, which
 * is the bed and is what the renderer needs to know how deep the water is. */
float fly_world_surface(const fly_world *w, float x, float y) {
    float h = fly_world_ground(w, x, y), wz = fly_world_water_at(w, x, y, h);
    return h > wz ? h : wz;
}

float fly_world_ground_cb(void *world, float x, float y) {
    return fly_world_surface((const fly_world *)world, x, y);
}

/* Stands, not scatter.
 *
 * A flat per-cell chance put a tree every seventy metres across the whole
 * habitable world: no woods, no meadows, and no edge to anything. Two octaves
 * of noise decide where woodland is instead, and the falloffs decide where it
 * stops — above the treeline, at the waterline, on ground too steep to hold
 * soil, and around the clearing a settlement stands in.
 *
 * The noise comes first so that most of the world costs two octaves rather
 * than a five-octave heightfield sample: at a typical camera two thirds of the
 * candidate points are rejected before `fly_world_ground` is ever called. */
float fly_world_forest_mask(const fly_world *w, float x, float y) {
    float broad = 0.5f + 0.5f * fly_noise2(w->seed + 91, x / 1100.0f, y / 1100.0f);
    float patch = 0.5f + 0.5f * fly_noise2(w->seed + 92, x / 290.0f, y / 290.0f);
    /* Tuned against the distribution, not by eye: over a 40 km transect this
     * leaves 54% of the world open, 30% under closed canopy and 16% in the
     * edge between the two. The first cut ran the smoothstep from 0.30 and
     * split 27/48/24 — half the world permanently half-wooded, which tints
     * everything and distinguishes nothing. A wood needs a meadow next to it
     * before it reads as a wood. */
    return fly_smoothstepf(0.48f, 0.66f, broad * 0.85f + patch * 0.15f);
}

float fly_world_clearing(int kind) {
    switch (kind) {
    case FLY_LOC_CITY: return 330.0f;      /* two rings of towers, out to 235 m */
    case FLY_LOC_REFINERY: return 240.0f;  /* tank farm and its bunds */
    case FLY_LOC_SKYPORT: return 240.0f;   /* the gantry stands well out */
    case FLY_LOC_MINE: return 210.0f;      /* headframe, spoil and haul road */
    case FLY_LOC_RUINS: return 200.0f;     /* a wood takes a ruin back slowly */
    default: return 170.0f;                /* an outpost is a shed and a mast */
    }
}

/* Whose ground this is, and how far out on it.
 *
 * The answer is in units of the winning settlement's own clearing radius
 * rather than in metres, so the nearest settlement is the one whose clearing
 * the point is furthest into rather than the one that happens to be closest —
 * a city three hundred metres away owns ground an outpost two hundred metres
 * away does not.
 *
 * Two fields are hung off it and neither may pay for its own loop: the wood
 * thins inside a clearing, and the fields lie in a belt outside one. `which`
 * comes back with the site that won, because the field pattern is laid out on
 * that settlement's own frame.
 *
 * There is a GLSL twin in fly_glsl.h (`fly_site_clearings`), reading the same
 * radius out of uLoc[i].w. */
static float site_clearings(const fly_world *w, float x, float y, int *which) {
    float best = 1e30f;
    int li, won = -1;
    for (li = 0; li < w->nloc; ++li) {
        float lx = x - w->loc[li].pos.e, ly = y - w->loc[li].pos.n;
        float d2 = lx * lx + ly * ly;
        float reach = fly_world_clearing(w->loc[li].kind);
        float u = d2 / (reach * reach);
        if (u < best) { best = u; won = li; }
    }
    if (which) *which = won;
    return sqrtf(best);
}

/* Whether the ground itself could carry a field, before anything is known
 * about whose it is: above the water — `wz`, which is the water standing here
 * and not sea level, so the bank of a river is a bank and not a shelf of
 * arable — and below the tops, and nothing else.
 *
 * Split from the belt because it is free — the height is already in the
 * caller's hand — and because it is the gate that lets `fly_world_tilth` skip
 * the settlement loop outright. Every other term is a factor no greater than
 * one, so ground this rejects is ground the whole field rejects, and the skip
 * is exact rather than an approximation. On a mountain frame or a coastal one
 * that is the loop gone from every pixel. */
static float tilth_ground(float wz, float gz) {
    return fly_smoothstepf(wz + 2.0f, wz + 12.0f, gz) *
           (1.0f - fly_smoothstepf(540.0f, 880.0f, gz));
}

/* How strongly the ground at `u` clearings out from its settlement is worked.
 *
 * The belt is the whole of the shape and it is deliberately not a disc. Three
 * things narrow it, in the order they are cheapest to ask: how far out the
 * point is, which is already in hand; what the ground is, since nothing is
 * farmed under water or up on the tops; and which quarters are worked at all,
 * which is a broad noise, so a settlement has fields on two sides and rough
 * grazing on the others rather than a ring painted round it. The outer bound
 * is then moved by a second noise, so no parish ends on a circle.
 *
 * Each gate returns early rather than multiplying out, for the reason
 * fly_world_forest orders its own: this is evaluated per pixel of terrain on
 * both renderers, and almost all of the world is not farmland. The cheap
 * rejects are what keep the two noise taps off the other ninety per cent.
 *
 * Twin: `fly_tilth_work` in fly_glsl.h, gate for gate and early-out for
 * early-out. */
static float tilth_work(const fly_world *w, float x, float y, float gz, float u) {
    float t, parish, ragged;
    if (u > FLY_TILTH_OUT / FLY_TILTH_RAGGED) return 0.0f;
    t = tilth_ground(fly_world_shore(w, x, y), gz);
    t *= fly_smoothstepf(FLY_TILTH_IN, FLY_TILTH_FULL, u);
    if (t < 0.02f) return 0.0f;
    parish = 0.5f + 0.5f * fly_noise2(w->seed + 45, x / 1450.0f, y / 1450.0f);
    t *= fly_smoothstepf(0.15f, 0.46f, parish);
    if (t < 0.02f) return 0.0f;
    ragged = 0.5f + 0.5f * fly_noise2(w->seed + 46, x / 620.0f, y / 620.0f);
    t *= 1.0f - fly_smoothstepf(FLY_TILTH_FADE, FLY_TILTH_OUT,
                                u * (FLY_TILTH_RAGGED +
                                     (1.0f - FLY_TILTH_RAGGED) * 2.0f * ragged));
    return t < 0.02f ? 0.0f : t;
}

/* The frame a parish lays its fields out on: where it is anchored, which way
 * it runs, and how big a parcel is. Every part of it is drawn at the
 * settlement's own position rather than at the sample's, which is what makes a
 * parish one pattern and its neighbour a different one. */
static void parish_frame(const fly_world *w, int li, float *sx, float *sy,
                         float *ca, float *sa, float *pw, float *pl) {
    float ang;
    *sx = w->loc[li].pos.e;
    *sy = w->loc[li].pos.n;
    ang = FLY_PI * fly_noise2(w->seed + 47, *sx / 1000.0f, *sy / 1000.0f);
    *ca = cosf(ang);
    *sa = sinf(ang);
    *pw = FLY_PARCEL_MIN +
          FLY_PARCEL_VAR * (0.5f + 0.5f * fly_noise2(w->seed + 48, *sy / 1000.0f,
                                                     *sx / 1000.0f));
    *pl = *pw * FLY_PARCEL_LONG;
}

/* How far the boundaries wander off the ruled grid, in parcel coordinates.
 *
 * A grid ruled both ways reads as a drawing rather than as country, and seven
 * metres over seventy-four is about what a hedge laid along a contour by eye
 * actually does. It is a function of the world position and not of the parcel
 * frame, so the same displacement applies to the paint and to the thorn
 * standing on it — which is the whole reason it is a function at all rather
 * than two noise calls written out twice. */
static void parcel_warp(const fly_world *w, float x, float y, float *dx, float *dy) {
    *dx = 7.0f * fly_noise2(w->seed + 49, x / 74.0f, y / 74.0f);
    *dy = 7.0f * fly_noise2(w->seed + 49, y / 74.0f + 31.0f, x / 74.0f - 17.0f);
}

/* Which parcel a point is in, on the frame the settlement `li` encloses its
 * ground on.
 *
 * A parish is a grid and that is the point of it: the one thing in this world
 * that is not a noise field, so from the air worked ground is the only ground
 * with straight edges on it. What keeps it from reading as graph paper is that
 * every part of the frame belongs to the settlement rather than to the world —
 * its bearing, its parcel size and its origin are all drawn at the site's own
 * position, so two parishes meet at an angle — that the courses are offset
 * like brickwork instead of ruled both ways, and that the boundaries wander a
 * few metres, because a hedge laid by eye is not a straight line.
 *
 * `crop` is one value for the whole parcel: what is standing in it. It is read
 * off the noise at the parcel's own index, so it is constant across a field
 * and independent between neighbours, which is what the eye reads a patchwork
 * as. Twin: `fly_tilth_parcel` in fly_glsl.h. */
void fly_world_parcel(const fly_world *w, int li, float x, float y,
                      float *crop, float *edge) {
    float sx, sy, ca, sa, pw, pl, dx, dy, wx, wy, qx, qy, fx, fy, tx, ty, ex, ey;
    *crop = 0.0f;
    *edge = 0.0f;
    if (li < 0 || li >= w->nloc) return;
    parish_frame(w, li, &sx, &sy, &ca, &sa, &pw, &pl);
    dx = x - sx;
    dy = y - sy;
    qx = dx * ca + dy * sa;
    qy = -dx * sa + dy * ca;
    parcel_warp(w, x, y, &wx, &wy);
    qx += wx;
    qy += wy;
    fy = floorf(qy / pl);
    /* Half a parcel of offset on every other course, so the pattern is
     * brickwork rather than a lattice. fabsf(fmodf) rather than an integer
     * test because the GLSL twin has no integers here and the two have to
     * agree on negative indices. */
    qx += fabsf(fmodf(fy, 2.0f)) * pw * 0.5f;
    fx = floorf(qx / pw);
    tx = qx / pw - fx;
    ty = qy / pl - fy;
    ex = (tx < 0.5f ? tx : 1.0f - tx) * pw;
    ey = (ty < 0.5f ? ty : 1.0f - ty) * pl;
    *edge = ex < ey ? ex : ey;
    *crop = 0.5f + 0.5f * fly_noise2(w->seed + 50, (fx + 0.5f) * 0.8137f,
                                     (fy + 0.5f) * 0.6529f);
}

/* Where a parcel boundary crossing a cell actually runs.
 *
 * A hedge is a line and the field is a distance, and the two are not the same
 * problem. Asking `fly_world_parcel` of scattered candidates and keeping the
 * ones that land near a boundary is rejection sampling against a band five
 * metres wide in a parcel two hundred across: twenty-odd candidates thrown
 * away for every one kept, and a hedge wants a bush every few metres, so the
 * arithmetic does not come out — the first version of this drew about one
 * bush per cell, which is a hedge nobody can see. So the boundary is walked
 * instead of searched for.
 *
 * The walk is done in parcel coordinates, where the boundaries are the lines
 * of the grid, and each point is carried back through the same displacement
 * the paint is drawn with: the warp is added to the sample on the way in, so
 * subtracting it at the walked point is one step of the fixed point and lands
 * the thorn on the ribbon rather than a few metres beside it.
 *
 * Every parcel edge is walked by every cell that overlaps it and each point is
 * kept only by the cell it actually falls in, so the phase is locked to the
 * parcel grid rather than to the caller's lattice: no double-planted bush
 * where two cells meet, and no gap either.
 *
 * `half` is the cell's half extent, `pitch` how far apart the points are along
 * the line, and the return is how many were written, never more than `max`. */
int fly_world_hedge(const fly_world *w, int li, float cx, float cy, float half,
                    float pitch, fly_wpos *out, int max) {
    float sx, sy, ca, sa, pw, pl, wx, wy;
    float bx0, bx1, by0, by1, m;
    int i0, i1, j0, j1, i, j, n = 0;
    if (li < 0 || li >= w->nloc || max <= 0 || pitch < 0.25f) return 0;
    parish_frame(w, li, &sx, &sy, &ca, &sa, &pw, &pl);
    /* The cell in parcel coordinates. A rotated square, so its bounding box is
     * the circle around it: half*sqrt(2), plus what the warp can move a
     * boundary by, so no line that could cross the cell is missed. */
    m = half * 1.41422f + 9.0f;
    {
        float dx = cx - sx, dy = cy - sy;
        float qx = dx * ca + dy * sa, qy = -dx * sa + dy * ca;
        bx0 = qx - m; bx1 = qx + m; by0 = qy - m; by1 = qy + m;
    }
    j0 = (int)floorf(by0 / pl);
    j1 = (int)floorf(by1 / pl);
    i0 = (int)floorf(bx0 / pw) - 1;
    i1 = (int)floorf(bx1 / pw) + 1;
    for (j = j0; j <= j1 && n < max; ++j) {
        float off = fabsf(fmodf((float)j, 2.0f)) * pw * 0.5f;
        float y0 = (float)j * pl;
        for (i = i0; i <= i1 && n < max; ++i) {
            float x0 = (float)i * pw - off;
            int e;
            /* the two low edges of parcel (i, j): between them, every cell
             * walks every boundary exactly once */
            for (e = 0; e < 2 && n < max; ++e) {
                float a0 = e == 0 ? x0 : y0;
                float a1 = e == 0 ? x0 + pw : y0 + pl;
                float lo = e == 0 ? bx0 : by0, hi = e == 0 ? bx1 : by1;
                float t;
                if (a0 < lo) a0 = lo;
                if (a1 > hi) a1 = hi;
                for (t = ceilf(a0 / pitch) * pitch; t <= a1 && n < max; t += pitch) {
                    float qx = e == 0 ? t : x0, qy = e == 0 ? y0 : t;
                    /* back to the world on the ruled line, then one step of
                     * the fixed point to put the point under the drawn
                     * boundary rather than beside it */
                    float px = sx + qx * ca - qy * sa;
                    float py = sy + qx * sa + qy * ca;
                    parcel_warp(w, px, py, &wx, &wy);
                    qx -= wx;
                    qy -= wy;
                    px = sx + qx * ca - qy * sa;
                    py = sy + qx * sa + qy * ca;
                    if (px < cx - half || px >= cx + half) continue;
                    if (py < cy - half || py >= cy + half) continue;
                    out[n++] = fly_wpos_mk(px, py);
                }
            }
        }
    }
    return n;
}

void fly_world_tilth(const fly_world *w, float x, float y, float gz, fly_tilth *out) {
    int li = -1;
    out->work = 0.0f;
    out->crop = 0.0f;
    out->edge = 0.0f;
    out->parish = -1;
    if (tilth_ground(fly_world_shore(w, x, y), gz) < 0.02f) return;
    out->work = tilth_work(w, x, y, gz, site_clearings(w, x, y, &li));
    if (out->work <= 0.0f || li < 0) { out->work = 0.0f; return; }
    out->parish = li;
    fly_world_parcel(w, li, x, y, &out->crop, &out->edge);
}

/* The woodland field down to — but not including — the slope cut.
 *
 * Split out because the slope cut is the one term that costs a *second*
 * heightfield sample, and the canopy proxy below cannot afford it: that one is
 * evaluated per vertex of the shadow cascades' caster grid, and it has a GLSL
 * twin which would have to run `fly_ground` twice to carry it.
 *
 * The mask and the height are arguments rather than lookups so that the
 * ordering both callers depend on stays theirs: the two cheap octaves decide
 * whether the heightfield is worth sampling at all, and neither the mask nor
 * the height is ever evaluated twice on the way here.
 *
 * Not exposed. Two callers, both in this file, and a third answer for "how
 * wooded is this" is exactly the kind of near-duplicate the field does not
 * need in its public interface. */
static float forest_open(const fly_world *w, float x, float y, float gz, float d) {
    float u;
    if (d < 0.03f) return 0.0f;
    /* Nothing grows in the water, and nothing grows on the beach.
     *
     * The band is measured off the water that is actually here rather than off
     * sea level, which for a channel three hundred metres up is not the same
     * number — left as it was, a wood grew straight through every river in the
     * world, because the ground in the channel is high even where it is wet. */
    {
        float wz = fly_world_shore(w, x, y);
        d *= fly_smoothstepf(wz + 1.0f, wz + 8.0f, gz);
    }
    d *= 1.0f - fly_smoothstepf(640.0f, 990.0f, gz);
    if (d < 0.03f) return 0.0f;

    /* The clearing, and it has to be as big as the thing standing in it.
     *
     * One radius for every settlement was 170 m bare thinning to 430, which is
     * right for an outpost and wrong for a city: a city's spire district is
     * laid out on rings at 130 and 235 m, so the outer ring of towers stood in
     * a wood that was still a quarter closed — trees inside the perimeter,
     * between the buildings and across the ground the aircraft come in over.
     * Nobody leaves woodland standing inside an airfield, and the renderer
     * cannot fix it afterwards: what it draws is what this field says.
     *
     * So the clearing is scaled per settlement by how far that kind of
     * settlement builds out. The edge is kept — the point of the original was
     * that a clearing thins rather than ending at a wall — and the ratio
     * between the bare radius and the outer one is kept with it. */
    u = site_clearings(w, x, y, NULL);
    d *= fly_smoothstepf(1.0f, 2.53f, u);
    if (d < 0.03f) return 0.0f;
    /* And a field is cleared ground. The wood gives way to the plough rather
     * than the other way about, which is what puts the treeline of a wood
     * along a hedge instead of running a stand through the middle of a crop —
     * and it is the reason the belt could be widened without the renderer
     * having to know anything about it. */
    d *= 1.0f - fly_smoothstepf(0.10f, 0.45f, tilth_work(w, x, y, gz, u));
    return d < 0.03f ? 0.0f : d;
}

float fly_world_forest(const fly_world *w, float x, float y, float *ground_out) {
    float d = fly_world_forest_mask(w, x, y);
    float gz, slope;
    if (ground_out) *ground_out = 0.0f;
    if (d < 0.03f) return 0.0f;

    gz = fly_world_ground(w, x, y);
    if (ground_out) *ground_out = gz;
    d = forest_open(w, x, y, gz, d);
    if (d <= 0.0f) return 0.0f;

    slope = fabsf(fly_world_ground(w, x + 16.0f, y) - gz);
    d *= 1.0f - fly_smoothstepf(4.0f, 7.5f, slope);
    return d < 0.03f ? 0.0f : d;
}

float fly_world_canopy(const fly_world *w, float x, float y, float gz) {
    return forest_open(w, x, y, gz, fly_world_forest_mask(w, x, y)) * FLY_CANOPY_H;
}

fly_v3 fly_world_ground_normal(const fly_world *w, float x, float y) {
    const float e = 1.0f;
    float hx0 = fly_world_ground(w, x - e, y), hx1 = fly_world_ground(w, x + e, y);
    float hy0 = fly_world_ground(w, x, y - e), hy1 = fly_world_ground(w, x, y + e);
    return fly_v3norm(fly_v3mk(hx0 - hx1, hy0 - hy1, 2.0f * e));
}

int fly_world_ground_safe(const fly_world *w, float x, float y, float max_slope) {
    if (fly_world_wet(w, x, y)) return 0;
    return fly_world_ground_normal(w, x, y).z >= cosf(max_slope);
}

/* Is this footprint clear of the water, with room to spare?
 *
 * Height alone is the wrong test. A metre above the waterline can be a metre
 * inland on a shallow beach or a hand's breadth on a steep one, so a threshold
 * that looks generous still leaves buildings perched on the surf line. Checking
 * a ring around the point instead makes the margin a *distance*: the whole
 * footprint plus a setback has to be out of the water, which puts the same gap
 * between a wall and the sea whatever the shore does. */
static int footprint_dry(const fly_world *w, float x, float y, float setback) {
    static const float ring[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
                                      { 0.7071f, 0.7071f }, { -0.7071f, 0.7071f },
                                      { 0.7071f, -0.7071f }, { -0.7071f, -0.7071f } };
    int i;
    {   float gz = fly_world_ground(w, x, y);
        if (gz <= fly_world_water_at(w, x, y, gz) + 1.2f) return 0; }
    for (i = 0; i < 8; ++i) {
        float rx = x + ring[i][0] * setback, ry = y + ring[i][1] * setback;
        float rg = fly_world_ground(w, rx, ry);
        if (rg <= fly_world_water_at(w, rx, ry, rg) + 0.4f) return 0;
    }
    return 1;
}

int fly_world_settle(const fly_world *w, fly_wpos site, fly_wpos *p, float *z) {
    const float setback = FLY_SHORE_SETBACK;
    /* How far in a piece may be walked. Two bounds, for two different bad
     * outcomes. Never onto the landing deck: a tower parked on the pad is a
     * worse artifact than the one being fixed, and inside that radius the apron
     * is graded to FLY_PAD_MIN_DRY anyway so a candidate there is already dry.
     * And never more than a fraction of the way in: dragging a waterfront block
     * all the way to the pad does not make the settlement look right, it just
     * piles it against the site. A piece that cannot stand near where it was
     * meant to is dropped instead, which costs a fraction of a percent of the
     * architecture and keeps the layout its own shape. */
    float floor_r = FLY_PAD_DECK_R + 18.0f;
    float dx, dy, rad, inner;
    int i, steps;
    if (!w || !p) return 0;
    dx = p->e - site.e;
    dy = p->n - site.n;
    rad = sqrtf(dx * dx + dy * dy);
    inner = rad * 0.6f > floor_r ? rad * 0.6f : floor_r;
    steps = rad > inner ? 5 : 1;
    for (i = 0; i < steps; ++i) {
        float want = rad + (inner - rad) * ((float)i / (float)(steps > 1 ? steps - 1 : 1));
        float s = rad > 1e-4f ? want / rad : 0.0f;
        float x = site.e + dx * s, y = site.n + dy * s;
        if (footprint_dry(w, x, y, setback)) {
            p->e = x;
            p->n = y;
            if (z) *z = fly_world_ground(w, x, y);
            return 1;
        }
    }
    return 0;
}

/* --- which way in ---------------------------------------------------------
 *
 * The cost of one course, measured the way an aeroplane meets it: walk out from
 * the pad along the bearing, and at each step ask how far the ground stands
 * above the slope that ends on the deck. The worst answer along the whole final
 * is the course's cost, because a final is only as flyable as the one rise on
 * it — an average would let a wall through on the strength of the flat ground
 * either side of it.
 *
 * The clearance is added to the ground rather than subtracted from the slope,
 * which is the same arithmetic and the honest way round to read it: what has to
 * fit under the aeroplane is the terrain plus the margin the flight law keeps
 * over it.
 *
 * Eight steps over four kilometres is a sample every five hundred metres. What
 * is being looked for is a ridge across the approach, which is kilometres wide
 * at this range; finer sampling finds the tussock the undercarriage would not
 * have noticed. */
static float approach_cost(const fly_world *w, const fly_location *L, float brg) {
    float cx = cosf(brg), cy = sinf(brg), worst = 0.0f;
    int i;
    for (i = 1; i <= 8; ++i) {
        float t = FLY_APPROACH_FINAL * (float)i / 8.0f;
        float h = fly_world_ground(w, L->pos.e + cx * t, L->pos.n + cy * t);
        float over = h + FLY_APPROACH_CLEAR - (L->pad_z + t * FLY_APPROACH_SLOPE);
        if (over > worst) worst = over;
    }
    return worst;
}

void fly_world_approach_survey(fly_world *w) {
    int i, k;
    if (!w) return;
    for (i = 0; i < w->nloc; ++i)
        for (k = 0; k < FLY_APPROACH_DIRS; ++k)
            w->loc[i].approach[k] =
                approach_cost(w, &w->loc[i],
                              (float)k * (2.0f * FLY_PI / (float)FLY_APPROACH_DIRS));
}

/* How much detour a metre of obstruction is worth.
 *
 * The choice is between courses, and the two things being traded are not the
 * same quantity: metres of ground standing in the way against radians of turn
 * away from the way the aeroplane is already pointing. Twenty-five metres per
 * radian makes a right-angle entry worth about forty metres of rise, and a
 * single step off the direct line worth six — so a clear straight-in is never
 * given up, a hillside on the nose always is, and nothing in between turns the
 * approach into a tour of the valley. */
#define FLY_APPROACH_TURN 25.0f

float fly_world_approach_course(const fly_world *w, int loc, float inbound) {
    const float step = 2.0f * FLY_PI / (float)FLY_APPROACH_DIRS;
    float best_cost = 1e30f, best = inbound;
    int k;
    if (!w || loc < 0 || loc >= w->nloc) return inbound;
    for (k = 0; k < FLY_APPROACH_DIRS; ++k) {
        float brg = (float)k * step;
        float turn = fabsf(fly_wrap_pi(brg - inbound));
        float cost = w->loc[loc].approach[k] + turn * FLY_APPROACH_TURN;
        if (cost < best_cost) { best_cost = cost; best = brg; }
    }
    return best;
}

fly_wpos fly_world_approach_aim(const fly_world *w, int loc, fly_wpos from, float *along) {
    fly_v2 out;
    float dist, inbound, course, phi, len = 0.0f;
    if (along) *along = 0.0f;
    if (!w || loc < 0 || loc >= w->nloc) return from;
    out = fly_world_delta(from, w->loc[loc].pos);   /* pad -> aeroplane */
    dist = hypotf(out.x, out.y);
    if (dist < 1.0f) return w->loc[loc].pos;
    inbound = atan2f(out.y, out.x);
    course = fly_world_approach_course(w, loc, inbound);
    /* How far off the chosen final the aeroplane is, as an angle at the pad.
     *
     * Inside half the survey's own step it *is* on that course — the answers
     * are 22.5 degrees apart, so a smaller difference than that is a difference
     * the survey never measured — and the aim point is the pad, which is the
     * straight-in approach and the common case. Past it the aim slides out
     * along the final, reaching the far end of it at 45 degrees off, so an
     * aeroplane arriving across the approach flies to the start of it and turns
     * on rather than cutting the corner into whatever the course was chosen to
     * avoid. */
    phi = fabsf(fly_wrap_pi(inbound - course));
    len = FLY_APPROACH_FINAL *
          fly_clampf((phi - FLY_PI / (float)FLY_APPROACH_DIRS) / (FLY_PI * 0.25f),
                     0.0f, 1.0f);
    /* And it fades out on short final. Below eight hundred metres the aeroplane
       is committed — the pad is under the nose, the flare is a few seconds away
       and a gate behind it is an instruction to go around — so whatever the
       geometry says, the aim point there is the pad. */
    len *= fly_clampf((dist - 800.0f) / 1200.0f, 0.0f, 1.0f);
    /* Never further out than the aeroplane already is: a gate beyond it is a
       turn away from the pad, and the point of the offset is to arrive. */
    if (len > dist) len = dist;
    if (along) *along = len;
    /* Stepped out on the chart rather than on the ball: four kilometres of it
       is a few metres of tangential scale on a 600 km sphere, which is inside
       the width of the apron this is a course into. Folded, because the chart
       comes back on itself and a site near its edge would otherwise be given an
       aim point off the end of it. */
    return fly_world_chart_wrap(fly_wpos_mk(w->loc[loc].pos.e + cosf(course) * len,
                                            w->loc[loc].pos.n + sinf(course) * len));
}

/* ---------------- weather ---------------- */

/* How much weather there is at an altitude.
 *
 * The field below is a map: it varies in x and y and it was handed a `pos`
 * whose z it never read, so a spacecraft at ninety-six kilometres flew through
 * exactly the storm that was over the ground beneath it. Two things came of
 * that and both are visible in the space frames: the renderer drew rain in
 * vacuum, and `fly_sim_step` buffeted a ship in orbit with a turbulence force
 * proportional to its mass and to nothing else — the one force in the flight
 * model that does not go as air density.
 *
 * Held flat to FLY_WEATHER_TOP and out by FLY_WEATHER_END, rather than tied to
 * the density model. The two are not the same claim: the air thins smoothly
 * all the way up, and *weather* stops at the top of the convective layer,
 * which on this planet is where the cloud deck and the storm tops are. Flat
 * below 6 km is also what keeps this from being a retune — everything the game
 * is flown and gated at is under it, and nothing measured below 6 km moves. */
#define FLY_WEATHER_TOP 6000.0f
#define FLY_WEATHER_END 14000.0f

static float weather_layer(float alt) {
    if (alt <= FLY_WEATHER_TOP) return 1.0f;
    if (alt >= FLY_WEATHER_END) return 0.0f;
    return fly_smoothstepf(FLY_WEATHER_END, FLY_WEATHER_TOP, alt);
}

void fly_world_weather(const fly_world *w, fly_v3 pos, double time_s, fly_weather *out) {
    float t = (float)(time_s / 3600.0); /* weather evolves hourly */
    float layer = weather_layer(pos.z);
    float nx = pos.x / 15000.0f, ny = pos.y / 15000.0f;
    /* prevailing wind rotates slowly with time and varies over the map */
    float dir = fly_noise2(w->seed + 11, nx + t * 0.13f, ny) * FLY_PI;
    /* --- how hard it blows when there is no storm ---
     *
     * This was `4 + 11 * u`, one noise mapped straight onto a narrow, high
     * band, and it meant there was no calm anywhere in the world at any hour
     * of any day. Sampled at pad height over 24 sites and 24 hours of seed
     * 4242: mean 15.1 m/s, a floor of 5.0 and nothing below it, with 29% of
     * the samples over 17.5. That is 29 knots mean and a 10-knot minimum —
     * every arrival in the game was a strong-wind arrival, which is not a
     * difficulty setting anybody chose but the shape of an interpolation.
     *
     * It costs more than it looks. The autopilot's taxi governed itself by
     * *ground* speed, so an aeroplane rolling at six metres a second into a
     * headwind of twenty had twenty-six over the wing and a Skylark stalls at
     * twenty-five: the taxi was flying, and lifted off the apron it had just
     * landed on. (That governor reads airspeed now — see fly_game.c — but it
     * only ever had to because the wind never let up.) The
     * wheels-do-not-go-sideways rule bills
     * drift on touchdown as structure, and there was always drift. And the
     * pilot's guide's own "on a windy day the last fifty metres of the descent
     * take you out of the wind" presumes days that are not windy.
     *
     * Squared, so the same noise spends most of its time near the bottom of
     * the range and reaches the top of it rarely: a light breeze is the
     * ordinary case, a fresh one is common, and a gale is weather rather than
     * climate. The same 2304 samples now read mean 11.6 m/s against 15.1, a
     * floor of 3.7 against 5.0, 29% of them under 7.5 m/s against 0.6%, and
     * 20% over 17.5 against 29%. The storm term below is untouched, so the
     * belt and the cells still blow as hard as they ever did — the difference
     * is that they are now the only thing that does.
     *
     * The floor of three used to carry a second argument, and that argument was
     * wrong. It said the floor could not come down because taking it out moved
     * the fleet — `game.pilots` going from three aircraft written off in its
     * twelve-minute window to nine, and 0.04% of airborne time past
     * never-exceed to 0.06% — so smooth air was taking the brake off a pilot
     * law that leaned on turbulence for its speed discipline. Repeated over 128
     * seeds and twenty minutes each, that is one world's luck: the same law
     * spends 3.64 hp an airborne hour past the line under this weather and 3.37
     * with the floor at zero. The structural half of the complaint was real and
     * is fixed where it belonged — `keep_it_flying` defends never-exceed
     * itself now, on the trend rather than on the needle — and with it in, zero
     * costs 2.95 against three's 2.57, below the old law at either floor. See
     * the changelog's *Pilots and the fleet*.
     *
     * So this is a weather number and only a weather number. It stays at three
     * because a world with no calm in it was the defect being fixed and a world
     * with no wind in it is not obviously better, not because anything in
     * fly_pilot needs it. */
    float u = fly_clampf(0.5f + 0.5f * fly_noise2(w->seed + 12, nx - t * 0.07f, ny + t * 0.05f),
                         0.0f, 1.0f);
    float speed = 3.0f + 10.5f * u * u;
    /* storm cells drift through; the storm belt is always rough */
    float cell = 0.5f + 0.5f * fly_noise2(w->seed + 13, nx * 2.0f + t * 0.21f, ny * 2.0f - t * 0.11f);
    float belt = fly_smoothstepf(w->storm_belt_w, w->storm_belt_w * 0.25f,
                                 fabsf(pos.y - w->storm_belt_y));
    float storm = fly_clampf(fly_smoothstepf(0.62f, 0.95f, cell) + belt, 0.0f, 1.0f);
    speed += storm * 16.0f;
    speed *= layer;
    out->wind = fly_v3mk(cosf(dir) * speed, sinf(dir) * speed, 0.0f);
    out->turbulence = fly_clampf(0.08f + storm * 0.85f + speed * 0.006f, 0.0f, 1.0f) * layer;
    out->precip = fly_clampf(storm * 1.2f - 0.15f, 0.0f, 1.0f) * layer;
    /* Visibility above the weather is the clear-air figure and not more: it
     * feeds haze, and how far you can see through air you are above is the
     * atmosphere's own question, answered by the scattering integral. */
    out->visibility = fly_lerpf(18000.0f, 900.0f, storm * layer);
}

float fly_world_danger(const fly_world *w, float x, float y) {
    /* hostile pirate activity clusters away from the origin city */
    float d = fly_v3len(fly_v3mk(x, y, 0)) / FLY_WORLD_HALF;
    float n = 0.5f + 0.5f * fly_noise2(w->seed + 21, x / 8000.0f, y / 8000.0f);
    float base = fly_clampf(d * 0.55f + n * 0.55f - 0.25f, 0.0f, 1.0f);
    /* The permanent belt, and what the nearest site demands before it will let
     * anything land. Both are things that were already true about the map, and
     * folding them in here rather than into a second field is deliberate: the
     * places that breed pirates and the places that drop good gear have to be
     * the same places, or the ladder is decoration. A site only reachable with
     * three kinds of kit is a site worth reaching. */
    float belt = 1.0f - fly_clampf(fabsf(y - w->storm_belt_y) /
                                   (w->storm_belt_w + 1.0f), 0.0f, 1.0f);
    float gated = 0.0f;
    int i;
    for (i = 0; i < w->nloc; ++i) {
        float near = 1.0f - fly_clampf(fly_world_dist(fly_wpos_mk(x, y), w->loc[i].pos)
                                       / 26000.0f, 0.0f, 1.0f);
        uint32_t gm = w->loc[i].gates;
        float bits = 0.0f;
        while (gm) { bits += (float)(gm & 1u); gm >>= 1; }
        near *= fly_clampf(bits * 0.26f, 0.0f, 1.0f);
        if (near > gated) gated = near;
    }
    if (belt > base) base = belt;
    if (gated > base) base = gated;
    return fly_clampf(base, 0.0f, 1.0f);
}

int fly_world_item_level(const fly_world *w, float x, float y) {
    int l = 1 + (int)(fly_world_danger(w, x, y) * 98.0f + 0.5f);
    return l < 1 ? 1 : l > 99 ? 99 : l;
}

/* ---------------- generation ---------------- */

static const char *syl_a[] = { "Kar", "Vel", "Nor", "Zeph", "Auro", "Mira", "Tal", "Oro",
                               "Hex", "Lum", "Cin", "Vor", "Sky", "Erid", "Jun", "Pax" };
static const char *syl_b[] = { "dan", "mir", "veth", "ral", "ka", "dros", "lin", "goro",
                               "than", "bex", "nova", "rell", "spire", "holm", "dine", "vale" };

/* A name nobody else in this world already has.
 *
 * Sixteen syllables each way is 256 names for 24 sites, and the birthday bound
 * puts a collision in about two thirds of worlds — measured: 26 of 40, with 41
 * colliding pairs between them. Two sites called Junspire is not cosmetic. The
 * routes list, the contract board, the chart and every line of the log name a
 * place and nothing else, so a duplicate makes a contract genuinely ambiguous:
 * the player cannot tell which one the job means, and the autopilot silently
 * takes whichever index the offer happened to carry.
 *
 * Exactly two draws, whatever happens. Redrawing from `rng` until it settles
 * was the obvious fix and the wrong one: it consumes a variable number of
 * values, so the first collision in a world shifts every position, economy and
 * gate generated after it — the same seed becomes a different world. The
 * resolution walks the syllable table instead, deterministically and without
 * touching the stream, so worlds that never collided are byte-identical and the
 * ones that did differ only in the name that was broken. */
static void gen_name(fly_rng *rng, char *out, size_t n,
                     const fly_location *taken, int ntaken) {
    uint32_t ia = fly_rng_range(rng, 16), ib = fly_rng_range(rng, 16);
    int step, k;
    for (step = 0; step < 256; ++step) {
        int clash = 0;
        uint32_t a = (ia + (uint32_t)step / 16u) & 15u;
        uint32_t b = (ib + (uint32_t)step) & 15u;
        snprintf(out, n, "%s%s", syl_a[a], syl_b[b]);
        for (k = 0; k < ntaken; ++k)
            if (strcmp(out, taken[k].name) == 0) { clash = 1; break; }
        if (!clash) return;
    }
}

/* per-kind economy profile: production/consumption per hour + stock targets */
static void loc_economy(fly_location *L, fly_rng *rng) {
    float s = fly_rng_span(rng, 0.8f, 1.3f); /* size factor */
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r) {
        L->prod[r] = L->cons[r] = 0.0f;
        L->target[r] = 20.0f * s;
    }
    switch (L->kind) {
    case FLY_LOC_CITY:
        /* the demand hub: soaks up fuel, alloy and salvaged data */
        L->cons[FLY_RES_FUEL] = 9.0f * s; L->cons[FLY_RES_ALLOY] = 4.5f * s;
        L->cons[FLY_RES_DATA] = 1.2f * s;
        L->prod[FLY_RES_LUX] = 1.5f * s;
        L->target[FLY_RES_FUEL] = 120.0f * s;
        break;
    case FLY_LOC_OUTPOST:
        L->cons[FLY_RES_LUX] = 1.2f * s; L->cons[FLY_RES_FUEL] = 1.5f * s;
        L->prod[FLY_RES_PARTS] = 1.8f * s;
        break;
    case FLY_LOC_MINE:
        L->prod[FLY_RES_ORE] = 6.0f * s;
        L->cons[FLY_RES_PARTS] = 2.4f * s; L->cons[FLY_RES_FUEL] = 2.0f * s;
        L->target[FLY_RES_ORE] = 80.0f * s;
        break;
    case FLY_LOC_REFINERY:
        L->cons[FLY_RES_ORE] = 5.0f * s;
        L->prod[FLY_RES_ALLOY] = 2.5f * s; L->prod[FLY_RES_FUEL] = 7.0f * s;
        L->target[FLY_RES_FUEL] = 160.0f * s; L->target[FLY_RES_ORE] = 90.0f * s;
        break;
    case FLY_LOC_RUINS:
        L->prod[FLY_RES_DATA] = 0.8f * s;
        L->cons[FLY_RES_PARTS] = 1.0f * s;
        L->target[FLY_RES_DATA] = 12.0f * s;
        break;
    default: /* skyport */
        L->cons[FLY_RES_LUX] = 1.2f * s; L->cons[FLY_RES_DATA] = 0.8f * s;
        L->prod[FLY_RES_FUEL] = 3.0f * s;
        L->target[FLY_RES_LUX] = 30.0f * s;
        break;
    }
    /* start near equilibrium-ish stock */
    for (r = 0; r < FLY_RES_COUNT; ++r)
        L->stock[r] = L->target[r] * fly_rng_span(rng, 0.25f, 1.1f);
}

void fly_world_gen(fly_world *w, uint32_t seed) {
    memset(w, 0, sizeof *w);
    w->seed = seed;
    /* before anything samples the terrain: the origin is what "here" means */
    w->origin = pick_origin(seed);
    w->frame = fly_world_frame(w->origin);
    fly_rng rng;
    fly_rng_seed(&rng, seed);
    w->storm_belt_y = fly_rng_span(&rng, -0.55f, 0.55f) * FLY_WORLD_HALF;
    w->storm_belt_w = fly_rng_span(&rng, 3200.0f, 5200.0f);

    /* home city near the origin, always known; keep it on dry land */
    fly_location *home = &w->loc[w->nloc++];
    home->kind = FLY_LOC_CITY;
    gen_name(&rng, home->name, sizeof home->name, w->loc, w->nloc - 1);
    {
        int htry;
        for (htry = 0; htry < 48; ++htry) {
            float rr = 700.0f + (float)htry * 90.0f; /* spiral outward until dry */
            float ha = fly_rng_span(&rng, 0, 2.0f * FLY_PI);
            home->pos = fly_wpos_mk(cosf(ha) * rr * fly_rng_span(&rng, 0.3f, 1.0f),
                                    sinf(ha) * rr * fly_rng_span(&rng, 0.3f, 1.0f));
            if (ground_raw(seed, w->frame, home->pos.e, home->pos.n) > FLY_WATER_LEVEL + 4.0f) break;
        }
    }
    home->pad_z = ground_raw(seed, w->frame, home->pos.e, home->pos.n);
    if (home->pad_z < FLY_PAD_MIN_DRY) home->pad_z = FLY_PAD_MIN_DRY;
    home->elev = home->pad_z;
    home->discovered = 1;
    loc_economy(home, &rng);

    static const fly_loc_kind plan[] = {
        FLY_LOC_CITY, FLY_LOC_CITY, FLY_LOC_OUTPOST, FLY_LOC_OUTPOST, FLY_LOC_OUTPOST,
        FLY_LOC_OUTPOST, FLY_LOC_MINE, FLY_LOC_MINE, FLY_LOC_MINE, FLY_LOC_REFINERY,
        FLY_LOC_REFINERY, FLY_LOC_RUINS, FLY_LOC_RUINS, FLY_LOC_RUINS, FLY_LOC_SKYPORT,
        FLY_LOC_SKYPORT, FLY_LOC_OUTPOST, FLY_LOC_MINE, FLY_LOC_RUINS, FLY_LOC_CITY,
        FLY_LOC_REFINERY, FLY_LOC_OUTPOST, FLY_LOC_SKYPORT
    };
    int i;
    for (i = 0; i < (int)(sizeof plan / sizeof plan[0]) && w->nloc < FLY_MAX_LOC; ++i) {
        fly_location *L = &w->loc[w->nloc];
        L->kind = plan[i];
        gen_name(&rng, L->name, sizeof L->name, w->loc, w->nloc);
        /* scatter with minimum spacing, further-out rings hold rarer sites */
        int tries;
        fly_wpos fallback = fly_wpos_mk(0, 0);
        int have_fallback = 0;
        for (tries = 0; tries < 48; ++tries) {
            float rmin = (L->kind == FLY_LOC_RUINS || L->kind == FLY_LOC_SKYPORT) ? 0.45f : 0.08f;
            float rad = fly_rng_span(&rng, rmin, 0.95f) * FLY_WORLD_HALF;
            float ang = fly_rng_span(&rng, 0, 2.0f * FLY_PI);
            fly_wpos p = fly_wpos_mk(cosf(ang) * rad, sinf(ang) * rad);
            int ok = 1, j;
            for (j = 0; j < w->nloc; ++j)
                if (fly_world_dist(p, w->loc[j].pos) < 3000.0f) { ok = 0; break; }
            if (!ok) continue;
            /* keep the first well-spaced spot as a fallback, but prefer dry
             * land so pads are not stranded in open water */
            if (!have_fallback) { fallback = p; have_fallback = 1; }
            L->pos = p;
            if (ground_raw(seed, w->frame, p.e, p.n) > FLY_WATER_LEVEL + 3.0f) break;
        }
        if (have_fallback &&
            ground_raw(seed, w->frame, L->pos.e, L->pos.n) <= FLY_WATER_LEVEL + 3.0f)
            L->pos = fallback;
        L->pad_z = ground_raw(seed, w->frame, L->pos.e, L->pos.n);
        if (L->pad_z < FLY_PAD_MIN_DRY) L->pad_z = FLY_PAD_MIN_DRY; /* berm above water */
        L->elev = L->pad_z;

        /* gates from placement + kind */
        if (L->elev > 1500.0f) L->gates |= FLY_GATE_HIGHALT;
        if (fabsf(L->pos.n - w->storm_belt_y) < w->storm_belt_w) L->gates |= FLY_GATE_STORM;
        if (fly_world_dist(L->pos, fly_wpos_mk(0, 0)) > 0.72f * FLY_WORLD_HALF) L->gates |= FLY_GATE_LONGRANGE;
        if (L->kind == FLY_LOC_RUINS || (L->kind == FLY_LOC_OUTPOST && fly_rng_f01(&rng) < 0.4f))
            L->gates |= FLY_GATE_VTOL;
        if (L->kind == FLY_LOC_SKYPORT) { L->gates |= FLY_GATE_HIGHALT; if (L->elev < 1500.0f) L->elev = fly_rng_span(&rng, 1700.0f, 2400.0f); }

        loc_economy(L, &rng);
        ++w->nloc;
    }

    /* And now the water. After the sites, because a channel keeps out of a
     * settlement's clearing rather than the other way about — the pad grades
     * the heightfield and the river cuts it, and the two arguing over the same
     * ground is a river through the aerodrome. Before everything else in the
     * game, because a river is what the country was found with: the rail and
     * the roads are surveyed against ground that already has one, which is the
     * whole reason fly_road's ford limit and the rail's piers exist.
     *
     * It draws nothing off `rng` — the survey is a property of the heightfield
     * and of nothing else — so the names, the economy and the ownership below
     * are the same with rivers in the world as they were without them. */
    fly_river_lay(w);

    /* Who holds all of it, and who is speaking to whom. Last, because it reads
     * the finished site list — and it draws nothing off `rng`, so a world
     * generated before there were factions in it generates the same terrain,
     * the same sites and the same names as one generated after. */
    fly_faction_world_init(w);
}

/* ---------------- economy ---------------- */

static const char *res_names[FLY_RES_COUNT] = {
    "fuel", "parts", "ore", "alloy", "data", "luxuries"
};
static const float res_price[FLY_RES_COUNT] = { 2.0f, 14.0f, 7.0f, 24.0f, 65.0f, 90.0f };
static const float res_mass[FLY_RES_COUNT] = { 1.0f, 4.0f, 8.0f, 5.0f, 0.5f, 2.0f };

const char *fly_resource_name(fly_resource r) { return r < FLY_RES_COUNT ? res_names[r] : "?"; }
float fly_resource_base_price(fly_resource r) { return r < FLY_RES_COUNT ? res_price[r] : 0; }
float fly_resource_mass(fly_resource r) { return r < FLY_RES_COUNT ? res_mass[r] : 1; }

float fly_world_price(const fly_world *w, int loc, fly_resource r) {
    const fly_location *L = &w->loc[loc];
    float target = L->target[r] > 1.0f ? L->target[r] : 1.0f;
    float fill = fly_clampf(L->stock[r] / target, 0.0f, 2.0f);
    /* scarcity multiplies price up to 4x, glut discounts to 0.4x; net
     * consumers pay more, net producers sell cheap */
    float scarcity = fill < 1.0f ? 1.0f + (1.0f - fill) * 3.0f : 1.0f - (fill - 1.0f) * 0.6f;
    float role = 1.0f + (L->cons[r] - L->prod[r]) * 0.03f;
    float p = fly_resource_base_price(r) * scarcity * fly_clampf(role, 0.7f, 1.6f);
    return p < 0.5f ? 0.5f : p;
}

void fly_world_economy_step(fly_world *w, float dt_hours) {
    int i, r;
    for (i = 0; i < w->nloc; ++i) {
        fly_location *L = &w->loc[i];
        for (r = 0; r < FLY_RES_COUNT; ++r) {
            float prod = L->prod[r];
            /* refineries need ore to run */
            if (L->kind == FLY_LOC_REFINERY && r != FLY_RES_ORE && L->stock[FLY_RES_ORE] < 0.5f)
                prod *= 0.15f;
            L->stock[r] += (prod - L->cons[r]) * dt_hours;
            if (L->stock[r] < 0.0f) L->stock[r] = 0.0f;
            if (L->stock[r] > L->target[r] * 3.0f) L->stock[r] = L->target[r] * 3.0f;
        }
    }
}

/* ---------------- queries ---------------- */

int fly_world_nearest(const fly_world *w, fly_wpos pos, int discovered_only) {
    int best = -1, i;
    float bd = 1e30f;
    for (i = 0; i < w->nloc; ++i) {
        if (discovered_only && !w->loc[i].discovered) continue;
        float d = fly_world_dist(pos, w->loc[i].pos);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int fly_world_discover(fly_world *w, fly_wpos pos, float radius) {
    int found = 0, i;
    for (i = 0; i < w->nloc; ++i) {
        if (w->loc[i].discovered) continue;
        float d = fly_world_dist(pos, w->loc[i].pos);
        if (d <= radius) { w->loc[i].discovered = 1; ++found; }
    }
    return found;
}

uint32_t fly_world_gate_check(const fly_world *w, int loc, const fly_airframe *af) {
    const fly_location *L = &w->loc[loc];
    if ((L->gates & FLY_GATE_VTOL) && !af->vtol) return FLY_GATE_VTOL;
    if ((L->gates & FLY_GATE_HIGHALT) && af->ceiling < L->elev + 500.0f) return FLY_GATE_HIGHALT;
    if ((L->gates & FLY_GATE_STORM) && af->weather_rating < 0.55f) return FLY_GATE_STORM;
    if (L->gates & FLY_GATE_LONGRANGE) {
        /* rough range: fuel / burn * cruise speed must beat the distance out */
        float endurance = af->burn_rate > 1e-6f ? af->fuel_cap / af->burn_rate : 1e9f;
        float range = endurance * af->vref * 0.85f;
        float dist = fly_world_dist(L->pos, fly_wpos_mk(0, 0));
        if (range < dist * 1.35f) return FLY_GATE_LONGRANGE;
    }
    (void)w;
    return 0;
}

const char *fly_gate_name(uint32_t gate) {
    switch (gate) {
    case FLY_GATE_VTOL: return "VTOL pad only";
    case FLY_GATE_HIGHALT: return "high altitude";
    case FLY_GATE_STORM: return "storm belt";
    case FLY_GATE_LONGRANGE: return "long range";
    default: return "open";
    }
}

const char *fly_loc_kind_name(fly_loc_kind k) {
    static const char *names[FLY_LOC_KIND_COUNT] = {
        "city", "outpost", "mine", "refinery", "ruins", "skyport"
    };
    return (int)k < FLY_LOC_KIND_COUNT ? names[k] : "?";
}
