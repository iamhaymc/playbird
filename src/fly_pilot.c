#include "fly_pilot.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Turnaround on a pad, seconds of game time. Long enough that a site has a
 * plausible number of aircraft sitting on it at any moment rather than a
 * constant churn of touch-and-goes. */
#define FLY_PILOT_TURNAROUND 90.0f
/* How close a pilot has to be to a pad to call itself arrived. Matches the
 * player's docking reach closely enough that both see the same world. */
#define FLY_PILOT_DOCK_R 260.0f
/* Cruise for the coarse simulation. Real enough that an ETA computed from it
 * matches roughly what the full simulation would take over the same leg. */
#define FLY_PILOT_CRUISE 62.0f
#define FLY_PILOT_CRUISE_ALT 620.0f
/* Grip earned per token turned over at a pad. Set so that a good delivery —
 * five or six hundred tokens of alloy — is worth about a fifteenth of what it
 * takes to prise a site loose, which puts a takeover at a dozen or so
 * deliveries against an owner who is doing none. That is hours of world for
 * one site, and it should be: a chart that redraws itself over a coffee break
 * is not a map, it is weather. */
#define FLY_TRADE_PUSH (1.0f / 9000.0f)

/* Where on the apron a pilot parks. Aircraft that all dock to the pad centre
 * end up inside one another, and a site with four visitors looks like a site
 * with one; a ring just outside the deck, indexed by the pilot's own id, reads
 * as parking and costs a sine. The deck itself stays clear for the player. */
static fly_wpos park_spot(const fly_location *L, uint32_t id) {
    float a = (float)(id * 2654435761u % 3600u) * 0.001745329f;
    float r = FLY_PAD_DECK_R + 22.0f + (float)(id % 3) * 16.0f;
    return fly_wpos_step(L->pos, fly_v2mk(cosf(a) * r, sinf(a) * r));
}

static const char *k_names[FLY_PILOT_KIND_COUNT] = {
    "Trader", "Courier", "Patrol", "Pirate", "Prospector", "Saucer"
};
static const char *t_names[FLY_TASK_COUNT] = {
    "idle", "trading", "service", "transit", "hunting", "evading", "survey"
};
static const char *c_names[FLY_PCMD_KIND_COUNT] = {
    "none", "controls", "launch", "dock", "buy", "sell", "refuel", "repair", "leg"
};

const char *fly_pilot_kind_name(fly_pilot_kind k) {
    return (unsigned)k < FLY_PILOT_KIND_COUNT ? k_names[k] : "?";
}
const char *fly_pilot_task_name(fly_pilot_task t) {
    return (unsigned)t < FLY_TASK_COUNT ? t_names[t] : "?";
}
const char *fly_pilot_cmd_name(fly_pilot_cmd_kind k) {
    return (unsigned)k < FLY_PCMD_KIND_COUNT ? c_names[k] : "?";
}

float fly_pilot_cargo_mass(const fly_pilot *p) {
    float m = 0.0f;
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r)
        m += p->cargo[r] * fly_resource_mass((fly_resource)r);
    return m;
}

float fly_pilot_gun_solution(const fly_craft *shooter, const fly_airframe *af,
                             const fly_craft *target) {
    fly_v3 to, fwd;
    float d, cosang;
    if (af->gun_dps <= 0.0f) return 0.0f;
    to = fly_v3sub(target->pos, shooter->pos);
    d = fly_v3len(to);
    if (d > af->gun_range || d < 1.0f) return 0.0f;
    fwd = fly_qrot(shooter->ori, fly_v3mk(1, 0, 0));
    cosang = fly_v3dot(fly_v3norm(to), fwd);
    /* A beam is a different weapon wearing the gun's interface.
     *
     * There is no time of flight worth modelling, so there is no lead to get
     * wrong and no falloff with range: it either bears or it does not, and when
     * it bears it is doing full damage at the far end of its reach. What it
     * pays is the cone — a third the width of a gun's, so it has to be aimed
     * rather than sprayed — and the heat, which the trigger charges for. */
    if (af->beam_heat > 0.0f) return cosang >= 0.9981f ? 1.0f : 0.0f;  /* ~3.5 deg */
    if (cosang < 0.985f) return 0.0f; /* ~10 degree cone */
    return (cosang - 0.985f) / 0.015f * (1.0f - d / (af->gun_range * 1.2f));
}

/* --- something is coming ---------------------------------------------------
 *
 * Closing, pointed roughly at me, and near enough to matter. Deliberately
 * ignorant of what the round is actually chasing: a pilot has eyes and a
 * warning tone, not the missile's target register, so it will occasionally
 * break for one that was never meant for it. That is the right behaviour — a
 * cheap decoy spent on a scare is exactly what a countermeasure is for — and
 * making it clairvoyant would be the kind of AI advantage the whole pilot
 * module exists to avoid.
 *
 * Time to impact rather than distance, because that is the quantity a decision
 * is made on: at four seconds you turn, at one you have already lost. */
float fly_pilot_ord_threat(const fly_pilot *p, const fly_ord *o) {
    fly_v3 to;
    float d, closing, bearing;
    if (!p || !o || !o->active) return -1.0f;
    if (o->damage <= 0.0f) return -1.0f;               /* a flare is not a threat */
    if (o->faction == p->faction && o->faction != 0) return -1.0f;
    if (o->owner.kind == FLY_ORD_ACTOR_PILOT && o->owner.id == p->id) return -1.0f;
    to = fly_v3sub(p->craft.pos, o->pos);
    d = fly_v3len(to);
    if (d < 1.0f || d > 2600.0f) return -1.0f;
    to = fly_v3scale(to, 1.0f / d);
    closing = fly_v3dot(o->vel, to);
    if (closing < 20.0f) return -1.0f;                 /* it is not coming here */
    /* Pointed at me: a round crossing the sky in front is somebody else's
     * problem and not worth a flare. Twenty-five degrees is generous, because
     * a seeker that is turning onto you looks off-bearing right up until it
     * is not. */
    bearing = fly_v3dot(fly_v3norm(o->vel), to);
    if (bearing < 0.90f) return -1.0f;
    return d / closing;
}

/* ---------------- naming ----------------
 *
 * Callsigns are drawn from the seed rather than stored, so a save file does not
 * carry a table of strings and two machines agree on who is who. */

static void pilot_name(fly_pilot *p, fly_rng *rng) {
    static const char *stem[16] = {
        "Kestrel", "Halcyon", "Tarpon", "Vireo", "Mirage", "Anvil", "Corvid",
        "Selkie", "Marlin", "Pallas", "Osprey", "Nimbus", "Falco", "Aster",
        "Rook", "Sable"
    };
    unsigned s = (unsigned)fly_rng_range(rng, 16);
    unsigned n = (unsigned)fly_rng_range(rng, 90) + 10;
    snprintf(p->name, sizeof p->name, "%s %u", stem[s], n);
}

/* ---------------- spawning ---------------- */

