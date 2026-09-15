#include "fly_sea.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* ---------------- the quay ----------------
 *
 * Where a settlement meets the water, and the only genuinely new question the
 * sea asks. Everything else here is the road network's answer with the medium
 * turned over; this one has no counterpart on land, because a road starts at
 * the gate and a lane cannot start at a gate at all.
 *
 * It is a walk rather than a search, and deliberately: the answer wanted is
 * "which way is the sea from here, and how far", which is 48 rays and a step
 * along each of them. A grid search would answer it to the metre and there is
 * nothing to spend that precision on — the berth is picked with a jetty's
 * length of slack either way and the lane is laid from it by a search that
 * does have a grid.
 *
 * Two things disqualify a bearing and both are about what the water *is*
 * rather than how far off it lies. Water that never reaches the draught
 * inside FLY_QUAY_RUN is a shoal, and a berth is not dredged out of one. Water
 * that reaches the draught but is not FLY_QUAY_BEAM wide is a channel — the
 * mouth of one of the rivers, most often, which is wet, deep at its middle and
 * emphatically not a harbour.
 *
 * It is also the most expensive thing this file does, because a terrain sample
 * is the most expensive thing the engine does and this is a few thousand of
 * them per site. Two rules keep it down and neither changes the answer: a
 * bearing stops at whatever the nearest one so far reached, since a shore
 * further out cannot win; and each sample asks the heightfield once and reads
 * both questions off the one height. */

static int navigable(const fly_world *w, float x, float y) {
    return fly_world_ground(w, x, y) <= FLY_WATER_LEVEL - FLY_SEA_DRAUGHT;
}

/* Room for a hull to lie in: navigable across the beam as well as along the
 * bearing. Asked square to the run, because the beam that matters is the one
 * across the channel and the run is pointing down it. */
static int berth_wide_enough(const fly_world *w, float x, float y, float ca, float sa) {
    float nx = -sa, ny = ca;
    return navigable(w, x + nx * FLY_QUAY_BEAM, y + ny * FLY_QUAY_BEAM) &&
           navigable(w, x - nx * FLY_QUAY_BEAM, y - ny * FLY_QUAY_BEAM);
}

/* The quay for one settlement, or 0 when it has no water it can reach. */
static int quay_find(const fly_world *w, const fly_road_net *roads, int loc,
                     fly_quay *out) {
    const fly_location *L = &w->loc[loc];
    float best = FLT_MAX;
    int b, found = 0;
    for (b = 0; b < FLY_QUAY_BEARINGS; ++b) {
        float a = (float)b / (float)FLY_QUAY_BEARINGS * 2.0f * FLY_PI;
        float ca = cosf(a), sa = sinf(a);
        float s, shore = -1.0f, berth = -1.0f;
        /* The landward end starts on the site's own apron and walks outward
           with the ray. Left at the origin instead — which is what happens
           when the very first sample is already water — the jetty is rooted at
           the middle of the chart and the quay is a structure thirty
           kilometres long. */
        float rx = L->pos.e + ca * FLY_PAD_BLEND_R, ry = L->pos.n + sa * FLY_PAD_BLEND_R;
        float rz = fly_world_ground(w, rx, ry);
        float bx = 0.0f, by = 0.0f;
        /* Out from the edge of the ground the pad grades — inside that the
           terrain is the settlement's apron rather than the country, and a
           waterline found in it would be one the aerodrome invented.

           Stopped at whatever the nearest bearing so far reached: a shore
           further out than one already found can never win, and walking to it
           anyway is a terrain sample every twenty-five metres for an answer
           that is thrown away. That is most of what this function costs. */
        {   float reach = FLY_QUAY_REACH < best ? FLY_QUAY_REACH : best;
            for (s = FLY_PAD_BLEND_R; s <= reach; s += FLY_QUAY_STEP) {
                float x = L->pos.e + ca * s, y = L->pos.n + sa * s;
                /* One sample, two questions. The bed says whether this is the
                   sea — a lake stands above sea level by its own spill point
                   and a channel is carved into ground that does, so anything
                   at or under the waterline out here is the sea or an arm of
                   it — and the same height says whether the ground is dry. */
                float gz = fly_world_ground(w, x, y);
                if (gz <= FLY_WATER_LEVEL) { shore = s; break; }
                /* The landward end is the last *dry* sample, which is not the
                   same as the last one above sea level: a river running out
                   across the beach is wet ground standing over the waterline,
                   and a jetty rooted in one has its shore end in the water. */
                if (gz <= fly_world_water_at(w, x, y, gz)) continue;
                rx = x;
                ry = y;
                rz = gz;
            } }
        /* No sea on this bearing inside the reach, or none nearer than the
           bearing that already won — the bound above stops the walk at `best`,
           so what is left here is the tie. */
        if (shore < 0.0f || shore >= best) continue;
        /* and if nothing on this bearing was dry, there is no root to build on */
        if (rz <= fly_world_water_at(w, rx, ry, rz)) continue;
        for (s = shore; s <= shore + FLY_QUAY_RUN; s += FLY_QUAY_STEP) {
            float x = L->pos.e + ca * s, y = L->pos.n + sa * s;
            if (!navigable(w, x, y)) continue;
            if (!berth_wide_enough(w, x, y, ca, sa)) continue;
            berth = s;
            bx = x;
            by = y;
            break;
        }
        if (berth < 0.0f) continue;   /* a shoal, or the mouth of a river */
        /* The landward end is a structure like any other and stands under the
           same rule: nothing is built in a carriageway. */
        if (roads && fly_road_blocked(roads, fly_wpos_mk(rx, ry))) continue;
        best = shore;
        found = 1;
        out->loc = loc;
        out->root = fly_v3mk(rx, ry, rz);
        out->head = fly_v3mk(bx, by, FLY_WATER_LEVEL);
        out->yaw = a;
        out->depth = FLY_WATER_LEVEL - fly_world_ground(w, bx, by);
        out->run = hypotf(bx - rx, by - ry);
    }
    return found;
}

