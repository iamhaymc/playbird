#include "fly_road.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* ---------------- one road ----------------
 *
 * The survey is fly_rail_lay's; what is left here is everything that makes the
 * line a road rather than a rail.
 *
 * The profile is a taut line over a shelf: made ground across the hollows, and
 * — since the heightfield learned to take a corridor down as well as build one
 * up — a cutting through the crests. Both, because a road that can only build
 * up is a road that goes over everything in its way.
 *
 * The cuttings are rationed, so the two are not symmetric in the code. Fill is
 * drawn geometry and costs nothing but triangles; a cut has to be a box the
 * heightfield carries to both renderers, and there are only so many of those.
 * So every station says how deep a cutting it *wants*, the network spends a
 * bounded budget on the best of them, and every road standing on ground that
 * moved is surveyed again against what it actually got. A road that assumed a
 * cutting it did not get would be a road inside a hill, and the terrain wins
 * the depth test.
 *
 * Water is a causeway and only briefly. A road that crossed open sea would need
 * to be a bridge, a bridge is a structure rather than a line, and a network
 * that quietly built four kilometres of one across a bay would be lying about
 * what it is. Past FLY_ROAD_FORD the answer is that there is no road. */

/* The surveyor's notes, one set per road.
 *
 * `shelf` is the ground each station's deck has to clear, `chain` the distance
 * along, `want_cut` how deep a cutting that station would take if the world
 * could afford one, and `want_z` the profile it would have had if it got it.
 * All four are filled by the drape and read by road_cut_budget, which is why
 * they outlive one road: the budget is spent across the whole network at once
 * and the second survey has to know what the first one asked for.
 *
 * File-static rather than a member of `fly_road`, which is where it used to be.
 * It is a quarter of a megabyte, it is meaningless the instant fly_road_build
 * returns, and on the struct it was carried in every fly_game for the rest of
 * the session and copied with every one of them. Nothing outside this file has
 * ever read it. */
static struct {
    float shelf[FLY_ROAD_POINT_MAX];
    float chain[FLY_ROAD_POINT_MAX];
    float want_cut[FLY_ROAD_POINT_MAX];
    float want_z[FLY_ROAD_POINT_MAX];
    /* The other bound: how high the deck may be at each station, which is set
     * by whatever is over it — the guideway it passes under, and the height an
     * earlier road holds where the two meet. FLT_MAX is open sky, which is what
     * nearly every station on nearly every road has. `bound` marks the stations
     * where the ceiling is the thing deciding the profile rather than the
     * shelf, because those are the ones whose earthwork the budget cannot
     * refuse: a road that has to get under a structure and did not get its
     * cutting is a road driving through the structure. */
    float ceiling[FLY_ROAD_POINT_MAX];
    /* And where the deck is not merely held under something but held *to* it:
     * the height an earlier road has where the two cross, which is what makes a
     * crossing a crossroads instead of one carriageway passing through another
     * a couple of metres up. -FLT_MAX at every station that crosses nothing. */
    float pin[FLY_ROAD_POINT_MAX];
    unsigned char bound[FLY_ROAD_POINT_MAX];
} road_work[FLY_ROAD_MAX];

/* What laying one link needs to know about the world it is being laid into:
 * the ground, the guideway that was there first, and the roads laid before it.
 * One struct rather than three parameters threaded through four functions —
 * and a parameter rather than file scope, because "what else is on the ground"
 * is an input to a survey and not a property of the module. */
typedef struct {
    const fly_world *w;
    const fly_rail_route *rail;
    const fly_road_net *net;   /* the network as far as it has been laid */
} road_ctx;

/* Longest unbroken run of water on the straight line between two sites, in
 * metres. A cheap veto before the expensive survey: the search can go round a
 * bay, but it cannot go round an ocean, and a chord that spends kilometres wet
 * is the ocean case almost every time.
 *
 * Wet, rather than under the sea, and the difference is the case this was
 * written for and had never met: a river three hundred metres wide across the
 * chord is a crossing the survey can go round or ford, and an arm of the sea
 * four kilometres of it is not — the question is the same one either way and
 * it is about water, not about altitude. */
static float chord_water(const fly_world *w, fly_wpos a, fly_wpos b) {
    float dx = b.e - a.e, dy = b.n - a.n;
    float len = sqrtf(dx * dx + dy * dy), run = 0.0f, worst = 0.0f;
    int i, n;
    if (len < 1.0f) return 0.0f;
    n = (int)(len / 150.0f) + 2;
    for (i = 0; i <= n; ++i) {
        float t = (float)i / (float)n;
        float x = a.e + dx * t, y = a.n + dy * t;
        if (fly_world_wet(w, x, y)) {
            run += len / (float)n;
            if (run > worst) worst = run;
        } else {
            run = 0.0f;
        }
    }
    return worst;
}

/* --- the shelf ------------------------------------------------------------
 *
 * The deck is level across, and it sits on the highest ground under its own
 * width rather than on the height at its centreline. On the flat those are the
 * same number; on the side of a mountain they are not, and the difference is
 * the whole of what a hill road looks like — the uphill edge meets the slope,
 * the downhill edge stands off it, and the wall between the deck and the ground
 * under it is the thing that was built.
 *
 * It reads `fly_world_ground`, so once a cutting has been published the shelf
 * inside it is the formation rather than the ridge that used to be there. That
 * is exactly why the surveyor asks for the shelf twice — once against the land
 * as found, and once against the land as it will be.
 *
 * The eight-way sample a little beyond the carriageway is not for the road's
 * sake but for the terrain's: the heightfield is drawn through rings that
 * coarsen with distance, so the *drawn* ground at a point can stand above what
 * the height function reports by about what it varies across the finest ring's
 * cell. A deck laid to the exact heightfield is swallowed by its own hillside a
 * kilometre away, and a road that comes apart into fragments as you climb away
 * from it is worse than no road. Thirteen metres is that cell, and it is the
 * same neighbourhood `ground_bias` lifts the settlements' pavement by. */
static float road_shelf(const fly_world *w, const fly_road_point *pts, int n, int i) {
    fly_v3 p = pts[i].pos, q = i + 1 < n ? pts[i + 1].pos : pts[i - 1].pos;
    float ex = q.x - p.x, ey = q.y - p.y, len = hypotf(ex, ey);
    float nx = len > 1e-3f ? -ey / len * FLY_ROAD_HALF : 0.0f;
    float ny = len > 1e-3f ? ex / len * FLY_ROAD_HALF : 0.0f;
    float hi = fly_world_ground(w, p.x, p.y);
    float gl = fly_world_ground(w, p.x - nx, p.y - ny);
    float gr = fly_world_ground(w, p.x + nx, p.y + ny);
    int a;
    if (gl > hi) hi = gl;
    if (gr > hi) hi = gr;
    for (a = 0; a < 8; ++a) {
        float th = (float)a * (FLY_PI / 4.0f);
        float gz = fly_world_ground(w, p.x + cosf(th) * FLY_ROAD_SHELF_R,
                                       p.y + sinf(th) * FLY_ROAD_SHELF_R);
        if (gz > hi) hi = gz;
    }
    /* A causeway, and over whatever water is actually here: the sea at the
     * coast, and a river's own surface where the road fords one. The deck
     * clears the water rather than clearing sea level, which for a channel
     * three hundred metres up is not the same number. */
    {
        float wz = fly_world_water(w, p.x, p.y);
        if (hi < wz) hi = wz + 0.6f;
    }
    return hi + FLY_ROAD_DECK;
}

/* --- what is over the road -------------------------------------------------
 *
 * Two things can be, and the answer to both is a height the deck may not
 * exceed: the guideway, which a road passes *under* because a pipe on piers is
 * five to thirteen metres up and going over it would be a flyover nobody
 * builds for a haul road, and an earlier road, which a later one meets at its
 * own height because two carriageways crossing at different heights is one
 * deck passing through the other.
 *
 * Both are measured off the surveyed centrelines rather than off the drawn
 * geometry: this runs while the world is being built and the drawing does not
 * exist yet. */