void fly_pilot_spawn(fly_pilot *p, uint32_t id, fly_pilot_kind kind, int home,
                     const fly_world *world, fly_rng *rng) {
    const fly_location *L;
    memset(p, 0, sizeof *p);
    p->active = 1;
    p->id = id;
    p->kind = kind;
    p->home = home;
    p->origin = home;
    p->dest = home;
    p->deal_res = -1;
    p->deal_origin = -1;
    /* Whose aircraft this is: whoever holds the field it flies out of.
     *
     * Outlaws are nobody's employee and always Corsairs. A patrol out of
     * unaligned ground answers to the Concord, because a police force with no
     * power behind it is not one — and the alternative, an unaligned patrol,
     * would be an armed aircraft nobody could be at war with, which is a hole
     * in the middle of the whole system. */
    p->faction = kind == FLY_PILOT_PIRATE ? FLY_FACTION_CORSAIR
               : home >= 0 && home < world->nloc ? (int)world->loc[home].owner
                                                 : FLY_FACTION_FREE;
    if (kind == FLY_PILOT_PATROL && p->faction == FLY_FACTION_FREE)
        p->faction = FLY_FACTION_CONCORD;
    /* Whatever is in the saucers did not sign anything. Unaligned is the
     * closest this vocabulary comes to "not a party to any of this", and it
     * keeps them out of the war on both sides: they start no fights, and no
     * power's quarrel with another makes one of them your problem. */
    if (kind == FLY_PILOT_UFO) p->faction = FLY_FACTION_FREE;
    pilot_name(p, rng);

    fly_airframe_default_plane(&p->airframe);
    snprintf(p->airframe.id, sizeof p->airframe.id, "pilot");
    switch (kind) {
    case FLY_PILOT_TRADER:
        snprintf(p->airframe.name, sizeof p->airframe.name, "Drover");
        p->airframe.cargo_cap *= 1.8f;
        p->airframe.fuel_cap *= 1.3f;
        p->airframe.visual_mask |= FLY_VIS_CARGOPOD;
        p->tokens = 900 + (long)fly_rng_range(rng, 900);
        break;
    case FLY_PILOT_COURIER:
        snprintf(p->airframe.name, sizeof p->airframe.name, "Runner");
        p->airframe.thrust_max *= 1.15f;
        p->airframe.visual_mask |= FLY_VIS_LONGWING;
        p->tokens = 500 + (long)fly_rng_range(rng, 600);
        break;
    case FLY_PILOT_PATROL:
        snprintf(p->airframe.name, sizeof p->airframe.name, "Marshal");
        p->airframe.gun_dps = 11.0f;
        p->airframe.gun_range = 820.0f;
        p->airframe.structure *= 1.5f;
        p->airframe.armor = 0.25f;
        p->airframe.visual_mask |= FLY_VIS_GUNPOD | FLY_VIS_SLATS;
        p->tokens = 700;
        break;
    case FLY_PILOT_PIRATE:
        snprintf(p->airframe.name, sizeof p->airframe.name, "Vulture");
        p->airframe.gun_dps = 8.0f;
        p->airframe.gun_range = 700.0f;
        p->airframe.structure *= 0.85f;
        p->airframe.thrust_max *= 1.1f;
        p->airframe.visual_mask |= FLY_VIS_GUNPOD;
        p->tokens = 120 + (long)fly_rng_range(rng, 300);
        break;
    case FLY_PILOT_UFO:
        /* Not built, and not pretending to be. Weightless, so it hangs where
         * it is rather than flying a trajectory; sealed and shielded, so the
         * altitude that kills everything else is simply where it lives; and
         * armed well enough that arriving in a converted hauler with a hundred
         * metres a second of manoeuvring left is a real fight. The gun is
         * short-ranged on purpose — it has to close, which is what gives a
         * spacecraft with almost no delta-v something to do about it. */
        /* Not a person. Whatever is aboard did not file a flight plan, so it
         * gets the designation the radar gave it rather than the name generator
         * every other pilot draws from. */
        snprintf(p->name, sizeof p->name, "Contact %02u", (unsigned)(id % 100u));
        snprintf(p->airframe.name, sizeof p->airframe.name, "Saucer");
        p->airframe.mass = 1800.0f;
        p->airframe.antigrav = 1.0f;
        p->airframe.rocket_thrust = 26000.0f;
        p->airframe.rocket_ve = 9.0e5f;   /* effectively never runs out */
        p->airframe.propellant_cap = 4000.0f;
        p->airframe.rcs_auth = 2.4f;
        p->airframe.pressure = 1.0f;
        p->airframe.heat_shield = 1.0f;
        p->airframe.structure = 520.0f;
        p->airframe.armor = 0.45f;
        p->airframe.gun_dps = 26.0f;
        p->airframe.gun_range = 620.0f;
        p->airframe.vne = 4000.0f;
        p->airframe.cd0 = 0.5f;           /* a disc: it does not want to be in air */
        /* Twenty-two metres across. `wing_span` is not in any of the flight
         * equations — `wing_area` is — so on something with no wings it is
         * exactly one thing: the size the renderer draws it and the radius the
         * culler keeps it on screen for. Twice a Skylark, which is what makes
         * it read as large rather than as near. */
        p->airframe.wing_span = 22.0f;
        p->airframe.radar_range = 40000.0f;
        p->tokens = 0;
        break;
    default:
        snprintf(p->airframe.name, sizeof p->airframe.name, "Sounder");
        p->airframe.fuel_cap *= 1.6f;
        p->airframe.radar_range = 5200.0f;
        p->airframe.visual_mask |= FLY_VIS_DROPTANK | FLY_VIS_LONGWING;
        p->tokens = 400 + (long)fly_rng_range(rng, 400);
        break;
    }

    /* What the power it flies for does to the aeroplane, and it is deliberately
     * only skin.
     *
     * The first version of this leaned the numbers too — Concord structure,
     * Foundry mass and cargo, Corsair thrust — and it had to come out. Not
     * because any of it was wrong in itself, but because it moved combat
     * outcomes, and a world whose fights resolve differently is a *different
     * world*: `game.autopilot` flies thirteen routes on seed 4242 and one of
     * them was already surviving a pirate on 24 of 100 structure. Nudging the
     * fleet's numbers by a tenth killed it. That is chaotic divergence rather
     * than a lethality increase, which makes it worse, not better — it means
     * any future tuning here reshuffles gates that have nothing to do with
     * factions.
     *
     * The visual mask changes nothing the simulation reads, so the fleet flies
     * exactly the world it flew before factions existed and still separates
     * into seven silhouettes at the range where a long wing or a cargo pod is
     * what the eye actually picks out. Faction *performance* is delivered
     * where it belongs and where it is opt-in: the seven airframes and
     * twenty-one modules an operator can go and buy. */
    switch (p->faction) {
    case FLY_FACTION_CONCORD: p->airframe.visual_mask |= FLY_VIS_ANTENNA; break;
    case FLY_FACTION_FOUNDRY:
        p->airframe.visual_mask |= FLY_VIS_CARGOPOD | FLY_VIS_ARMOR; break;
    case FLY_FACTION_LANTERN: p->airframe.visual_mask |= FLY_VIS_LONGWING; break;
    case FLY_FACTION_HALCYON: p->airframe.visual_mask |= FLY_VIS_DROPTANK; break;
    case FLY_FACTION_CINDER:
        p->airframe.visual_mask |= FLY_VIS_SLATS | FLY_VIS_ARMOR; break;
    case FLY_FACTION_MERIDIAN: p->airframe.visual_mask |= FLY_VIS_RADARDOME; break;
    /* The Corsairs add nothing. Every outlaw in the world is one, and a mark
     * that every pirate carries is a mark that says "pirate" — which the
     * livery already says, in red. */
    default: break;
    }

    /* Rank, off the danger of the ground this pilot works. It buys a modest
     * amount of aeroplane — enough that a tier 3 outlaw is a fight and not a
     * different game — and a great deal of drop. The asymmetry is the point:
     * you are hunting what they are carrying, not their hit points. */
    {
        float dgr = fly_world_danger(world, world->loc[home].pos.e, world->loc[home].pos.n);
        float roll = fly_rng_f01(rng);
        /* Rank is meant to be a tail, not a gradient. Each step up needs the
         * country *and* the draw: a tier 3 wants dangerous ground and the top
         * few per cent of the roll, so an ace is something you meet rather
         * than something the far half of the map is made of. The one thing
         * worth having comes off the one aircraft you rarely see. */
        int t = 0;
        if (roll < dgr * 0.85f) ++t;
        if (roll < dgr * 0.34f) ++t;
        if (roll < dgr * dgr * 0.10f) ++t;
        if (kind == FLY_PILOT_PIRATE && t && roll < dgr * 0.12f) ++t;
        p->tier = (unsigned char)(t < 0 ? 0 : t > 3 ? 3 : t);
        /* An ace you cannot see coming is a trader that killed you, so from
         * rank two the name carries it. */
        /* Built by hand rather than by a format, because the compiler cannot
         * prove a truncating snprintf into a buffer it also read from. */
        if (p->tier >= 2) {
            const char *pre = p->tier >= 3 ? "" : "Old ";
            const char *suf = p->tier >= 3 ? " the Ace" : "";
            char out[sizeof p->name];
            size_t at = 0, k;
            for (k = 0; pre[k] && at + 1 < sizeof out; ++k) out[at++] = pre[k];
            for (k = 0; p->name[k] && at + strlen(suf) + 1 < sizeof out; ++k) out[at++] = p->name[k];
            for (k = 0; suf[k] && at + 1 < sizeof out; ++k) out[at++] = suf[k];
            out[at] = 0;
            memcpy(p->name, out, sizeof p->name);
        }
        if (p->tier) {
            float k = (float)p->tier;
            p->airframe.structure *= 1.0f + 0.15f * k;
            p->airframe.armor = fly_clampf(p->airframe.armor + 0.06f * k, 0.0f, 0.6f);
            if (p->airframe.gun_dps > 0.0f) p->airframe.gun_dps *= 1.0f + 0.22f * k;
            p->tokens += (long)(180 * k);
        }
    }

    /* --- what it is carrying to defend itself with -------------------------
     *
     * Countermeasures first, and for everybody, which is the important half.
     * The world now has missiles in it, and a fleet of haulers that could be
     * shot at by them but could do nothing about them would not be a harder
     * world — it would be a world where the traffic quietly dies and the
     * economy the whole game runs on goes with it. A dispenser is cheap, it is
     * the one piece of kit a courier would actually buy, and it is what makes
     * an outlaw's launch a thing that sometimes fails.
     *
     * Launchers second, and only for those whose job is fighting. Ranks buy
     * magazine rather than a bigger warhead: an ace is somebody who can keep
     * taking shots, not somebody whose missiles hurt more, and scaling the
     * damage would be the sort of quiet lethality change the faction block
     * above records as having had to be reverted.
     *
     * Deliberately thin. Two rounds and six flares is enough that a pilot uses
     * them, has to choose when, and runs out — which is what keeps the sky a
     * place where guns still decide most fights. */
    {
        int rank = (int)p->tier;
        if (fly_pilot_is_combatant(p)) {
            p->airframe.cm_kind = FLY_ORD_FLARE;
            p->airframe.cm_ammo = 6 + 3 * rank;
        } else if (p->kind != FLY_PILOT_UFO) {
            /* Freight gets fewer, and gets them at all only because a trader
             * with no answer to a seeker is freight the world stops having. */
            p->airframe.cm_kind = FLY_ORD_FLARE;
            p->airframe.cm_ammo = 4 + 2 * rank;
        }
        if (p->kind == FLY_PILOT_PATROL) {
            /* The law carries the patient weapon: a seeker off a Marshal is
             * what turns an outlaw's run for the horizon into a decision. */
            p->airframe.ord_kind = FLY_ORD_SEEKER;
            p->airframe.ord_range = 2000.0f;
            p->airframe.ord_damage = 1.0f;
            p->airframe.ord_ammo = 1 + rank;
            p->airframe.visual_mask |= FLY_VIS_MISSILE | FLY_VIS_DISPENSER;
        } else if (p->kind == FLY_PILOT_PIRATE) {
            /* And the outlaws carry the cheap one, fired at knife range at
             * something that is already trying to get away. */
            p->airframe.ord_kind = FLY_ORD_ROCKET;
            p->airframe.ord_range = 1100.0f;
            p->airframe.ord_damage = 1.0f;
            p->airframe.ord_ammo = 8 + 4 * rank;
            p->airframe.visual_mask |= FLY_VIS_ROCKETPOD | FLY_VIS_DISPENSER;
        } else if (p->kind == FLY_PILOT_UFO) {
            /* It does not carry ordnance and does not need to. What it has
             * instead is the one weapon in the world with no time of flight —
             * which is the correct shape for something that was not built the
             * way anything else here was built, and it is why closing with one
             * is a different problem from closing with an aeroplane. */
            p->airframe.beam_heat = 0.12f;
            p->airframe.visual_mask |= FLY_VIS_EMITTER;
        } else if (p->airframe.cm_ammo > 0) {
            p->airframe.visual_mask |= FLY_VIS_DISPENSER;
        }
    }

    L = &world->loc[home];
    if (kind == FLY_PILOT_UFO) {
        /* Above the line, over open country, and already airborne. A saucer
         * has no pad to spawn on and no leg to fly: it is placed where it will
         * be found, which is somewhere nobody can currently get to. */
        float ang = fly_rng_f01(rng) * 2.0f * FLY_PI;
        float rad = fly_rng_span(rng, 0.0f, FLY_WORLD_HALF * 0.55f);
        fly_craft_init(&p->craft, &p->airframe,
                       fly_v3mk(cosf(ang) * rad, sinf(ang) * rad,
                                fly_rng_span(rng, FLY_KARMAN * 1.15f, FLY_KARMAN * 1.9f)));
        p->craft.on_ground = 0;
        p->craft.ori = fly_qeuler(0.0f, 0.0f, ang);
        p->task = FLY_TASK_SURVEY;
        p->tier = 3;
        p->think_in = fly_rng_span(rng, 0.2f, 1.5f);
        p->craft.fuel = p->airframe.fuel_cap;
        p->craft.hp = p->airframe.structure;
        return;
    }
    {
        fly_wpos spot = park_spot(L, id);
        fly_craft_init(&p->craft, &p->airframe,
                       fly_wpos_at(spot, L->pad_z + p->airframe.gear_height));
    }
    p->craft.on_ground = 1;
    p->craft.fuel = p->airframe.fuel_cap;
    p->craft.hp = p->airframe.structure;
    p->task = FLY_TASK_IDLE;
    p->pad_until = fly_rng_span(rng, 5.0f, FLY_PILOT_TURNAROUND);
    p->think_in = fly_rng_span(rng, 0.2f, 1.5f);
}

/* ---------------- deciding ----------------
 *
 * Everything below reads and returns; nothing here writes to the world, the
 * pilot, or anything reachable from either. */

static float leg_km(const fly_world *w, int a, int b) {
    return fly_world_dist(w->loc[a].pos, w->loc[b].pos) * 0.001f;
}

/* How far this airframe can still fly, in km, with a reserve held back. */
static float range_km(const fly_pilot *p) {
    float burn = p->airframe.burn_rate > 1e-4f ? p->airframe.burn_rate : 0.01f;
    float endurance = (p->craft.fuel * 0.82f) / (burn * 0.72f); /* s at cruise */
    return endurance * FLY_PILOT_CRUISE * 0.001f;
}

/* The best buy-here-sell-there this pilot can reach and afford. Returns the
 * expected margin in tokens, 0 if nothing is worth flying. */
static long best_trade(const fly_pilot *p, const fly_world *w, int here,
                       int *out_res, float *out_qty, int *out_dest) {
    long best = 0;
    float reach = range_km(p);
    int to, r;
    for (to = 0; to < w->nloc; ++to) {
        if (to == here) continue;
        if (!w->loc[to].discovered) continue;
        {
            float km = leg_km(w, here, to);
            if (km > reach) continue;
            for (r = 0; r < FLY_RES_COUNT; ++r) {
                float buy = fly_world_price(w, here, (fly_resource)r);
                float sell = fly_world_price(w, to, (fly_resource)r);
                float have = w->loc[here].stock[r];
                float room = w->loc[to].target[r] - w->loc[to].stock[r];
                float cap = p->airframe.cargo_cap / fly_resource_mass((fly_resource)r);
                float qty;
                long margin;
                if (sell <= buy * 1.12f) continue;   /* not worth the fuel */
                if (have < 6.0f || room < 4.0f) continue;
                qty = have * 0.35f;
                if (qty > room * 0.8f) qty = room * 0.8f;
                if (qty > cap) qty = cap;
                if (buy > 0.01f && (float)p->tokens / buy < qty) qty = (float)p->tokens / buy;
                qty = floorf(qty);
                if (qty < 3.0f) continue;
                margin = (long)((sell - buy) * qty) - (long)(km * 1.4f);
                if (margin > best) {
                    best = margin;
                    *out_res = r;
                    *out_qty = qty;
                    *out_dest = to;
                }
            }
        }
    }
    return best;
}

/* The site most short of something this pilot could carry there: a courier's
 * job board, drawn from the same shortage signal the player's contracts are. */
static long best_haul(const fly_pilot *p, const fly_world *w, int here,
                      int *out_res, float *out_qty, int *out_dest) {
    long best = 0;
    float reach = range_km(p);
    int to, r;
    for (to = 0; to < w->nloc; ++to) {
        if (to == here || !w->loc[to].discovered) continue;
        {
            float km = leg_km(w, here, to);
            if (km > reach) continue;
            for (r = 0; r < FLY_RES_COUNT; ++r) {
                float need = w->loc[to].target[r] - w->loc[to].stock[r];
                float have = w->loc[here].stock[r];
                float cap = p->airframe.cargo_cap / fly_resource_mass((fly_resource)r);
                float qty;
                long fee;
                if (need < 8.0f || w->loc[to].cons[r] <= 0.0f) continue;
                if (have < need * 0.5f) continue;   /* the source has to have it */
                qty = need * 0.5f;
                if (qty > cap) qty = cap;
                if (qty > have * 0.4f) qty = have * 0.4f;
                qty = floorf(qty);
                if (qty < 3.0f) continue;
                fee = (long)(qty * fly_world_price(w, to, (fly_resource)r) * 0.55f +
                             km * 2.0f);
                if (fee > best) {
                    best = fee;
                    *out_res = r;
                    *out_qty = qty;
                    *out_dest = to;
                }
            }
        }
    }
    return best;
}

/* Somewhere worth looking at: undiscovered first, then simply far from home. */
static int survey_target(const fly_pilot *p, const fly_world *w, fly_rng *rng) {
    int i, pick = -1, seen = 0;
    float reach = range_km(p);
    for (i = 0; i < w->nloc; ++i) {
        if (i == p->home) continue;
        if (leg_km(w, p->home, i) > reach) continue;
        if (w->loc[i].discovered) continue;
        /* reservoir sample, so the choice does not favour a low index */
        if (fly_rng_range(rng, (uint32_t)(++seen)) == 0) pick = i;
    }
    if (pick >= 0) return pick;
    for (i = 0; i < w->nloc; ++i) {
        if (i == p->home) continue;
        if (leg_km(w, p->home, i) > reach) continue;
        if (fly_rng_range(rng, (uint32_t)(++seen)) == 0) pick = i;
    }
    return pick;
}

