#include "fly_convoy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------- the ladder ----------------
 *
 * Four rungs, and every number a convoy has is on one of them. The claim the
 * table makes is that size, strength and worth are the same axis: a Leviathan
 * is three times the column of a Dray, takes twenty times the punishment, puts
 * up real flak, crawls, and sheds five rolls at a bias nothing on a road below
 * it reaches. Nothing picks a tier for how it looks — see convoy_tier, where
 * the rung comes off how dangerous the country the road crosses is. */
static const fly_convoy_class CLASS[FLY_CONVOY_TIER_COUNT] = {
    /*                    trk hull  armr  dps   rng   spd  cargo bias rolls  gap */
    { "Dray",              2,  25,  0.00f,  3,  420,  1.00f,  9,  -0.4f, 1,  26 },
    { "Hauler",            3,  40,  0.14f,  8,  620,  0.92f, 14,   0.4f, 2,  26 },
    { "Armoured column",   4,  65,  0.28f, 16,  850,  0.84f, 20,   1.2f, 3,  26 },
    { "Leviathan train",   6, 100,  0.42f, 28, 1100,  0.74f, 26,   2.1f, 5,  26 }
};

/* And the ladder afloat, read down the same columns and making the same claim
 * one medium over: bigger hulls are longer, tougher, better armed, slower and
 * worth more.
 *
 * The numbers are not the road's numbers scaled, because a ship is not a big
 * lorry. A hull carries three to four times the freight of a whole column, so
 * a lane matters to the market out of all proportion to how many ships are on
 * it; it takes several times the punishment, because a strafing pass that
 * stops a lorry does not stop four thousand tonnes; and it makes nine metres a
 * second against a road's twenty-one, which is what turns a strafing run into
 * a decision rather than a reflex — a ship cannot outrun anything and does not
 * try, so an attack on one is a fight rather than an ambush.
 *
 * The bottom rung has no guns at all and that is the point of it: a lighter
 * under tow is the one piece of freight in this world that cannot answer, so
 * burning one is a choice about what sort of operator you are rather than a
 * risk you took. It is also the only rung that is more than one vessel, and
 * for the same reason a Dray is two lorries — a tow is a tug and its barge. */
static const fly_convoy_class SHIP[FLY_SHIP_TIER_COUNT] = {
    /*                    vsl hull  armr  dps   rng   spd  cargo bias rolls  gap */
    { "Lighter",           2,  55,  0.00f,  0,    0,  1.00f, 12,  -0.3f, 1,  62 },
    { "Coaster",           1, 170,  0.10f,  7,  700,  0.94f, 24,   0.5f, 2,  74 },
    { "Freighter",         1, 300,  0.26f, 15,  950,  0.86f, 36,   1.3f, 3,  92 },
    { "Ore carrier",       2, 420,  0.38f, 26, 1250,  0.72f, 48,   2.2f, 5, 118 }
};

const fly_convoy_class *fly_convoy_class_get(int sea, int tier) {
    if (sea) return tier >= 0 && tier < FLY_SHIP_TIER_COUNT ? &SHIP[tier] : NULL;
    return tier >= 0 && tier < FLY_CONVOY_TIER_COUNT ? &CLASS[tier] : NULL;
}

const char *fly_convoy_tier_name(int sea, int tier) {
    const fly_convoy_class *k = fly_convoy_class_get(sea, tier);
    return k ? k->name : "?";
}

/* --- the two ways, handed over as one --------------------------------------
 *
 * Both of these are a lookup and a copy of six fields. They live here rather
 * than in fly_road and fly_sea because a `fly_way` is the convoy's idea of a
 * line, not the network's: neither network has any use for one, and putting
 * the constructor beside the consumer keeps both networks unaware that
 * anything runs on them at all. */
int fly_road_as_way(const fly_road_net *net, int road, fly_way *out) {
    const fly_road *r;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!net || road < 0 || road >= net->count) return 0;
    r = &net->road[road];
    if (r->point_count < 2) return 0;
    out->points = r->points;
    out->count = r->point_count;
    out->length = r->length;
    out->speed = r->speed;
    out->from = r->from;
    out->to = r->to;
    out->sea = 0;
    return 1;
}

