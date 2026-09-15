#include "fly_rail.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- routing ----------------
 *
 * The line used to be drawn straight from one site to the other and put on
 * stilts wherever it crossed water, which meant a route between two coastal
 * sites spent kilometres out at sea on a viaduct nobody would build. A real
 * line follows the land: it takes a longer way round a bay when the land is
 * there, and crosses only where crossing is genuinely shorter than going
 * around — an island terminus, a strait.
 *
 * That is a shortest-path problem with water priced higher than land, so it is
 * solved as one: A* over a coarse grid spanning the two sites, eight-connected,
 * with each step costing its length times a multiplier that is 1 over dry
 * ground and FLY_RAIL_WET_COST over wet. The heuristic is plain euclidean
 * distance, which never overestimates because the cheapest possible multiplier
 * is 1, so the result is optimal for the grid. The path is then simplified,
 * smoothed and resampled at the spacing the rest of the module expects.
 *
 * The grid is coarse on purpose. It only has to decide which side of a bay to
 * pass; the resampling and smoothing below give the line its final shape, and
 * a finer grid would cost memory and time to answer a question the terrain
 * does not pose. If anything fails — the sites are close, the search runs out
 * of room, no path exists — the straight line is still there as the fallback,
 * which is exactly what the route used to be. */

#define FLY_RAIL_GRID 96                                  /* cells per side */
#define FLY_RAIL_CELLS (FLY_RAIL_GRID * FLY_RAIL_GRID)
#define FLY_RAIL_OPEN_MAX 16384                           /* priority-queue cap */
#define FLY_RAIL_WET_COST 5.0f  /* how much longer a dry detour may be */
/* And the same number at sea, where it is not a price but a refusal. A road
 * fords a creek because a causeway is a thing people build; nothing sails over
 * a headland, so the multiplier is set where no detour the grid can offer is
 * ever worse than crossing one. It stays finite rather than becoming a wall so
 * that the search always has an answer to give — a quay whose water the grid
 * resolves as a single cell still gets a lane out of it, which a hard veto
 * would turn into no lane at all. */
#define FLY_LANE_LAND_COST 60.0f
/* How far from the shore the line prefers to sit, in cells, and how much it
 * will pay for it. Kept well under the water multiplier: this bends a route
 * inland where inland is cheap, it does not send it round a continent. */
#define FLY_RAIL_SHORE_CELLS 3
#define FLY_RAIL_SHORE_TOLL 0.9f
/* Minimum curve radius the finished line is relaxed to. A 120 m span turning
 * through 1.7 degrees is 4 km; that is the shape of a line laid across open
 * country rather than one that took a corner. */
#define FLY_RAIL_MIN_RADIUS 4000.0f
/* --- what the ridden line is shaped like --------------------------------
 *
 * The route the player rides had neither of these, and both were the same
 * mistake: it was laid as the shortest thing the search could find and then
 * held to a radius no curve could reach, so it came out as a straight line
 * between two settlements. Twenty kilometres of dead-straight pipe reads as a
 * ruler laid on the map — and from the one seat that looks straight down it
 * for the whole journey, a line with nothing ahead but its own vanishing point
 * is the least interesting thing a ride can be.
 *
 * So the line wanders, the way the roads do and for the same reasons (see
 * fly_rail_shape's `meander`): a broad swing that opens a view along the
 * outside of every curve, shows the rider the country the line is crossing,
 * and puts the pipe itself in shot ahead instead of under the nose.
 *
 * The radius is what buys it. Four kilometres is the bound of a line that
 * cannot turn at all; fifteen hundred is a real curve and still a gentle one
 * for what runs here — at the route's 38 m/s that is 0.96 m/s² of lateral,
 * about a tenth of gravity, which is a curve a seated passenger notices and
 * does not brace against.
 *
 * The amplitude follows from the radius rather than being chosen beside it.
 * route_meander trims a swing to whatever the curve bound will actually hold,
 * and at fifteen hundred metres over the wavelength a route this long wanders
 * on that is about a hundred metres — so the number below is a ceiling that
 * never binds, and the radius is the decision. It is written down anyway, and
 * generously, so that a swing cannot run away if the radius is ever loosened:
 * an amplitude with no ceiling at all is a road that has been given permission
 * to become a spiral. */
#define FLY_RAIL_ROUTE_RADIUS 1500.0f
#define FLY_RAIL_ROUTE_MEANDER 160.0f
#define FLY_RAIL_NONE 0xFFFFu

/* A*'s working set. Static because it is a few hundred kilobytes that would
 * otherwise be a stack overflow, and route generation is not reentrant. */
static struct {
    float g[FLY_RAIL_CELLS];
    float h[FLY_RAIL_CELLS];       /* ground height at each cell */
    uint16_t parent[FLY_RAIL_CELLS];
    uint8_t closed[FLY_RAIL_CELLS];
    uint8_t wet[FLY_RAIL_CELLS];
    uint8_t shore[FLY_RAIL_CELLS]; /* cells from water, saturating at 255 */
    struct { float f; uint16_t cell; } open[FLY_RAIL_OPEN_MAX];
    int nopen;
} A;

/* --- the corridor field ----------------------------------------------------
 *
 * How unwelcome each patch of ground is because of what is already built on
 * it: 1 on the centreline of an existing line, falling to 0 at `reach`, and the
 * most of any line where several run together. See fly_rail_shape's `avoid`.
 *
 * A field rather than a distance query, because two stages want the answer and
 * one of them wants it half a million times. The search asks it once per step
 * and could afford to walk the lines; the meander asks it five times for every
 * station of every road in the network against every line already laid, which
 * is the difference between a survey and a load screen. Rasterising it once per
 * line laid answers both in a lookup.
 *
 * Finer than the search grid on purpose: the plan only has to decide which side
 * of an existing line to pass, but the wander that comes after it swings three
 * hundred metres and has to know whether it is swinging into something.
 *
 * Stamped point by point rather than segment by segment, at whatever stride
 * keeps consecutive stamps inside half the reach — the lines are sampled every
 * sixty to seventy-five metres against a corridor a few hundred wide, so a disc
 * at each stamped point covers the run between them with room to spare and
 * costs a fraction of the segment arithmetic. */
#define FLY_RAIL_KEEP_GRID 192

static struct {
    float ox, oy, cell;
    int gw;
    float v[FLY_RAIL_KEEP_GRID * FLY_RAIL_KEEP_GRID];
} K;

static void keep_clear(void) { K.gw = 0; }