/* How far this point is from the guideway, and how high the guideway's axis is
 * where it passes. FLT_MAX when there is no line to be near. */
static float rail_near(const fly_rail_route *rail, float x, float y, float *deck) {
    float best = FLT_MAX, bz = 0.0f;
    int i;
    if (!rail || rail->point_count < 2) return FLT_MAX;
    for (i = 0; i + 1 < rail->point_count; ++i) {
        fly_v3 a = rail->points[i].pos, b = rail->points[i + 1].pos;
        float ex = b.x - a.x, ey = b.y - a.y, ll = ex * ex + ey * ey, t, d;
        if (ll < 1e-6f) continue;
        t = fly_clampf(((x - a.x) * ex + (y - a.y) * ey) / ll, 0.0f, 1.0f);
        d = hypotf(x - (a.x + ex * t), y - (a.y + ey * t));
        if (d < best) { best = d; bz = fly_lerpf(a.z, b.z, t); }
    }
    if (deck) *deck = bz;
    return best;
}

/* The two bounds on the profile, for a whole road at once, written into the
 * surveyor's notes: how high the deck may be at each station, and where it is
 * held to a height rather than under one.
 *
 * Both hang on a crossing rather than on proximity, and they are different
 * rules after that because the two things in a road's way are different things.
 * The guideway is a structure with a width, so what it imposes is a clearance
 * over a stretch — every station within a run of the crossing is held under the
 * barrel's underside, tapered back to open sky beyond it, so the road ducks
 * through an approach and a dip rather than a step.
 *
 * An earlier road is not over the road at all: it is in the way of it, at one
 * point, and what the two want there is to be the same height — so only the two
 * stations the crossing falls between are pinned. Both of them, and it is the
 * span rather than the nearest station because the stations are sixty metres
 * apart and a carriageway is fifteen wide: a rule that bound only stations
 * within half a carriageway of the other line would bind at neither of them,
 * which is exactly what the first cut of this did — the roads crossed, and one
 * of them still sat three metres over the other.
 *
 * Recomputed rather than remembered, because it is asked twice — once against
 * the ground as found, and again after the earthworks, when the road it is
 * pinned to may itself have been re-solved. */
static void road_bounds(const road_ctx *cx, int idx, const fly_road *r) {
    int n = r->point_count, i, s2, j;
    for (i = 0; i < n; ++i) {
        road_work[idx].ceiling[i] = FLT_MAX;
        road_work[idx].pin[i] = -FLT_MAX;
        road_work[idx].bound[i] = 0;
    }
    /* --- under the guideway ---
     *
     * At a crossing, and only there. The first cut of this held the deck under
     * the barrel wherever the road came within a hundred and fifty metres of
     * the line, which is a different claim entirely: a road leaving a
     * settlement runs beside the guideway's own approach for a few hundred
     * metres, the deck there is at platform height, and a road told to get
     * under a platform it is running *alongside* is a road buried in the yard
     * it starts in. What has to clear the barrel is the carriageway that goes
     * under the barrel, so the rule is hung on the crossing rather than on
     * proximity — and the run either side of it is the approach, which is what
     * lets the vertical curve draw a dip instead of a step. */
    if (cx->rail && cx->rail->point_count > 1)
        for (s2 = 0; s2 + 1 < n; ++s2) {
            fly_v3 a = r->points[s2].pos, b = r->points[s2 + 1].pos;
            float rx = b.x - a.x, ry = b.y - a.y;
            for (j = 0; j + 1 < cx->rail->point_count; ++j) {
                fly_v3 c = cx->rail->points[j].pos, d = cx->rail->points[j + 1].pos;
                float sx = d.x - c.x, sy = d.y - c.y;
                float den = rx * sy - ry * sx, t, u, hx, hy;
                int q;
                if (fabsf(den) < 1e-9f) continue;
                t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
                u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
                if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) continue;
                hx = a.x + rx * t;
                hy = a.y + ry * t;
                for (q = 1; q + 1 < n; ++q) {
                    float deck, run = hypotf(r->points[q].pos.x - hx, r->points[q].pos.y - hy);
                    float lid;
                    if (run >= FLY_ROAD_UNDER_RUN) continue;
                    /* the barrel's own height where *this* station passes under
                       it, not where the centrelines happened to cross */
                    rail_near(cx->rail, r->points[q].pos.x, r->points[q].pos.y, &deck);
                    lid = deck - FLY_RAIL_PIPE_R - FLY_ROAD_UNDER_CLEAR;
                    if (run > FLY_ROAD_UNDER_R)
                        lid += (run - FLY_ROAD_UNDER_R) /
                               (FLY_ROAD_UNDER_RUN - FLY_ROAD_UNDER_R) * 8.0f;
                    else road_work[idx].bound[q] = 1;
                    if (lid < road_work[idx].ceiling[q]) road_work[idx].ceiling[q] = lid;
                }
            }
        }
    /* --- level with an earlier road, where the two actually cross --- */
    if (cx->net)
        for (s2 = 0; s2 + 1 < n; ++s2) {
            fly_v3 a = r->points[s2].pos, b = r->points[s2 + 1].pos;
            float rx = b.x - a.x, ry = b.y - a.y;
            int o;
            for (o = 0; o < idx && o < cx->net->count; ++o) {
                const fly_road *rd = &cx->net->road[o];
                float lo = a.x < b.x ? a.x : b.x, hi = a.x > b.x ? a.x : b.x;
                if (hi < rd->lo.x || lo > rd->hi.x) continue;
                lo = a.y < b.y ? a.y : b.y;
                hi = a.y > b.y ? a.y : b.y;
                if (hi < rd->lo.y || lo > rd->hi.y) continue;
                for (j = 0; j + 1 < rd->point_count; ++j) {
                    fly_v3 c = rd->points[j].pos, d = rd->points[j + 1].pos;
                    float sx = d.x - c.x, sy = d.y - c.y;
                    float den = rx * sy - ry * sx, t, u, z;
                    int k;
                    if (fabsf(den) < 1e-9f) continue;
                    t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
                    u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
                    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) continue;
                    z = fly_lerpf(c.z, d.z, u);
                    /* The two stations the crossing falls between are held to
                       it, and nothing else is: the approach either side is the
                       vertical curve's to draw, and a hand-drawn taper laid on
                       top of it would be a second opinion about the shape of a
                       road. Where a station is already pinned by another
                       crossing the higher of the two wins — two roads meeting
                       the same one cannot each pull it down. */
                    for (k = 0; k <= 1; ++k) {
                        int q = s2 + k;
                        if (q < 1 || q + 1 >= n) continue;
                        if (z > road_work[idx].pin[q]) road_work[idx].pin[q] = z;
                        road_work[idx].bound[q] = 1;
                    }
                }
            }
        }
    /* The ends are the settlement's own yard, pinned to it by whoever laid the
       site out; a bound on either of them is a bound on a height nobody is
       solving for. */
    if (n > 0) {
        road_work[idx].bound[0] = road_work[idx].bound[n - 1] = 0;
        road_work[idx].pin[0] = road_work[idx].pin[n - 1] = -FLT_MAX;
        road_work[idx].ceiling[0] = road_work[idx].ceiling[n - 1] = FLT_MAX;
    }
}

/* The band the profile is solved inside, at every station: the floor it may
 * not sink below and the ceiling it may not rise above.
 *
 * Three cases, and they are three because a road is bounded by three different
 * things. Open country is the shelf underneath and the sky above. Under the
 * guideway the ceiling comes down to the headroom a lorry needs, and the shelf
 * still holds the road up — so where the two cross the ceiling wins and the
 * difference between them is a cutting the network has to find room for. And at
 * a crossing with an earlier road the two bounds close on the same number,
 * which is that road's own height: a junction is not a clearance, it is an
 * agreement about where the surface is.
 *
 * `may_cut` is the free-handed first solve, which is allowed to draw the
 * profile a cutting would give it so that the drape can measure how deep a
 * cutting each station is asking for. A pin is honoured in both solves and
 * clamped into what a road can actually be built to either way — a junction is
 * a junction, not a licence to build a viaduct up to somebody else's
 * embankment or to sink a shaft under it. */
