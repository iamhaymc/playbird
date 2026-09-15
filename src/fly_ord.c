#include "fly_ord.h"

#include <math.h>
#include <string.h>

/* ---------------- the ladder ----------------
 *
 * Eight rows, and every number a round in the air carries is on one of them.
 * Read down the columns and the design is the claim:
 *
 *   the unguided are fast, cheap and short-lived — they hit what was already
 *   in front of the nose, and nothing else;
 *   the seeker is the slowest thing that flies and the only one that will
 *   follow you, and its motor is out after three seconds so most of its life
 *   is spent spending energy it cannot replace;
 *   the dart is quick and reaches a long way, and pays for it by needing the
 *   shooter to keep pointing;
 *   the countermeasures carry no damage at all and exist only to be looked at.
 *
 * `speed` is what the motor is worth on top of the platform's own velocity, so
 * a rocket off a fast aeroplane really does arrive sooner. */
/* One thing to read the `speed` column against, because it is the number every
 * envelope in the registry is really a statement about: the aeroplanes in this
 * world cruise at sixty to eighty metres a second. A round at 210 therefore
 * closes a tail chase at about 140, and its sixteen seconds of life reach a
 * little over two kilometres — which is why the seeker rack's envelope is 2400
 * and not the 3200 it was first written as. An envelope longer than the round
 * can fly is a weapon that misses for reasons the player cannot see, and the
 * first version of this table had exactly that: a 150 m/s missile with a 3.2 km
 * launch range, closing at eighty, needing seventeen seconds of a fifteen
 * second life. It never arrived, and nothing on screen said why. */
/* Named ORD_CLASS rather than CLASS because the amalgamated `build/<rid>/fly.h`
 * puts every module in one translation unit, where a file-static in two of them
 * is a redefinition — fly_convoy.c has a `CLASS` of its own, and `make.py
 * --roll` catches the collision by compiling the roll. */
static const fly_ord_class ORD_CLASS[FLY_ORD_KIND_COUNT] = {
    /*  name          tag     speed burn  life  turn  cone  dmg  blast arm  drag  slv reload */
    {  "none",       "----",    0,    0,    0,    0, 0.00f,   0,   0, 0.0f, 0.0f, 0, 0.0f },
    {  "rockets",    "RKT",  240.0f, 0.9f, 4.0f,   0, 0.00f,  26,  14, 0.10f, 0.00016f, 4, 1.4f },
    {  "bombs",      "BMB",    6.0f, 0.0f, 20.0f,  0, 0.00f,  95,  46, 0.35f, 0.00004f, 1, 2.2f },
    {  "seeker",     "SKR",  210.0f, 3.2f, 16.0f, 62, 0.52f,  58,  22, 0.45f, 0.00026f, 1, 3.6f },
    {  "dart",       "DRT",  420.0f, 2.0f, 14.0f, 96, 0.30f,  46,  16, 0.40f, 0.00012f, 1, 4.2f },
    {  "sam",        "SAM",  240.0f, 4.0f, 18.0f, 74, 0.48f,  62,  24, 0.60f, 0.00016f, 1, 9.0f },
    {  "flares",     "FLR",   14.0f, 0.0f, 5.2f,   0, 0.00f,   0,   0, 9.9f, 0.00340f, 3, 1.1f },
    {  "chaff",      "CHF",    9.0f, 0.0f, 6.0f,   0, 0.00f,   0,   0, 9.9f, 0.00900f, 2, 1.1f }
};

const fly_ord_class *fly_ord_class_get(int kind) {
    if (kind <= 0 || kind >= FLY_ORD_KIND_COUNT) return NULL;
    return &ORD_CLASS[kind];
}

const char *fly_ord_kind_name(int kind) {
    const fly_ord_class *k = fly_ord_class_get(kind);
    return k ? k->name : "none";
}

int fly_ord_is_heat(int kind) {
    return kind == FLY_ORD_SEEKER || kind == FLY_ORD_SAM;
}

int fly_ord_is_radar(int kind) {
    return kind == FLY_ORD_DART;
}

int fly_ord_is_ballistic(int kind) {
    return kind == FLY_ORD_BOMB || kind == FLY_ORD_FLARE || kind == FLY_ORD_CHAFF;
}