static void keep_build(const fly_rail_avoid *a, float cx, float cy, float half) {
    int l, i, gw = FLY_RAIL_KEEP_GRID;
    keep_clear();
    if (!a || !a->line || a->count <= 0 || a->reach <= 1.0f || a->toll <= 0.0f) return;
    if (half <= 1.0f) return;
    K.gw = gw;
    K.cell = 2.0f * half / (float)(gw - 1);
    K.ox = cx - half;
    K.oy = cy - half;
    memset(K.v, 0, sizeof K.v);
    for (l = 0; l < a->count && l < FLY_RAIL_AVOID_MAX; ++l) {
        const fly_rail_line *ln = &a->line[l];
        int stride = 1;
        if (!ln->pos || ln->count < 2) continue;
        /* the stride that keeps the stamps overlapping, from the line's own
           mean spacing rather than from an assumption about it */
        {   float run = 0.0f, pitch;
            for (i = 1; i < ln->count; ++i)
                run += hypotf(ln->pos[i].x - ln->pos[i - 1].x,
                              ln->pos[i].y - ln->pos[i - 1].y);
            pitch = run / (float)(ln->count - 1);
            if (pitch > 0.1f) stride = (int)(a->reach * 0.5f / pitch);
            if (stride < 1) stride = 1;
        }
        for (i = 0; i < ln->count; i += stride) {
            float w = ln->weight ? fly_clampf(ln->weight[i], 0.0f, 1.0f) : 1.0f;
            float px = ln->pos[i].x, py = ln->pos[i].y;
            int i0, i1, j0, j1, gx, gy;
            if (w <= 0.0f) continue;
            i0 = (int)floorf((px - a->reach - K.ox) / K.cell);
            i1 = (int)ceilf((px + a->reach - K.ox) / K.cell);
            j0 = (int)floorf((py - a->reach - K.oy) / K.cell);
            j1 = (int)ceilf((py + a->reach - K.oy) / K.cell);
            if (i0 < 0) i0 = 0;
            if (j0 < 0) j0 = 0;
            if (i1 > gw - 1) i1 = gw - 1;
            if (j1 > gw - 1) j1 = gw - 1;
            for (gy = j0; gy <= j1; ++gy)
                for (gx = i0; gx <= i1; ++gx) {
                    float x = K.ox + (float)gx * K.cell, y = K.oy + (float)gy * K.cell;
                    float d = hypotf(x - px, y - py), t;
                    if (d >= a->reach) continue;
                    t = w * (1.0f - d / a->reach);
                    if (t > K.v[gy * gw + gx]) K.v[gy * gw + gx] = t;
                }
        }
    }
}

/* What the field says here, bilinearly; 0 anywhere it was never built. */
static float keep_at(float x, float y) {
    float fx, fy, tx, ty, a, b;
    int i, j;
    if (K.gw <= 0) return 0.0f;
    fx = (x - K.ox) / K.cell;
    fy = (y - K.oy) / K.cell;
    if (fx < 0.0f || fy < 0.0f || fx > (float)(K.gw - 1) || fy > (float)(K.gw - 1)) return 0.0f;
    i = (int)fx;
    j = (int)fy;
    if (i > K.gw - 2) i = K.gw - 2;
    if (j > K.gw - 2) j = K.gw - 2;
    tx = fx - (float)i;
    ty = fy - (float)j;
    a = fly_lerpf(K.v[j * K.gw + i], K.v[j * K.gw + i + 1], tx);
    b = fly_lerpf(K.v[(j + 1) * K.gw + i], K.v[(j + 1) * K.gw + i + 1], tx);
    return fly_lerpf(a, b, ty);
}

/* surcharge for a cell this close to the edge of the medium, 0 once
 * comfortably inside it: inland for a line on land, and an offing for a lane
 * at sea, which is the same rule and the same reason. A line that merely stays
 * out of the water hugs every headland and runs along the beach; a lane that
 * merely stays afloat scrapes past every rock. */
static float shore_toll(int cells) {
    if (cells >= FLY_RAIL_SHORE_CELLS) return 0.0f;
    return FLY_RAIL_SHORE_TOLL * (1.0f - (float)cells / (float)FLY_RAIL_SHORE_CELLS);
}

/* --- the medium a line is laid in ------------------------------------------
 *
 * Everything below this point is written once and read two ways round. A line
 * across the land wants dry ground and pays to cross water; a lane at sea
 * wants water deep enough to float a hull and may not cross land at all.
 * Those are the same question with the sign turned over, so they are the same
 * code with one predicate under it: `medium_barred` is the ground this line
 * may not be laid on, and every rule after it — what a step costs, how far the
 * standoff reaches, what the smoothing may not undo, what the curvature
 * relaxation may not start — asks that and nothing else.
 *
 * Writing the lane as its own search instead would have been a third copy of
 * the same A*, the same chamfer, the same simplify-smooth-spline, and the same
 * bugs to find again — see fly_line.h, which is the same argument about the
 * other half of a line. */
typedef struct {
    const fly_world *w;
    int sea;
    float draught;
} fly_medium;

static int medium_barred(const fly_medium *m, float x, float y) {
    /* Wet, not "below sea level": a river is water a line on land has to be
       carried over the same way an arm of the sea is, and it is the case the
       standoff and the piers were written for. */
    if (!m->sea) return fly_world_wet(m->w, x, y);
    /* And at sea, navigable rather than wet, which is not the same question:
       a river's channel and a lake's disc are wet ground standing above the
       sea, and no hull is going up either. So it is asked of the bed, and it
       is asked with the keel's own clearance under it. */
    return fly_world_ground(m->w, x, y) > FLY_WATER_LEVEL - m->draught;
}

static void heap_push(float f, uint16_t cell) {
    int i = A.nopen++;
    if (i >= FLY_RAIL_OPEN_MAX) { A.nopen = FLY_RAIL_OPEN_MAX; return; }
    A.open[i].f = f;
    A.open[i].cell = cell;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (A.open[p].f <= A.open[i].f) break;
        { float tf = A.open[p].f; uint16_t tc = A.open[p].cell;
          A.open[p] = A.open[i];
          A.open[i].f = tf; A.open[i].cell = tc; }
        i = p;
    }
}

static uint16_t heap_pop(void) {
    uint16_t top = A.open[0].cell;
    int i = 0;
    A.open[0] = A.open[--A.nopen];
    for (;;) {
        int l = i * 2 + 1, r = l + 1, m = i;
        if (l < A.nopen && A.open[l].f < A.open[m].f) m = l;
        if (r < A.nopen && A.open[r].f < A.open[m].f) m = r;
        if (m == i) break;
        { float tf = A.open[m].f; uint16_t tc = A.open[m].cell;
          A.open[m] = A.open[i];
          A.open[i].f = tf; A.open[i].cell = tc; }
        i = m;
    }
    return top;
}

/* Search the grid from `a` to `b`, writing the path as world points into `out`.
 * Returns the number of points, or 0 when the caller should fall back to a
 * straight line. `ox`/`oy`/`cell` describe the grid's placement in the world and
 * `gw` how many cells it is across, up to the storage's FLY_RAIL_GRID.
 *
 * The resolution is the caller's because the cost is almost all in filling the
 * grid — one terrain sample per cell, and the terrain is the most expensive
 * thing in the engine to ask. A rail is laid once and wants the fine answer; a
 * road network is two dozen lines laid at startup and wants the cheap one, and
 * the difference is only ever which side of a bay the line passes, since the
 * spline and the relaxation below give it its shape either way. */