static void road_band(int idx, const float *shelf, int may_cut,
                      float *lo, float *hi, int n) {
    int i;
    for (i = 0; i < n; ++i) {
        if (road_work[idx].pin[i] > -FLT_MAX) {
            float p = fly_clampf(road_work[idx].pin[i], shelf[i] - FLY_ROAD_CUT,
                                 shelf[i] + FLY_ROAD_FILL);
            /* And still under whatever is over it. Where a crossroads and a
             * portal want the same sixty metres of road at different heights
             * the structure wins, because the two failures are not the same
             * size: a crossroads a metre out is a bump, and a carriageway
             * inside a guideway is not a road. It happens — a road crossing
             * the line is turning to cross it squarely, and turning is what
             * takes it across another road — and left to the pin the
             * carriageway came out with 4.11 m of headroom where the lorry
             * needs 5.20. */
            if (p > road_work[idx].ceiling[i]) p = road_work[idx].ceiling[i];
            lo[i] = hi[i] = p;
            continue;
        }
        lo[i] = may_cut ? shelf[i] - FLY_ROAD_CUT : shelf[i];
        hi[i] = road_work[idx].ceiling[i];
    }
}

/* Drape a laid centreline onto the ground and fill in everything derived.
 * Returns 0 when the result is a road, negative when it is not one. */
static int road_drape(fly_road *r, int idx, const road_ctx *cx, const fly_v2 *path, int n) {
    const fly_world *w = cx->w;
    float ground[FLY_ROAD_POINT_MAX];
    float run = 0.0f;
    int i, k, pass;
    if (n < 4) return -1;
    /* Drop the stubs the trim leaves. Cutting the line at the clearance circle
     * puts the terminus wherever the crossing happened to fall, which is
     * sometimes a metre from the point after it — and a metre-long span between
     * two heights that differ by three is a piece of road standing on end.
     * Nothing else notices a span that short, so it is removed rather than
     * special-cased everywhere downstream. */
    for (i = 0, k = 0; i < n; ++i) {
        if (k > 0 && i + 1 < n &&
            hypotf(path[i].x - r->points[k - 1].pos.x,
                   path[i].y - r->points[k - 1].pos.y) < FLY_ROAD_SPACING * 0.25f) continue;
        r->points[k].pos = fly_v3mk(path[i].x, path[i].y, 0.0f);
        ++k;
    }
    n = k;
    /* The far terminus is kept whatever happens, so a stub there is closed from
       the other side: the point before it is the one that goes. */
    while (n >= 5 && fly_v3dist(r->points[n - 2].pos, r->points[n - 1].pos)
                     < FLY_ROAD_SPACING * 0.25f) {
        r->points[n - 2] = r->points[n - 1];
        --n;
    }
    if (n < 4) return -1;
    r->point_count = n;
    for (i = 0; i < n; ++i) {
        ground[i] = road_shelf(w, r->points, n, i);
        r->points[i].pos.z = ground[i];
    }
    /* And what is over it, before anything is solved: the profile is held under
       the guideway and level with an earlier road where the two cross, and both
       are properties of the ground the survey crosses rather than of the answer
       it arrives at. */
    road_bounds(cx, idx, r);
    /* --- the grade ---------------------------------------------------------
     *
     * A road climbs at a rate somebody chose; the ground does not. Left draped,
     * the profile takes every dip and hummock along the way and the grade
     * changes at every point on it, which is a track across a field rather than
     * a road between two towns. So the profile is the taut line over the shelf
     * — see fly_rail_grade, which the rail lays its deck on for the same
     * reason and by the same solve.
     *
     * What was here was that solve with one side missing. It raised a station
     * toward the midpoint of its neighbours and never lowered one, which is a
     * ratchet: over rolling ground every station climbs until it meets the fill
     * cap, and the road crosses the country on a twelve-metre bank the whole
     * way. That is where "the roads are too tall" came from — not from the
     * shelf and not from the deck, from a relaxation that could only go up.
     *
     * Two-sided, it converges on the line that spends the least earth for the
     * smoothness it buys, which is what a surveyor draws. The fill cap comes
     * down with it: a taut line over the shelf does not need a dozen metres to
     * hold a gradient, and past about six an embankment stops reading as
     * earthworks and starts reading as a dam. The ends stay pinned — they are
     * the settlement's own yard and their height is not ours to choose. */
    {
        static float zs[FLY_ROAD_POINT_MAX], ch[FLY_ROAD_POINT_MAX];
        static float dig[FLY_ROAD_POINT_MAX];
        static float fl[FLY_ROAD_POINT_MAX], rf[FLY_ROAD_POINT_MAX];
        for (i = 0; i < n; ++i) {
            ch[i] = i ? ch[i - 1] + hypotf(r->points[i].pos.x - r->points[i - 1].pos.x,
                                           r->points[i].pos.y - r->points[i - 1].pos.y)
                      : 0.0f;
        }
        /* --- what it would cut if it could ---
         *
         * Solved twice. The first solve lets the line drop below the shelf by a
         * cutting's depth, which is the profile a surveyor would draw with a
         * free hand; the difference between it and the shelf is how deep a
         * cutting each station wants. The second solve is the one that is kept,
         * and its floor is the shelf again everywhere the world could not
         * afford the earthworks — because the cut budget is bounded by what the
         * heightfield can carry to the GPU, and a road that assumed a cutting
         * it did not get would be a road buried in a hill.
         *
         * The order matters: the wants are collected here, the budget is spent
         * in fly_road_build across the whole network, and only the granted ones
         * come back. That is why `want_cut` is an output of the drape rather
         * than something it decides for itself. */
        /* The ends are pinned and fly_rail_grade does not write them, so they
         * are the caller's to set — and `zs` is static, so leaving them alone
         * hands the solver the previous road's termini. That is exactly what
         * happened: the profile was pulled toward a height belonging to a
         * settlement somewhere else on the map, and the network came out with
         * 127 m of fill on it. */
        for (i = 0; i < n; ++i) zs[i] = ground[i];
        road_band(idx, ground, 1, dig, rf, n);
        fly_rail_grade(zs, dig, rf, ch, n, FLY_ROAD_FILL + FLY_ROAD_CUT, 0.5f, 4000);
        for (i = 0; i < n; ++i) {
            road_work[idx].want_z[i] = zs[i];
            road_work[idx].want_cut[i] = ground[i] - zs[i];
            if (road_work[idx].want_cut[i] < 0.0f) road_work[idx].want_cut[i] = 0.0f;
            if (road_work[idx].want_cut[i] > FLY_ROAD_CUT)
                road_work[idx].want_cut[i] = FLY_ROAD_CUT;
        }
        for (i = 0; i < n; ++i) zs[i] = ground[i];
        road_band(idx, ground, 0, fl, rf, n);
        fly_rail_grade(zs, fl, rf, ch, n, FLY_ROAD_FILL, 0.5f, 4000);
        for (i = 0; i < n; ++i) {
            r->points[i].pos.z = zs[i];
            road_work[idx].shelf[i] = ground[i];
            road_work[idx].chain[i] = ch[i];
        }
    }
    (void)pass;
    r->lo = fly_v2mk(r->points[0].pos.x, r->points[0].pos.y);
    r->hi = r->lo;
    for (i = 0; i < n; ++i) {
        fly_v3 p = r->points[i].pos;
        r->points[i].distance = i ? r->points[i - 1].distance +
            fly_v3dist(r->points[i - 1].pos, p) : 0.0f;
        if (p.x < r->lo.x) r->lo.x = p.x;
        if (p.y < r->lo.y) r->lo.y = p.y;
        if (p.x > r->hi.x) r->hi.x = p.x;
        if (p.y > r->hi.y) r->hi.y = p.y;
        /* the run of causeway, measured on the line the road actually took
           rather than on the chord the veto above looked at */
        if (fly_world_wet(w, p.x, p.y)) {
            run += i ? fly_v3dist(r->points[i - 1].pos, p) : 0.0f;
            if (run > FLY_ROAD_FORD) return -2;
        } else {
            run = 0.0f;
        }
    }
    r->length = r->points[n - 1].distance;
    if (r->length > FLY_ROAD_MAX_LENGTH) return -3;
    /* A road is only as quick as its worst gradient, and a haul road over a
     * ridge is a slow one. The average climb per metre is the honest summary
     * and it is what a convoy's speed comes off. */
    {
        float climb = 0.0f;
        for (i = 1; i < n; ++i) climb += fabsf(r->points[i].pos.z - r->points[i - 1].pos.z);
        r->speed = 21.0f / (1.0f + 6.0f * (r->length > 1.0f ? climb / r->length : 0.0f));
    }
    return 0;
}