int fly_sea_as_way(const fly_sea_net *sea, int lane, fly_way *out) {
    const fly_lane *l;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!sea || lane < 0 || lane >= sea->count) return 0;
    l = &sea->lane[lane];
    if (l->point_count < 2) return 0;
    out->points = l->points;
    out->count = l->point_count;
    out->length = l->length;
    out->speed = l->speed;
    out->from = l->from;
    out->to = l->to;
    out->sea = 1;
    return 1;
}

/* Where the way is, and where on it a vehicle keeping its own side sits. The
 * two media differ here and nowhere else in this file: a carriageway has a
 * near side and a camber, and a fairway has a starboard side and no camber at
 * all, because the sea is flat. */
static int way_eval(const fly_way *w, float d, fly_v3 *pos, fly_v3 *tangent) {
    return fly_line_eval(w->points, w->count, w->length, FLY_LINE_EVEN,
                         d, pos, tangent);
}

static fly_v3 way_side(const fly_way *w, fly_v3 p, fly_v3 heading) {
    return w->sea ? fly_sea_side(p, heading) : fly_road_lane(p, heading);
}

/* ---------------- spawning ---------------- */

static void convoy_name(fly_convoy *c, fly_rng *rng) {
    static const char *stem[8] = {
        "Ironhaul", "Longspan", "Slowmatch", "Dustline",
        "Trundle", "Cinderway", "Packhorse", "Bulwark"
    };
    /* A hull is named, and a road column is numbered. That is not decoration:
       what the event log says is the only way a player ever learns which of
       the two they are looking at from twelve kilometres away, and "Ironhaul
       47" and "Saltmare" read as different kinds of thing on sight. */
    static const char *hull[8] = {
        "Saltmare", "Longshoal", "Kelpdrift", "Brineway",
        "Coldreach", "Tidewrack", "Sternlight", "Greyfathom"
    };
    unsigned s = (unsigned)fly_rng_range(rng, 8);
    unsigned n = (unsigned)fly_rng_range(rng, 90) + 10;
    if (c->sea) snprintf(c->name, sizeof c->name, "%s", hull[s]);
    else snprintf(c->name, sizeof c->name, "%s %u", stem[s], n);
}

/* The rung this road deserves. Danger is the field the loot economy already
 * hangs off, so a road through pirate country carries the columns worth
 * escorting and the ones worth robbing, and both are the same columns. */
static int convoy_tier(const fly_world *w, const fly_way *way, fly_rng *rng) {
    fly_v3 mid = way->points[way->count / 2].pos;
    float danger = fly_world_danger(w, mid.x, mid.y);
    float f = danger * 3.9f - 0.35f + fly_rng_span(rng, -0.45f, 0.45f);
    int t = (int)(f + 0.5f);
    if (t < 0) t = 0;
    if (t >= FLY_CONVOY_TIER_COUNT) t = FLY_CONVOY_TIER_COUNT - 1;
    return t;
}

/* What is worth hauling, and which way. Surplus at one end against shortfall at
 * the other, which is the same thing a trader reads off the market — so a road
 * carries what the map actually wants moved rather than a random crate. */
static int convoy_freight(const fly_world *w, int a, int b, int *reverse, float *qty) {
    float best = 0.0f;
    int r, pick = -1;
    for (r = 0; r < FLY_RES_COUNT; ++r) {
        int d;
        for (d = 0; d < 2; ++d) {
            int from = d ? b : a, to = d ? a : b;
            float surplus = w->loc[from].stock[r] - w->loc[from].target[r] * 0.6f;
            float want = w->loc[to].target[r] - w->loc[to].stock[r];
            float move = surplus < want ? surplus : want;
            if (move > best) { best = move; pick = r; *reverse = d; }
        }
    }
    if (pick < 0) return -1;
    *qty = best;
    return pick;
}