static int route_search(const fly_medium *m, fly_v2 a, fly_v2 b, int gw, float climb,
                        float keep_toll, float ox, float oy, float cell,
                        fly_v2 *out, int max) {
    int ai, aj, bi, bj, start, goal, n = 0, i, cells;
    if (cell <= 0.0f || gw < 8 || gw > FLY_RAIL_GRID) return 0;
    cells = gw * gw;
    ai = (int)((a.x - ox) / cell + 0.5f);
    aj = (int)((a.y - oy) / cell + 0.5f);
    bi = (int)((b.x - ox) / cell + 0.5f);
    bj = (int)((b.y - oy) / cell + 0.5f);
    if (ai < 0 || aj < 0 || bi < 0 || bj < 0) return 0;
    if (ai >= gw || aj >= gw) return 0;
    if (bi >= gw || bj >= gw) return 0;
    start = aj * gw + ai;
    goal = bj * gw + bi;
    if (start == goal) return 0;

    for (i = 0; i < cells; ++i) {
        int cx = i % gw, cy = i / gw;
        float x = ox + (float)cx * cell, y = oy + (float)cy * cell;
        A.g[i] = FLT_MAX;
        /* The ground height, and only where the climb toll will read it. It is
           a terrain sample per cell — the most expensive thing in the engine,
           and half the cost of filling this grid — and a line that does not pay
           for rise never looks at it: a pipe on piers does not, and neither
           does a lane, since the bed under a hull is not something she is
           going over. */
        A.h[i] = climb > 0.0f ? fly_world_ground(m->w, x, y) : 0.0f;
        A.parent[i] = FLY_RAIL_NONE;
        A.closed[i] = 0;
        /* The ground this line may not be laid on: water for a line on land,
           and land for a lane at sea. See medium_barred. */
        A.wet[i] = (uint8_t)medium_barred(m, x, y);
        A.shore[i] = A.wet[i] ? 0 : 255;
    }
    /* Distance from the edge of the medium, in cells, by two chamfer sweeps.
     * Avoiding the edge is not the same as keeping away from it: a line that
     * merely stays dry hugs every headland and runs along the beach, and a
     * lane that merely stays afloat does the same thing from the other side.
     * Pricing the first few cells in turns "not in the sea" into "a sensible
     * distance from it" for the cost of two passes. */
    for (i = 0; i < cells; ++i) {
        int cx = i % gw, cy = i / gw, best = A.shore[i];
        if (cx > 0 && A.shore[i - 1] + 1 < best) best = A.shore[i - 1] + 1;
        if (cy > 0 && A.shore[i - gw] + 1 < best) best = A.shore[i - gw] + 1;
        A.shore[i] = (uint8_t)best;
    }
    for (i = cells - 1; i >= 0; --i) {
        int cx = i % gw, cy = i / gw, best = A.shore[i];
        if (cx + 1 < gw && A.shore[i + 1] + 1 < best) best = A.shore[i + 1] + 1;
        if (cy + 1 < gw && A.shore[i + gw] + 1 < best)
            best = A.shore[i + gw] + 1;
        A.shore[i] = (uint8_t)best;
    }
    /* the endpoints are pads on dry land by construction; never let a stray
       sample at the terminus price the whole route as a sea crossing */
    A.wet[start] = A.wet[goal] = 0;
    A.nopen = 0;
    A.g[start] = 0.0f;
    heap_push(0.0f, (uint16_t)start);

    while (A.nopen > 0) {
        uint16_t cur = heap_pop();
        int cx = cur % gw, cy = cur / gw, d;
        if (A.closed[cur]) continue;
        A.closed[cur] = 1;
        if (cur == goal) break;
        for (d = 0; d < 8; ++d) {
            static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
            static const int dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            int nx = cx + dx[d], ny = cy + dy[d], ni;
            float step, cost, gn, h;
            if (nx < 0 || ny < 0 || nx >= gw || ny >= gw) continue;
            ni = ny * gw + nx;
            if (A.closed[ni]) continue;
            step = (dx[d] && dy[d]) ? cell * 1.41421356f : cell;
            /* price the step by where it lands; a diagonal that clips a corner
               of water is close enough to a wet step to be charged as one */
            cost = step * (A.wet[ni] || A.wet[cur]
                           ? (m->sea ? FLY_LANE_LAND_COST : FLY_RAIL_WET_COST)
                           : 1.0f + shore_toll(A.shore[ni]));
            /* Climb, priced in metres of route per metre of rise. A line that
             * only avoids water still walks straight over whatever is in the
             * way, and on a heightfield that means the occasional span pitched
             * at sixty degrees — which is not a road, it is a cliff with a road
             * drawn on it. Paying for the rise sends it round the shoulder of
             * the hill instead, which is where anyone would put it. Zero leaves
             * the cost exactly as it was, which is what the rail wants. */
            if (climb > 0.0f) cost += climb * fabsf(A.h[ni] - A.h[cur]);
            /* And what is already built here. A step that lands in somebody
             * else's corridor pays for its own length again and then some,
             * which is what makes running alongside an existing line expensive
             * and crossing one merely a price — see fly_rail_shape's `avoid`.
             * It is a surcharge on the step rather than a barrier, so the
             * heuristic below is still an underestimate and the search is still
             * optimal for the grid. */
            if (keep_toll > 0.0f) {
                float k = keep_at(ox + (float)nx * cell, oy + (float)ny * cell);
                if (k > 0.0f) cost += step * keep_toll * k;
            }
            gn = A.g[cur] + cost;
            if (gn >= A.g[ni]) continue;
            A.g[ni] = gn;
            A.parent[ni] = cur;
            h = sqrtf((float)((nx - bi) * (nx - bi) + (ny - bj) * (ny - bj))) * cell;
            if (A.nopen >= FLY_RAIL_OPEN_MAX) return 0; /* out of room: fall back */
            heap_push(gn + h, (uint16_t)ni);
        }
    }
    if (!A.closed[goal]) return 0;

    /* walk the chain back, then reverse it into world space */
    { int chain[FLY_RAIL_CELLS > 4096 ? 4096 : FLY_RAIL_CELLS];
      int c = goal, len = 0;
      while (c != start && len < (int)(sizeof chain / sizeof chain[0])) {
          chain[len++] = c;
          if (A.parent[c] == FLY_RAIL_NONE) return 0;
          c = A.parent[c];
      }
      if (c != start) return 0;
      if (len + 2 > max) return 0;
      out[n++] = a;
      for (i = len - 1; i >= 0; --i) {
          int cx = chain[i] % gw, cy = chain[i] / gw;
          out[n++] = fly_v2mk(ox + (float)cx * cell, oy + (float)cy * cell);
      }
      out[n - 1] = b; /* the last cell is the goal; pin it to the terminus */
    }
    return n;
}

/* Drop points that sit close to the straight line between their neighbours.
 * The grid answers in cell-sized staircases, and a run of them across open
 * ground is one straight leg. */
static int route_simplify(fly_v2 *p, int n, float tol) {
    int i, k = 1;
    if (n < 3) return n;
    for (i = 1; i < n - 1; ++i) {
        fly_v2 a = p[k - 1], b = p[i], c = p[i + 1];
        float ex = c.x - a.x, ey = c.y - a.y;
        float len = sqrtf(ex * ex + ey * ey);
        float dev = len > 1e-3f ? fabsf((b.x - a.x) * ey - (b.y - a.y) * ex) / len : 0.0f;
        if (dev > tol) p[k++] = b;
    }
    p[k++] = p[n - 1];
    return k;
}

/* Round off the corners the grid left, but never round a leg out onto ground
 * it was routed around — the water a line on land avoided, the land a lane at
 * sea did. That would undo the whole search one metre at a time. */