/* --- what this link is asked to keep off -----------------------------------
 *
 * The lines handed to the survey as already built. They are kept in the plane,
 * because that is the frame the survey works in and copying them once per
 * network is cheaper than the survey asking a route for its points a few
 * hundred thousand times.
 *
 * The guideway carries a weight per point, which is where a crossing gets to
 * choose its place: the toll is full wherever the line is low enough that
 * getting under it would take an earthwork, and down to a little over a third
 * where the piers are tall enough that a road passes beneath on the flat. A
 * road that has to cross therefore crosses where there is room, which is the
 * difference between an underpass and a hole dug under a viaduct. */
static fly_v2 keep_rail[FLY_RAIL_POINT_MAX];
static float keep_rail_w[FLY_RAIL_POINT_MAX];
static int keep_rail_n;
static fly_v2 keep_road[FLY_ROAD_MAX][FLY_ROAD_POINT_MAX];
static float keep_road_w[FLY_ROAD_POINT_MAX];
static fly_rail_line keep_line[FLY_RAIL_AVOID_MAX];

static void keep_reset(const fly_world *w, const fly_rail_route *rail) {
    int i;
    keep_rail_n = 0;
    for (i = 0; i < FLY_ROAD_POINT_MAX; ++i) keep_road_w[i] = FLY_ROAD_KEEP_ROAD;
    if (!rail || rail->point_count < 2) return;
    keep_rail_n = rail->point_count < FLY_RAIL_POINT_MAX ? rail->point_count
                                                         : FLY_RAIL_POINT_MAX;
    for (i = 0; i < keep_rail_n; ++i) {
        fly_v3 p = rail->points[i].pos;
        float room = p.z - FLY_RAIL_PIPE_R - fly_world_ground(w, p.x, p.y);
        keep_rail[i] = fly_v2mk(p.x, p.y);
        /* full price where a road could not get under it, and a little over a
           third where it comfortably could */
        keep_rail_w[i] = 1.0f - 0.6f * fly_smoothstepf(FLY_ROAD_UNDER_CLEAR,
                                                       FLY_ROAD_UNDER_CLEAR + 4.0f, room);
    }
}

/* Remember a road as laid, so the next link keeps off it too. */
static void keep_note(int idx, const fly_road *rd) {
    int i;
    if (idx < 0 || idx >= FLY_ROAD_MAX) return;
    for (i = 0; i < rd->point_count && i < FLY_ROAD_POINT_MAX; ++i)
        keep_road[idx][i] = fly_v2mk(rd->points[i].pos.x, rd->points[i].pos.y);
}

/* The lines worth handing to this particular survey: the guideway, and the
 * roads whose bounds reach the patch of world the search is about to cover.
 * Nearly every road in the network is on the other side of the map, and a
 * corridor rasterised for one of those is a corridor rasterised outside the
 * grid it is being stamped into. */
static int keep_gather(const road_ctx *cx, fly_wpos from, fly_wpos to) {
    float mx = (from.e + to.e) * 0.5f, my = (from.n + to.n) * 0.5f;
    float half = hypotf(to.e - from.e, to.n - from.n) * 0.85f + 600.0f + FLY_ROAD_KEEP;
    int n = 0, r;
    if (keep_rail_n >= 2) {
        keep_line[n].pos = keep_rail;
        keep_line[n].weight = keep_rail_w;
        keep_line[n].count = keep_rail_n;
        ++n;
    }
    if (!cx->net) return n;
    for (r = 0; r < cx->net->count && n < FLY_RAIL_AVOID_MAX; ++r) {
        const fly_road *rd = &cx->net->road[r];
        if (rd->point_count < 2) continue;
        if (rd->hi.x < mx - half || rd->lo.x > mx + half) continue;
        if (rd->hi.y < my - half || rd->lo.y > my + half) continue;
        keep_line[n].pos = keep_road[r];
        keep_line[n].weight = keep_road_w;
        keep_line[n].count = rd->point_count;
        ++n;
    }
    return n;
}

static int road_lay(fly_road *r, int idx, const road_ctx *cx, int from, int to) {
    static fly_v2 path[FLY_ROAD_POINT_MAX];
    const fly_world *w = cx->w;
    fly_rail_shape shape;
    int n;
    if (from < 0 || to < 0 || from >= w->nloc || to >= w->nloc || from == to) return -1;
    if (fly_world_dist(w->loc[from].pos, w->loc[to].pos) > FLY_ROAD_MAX_LENGTH) return -1;
    if (chord_water(w, w->loc[from].pos, w->loc[to].pos) > 1500.0f) return -1;
    memset(r, 0, sizeof *r);
    memset(&shape, 0, sizeof shape);
    /* Wide of the deck, so the road stops at the edge of the apron and the last
     * few hundred metres of it are the yard rather than the pad. */
    shape.clear = FLY_PAD_DECK_R + 30.0f;
    shape.spacing = FLY_ROAD_SPACING;
    shape.min_radius = FLY_ROAD_MIN_RADIUS;
    /* Two thirds of the rail's resolution. Two dozen of these are laid at
     * world-gen and the survey is nearly all terrain sampling; see
     * route_search. Coarser than this and the search stops being able to see a
     * ridge it should be going round the end of. */
    shape.grid = 64;
    /* Twenty-five metres of detour per metre of climb. Raising it does not buy
     * a flatter road: at sixty the survey trades one steep touch-down for
     * another and the worst gradient in the network came out marginally higher,
     * because a toll that large stops the route from picking its crossing point
     * and starts making it wander until it meets the ridge somewhere arbitrary.
     * The remaining steep stations are earthworks' problem, not the survey's. */
    shape.climb_toll = 25.0f;
    /* And it wanders. The search answers which side of the bay to pass and
       nothing else; what makes the answer a road rather than a ruled line is
       the swing about it — see fly_rail_shape's `meander`. */
    shape.meander = FLY_ROAD_MEANDER;
    /* And it keeps off what is already here. The guideway above all — see
       FLY_ROAD_KEEP — and the roads laid before this one, at half the price,
       because a road can be crossed at grade and a line on piers cannot. */
    shape.avoid.line = keep_line;
    shape.avoid.count = keep_gather(cx, w->loc[from].pos, w->loc[to].pos);
    shape.avoid.reach = FLY_ROAD_KEEP;
    shape.avoid.toll = FLY_ROAD_KEEP_TOLL;
    n = fly_rail_lay(w, w->loc[from].pos, w->loc[to].pos, &shape, path, FLY_ROAD_POINT_MAX);
    if (n < 4) return -1;
    r->from = from;
    r->to = to;
    if (road_drape(r, idx, cx, path, n) != 0) return -1;
    /* the drape drops stub spans, so the terminus is wherever it left the count
       — never `n`, which is what came out of the survey */
    n = r->point_count;
    r->bearing_from = atan2f(r->points[0].pos.y - w->loc[from].pos.n,
                             r->points[0].pos.x - w->loc[from].pos.e);
    r->bearing_to = atan2f(r->points[n - 1].pos.y - w->loc[to].pos.n,
                           r->points[n - 1].pos.x - w->loc[to].pos.e);
    return 0;
}

