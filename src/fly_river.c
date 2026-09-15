#include "fly_river.h"

#include "fly_rail.h"

#include <math.h>
#include <string.h>

/* ---------------- putting water on the land ----------------
 *
 * The planet had a sea and nothing else wet: every valley the orogeny cut was
 * dry, which is the largest natural feature a world can be missing and the one
 * a pilot would navigate by.
 *
 * A river is a line laid across the ground, and this codebase had already
 * written that survey twice — fly_rail_lay and the road network on top of it.
 * What is different here is that the line is not chosen, it is *found*: water
 * goes downhill, so the only question the survey has to answer is where the
 * downhill goes, and the answer is a property of the heightfield rather than
 * of anybody's preference.
 *
 * The whole of it is one pass over a coarse lattice of the chart:
 *
 *   1. Sample the ground. RGRID across the playable chart, which is
 *      312 m a cell — coarse for a channel and ample for a catchment, and it
 *      is the catchment this is looking for. The line is smoothed off the
 *      lattice afterwards.
 *
 *   2. Flood it from the outside in, lowest first (`priority_flood`). Every
 *      cell comes out with the height water standing on it would have to reach
 *      before it could leave: on a hillside that is the ground itself, and in a
 *      hollow it is the lowest lip the hollow has. That single number is both
 *      halves of the entry — it is what makes every descent terminate at the
 *      sea instead of dead-ending in a pit, *and* it is the lake, because a
 *      cell whose filled height stands above its ground is a cell under water.
 *      A lake is therefore the answer to a question the trace already had to
 *      ask rather than a second placement rule with a scatter of its own.
 *
 *   3. Accumulate. Every cell drains to one neighbour, so summing rainfall
 *      down that tree says how much country each cell carries — which is what
 *      decides where a river is worth drawing and how wide it is when it gets
 *      there. Rainfall is `fly_world_aridity` read backwards, so a desert
 *      catchment carries a stream and a watered one carries a river.
 *
 *   4. Trace, from the sources carrying the most country, by steepest descent
 *      to the sea or to a lake. Smooth the staircase the lattice leaves off,
 *      simplify what is left, and publish it.
 *
 * What it costs to publish is the whole of the design. `fly_world_ground` has
 * a GLSL twin and a path-tracer copy, so a channel it knows about is a channel
 * every shader walks per invocation — see fly_world.h for why the counts are
 * what they are. The budget is spent on the longest rivers rather than on the
 * most of them: one watercourse you can follow home is worth four ditches. */

/* The lattice. 192 across ±30 km is 312 m a cell, and the number is set by
 * what the flood is for rather than by what a channel looks like: a basin has
 * to be several cells across before "this hollow does not drain" is a fact
 * about the terrain rather than about the sampling, and a 40 m channel is
 * never going to be resolved by any lattice this survey could afford. */
#define RGRID 192
#define RCELLS (RGRID * RGRID)
#define RNONE 0xFFFFu

/* How finely the traced line is resampled before it is shaped, in metres. Not
 * the station pitch — this is the line the smoothing, the snap and the
 * curvature bound all work on, and the stations are chosen off it afterwards.
 * A shape is only as good as the samples it is carried on and these are cheap:
 * nothing here touches the heightfield. */
#define RIVER_FINE 130.0f

/* --- how a river is allowed to turn --------------------------------------
 *
 * A river meanders; it does not take corners. Both of these are about that and
 * they work at different scales.
 *
 * FLY_RIVER_RADIUS is the tightest bend the published line may hold, and it is
 * `fly_rail_bend` that holds it — the same relaxation the guideway is laid
 * with, because "this line may not turn more sharply than that" is the same
 * problem for both and neither smoothing passes nor a spline *bound* anything.
 *
 * RIVER_TURN is what a station is for. The surface, the channel and the banks
 * are all interpolated linearly between stations, so what a viewer sees turn is
 * the angle between two spans and nothing else: a station goes in wherever the
 * line has turned this much since the last one, and a straight reach costs two
 * whatever its length. That is the whole reason the stations are not evenly
 * spaced. Douglas-Peucker was here first, and it is the wrong measure —
 * bounding how far the chord strays from the line says nothing about the angle
 * where two chords meet, so a wandering river came out as kilometre-long
 * straights meeting at fifty degrees.
 *
 * The two together bound the station count from below: at the tightest bend a
 * turn of RIVER_TURN takes FLY_RIVER_RADIUS * RIVER_TURN metres of river, which
 * is 300 m, so even a river that bends the whole way costs about what an even
 * pitch would have. What a station's turn actually comes out at is that plus
 * one fine step's worth of overshoot, because the threshold is tested at the
 * samples and crossed between two of them — which is what RIVER_FINE is set
 * against as much as the shape is. RIVER_SPAN_MAX is the other end — a station every so often
 * regardless, because the surface is a straight line between stations and it
 * has to be able to follow the ground down as well as round. */
#define FLY_RIVER_RADIUS 2600.0f
#define RIVER_TURN 0.115f      /* radians: six and a half degrees */
#define RIVER_SPAN_MAX 780.0f
#define RIVER_SPAN_MIN 220.0f
#define RIVER_PT_PER_RIVER 28
/* The deepest a channel may be cut below the ground it runs through before the
 * survey gives up on that line. A river valley is tens of metres; a slot two
 * hundred metres deep is a line that ended up on a shelf or against a cliff,
 * where the lowest ground in its own section is nowhere near it. */
#define FLY_RIVER_CUT_MAX 26.0f
/* How many points along a span the chord is held under the ground. Seven
 * splits a span into eighths, and the longest span the simplification
 * produces is about a kilometre. */
#define FLY_RIVER_SUBS 7
/* How much standing water ends a river rather than being run through: see
 * ponded(). Three metres is deeper than the lattice's own sampling error and
 * shallower than anything that reads as a body of water from the air. */