static void route_smooth(const fly_medium *m, fly_v2 *p, int n, int passes) {
    int pass, i;
    for (pass = 0; pass < passes; ++pass)
        for (i = 1; i < n - 1; ++i) {
            fly_v2 q = fly_v2mk((p[i - 1].x + 2.0f * p[i].x + p[i + 1].x) * 0.25f,
                                (p[i - 1].y + 2.0f * p[i].y + p[i + 1].y) * 0.25f);
            if (!medium_barred(m, p[i].x, p[i].y) && medium_barred(m, q.x, q.y)) continue;
            p[i] = q;
        }
}

/* Centripetal Catmull-Rom through the simplified path.
 *
 * Resampling the polyline linearly keeps every corner the grid left as a
 * crease, and a rail line does not have creases — it has curves, because that
 * is what a vehicle can take and what anyone laying one would build. The
 * spline passes through its control points, so the route still goes exactly
 * where the search decided it should.
 *
 * Centripetal (knots spaced by the square root of chord length) rather than
 * the uniform form, and not as a refinement: the control points here are
 * wildly uneven — a 130 m straight into a terminus followed by a 19 km run
 * across open ground — and uniform Catmull-Rom overshoots hard on spacing like
 * that. It sent the first span backwards past the pad and then 300 m forward
 * to catch up, a 170-degree turn in a line whose next worst was four. */
static fly_v2 route_spline(const fly_v2 *p, int n, int seg, float t) {
    fly_v2 q[4];
    float k[4], u, w0, w1;
    fly_v2 a1, a2, a3, b1, b2;
    int i;
    q[0] = p[seg > 0 ? seg - 1 : 0];
    q[1] = p[seg];
    q[2] = p[seg + 1 < n ? seg + 1 : n - 1];
    q[3] = p[seg + 2 < n ? seg + 2 : n - 1];
    k[0] = 0.0f;
    for (i = 1; i < 4; ++i) {
        float dx = q[i].x - q[i - 1].x, dy = q[i].y - q[i - 1].y;
        float d = sqrtf(sqrtf(dx * dx + dy * dy));
        k[i] = k[i - 1] + (d > 1e-4f ? d : 1e-4f);
    }
    u = k[1] + (k[2] - k[1]) * fly_clampf(t, 0.0f, 1.0f);
#define RL(A, B, KA, KB) (w1 = ((KB) - u) / ((KB) - (KA)), w0 = 1.0f - w1, \
                          fly_v2mk((A).x * w1 + (B).x * w0, (A).y * w1 + (B).y * w0))
    a1 = RL(q[0], q[1], k[0], k[1]);
    a2 = RL(q[1], q[2], k[1], k[2]);
    a3 = RL(q[2], q[3], k[2], k[3]);
    b1 = RL(a1, a2, k[0], k[2]);
    b2 = RL(a2, a3, k[1], k[3]);
    return RL(b1, b2, k[1], k[2]);
#undef RL
}

/* --- the wander -----------------------------------------------------------
 *
 * The search draws the shortest line that stays dry and does not climb, and
 * across open country that is a straight one. It is a correct answer to the
 * question it was asked and it is not what a road looks like: a hauling route
 * between two settlements swings — round the shoulder of a rise, along the
 * inside of a valley, out to a crossing point and back — and the reason a real
 * one does that is a hundred local decisions no procedural survey is going to
 * make. What can be reproduced is the shape those decisions leave behind.
 *
 * So: a smooth lateral swing about the surveyed line, three octaves of it, with
 * wavelengths long enough that every crest of it is an arc a vehicle takes at
 * speed rather than a corner it brakes for. It is seeded off the two termini,
 * so a given pair of settlements is joined by the same wandering road in every
 * run of the same world, and it is applied before the curvature bound so that
 * anything it asks for that is too tight for the road gets straightened back
 * out by the relaxation rather than shipped.
 *
 * Two things veto a swing, and both are about where it would put the line
 * rather than about how far it moved:
 *
 *   Water. The search spent its whole budget deciding which side of the bay to
 *   pass, and a wander that walks back into the bay throws that away. A point
 *   already wet may move — that is a crossing being lengthened, which is fine
 *   — but a dry one may not become wet.
 *
 *   Height. The line was routed along the low ground for a reason. A swing
 *   that climbs the valley side to get its amplitude is a road that goes over
 *   the hill it was routed around, and it shows up as gradient. So a candidate
 *   standing more than `RISE` above the ground the survey chose is refused.
 *
 * A refused swing is halved and offered again, up to four times, rather than
 * dropped: what the line gets is the largest wander that is still on ground the
 * search picked out. Dropping it outright would put a notch in a smooth curve,
 * and a notch is the one thing the whole exercise is trying to avoid. */
#define FLY_RAIL_MEANDER_RISE 22.0f

/* How much a swing wanders, and how tight an arc it costs, are the same two
 * numbers read twice. For a swing of amplitude A and wavelength L:
 *
 *     extra distance travelled ~ (pi A / L)^2       — how winding it is
 *     tightest radius          ~ L^2 / (4 pi^2 A)   — what a vehicle must take
 *
 * so windiness depends on the *ratio* A/L and the radius on the scale. Hold the
 * ratio and stretch both, and you get exactly the same wandering line at a
 * gentler radius. That is the whole tuning, and getting it backwards is what
 * the first version of this did: it scaled the long octave's wavelength with
 * the length of the road, so a twenty-kilometre link got a nine-kilometre swing
 * — which at 220 m of amplitude is a road that deviates by one part in a
 * hundred and is, to look at, straight.
 *
 * So the wavelengths are absolute. Two and a half kilometres is the scale a
 * road wanders on whether it is going four kilometres or twenty, and the two
 * shorter octaves are fixed fractions of it so the whole shape scales as one
 * thing. The only concession to a short link is the cap on the long octave: a
 * three-kilometre road gets one sweep rather than a piece of one.
 *
 * The shorter octaves are small, and it is the same arithmetic that makes them
 * small. Curvature is 4 pi^2 A / L^2, so an octave at a third of the wavelength
 * costs nine times the radius for the same amplitude, and the three add: the
 * first cut of this ran them at a fifth and a twentieth of the swing and the
 * three together came to a two-hundred-metre arc, which the curvature bound
 * then spent twenty thousand passes flattening — every octave, including the
 * one that was doing the work. What came out was a road that wandered by three
 * percent while claiming eleven. Detail an octave down has to be paid for in
 * radius somewhere, and there is no radius spare. */
static float meander_at(float s, float run, float lam0, float amp, const float *phase) {
    float sw =   sinf(2.0f * FLY_PI * s / lam0            + phase[0]);
    sw += 0.10f * sinf(2.0f * FLY_PI * s / (lam0 * 0.62f) + phase[1]);
    sw += 0.03f * sinf(2.0f * FLY_PI * s / (lam0 * 0.35f) + phase[2]);
    /* Held off both ends. The termini are the settlements' own yards and the
     * approach to them is the line's own tangent — see route_trim, which cuts
     * the finished curve at the clearance circle and relies on the last stretch
     * being where the survey put it. What a swing in the run-in costs is the
     * one thing the settlement layout cannot absorb: the road arrives across
     * the gate rather than down it.
     *
     * Held off, not held down — the taper is a few hundred metres and a tenth
     * of the run, not a fifth. A fifth of a five-kilometre link is most of the
     * link, and a road whose middle mile is the only part allowed to wander is
     * a straight road with a kink in it. */
    { float hold = run * 0.12f + 900.0f;
      return sw * amp * fly_smoothstepf(0.0f, 1.0f, fly_clampf(s / hold, 0, 1))
                      * fly_smoothstepf(0.0f, 1.0f, fly_clampf((run - s) / hold, 0, 1)); }
}