/* How a fight between these two would go, as 0..1 for the first one.
 *
 * This replaces a flat `worth *= 0.35 if the mark has a gun`, which treated a
 * lightly armed courier and an armoured patrol with half again the structure as
 * the same problem, and never once asked what the attacker was bringing. Time
 * to kill each way, and the share of it that belongs to me: one means it cannot
 * touch me, a half is an even fight, and below that I am the one being hunted.
 * Armour enters where the combat step applies it, as a divisor on damage. */
static float fight_edge(float my_dps, float my_hp, float my_armor,
                        float foe_dps, float foe_hp, float foe_armor) {
    float mine = foe_hp / (1.0f - fly_clampf(foe_armor, 0.0f, 0.9f)) /
                 (my_dps > 0.01f ? my_dps : 0.01f);       /* seconds to kill it */
    float theirs = my_hp / (1.0f - fly_clampf(my_armor, 0.0f, 0.9f)) /
                   (foe_dps > 0.01f ? foe_dps : 0.01f);   /* seconds to kill me */
    return theirs / (mine + theirs + 1e-6f);
}

/* Is this pilot in any shape to start something? */
static float fit_to_fight(const fly_pilot *p) {
    float hp = p->craft.hp / (p->airframe.structure + 1e-3f);
    float fuel = p->craft.fuel / (p->airframe.fuel_cap + 1e-3f);
    if (p->airframe.gun_dps <= 0.0f) return 0.0f;
    return fly_clampf((hp - 0.30f) / 0.45f, 0.0f, 1.0f) *
           fly_clampf((fuel - 0.22f) / 0.30f, 0.0f, 1.0f) *
           (1.0f - fly_clampf(p->craft.wear, 0.0f, 0.6f));
}

int fly_pilot_is_combatant(const fly_pilot *p) {
    if (!p) return 0;
    if (p->kind == FLY_PILOT_PIRATE || p->kind == FLY_PILOT_PATROL) return 1;
    return p->airframe.gun_dps > 0.0f && p->kind != FLY_PILOT_UFO;
}

/* Is `q` fair game for `p`? One predicate, and everything that decides whether
 * a trigger is pulled goes through it.
 *
 * Two rules, and the second one is what keeps a seven-power war from emptying
 * the sky. An outlaw robs anyone it is not at terms with — that is the job.
 * Everybody else fights *combatants*: an armed aircraft of a power theirs is
 * at war with, or an outlaw. A Concord patrol will not machine-gun a Foundry
 * courier over a border dispute, which is both the decent reading and the one
 * that leaves anybody alive to trade with. */
static int prey_ok(const fly_pilot_view *v, const fly_pilot *p, const fly_pilot *q) {
    if (!v->world || p->faction == q->faction) return 0;
    if (q->faction == FLY_FACTION_FREE && p->kind != FLY_PILOT_PIRATE) return 0;
    if (fly_diplomacy_state(&v->world->dip, p->faction, q->faction) >= FLY_REL_PACT)
        return 0;
    if (p->kind == FLY_PILOT_PIRATE) return 1;
    /* Piracy is a crime, not a foreign policy. Anybody armed will go after an
     * outlaw whatever the relation between their two flags says — and that has
     * to be structural rather than left to the matrix, or a seed where the
     * Concord and the Corsairs happened to roll *cold* rather than at war
     * would be a seed with no police in it. A power that is at outright terms
     * with them is the one exception, and it is caught by the pact check
     * above: that is a criminal arrangement and it should look like one. */
    if (q->kind == FLY_PILOT_PIRATE) return fly_pilot_is_combatant(p);
    if (!fly_faction_hostile(v->world, p->faction, q->faction)) return 0;
    return fly_pilot_is_combatant(q);
}

/* The same question about the player, who is not a `fly_pilot` yet and so
 * cannot be handed to `prey_ok`. An unaligned operator is what the world has
 * always been able to see — a hull with cargo in it — so outlaws hunt them and
 * nobody else does. Wearing a flag changes that in both directions: it puts
 * that power's enemies onto you, and it takes their friends off. */
static int player_prey_ok(const fly_pilot_view *v, const fly_pilot *p) {
    if (!v->world) return 0;
    if (p->faction == v->player_faction && v->player_faction != FLY_FACTION_FREE) return 0;
    if (v->player_faction != FLY_FACTION_FREE &&
        fly_diplomacy_state(&v->world->dip, p->faction, v->player_faction) >= FLY_REL_PACT)
        return 0;
    if (p->kind == FLY_PILOT_PIRATE) return 1;
    return fly_faction_hostile(v->world, p->faction, v->player_faction);
}

/* The fattest reachable mark for an outlaw: loaded, airborne, and worth the
 * risk of picking the fight rather than merely capable of being caught. */
static int hunt_mark(const fly_pilot *p, const fly_pilot_view *v,
                     uint32_t *out_id, int *out_player) {
    float best = 0.0f;
    float fit = fit_to_fight(p);
    int i, found = 0;
    *out_id = 0;
    *out_player = 0;
    if (fit <= 0.05f) return 0;   /* shot up, nearly dry, or unarmed: go home */
    for (i = 0; i < v->peer_count; ++i) {
        const fly_pilot *q = &v->peers[i];
        float d, worth, edge;
        if (!q->active || q->id == p->id) continue;
        if (!prey_ok(v, p, q)) continue;
        if (q->craft.on_ground) continue;      /* not on a pad: no witnesses */
        d = fly_v3dist(q->craft.pos, p->craft.pos);
        /* Only marks it can actually catch. A stern chase closes at the
           difference of two cruise speeds, so a target nine kilometres off is
           twenty minutes away and the chase times out long before the guns
           bear — an outlaw permanently en route to nothing. */
        if (d > 4800.0f) continue;
        edge = fight_edge(p->airframe.gun_dps, p->craft.hp, p->airframe.armor,
                          q->airframe.gun_dps, q->craft.hp, q->airframe.armor);
        if (edge < 0.45f) continue;            /* that one wins: leave it alone */
        worth = (fly_pilot_cargo_mass(q) + 40.0f) / (d * 0.001f + 1.0f);
        worth *= edge * fit;
        if (worth > best) { best = worth; *out_id = q->id; *out_player = 0; found = 1; }
    }
    if (v->player_airborne && player_prey_ok(v, p)) {
        float d = fly_v3dist(v->player_pos, p->craft.pos);
        if (d < 9000.0f) {
            /* The player's hull is not in the view and should not be — a pilot
               can see what someone is shooting with, not how much punishment
               they have left. Assume a fresh airframe of its own class. */
            float edge = fight_edge(p->airframe.gun_dps, p->craft.hp, p->airframe.armor,
                                    v->player_threat, p->airframe.structure, 0.0f);
            float worth = 260.0f / (d * 0.001f + 1.0f) * edge * fit;
            if (edge >= 0.45f && worth > best) { *out_id = 0; *out_player = 1; found = 1; }
        }
    }
    return found;
}

/* Is this column fair game for `p`?
 *
 * The road half of `prey_ok`, and deliberately the same shape: an outlaw robs
 * anything not its own, everybody else burns the freight of a power theirs is
 * at war with, and a column under no flag is only ever an outlaw's business.
 * `convoy_will_fire` in fly_game.c is the trigger this decision has to agree
 * with — the two are separate because one decides and the other acts, and a
 * pilot that chased something it would not fire on would be worse than one
 * that never chased at all. */
static int convoy_prey_ok(const fly_pilot_view *v, const fly_pilot *p,
                          const fly_convoy *c) {
    if (!v->world || !c->active) return 0;
    if (p->airframe.gun_dps <= 0.0f) return 0;
    if (p->faction == c->faction && c->faction != FLY_FACTION_FREE) return 0;
    if (fly_diplomacy_state(&v->world->dip, p->faction, c->faction) >= FLY_REL_PACT)
        return 0;
    if (p->kind == FLY_PILOT_PIRATE) return 1;
    if (!fly_pilot_is_combatant(p)) return 0;
    if (c->faction == FLY_FACTION_FREE) return 0;
    return fly_faction_hostile(v->world, p->faction, c->faction);
}

/* Guns hot on the run in.
 *
 * `fly_convoy_gun_solution` is the *trigger* — it decides whether a burst
 * actually hits, and it is checked again at the moment of firing. This is the
 * decision to have the guns armed at all, and it has to be looser than the
 * trigger or nothing ever fires: a decision is taken ten times a second and an
 * attack pass sweeps the cone across the column in rather less than that, so a
 * pilot that only armed its guns while the solution was already good spent the
 * whole run with the safety on. Wider cone, same range, and the trigger still
 * decides what lands. */
static int convoy_guns_hot(const fly_craft *c, const fly_airframe *af,
                           const fly_convoy *cv) {
    fly_v3 to = fly_v3sub(cv->pos, c->pos), fwd;
    float d = fly_v3len(to);
    if (af->gun_dps <= 0.0f || d > af->gun_range || d < 1.0f) return 0;
    fwd = fly_qrot(c->ori, fly_v3mk(1, 0, 0));
    return fly_v3dot(fly_v3norm(to), fwd) > 0.90f; /* ~25 degrees */
}

/* The column worth breaking off for.
 *
 * Weighed the way an aircraft mark is — what it is carrying against how far
 * away it is — with one extra term that an aeroplane does not have: a column
 * shoots back without needing to point at anything, so its flak is priced in
 * directly rather than through `fight_edge`. A Dray is a free lunch and a
 * Leviathan is a decision. */
static int convoy_mark(const fly_pilot *p, const fly_pilot_view *v, uint32_t *out_id) {
    float best = 0.0f, fit = fit_to_fight(p);
    int i, found = 0;
    *out_id = 0;
    if (fit <= 0.05f || !v->convoys) return 0;
    for (i = 0; i < v->convoy_count; ++i) {
        const fly_convoy *c = &v->convoys[i];
        float d, worth, flak;
        if (!convoy_prey_ok(v, p, c)) continue;
        d = fly_v3dist(c->pos, p->craft.pos);
        if (d > 5200.0f) continue;
        flak = fly_convoy_threat(c);
        /* Guns it cannot outlast: a column that puts three seconds of its air
           defence into this aircraft and still has it flying is a fight worth
           having, and one that does not is somebody else's problem. */
        if (flak * 3.0f > p->craft.hp * (1.0f - p->airframe.armor)) continue;
        worth = (c->qty * 3.0f + 30.0f) / (d * 0.001f + 1.0f) * fit;
        if (worth > best) { best = worth; *out_id = c->id; found = 1; }
    }
    return found;
}

/* The outlaw an authority pilot should go after.
 *
 * This used to be "the nearest one", full stop, which makes a patrol a proximity
 * sensor rather than a police force: an outlaw idling past at four kilometres
 * outranked one shooting a courier down at five, and `distress` — set on every
 * victim by the combat step — was read by nothing but the radio chatter. A call
 * for help is now worth three times the distance term, so patrols converge on
 * trouble instead of merely patrolling near it.
 *
 * What it deliberately does *not* do is decline a fight it loses, which a
 * pirate does and which was tried here too. Picking only winnable fights is an
 * opportunist's rule, and a police force that applies it is not one; measured,
 * it also made things worse across the board — patrols hung back, outlaws lived
 * longer and preyed more. Answering the call costs the authority aircraft and
 * nobody else: over three hours it takes patrol losses from 5 to 11 and outlaw
 * losses from 20 to 30 while traders lose nobody extra, and deliveries on that
 * world go from 128 to 198. That is the trade this is for. */
static int police_mark(const fly_pilot *p, const fly_pilot_view *v, uint32_t *out_id) {
    float best = 0.0f;
    int i, j, found = 0;
    *out_id = 0;
    for (i = 0; i < v->peer_count; ++i) {
        const fly_pilot *q = &v->peers[i];
        float d, score;
        if (!q->active || !prey_ok(v, p, q)) continue;
        if (q->craft.on_ground) continue;   /* it was not even checking this */
        d = fly_v3dist(q->craft.pos, p->craft.pos);
        if (d > 6500.0f) continue;
        score = 1.0f / (d * 0.001f + 1.0f);
        for (j = 0; j < v->peer_count; ++j) {
            const fly_pilot *r = &v->peers[j];
            if (!r->active || r->distress < 0.35f || r->mark != q->id) continue;
            score *= 3.0f;   /* somebody is calling, and it is this one's doing */
            break;
        }
        if (score > best) { best = score; *out_id = q->id; found = 1; }
    }
    return found;
}

static const fly_convoy *convoy_by_id(const fly_pilot_view *v, uint32_t id) {
    int i;
    if (!id || !v->convoys) return NULL;
    for (i = 0; i < v->convoy_count; ++i)
        if (v->convoys[i].active && v->convoys[i].id == id) return &v->convoys[i];
    return NULL;
}

static const fly_pilot *peer_by_id(const fly_pilot_view *v, uint32_t id) {
    int i;
    if (!id) return NULL;
    for (i = 0; i < v->peer_count; ++i)
        if (v->peers[i].active && v->peers[i].id == id) return &v->peers[i];
    return NULL;
}