#define FLY_RIVER_POND 3.0f
/* How far clear of a lake's level the ground inside its disc has to read on
 * the lattice before the disc is trusted — see find_lakes. */
#define FLY_LAKE_RIM 0.5f
/* The least a lake's guaranteed-water core may be, in metres of radius: a
 * pool eight hundred metres across is a landmark and one two hundred across
 * is a puddle that costs the same to carry. */
#define FLY_LAKE_CORE_MIN 400.0f

/* Working state. File-static for the reason fly_road.c's is: it is a megabyte,
 * it is meaningless the instant fly_river_lay returns, and nothing outside
 * this file has any use for it. */
static struct {
    float h[RCELLS];      /* the ground, as the survey found it */
    float fill[RCELLS];   /* ...raised until every cell can drain */
    float acc[RCELLS];    /* country draining through here, in cells of rain */
    float rain[RCELLS];
    uint16_t down[RCELLS];   /* the neighbour this cell drains into */
    uint16_t order[RCELLS];  /* the flood's own order: a parent before its children */
    uint16_t lake[RCELLS];   /* which lake a submerged cell belongs to, or RNONE */
    uint16_t claim[RCELLS];  /* which published river has already taken this cell */
    uint8_t done[RCELLS];
    struct { float f; uint16_t c; } open[RCELLS];
    int nopen, norder;
    uint16_t stack[RCELLS];
    float ox, oy, cell;
} R;

static float rx_of(int i) { return R.ox + (float)(i % RGRID) * R.cell; }
static float ry_of(int i) { return R.oy + (float)(i / RGRID) * R.cell; }

static void flood_push(float f, uint16_t c) {
    int i = R.nopen++;
    R.open[i].f = f;
    R.open[i].c = c;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (R.open[p].f <= R.open[i].f) break;
        { float tf = R.open[p].f; uint16_t tc = R.open[p].c;
          R.open[p] = R.open[i];
          R.open[i].f = tf; R.open[i].c = tc; }
        i = p;
    }
}

static uint16_t flood_pop(void) {
    uint16_t top = R.open[0].c;
    int i = 0;
    R.open[0] = R.open[--R.nopen];
    for (;;) {
        int l = i * 2 + 1, r = l + 1, m = i;
        if (l < R.nopen && R.open[l].f < R.open[m].f) m = l;
        if (r < R.nopen && R.open[r].f < R.open[m].f) m = r;
        if (m == i) break;
        { float tf = R.open[m].f; uint16_t tc = R.open[m].c;
          R.open[m] = R.open[i];
          R.open[i].f = tf; R.open[i].c = tc; }
        i = m;
    }
    return top;
}

/* Is this cell under water once the flood has finished with it? A cell whose
 * filled height stands above its ground is the floor of a basin, and half a
 * metre is the noise floor of a lattice this coarse. Every lake is made of
 * these; so is every hollow too small, too straggling or too late in the queue
 * to have been published as one, and a river may not run through either. */
static int submerged(int c) { return R.fill[c] > R.h[c] + 0.5f; }

/* And is it enough water to be the end of a river?
 *
 * Not the same question, and the difference is most of the length of every
 * river in the world. A heightfield with nine metres of relief at a two
 * hundred metre wavelength is pitted with hollows a lattice cell across that
 * hold half a metre; a trace that stopped at the first of *those* got four
 * kilometres from its source and ended in a puddle nobody would draw. A river
 * runs through a hollow that shallow — it fills it and carries on, which is
 * what the whole entry says about basins — and the stage is read off the
 * ground under the line rather than off the lattice, so passing through one
 * costs nothing. What ends a river is water deep enough to be water. */
static int ponded(int c) { return R.fill[c] > R.h[c] + FLY_RIVER_POND; }

static const int NB_DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int NB_DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

/* Priority flood: raise every cell to the height water on it would have to
 * reach before it could leave the map.
 *
 * The queue starts on everything water can already leave from — the edge of
 * the lattice and every cell that is under the sea — and grows inward lowest
 * first. A cell reached from a neighbour standing at `f` cannot drain below
 * `f`, so its filled height is the higher of its own ground and that, and the
 * neighbour it was reached from is the way out: the flood runs *up* the
 * drainage, so the cell it came from is downstream of it by construction.
 *
 * That is what makes the result total. Every cell has a way to the sea, no
 * descent can dead-end, and the hollows that had no outlet at all come out
 * holding water to the exact height of their lowest lip — which is the lake,
 * and it cost nothing beyond the pass that had to happen anyway. */
static void priority_flood(void) {
    int i, d;
    R.nopen = R.norder = 0;
    for (i = 0; i < RCELLS; ++i) {
        int cx = i % RGRID, cy = i / RGRID;
        R.done[i] = 0;
        R.down[i] = RNONE;
        R.fill[i] = R.h[i];
        if (cx == 0 || cy == 0 || cx == RGRID - 1 || cy == RGRID - 1 ||
            R.h[i] <= FLY_WATER_LEVEL) {
            R.done[i] = 1;
            flood_push(R.h[i], (uint16_t)i);
        }
    }
    while (R.nopen > 0) {
        uint16_t cur = flood_pop();
        int cx = cur % RGRID, cy = cur / RGRID;
        R.order[R.norder++] = cur;
        for (d = 0; d < 8; ++d) {
            int nx = cx + NB_DX[d], ny = cy + NB_DY[d], n;
            if (nx < 0 || ny < 0 || nx >= RGRID || ny >= RGRID) continue;
            n = ny * RGRID + nx;
            if (R.done[n]) continue;
            R.done[n] = 1;
            R.fill[n] = R.h[n] > R.fill[cur] ? R.h[n] : R.fill[cur];
            R.down[n] = cur;
            flood_push(R.fill[n], (uint16_t)n);
        }
    }
}