static void route_meander(const fly_medium *m, fly_v2 *rp, int n, float amp,
                          float min_radius, uint32_t seed) {
    /* The corridor field is read here as well as by the search, and it has to
     * be: the plan the search drew is clear of what is already built, and a
     * three-hundred-metre swing laid on top of it that took no notice would put
     * the line straight back over the rail it was routed round. */
    static float chain[FLY_RAIL_POINT_MAX];
    static fly_v2 src[FLY_RAIL_POINT_MAX];
    float phase[3], run, lam0, fits;
    int i, k;
    if (n < 8 || amp <= 0.0f || n > FLY_RAIL_POINT_MAX) return;
    chain[0] = 0.0f;
    for (i = 1; i < n; ++i)
        chain[i] = chain[i - 1] + hypotf(rp[i].x - rp[i - 1].x, rp[i].y - rp[i - 1].y);
    run = chain[n - 1];
    if (run < 1200.0f) return;   /* a link this short is a yard road; leave it */
    lam0 = run / 2.2f;
    if (lam0 < 1800.0f) lam0 = 1800.0f;
    if (lam0 > 2900.0f) lam0 = 2900.0f;
    /* --- what the curve bound will actually let through ---
     *
     * A swing of amplitude A at wavelength L turns at 4 pi^2 A / L^2, so asking
     * for an amplitude without reference to the wavelength is asking for a
     * radius by accident. On a long link, where L is capped, `amp` is a real
     * request; on a short one, where L comes down with the run, the same `amp`
     * is a hairpin — and what happens to a hairpin is that the relaxation
     * downstream spends twenty thousand passes taking it back out, along with
     * every other swing on the road. Measured: at a 500 m bound, roads asked
     * for eleven percent of extra distance and were left with four.
     *
     * So the amplitude is whatever fits the radius the caller is going to hold
     * the line to, and `amp` is the ceiling rather than the request. A third
     * over the bound, because the wander is laid on top of a route that has
     * curvature of its own and the two add. */
    fits = lam0 * lam0 / (4.0f * FLY_PI * FLY_PI * (min_radius > 1.0f ? min_radius : 1.0f)
                          * 1.35f);
    if (amp > fits) amp = fits;
    /* And a swing of a fifth of the whole run is not a road, it is a spiral. */
    if (amp > run * 0.060f) amp = run * 0.060f;
    for (k = 0; k < 3; ++k)
        phase[k] = (float)(fly_hash2(seed, k, 0) & 0xFFFFu) / 65536.0f * 2.0f * FLY_PI;
    /* Every point is displaced along the normal of the line *as the search left
     * it*, which is why the line is copied first. Taking the normal from the
     * live array instead reads a neighbour that has already moved, and at these
     * amplitudes that is not a small error: a station is sixty metres from its
     * neighbour and the swing is three hundred, so the vector between them
     * stops pointing along the road and starts pointing across it, and each
     * point is then offset in a direction with almost no relation to the line.
     * What came out was a 17 km road laid as 31 km of switchbacks, which the
     * curvature relaxation downstream then spent twenty thousand passes
     * flattening back into something straighter than it started. */
    for (i = 0; i < n; ++i) src[i] = rp[i];
    for (i = 1; i + 1 < n; ++i) {
        float ex = src[i + 1].x - src[i - 1].x, ey = src[i + 1].y - src[i - 1].y;
        float len = hypotf(ex, ey), off, gz0, keep0;
        float nx, ny;
        int wet0;
        if (len < 1e-3f) continue;
        nx = -ey / len;
        ny = ex / len;
        off = meander_at(chain[i], run, lam0, amp, phase);
        gz0 = fly_world_ground(m->w, src[i].x, src[i].y);
        wet0 = medium_barred(m, src[i].x, src[i].y);
        keep0 = keep_at(src[i].x, src[i].y);
        for (k = 0; k < 5; ++k) {
            float cx = src[i].x + nx * off, cy = src[i].y + ny * off;
            float gz = fly_world_ground(m->w, cx, cy);
            /* Never further into somebody else's corridor than the plan already
               was: a station that starts inside one — the run-in to a
               settlement the guideway also serves — may stay where it is, and
               nothing is allowed to wander in. */
            if (keep_at(cx, cy) > keep0 + 1e-3f) { off *= 0.5f; continue; }
            if ((!medium_barred(m, cx, cy) || wet0) &&
                gz - gz0 < FLY_RAIL_MEANDER_RISE) {
                rp[i].x = cx;
                rp[i].y = cy;
                break;
            }
            off *= 0.5f;
        }
    }
}

/* Put the stations back on an even pitch.
 *
 * The wander above moves points sideways, which lengthens the spans on the
 * outside of every swing and shortens them on the inside — the line is still
 * the same line, but the stations along it are no longer the even pitch the
 * resample produced. That matters to exactly one thing and it matters a lot:
 * the curvature bound below reads a radius as `(la + lb) / 2 / turn`, so a
 * station whose two spans came out half again as long as the pitch is allowed
 * half again the turn, and the worst crease in the network is always one of
 * those. Re-spacing first is what makes "nothing turns tighter than
 * `min_radius`" also mean "no span turns by more than pitch over radius",
 * which is the form the claim is actually checked in.
 *
 * Piecewise-linear, because by this point the line is sampled far more finely
 * than any curve on it: the chord it walks and the arc it is sampling agree to
 * a couple of centimetres, and the relaxation immediately afterwards is working
 * at metres. */
static int route_respace(fly_v2 *rp, int n, float pitch, int max) {
    static fly_v2 src[FLY_RAIL_POINT_MAX];
    float total = 0.0f, step, walked = 0.0f, seg_len;
    int i, seg = 0, count;
    if (n < 3 || pitch <= 1.0f || n > FLY_RAIL_POINT_MAX) return n;
    for (i = 0; i < n; ++i) src[i] = rp[i];
    for (i = 1; i < n; ++i) total += hypotf(src[i].x - src[i - 1].x, src[i].y - src[i - 1].y);
    if (total < pitch * 2.0f) return n;
    count = (int)(total / pitch) + 2;
    if (count > max) count = max;
    if (count > FLY_RAIL_POINT_MAX) count = FLY_RAIL_POINT_MAX;
    if (count < 3) return n;
    step = total / (float)(count - 1);
    seg_len = hypotf(src[1].x - src[0].x, src[1].y - src[0].y);
    for (i = 0; i < count; ++i) {
        float want = (float)i * step, t;
        while (seg < n - 2 && walked + seg_len < want) {
            walked += seg_len;
            ++seg;
            seg_len = hypotf(src[seg + 1].x - src[seg].x, src[seg + 1].y - src[seg].y);
        }
        t = seg_len > 1e-4f ? fly_clampf((want - walked) / seg_len, 0.0f, 1.0f) : 0.0f;
        rp[i] = fly_v2mk(fly_lerpf(src[seg].x, src[seg + 1].x, t),
                         fly_lerpf(src[seg].y, src[seg + 1].y, t));
    }
    rp[0] = src[0];             /* the ends are pinned, here as everywhere */
    rp[count - 1] = src[n - 1];
    return count;
}