/* Where a craft will be in `t` seconds if it holds its current turn.
 *
 * Straight-line lead is no lead at all against the only target that needs it.
 * A mark flying straight is easy however you aim; a mark in a turn is the one
 * pure pursuit never converts, because aiming where it is now always points
 * behind where it is going, and the pursuer settles into a tail chase it can
 * hold for ever without ever getting the nose across. Rotating the velocity at
 * the mark's own yaw rate and integrating the arc costs two trig calls and is
 * the difference between closing and orbiting. */
static fly_v3 predict_craft(const fly_craft *c, float t) {
    fly_v3 wrate = fly_qrot(c->ori, c->omega);   /* body rates into the world */
    float wz = wrate.z, s, c1;
    fly_v3 d;
    if (t <= 0.0f) return c->pos;
    if (fabsf(wz) < 1.0e-3f) return fly_v3add(c->pos, fly_v3scale(c->vel, t));
    s = sinf(wz * t);
    c1 = 1.0f - cosf(wz * t);
    d.x = (c->vel.x * s - c->vel.y * c1) / wz;
    d.y = (c->vel.x * c1 + c->vel.y * s) / wz;
    d.z = c->vel.z * t;
    return fly_v3add(c->pos, d);
}

#define FLY_PILOT_G 9.80665f

/* --- the four things no control law is allowed to override ----------------
 *
 * All four are hard limits, all four are invisible to the law that produced
 * the command, and all four write the aeroplane off.
 *
 * The wing: past three quarters of the way to the break the stick goes forward
 * and stays there. Easing off is not enough — the elevator at three quarters
 * of Vref has half the authority it has at cruise, so a correction sized for
 * cruise arrives at half strength exactly when it is needed at full, and the
 * fleet mushed for thirty and fifty seconds at a time before flying into the
 * ground with most of its structure intact. No pilot flying a hauler between
 * two markets ever needs to be near the stall, so nothing is lost by refusing
 * outright.
 *
 * The spar: a wing at speed can pull twice what the airframe is stressed for,
 * and a hunter chasing a mark will happily do it. The pull is bled off as the
 * load factor approaches the limit, which is what a pilot's own arms do for
 * them.
 *
 * It lives in its own function because the climb-out has its own control law —
 * wings level, full power, nose up until it is flying — which is exactly the
 * phase where a heavy aeroplane rotates early and mushes, and it does not go
 * anywhere near the one in steer_to. */
static void keep_it_flying(const fly_world *w, const fly_pilot *p, fly_controls *c) {
    const fly_airframe *af = &p->airframe;
    float amax = af->alpha_stall * 0.72f;
    float glim = fly_airframe_g_limit(af);
    float soft = glim * 0.85f;
    float mass = fly_craft_mass(&p->craft, af, 0.0f);
    float vmin = fly_craft_stall_speed(af, p->craft.pos.z, mass) * 1.25f;
    /* The other end of the same envelope. Ninety-four hundredths of
     * never-exceed is where an overspeed warning lives on a real machine: far
     * enough inside that the answer has time to work, far enough outside that
     * an ordinary descent does not trip it. */
    float vmax = af->vne > 1.0f ? af->vne * 0.94f : 0.0f;
    float v = fly_v3len(p->craft.vel);
    /* The ground, first, because it is the only one of these that is fatal on
     * its own and the only one with a deadline.
     *
     * A pull that begins when the terrain is already close is a pull that
     * finishes underground: a hauler recovering from an upset descends at
     * fifty metres a second, and from two hundred metres that is four seconds
     * of warning against an aeroplane that needs six. So the pull starts on
     * *time to impact* rather than on height, at fifteen seconds, and it is
     * asked for before the wing and the spar have their say — the guards
     * below then bound it to the hardest pull the aeroplane can actually
     * make, which is what a maximum-performance recovery is. */
    if (w && !p->craft.on_ground) {
        float agl = p->craft.pos.z - fly_world_ground(w, p->craft.pos.x, p->craft.pos.y);
        float sink = -p->craft.vel.z;
        if (sink > 2.0f && agl > 0.0f && agl < sink * 15.0f) {
            float urgency = 1.0f - (agl / (sink * 15.0f));
            c->pitch = fly_clampf(c->pitch + urgency * 1.2f, c->pitch, 1.0f);
            /* Power, but only where power is the thing that is short. A
             * recovery from a mush needs energy and this is where it comes
             * from; a recovery from a dive has more energy than it can hold
             * already, and opening the throttle into one buys nothing the
             * elevator is not doing better while the spar pays for every metre
             * a second of it. Seventy-one per cent of the structure this fleet
             * spent past never-exceed was spent inside this branch with the
             * throttle it had just been handed — and that is a correlation, not
             * a cause: gated on its own the bill moves 3.64 hp an airborne hour
             * to 3.57, which is inside the noise. The guard below is the whole
             * of the gain and it runs after this, so by the time an excursion
             * is large there is nothing here left to save. What this does catch
             * is the *marginal* one, where the cut below is a few tenths and
             * the pull would otherwise hold the throttle open under it. */
            if (vmax <= 1.0f || v < vmax) c->throttle = 1.0f;
        }
    }
    if (!p->craft.on_ground && p->craft.alpha > amax)
        c->pitch = fly_clampf(c->pitch - (p->craft.alpha - amax) * 6.0f, -1.0f, 0.0f);
    /* Speed first, and before the wing has anything to say about it. The
     * energy law can only ask for so much acceleration — its demand is capped
     * at a couple of metres per second squared — so once an aeroplane is far
     * enough below its cruise the height term outvotes the speed term and it
     * keeps the nose up while decaying. That is a straightforward description
     * of how a fleet of haulers ends up at twenty-three metres a second in an
     * eighty-degree bank, and then in the ground, with full tanks and no
     * damage on it. Under a quarter over the stall nothing else matters:
     * lower the nose, get the speed back, climb afterwards. */
    if (!p->craft.on_ground && vmin > 1.0f && v < vmin)
        c->pitch = fly_clampf(c->pitch - (vmin - v) * 0.10f, -1.0f, 0.0f);
    /* And the top of it, which nothing here defended.
     *
     * The energy law does answer an overspeed — a negative acceleration demand
     * subtracts from the total energy and adds to the split, so it throttles
     * back and raises the nose — but it answers it as one term among several
     * and with a bounded voice: the acceleration demand is clamped at three and
     * a half metres per second squared and the pitch it produces at six tenths,
     * and a height demand of twelve metres a second is quite capable of
     * outvoting both. What was actually holding the fleet inside its envelope
     * was the *cruise demand* being low, and about a fifth of how low it sat
     * came from the turbulence term in `steer_to` — deleting that term outright
     * takes the structure spent past never-exceed from 3.64 hp an airborne hour
     * to 4.54, over 128 seeds and twenty minutes each. A fleet whose speed
     * discipline is a property of the air it is in does not have one.
     *
     * So say it once, here, where the low-speed guard already has the last word
     * on the other end. Throttle first and hard, because thrust costs nothing
     * to give up and the propeller is still making some at this speed; then the
     * nose, gently and only upward, which trades the speed for height the spar
     * does not pay for. The pull goes in before the load limiter below, so what
     * comes out is the hardest recovery the airframe can actually make rather
     * than the one that breaks it — the arrangement the ground pull already
     * has. */
    if (!p->craft.on_ground && vmax > 1.0f) {
        /* And it is asked of the speed the aeroplane is *going* to have, which
         * is the whole of why this works where a level guard did not.
         *
         * A guard that waits for the needle to reach the line is a guard that
         * starts its answer at the one moment the answer cannot work: from
         * ninety-four hundredths of never-exceed in a forty-degree dive there
         * are under two seconds before the line, and the elevator has a load
         * limiter in front of it. What a crew reads instead is the trend —
         * nose down, speed running — and comes off the power and eases the
         * nose up long before the needle is anywhere near. The trend is free
         * here: a descent at flight-path angle gamma is worth g*sin(gamma) of
         * acceleration whatever else the aeroplane is doing, and both terms are
         * on the velocity. Four seconds of it is about the time the answer
         * takes to work.
         *
         * It is also what makes the discipline the pilot's rather than the
         * weather's. A level guard defends a *number*, so how much room there
         * is to defend comes from wherever the cruise demand happens to sit,
         * and part of where that sits is `steer_to`'s turbulence term. A guard
         * on the trend defends the *approach* to the line, and a dive is a dive
         * at any cruise speed. 3.64 hp an airborne hour to 2.57 over 128 seeds,
         * on 2574 deliveries against 2489 — the fleet gets more done, not less
         * — and two independent halves of that seed set agree to 4% where the
         * old law's disagree by 40%, because what this removes is the tail. */
        float gamma_v = v > 1.0f ? -p->craft.vel.z / v : 0.0f;  /* +ve descending */
        float vsoon = v + FLY_PILOT_G * gamma_v * 4.0f;
        if (vsoon < v) vsoon = v;   /* a climb does not buy room at the top */
        if (vsoon > vmax) {
            float over = vsoon - vmax;
            float want = fly_clampf(c->pitch + over * 0.05f, -1.0f, 0.85f);
            c->throttle = fly_clampf(c->throttle - over * 0.25f, 0.0f, 1.0f);
            if (want > c->pitch) c->pitch = want;
        }
    }
    if (c->pitch > 0.0f && p->craft.gload > soft)
        c->pitch *= fly_clampf(1.0f - (p->craft.gload - soft) / (glim * 0.30f), 0.0f, 1.0f);
    /* And the bank, which is the one that actually kills them.
     *
     * A steep turn is a spiral waiting to start: the lift vector goes sideways,
     * the nose drops, the speed builds, and the roll needed to stop it grows
     * faster than the aeroplane's roll rate does — and with control power now
     * measured against dynamic pressure, the moment it is also slow it has
     * neither. Aircraft ended up past ninety degrees of bank, nose down, doing
     * fifty metres a second at the ground with full tanks and no damage on
     * them.
     *
     * Measured off the body axes rather than off the euler roll, because a
     * limiter that reads euler roll near a vertical nose rolls the aeroplane
     * the long way round — which is the failure it is supposed to prevent. */
    {
        fly_v3 left = fly_qrot(p->craft.ori, fly_v3mk(0, 1, 0));
        fly_v3 up = fly_qrot(p->craft.ori, fly_v3mk(0, 0, 1));
        float bank = atan2f(left.z, up.z);
        float lim = 0.90f;   /* about fifty degrees */
        if (!p->craft.on_ground && fabsf(bank) > lim)
            c->roll = fly_clampf(-(bank - fly_signf(bank) * lim) * 2.5f -
                                     p->craft.omega.x * 0.5f, -1.0f, 1.0f);
    }
}

/* Fly toward a point.
 *
 * Roll to turn, as the player's autopilot does, and then the longitudinal half
 * on total energy rather than on height with corrections bolted to it.
 *
 * The old law was an altitude error driving pitch and a fixed cruise throttle,
 * with three guards stacked on top: a cap on pitch scaled by how much airspeed
 * there was to spend, full power whenever that cap bound, and a throttle cut
 * above 0.82 of never-exceed. Each guard was added against a real failure and
 * each worked on the failure it was added for, and together they still left the
 * single largest cause of death in the world. A three-hour probe: sixty-six of
 * a hundred and twenty-seven aircraft written off by *structure*, having spent
 * an average of a hundred and forty seconds past never-exceed, and every
 * airborne pilot spending 7.4% of its life over the line. Not a dive that got
 * away — the peak was only 1.12 of vne. They cruised slightly too fast for
 * minutes at a time and ground themselves to pieces, because the guard could
 * take the throttle to zero and there its authority ended. Nothing in the law
 * could trade the speed away, and in a descent gravity supplies it for free.
 *
 * So state the two things an aeroplane actually controls. Thrust sets how much
 * energy it has; the elevator sets how that energy is divided between height
 * and speed. Demand a climb rate and an acceleration, express both as specific
 * energy rates in metres per second, and then:
 *
 *   their sum        is how much total energy is wanted  -> throttle
 *   their difference is how it should be distributed     -> pitch
 *
 * Both failures fall out of the arithmetic rather than being caught after the
 * fact. Too fast wants a negative acceleration, which subtracts from the sum
 * (throttle back) and adds to the difference (nose up) — it climbs and puts the
 * energy where it does no harm. Too slow while low wants both height and speed;
 * the sum saturates the throttle and the difference goes *negative*, so it
 * lowers the nose and accelerates before it climbs, which is precisely the
 * mush-into-the-ground the pitch cap existed to prevent.
 *
 * One limit of that last claim, found when the flight model started charging
 * for mass and measuring control power against dynamic pressure. The
 * acceleration demand is clamped to a couple of metres per second squared, so
 * the speed term it contributes is bounded by a quarter of the current speed —
 * and once an aeroplane is slow enough, a climb demand of nine metres a second
 * simply outvotes it and the nose stays up while the speed decays. The law is
 * right about the trade and wrong about the priority at the bottom of the
 * envelope, which is why `keep_it_flying` has the last word on speed. */