/* ---------------- the network ----------------
 *
 * Kruskal over the settlement pairs, with two rules that make the result a road
 * network rather than a minimum spanning tree.
 *
 * A degree cap, because a tree built by cheapest-first hangs six roads off
 * whichever site happens to sit in the middle, and six roads meeting in a
 * mining camp is an interchange nobody would build. Three is a junction.
 *
 * And a second pass that adds short links between sites already connected, so
 * the network has loops in it. A tree is the cheapest way to join everything
 * and it is exactly wrong for what the roads are *for*: freight that can only
 * ever reach a place one way makes every road the same road, and a convoy
 * ambushed on a tree has no other way home.
 *
 * The second pass walks the same edge list the first did, so it has to remember
 * what the first laid. Without that memory a short link the tree already built
 * is simply built again — an identical survey, an identical drape, a second
 * carriageway occupying the same ground as the first, and a slot of both ends'
 * degree cap spent on it. Three of the twenty-eight roads on seed 4242 were
 * that. One flag on the edge is the whole of the fix, and it is a fix at source
 * rather than in the drawing: fewer roads is a different set of stations
 * bidding for the cut budget, which is a different heightfield and therefore a
 * different world.
 *
 * Roads that cannot be laid — across a strait, over a mountain further than
 * anyone would drive — are simply not there, so the network is a forest rather
 * than necessarily one piece. An island settlement with a pad and no road is a
 * true thing about that world, and the game already has aircraft for it. */

typedef struct { int a, b; float len; int laid; } road_edge;

/* Mark a cell and its eight neighbours. See the mask's note in fly_road.h. */
static void mask_mark(fly_road_net *net, float x, float y) {
    int cx = (int)floorf((x + FLY_WORLD_HALF) / FLY_ROAD_MASK_CELL);
    int cy = (int)floorf((y + FLY_WORLD_HALF) / FLY_ROAD_MASK_CELL);
    int dx, dy;
    for (dy = -1; dy <= 1; ++dy)
        for (dx = -1; dx <= 1; ++dx) {
            int i = cx + dx, j = cy + dy, bit;
            if (i < 0 || j < 0 || i >= FLY_ROAD_MASK || j >= FLY_ROAD_MASK) continue;
            bit = j * FLY_ROAD_MASK + i;
            net->mask[bit >> 3] |= (unsigned char)(1u << (bit & 7));
        }
}

static int uf_find(int *parent, int i) {
    while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
    return i;
}

/* A settlement the network reaches. Skyports are the exception and the only
 * one: a high-altitude trade hub is a deck in the sky, and a road to it would
 * have to climb something that is not there. */
static int road_serves(const fly_world *w, int loc) {
    return w->loc[loc].kind != FLY_LOC_SKYPORT;
}

/* One candidate earthwork: a station on a road, how far its box reaches either
 * side of that station, and what building it would be worth. `depth` is that
 * score — see road_cut_budget, which also borrows it as the "already taken"
 * marker by setting it negative. */
typedef struct { int road, i, arm; float depth; } road_dig;

/* The longest a cut box gets — how far it reaches either side of the station
 * that asked for it — the shortest that is still worth digging, and how far
 * apart two of them must stand. All three in stations.
 *
 * Written as metres and divided, because that is what they are. They used to be
 * the literals 2 and 6, which meant a 600 m box and a 900 m spacing at the
 * spacing the survey had then; left as literals through a resample they would
 * silently have become a 240 m box and a 360 m spacing, and the note above
 * about box length is a note about metres of ridge, not about how many times
 * the surveyor happened to sample it. */
#define FLY_ROAD_CUT_ARM ((int)(300.0f / FLY_ROAD_SPACING + 0.5f))
#define FLY_ROAD_CUT_MIN_ARM ((int)(90.0f / FLY_ROAD_SPACING + 0.5f))
#define FLY_ROAD_CUT_GAP ((int)(900.0f / FLY_ROAD_SPACING + 0.5f))
/* And the shortest a station *held under something* may take, which is a
 * different question with a different answer.
 *
 * Ninety metres is the least a cutting is worth digging: below that the
 * earthwork costs a box of a bounded supply and takes out less gradient than
 * the box is worth. A station under a guideway is not spending the budget on
 * gradient at all — it is buying the headroom a lorry needs — and what it has
 * to cover is the width of the structure over it, FLY_ROAD_UNDER_R, and
 * nothing more.
 *
 * The difference is the difference between a portal a lorry fits under and one
 * it does not. A road crossing a guideway is *turning* while it does it,
 * because crossing squarely is what the survey was priced to do, and over
 * ninety metres of that turn the line stands far enough off its own chord that
 * the bow test refuses every box: on seed 4242 the bow at the shortest ordinary
 * arm was 14.8 m against a limit of 9, and the crossing came out with 4.11 m of
 * headroom where 5.20 is what the ceiling was set to buy. Over the crossing's
 * own width the same line bows 3.5 m. */
#define FLY_ROAD_CUT_UNDER_ARM ((int)(FLY_ROAD_UNDER_R / FLY_ROAD_SPACING + 0.5f))

/* How far the road may bow off the straight line between the two ends of a box
 * before the box stops describing the road.
 *
 * A `fly_cut` is a rectangle: a centre, an axis, a half-length and a
 * half-width, and everything inside it is graded to a formation that ramps
 * linearly from one end to the other. That is a fair description of six hundred
 * metres of road only while the road is straight through them. It never quite
 * was, and it stopped being nearly true when the network learned to wander: at
 * the tightest arc a road may hold, six hundred metres of it bows a hundred
 * metres off its own chord, which is seven times the width of the box. What
 * gets dug then is a trench through the countryside beside the road, and the
 * road — its profile solved a second time against ground that did move, just
 * not under it — ends up on a bank in the middle of it.
 *
 * So the box is shortened until it does describe the road, rather than the
 * candidate being refused. Refusing was the first version and it costs real
 * grading: on a wandering network most of the deep cuttings *are* on curves,
 * because a road that is turning is usually turning round something, and the
 * approach into one settlement went from a landing to a write-off when the
 * earthwork under it stopped being built. A short box on a curve grades less
 * than a long one would; it grades the right ground. */
#define FLY_ROAD_CUT_BOW 9.0f

/* The most any station between `i0` and `i1` stands off the chord between
 * them, which is the error a box laid on that chord would make. */
static float road_bow(const fly_road *rd, int i0, int i1) {
    fly_v3 p0 = rd->points[i0].pos, p1 = rd->points[i1].pos;
    float ax = p1.x - p0.x, ay = p1.y - p0.y;
    float alen = hypotf(ax, ay), bow = 0.0f;
    int j;
    if (alen < 1.0f) return 1e30f;
    for (j = i0; j <= i1; ++j) {
        float off = fabsf((rd->points[j].pos.x - p0.x) * ay -
                          (rd->points[j].pos.y - p0.y) * ax) / alen;
        if (off > bow) bow = off;
    }
    return bow;
}

/* The longest box centred on `i` that still describes the road, or 0 if even
 * the shortest one does not. `least` is how short "shortest" is allowed to be
 * — see FLY_ROAD_CUT_UNDER_ARM for the station that gets a different answer. */
static int road_cut_arm(const fly_road *rd, int i, int least) {
    int arm;
    for (arm = FLY_ROAD_CUT_ARM; arm >= least; --arm) {
        if (i - arm < 1 || i + arm >= rd->point_count - 1) continue;
        if (road_bow(rd, i - arm, i + arm) <= FLY_ROAD_CUT_BOW) return arm;
    }
    return 0;
}