/* Where a cell actually sends its water.
 *
 * Steepest descent on the filled surface wherever there is one — the fall per
 * metre travelled, so a diagonal is not preferred for being longer — and the
 * flood's own parent where there is not, which is every cell of every lake and
 * every flat. Both are acyclic and for different reasons: a steepest step
 * strictly lowers the filled height, and a parent step strictly lowers the
 * order the flood popped in, so no chain of either can come back to where it
 * started. */
static void flow_directions(void) {
    int i, d;
    for (i = 0; i < RCELLS; ++i) {
        int cx = i % RGRID, cy = i / RGRID;
        float best = 0.0f;
        int pick = -1;
        for (d = 0; d < 8; ++d) {
            int nx = cx + NB_DX[d], ny = cy + NB_DY[d], n;
            float run, slope;
            if (nx < 0 || ny < 0 || nx >= RGRID || ny >= RGRID) continue;
            n = ny * RGRID + nx;
            if (R.fill[n] >= R.fill[i]) continue;
            run = (NB_DX[d] && NB_DY[d]) ? R.cell * 1.41421356f : R.cell;
            slope = (R.fill[i] - R.fill[n]) / run;
            if (slope > best) { best = slope; pick = n; }
        }
        if (pick >= 0) R.down[i] = (uint16_t)pick;
    }
}

/* How much country drains through each cell, in cells of rain. Summed against
 * the flood's own order, which puts every cell after the one it drains into —
 * so walking it backwards hands a cell all of its tributaries before it has to
 * pass anything on. */
static void accumulate(void) {
    int k;
    for (k = 0; k < RCELLS; ++k) R.acc[k] = R.rain[k];
    for (k = R.norder - 1; k >= 0; --k) {
        int c = R.order[k];
        if (R.down[c] != RNONE) R.acc[R.down[c]] += R.acc[c];
    }
}

/* --- the lakes -----------------------------------------------------------
 *
 * A submerged cell is one the flood had to raise: its filled height stands
 * above its ground. Neighbouring cells raised to the *same* height are the
 * same body of water, so a lake is one connected run of them, and its surface
 * is that height — the lip the basin spills over, which is where the river
 * below it starts.
 *
 * What is published is a disc and a level rather than an outline. The water is
 * drawn wherever the ground inside the disc is under the level, so the
 * shoreline is the terrain's own contour and the disc is only a bound — which
 * is why the bound has to be honest, and why a basin that is not compact, or
 * whose disc would flood ground that is not part of it, is refused rather than
 * approximated. */
static void find_lakes(fly_world *w) {
    int i, k;
    for (i = 0; i < RCELLS; ++i) R.lake[i] = RNONE;
    for (i = 0; i < RCELLS; ++i) {
        float z, cx = 0.0f, cy = 0.0f, far = 0.0f, area, core;
        int ns = 0, n = 0, head = 0, bad = 0;
        fly_lake L;
        if (R.lake[i] != RNONE || !submerged(i)) continue;
        z = R.fill[i];
        R.stack[ns++] = (uint16_t)i;
        R.lake[i] = 0xFFFEu;  /* provisionally taken, so the walk cannot revisit */
        while (head < ns) {
            int c = R.stack[head++], gx = c % RGRID, gy = c / RGRID, d;
            cx += rx_of(c);
            cy += ry_of(c);
            ++n;
            for (d = 0; d < 8; ++d) {
                int nx = gx + NB_DX[d], ny = gy + NB_DY[d], nc;
                if (nx < 0 || ny < 0 || nx >= RGRID || ny >= RGRID) continue;
                nc = ny * RGRID + nx;
                if (R.lake[nc] != RNONE) continue;
                if (!submerged(nc)) continue;
                if (fabsf(R.fill[nc] - z) > 0.05f) continue;
                R.lake[nc] = 0xFFFEu;
                if (ns < RCELLS) R.stack[ns++] = (uint16_t)nc;
            }
        }
        cx /= (float)n;
        cy /= (float)n;
        for (k = 0; k < ns; ++k) {
            float dx = rx_of(R.stack[k]) - cx, dy = ry_of(R.stack[k]) - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 > far) far = d2;
        }
        far = sqrtf(far) + R.cell;
        area = (float)n * R.cell * R.cell;
        /* And how big a disc round that centre is *nothing but* this water.
         *
         * The far radius is the basin's reach and the disc is drawn on it, but
         * a basin is not a disc and the difference is somebody else's ground
         * inside the circle. On the lattice that is checked for below; what
         * the lattice cannot see is a hollow between two cells that both read
         * clear of the level, and a disc drawn out to the basin's reach finds
         * enough of those to speckle a night frame with isolated pools six
         * kilometres away — which is what it did. Inside this radius every
         * cell is the lake's own, so the only thing under the level there is
         * the lake. */
        core = 1e30f;
        for (k = 0; k < RCELLS; ++k) {
            float dx, dy, d2;
            if (R.lake[k] == 0xFFFEu) continue;
            dx = rx_of(k) - cx;
            dy = ry_of(k) - cy;
            d2 = dx * dx + dy * dy;
            if (d2 < core) core = d2;
        }
        core = sqrtf(core) - R.cell * 0.5f;
        if (core > far) core = far;
        /* Compact enough for a disc to be a fair bound, and big enough to be
         * worth one of six. A ribbon of hollows strung along a valley is not a
         * lake and a disc around it is a lie about a great deal of dry ground. */
        if (n >= 6 && far <= 4000.0f && core >= FLY_LAKE_CORE_MIN &&
            area >= 0.30f * FLY_PI * far * far) {
            /* And the bound has to hold: no ground inside the disc that is
             * under the surface and is not part of this water. Left unchecked,
             * a disc drawn round one basin floods the next valley over. */
            /* The disc is the core plus the band the level is let down over,
             * and never past the basin's own reach plus that band. */
            float rr = core + FLY_LAKE_MARGIN;
            if (rr > far + FLY_LAKE_MARGIN) rr = far + FLY_LAKE_MARGIN;
            int i0 = (int)((cx - rr - R.ox) / R.cell), i1 = (int)((cx + rr - R.ox) / R.cell) + 1;
            int j0 = (int)((cy - rr - R.oy) / R.cell), j1 = (int)((cy + rr - R.oy) / R.cell) + 1;
            int gx, gy;
            if (i0 < 0) i0 = 0;
            if (j0 < 0) j0 = 0;
            if (i1 > RGRID - 1) i1 = RGRID - 1;
            if (j1 > RGRID - 1) j1 = RGRID - 1;
            for (gy = j0; gy <= j1 && !bad; ++gy)
                for (gx = i0; gx <= i1 && !bad; ++gx) {
                    int c = gy * RGRID + gx;
                    float dx = rx_of(c) - cx, dy = ry_of(c) - cy;
                    if (dx * dx + dy * dy > rr * rr) continue;
                    if (R.lake[c] == 0xFFFEu) continue;
                    /* Nothing under the level inside the disc that is not
                     * this water: a disc drawn round one basin that reaches
                     * into the next valley floods it. Half a metre rather than
                     * nothing, because the lip a basin spills over reads as
                     * level with it and every lake there is would fail a
                     * margin. What the lattice cannot see — a hollow between
                     * two cells, both of which read clear — is what the rim
                     * band is for; see FLY_LAKE_EDGE. */
                    if (R.h[c] < z - FLY_LAKE_RIM) bad = 1;
                }
            if (!bad && w->nlake < FLY_LAKE_MAX) {
                L.x = cx; L.y = cy; L.r = rr; L.z = z;
                w->lake[w->nlake] = L;
                for (k = 0; k < ns; ++k) R.lake[R.stack[k]] = (uint16_t)w->nlake;
                ++w->nlake;
                continue;
            }
        }
        for (k = 0; k < ns; ++k) R.lake[R.stack[k]] = 0xFFFDu;  /* seen, not a lake */
    }
    for (i = 0; i < RCELLS; ++i)
        if (R.lake[i] >= 0xFFFDu) R.lake[i] = RNONE;
}