/* ---------------- laying ---------------- */

typedef struct { int a, b; float len; int laid; } lane_edge;

/* Union-find over the harbours, so the first pass lays the links that join
 * parts of the map nothing else joins. The road network keeps its own copy of
 * this three-line function rather than sharing one, and so does this: it is
 * smaller than the header line that would export it. */
static int lane_root(int *p, int i) {
    while (p[i] != i) { p[i] = p[p[i]]; i = p[i]; }
    return i;
}

/* Lay one lane between two quays. Returns 0, or negative when there is no lane
 * to be had between them. */
static int lane_lay(fly_lane *ln, const fly_world *w, const fly_sea_net *sea,
                    int a, int b) {
    static fly_v2 plan[FLY_LANE_POINT_MAX];
    fly_rail_shape shape;
    const fly_quay *qa = &sea->quay[a], *qb = &sea->quay[b];
    int n, i;
    memset(&shape, 0, sizeof shape);
    shape.clear = FLY_SEA_CLEAR;
    shape.spacing = FLY_LANE_SPACING;
    shape.min_radius = FLY_LANE_MIN_RADIUS;
    /* The finest grid the search has. A lane is laid once per world, like the
       guideway and unlike the two dozen roads, and which side of an island it
       passes is exactly the question the resolution decides. */
    shape.grid = 0;
    /* No climb toll: there is no gradient on water, and the bed under it is
       not something a hull is going over. */
    shape.climb_toll = 0.0f;
    /* And no wander. A road wanders because a hundred local decisions made it;
       a shipping lane is the great circle somebody drew between two harbours,
       and the only reason it is not straight is what is in the way. */
    shape.meander = 0.0f;
    shape.sea = 1;
    shape.draught = FLY_SEA_DRAUGHT;
    n = fly_rail_lay(w, fly_wpos_mk(qa->head.x, qa->head.y),
                     fly_wpos_mk(qb->head.x, qb->head.y),
                     &shape, plan, FLY_LANE_POINT_MAX);
    if (n < 4) return -1;
    memset(ln, 0, sizeof *ln);
    ln->from = a;
    ln->to = b;
    ln->point_count = n;
    ln->speed = FLY_SEA_SPEED;
    for (i = 0; i < n; ++i) {
        /* The drape, and it is one line: the sea is a plane and a hull floats
           on it. Everything a road's drape has to do — the shelf, the taut
           profile, the earthworks, the causeway — exists because the ground
           under a road has a shape, and the ground under a lane does not. */
        ln->points[i].pos = fly_v3mk(plan[i].x, plan[i].y, FLY_WATER_LEVEL);
        ln->points[i].distance = i ? ln->points[i - 1].distance +
            fly_v3dist(ln->points[i - 1].pos, ln->points[i].pos) : 0.0f;
        ln->points[i].bridge = 0;
    }
    ln->length = ln->points[n - 1].distance;
    if (ln->length > FLY_LANE_MAX_LENGTH) return -2;
    if (ln->length < FLY_LANE_MIN_LENGTH) return -3;
    /* Every station has to be water a hull can float in. The search prices
     * land at FLY_LANE_LAND_COST rather than forbidding it, so a pair of
     * harbours with no sea route between them gets the cheapest crossing of
     * the isthmus instead of nothing — which is the right answer for a road
     * and a lane sailing over a hill for the wrong one. This is where that is
     * caught, and it is checked on the finished curve rather than on the plan
     * because the relaxation is entitled to move a station after the search
     * has approved it. */
    for (i = 0; i < n; ++i) {
        fly_v3 p = ln->points[i].pos;
        /* Between the stations as well as at them. A hundred and thirty metres
           is wider than a bar, a spit or the neck of a headland, so a lane
           checked only where it was sampled is a lane that can step over the
           one piece of land in its way — and the drawn hull would sail
           through it. Three samples a span put the coarsest question at
           thirty-three metres, which is narrower than anything the heightfield
           resolves. */
        if (i + 1 < n) {
            fly_v3 q = ln->points[i + 1].pos;
            int k;
            for (k = 1; k <= 3; ++k) {
                float t = (float)k * 0.25f;
                if (!navigable(w, fly_lerpf(p.x, q.x, t), fly_lerpf(p.y, q.y, t))) return -4;
            }
        }
        if (!navigable(w, p.x, p.y)) return -4;
    }
    ln->lo = fly_v2mk(ln->points[0].pos.x, ln->points[0].pos.y);
    ln->hi = ln->lo;
    for (i = 0; i < n; ++i) {
        fly_v3 p = ln->points[i].pos;
        if (p.x < ln->lo.x) ln->lo.x = p.x;
        if (p.y < ln->lo.y) ln->lo.y = p.y;
        if (p.x > ln->hi.x) ln->hi.x = p.x;
        if (p.y > ln->hi.y) ln->hi.y = p.y;
    }
    return 0;
}