static void road_cut_publish(fly_world *w, const fly_road *rd, int idx, int i0, int i1) {
    const fly_road_point *a = &rd->points[i0], *b = &rd->points[i1];
    fly_cut *c;
    float dx = b->pos.x - a->pos.x, dy = b->pos.y - a->pos.y;
    float len = hypotf(dx, dy), deep = 0.0f;
    int i;
    if (w->ncut >= FLY_CUT_MAX || len < 1.0f) return;
    c = &w->cut[w->ncut++];
    c->x = (a->pos.x + b->pos.x) * 0.5f;
    c->y = (a->pos.y + b->pos.y) * 0.5f;
    c->dx = dx / len;
    c->dy = dy / len;
    /* A little longer than the stations it spans, so the cut carries past the
     * point the road stops needing it. A cutting that ends exactly where the
     * road climbs out of it leaves a step at each end and the road drives into
     * a wall. */
    c->half = len * 0.5f + FLY_ROAD_SPACING * 0.35f;
    /* The whole formation, which is what road_shelf measures over: see
     * FLY_ROAD_SHELF_R. A metre past it so the shelf's outermost sample is
     * inside the flat rather than on the lip of the batter. */
    c->wide = FLY_ROAD_SHELF_R + 1.0f;
    for (i = i0; i <= i1; ++i)
        if (road_work[idx].want_cut[i] > deep) deep = road_work[idx].want_cut[i];
    /* The side slope. Tipped material finds an angle and so does a cut face,
     * and a cutting whose sides are vertical is a trench somebody lined. */
    c->feather = deep * 2.6f + 7.0f;
    /* Formation, which is the deck less its own thickness: the slab still
     * stands its kerb proud of the ground it was cut into. Taken from the
     * profile the road *would* have had with a free hand, which is the whole
     * point of having surveyed that one first. */
    c->z0 = road_work[idx].want_z[i0] - FLY_ROAD_DECK;
    c->z1 = road_work[idx].want_z[i1] - FLY_ROAD_DECK;
}

/* --- spending the cut budget ---------------------------------------------
 *
 * Every road has said, station by station, how deep a cutting it would take if
 * the world could afford one. There are far more of those than the heightfield
 * can carry — see fly_cut — so a bounded number get built and the rest do not.
 *
 * They are ranked by the gradient the earthwork removes, not by how deep it is.
 * Those are different questions and ranking by depth answers the wrong one: the
 * deepest cuts are in the flanks of long climbs where the road was already
 * holding a comfortable grade, and spending the whole budget there left the
 * steepest station in the network at exactly the gradient it had before any
 * earthworks existed. What a cutting is *for* is the crest — the place where
 * the profile a surveyor would draw and the shelf the road must otherwise stand
 * on diverge fastest — so the key is how much steeper the shelf is than the
 * free profile across the stations the box would cover.
 *
 * One box per chosen station, spanning its two neighbours, and the boxes are
 * kept apart so two do not spend the budget on the same earthwork. Short on
 * purpose: a cut's formation ramps *linearly* from one end of its box to the
 * other, and the profile it is meant to follow is a curve, so a box long enough
 * to cover a whole ridge crossing tracks the profile only at its ends and sits
 * above it in the middle — which is a cutting that does not cut where the road
 * most needed it. Five stations is short enough for the chord to be the curve;
 * the first cut of this was a run of twenty and it graded nothing useful.
 *
 * Then the roads standing on ground that moved are re-draped against it. A road
 * that assumed a cutting it did not get would be a road buried in a hill, and
 * the only way to be sure is to solve again with the answer in hand. */
static void road_cut_budget(fly_road_net *net, fly_world *w, const road_ctx *cx) {
    static road_dig dig[FLY_ROAD_MAX * FLY_ROAD_POINT_MAX / 4];
    int nd = 0, r, i, k, taken = 0;
    unsigned char redo[FLY_ROAD_MAX];
    memset(redo, 0, sizeof redo);
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        for (i = 1; i + 1 < rd->point_count; ++i) {
            float gain = 0.0f;
            int j, arm;
            if (road_work[r].want_cut[i] < FLY_ROAD_CUT_MIN) continue;
            if (nd >= (int)(sizeof dig / sizeof dig[0])) break;
            /* the longest box that still describes the road through here */
            arm = road_cut_arm(rd, i, road_work[r].bound[i] ? FLY_ROAD_CUT_UNDER_ARM
                                                            : FLY_ROAD_CUT_MIN_ARM);
            if (arm <= 0) continue;
            /* The worst gradient the box would take out, over the span it
               covers: central differences on both profiles, the shelf's own
               steepness less the steepness the free profile settled for. */
            for (j = i - arm; j <= i + arm; ++j) {
                float run, raw, want;
                /* The station has to have a neighbour on each side before any
                   of them may be read: j runs from i-2 and i is only known to
                   be 2 or more, so the first pass of this loop is a central
                   difference about station 0 and `chain[j - 1]` is off the
                   front of the array. It was read and then discarded, which is
                   an out-of-bounds load whatever it is used for. */
                if (j < 1 || j + 1 >= rd->point_count) continue;
                run = road_work[r].chain[j + 1] - road_work[r].chain[j - 1];
                if (run < 1.0f) continue;
                raw = fabsf(road_work[r].shelf[j + 1] - road_work[r].shelf[j - 1]) / run;
                want = fabsf(road_work[r].want_z[j + 1] - road_work[r].want_z[j - 1]) / run;
                if (raw - want > gain) gain = raw - want;
            }
            /* A station held down by a ceiling is not bidding against the
             * others: it is a road that has to get under a structure, and a
             * cutting it does not get is a carriageway inside a guideway. The
             * gradient it takes out is worth a fraction of a percent and every
             * other candidate outbids it, so it is scored above all of them and
             * the handful of them there are come off the top of the budget. */
            if (road_work[r].bound[i]) gain += 10.0f;
            if (gain <= 0.0f) continue;
            dig[nd].road = r;
            dig[nd].i = i;
            dig[nd].arm = arm;
            dig[nd].depth = gain;
            ++nd;
        }
    }
    /* Best first — most gradient removed — with ties broken by where they are
       so the answer does not depend on the order the roads were laid in. */
    for (i = 0; i < nd; ++i) {
        int best = i;
        for (k = i + 1; k < nd; ++k)
            if (dig[k].depth > dig[best].depth ||
                (dig[k].depth == dig[best].depth &&
                 (dig[k].road < dig[best].road ||
                  (dig[k].road == dig[best].road && dig[k].i < dig[best].i)))) best = k;
        if (best != i) { road_dig t = dig[i]; dig[i] = dig[best]; dig[best] = t; }
    }
    for (i = 0; i < nd && taken < FLY_CUT_MAX; ++i) {
        int clash = 0;
        for (k = 0; k < i; ++k) {
            int d = dig[i].i - dig[k].i;
            if (dig[k].depth < 0.0f && dig[k].road == dig[i].road &&
                d > -FLY_ROAD_CUT_GAP && d < FLY_ROAD_CUT_GAP) { clash = 1; break; }
        }
        if (clash) continue;
        road_cut_publish(w, &net->road[dig[i].road], dig[i].road,
                         dig[i].i - dig[i].arm, dig[i].i + dig[i].arm);
        dig[i].depth = -1.0f;   /* mark as taken, for the spacing test above */
        ++taken;
    }
    /* Who has to be solved again: every road standing on ground a cutting
     * moved, not only the road that asked for one.
     *
     * Roads cross, and near a settlement several of them run together down the
     * same valley. An earthwork dug for one of them takes the hillside out from
     * under whatever else is on it, and a road that is not told about it keeps
     * the profile it had — which is how road 21 came to stand on an 11.7 m bank
     * in the middle of a cutting somebody else had dug through it. The
     * footprint is the box plus its feather, exactly what fly_world_ground
     * tests, so this asks the same question the heightfield answers. */
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        /* A road held under a structure is re-solved whatever the earthworks
           did, because the cutting it asked for is the one thing that decides
           whether it can be where the ceiling puts it. */
        for (i = 0; i < rd->point_count; ++i)
            if (road_work[r].bound[i]) { redo[r] = 1; break; }
        for (i = 0; i < rd->point_count && !redo[r]; ++i)
            for (k = 0; k < w->ncut; ++k) {
                const fly_cut *c = &w->cut[k];
                float rx = rd->points[i].pos.x - c->x, ry = rd->points[i].pos.y - c->y;
                float u = rx * c->dx + ry * c->dy;
                float v = rx * -c->dy + ry * c->dx;
                if (fabsf(u) > c->half + c->feather || fabsf(v) > c->wide + c->feather) continue;
                redo[r] = 1;
                break;
            }
    }
    /* Re-solve them against the ground as it now is. */
    for (r = 0; r < net->count; ++r) {
        fly_road *rd = &net->road[r];
        static float zs[FLY_ROAD_POINT_MAX], sh[FLY_ROAD_POINT_MAX];
        static float fl[FLY_ROAD_POINT_MAX], rf[FLY_ROAD_POINT_MAX];
        if (!redo[r]) continue;
        /* the shelf again, and over the graded ground this time: inside a
           cutting it is the formation, outside it is what it always was */
        for (i = 0; i < rd->point_count; ++i) {
            sh[i] = road_shelf(w, rd->points, rd->point_count, i);
            zs[i] = sh[i];
        }
        /* And the bounds again, because one of them may have moved: a road is
           pinned to the height of a road laid before it, this pass is in that
           same order, and by the time it reaches this one every road it is
           pinned to is finished. Reading the heights recorded at lay time
           instead would hold a junction level with where the other carriageway
           used to be. */
        road_bounds(cx, r, rd);
        road_band(r, sh, 0, fl, rf, rd->point_count);
        zs[0] = rd->points[0].pos.z;
        zs[rd->point_count - 1] = rd->points[rd->point_count - 1].pos.z;
        fly_rail_grade(zs, fl, rf, road_work[r].chain,
                       rd->point_count, FLY_ROAD_FILL, 0.5f, 4000);
        for (i = 0; i < rd->point_count; ++i) {
            rd->points[i].pos.z = zs[i];
            rd->points[i].distance = i ? rd->points[i - 1].distance +
                fly_v3dist(rd->points[i - 1].pos, rd->points[i].pos) : 0.0f;
        }
        rd->length = rd->points[rd->point_count - 1].distance;
    }
}