/* --- laying one river ----------------------------------------------------- */

/* Follow the water from `src` to wherever it goes, writing the cells it passes
 * through. Stops at the sea, at a lake, at a cell an earlier river has already
 * taken, or at the edge of the lattice. `end_lake` comes back with the lake it
 * ended in, or -1; `hit_claim` with whether it ran into another river, which is
 * a refusal rather than a confluence — see fly_river_lay. */
static int trace_down(uint16_t src, uint16_t *out, int max, int *end_lake, int *hit_claim) {
    int n = 0;
    uint16_t c = src;
    *end_lake = -1;
    *hit_claim = 0;
    if (R.claim[c] != RNONE) { *hit_claim = 1; return 0; }
    while (n + 1 < max) {
        out[n++] = c;
        if (R.h[c] <= FLY_WATER_LEVEL) return n;
        if (R.down[c] == RNONE) return n;
        c = R.down[c];
        if (R.claim[c] != RNONE) { *hit_claim = 1; return n; }
        /* Any standing water ends the river, published lake or not. What is
         * downstream of a basin is a different river — it starts at the spill,
         * which is a candidate source in its own right — and a line that ran
         * *through* one would be surveyed against a filled surface tens of
         * metres over the ground it is drawn on, which is a river on an
         * embankment with the country falling away either side of it. */
        if (R.lake[c] != RNONE || ponded(c)) {
            *end_lake = R.lake[c] == RNONE ? -1 : (int)R.lake[c];
            out[n++] = c;
            return n;
        }
    }
    return n;
}

/* Choose the stations: one wherever the line has turned RIVER_TURN since the
 * last, and one every RIVER_SPAN_MAX regardless.
 *
 * The turn is accumulated *signed*, so a reach that bends one way and back
 * again does not read as straight — the two halves would cancel on a sum of
 * absolute angles too, but a signed sum is what a chord actually has to
 * follow, and it is what lets a long lazy S run on two stations while a hairpin
 * gets four. Both ends are stations by construction: the mouth is where the
 * water meets something and the head is where the survey stopped looking. */
static void stations(const fly_v2 *p, int n, unsigned char *keep) {
    float turn = 0.0f, run = 0.0f;
    int i;
    for (i = 0; i < n; ++i) keep[i] = 0;
    keep[0] = keep[n - 1] = 1;
    for (i = 1; i + 1 < n; ++i) {
        float ax = p[i].x - p[i - 1].x, ay = p[i].y - p[i - 1].y;
        float bx = p[i + 1].x - p[i].x, by = p[i + 1].y - p[i].y;
        float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
        run += la;
        if (la < 1e-3f || lb < 1e-3f) continue;
        turn += atan2f(ax * by - ay * bx, ax * bx + ay * by);
        if (run < RIVER_SPAN_MIN) continue;
        if (fabsf(turn) < RIVER_TURN && run < RIVER_SPAN_MAX) continue;
        keep[i] = 1;
        turn = 0.0f;
        run = 0.0f;
    }
}

/* Would this line run through somewhere people live?
 *
 * A pad's apron is graded into the heightfield and a channel is cut out of it,
 * and the two arguing over the same ground is a river through the aerodrome.
 * Rather than let either win, a line that comes near a settlement is not
 * published at all — there are more candidate sources than there is budget, so
 * refusing one costs nothing but the next-longest river. The margin is the
 * settlement's own clearing, so what is kept out is the built ground and not
 * merely the pad. */
static int clears_sites(const fly_world *w, const fly_v2 *p, int n) {
    int i, k;
    for (i = 0; i < n; ++i)
        for (k = 0; k < w->nloc; ++k) {
            float keep = fly_world_clearing(w->loc[k].kind) + 120.0f;
            float dx = p[i].x - w->loc[k].pos.e, dy = p[i].y - w->loc[k].pos.n;
            if (dx * dx + dy * dy < keep * keep) return 0;
        }
    return 1;
}