int fly_convoy_spawn(fly_convoy *c, uint32_t id, const fly_way *way,
                     const fly_world *w, int line, fly_rng *rng) {
    const fly_convoy_class *k;
    int res, reverse = 0;
    float qty = 0.0f, hold;
    if (!c || !way || !way->points || way->count < 2 || !w || !rng) return -1;
    res = convoy_freight(w, way->from, way->to, &reverse, &qty);
    if (res < 0) return -2;
    memset(c, 0, sizeof *c);
    c->sea = (unsigned char)(way->sea != 0);
    c->tier = (unsigned char)convoy_tier(w, way, rng);
    k = fly_convoy_class_get(c->sea, c->tier);
    hold = k->cargo * (float)k->trucks;
    if (qty > hold) qty = hold;
    /* --- not worth turning a key for ---
     *
     * A quarter of the hold on the road: a column that leaves with a crate in
     * it is traffic pretending to be freight, and there are two dozen roads,
     * so the one with nothing worth moving today simply does not run.
     *
     * A tenth of it at sea, and the difference is a fact about the two rather
     * than a dial. A hull is on a schedule and sails part-loaded, because the
     * alternative is a berth she is not earning in; and there are a handful of
     * lanes against two dozen roads, so a lane standing down for want of a
     * full hold is an empty sea rather than one quiet road. */
    if (qty < hold * (c->sea ? 0.10f : 0.25f)) return -2;
    c->active = 1;
    c->id = id;
    convoy_name(c, rng);
    c->line = line;
    c->reverse = reverse;
    c->faction = w->loc[reverse ? way->to : way->from].owner;
    c->trucks = k->trucks;
    c->hp = c->hp_max = k->hull * (float)k->trucks;
    /* And the launcher, on the rungs that carry one. Loaded at spawn rather
     * than replenished, so outlasting a column's missiles is a thing that can
     * actually be done — see FLY_CONVOY_SAM_LOAD. */
    c->sam_ammo = c->tier >= FLY_CONVOY_SAM_TIER ? FLY_CONVOY_SAM_LOAD(c->tier) : 0;
    c->res = res;
    c->qty = qty;
    c->speed = way->speed * k->speed_mul;
    c->seed = id * 2654435761u + 17u;
    c->distance = reverse ? way->length : 0.0f;
    way_eval(way, c->distance, &c->pos, &c->tangent);
    if (reverse) c->tangent = fly_v3scale(c->tangent, -1.0f);
    c->pos = way_side(way, c->pos, c->tangent);
    return 0;
}

int fly_convoy_origin(const fly_convoy *c, const fly_way *way) {
    if (!c || !way || !way->points) return -1;
    return c->reverse ? way->to : way->from;
}

int fly_convoy_destination(const fly_convoy *c, const fly_way *way) {
    if (!c || !way || !way->points) return -1;
    return c->reverse ? way->from : way->to;
}

void fly_convoy_load(fly_convoy *c, fly_world *w, const fly_way *way) {
    int from = fly_convoy_origin(c, way);
    if (!c || !w || from < 0 || c->res < 0 || c->res >= FLY_RES_COUNT) return;
    if (w->loc[from].stock[c->res] < c->qty) c->qty = w->loc[from].stock[c->res];
    w->loc[from].stock[c->res] -= c->qty;
}

void fly_convoy_unload(const fly_convoy *c, fly_world *w, const fly_way *way) {
    int to = fly_convoy_destination(c, way);
    if (!c || !w || to < 0 || c->res < 0 || c->res >= FLY_RES_COUNT) return;
    w->loc[to].stock[c->res] += c->qty;
}

/* ---------------- running ---------------- */

int fly_convoy_step(fly_convoy *c, const fly_way *way, float dt) {
    float dir;
    if (!c || !c->active || !way || !way->points || way->count < 2) return 0;
    dir = c->reverse ? -1.0f : 1.0f;
    c->alert = c->alert > dt * 0.25f ? c->alert - dt * 0.25f : 0.0f;
    if (c->fire_cooldown > 0.0f) c->fire_cooldown -= dt;
    /* Under fire the column runs for the far end as hard as it will go. There
       is nowhere else to be: a road has two ends and one of them is home. */
    c->distance += dir * c->speed * (1.0f + 0.25f * c->alert) * dt;
    if (c->distance <= 0.0f) c->distance = 0.0f;
    if (c->distance >= way->length) c->distance = way->length;
    way_eval(way, c->distance, &c->pos, &c->tangent);
    if (c->reverse) c->tangent = fly_v3scale(c->tangent, -1.0f);
    /* The column reports where its lead truck actually is, which after the
       lane rule is not the centreline. Everything that aims at a convoy, draws
       one or measures the distance to one reads this, so a position taken off
       the middle of the road would be a column the guns and the geometry
       disagree about by half a carriageway. */
    c->pos = way_side(way, c->pos, c->tangent);
    return c->reverse ? c->distance <= 0.0f : c->distance >= way->length;
}