/* --- where a road ends up under the line -----------------------------------
 *
 * The corridor toll prices a crossing rather than forbidding it, so some worlds
 * have one: a settlement on the far side of the guideway is reached by going
 * under it, and the alternative — the road that would rather drive fifteen
 * kilometres round the end of the line — is not the road anybody would build.
 *
 * What is not acceptable is a crossing nobody wrote down. The structure that
 * carries the line over the carriageway is drawn here, the piers that would
 * otherwise stand in the road are moved out of it, and the tests ask how square
 * the crossing is and how much room is left under it — and all three want the
 * same handful of points, found once, off the same curve the road is drawn
 * along rather than off the chords the survey wrote down.
 *
 * Found after the earthworks, not before: the profile at a crossing is the one
 * the cutting gave it, and the headroom recorded here is the headroom a lorry
 * actually has. */
static void road_cross_find(fly_road_net *net, const fly_rail_route *rail) {
    int r, i, j;
    net->cross_count = 0;
    if (!rail || rail->point_count < 2) return;
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        for (i = 0; i + 1 < rd->point_count; ++i) {
            fly_v3 a = rd->points[i].pos, b = rd->points[i + 1].pos;
            float rx = b.x - a.x, ry = b.y - a.y;
            for (j = 0; j + 1 < rail->point_count; ++j) {
                fly_v3 c = rail->points[j].pos, d = rail->points[j + 1].pos;
                float sx = d.x - c.x, sy = d.y - c.y;
                float den = rx * sy - ry * sx, t, u;
                fly_road_cross *x;
                fly_v3 pos, tan;
                int k, seen = 0;
                if (fabsf(den) < 1e-9f) continue;   /* parallel here */
                t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
                u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
                if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) continue;
                if (!fly_road_span(rd, i, t, &pos, &tan)) continue;
                /* One crossing, however many spans it is found on: a hit that
                   lands on a station belongs to both of them. */
                for (k = 0; k < net->cross_count; ++k)
                    if (net->cross[k].road == r &&
                        fly_v3dist(net->cross[k].pos, pos) < 60.0f) { seen = 1; break; }
                if (seen) continue;
                if (net->cross_count >= FLY_ROAD_CROSS_MAX) return;
                x = &net->cross[net->cross_count++];
                x->road = r;
                x->station = i;
                x->t = t;
                x->pos = pos;
                x->yaw = atan2f(sy, sx);
                x->deck = fly_lerpf(c.z, d.z, u);
                x->headroom = x->deck - FLY_RAIL_PIPE_R - pos.z;
                {   float lr = hypotf(tan.x, tan.y), ls = hypotf(sx, sy);
                    float dotp = lr > 1e-6f && ls > 1e-6f
                               ? fabsf((tan.x * sx + tan.y * sy) / (lr * ls)) : 1.0f;
                    x->angle = acosf(fly_clampf(dotp, 0.0f, 1.0f)); }
            }
        }
    }
}

void fly_road_build(fly_road_net *net, fly_world *w, const fly_rail_route *rail) {
    static road_edge edge[FLY_MAX_LOC * (FLY_MAX_LOC - 1) / 2];
    int parent[FLY_MAX_LOC], degree[FLY_MAX_LOC];
    int ne = 0, i, j, pass;
    road_ctx cx;
    if (!net || !w) return;
    memset(net, 0, sizeof *net);
    cx.w = w;
    cx.rail = rail;
    cx.net = net;
    keep_reset(w, rail);
    for (i = 0; i < FLY_MAX_LOC; ++i) { parent[i] = i; degree[i] = 0; }
    for (i = 0; i < w->nloc; ++i)
        for (j = i + 1; j < w->nloc; ++j) {
            float len;
            if (!road_serves(w, i) || !road_serves(w, j)) continue;
            len = fly_world_dist(w->loc[i].pos, w->loc[j].pos);
            if (len > FLY_ROAD_MAX_LENGTH) continue;
            if (ne >= (int)(sizeof edge / sizeof edge[0])) break;
            edge[ne].a = i;
            edge[ne].b = j;
            edge[ne].len = len;
            edge[ne].laid = 0;
            ++ne;
        }
    /* Shortest first, ties broken by the sites' own order so the network does
       not depend on how the pairs happened to be enumerated. */
    for (i = 0; i < ne; ++i) {
        int best = i;
        for (j = i + 1; j < ne; ++j)
            if (edge[j].len < edge[best].len ||
                (edge[j].len == edge[best].len &&
                 (edge[j].a < edge[best].a ||
                  (edge[j].a == edge[best].a && edge[j].b < edge[best].b)))) best = j;
        if (best != i) { road_edge t = edge[i]; edge[i] = edge[best]; edge[best] = t; }
    }
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < ne && net->count < FLY_ROAD_MAX; ++i) {
            int a = edge[i].a, b = edge[i].b, joined;
            if (edge[i].laid) continue;   /* the tree already built this one */
            if (degree[a] >= FLY_ROAD_DEGREE_MAX || degree[b] >= FLY_ROAD_DEGREE_MAX) continue;
            joined = uf_find(parent, a) == uf_find(parent, b);
            /* First the links that join two parts of the map that are not yet
               joined; then, and only then, the short ones that close a loop. */
            if (pass == 0 && joined) continue;
            if (pass == 1 && (!joined || edge[i].len > 14000.0f)) continue;
            if (road_lay(&net->road[net->count], net->count, &cx, a, b) != 0) continue;
            {   const fly_road *rd = &net->road[net->count];
                int k;
                for (k = 0; k < rd->point_count; ++k)
                    mask_mark(net, rd->points[k].pos.x, rd->points[k].pos.y);
                keep_note(net->count, rd); }
            ++net->count;
            edge[i].laid = 1;
            ++degree[a];
            ++degree[b];
            parent[uf_find(parent, a)] = uf_find(parent, b);
        }
    road_cut_budget(net, w, &cx);
    road_cross_find(net, rail);
}