/* May this guidance look at this contact at all?
 *
 * One function, because a flare that decoyed a radar dart would quietly delete
 * the reason chaff exists, and a seeker that locked a truck would make the
 * air-to-air weapon the best ground-attack weapon in the game. A decoy is only
 * visible to the guidance it was built to answer; everything else has to be
 * flying. */
static int seeker_sees(int seeker_kind, const fly_ord_contact *c) {
    if (c->heat_decoy > 0.0f) return fly_ord_is_heat(seeker_kind);
    if (c->radar_decoy > 0.0f) return fly_ord_is_radar(seeker_kind);
    return c->airborne;
}

/* And how bright it is from where this round is sitting. */
static float signature_of(const fly_ord *o, const fly_ord_contact *c) {
    fly_v3 to_round = fly_v3sub(o->pos, c->pos);
    if (fly_ord_is_radar(o->kind)) {
        if (c->radar_decoy > 0.0f) return c->radar_decoy;
        return fly_ord_radar_signature(c->mass, to_round, c->vel);
    }
    if (c->heat_decoy > 0.0f) return c->heat_decoy;
    /* Negated: `to_round` points from the target out to the round, so a round
     * sitting astern gives -1 there and is the shot the seeker wants. */
    return fly_ord_heat_signature(c->throttle, fly_v3len(c->vel),
                                  -fly_v3dot(c->nose, fly_v3norm(to_round)), c->pos.z);
}

/* ---------------- what a target looks like ----------------
 *
 * The two functions the counterplay hangs off. Neither draws a random number,
 * and both are written so that the thing a pilot would instinctively try is the
 * thing that works.
 */

float fly_ord_heat_signature(float throttle, float speed, float aspect, float alt) {
    /* Tail-on is the shot. `aspect` is the cosine between where the target is
     * pointing and where the seeker is sitting, so 1 is straight up the
     * tailpipe and -1 is head-on; the plume term is raised to a power so the
     * fall-off is concentrated near the beam rather than spread evenly, which
     * is what makes "turn your tail away from it" a real answer instead of a
     * gentle discount. */
    float a01 = fly_clampf((aspect + 1.0f) * 0.5f, 0.0f, 1.0f);
    float look = a01 * a01 * a01;
    /* And the throttle, which is the other half. An aeroplane at idle is dim
     * from every angle, so going cold is an answer — an expensive one, because
     * it is also how you stay in the air and how you get away. */
    float plume = (0.06f + 0.94f * fly_clampf(throttle, 0.0f, 1.0f)) *
                  (0.10f + 0.90f * look);
    /* Skin friction: at speed the leading edges glow whatever the engine is
     * doing, which is why a cold high-speed dash is not invisible either. */
    float skin = (speed / 260.0f) * (speed / 260.0f) * 0.22f;
    /* Thinner air carries less of it away, but the effect is gentle — this is
     * a tilt, not a gate, because a seeker that stopped working at altitude
     * would make "climb" the one answer to everything. */
    float thin = 1.0f / (1.0f + fly_clampf(alt, 0.0f, 40000.0f) / 26000.0f);
    return (plume + skin) * thin;
}

float fly_ord_radar_signature(float mass, fly_v3 to_emitter, fly_v3 vel) {
    /* Size, and then the Doppler gate — which is the whole of why a radar
     * missile is a different problem from a heat one. A set that could not
     * reject the ground it is looking down at would see nothing else, so it
     * keeps only what is closing or opening; a target turned side-on to the
     * emitter has almost no radial velocity, falls into the notch, and is
     * simply not there. So the answer to a dart is a hard beam turn, where the
     * answer to a seeker is to show it something hotter — two threats, two
     * manoeuvres, and neither one works on the other. */
    fly_v3 u = fly_v3norm(to_emitter);
    float radial = fabsf(fly_v3dot(vel, u));
    float gate = fly_smoothstepf(5.0f, 24.0f, radial);
    float size = fly_clampf(mass / 1400.0f, 0.15f, 3.0f);
    return size * (0.10f + 0.90f * gate);
}

/* ---------------- terrain ---------------- */