/* Relax the sampled line until nothing on it turns more sharply than
 * `min_radius`. See fly_rail.h, where the shape of the solve is written down;
 * the guideway is one of its two callers and the rivers are the other.
 *
 * Only the two ends are pinned: pinning the first *span* as well, to hold a
 * radial approach, gave the difference between the radial and the route's
 * bearing nowhere to go and piled it into one 28-degree span. */
void fly_rail_bend(fly_v2 *rp, int n, float min_radius, int iters,
                   fly_bend_allow allow, void *ctx) {
    int it, i;
    if (n < 6) return;
#define PT(I) (rp[I])
    for (it = 0; it < iters; ++it) {
        int moved = 0;
        for (i = 1; i + 1 < n; ++i) {
            float ax = PT(i).x - PT(i - 1).x, ay = PT(i).y - PT(i - 1).y;
            float bx = PT(i + 1).x - PT(i).x, by = PT(i + 1).y - PT(i).y;
            float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
            float turn, radius, mx, my, pull;
            fly_v2 to;
            if (la < 1e-3f || lb < 1e-3f) continue;
            turn = acosf(fly_clampf((ax * bx + ay * by) / (la * lb), -1.0f, 1.0f));
            radius = turn > 1e-5f ? (la + lb) * 0.5f / turn : 1e30f;
            if (radius >= min_radius) continue;
            mx = (PT(i - 1).x + PT(i + 1).x) * 0.5f;
            my = (PT(i - 1).y + PT(i + 1).y) * 0.5f;
            /* small steps: a large pull overshoots into a kink of the opposite
               sign and the two constraints oscillate instead of settling */
            pull = fly_clampf(1.0f - radius / min_radius, 0.02f, 0.12f);
            to = fly_v2mk(PT(i).x + (mx - PT(i).x) * pull,
                          PT(i).y + (my - PT(i).y) * pull);
            if (allow && !allow(ctx, PT(i - 1), PT(i), to, PT(i + 1))) continue;
            PT(i) = to;
            moved = 1;
        }
        if (!moved) break;
    }
#undef PT
}

/* The guideway's veto on that relaxation.
 *
 * A hard water veto and a hard curvature bound are not always both satisfiable.
 * A line rounding the tip of a headland is pinned by the sea on the inside of
 * the turn and can never reach the bound, which is where a 30-degree span came
 * from — and it sits exactly at a shoreline, where the crossing begins.
 *
 * So the rule is about what the move *is*, not how many are left: lengthening a
 * crossing the route already committed to is allowed, because a bridge a span
 * longer is what a designer would build to take the corner properly; starting a
 * new one is not, because that is how the relaxation straightens the whole line
 * back across the bay it was routed around. A plain budget cannot tell those
 * apart and traded six percent more sea for four degrees. */
static int route_dry_move(void *ctx, fly_v2 prev, fly_v2 from, fly_v2 to, fly_v2 next) {
    const fly_medium *m = (const fly_medium *)ctx;
    return !(!medium_barred(m, from.x, from.y) && medium_barred(m, to.x, to.y) &&
             !medium_barred(m, prev.x, prev.y) && !medium_barred(m, next.x, next.y));
}

static void route_relax(const fly_medium *m, fly_v2 *rp, int n,
                        float min_radius, int iters) {
    fly_rail_bend(rp, n, min_radius, iters, route_dry_move, (void *)m);
}

/* Cut the finished line back to where it crosses each pad's clearance circle,
 * interpolating the exact crossing so the terminus lands on the curve. */
static int route_trim(fly_v2 *rp, int n, fly_v2 sa, fly_v2 sb, float clear) {
    int head = -1, tail = n, i;
    /* last index still inside each pad's circle; the crossing is the span after */
    for (i = 0; i < n; ++i)
        if (hypotf(rp[i].x - sa.x, rp[i].y - sa.y) < clear) head = i;
        else break;
    for (i = n - 1; i >= 0; --i)
        if (hypotf(rp[i].x - sb.x, rp[i].y - sb.y) < clear) tail = i;
        else break;
    if (head < 0) head = 0;
    if (tail >= n) tail = n - 1;
    if (tail - head < 4) return n; /* too short to trim safely */
    if (head + 1 < n) {
        fly_v2 p = rp[head], q = rp[head + 1];
        float lo = 0.0f, hi = 1.0f;
        int k;
        for (k = 0; k < 30; ++k) {
            float t = (lo + hi) * 0.5f;
            if (hypotf(fly_lerpf(p.x, q.x, t) - sa.x, fly_lerpf(p.y, q.y, t) - sa.y) < clear) lo = t;
            else hi = t;
        }
        rp[head].x = fly_lerpf(p.x, q.x, hi);
        rp[head].y = fly_lerpf(p.y, q.y, hi);
    }
    if (tail > 0) {
        fly_v2 p = rp[tail], q = rp[tail - 1];
        float lo = 0.0f, hi = 1.0f;
        int k;
        for (k = 0; k < 30; ++k) {
            float t = (lo + hi) * 0.5f;
            if (hypotf(fly_lerpf(p.x, q.x, t) - sb.x, fly_lerpf(p.y, q.y, t) - sb.y) < clear) lo = t;
            else hi = t;
        }
        rp[tail].x = fly_lerpf(p.x, q.x, hi);
        rp[tail].y = fly_lerpf(p.y, q.y, hi);
    }
    for (i = 0; i <= tail - head; ++i) rp[i] = rp[head + i];
    return tail - head + 1;
}

/* ---------------- laying ---------------- */