void fly_sea_build(fly_sea_net *sea, const fly_world *w, const fly_road_net *roads) {
    static lane_edge edge[FLY_MAX_LOC * (FLY_MAX_LOC - 1) / 2];
    int parent[FLY_MAX_LOC], degree[FLY_MAX_LOC];
    int ne = 0, i, j, pass;
    if (!sea || !w) return;
    memset(sea, 0, sizeof *sea);
    for (i = 0; i < FLY_MAX_LOC; ++i) {
        sea->quay[i].loc = -1;
        parent[i] = i;
        degree[i] = 0;
    }
    for (i = 0; i < w->nloc; ++i) quay_find(w, roads, i, &sea->quay[i]);

    for (i = 0; i < w->nloc; ++i)
        for (j = i + 1; j < w->nloc; ++j) {
            float len;
            if (sea->quay[i].loc < 0 || sea->quay[j].loc < 0) continue;
            len = hypotf(sea->quay[i].head.x - sea->quay[j].head.x,
                         sea->quay[i].head.y - sea->quay[j].head.y);
            if (len > FLY_LANE_MAX_LENGTH) continue;
            if (ne >= (int)(sizeof edge / sizeof edge[0])) break;
            edge[ne].a = i;
            edge[ne].b = j;
            edge[ne].len = len;
            edge[ne].laid = 0;
            ++ne;
        }
    /* Shortest first, ties broken by the sites' own order so the network does
       not depend on how the pairs happened to be enumerated. The road network
       sorts the same way for the same reason. */
    for (i = 0; i < ne; ++i) {
        int best = i;
        for (j = i + 1; j < ne; ++j)
            if (edge[j].len < edge[best].len ||
                (edge[j].len == edge[best].len &&
                 (edge[j].a < edge[best].a ||
                  (edge[j].a == edge[best].a && edge[j].b < edge[best].b)))) best = j;
        if (best != i) { lane_edge t = edge[i]; edge[i] = edge[best]; edge[best] = t; }
    }
    /* First the links that join two harbours nothing else joins, then the
       short ones that close a loop — the road network's two passes, and for
       the road network's reason: a network wants to be connected before it
       wants to be dense. */
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < ne && sea->count < FLY_LANE_MAX; ++i) {
            int a = edge[i].a, b = edge[i].b, joined;
            if (edge[i].laid) continue;
            if (degree[a] >= FLY_LANE_DEGREE_MAX || degree[b] >= FLY_LANE_DEGREE_MAX) continue;
            joined = lane_root(parent, a) == lane_root(parent, b);
            if (pass == 0 && joined) continue;
            if (pass == 1 && (!joined || edge[i].len > 16000.0f)) continue;
            if (lane_lay(&sea->lane[sea->count], w, sea, a, b) != 0) continue;
            ++sea->count;
            edge[i].laid = 1;
            ++degree[a];
            ++degree[b];
            parent[lane_root(parent, a)] = lane_root(parent, b);
        }
}