int fly_ord_line_of_sight(fly_ground_fn ground, void *guser, fly_v3 a, fly_v3 b) {
    int i;
    if (!ground) return 1;
    /* Sixteen samples, and the ends are skipped: a round that has just left a
     * pylon is by definition a metre from an aeroplane that is by definition
     * not underground, and a sample exactly at the target reports the target's
     * own clearance rather than anything between. */
    for (i = 1; i < 16; ++i) {
        float t = (float)i / 16.0f;
        fly_v3 p = fly_v3lerp(a, b, t);
        if (p.z < ground(guser, p.x, p.y)) return 0;
    }
    return 1;
}

/* ---------------- launching ---------------- */

void fly_ord_launch(fly_ord *o, uint32_t id, int kind, int faction, fly_ord_ref owner,
                    fly_ord_ref target, fly_v3 pos, fly_v3 vel, float damage_scale) {
    const fly_ord_class *k = fly_ord_class_get(kind);
    if (!o || !k) return;
    memset(o, 0, sizeof *o);
    o->active = 1;
    o->id = id;
    o->kind = (unsigned char)kind;
    o->faction = (unsigned char)faction;
    o->owner = owner;
    o->target = target;
    o->pos = pos;
    o->vel = vel;
    o->vmax = fly_v3len(vel) + k->speed;
    o->life = k->life;
    o->burn = k->burn;
    o->damage = k->damage * (damage_scale > 0.0f ? damage_scale : 1.0f);
    o->blast = k->blast;
    o->turn = k->turn;
    o->seek_cone = k->seek_cone;
    /* A countermeasure is a target, not a weapon: what it carries is a
     * signature, and it is far brighter than the aeroplane that shed it —
     * which is exactly why it works and exactly why it does not work for long.
     * Both decay in the step. */
    if (kind == FLY_ORD_FLARE) o->decoy_heat = 7.5f;
    if (kind == FLY_ORD_CHAFF) o->decoy_radar = 6.0f;
    o->seed = fly_hash2(id * 2654435761u, kind, faction);
}

/* ---------------- guidance ----------------
 *
 * Proportional navigation: turn at a rate proportional to how fast the line of
 * sight to the mark is rotating. It is the law a real seeker flies and it is
 * the right one here for the reason the TODO gives about the gun being the
 * opposite case — a round with travel time has to aim at where the target will
 * be, and driving the line-of-sight rate to zero *is* that, computed afresh
 * every step rather than predicted once at launch.
 *
 * The pure form has one hole: when the geometry is already a collision course
 * the rotation rate is zero, so the command is zero, and small integration
 * errors are never corrected. A light pursuit term rides along underneath to
 * close that, which costs nothing when the round is tracking well and keeps the
 * miss distance inside the fuse when it is not. */
static fly_v3 guidance_accel(const fly_ord *o, fly_v3 tpos, fly_v3 tvel) {
    fly_v3 r = fly_v3sub(tpos, o->pos);
    fly_v3 vr = fly_v3sub(tvel, o->vel);
    float r2 = fly_v3dot(r, r);
    fly_v3 omega, a, pursue;
    float mag;
    if (r2 < 1.0f) return fly_v3zero();
    omega = fly_v3scale(fly_v3cross(r, vr), 1.0f / r2);
    a = fly_v3scale(fly_v3cross(omega, o->vel), 3.4f);
    pursue = fly_v3scale(fly_v3sub(fly_v3norm(r), fly_v3norm(o->vel)), o->turn * 0.45f);
    a = fly_v3add(a, pursue);
    /* And the limit, which is the number that decides whether the shot can be
     * out-turned. Bounded lateral acceleration is the whole reason a hard break
     * at the right moment works: the round is asking for more than it has. */
    mag = fly_v3len(a);
    if (mag > o->turn && mag > 1e-4f) a = fly_v3scale(a, o->turn / mag);
    return a;
}

/* Pick what to fly at.
 *
 * Re-run every step, and that is the point: a lock is not something acquired
 * once and kept, it is a continuous judgement about which is the brightest
 * thing in the seeker's window. A flare popped across the line of sight is
 * brighter than an engine for about two seconds, so it wins that judgement, and
 * by the time it has burned down the aeroplane is somewhere the head is no
 * longer looking. Nothing rolls; the timing is the mechanic.
 *
 * The bias toward whatever it is already tracking is small and deliberate: with
 * none at all a seeker flickers between two similar contacts and flies at
 * neither, and with much more a flare would have to be implausibly bright. */