/* The height the water may stand at here: the lowest ground anywhere across
 * the channel and its banks.
 *
 * Not the ground on the centreline, which is what it was first, and the
 * difference is the whole of whether a river holds its water. The stage is
 * what `fly_world_water` answers inside the footprint and the footprint has a
 * hard outer edge, so ground at that edge standing *under* the surface is a
 * wall of water with the country falling away behind it. Taking the section's
 * own minimum makes containment a property of the number rather than something
 * to hope the lattice got right: the ground on the bank is above the water
 * because the water was put at the height of the lowest bank there is.
 *
 * Sampled a little past the footprint for the same reason a cutting's shelf is
 * sampled past the carriageway — what is drawn has to be inside what was
 * measured, not exactly equal to it. */
static float channel_floor(const fly_world *w, fly_v2 a, fly_v2 b, fly_v2 p,
                           float wide, float batter) {
    float ex = b.x - a.x, ey = b.y - a.y, len = sqrtf(ex * ex + ey * ey);
    float nx, ny, tx, ty, lo = fly_world_ground(w, p.x, p.y);
    /* Out to a fifth past the footprint, in steps that land on the footprint's
     * own edge exactly. Past it, because what is drawn has to be inside what
     * was measured; *on* it, because that edge is where the water stops and a
     * sample that straddles it is a sample that missed the one point this
     * function exists to bound. */
    float span = (wide + batter) * 1.2f;
    int k, m;
    if (len < 1e-3f) return lo;
    tx = ex / len;
    ty = ey / len;
    nx = -ty;
    ny = tx;
    /* Across the line only. Along it the ground is *meant* to fall — that is
     * what a river is — and taking a minimum in that direction would hand
     * every station the level of the one below it and carve the difference
     * out of the hillside between them. The along direction is covered by
     * there being a station along it often enough. */
    for (m = 0, k = -12; k <= 12; ++k) {
        float off = span * (float)k / 12.0f;
        float g = fly_world_ground(w, p.x + nx * off, p.y + ny * off);
        if (g < lo) lo = g;
    }
    (void)m;
    return lo;
}

/* Diffuse the line toward its own chord. Pins the ends, which is where the
 * mouth is. */
static void smooth_line(fly_v2 *p, int n, int passes) {
    int pass, i;
    for (pass = 0; pass < passes; ++pass)
        for (i = 1; i + 1 < n; ++i) {
            p[i].x += 0.30f * (p[i - 1].x + p[i + 1].x - 2.0f * p[i].x);
            p[i].y += 0.30f * (p[i - 1].y + p[i + 1].y - 2.0f * p[i].y);
        }
}

/* Put a station in the bottom of its own valley.
 *
 * The trace is a walk on 312 m cells and the smoothing above is a chord across
 * the zigzag it leaves, so between the two the line is somewhere in the right
 * valley and nowhere near the bottom of it — which for a river is the whole
 * of the answer. A channel cut two hundred metres up the side of a valley is a
 * canal on a shelf with the ground falling away beside it, and the terrain
 * does not care that the survey meant well.
 *
 * So each station is walked across the line to the lowest ground within about
 * a cell of it. Across rather than along, because along is the direction the
 * descent already solved; and bounded by the cell, because past that it is not
 * the same valley any more. */
static void snap_to_valley(const fly_world *w, fly_v2 *p, int n) {
    int i, k;
    for (i = 0; i < n; ++i) {
        fly_v2 a = p[i > 0 ? i - 1 : i], b = p[i + 1 < n ? i + 1 : i];
        float ex = b.x - a.x, ey = b.y - a.y, len = sqrtf(ex * ex + ey * ey);
        float nx, ny, best, span = R.cell * 0.55f;
        float bx = p[i].x, by = p[i].y;
        if (len < 1e-3f) continue;
        nx = -ey / len;
        ny = ex / len;
        best = fly_world_ground(w, bx, by);
        for (k = -4; k <= 4; ++k) {
            float off = span * (float)k / 4.0f;
            float x = p[i].x + nx * off, y = p[i].y + ny * off;
            float g = fly_world_ground(w, x, y);
            if (g < best) { best = g; bx = x; by = y; }
        }
        p[i].x = bx;
        p[i].y = by;
    }
}

/* How wide a channel the country behind a station fills, as a half width in
 * metres. Hydraulic geometry is a power law in discharge and this is the same
 * shape of thing against catchment area, clamped at both ends.
 *
 * The floor is what makes the entry worth its budget rather than what a
 * catchment that size would really carry: the first cut ran 6 m to 26 m, and
 * from two thousand feet — which is where this is flown from — a twelve-metre
 * stream is under a pixel and a river came apart into a chain of dashes with
 * the reaches between them invisible. Twenty metres across at the head and
 * eighty at the mouth is a watercourse you can follow. The ceiling is what the
 * lattice that found it can justify. */
static float channel_wide(float acc_cells, float cellarea) {
    float km2 = acc_cells * cellarea / 1.0e6f;
    float wide = 3.2f * sqrtf(km2 > 0.0f ? km2 : 0.0f);
    return fly_clampf(wide, 10.0f, 40.0f);
}

/* The most points a resampled line may carry before the stations are chosen
 * off it. Sixty-six kilometres of river at RIVER_FINE, which is most of the
 * chart's 85 km diagonal and not all of it — so a trace longer than this gives
 * up its head, the same end and for the same reason the budget takes one. */
#define RIVER_RAW_MAX 512

typedef struct { uint16_t src; float len; } river_cand;