static void steer_to(const fly_world *w, const fly_pilot *p, fly_v3 goal,
                     float throttle, fly_controls *out) {
    float roll, pitch, yaw, want_yaw, yerr, roll_des;
    fly_v3 to = fly_v3sub(goal, p->craft.pos);
    memset(out, 0, sizeof *out);
    fly_qto_euler(p->craft.ori, &roll, &pitch, &yaw);
    want_yaw = atan2f(to.y, to.x);
    yerr = fly_wrap_pi(want_yaw - yaw);
    roll_des = fly_clampf(-yerr * 1.3f, -0.85f, 0.85f);
    out->roll = fly_clampf((roll_des - roll) * 1.6f, -0.85f, 0.85f);
    out->yaw = fly_clampf(-yerr * 0.3f, -0.35f, 0.35f);

    {
        const fly_airframe *af = &p->airframe;
        /* Ground speed, not airspeed: `fly_pilot_view` carries no weather, and
           a decision has to be reachable from what think is allowed to see.
           Wind is a few metres a second against a sixty-metre cruise. */
        float spd = fly_v3len(p->craft.vel);
        float vref = af->vref > 1.0f ? af->vref : 30.0f;
        float vne = af->vne > 1.0f ? af->vne : vref * 2.0f;
        /* Fast enough to be efficient, far enough inside vne that a gust or a
           shallow descent does not put it over. */
        /* Turbulence penetration, felt rather than looked up. `fly_pilot_view`
           carries no weather — a decision has to be reachable from what think
           is allowed to see — but the gust the aeroplane is sitting in is on
           the craft, because it is a property of the aeroplane's own state.
           That is exactly what a crew has too: nobody reads the turbulence off
           a chart, they feel it and they slow down. The load a gust puts
           through a wing goes with the speed it is met at, and a fleet that
           crossed the storm belt at cruise bent its spars. */
        float rough = fly_clampf(fly_v3len(p->craft.gust) / 11.0f, 0.0f, 1.0f);
        float vdem = fly_clampf(vref * (1.25f - 0.22f * rough),
                                vref * 0.95f, vne * 0.72f);
        float hdot = fly_clampf((goal.z - p->craft.pos.z) * 0.09f, -12.0f, 9.0f);
        float vdot = fly_clampf((vdem - spd) * 0.35f, -3.5f, 2.5f);
        float trade = spd * vdot / FLY_PILOT_G;   /* the acceleration, as a climb rate */
        float ste = hdot + trade;                 /* total energy wanted */
        float seb = hdot - trade;                 /* and how to split it */
        out->throttle = fly_clampf(throttle + ste * 0.085f, 0.0f, 1.0f);
        out->pitch = fly_clampf(seb * 0.055f, -0.5f, 0.6f);

        /* Two things the energy law does not say, kept from the old one.
           A stall is not a slow aeroplane, it is a detached wing, and the only
           answer is to unload it — the law would ease the nose down but not
           push it. And a banked stall is a spin, which at eight hundred metres
           over a ridge is an aircraft that does not come back. */
        if (p->craft.stalled && out->pitch > -0.35f) out->pitch = -0.35f;
        if (spd < vref * 0.83f) {
            out->roll = fly_clampf(-roll * 2.0f, -0.5f, 0.5f);
            out->yaw *= 0.3f;
        }
        /* And two things the flight model started saying after this law was
           written, both of which a pilot has to answer with their feet and
           their hands rather than with their energy budget.

           The ball: adverse yaw pushes the nose out of every turn entered on
           ailerons and the propeller pushes it left at any power that is not
           cruise. Uncorrected that is a permanent skid, which is drag, which
           is a slower aeroplane flying a longer leg — and on final it is a
           wing that drops first. One term, and the fleet stops flying
           sideways.

           The buffet: the wing now warns before it goes, and something that
           can hear the warning and keeps pulling is not a pilot. Easing the
           back pressure in proportion is exactly what the warning is for, and
           it is a far better guard than waiting for `stalled`, which is the
           report that it is already too late. */
        out->yaw = fly_clampf(out->yaw + p->craft.slip * 1.6f, -0.5f, 0.5f);
        if (p->craft.buffet > 0.2f && out->pitch > 0.0f)
            out->pitch *= fly_clampf(1.0f - p->craft.buffet, 0.0f, 1.0f);

        keep_it_flying(w, p, out);
    }
}


/* Terrain-aware cruise height for a leg: over the higher of the two ends, plus
 * clearance, so a pilot does not fly a straight line into a ridge. */
/* The highest ground on the next few kilometres of track.
 *
 * Sampling only the ground directly beneath is what put aircraft into hillsides:
 * the altitude term reacts to where the aeroplane *is*, and a climb takes tens
 * of seconds, so by the time a ridge is underneath there is no longer room to
 * out-climb it. Looking along the track instead turns the same term into a
 * plan — the aircraft is already high by the time it gets there.
 *
 * Eight samples. The relief that matters here is a ridge line, kilometres
 * across; sampling finer buys nothing a 620 m cruise margin does not already
 * cover, and this runs for every airborne pilot several times a second. */
#define FLY_PILOT_LOOKAHEAD 4500.0f

static float ridge_ahead(const fly_world *w, fly_v3 from, fly_v3 dir, float reach) {
    float hi = -1.0e9f;
    int i;
    if (reach < 1.0f) return fly_world_ground(w, from.x, from.y);
    for (i = 0; i <= 8; ++i) {
        float t = reach * (float)i / 8.0f;
        float h = fly_world_ground(w, from.x + dir.x * t, from.y + dir.y * t);
        if (h > hi) hi = h;
    }
    return hi;
}

/* Unit horizontal heading from the pilot toward a site. */
static fly_v3 track_dir(const fly_world *w, const fly_pilot *p, int dest) {
    fly_v2 q = fly_world_delta(w->loc[dest].pos, fly_wpos_of(p->craft.pos));
    fly_v3 d = fly_v3mk(q.x, q.y, 0.0f);
    float len = fly_v3len(d);
    return len > 1.0f ? fly_v3scale(d, 1.0f / len) : fly_v3mk(1, 0, 0);
}

/* Raise a goal until the track to it clears the ground.
 *
 * Transit and the approach have had this since aircraft were found flying into
 * the hill in front of a valley pad. Combat did not, and combat is where the
 * aircraft are: a hunter follows its mark's lead point wherever that leads, an
 * evader flies whichever way is away, and neither ever looked at what was in
 * front of it. Fifty of the hundred and twenty-seven losses in a three-hour
 * probe happened while hunting or evading, and terrain was the largest single
 * cause. Same eight samples along the same track — this is `cruise_alt`'s
 * lookahead with the destination made an argument. */
static fly_v3 clear_track(const fly_world *w, const fly_pilot *p, fly_v3 goal,
                          float clearance) {
    fly_v3 d = fly_v3mk(goal.x - p->craft.pos.x, goal.y - p->craft.pos.y, 0.0f);
    float len = fly_v3len(d), floor_z;
    d = len > 1.0f ? fly_v3scale(d, 1.0f / len) : fly_v3mk(1, 0, 0);
    floor_z = ridge_ahead(w, p->craft.pos, d,
                          len < FLY_PILOT_LOOKAHEAD ? len : FLY_PILOT_LOOKAHEAD) + clearance;
    if (goal.z < floor_z) goal.z = floor_z;
    return goal;
}

static float cruise_alt(const fly_world *w, const fly_pilot *p, int dest) {
    float a = w->loc[p->origin].pad_z, b = w->loc[dest].pad_z;
    float hi = a > b ? a : b;
    float reach = fly_world_dist(w->loc[dest].pos, fly_wpos_of(p->craft.pos));
    float ahead;
    if (reach > FLY_PILOT_LOOKAHEAD) reach = FLY_PILOT_LOOKAHEAD;
    ahead = ridge_ahead(w, p->craft.pos, track_dir(w, p, dest), reach);
    if (ahead > hi) hi = ahead;
    return hi + FLY_PILOT_CRUISE_ALT;
}


/* Is the pilot sitting on the pad it belongs to right now? */
static int on_pad(const fly_pilot *p) {
    return p->craft.on_ground &&
           (p->task == FLY_TASK_IDLE || p->task == FLY_TASK_TRADING ||
            p->task == FLY_TASK_SERVICE);
}