static int reacquire(const fly_ord *o, const fly_ord_contact *cs, int n,
                     fly_ground_fn ground, void *guser) {
    fly_v3 fwd = fly_v3norm(o->vel);
    float best = 0.0f;
    int i, pick = -1;
    for (i = 0; i < n; ++i) {
        const fly_ord_contact *c = &cs[i];
        fly_v3 to;
        float d, cosang, sig;
        if (!seeker_sees(o->kind, c)) continue;
        if (c->ref.kind == o->owner.kind && c->ref.id == o->owner.id) continue;
        if (c->faction == o->faction && c->faction != 0) continue;
        to = fly_v3sub(c->pos, o->pos);
        d = fly_v3len(to);
        if (d < 1.0f) { pick = i; break; }
        cosang = fly_v3dot(fly_v3scale(to, 1.0f / d), fwd);
        if (cosang < cosf(o->seek_cone)) continue;   /* outside the head's window */
        /* Ground in the way and the head is looking at a hillside. This is the
         * cheapest and most satisfying answer to a launch: get low, put a ridge
         * between the two of you, and the thing loses you. */
        if (!fly_ord_line_of_sight(ground, guser, o->pos, c->pos)) continue;
        sig = signature_of(o, c) / (1.0f + d * d / 1.6e6f);
        if (c->ref.kind == o->target.kind && c->ref.id == o->target.id) sig *= 1.25f;
        if (sig > best) { best = sig; pick = i; }
    }
    return pick;
}

static int contact_index(const fly_ord_contact *cs, int n, fly_ord_ref ref) {
    int i;
    if (ref.kind == FLY_ORD_ACTOR_NONE) return -1;
    for (i = 0; i < n; ++i)
        if (cs[i].ref.kind == ref.kind && cs[i].ref.id == ref.id) return i;
    return -1;
}