/* ---------------- queries ---------------- */

const fly_quay *fly_sea_quay(const fly_sea_net *sea, int loc) {
    if (!sea || loc < 0 || loc >= FLY_MAX_LOC) return NULL;
    return sea->quay[loc].loc == loc ? &sea->quay[loc] : NULL;
}

int fly_sea_eval(const fly_lane *ln, float d, fly_v3 *pos, fly_v3 *tangent) {
    if (!ln || ln->point_count < 2) return -1;
    /* Evenly spaced stations, so the uniform curve — the same decision, and
       the same reason, as the carriageway's. See fly_line.h. */
    return fly_line_eval(ln->points, ln->point_count, ln->length, FLY_LINE_EVEN,
                         d, pos, tangent) ? 0 : -1;
}

fly_v3 fly_sea_side(fly_v3 c, fly_v3 heading) {
    float len = hypotf(heading.x, heading.y);
    if (len < 1e-4f) return c;
    c.x += heading.y / len * FLY_LANE_SIDE;
    c.y += -heading.x / len * FLY_LANE_SIDE;
    return c;
}

int fly_sea_at(const fly_sea_net *sea, int loc, int *out, int max) {
    int i, n = 0;
    if (!sea || !out) return 0;
    for (i = 0; i < sea->count && n < max; ++i)
        if (sea->lane[i].from == loc || sea->lane[i].to == loc) out[n++] = i;
    return n;
}

int fly_sea_project(const fly_sea_net *sea, fly_v3 q, float *distance, float *separation) {
    float best = FLT_MAX, best_d = 0.0f;
    int i, found = -1;
    if (!sea) return -1;
    for (i = 0; i < sea->count; ++i) {
        const fly_lane *ln = &sea->lane[i];
        float d = 0.0f, sep = 0.0f;
        if (q.x < ln->lo.x - best || q.x > ln->hi.x + best) continue;
        if (q.y < ln->lo.y - best || q.y > ln->hi.y + best) continue;
        if (!fly_line_project(ln->points, ln->point_count, q, &d, &sep)) continue;
        if (sep < best) { best = sep; best_d = d; found = i; }
    }
    if (found < 0) return -1;
    if (distance) *distance = best_d;
    if (separation) *separation = best;
    return found;
}

int fly_sea_buoy(const fly_lane *ln, int k, fly_v3 *pos, int *port) {
    float chain;
    fly_v3 p, t;
    if (!ln || ln->point_count < 2 || k < 0) return 0;
    /* Half a pitch in from the terminus at each end, so the first mark is a
       fairway buoy off the harbour rather than one sitting on the berth. */
    chain = FLY_SEA_BUOY_PITCH * ((float)k + 0.5f);
    if (chain >= ln->length) return 0;
    if (fly_sea_eval(ln, chain, &p, &t) != 0) return 0;
    /* Alternating sides of the fairway, which is what makes a pair of them a
       channel rather than a row of floats. */
    {   int left = (k & 1) != 0;
        float s = left ? -1.0f : 1.0f;
        float len = hypotf(t.x, t.y);
        if (len < 1e-4f) return 0;
        p.x += t.y / len * FLY_LANE_HALF * s;
        p.y += -t.x / len * FLY_LANE_HALF * s;
        p.z = FLY_WATER_LEVEL;
        if (pos) *pos = p;
        if (port) *port = left; }
    return 1;
}