int fly_rail_lay(const fly_world *w, fly_wpos from, fly_wpos to,
                 const fly_rail_shape *shape, fly_v2 *out, int max) {
    fly_v2 a, b;
    float dx, dy, planar, clear, spacing, total, step;
    int count, i, np = 0, seg;
    static fly_v2 path[FLY_RAIL_POINT_MAX];
    fly_medium med;

    if (!w || !shape || !out || max < 2) return 0;
    /* Which ground this line may be laid on, settled once and read by every
       stage below it — see the fly_medium note above route_search. */
    med.w = w;
    med.sea = shape->sea != 0;
    med.draught = shape->draught;
    /* The line is built in the plane, out of chart coordinates taken apart here
     * and not put back together as positions until they are handed to the
     * world. That is the right frame for it: a line joins two settlements, both
     * inside the thirty kilometres the chart is flat across to four decimal
     * places, and the grid search, the spline and the arc length all want a
     * plane to work in. Nothing laid between two settlements is ever far enough
     * away to know the world is round. */
    a = fly_v2mk(from.e, from.n);
    b = fly_v2mk(to.e, to.n);
    dx = b.x - a.x;
    dy = b.y - a.y;
    planar = sqrtf(dx * dx + dy * dy);
    clear = shape->clear > 0.0f ? shape->clear : 1.0f;
    spacing = shape->spacing > 1.0f ? shape->spacing : 120.0f;
    if (planar < clear * 2.0f) return 0; /* the two ends are the same place */

    /* Fit the search grid around the pair with room to detour sideways. Half
     * the separation of slack each way is enough for any bay worth going round
     * and keeps the cell size — and so the cost of a wrong turn — small. */
    {
        int gw = shape->grid >= 8 && shape->grid < FLY_RAIL_GRID ? shape->grid : FLY_RAIL_GRID;
        float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
        float half = planar * 0.85f + 600.0f;
        float cell = 2.0f * half / (float)(gw - 1);
        /* What is already on this ground, rasterised over the same patch of
           world the search is about to cross. Built before the search because
           both it and the wander after it read the same field. */
        keep_build(&shape->avoid, cx, cy, half);
        np = route_search(&med, a, b, gw, shape->climb_toll, shape->avoid.toll,
                          cx - half, cy - half, cell, path, FLY_RAIL_POINT_MAX);
        if (np >= 2) {
            np = route_simplify(path, np, cell * 0.35f);
            route_smooth(&med, path, np, 8);
        }
    }
    if (np < 2) { /* no grid answer: the straight line, as before */
        np = 2;
        path[0] = a;
        path[1] = b;
        if (planar > clear * 4.0f) {
            float t0 = clear / planar;
            path[0] = fly_v2mk(fly_lerpf(a.x, b.x, t0), fly_lerpf(a.y, b.y, t0));
            path[1] = fly_v2mk(fly_lerpf(a.x, b.x, 1.0f - t0), fly_lerpf(a.y, b.y, 1.0f - t0));
        }
    }

    /* resample the polyline at the caller's spacing */
    total = 0.0f;
    for (i = 1; i < np; ++i)
        total += sqrtf((path[i].x - path[i - 1].x) * (path[i].x - path[i - 1].x) +
                       (path[i].y - path[i - 1].y) * (path[i].y - path[i - 1].y));
    count = (int)(total / spacing) + 2;
    if (count < 2) count = 2;
    if (count > max) count = max;
    step = total / (float)(count - 1);
    seg = 0;
    {
        float walked = 0.0f, seg_len;
        seg_len = np > 1 ? sqrtf((path[1].x - path[0].x) * (path[1].x - path[0].x) +
                                 (path[1].y - path[0].y) * (path[1].y - path[0].y)) : 0.0f;
        for (i = 0; i < count; ++i) {
            float want = (float)i * step, t;
            fly_v2 q;
            while (seg < np - 2 && walked + seg_len < want) {
                walked += seg_len;
                ++seg;
                seg_len = sqrtf((path[seg + 1].x - path[seg].x) * (path[seg + 1].x - path[seg].x) +
                                (path[seg + 1].y - path[seg].y) * (path[seg + 1].y - path[seg].y));
            }
            t = seg_len > 1e-4f ? fly_clampf((want - walked) / seg_len, 0.0f, 1.0f) : 0.0f;
            q = route_spline(path, np, seg, t);
            /* Pin the ends exactly. Doing it after the loop instead left the
               last point moved but its arc length not, which showed up as a
               stub segment doubling back on itself — a 170-degree turn in a
               line whose next-worst was four. */
            if (i == 0) q = path[0];
            if (i == count - 1) q = path[np - 1];
            out[i] = q;
        }
    }
    /* Let it wander, if whoever is laying it wants a road rather than a
     * guideway. Before the bound below, never after: the relaxation is what
     * turns the swing into arcs a vehicle can take, and a wander applied to an
     * already-bounded line would simply put the corners back. Seeded off the
     * two termini, rounded to the metre, so the same pair of settlements is
     * joined by the same road every time the world is built. */
    if (shape->meander > 0.0f) {
        route_meander(&med, out, count, shape->meander,
                      shape->min_radius > 0.0f ? shape->min_radius : FLY_RAIL_MIN_RADIUS,
                      fly_hash2(0x9e3779b9u, (int32_t)(a.x + a.y), (int32_t)(b.x + b.y)));
        count = route_respace(out, count, step, max);
    }
    /* Bound the curvature. Whoever is laying this drapes it afterwards, which
     * is the only order that works: the line has to follow the ground it
     * actually crosses, and until the relaxation has settled nobody knows which
     * ground that is.
     *
     * Twenty thousand passes rather than four. It costs nothing when the line
     * is already legal — the loop stops the first pass that moves nothing —
     * and a wandering road is emphatically not: the relaxation is a diffusion,
     * each pass takes a hundredth off the worst turn, and four thousand of them
     * left the network's worst crease at fourteen degrees where the bound says
     * nine. That is the difference between a road with arcs in it and a road
     * with corners, and it was being decided by an iteration count. */
    route_relax(&med, out, count, shape->min_radius > 0.0f ? shape->min_radius
                                                           : FLY_RAIL_MIN_RADIUS, 20000);
    /* Trim to the terminus *after* smoothing, not before.
     *
     * Choosing the terminus first and asking the line to reach it puts a corner
     * at the terminus by construction: the point sits on a bearing the route
     * may not want to leave on, and only the one span after it is free to
     * absorb the difference. Cutting the finished curve where it crosses the
     * clearance circle instead means the terminus is a point *on* the line, its
     * approach is the line's own tangent, and there is no junction to smooth —
     * and it is what makes the line arrive at a settlement through an opening
     * kept for it rather than run over the top of the pad it serves. */
    {   int trimmed = route_trim(out, count, a, b, clear);
        /* The field belongs to this line and to the patch of world it was laid
           across; leaving it standing would answer the next line's questions
           about a corridor somewhere else on the map. */
        keep_clear();
        return trimmed; }
}

/* ---------------- generation ---------------- */

void fly_rail_grade(float *z, const float *floor, const float *ceiling,
                    const float *chain, int n, float fill, float curve, int passes) {
    int pass, i;
    if (n < 3) return;
    /* Start on the floor, or on the ceiling wherever something is in the way.
     * Starting from wherever the caller left the heights makes the answer
     * depend on them, and the taut line does not: it is a property of its two
     * bounds and its two pinned ends alone. */
    for (i = 1; i + 1 < n; ++i) {
        z[i] = floor[i];
        if (ceiling && ceiling[i] < z[i]) z[i] = ceiling[i];
    }
    for (pass = 0; pass < passes; ++pass) {
        float worst = 0.0f;
        for (i = 1; i + 1 < n; ++i) {
            float d0 = chain[i] - chain[i - 1], d1 = chain[i + 1] - chain[i];
            float t = d0 + d1 > 1e-3f ? d0 / (d0 + d1) : 0.5f;
            float want = z[i - 1] + (z[i + 1] - z[i - 1]) * t;
            float move;
            /* --- the vertical curve ---
             *
             * The taut line is the shortest line over the floor, not the
             * smoothest one, and the difference is what happens where it
             * touches down: it arrives at a ridge on one gradient and leaves on
             * another, and the change between them is a corner. A road has a
             * vertical curve there and so does a guideway, and the length of
             * that curve is what makes a crest comfortable rather than a jump.
             *
             * Taking part of the target from the stations *two* out measures
             * the line against a baseline twice as long, so a station is pulled
             * off a corner and onto the arc through it. `curve` is how much of
             * the answer comes from that wider stencil; at zero this is the
             * taut line exactly. It costs fill, which is the honest price —
             * where you may not cut a crest, the way to round it is to build
             * the approaches up to it, and that is what a surveyor does too. */
            if (curve > 0.0f && i >= 2 && i + 2 < n) {
                /* The station that makes the second difference smoothest over
                 * five: minimising the sum of squared curvature gives this
                 * stencil, and taking the midpoint of the stations two out
                 * instead does not — that is a second taut line with a longer
                 * baseline, and mixing two taut lines at different scales has a
                 * fixed point that zig-zags between them. Measured: the wrong
                 * one took the worst grade break on a road from 15.5% to 18.3%
                 * while claiming to be a vertical curve. */
                float wide = (4.0f * (z[i - 1] + z[i + 1]) - (z[i - 2] + z[i + 2])) / 6.0f;
                want += (wide - want) * curve;
            }
            if (want < floor[i]) want = floor[i];
            if (want > floor[i] + fill) want = floor[i] + fill;
            /* And under whatever is over it. The ceiling is applied last and
             * wins outright, floor included: a line that has to pass beneath a
             * structure passes beneath it, and ground standing in the way of
             * that is an earthwork rather than a reason to drive through the
             * structure. */
            if (ceiling && want > ceiling[i]) want = ceiling[i];
            /* Under-relaxed: the curvature stencil has a negative lobe and
             * overshoots on its own, which on a long even gradient is a ripple
             * the taut term then has to take back out every pass. */
            move = (want - z[i]) * 0.72f;
            z[i] += move;
            if (move < 0.0f) move = -move;
            if (move > worst) worst = move;
        }
        if (worst < 0.005f) break;
    }
}