/* ---------------- queries ---------------- */

/* Where the road is, which is one question with two ways of asking it. Both
 * are fly_line's — see fly_line.h, where the arithmetic and the choice of
 * curve are argued out — and the road's part of the answer is only that its
 * stations are evenly spaced, because the drape resampled them at
 * FLY_ROAD_SPACING, so it is the uniform form that fits them. */
int fly_road_span(const fly_road *r, int i, float t, fly_v3 *pos, fly_v3 *tangent) {
    if (!r) return 0;
    return fly_line_span(r->points, r->point_count, FLY_LINE_EVEN, i, t, pos, tangent);
}

int fly_road_eval(const fly_road *r, float d, fly_v3 *pos, fly_v3 *tangent) {
    if (!r || r->point_count < 2) return -1;
    return fly_line_eval(r->points, r->point_count, r->length, FLY_LINE_EVEN,
                         d, pos, tangent) ? 0 : -1;
}

fly_v3 fly_road_lane(fly_v3 c, fly_v3 heading) {
    /* A quarter turn clockwise off the direction of travel is the near side,
       which is what "drive on the right" means when it is written as vectors
       rather than as a word. */
    float len = hypotf(heading.x, heading.y);
    float u = FLY_ROAD_LANE / FLY_ROAD_HALF;
    if (len < 1e-4f) return c;
    c.x += heading.y / len * FLY_ROAD_LANE;
    c.y += -heading.x / len * FLY_ROAD_LANE;
    c.z -= FLY_ROAD_CAMBER * u * u;
    return c;
}

/* The nearest carriageway to a point. The per-road answer is fly_line's; what
 * is here is the network's half of it — reject a road the query is nowhere
 * near before walking any of its spans, and keep the best across the rest. */
int fly_road_project(const fly_road_net *net, fly_v3 q, float *distance, float *separation) {
    float best = FLT_MAX, best_d = 0.0f;
    int r, found = -1;
    if (!net) return -1;
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        float d = 0.0f, sep = 0.0f;
        if (q.x < rd->lo.x - best || q.x > rd->hi.x + best) continue;
        if (q.y < rd->lo.y - best || q.y > rd->hi.y + best) continue;
        if (!fly_line_project(rd->points, rd->point_count, q, &d, &sep)) continue;
        if (sep < best) { best = sep; best_d = d; found = r; }
    }
    if (found < 0) return -1;
    if (distance) *distance = best_d;
    if (separation) *separation = best;
    return found;
}

int fly_road_at(const fly_road_net *net, int loc, int *out, int max) {
    int i, n = 0;
    if (!net || !out) return 0;
    for (i = 0; i < net->count && n < max; ++i)
        if (net->road[i].from == loc || net->road[i].to == loc) out[n++] = i;
    return n;
}

int fly_road_end(const fly_road_net *net, int road, int loc, fly_v3 *pos, fly_v3 *inward) {
    const fly_road *rd;
    fly_v3 end, next;
    float dx, dy, len;
    if (!net || road < 0 || road >= net->count) return 0;
    rd = &net->road[road];
    if (rd->point_count < 2) return 0;
    /* The terminus is the end of the line nearest this settlement, and the
       station one in from it is the way the road came — so the heading from
       the second point to the first is the heading toward the settlement. */
    if (rd->from == loc) {
        end = rd->points[0].pos;
        next = rd->points[1].pos;
    } else if (rd->to == loc) {
        end = rd->points[rd->point_count - 1].pos;
        next = rd->points[rd->point_count - 2].pos;
    } else {
        return 0;
    }
    dx = end.x - next.x;
    dy = end.y - next.y;
    len = hypotf(dx, dy);
    if (len < 1e-3f) return 0;
    if (pos) *pos = end;
    if (inward) *inward = fly_v3mk(dx / len, dy / len, 0.0f);
    return 1;
}

int fly_road_near(const fly_road_net *net, fly_wpos p) {
    int cx, cy, bit;
    if (!net || net->count <= 0) return 0;
    cx = (int)floorf((p.e + FLY_WORLD_HALF) / FLY_ROAD_MASK_CELL);
    cy = (int)floorf((p.n + FLY_WORLD_HALF) / FLY_ROAD_MASK_CELL);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= FLY_ROAD_MASK) cx = FLY_ROAD_MASK - 1;
    if (cy >= FLY_ROAD_MASK) cy = FLY_ROAD_MASK - 1;
    bit = cy * FLY_ROAD_MASK + cx;
    return (net->mask[bit >> 3] >> (bit & 7)) & 1;
}

float fly_road_gap(const fly_road_net *net, fly_wpos p) {
    float best = FLY_ROAD_REACH;
    int r, i;
    if (!fly_road_near(net, p)) return FLY_ROAD_REACH;
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        if (p.e < rd->lo.x - best || p.e > rd->hi.x + best) continue;
        if (p.n < rd->lo.y - best || p.n > rd->hi.y + best) continue;
        for (i = 0; i + 1 < rd->point_count; ++i) {
            fly_v3 a = rd->points[i].pos, b = rd->points[i + 1].pos;
            float ex = b.x - a.x, ey = b.y - a.y;
            float len2 = ex * ex + ey * ey, t, dx, dy, d2;
            if (len2 < 1e-4f) continue;
            t = fly_clampf(((p.e - a.x) * ex + (p.n - a.y) * ey) / len2, 0.0f, 1.0f);
            dx = p.e - (a.x + ex * t);
            dy = p.n - (a.y + ey * t);
            d2 = dx * dx + dy * dy;
            if (d2 < best * best) best = sqrtf(d2);
        }
    }
    return best;
}

int fly_road_blocked(const fly_road_net *net, fly_wpos p) {
    int r, i;
    if (!fly_road_near(net, p)) return 0;
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        if (p.e < rd->lo.x - FLY_ROAD_GATE || p.e > rd->hi.x + FLY_ROAD_GATE) continue;
        if (p.n < rd->lo.y - FLY_ROAD_GATE || p.n > rd->hi.y + FLY_ROAD_GATE) continue;
        for (i = 0; i + 1 < rd->point_count; ++i) {
            fly_v3 a = rd->points[i].pos, b = rd->points[i + 1].pos;
            float ex = b.x - a.x, ey = b.y - a.y;
            float len2 = ex * ex + ey * ey, t, dx, dy;
            /* the settlement end needs an apron, not just a corridor: that
               widening is the opening the road comes in through */
            float want = (rd->points[i].distance < FLY_ROAD_GATE_RUN ||
                          rd->length - rd->points[i + 1].distance < FLY_ROAD_GATE_RUN)
                       ? FLY_ROAD_GATE : FLY_ROAD_ROW;
            if (len2 < 1e-4f) continue;
            t = fly_clampf(((p.e - a.x) * ex + (p.n - a.y) * ey) / len2, 0.0f, 1.0f);
            dx = p.e - (a.x + ex * t);
            dy = p.n - (a.y + ey * t);
            if (dx * dx + dy * dy < want * want) return 1;
        }
    }
    return 0;
}

int fly_road_bearing(const fly_road_net *net, int loc, float *out) {
    int r;
    if (!net || !out) return 0;
    for (r = 0; r < net->count; ++r) {
        const fly_road *rd = &net->road[r];
        if (rd->point_count < 2) continue;
        if (rd->from == loc) { *out = rd->bearing_from; return 1; }
        if (rd->to == loc) { *out = rd->bearing_to; return 1; }
    }
    return 0;
}