int fly_convoy_damage(fly_convoy *c, float damage) {
    const fly_convoy_class *k;
    int left;
    if (!c || !c->active || damage <= 0.0f) return 0;
    k = fly_convoy_class_get(c->sea, c->tier);
    if (!k) return 0;
    c->hp -= damage * (1.0f - k->armour);
    c->alert = 1.0f;
    /* The column shortens as it burns: what is left of the hit points is what
       is left of the trucks, and everything that reads the column — the guns it
       still has, the loot, the shape of it on the road — reads that. */
    left = (int)ceilf(c->hp / k->hull);
    if (left < 1) left = 1;
    if (left < c->trucks) c->trucks = left;
    if (c->hp <= 0.0f) { c->hp = 0.0f; return 1; }
    return 0;
}

/* ---------------- guns ---------------- */

float fly_convoy_gun_solution(const fly_craft *shooter, const fly_airframe *af,
                              const fly_convoy *c) {
    fly_v3 to, fwd;
    float d, cosang;
    if (!shooter || !af || !c || !c->active || af->gun_dps <= 0.0f) return 0.0f;
    to = fly_v3sub(c->pos, shooter->pos);
    d = fly_v3len(to);
    if (d > af->gun_range || d < 1.0f) return 0.0f;
    fwd = fly_qrot(shooter->ori, fly_v3mk(1, 0, 0));
    cosang = fly_v3dot(fly_v3norm(to), fwd);
    if (cosang < 0.970f) return 0.0f; /* ~14 degree cone */
    return (cosang - 0.970f) / 0.030f * (1.0f - d / (af->gun_range * 1.2f));
}

float fly_convoy_air_solution(const fly_convoy *c, fly_v3 pos, fly_v3 vel) {
    const fly_convoy_class *k;
    float d, cross, aim;
    if (!c || !c->active) return 0.0f;
    k = fly_convoy_class_get(c->sea, c->tier);
    if (!k || k->gun_dps <= 0.0f) return 0.0f;
    d = fly_v3dist(pos, c->pos);
    if (d > k->gun_range || d < 1.0f) return 0.0f;
    if (pos.z < c->pos.z - 20.0f) return 0.0f; /* it cannot depress into the hill */
    /* Crossing speed, not closing: a target coming down the road at you is the
       easy one, and the fast beam pass is what a gunner misses. */
    cross = fly_v3len(fly_v3sub(vel, fly_v3scale(c->tangent, fly_v3dot(vel, c->tangent))));
    aim = (1.0f - d / k->gun_range) / (1.0f + cross / 110.0f);
    /* Awake or merely watching: the first pass is the cheap one. */
    return fly_clampf(aim * (0.55f + 0.45f * c->alert), 0.0f, 1.0f);
}

float fly_convoy_threat(const fly_convoy *c) {
    const fly_convoy_class *k;
    if (!c || !c->active) return 0.0f;
    k = fly_convoy_class_get(c->sea, c->tier);
    if (!k) return 0.0f;
    return k->gun_dps * (float)c->trucks / (float)k->trucks;
}

int fly_convoy_truck(const fly_convoy *c, const fly_way *way, int i,
                     fly_v3 *pos, float *yaw) {
    const fly_convoy_class *k;
    float dir, d;
    fly_v3 p, t;
    if (!c || !c->active || !way || !way->points || i < 0 || i >= c->trucks) return 0;
    k = fly_convoy_class_get(c->sea, c->tier);
    if (!k) return 0;
    dir = c->reverse ? -1.0f : 1.0f;
    d = c->distance - dir * k->gap * (float)i;
    if (d < 0.0f || d > way->length) return 0;   /* still coming out of the yard */
    if (!way_eval(way, d, &p, &t)) return 0;
    t = fly_v3scale(t, dir);
    if (pos) *pos = way_side(way, p, t);
    if (yaw) *yaw = atan2f(t.y, t.x);
    return 1;
}