int fly_rail_generate(fly_rail_route *r, const fly_world *w,
                      int from, int to) {
    static fly_v2 path[FLY_RAIL_POINT_MAX];
    fly_rail_shape shape;
    int count, i;

    if (!r || !w || from < 0 || from >= w->nloc || to < 0 || to >= w->nloc || from == to)
        return -1;
    memset(r, 0, sizeof *r);
    memset(&shape, 0, sizeof shape);   /* nothing is built here to keep clear of */
    /* from/to are validated location indices (< nloc <= FLY_MAX_LOC); mod keeps
     * the slug two digits and lets the compiler bound the write */
    snprintf(r->id, sizeof r->id, "rail-%02d-%02d", from % 100, to % 100);
    r->from_location = from;
    r->to_location = to;
    /* Deck height over the ground. Low enough to board from, high enough that
       the piers under it are structure rather than kerbstones — at 3.5 m the
       line looked like a pipe lying in the grass. */
    r->clearance = 6.5f;
    r->speed = 38.0f;
    /* Stop clear of both landing decks. The line used to be generated centre to
     * centre, so the pipe ran straight over each pad and punched up through the
     * deck it was supposed to serve. A terminus set just outside the deck reads
     * as infrastructure arriving at the site instead of through it — and the
     * rider still steps off within docking range. */
    shape.clear = FLY_PAD_DECK_R + 14.0f;
    /* Seventy-five metres, not a hundred and twenty. The plan is bounded to a
     * four-kilometre radius and needs nothing finer than that; the *profile* is
     * what wanted it. A taut line over the ground changes gradient wherever it
     * touches down, and at 120 m stations that change is a visible kink in the
     * one piece of geometry the rider spends the whole journey looking down.
     * Finer stations put the vertical curve where fly_rail_grade can draw it,
     * and the deck comes out of the solve as a curve rather than as a chain of
     * straights with bends between them. */
    shape.spacing = 75.0f;
    shape.min_radius = FLY_RAIL_ROUTE_RADIUS;
    shape.grid = 0;       /* one line, laid once: take the fine survey */
    shape.climb_toll = 0.0f; /* a pipe on piers does not care about the rise */
    shape.meander = FLY_RAIL_ROUTE_MEANDER;   /* and it winds, as a road does */
    count = fly_rail_lay(w, w->loc[from].pos, w->loc[to].pos, &shape,
                         path, FLY_RAIL_POINT_MAX);
    if (count < 2) return -1;
    r->point_count = count;
    /* --- the profile ------------------------------------------------------
     *
     * A guideway holds a grade; it does not follow the ground. This used to be
     * `ground + clearance` at every station, which drapes the line over every
     * hummock between two settlements — the deck rose and fell by whatever the
     * heightfield did under it, and from the rider's seat the horizon bobbed.
     * The piers are what a line on piers has for exactly this: the deck runs
     * taut and each pier is however tall it has to be under it.
     *
     * So the floor is the ground plus the least clearance the line will accept,
     * and the fill is the tallest pier worth building. Over water the floor is
     * the water plus a navigation clearance, which is what makes a crossing a
     * bridge rather than a line of piers standing in the sea at the height the
     * bed happens to be. */
    {
        static float zs[FLY_RAIL_POINT_MAX], fl[FLY_RAIL_POINT_MAX];
        static float ch[FLY_RAIL_POINT_MAX];
        for (i = 0; i < count; ++i) {
            float ground = fly_world_ground(w, path[i].x, path[i].y);
            float wz = fly_world_water_at(w, path[i].x, path[i].y, ground);
            r->points[i].bridge = ground <= wz;
            /* Over water the floor is the water plus the navigation clearance,
               and it is *this* water: the sea at a bay, a river's own surface
               where the line crosses one three hundred metres up. */
            fl[i] = r->points[i].bridge ? wz + 8.0f : ground + FLY_RAIL_CLEAR_MIN;
            zs[i] = fl[i];
            ch[i] = i ? ch[i - 1] + hypotf(path[i].x - path[i - 1].x,
                                           path[i].y - path[i - 1].y) : 0.0f;
        }
        /* the termini are the platforms, and they sit at the deck height the
           settlement was built to meet */
        zs[0] = fl[0] + (r->clearance - FLY_RAIL_CLEAR_MIN);
        zs[count - 1] = fl[count - 1] + (r->clearance - FLY_RAIL_CLEAR_MIN);
        fly_rail_grade(zs, fl, NULL, ch, count, FLY_RAIL_PIER_MAX, 0.55f, 4000);
        for (i = 0; i < count; ++i) {
            r->points[i].pos = fly_v3mk(path[i].x, path[i].y, zs[i]);
            r->points[i].distance = i ? r->points[i - 1].distance +
                fly_v3dist(r->points[i - 1].pos, r->points[i].pos) : 0.0f;
        }
    }
    r->length = r->points[count - 1].distance;
    return 0;
}

/* Where the line is, and how far off it something is. Both are fly_line's —
 * see fly_line.h — and the guideway's part of the answer is which curve its
 * stations take: the centripetal one, because the trim that stops the line
 * short of each pad leaves a stub of a few metres against neighbours seventy
 * five long, and the uniform form on spacing like that shoots past the
 * terminus and doubles back through itself.
 *
 * It used to interpolate the chord instead, which is a third answer to a
 * question the drawing had already answered its own way: the pipe is drawn as
 * the curve through the same stations, so a railcar on the chord sat a sagitta
 * inside its own guideway on every bend. */
int fly_rail_eval(const fly_rail_route *r, float d, fly_v3 *p, fly_v3 *tangent) {
    if (!r || r->point_count < 2) return -1;
    return fly_line_eval(r->points, r->point_count, r->length, FLY_LINE_CHORD,
                         d, p, tangent) ? 0 : -1;
}

int fly_rail_project(const fly_rail_route *r, fly_v3 q, float *distance, float *separation) {
    if (!r || r->point_count < 2) return -1;
    return fly_line_project(r->points, r->point_count, q, distance, separation) ? 0 : -1;
}

float fly_rail_advance(const fly_rail_route *r, float d, float speed, float dt) {
    if (!r || dt <= 0.0f) return d;
    return fly_clampf(d + (speed > 0.0f ? speed : r->speed) * dt, 0.0f, r->length);
}