void fly_river_lay(fly_world *w) {
    static uint16_t path[RCELLS];
    static fly_v2 line[RIVER_RAW_MAX];
    static float lacc[RIVER_RAW_MAX];
    static unsigned char keep[RIVER_RAW_MAX];
    static float floor_z[RIVER_RAW_MAX];
    static float gz_c[RIVER_RAW_MAX];
    static float zs[RIVER_RAW_MAX];
    static int kidx[RIVER_RAW_MAX];
    static float spanf[RIVER_RAW_MAX * FLY_RIVER_SUBS];
    river_cand cand[192];
    int ncand = 0, i, k, r, attempt;

    w->nriver = w->nriver_pt = w->nlake = 0;
    R.ox = -FLY_WORLD_HALF;
    R.oy = -FLY_WORLD_HALF;
    R.cell = 2.0f * FLY_WORLD_HALF / (float)(RGRID - 1);

    for (i = 0; i < RCELLS; ++i) {
        float x = rx_of(i), y = ry_of(i), arid;
        R.h[i] = fly_world_ground(w, x, y);
        R.claim[i] = RNONE;
        /* What falls on this cell. A desert catchment carries a stream and a
         * watered one carries a river, and the field that says which is the
         * one the planet is already drawn with. */
        arid = fly_world_aridity(w->seed, fly_world_ball_dir(w->frame, x, y));
        R.rain[i] = 0.15f + 0.85f * (1.0f - arid) * (1.0f - arid);
    }
    priority_flood();
    flow_directions();
    accumulate();
    find_lakes(w);

    /* Candidate sources: high ground already carrying enough country to be a
     * watercourse rather than a rill, spread out so two rivers do not start
     * within sight of each other. Ranked by how far the water actually gets,
     * because length is the whole of what a landmark is. */
    for (i = 0; i < RCELLS && ncand < (int)(sizeof cand / sizeof cand[0]); ++i) {
        float x, y, len = 0.0f;
        int n, el, hc, ok = 1, outlet = 0, d;
        if (R.h[i] <= FLY_WATER_LEVEL + 60.0f) continue;
        if (R.lake[i] != RNONE || ponded(i)) continue;
        if (R.acc[i] < 55.0f) continue;
        /* A cell water leaves a basin through is the head of the river below
         * it, whatever it is already carrying: that is what "fill the basin to
         * its spill point and carry on" means, and it is the only way the
         * reach under a lake ever gets surveyed. */
        for (d = 0; d < 8; ++d) {
            int nx = (i % RGRID) + NB_DX[d], ny = (i / RGRID) + NB_DY[d], nc;
            if (nx < 0 || ny < 0 || nx >= RGRID || ny >= RGRID) continue;
            nc = ny * RGRID + nx;
            if ((R.lake[nc] != RNONE || ponded(nc)) && R.down[nc] == (uint16_t)i)
                { outlet = 1; break; }
        }
        /* Otherwise the source is where the stream starts, so it is the
         * *smallest* catchment worth drawing — a cell whose own feeder already
         * qualifies is somewhere in the middle of a river, not the head. */
        if (!outlet && R.down[i] != RNONE && R.acc[i] > 140.0f) continue;
        x = rx_of(i); y = ry_of(i);
        for (k = 0; k < ncand; ++k) {
            float dx = x - rx_of(cand[k].src), dy = y - ry_of(cand[k].src);
            if (dx * dx + dy * dy < 3500.0f * 3500.0f) { ok = 0; break; }
        }
        if (!ok) continue;
        n = trace_down((uint16_t)i, path, RCELLS, &el, &hc);
        for (k = 1; k < n; ++k) {
            float dx = rx_of(path[k]) - rx_of(path[k - 1]);
            float dy = ry_of(path[k]) - ry_of(path[k - 1]);
            len += sqrtf(dx * dx + dy * dy);
        }
        if (len < FLY_RIVER_MIN_LEN) continue;
        cand[ncand].src = (uint16_t)i;
        cand[ncand].len = len;
        ++ncand;
    }
    /* Longest first, ties broken by cell index so the answer does not depend
     * on anything but the seed. */
    for (i = 0; i < ncand; ++i) {
        int best = i;
        for (k = i + 1; k < ncand; ++k)
            if (cand[k].len > cand[best].len ||
                (cand[k].len == cand[best].len && cand[k].src < cand[best].src)) best = k;
        if (best != i) { river_cand t = cand[i]; cand[i] = cand[best]; cand[best] = t; }
    }

    /* Only the best of them are worth surveying properly: everything below
       here samples the heightfield per station, and the list is already in
       the order that matters. */
    if (ncand > 64) ncand = 64;
    /* Twice, and the second time is a concession rather than a preference.
     * The depth cap below refuses a line whose own section floor is a long way
     * under it, which is the right answer while there is another candidate to
     * try — but on a world whose every descent is like that it is the
     * difference between a compromised river and no water on the land at all,
     * which is the thing this entry exists to fix. So the strict pass runs
     * first, and only a world it left dry gets the loose one. */
    for (attempt = 0; attempt < 2; ++attempt) {
    float cutmax = attempt ? FLY_RIVER_CUT_MAX * 2.0f : FLY_RIVER_CUT_MAX;
    if (attempt && w->nriver > 0) break;
    for (r = 0; r < ncand && w->nriver < FLY_RIVER_MAX; ++r) {
        int n, el, hc, np = 0, nk = 0, pass, budget, head = 0;
        float run = 0.0f, prev;
        fly_river rv;
        if (FLY_RIVER_PT_MAX - w->nriver_pt < FLY_RIVER_MIN_PT) break;
        n = trace_down(cand[r].src, path, RCELLS, &el, &hc);
        if (hc || n < 3) continue;
        if (el < 0 && R.h[path[n - 1]] > FLY_WATER_LEVEL) continue;  /* went nowhere */

        /* Resample the lattice walk at a fixed pitch, so what follows is
         * carried on samples evenly spread along the water rather than along
         * the diagonal steps the eight-way descent happened to take.
         *
         * From far enough down the path that the whole of it fits: a walk
         * longer than the buffer would otherwise be resampled until the buffer
         * filled and then joined to its own mouth by one straight span across
         * however much river was left. */
        {
            float total = 0.0f;
            for (k = 1; k < n; ++k)
                total += hypotf(rx_of(path[k]) - rx_of(path[k - 1]),
                                ry_of(path[k]) - ry_of(path[k - 1]));
            for (head = 0; head + 3 < n &&
                 total > (float)(RIVER_RAW_MAX - 2) * RIVER_FINE; ++head)
                total -= hypotf(rx_of(path[head + 1]) - rx_of(path[head]),
                                ry_of(path[head + 1]) - ry_of(path[head]));
        }
        line[np] = fly_v2mk(rx_of(path[head]), ry_of(path[head]));
        lacc[np] = R.acc[path[head]];
        ++np;
        for (k = head + 1; k < n && np + 1 < RIVER_RAW_MAX; ++k) {
            float ax = rx_of(path[k - 1]), ay = ry_of(path[k - 1]);
            float bx = rx_of(path[k]), by = ry_of(path[k]);
            float seg = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            float used = 0.0f;
            if (seg < 1e-3f) continue;
            while (run + (seg - used) >= RIVER_FINE && np + 1 < RIVER_RAW_MAX) {
                float t;
                used += RIVER_FINE - run;
                run = 0.0f;
                t = used / seg;
                line[np] = fly_v2mk(fly_lerpf(ax, bx, t), fly_lerpf(ay, by, t));
                lacc[np] = fly_lerpf(R.acc[path[k - 1]], R.acc[path[k]], t);
                ++np;
            }
            run += seg - used;
        }
        /* the mouth is a station whatever the pitch left over: it is where the
           river meets what it ends in, and it is not the survey's to round off */
        line[np] = fly_v2mk(rx_of(path[n - 1]), ry_of(path[n - 1]));
        lacc[np] = R.acc[path[n - 1]];
        ++np;
        if (np < FLY_RIVER_MIN_PT) continue;

        /* Take the staircase off it. Eight-way descent on a 312 m lattice is a
         * zigzag at the scale of the cell and a valley at the scale of the
         * river. The diffusion reaches about sqrt(0.6 * passes) samples, so twenty-two
         * of them is four hundred metres — over the cell and well under the
         * meander. More is not better here and a kilometre of it is what the
         * first cut had: diffusion does not distinguish a staircase from a
         * bend, it just loses the smaller one first, and a river smoothed until
         * it is smooth is a canal. What holds the *shape* is the curvature
         * bound below, which bounds without flattening.
         * The ends are pinned because one of them is the sea. */
        smooth_line(line, np, 22);
        /* Into the bottom of the valley, then a few passes to take the chatter
         * out of what the search picked, then into the bottom again: the
         * smoothing is what makes it a line and the snap is what makes it a
         * *river*, and doing either of them last on its own gives up the
         * other. The mouth is pinned through both — it is where the water
         * meets the sea and it is not the survey's to move. */
        snap_to_valley(w, line, np - 1);
        smooth_line(line, np, 8);
        snap_to_valley(w, line, np - 1);

        /* And last, the bound. Everything above *softens* the line and none of
         * it constrains anything, so a valley that turns a corner leaves a
         * corner in the line and the snap is free to put a new one back. What a
         * river may not do is turn more sharply than FLY_RIVER_RADIUS, and that
         * is a property to constrain rather than to smooth toward — see
         * fly_rail_bend, which is what the guideway is laid with and for the
         * same reason. It comes after the snap and not before it: whichever of
         * the two runs last is the one that decides, and a kink is worse than
         * a channel a few tens of metres off the thalweg. */
        fly_rail_bend(line, np, FLY_RIVER_RADIUS, 3000, NULL, NULL);
        smooth_line(line, np, 4);

        if (!clears_sites(w, line, np)) continue;

        stations(line, np, keep);
        for (k = 0; k < np; ++k) if (keep[k]) ++nk;

        /* What is left has to fit what is unspent, and no river may take so
         * much of the budget that there is nothing left to be a second one.
         *
         * What gives is the length, not the fidelity. Thinning the stations
         * evenly was the first version and it is the wrong trade: the
         * tolerance above is what keeps the drawn channel inside the valley
         * the trace found, and a line that has had every third station taken
         * out of it is a line standing wherever the arithmetic left it. So the
         * head is given up instead — the river starts further down, which is
         * what a spring is — and the mouth, which is the end that has to meet
         * something, is never the part that goes. */
        budget = FLY_RIVER_PT_MAX - w->nriver_pt;
        if (budget > RIVER_PT_PER_RIVER) budget = RIVER_PT_PER_RIVER;
        for (k = 0; k < np && nk > budget; ++k)
            if (keep[k]) { keep[k] = 0; --nk; }
        if (nk < FLY_RIVER_MIN_PT || nk > budget) continue;
        /* Long enough *after* the head has been given up: a river the budget
           cut down to a brook is not the landmark this spent stations on. */
        {
            float kept = 0.0f;
            int a = -1;
            for (k = 0; k < np; ++k) {
                if (!keep[k]) continue;
                if (a >= 0) kept += sqrtf((line[k].x - line[a].x) * (line[k].x - line[a].x) +
                                          (line[k].y - line[a].y) * (line[k].y - line[a].y));
                a = k;
            }
            if (kept < FLY_RIVER_MIN_LEN) continue;
        }

        /* The widest bank this river has, which is what its bounding box and
           its span test have to allow for; the bank at a point is
           fly_river_batter of the channel there. */
        rv.batter = 0.0f;
        for (k = 0; k < np; ++k) {
            float bt = fly_river_batter(channel_wide(lacc[k], R.cell * R.cell));
            if (bt > rv.batter) rv.batter = bt;
        }
        /* What the ground under each station will let the water stand at. Only
           the stations: the ground *between* two of them is what the spans are
           sectioned against below, and everything else on the fine line is
           carrying the shape rather than the profile. */
        for (k = 0; k < np; ++k) {
            float wd;
            if (!keep[k]) continue;
            wd = channel_wide(lacc[k], R.cell * R.cell);
            floor_z[k] = channel_floor(w, line[k > 0 ? k - 1 : k],
                                       line[k + 1 < np ? k + 1 : k], line[k],
                                       wd, fly_river_batter(wd));
            gz_c[k] = fly_world_ground(w, line[k].x, line[k].y);
        }

        /* --- the profile ---------------------------------------------------
         *
         * Three things have to be true of it at once, and each of them was a
         * separate wrong answer before they were solved together.
         *
         * It descends. A span whose lower end stands higher than its upper one
         * is a pond in a river, and the whole point of a watercourse is that
         * it goes somewhere.
         *
         * It stays under the ground beside it, *along the whole span* and not
         * only at the stations. The surface is a straight line between
         * stations and the simplification puts them as much as a kilometre
         * apart, so a chord drawn between two contained stations can still
         * stand over a dip between them — and because the footprint has a hard
         * outer edge, water standing over ground at that edge is a wall of it
         * with the country falling away behind. So every span is sectioned at
         * seven points along it and both its ends are lowered until the chord
         * clears all seven.
         *
         * And it ends where it meets something. The sea is FLY_WATER_LEVEL and
         * a lake is its own spill point; the mouth cell's own ground is the
         * *bed* under that, tens of metres down, and taking it would carve the
         * last kilometre into the shelf.
         *
         * Lowering, never raising, is what makes the first two compose: a
         * stage dropped is a stage its banks still hold. Only the mouth is
         * raised, and only the estuary behind it has to make room. */
        nk = 0;
        for (k = 0; k < np; ++k)
            if (keep[k]) { kidx[nk] = k; zs[nk] = floor_z[k]; ++nk; }
        /* The floor under each span, at the seven points along it the stations
           themselves are not: the ground between two stations is what a chord
           between them has to clear, and after the simplification they can be
           a kilometre apart. Read once — the ground does not move — and then
           the profile is settled against it. */
        for (i = 0; i + 1 < nk; ++i) {
            fly_v2 a = line[kidx[i]], b = line[kidx[i + 1]];
            for (k = 0; k < FLY_RIVER_SUBS; ++k) {
                float t = (float)(k + 1) / (float)(FLY_RIVER_SUBS + 1);
                fly_v2 m = fly_v2mk(fly_lerpf(a.x, b.x, t), fly_lerpf(a.y, b.y, t));
                /* The wider of the two ends, because the footprint drawn over
                   this span is as wide as its widest end and a section
                   measured to the narrow one misses the edge the water
                   actually stops at. */
                float wa = channel_wide(lacc[kidx[i]], R.cell * R.cell);
                float wb = channel_wide(lacc[kidx[i + 1]], R.cell * R.cell);
                float wd = wa > wb ? wa : wb;
                spanf[i * FLY_RIVER_SUBS + k] =
                    channel_floor(w, a, b, m, wd, fly_river_batter(wd));
            }
        }
        for (pass = 0; pass < 8; ++pass) {
            for (i = 0; i + 1 < nk; ++i) {
                float worst = 0.0f;
                for (k = 0; k < FLY_RIVER_SUBS; ++k) {
                    float t = (float)(k + 1) / (float)(FLY_RIVER_SUBS + 1);
                    float over = fly_lerpf(zs[i], zs[i + 1], t) - spanf[i * FLY_RIVER_SUBS + k];
                    if (over > worst) worst = over;
                }
                /* the chord drops by exactly this at every point along it */
                zs[i] -= worst;
                zs[i + 1] -= worst;
            }
            prev = 1e30f;
            for (i = 0; i < nk; ++i) {
                if (zs[i] > prev - 0.05f) zs[i] = prev - 0.05f;
                prev = zs[i];
            }
        }

        /* How much earth this asks for. A channel is a valley, not a gorge:
           where the line ended up on a shelf or against a cliff the section's
           floor is a hundred metres under it, and what comes out is a slot cut
           through the countryside rather than a river in it. There are more
           candidate sources than budget, so the answer is to survey a
           different one. */
        {
            int deep = 0;
            for (i = 0; i < nk; ++i)
                if (gz_c[kidx[i]] - zs[i] > cutmax) deep = 1;
            if (deep) continue;
        }

        rv.first = w->nriver_pt;
        rv.count = 0;
        rv.lake = el;
        rv.x0 = rv.y0 = 1e30f;
        rv.x1 = rv.y1 = -1e30f;
        for (i = 0; i < nk; ++i) {
            fly_river_pt *pt = &w->river_pt[w->nriver_pt];
            k = kidx[i];
            pt->x = line[k].x;
            pt->y = line[k].y;
            pt->wide = channel_wide(lacc[k], R.cell * R.cell);
            pt->z = zs[i];
            if (pt->x - pt->wide < rv.x0) rv.x0 = pt->x - pt->wide;
            if (pt->y - pt->wide < rv.y0) rv.y0 = pt->y - pt->wide;
            if (pt->x + pt->wide > rv.x1) rv.x1 = pt->x + pt->wide;
            if (pt->y + pt->wide > rv.y1) rv.y1 = pt->y + pt->wide;
            ++w->nriver_pt;
            ++rv.count;
        }
        prev = el >= 0 ? w->lake[el].z : FLY_WATER_LEVEL;
        w->river_pt[rv.first + rv.count - 1].z = prev;
        for (k = rv.first + rv.count - 2; k >= rv.first; --k) {
            if (w->river_pt[k].z >= prev + 0.05f) break;
            w->river_pt[k].z = prev + 0.05f;
            prev = w->river_pt[k].z;
        }
        rv.bed = fly_clampf(1.2f + 0.11f * w->river_pt[rv.first + rv.count - 1].wide,
                            1.8f, 4.5f);
        rv.x0 -= rv.batter; rv.y0 -= rv.batter;
        rv.x1 += rv.batter; rv.y1 += rv.batter;
        w->river[w->nriver++] = rv;
        for (k = 0; k < n; ++k) R.claim[path[k]] = (uint16_t)(w->nriver - 1);
    }
    }
}