int fly_ord_step(fly_ord *o, const fly_ord_contact *contacts, int contact_count,
                 fly_ground_fn ground, void *guser, fly_v3 wind, float dt) {
    const fly_ord_class *k;
    float speed, gz;
    int guided, ti, i;
    if (!o || !o->active) return -1;
    k = fly_ord_class_get(o->kind);
    if (!k) return -2;
    guided = o->turn > 0.0f;

    o->age += dt;
    if (o->burn > 0.0f) o->burn -= dt;
    if (o->age >= o->life) return -2;
    if (o->age >= k->arm_time) o->armed = 1;
    /* A countermeasure is only worth what it is still burning. Both decay on a
     * curve rather than switching off, so a flare popped too early is a flare
     * that hands the seeker back to you halfway through the turn. */
    if (o->decoy_heat > 0.0f)
        o->decoy_heat = 7.5f * fly_clampf(1.0f - o->age / 3.4f, 0.0f, 1.0f);
    if (o->decoy_radar > 0.0f)
        o->decoy_radar = 6.0f * fly_clampf(1.0f - o->age / 4.2f, 0.0f, 1.0f);

    /* --- what it is chasing ------------------------------------------- */
    if (guided) {
        int pick = reacquire(o, contacts, contact_count, ground, guser);
        if (pick >= 0) {
            o->target = contacts[pick].ref;
        } else if (o->age > 0.4f) {
            /* Nothing in the window. It does not stop flying — it carries
             * straight on, which is what a missile that has lost its mark
             * actually does, and it means breaking a lock buys the seconds
             * until the motor runs out rather than switching the threat off.
             *
             * The first four tenths of a second are exempt because the round
             * has not finished leaving the rail: it is still pointed wherever
             * the pylon was aimed rather than at the mark, and a head that
             * gave up in that window would give up on every shot taken from
             * anything but dead astern. */
            o->target.kind = FLY_ORD_ACTOR_NONE;
            o->target.id = 0;
        }
    }
    ti = contact_index(contacts, contact_count, o->target);

    /* --- forces -------------------------------------------------------- */
    speed = fly_v3len(o->vel);
    {
        fly_v3 acc = fly_v3zero();
        if (fly_ord_is_ballistic(o->kind)) {
            acc.z -= 9.80665f;
            /* And the light things stop, fast, and then go where the air
             * goes. A flare is a lump of burning metal with no shape to it: it
             * sheds the aeroplane's speed in about a second and then hangs and
             * falls, which is the whole of how a decoy separates from the
             * aircraft that dropped it. Coupled gently it stayed in formation
             * with the aeroplane and the seeker arrived at both, missing by
             * seventeen metres — which is not a countermeasure. */
            if (o->kind == FLY_ORD_FLARE || o->kind == FLY_ORD_CHAFF)
                acc = fly_v3add(acc, fly_v3scale(fly_v3sub(wind, o->vel), 2.2f));
        } else if (guided && ti >= 0) {
            fly_v3 aim = contacts[ti].pos;
            /* Jamming, applied where a jammer actually works: on the head's
             * idea of where the target is, not on whether the round exists.
             * The error is thrown along the target's own track and swings with
             * the round's age, so it is a wobble the missile chases rather than
             * a constant bias it can trim out — and what a strong jammer buys
             * is a miss distance measured in tens of metres, which a warhead
             * with a blast radius still partly collects. Derived from the age
             * rather than drawn, so it costs no random numbers. */
            if (contacts[ti].ecm > 0.0f) {
                float sway = sinf(o->age * 2.3f) * 34.0f * contacts[ti].ecm;
                aim = fly_v3add(aim, fly_v3scale(fly_v3norm(contacts[ti].vel), sway));
            }
            acc = guidance_accel(o, aim, contacts[ti].vel);
            acc.z += 9.80665f;   /* it holds its own weight on the fins */
        } else if (!fly_ord_is_ballistic(o->kind)) {
            /* Unguided, or guided with nothing to guide on: it flies the line
             * it was pointed down, sagging as it slows. */
            acc.z -= 9.80665f * (o->burn > 0.0f ? 0.15f : 0.65f);
        }
        /* The motor drives it *toward* its top speed rather than shoving at a
         * fixed rate: a constant push accelerated a three-second burn to four
         * hundred metres a second, which made a "slow heat seeker" the fastest
         * thing in the world and quietly deleted out-running it as an answer.
         * First order, so it arrives at the number and stays there. */
        if (o->burn > 0.0f && speed > 0.1f) {
            fly_v3 fwd = fly_v3scale(o->vel, 1.0f / speed);
            float push = fly_clampf((o->vmax - speed) * 1.4f, 0.0f, 220.0f);
            acc = fly_v3add(acc, fly_v3scale(fwd, push));
        }
        o->vel = fly_v3add(o->vel, fly_v3scale(acc, dt));
    }
    /* Drag, and the reason the whole thing is survivable. A coasting round is
     * spending energy it has no way of replacing, so every second after
     * burnout it can pull less and reach less far — and every g the target
     * pulls costs the round more than it costs the aeroplane. */
    speed = fly_v3len(o->vel);
    if (speed > 0.1f) {
        float loss = k->drag * speed * speed * dt;
        float s = fly_clampf(1.0f - loss / speed, 0.0f, 1.0f);
        o->vel = fly_v3scale(o->vel, s);
        /* And what it can still pull goes with the speed it still has, which is
         * what turns "out-turn it" from a hope into a plan. */
        if (guided && o->vmax > 1.0f) {
            float f = fly_clampf(fly_v3len(o->vel) / (o->vmax * 0.75f), 0.15f, 1.0f);
            o->turn = k->turn * f;
        }
    }

    /* --- and where that puts it ---------------------------------------- */
    {
        fly_v3 prev = o->pos;
        o->pos = fly_v3add(o->pos, fly_v3scale(o->vel, dt));
        gz = ground ? ground(guser, o->pos.x, o->pos.y) : -1.0e9f;
        if (o->pos.z <= gz) {
            o->pos.z = gz;
            return -2;   /* the ground: a bomb's whole purpose, everything else's end */
        }
        /* Proximity and impact, against everything rather than only the mark.
         * A round is dangerous to whatever it actually reaches, which is what
         * makes flying through somebody else's missile a thing that can happen.
         * Stepped against the segment travelled rather than against the
         * endpoint, because a dart at four hundred metres a second crosses
         * seven metres in a sixtieth of a second and would otherwise pass
         * clean through the aeroplane it was aimed at. */
        if (o->armed && !fly_ord_is_ballistic(o->kind)) {
            for (i = 0; i < contact_count; ++i) {
                const fly_ord_contact *c = &contacts[i];
                fly_v3 seg, rel;
                float t, miss, reach;
                if (c->ref.kind == o->owner.kind && c->ref.id == o->owner.id) continue;
                if (c->faction == o->faction && c->faction != 0) continue;
                /* A decoy is not a thing you can hit. A seeker that has taken
                 * the bait flies through the flare and out the other side with
                 * its energy spent, which is a better outcome than detonating
                 * on it: the round is wasted either way, and this way the
                 * player watches it go past. */
                if (c->radius <= 0.0f) continue;
                seg = fly_v3sub(o->pos, prev);
                rel = fly_v3sub(c->pos, prev);
                t = fly_v3dot(seg, seg) > 1e-6f
                        ? fly_clampf(fly_v3dot(rel, seg) / fly_v3dot(seg, seg), 0.0f, 1.0f)
                        : 0.0f;
                miss = fly_v3dist(fly_v3add(prev, fly_v3scale(seg, t)), c->pos);
                reach = c->radius + o->blast * 0.30f;
                if (miss <= reach) return i;
            }
        }
    }
    return -1;
}