int fly_pilot_think(const fly_pilot *p, const fly_pilot_view *v, fly_rng *rng,
                    float dt, fly_pilot_cmd *out) {
    const fly_world *w = v->world;
    memset(out, 0, sizeof *out);
    (void)dt;
    if (!p->active) return 0;

    /* ---- saucers ----
     *
     * Everything below this point is a working life: pads, cargo, legs, fuel.
     * A saucer has none of that. It hangs above the atmosphere and either
     * something has come up to it or nothing has, and those are the only two
     * states it has.
     *
     * Station-keeping is the whole of "floating". It is weightless, so holding
     * altitude costs nothing — what it actually does is bleed off whatever
     * velocity it has picked up, which is why it drifts rather than orbits and
     * why it is still there when you come back. Nothing else in the game can
     * do that, and nothing else in the game is supposed to look like it can. */
    if (p->kind == FLY_PILOT_UFO) {
        float reach = p->airframe.radar_range;
        fly_v3 to = fly_v3sub(v->player_pos, p->craft.pos);
        float d = fly_v3len(to);
        float spd = fly_v3len(p->craft.vel);
        out->kind = FLY_PCMD_CONTROLS;
        if (v->player_airborne && v->player_pos.z > FLY_KARMAN * 0.6f && d < reach) {
            /* Something is up here. Close, and shoot when it is in range. */
            steer_to(w, p, v->player_pos, 0.0f, &out->controls);
            out->controls.throttle = 0.0f;
            out->controls.rocket = d > p->airframe.gun_range * 0.55f;
            out->controls.fire = d < p->airframe.gun_range;
            return 1;
        }
        /* Nothing to do: kill the drift and wait. Braking is a burn pointed
         * back along the velocity, which is the only way anything moves up
         * here — there is no air to slow down in. */
        if (spd > 2.0f) {
            fly_v3 back = fly_v3sub(p->craft.pos, fly_v3scale(p->craft.vel, 60.0f));
            steer_to(w, p, back, 0.0f, &out->controls);
            out->controls.throttle = 0.0f;
            out->controls.rocket = spd > 6.0f;
        }
        return 1;
    }

    /* ---- on a pad: service, settle up, take on work, go ---- */
    if (on_pad(p)) {
        int here = p->origin;
        if (p->pad_until > 0.0f) return 0;
        if (p->craft.fuel < p->airframe.fuel_cap * 0.6f) {
            out->kind = FLY_PCMD_REFUEL;
            return 1;
        }
        if (p->craft.hp < p->airframe.structure * 0.7f || p->craft.wear > 0.25f) {
            out->kind = FLY_PCMD_REPAIR;
            return 1;
        }
        /* Cargo is sold where it was *going*, not where it is. Selling it back
           to the site it came from is the trade the pilot just decided was not
           worth making, and doing that on the pad it is standing on is an
           infinite loop that prints money — which is exactly what it did. */
        if (p->deal_res >= 0 && p->cargo[p->deal_res] > 0.5f) {
            if (here != p->deal_origin) {
                out->kind = FLY_PCMD_SELL;
                out->a = p->deal_res;
                out->f = p->cargo[p->deal_res];
                return 1;
            }
            /* loaded and still at the source: fly the leg it was bought for */
            out->kind = FLY_PCMD_SET_LEG;
            out->a = p->dest != here ? p->dest : p->home;
            out->task = FLY_TASK_TRANSIT;
            return 1;
        }
        {
            int res = -1, dest = -1;
            float qty = 0.0f;
            long worth = 0;
            switch (p->kind) {
            case FLY_PILOT_TRADER:  worth = best_trade(p, w, here, &res, &qty, &dest); break;
            case FLY_PILOT_COURIER: worth = best_haul(p, w, here, &res, &qty, &dest); break;
            default: break;
            }
            if (worth > 0 && dest >= 0 && res >= 0) {
                out->kind = FLY_PCMD_BUY;
                out->a = res;
                out->f = qty;
                out->task = dest;   /* apply records where the load is bound */
                return 1;
            }
        }
        /* no cargo work: fly the beat, or go and look at something */
        {
            int dest = -1, task = FLY_TASK_TRANSIT;
            if (p->kind == FLY_PILOT_PROSPECTOR) {
                dest = survey_target(p, w, rng);
                task = FLY_TASK_SURVEY;
            } else {
                float reach = range_km(p);
                int i, seen = 0;
                for (i = 0; i < w->nloc; ++i) {
                    if (i == here || leg_km(w, here, i) > reach) continue;
                    if (fly_rng_range(rng, (uint32_t)(++seen)) == 0) dest = i;
                }
            }
            if (dest >= 0) {
                out->kind = FLY_PCMD_SET_LEG;
                out->a = dest;
                out->task = task;
                return 1;
            }
        }
        return 0; /* nowhere to go: sit tight and look again shortly */
    }

    /* ---- rolling on the pad after a SET_LEG ----
     * One LAUNCH to open the throttle, and then the ordinary flight logic
     * below, which pitches up as it steers: emitting LAUNCH again every tick
     * instead left a line of aircraft at full power that never rotated. */
    if (p->craft.on_ground && p->controls.throttle < 0.5f) {
        out->kind = FLY_PCMD_LAUNCH;
        return 1;
    }

    /* ---- climbing out ----
     * Turning toward the destination while still on the runway rolls a wing
     * into the ground; the full simulation duly wrote the aircraft off, and at
     * seventy losses an hour the restock was hiding a world where nobody ever
     * got anywhere. Straight ahead, wings level, until it is flying. */
    {
        float agl = p->craft.pos.z - fly_world_ground(w, p->craft.pos.x, p->craft.pos.y);
        if (p->craft.on_ground || agl < 90.0f) {
            float roll, pitch, yaw;
            fly_qto_euler(p->craft.ori, &roll, &pitch, &yaw);
            memset(&out->controls, 0, sizeof out->controls);
            out->controls.throttle = 1.0f;
            out->controls.roll = fly_clampf(-roll * 2.2f, -0.6f, 0.6f);
            /* Rotate at a speed the wing can actually fly at, not at a
               constant. Thirty-four metres a second was chosen when an empty
               airframe was the only airframe there was; with fuel and freight
               in the mass the stall speed moves with the load, and a hauler
               that rotated at a fixed number left the ground below flying
               speed, mushed for half a minute at full power and settled back
               into the ground with its structure intact. */
            float vr = fly_craft_stall_speed(&p->airframe, p->craft.pos.z,
                                             fly_craft_mass(&p->craft, &p->airframe, 0.0f));
            out->controls.pitch = fly_v3len(p->craft.vel) > vr * 1.18f
                                ? fly_clampf(0.42f - p->craft.vel.z * 0.05f, -0.1f, 0.55f)
                                : 0.0f;
            keep_it_flying(w, p, &out->controls);
            out->kind = FLY_PCMD_CONTROLS;
            return 1;
        }
    }

    /* ---- airborne ---- */
    {
        float fuel_frac = p->craft.fuel / (p->airframe.fuel_cap + 1e-3f);
        int dest = p->dest;

        /* Running dry beats every other consideration. A pilot that presses on
           and goes down in the hills is not a pilot the player ever meets.

           This used to read `fuel_frac < 0.16 && task != TRANSIT`, which
           excluded the one task that is a long leg: a transit pilot never
           diverted at all, it simply pressed on and `fly_pilot_step` wrote it
           off when the tank emptied. The exclusion was there to stop a pilot
           already heading for a pad re-issuing SET_LEG every tick, and the
           honest form of that is to ask whether it can reach the pad it has
           chosen rather than which task it is flying under. Comparing reach
           against the leg also terminates: after the diversion `near` is the
           destination, so the test stops firing. */
        {
            int near = fly_world_nearest(w, fly_wpos_of(p->craft.pos), 1);
            float reach = range_km(p) * 1000.0f;
            float d_dest = dest >= 0 && dest < w->nloc
                         ? fly_world_dist(w->loc[dest].pos, fly_wpos_of(p->craft.pos))
                         : 1.0e30f;
            if (d_dest > reach && near >= 0 && near != dest) {
                float d_near = fly_world_dist(w->loc[near].pos, fly_wpos_of(p->craft.pos));
                if (d_near < d_dest) {
                    out->kind = FLY_PCMD_SET_LEG;
                    out->a = near;
                    out->task = FLY_TASK_TRANSIT;
                    return 1;
                }
            }
        }

        /* --- and a missile outranks everything -----------------------------
         *
         * Above the task machine entirely, because there is no errand worth
         * flying straight through a seeker for. It emits controls and no leg
         * change, so the pilot goes back to whatever it was doing the moment
         * the round is gone — a defensive break is four seconds, not a state.
         *
         * The manoeuvre is the one an aeroplane actually flies, and it is the
         * same one the player has to learn:
         *
         *   put the thing on the beam, because a round that has to turn to
         *   follow spends the energy it cannot replace, and a radar round is
         *   dropped by the Doppler gate outright;
         *   go down, because the ground behind you is what breaks the head's
         *   line of sight and because that is where the round has to follow;
         *   come off the power, because a plume is what a heat seeker is
         *   looking at;
         *   and shed a decoy, timed — not on the launch, but at three seconds
         *   out, so the flare is brightest in the window the seeker is
         *   deciding in. Popping the moment the tone starts is the beginner's
         *   mistake and this deliberately does not make it. */
        if (v->ord && p->craft.hp > 0.0f) {
            const fly_ord *worst = NULL;
            float soonest = 1.0e9f;
            int i;
            for (i = 0; i < v->ord_count; ++i) {
                float t = fly_pilot_ord_threat(p, &v->ord[i]);
                if (t >= 0.0f && t < soonest) { soonest = t; worst = &v->ord[i]; }
            }
            if (worst && soonest < 7.0f) {
                fly_v3 across = fly_v3cross(fly_v3norm(worst->vel), fly_v3mk(0, 0, 1));
                fly_v3 goal;
                float ground_z;
                if (fly_v3len(across) < 0.1f) across = fly_v3mk(1, 0, 0);
                across = fly_v3norm(across);
                /* Whichever way across is already the way it is pointing: a
                 * break that reverses the turn wastes the second it takes to
                 * roll, and a second is a quarter of the warning. */
                if (fly_v3dot(across, p->craft.vel) < 0.0f)
                    across = fly_v3scale(across, -1.0f);
                goal = fly_v3add(p->craft.pos, fly_v3scale(across, 2200.0f));
                ground_z = fly_world_ground(w, goal.x, goal.y);
                goal.z = ground_z + 90.0f;
                if (goal.z > p->craft.pos.z) goal.z = p->craft.pos.z;
                goal = clear_track(w, p, goal, 70.0f);
                steer_to(w, p, goal, fly_ord_is_heat(worst->kind) ? 0.25f : 1.0f,
                         &out->controls);
                if (fly_ord_is_heat(worst->kind)) out->controls.throttle *= 0.35f;
                out->controls.decoy = soonest < 3.0f;
                out->kind = FLY_PCMD_CONTROLS;
                return 1;
            }
        }

        /* Being shot at outranks the errand — for everybody, which it did not.
         *
         * This branch used to require `gun_dps <= 0`, so only the defenceless
         * reacted at all. An armed pilot took fire, had `distress` and `mark`
         * set on it by the combat step, and flew on to its next pad as though
         * nothing were happening. Whether to shoot back is a separate question
         * from whether to notice.
         *
         * Shooting back is for those who can. `pilots_combat` refuses to let a
         * non-combatant be the aggressor, so a trader that turned to fight
         * would chase a target it is structurally unable to damage — hauliers
         * run whatever they are carrying, whoever's flag is on it. Everyone
         * else turns and fights while there is enough airframe left to be
         * worth fighting in. */
        if (p->distress > 0.35f && p->task != FLY_TASK_EVADE &&
            p->task != FLY_TASK_HUNT && (p->mark || p->mark_player || p->mark_convoy)) {
            int can_fight = p->airframe.gun_dps > 0.0f &&
                            fly_pilot_is_combatant(p) &&
                            p->craft.hp > p->airframe.structure * 0.35f;
            out->kind = FLY_PCMD_SET_LEG;
            out->mark = p->mark;
            out->mark_player = p->mark_player;
            out->mark_convoy = p->mark_convoy;
            if (can_fight) {
                out->a = dest >= 0 ? dest : p->home;   /* the errand it returns to */
                out->task = FLY_TASK_HUNT;
                if (out->a >= 0) return 1;
            } else {
                out->a = fly_world_nearest(w, fly_wpos_of(p->craft.pos), 1);
                out->task = FLY_TASK_EVADE;
                if (out->a >= 0) return 1;
            }
        }

        if (p->task == FLY_TASK_HUNT) {
            const fly_pilot *q = peer_by_id(v, p->mark);
            const fly_convoy *cv = convoy_by_id(v, p->mark_convoy);
            fly_v3 mpos;
            int gone = 0;
            if (p->mark_player) {
                mpos = v->player_pos;
                gone = !v->player_airborne;
            } else if (p->mark_convoy) {
                gone = cv == NULL;
                mpos = cv ? cv->pos : p->craft.pos;
            } else if (q) {
                mpos = q->craft.pos;
                gone = q->craft.on_ground;
            } else {
                gone = 1;
                mpos = p->craft.pos;
            }
            {
                fly_craft mark_craft;
                fly_v3 to = fly_v3sub(mpos, p->craft.pos), fwd, lead;
                float d = fly_v3len(to);
                float closing, aim_err, horizon;

                if (!p->mark_player && !p->mark_convoy && q) mark_craft = q->craft;
                else {
                    memset(&mark_craft, 0, sizeof mark_craft);
                    mark_craft.pos = mpos;
                    /* A column is not a craft, but it does have a velocity, and
                       the lead below is worthless without it: at a hundred metres
                       a second of closure the difference between aiming at a
                       truck and at where the truck is going is the length of the
                       column. */
                    if (cv) mark_craft.vel = fly_v3scale(cv->tangent, cv->speed);
                }
                closing = d > 1.0f
                        ? fly_v3dot(fly_v3sub(p->craft.vel, mark_craft.vel),
                                    fly_v3scale(to, 1.0f / d))
                        : 0.0f;

                /* Give it up. A chase that has run two minutes, or one where
                   the mark is already outside twice the gun's reach and pulling
                   further away, is a chase that ends with an empty tank and no
                   kill — and an outlaw permanently off hunting nothing is one
                   fewer thing happening in the world. */
                if (gone || d > 11000.0f ||
                    v->time_s - p->leg_start > 180.0 ||
                    (d > p->airframe.gun_range * 2.5f && closing < 0.0f)) {
                    out->kind = FLY_PCMD_SET_LEG;
                    out->a = p->home;
                    out->task = FLY_TASK_TRANSIT;
                    return 1;
                }

                /* Losing is a reason to stop, and it was not one.
                 *
                 * A chase ended when the mark got away or when three minutes
                 * were up, and in no other circumstance — so a pilot that had
                 * picked a fight it could not win pressed the attack until one
                 * of them was destroyed, and it was usually the one that had
                 * started it. Breaking off to EVADE rather than TRANSIT matters:
                 * a beaten aircraft that turns for home flies a straight line
                 * in front of the guns that beat it. */
                if (p->craft.hp < p->airframe.structure * 0.35f && !gone) {
                    out->kind = FLY_PCMD_SET_LEG;
                    out->a = fly_world_nearest(w, fly_wpos_of(p->craft.pos), 1);
                    out->task = FLY_TASK_EVADE;
                    out->mark = p->mark;
                    out->mark_player = p->mark_player;
                    out->mark_convoy = p->mark_convoy;
                    if (out->a >= 0) return 1;
                }

                /* Aim at the collision, not at the aeroplane.
                 *
                 * This used to lead by how long the nose needed to swing
                 * through its current error — `aim_err / 0.40`, floored at four
                 * tenths of a second. Read that at the moment it matters and it
                 * says: the better you are pointing at the mark, the less you
                 * lead, and once the nose is nearly on, you lead by nothing at
                 * all. That is pure pursuit wearing a lead term, and pure
                 * pursuit behind a mark of the same cruise speed is a curve
                 * that converges on its tail and stays there. Eight hundred and
                 * sixteen hunts in three hours converted seven times.
                 *
                 * The right horizon is time to intercept: how long until this
                 * aircraft, at the speed it is actually doing, reaches the
                 * place the mark is going. Aiming there each tick *is* the
                 * constant-bearing course — the bearing stops rotating, the
                 * range closes, and the nose arrives with the mark rather than
                 * behind it. Two fixed-point passes converge it well inside the
                 * error in predicting a turning target that far ahead. */
                fwd = fly_qrot(p->craft.ori, fly_v3mk(1, 0, 0));
                aim_err = d > 1.0f
                        ? acosf(fly_clampf(fly_v3dot(fly_v3scale(to, 1.0f / d), fwd),
                                           -1.0f, 1.0f))
                        : 0.0f;
                horizon = fly_clampf(aim_err / 0.40f, 0.4f, 5.0f);
                lead = predict_craft(&mark_craft, horizon);
                /* 80 m, not the 620 of a cruise: a hunter that refuses to go
                   low can never engage anything low, and the point is to clear
                   the ridge between here and the mark, not to stay above the
                   fight. */
                lead = clear_track(w, p, lead, 80.0f);
                steer_to(w, p, lead, 1.0f, &out->controls);
                out->controls.fire =
                    cv ? convoy_guns_hot(&p->craft, &p->airframe, cv)
                       : fly_pilot_gun_solution(&p->craft, &p->airframe, &mark_craft) > 0.0f;
                /* And the launcher, which is a different weapon and therefore a
                 * different decision. The gun wants the nose on the mark right
                 * now; a rack wants the mark inside an envelope that is far
                 * wider and much further out, and the whole value of carrying
                 * one is taking the shot at two kilometres instead of closing
                 * to four hundred metres against something that shoots back.
                 *
                 * `fly_ord_launch_ok` is the same predicate the trigger in
                 * fly_game.c applies, so a pilot cannot ask for a shot the
                 * world will refuse — the arrangement `gun_solution` and
                 * `pilots_will_fire` already have. Ammunition is finite, so
                 * this fires when the envelope is good and not merely when it
                 * is possible: a seeker rack emptied at the edge of its reach
                 * is a rack of four misses. */
                if (p->airframe.ord_kind && p->craft.ammo > 0 &&
                    p->craft.ord_cooldown <= 0.0f) {
                    float want = p->airframe.ord_range *
                                 (fly_ord_is_ballistic(p->airframe.ord_kind) ? 1.0f : 0.72f);
                    out->controls.launch =
                        fly_ord_launch_ok(p->airframe.ord_kind, want, p->craft.pos,
                                          p->craft.vel, mpos, mark_craft.vel);
                }
                out->kind = FLY_PCMD_CONTROLS;
                return 1;
            }
        }

        if (p->task == FLY_TASK_EVADE) {
            fly_v3 away;
            const fly_pilot *q = peer_by_id(v, p->mark);
            const fly_convoy *cv = convoy_by_id(v, p->mark_convoy);
            fly_v3 from = p->mark_player ? v->player_pos
                        : cv ? cv->pos
                        : (q ? q->craft.pos : p->craft.pos);
            /* Stop running once there is nothing to run from.
             *
             * Nothing in this function ever left EVADE. A pilot that was shot
             * at once flew away from the place it happened until its tanks
             * forced a diversion, and then did it again — so the state was
             * effectively terminal, and the more pilots reacted to being
             * attacked the more of the world's flying hours it consumed. It was
             * a quarter of all airborne time (50,506 s of 197,000) and it cost
             * a third of the deliveries. `distress` decays in about two and a
             * half seconds and is pushed back to full by every hit, so it reads
             * as "still being shot at" exactly the way this wants. */
            {
                int chaser_gone = p->mark_player ? !v->player_airborne
                                : p->mark_convoy ? cv == NULL
                                : (q == NULL || q->craft.on_ground);
                float gap = fly_v3dist(p->craft.pos, from);
                if (p->distress < 0.15f && (chaser_gone || gap > 3500.0f)) {
                    out->kind = FLY_PCMD_SET_LEG;
                    out->a = dest >= 0 && dest < w->nloc ? dest : p->home;
                    out->task = FLY_TASK_TRANSIT;
                    if (out->a >= 0) return 1;
                }
            }
            away = fly_v3sub(p->craft.pos, from);
            away.z = 0.0f;
            if (fly_v3len(away) < 1.0f) away = fly_v3mk(1, 0, 0);
            away = fly_v3add(p->craft.pos, fly_v3scale(fly_v3norm(away), 4000.0f));
            away.z = cruise_alt(w, p, dest >= 0 ? dest : p->home);
            /* `cruise_alt` clears the track to the *destination*; an evader is
               running the other way, so the ground it is about to cross is not
               the ground that term looked at. */
            away = clear_track(w, p, away, 110.0f);
            steer_to(w, p, away, 1.0f, &out->controls);
            out->kind = FLY_PCMD_CONTROLS;
            return 1;
        }

        /* transit and survey both just fly the leg */
        if (dest >= 0 && dest < w->nloc) {
            const fly_location *T = &w->loc[dest];
            float d = fly_world_dist(T->pos, fly_wpos_of(p->craft.pos));
            float plan_z;
            /* Predators pick up work in the air: a pirate on its way somewhere
               will break off for a fat enough mark, which is what makes the
               map feel occupied rather than scheduled. */
            if (p->kind == FLY_PILOT_PIRATE || p->kind == FLY_PILOT_PATROL) {
                uint32_t id = 0;
                int is_player = 0, got;
                uint32_t convoy_id = 0;
                got = p->kind == FLY_PILOT_PIRATE ? hunt_mark(p, v, &id, &is_player)
                                                  : police_mark(p, v, &id);
                /* Aircraft first, the road second. A loaded trader in the air is
                   the easier fight and the one already in front of you; a column
                   is what there is to do when the sky is empty — which, on a map
                   this size, it usually is. */
                if (!got && convoy_mark(p, v, &convoy_id)) got = 1;
                if (got && fuel_frac > 0.3f) {
                    out->kind = FLY_PCMD_SET_LEG;
                    out->a = dest;
                    out->task = FLY_TASK_HUNT;
                    out->mark = convoy_id ? 0 : id;
                    out->mark_player = convoy_id ? 0 : is_player;
                    out->mark_convoy = convoy_id;
                    return 1;
                }
            }
            if (d < FLY_PILOT_DOCK_R && p->craft.pos.z < T->pad_z + 140.0f) {
                out->kind = FLY_PCMD_DOCK;
                out->a = dest;
                return 1;
            }
            /* Which way in, and how far there is to fly to get there.
             *
             * The fleet flew straight at the pad from wherever it happened to
             * be, and a straight line into a pad with a rise in front of it is
             * the one approach the floor below cannot let it finish: the
             * aircraft is held high until it is too close to come down, and the
             * arrival is a go-around at best. The world knows which courses
             * into a site are flyable — see fly_world_approach_aim — so the leg
             * is flown to the point it hands back, and the descent is spread
             * over the distance actually left to fly rather than over the
             * straight line to a pad the aeroplane is not pointing at. */
            float along = 0.0f;
            fly_wpos ap = fly_world_approach_aim(w, dest, fly_wpos_of(p->craft.pos), &along);
            fly_v2 aq = fly_world_delta(ap, fly_wpos_of(p->craft.pos));
            float leg = hypotf(aq.x, aq.y);
            float path = leg + along;
            fly_v3 dir = leg > 1.0f ? fly_v3mk(aq.x / leg, aq.y / leg, 0.0f)
                                    : track_dir(w, p, dest);
            /* Descend on the approach so the arrival is a landing, not a dive —
               but never below the ground under the aircraft. A pad in a valley
               had aircraft flying the last two kilometres at pad height into
               the side of the hill in front of it. */
            plan_z = path > 1800.0f ? cruise_alt(w, p, dest)
                                 : T->pad_z + 40.0f + (path / 1800.0f) * (cruise_alt(w, p, dest) - T->pad_z - 40.0f);
            {
                /* and the same lookahead on the way in: the descent to a pad in
                   a valley used to be flown into the hill in front of it. The
                   floor relaxes to the pad's own ground as the distance closes,
                   so this never blocks the landing it is protecting. It looks
                   along the leg the aircraft is flying, which on an offset
                   approach is not the line to the pad. */
                float reach = leg < FLY_PILOT_LOOKAHEAD ? leg : FLY_PILOT_LOOKAHEAD;
                float floor_z = ridge_ahead(w, p->craft.pos, dir, reach) + 110.0f;
                if (d > FLY_PILOT_DOCK_R && plan_z < floor_z) plan_z = floor_z;
            }
            /* The goal in the aircraft's own coordinates: `fly_world_delta`
               has already answered the wrap, and steer_to subtracts a position
               from a position. */
            steer_to(w, p, fly_v3mk(p->craft.pos.x + aq.x, p->craft.pos.y + aq.y, plan_z),
                     path > 1400.0f ? 0.92f : 0.55f, &out->controls);
            out->kind = FLY_PCMD_CONTROLS;
            return 1;
        }
    }
    return 0;
}

/* ---------------- applying ----------------
 *
 * The only place a pilot or the market changes. Every branch validates first:
 * a command that arrived over a wire gets exactly the same scrutiny as one the
 * think step just produced, because eventually one of them will have. */

int fly_pilot_apply(fly_pilot *p, fly_world *world, double time_s,
                    const fly_pilot_cmd *cmd) {
    if (!p || !p->active || !world || !cmd) return -1;
    switch (cmd->kind) {
    case FLY_PCMD_NONE:
        return 0;
    case FLY_PCMD_CONTROLS:
        if (on_pad(p)) return -2;
        p->controls = cmd->controls;
        p->controls.throttle = fly_clampf(p->controls.throttle, 0.0f, 1.0f);
        p->controls.pitch = fly_clampf(p->controls.pitch, -1.0f, 1.0f);
        p->controls.roll = fly_clampf(p->controls.roll, -1.0f, 1.0f);
        p->controls.yaw = fly_clampf(p->controls.yaw, -1.0f, 1.0f);
        return 0;
    case FLY_PCMD_LAUNCH:
        if (!p->craft.on_ground) return -2;
        if (fly_pilot_cargo_mass(p) > p->airframe.cargo_cap) return -3;
        p->leg_start = time_s;
        p->controls.throttle = 1.0f;
        return 0;
    case FLY_PCMD_DOCK: {
        const fly_location *L;
        if (cmd->a < 0 || cmd->a >= world->nloc) return -2;
        L = &world->loc[cmd->a];
        if (fly_world_dist(L->pos, fly_wpos_of(p->craft.pos)) > FLY_PILOT_DOCK_R) return -3;
        p->craft.pos = fly_wpos_at(park_spot(L, p->id),
                                   L->pad_z + p->airframe.gear_height);
        p->craft.vel = fly_v3mk(0, 0, 0);
        p->craft.omega = fly_v3mk(0, 0, 0);
        p->craft.ori = fly_qeuler(0, 0, 0);
        p->craft.on_ground = 1;
        p->craft.air_time = 0.0f;
        memset(&p->controls, 0, sizeof p->controls);
        p->origin = cmd->a;
        p->dest = cmd->a;
        p->task = FLY_TASK_IDLE;
        p->pad_until = FLY_PILOT_TURNAROUND;
        p->mark = 0;
        p->mark_player = 0;
        p->mark_convoy = 0;
        p->distress = 0.0f;
        return 0;
    }
    case FLY_PCMD_BUY: {
        fly_location *L;
        float px, cost, mass;
        if (!on_pad(p)) return -2;
        if (cmd->a < 0 || cmd->a >= FLY_RES_COUNT || cmd->f <= 0.0f) return -3;
        L = &world->loc[p->origin];
        if (L->stock[cmd->a] < cmd->f) return -4;
        px = fly_world_price(world, p->origin, (fly_resource)cmd->a);
        cost = px * cmd->f + 1.0f;
        if ((float)p->tokens < cost) return -5;
        mass = fly_pilot_cargo_mass(p) + cmd->f * fly_resource_mass((fly_resource)cmd->a);
        if (mass > p->airframe.cargo_cap) return -6;
        p->tokens -= (long)cost;
        L->stock[cmd->a] -= cmd->f;
        p->cargo[cmd->a] += cmd->f;
        p->deal_res = cmd->a;
        p->deal_origin = p->origin;
        p->deal_qty = cmd->f;
        p->deal_cost = (long)cost;
        fly_faction_push(world, p->origin, p->faction, cost * FLY_TRADE_PUSH * 0.5f);
        /* the buy carries its own destination: a load with nowhere to go is how
           a pilot ends up shuttling the same crate on and off one pad */
        if (cmd->task >= 0 && cmd->task < world->nloc) p->dest = cmd->task;
        p->task = FLY_TASK_TRADING;
        return 0;
    }
    case FLY_PCMD_SELL: {
        fly_location *L;
        float px, qty;
        if (!on_pad(p)) return -2;
        if (cmd->a < 0 || cmd->a >= FLY_RES_COUNT || cmd->f <= 0.0f) return -3;
        qty = cmd->f < p->cargo[cmd->a] ? cmd->f : p->cargo[cmd->a];
        if (qty <= 0.0f) return -4;
        L = &world->loc[p->origin];
        px = fly_world_price(world, p->origin, (fly_resource)cmd->a);
        /* the same 8% the player's sale gives up, so neither side of the
           market is quietly a better place to stand than the other */
        p->tokens += (long)(px * qty * 0.92f);
        p->cargo[cmd->a] -= qty;
        L->stock[cmd->a] += qty;
        /* Landing a load is how a power takes a place. Not the only way — a
         * patrol on station and a gun count too — but the main one, and
         * deliberately so: the world is won by the aircraft that keep the
         * shelves full, which is what everybody in it is already doing. It
         * belongs in `apply` and not in the game's tick because it is a
         * consequence of a *command*, so an injected stream produces the same
         * front line as a lived one. */
        fly_faction_push(world, p->origin, p->faction, px * qty * FLY_TRADE_PUSH);
        if (p->cargo[cmd->a] < 0.5f && cmd->a == p->deal_res) {
            p->deal_res = -1;
            p->deal_origin = -1;
            p->deal_qty = 0.0f;
            ++p->hauls;
            ++p->haul_done;   /* the caller folds this into the world total */
        }
        p->task = FLY_TASK_TRADING;
        return 0;
    }
    case FLY_PCMD_REFUEL: {
        fly_location *L;
        float need, qty, cost;
        if (!on_pad(p)) return -2;
        L = &world->loc[p->origin];
        need = p->airframe.fuel_cap - p->craft.fuel;
        if (need <= 0.5f) return 0;
        qty = need < L->stock[FLY_RES_FUEL] ? need : L->stock[FLY_RES_FUEL];
        if (qty < 0.5f) return -4;
        cost = qty * fly_world_price(world, p->origin, FLY_RES_FUEL) + 1.0f;
        if ((float)p->tokens < cost) {
            qty *= (float)p->tokens / cost;
            cost = (float)p->tokens;
            if (qty < 0.5f) return -5;
        }
        p->tokens -= (long)cost;
        L->stock[FLY_RES_FUEL] -= qty;
        p->craft.fuel += qty;
        p->task = FLY_TASK_SERVICE;
        return 0;
    }
    case FLY_PCMD_REPAIR: {
        fly_location *L;
        float dmg, parts_need, parts_used;
        long labor;
        if (!on_pad(p)) return -2;
        L = &world->loc[p->origin];
        dmg = 1.0f - p->craft.hp / (p->airframe.structure + 1e-3f);
        if (dmg < 0.02f && p->craft.wear < 0.02f) return 0;
        parts_need = ceilf((dmg + p->craft.wear) * 6.0f);
        parts_used = parts_need < L->stock[FLY_RES_PARTS] ? parts_need : L->stock[FLY_RES_PARTS];
        labor = (long)((parts_need - parts_used) * 30.0f) + 15;
        if (p->tokens < labor) return -4;
        p->tokens -= labor;
        L->stock[FLY_RES_PARTS] -= parts_used;
        p->craft.hp = p->airframe.structure;
        p->craft.wear = 0.0f;
        p->task = FLY_TASK_SERVICE;
        return 0;
    }
    case FLY_PCMD_SET_LEG:
        if (cmd->a < 0 || cmd->a >= world->nloc) return -2;
        if (cmd->task < 0 || cmd->task >= FLY_TASK_COUNT) return -3;
        p->dest = cmd->a;
        p->task = (fly_pilot_task)cmd->task;
        p->mark = cmd->mark;
        p->mark_player = cmd->mark_player;
        p->mark_convoy = cmd->mark_convoy;
        p->leg_start = time_s;
        p->leg_eta = time_s + (double)(leg_km(world, p->origin, cmd->a) * 1000.0f /
                                       FLY_PILOT_CRUISE);
        return 0;
    default:
        return -1;
    }
}