float fly_ord_blast_damage(const fly_ord *o, float dist) {
    float t;
    if (!o || o->blast <= 0.0f) return 0.0f;
    if (dist >= o->blast) return 0.0f;
    /* Full at the centre, nothing at the rim, and squared in between so a near
     * miss is a near miss rather than a clean escape — the shape that makes a
     * bomb worth aiming and a proximity fuse worth setting. */
    t = 1.0f - dist / o->blast;
    return o->damage * t * t;
}

/* ---------------- the launch decision ---------------- */

int fly_ord_launch_ok(int kind, float range, fly_v3 from, fly_v3 vel,
                      fly_v3 target, fly_v3 target_vel) {
    fly_v3 to = fly_v3sub(target, from);
    float d = fly_v3len(to), cosang;
    const fly_ord_class *k = fly_ord_class_get(kind);
    if (!k || d > range || d < 1.0f) return 0;
    (void)target_vel;
    /* A bomb is not fired at anything: it is released above it, and the only
     * question is whether it can reach. Everything else has to be pointed. */
    if (fly_ord_is_ballistic(kind)) return from.z > target.z + 20.0f;
    if (fly_v3len(vel) < 1.0f) return 0;
    cosang = fly_v3dot(fly_v3norm(to), fly_v3norm(vel));
    /* Inside the fuse's arming distance the round is a dud, so a shot taken
     * there is a wasted round rather than a close one. */
    if (d < k->speed * k->arm_time) return 0;
    if (fly_ord_is_heat(kind) || fly_ord_is_radar(kind))
        return cosang > cosf(k->seek_cone * 0.8f);
    return cosang > cosf(0.22f);   /* unguided: about twelve degrees */
}

int fly_ord_impact_point(int kind, fly_v3 from, fly_v3 vel, fly_ground_fn ground,
                         void *guser, float max_t, fly_v3 *out) {
    const fly_ord_class *k = fly_ord_class_get(kind);
    fly_v3 p = from, v = vel;
    float t;
    if (!k || !out) return 0;
    /* The same integration the round itself flies, at a coarser step: a
     * bombsight that solved a different trajectory from the bomb would be a
     * bombsight that lies, and the one thing a pipper has to be is honest. */
    for (t = 0.0f; t < max_t; t += 0.05f) {
        float speed;
        v.z -= 9.80665f * 0.05f;
        speed = fly_v3len(v);
        if (speed > 0.1f) {
            float loss = k->drag * speed * speed * 0.05f;
            v = fly_v3scale(v, fly_clampf(1.0f - loss / speed, 0.0f, 1.0f));
        }
        p = fly_v3add(p, fly_v3scale(v, 0.05f));
        if (ground && p.z <= ground(guser, p.x, p.y)) {
            p.z = ground(guser, p.x, p.y);
            *out = p;
            return 1;
        }
    }
    return 0;
}