/* ---------------- stepping ----------------
 *
 * Two fidelities, one behaviour. `detail` decides whether the aircraft is flown
 * or merely moved; everything else — fuel, the clock on a pad, the market — is
 * identical, because a world that only runs where you are looking is a world
 * that resets every time you turn around. */

/* Where a coarsely-flown pilot is heading this instant, and how fast.
 *
 * A hunt has to work out here too. The coarse step used to fly the leg and
 * nothing else, so an outlaw that had decided to attack somebody kept sedately
 * on course for its next pad — which meant combat only ever happened inside the
 * seven kilometres around the camera that get the full simulation, and the map
 * beyond it was a place where nobody was ever actually attacked. A hunter
 * steers at its mark and runs the throttle up; the rest is the same. */
static int coarse_goal(const fly_pilot *p, const fly_pilot_view *v,
                       const fly_world *w, fly_v3 *goal, float *speed) {
    *speed = FLY_PILOT_CRUISE;
    if (p->task == FLY_TASK_HUNT) {
        const fly_pilot *q = peer_by_id(v, p->mark);
        const fly_convoy *cv = convoy_by_id(v, p->mark_convoy);
        if (p->mark_player && v->player_airborne) *goal = v->player_pos;
        else if (cv) {
            /* Above the road, on a shallow dive onto it.
             *
             * The mark is a truck, and an aircraft that steers at the thing
             * itself is steering at the ground — out here there is no flight
             * law to pull out, only the clearance check, which turns the attack
             * into a crash. Aiming at a fixed height above it is the other
             * failure: the nose stays level, the column sits below the gun's
             * cone the whole way in, and the pass never converts. So the height
             * comes off the range: a one-in-seven glide that flattens as it
             * closes, which keeps the column in front of the guns and the
             * aeroplane off the road. */
            float run = hypotf(cv->pos.x - p->craft.pos.x, cv->pos.y - p->craft.pos.y);
            float up = run * 0.14f;
            *goal = cv->pos;
            goal->z += up < 90.0f ? up : 90.0f;
        } else if (q && !q->craft.on_ground) *goal = q->craft.pos;
        else return 0;
        /* a stern chase between two aircraft at the same cruise never closes:
           the hunter is at full throttle and the mark is not */
        *speed = FLY_PILOT_CRUISE * 1.45f;
        return 1;
    }
    if (p->task == FLY_TASK_EVADE) {
        const fly_pilot *q = peer_by_id(v, p->mark);
        const fly_convoy *cv = convoy_by_id(v, p->mark_convoy);
        fly_v3 from = p->mark_player ? v->player_pos
                    : cv ? cv->pos
                    : (q ? q->craft.pos : p->craft.pos);
        fly_v3 away = fly_v3sub(p->craft.pos, from);
        away.z = 0.0f;
        if (fly_v3len(away) < 1.0f) away = fly_v3mk(1, 0, 0);
        *goal = fly_v3add(p->craft.pos, fly_v3scale(fly_v3norm(away), 3000.0f));
        goal->z = p->craft.pos.z;
        *speed = FLY_PILOT_CRUISE * 1.15f;
        return 1;
    }
    (void)w;
    return 0;
}

/* Did this leave the aeroplane inside a hill?
 *
 * The coarse path could not hit anything, and that was not a simplification so
 * much as a different world: every flying accident in the game happened inside
 * the seven kilometres around the camera, so the loss rate the player saw was
 * not the loss rate anywhere else, and the flight law could only ever be
 * improved for the handful of aircraft being watched. It is not a stall model
 * and does not pretend to be — no aerodynamics out here — but running out of
 * climb in front of rising ground is the one accident this path has enough
 * state to have honestly, now that the climb is rate-limited. */
static void coarse_clearance(fly_pilot *p, const fly_world *w) {
    float gz = fly_world_ground(w, p->craft.pos.x, p->craft.pos.y);
    if (p->craft.on_ground) return;
    if (p->craft.pos.z > gz + p->airframe.gear_height) return;
    p->craft.pos.z = gz + p->airframe.gear_height;
    p->craft.crashed = 1;
}

static void step_coarse(fly_pilot *p, const fly_pilot_view *v, const fly_world *w,
                        float dt) {
    const fly_location *T;
    fly_v3 to, chase;
    float d, step, alt, speed;
    /* A saucer has no leg to fly. Everything below is "make progress along a
       route at cruise", which for something that has no destination and no
       cruise meant it was quietly flown down out of orbit toward the site it
       happened to be spawned against — eighteen kilometres of ground track in
       five minutes, and the whole point of the thing is that it stays put. Out
       of sight it does exactly what it does in sight: nothing. */
    if (p->kind == FLY_PILOT_UFO) return;
    if (p->dest < 0 || p->dest >= w->nloc) return;
    /* A takeoff out of sight is a takeoff all the same. Without this a pilot
       that decided to fly somewhere while the player was elsewhere sat at full
       throttle on its pad for ever, because the coarse step only ever moved
       aircraft that were already up. */
    if (p->craft.on_ground) {
        if (p->controls.throttle < 0.5f) return;
        p->craft.on_ground = 0;
        p->craft.pos.z = fly_world_ground(w, p->craft.pos.x, p->craft.pos.y) +
                         FLY_PILOT_CRUISE_ALT * 0.35f;
        p->craft.air_time = 0.0f;
    }
    if (coarse_goal(p, v, w, &chase, &speed)) {
        fly_v3 delta = fly_v3sub(chase, p->craft.pos);
        float cd = fly_v3len(delta);
        if (cd > 1.0f) {
            fly_v3 dir = fly_v3scale(delta, 1.0f / cd);
            float move = speed * dt;
            p->craft.pos = fly_v3add(p->craft.pos, fly_v3scale(dir, move < cd ? move : cd));
            p->craft.vel = fly_v3scale(dir, speed);
            /* Nose along the track. Yaw alone out here, with one exception.
             *
             * Every aircraft a coarse pilot chases is at roughly its own
             * altitude, so a level nose is a good enough approximation and it
             * is the one this path has always used — changing it for everybody
             * would quietly re-tune every engagement in the world that happens
             * out of sight. A column on a road is the exception and the only
             * one: it is a hundred metres below the aeroplane attacking it, so
             * a level nose points the guns at the horizon and the one target
             * that never manoeuvres becomes the one target this path cannot
             * hit. The sign convention is the flight model's — pitch up is
             * negative about the body y. */
            p->craft.ori = fly_qeuler(0,
                p->mark_convoy && p->task == FLY_TASK_HUNT
                    ? -asinf(fly_clampf(dir.z, -1.0f, 1.0f)) : 0.0f,
                atan2f(dir.y, dir.x));
        }
        p->craft.fuel -= p->airframe.burn_rate * 0.95f * dt;
        if (p->craft.fuel < 0.0f) p->craft.fuel = 0.0f;
        p->craft.wear += dt * 1.4e-5f;
        coarse_clearance(p, w);
        return;
    }
    T = &w->loc[p->dest];
    alt = cruise_alt(w, p, p->dest);
    {   /* Come down on the approach. Flying the whole leg at cruise and
           stopping dead over the pad leaves an aircraft hovering at six hundred
           metres for ever, because the arrival test quite rightly wants it low
           before it will call it landed. */
        float plan = fly_world_dist(T->pos, fly_wpos_of(p->craft.pos));
        if (plan < 2200.0f) {
            float k = plan / 2200.0f;
            float floor_z = fly_world_ground(w, p->craft.pos.x, p->craft.pos.y) + 40.0f;
            alt = (T->pad_z + 55.0f) * (1.0f - k) + alt * k;
            if (plan > FLY_PILOT_DOCK_R && alt < floor_z) alt = floor_z;
        }
    }
    /* Horizontally at cruise, vertically at a climb rate it could actually
       hold. Moving straight at a 3-D point let a coarse aircraft gain six
       hundred metres as fast as the geometry asked for, which is what made this
       path incapable of hitting anything: it simply rose over every ridge.
       Nine metres a second up is what the flight law's own demand clamps to. */
    {   fly_v2 q = fly_world_delta(T->pos, fly_wpos_of(p->craft.pos));
        to = fly_v3mk(q.x, q.y, 0.0f); }
    d = fly_v3len(to);
    step = FLY_PILOT_CRUISE * dt;
    if (d <= step || d < 1.0f) {
        p->craft.pos.x = T->pos.e;
        p->craft.pos.y = T->pos.n;
        p->craft.vel = fly_v3mk(0, 0, 0);
    } else {
        fly_v3 dir = fly_v3scale(to, 1.0f / d);
        p->craft.pos = fly_v3add(p->craft.pos, fly_v3scale(dir, step));
        p->craft.vel = fly_v3scale(dir, FLY_PILOT_CRUISE);
        p->craft.ori = fly_qeuler(0, 0, atan2f(dir.y, dir.x));
    }
    {
        float dz = alt - p->craft.pos.z;
        float rate = (dz > 0.0f ? 9.0f : 12.0f) * dt;
        p->craft.pos.z += fly_clampf(dz, -rate, rate);
        p->craft.vel.z = fly_clampf(dz, -rate, rate) / (dt > 1e-4f ? dt : 1e-4f);
    }
    /* burn as if cruising: a coarse pilot still runs its tanks down and still
       has to stop for fuel, so the economy sees the same demand either way */
    p->craft.fuel -= p->airframe.burn_rate * 0.72f * dt;
    if (p->craft.fuel < 0.0f) p->craft.fuel = 0.0f;
    p->craft.wear += dt * 1.0e-5f;
    coarse_clearance(p, w);
}

void fly_pilot_step(fly_pilot *p, const fly_pilot_view *v, fly_weather *wx,
                    fly_world *world, fly_rng *rng, int detail, float dt) {
    if (!p->active) return;
    p->detail = detail;
    if (p->pad_until > 0.0f) p->pad_until -= dt;
    if (p->think_in > 0.0f) p->think_in -= dt;
    p->distress = p->distress > dt * 0.35f ? p->distress - dt * 0.35f : 0.0f;
    if (p->fire_cooldown > 0.0f) p->fire_cooldown -= dt;

    if (on_pad(p)) return; /* the pad clock is the whole of a parked pilot */

    if (detail) {
        fly_sim_step(&p->craft, &p->airframe, &p->controls, wx, fly_world_ground_cb,
                     (void *)world, fly_pilot_cargo_mass(p), rng, dt);
    } else {
        step_coarse(p, v, world, dt);
    }
    /* the same fold the player's craft takes — see fly_game_step */
    {
        fly_wpos w = fly_world_chart_wrap(fly_wpos_of(p->craft.pos));
        p->craft.pos.x = w.e;
        p->craft.pos.y = w.n;
    }
    if (p->craft.fuel <= 0.0f && !p->craft.on_ground && p->craft.pos.z <
        fly_world_ground(world, p->craft.pos.x, p->craft.pos.y) + 2.0f)
        p->craft.crashed = 1;
}
