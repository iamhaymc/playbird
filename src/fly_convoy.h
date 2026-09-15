/* fly_convoy: the freight that runs on the ground and on the water.
 *
 * A convoy is the surface's answer to a courier: a column of trucks — or a
 * hull, which is the same thing with a lane under it instead of a road —
 * carrying a settlement's surplus to a settlement that is short of it, moving
 * the same market stock a pilot's delivery moves. That is what makes it worth
 * attacking and what makes attacking it cost something: a burnt column is
 * freight that never arrives, and somebody notices.
 *
 * The medium is one flag and two numbers. A coaster is this struct with `sea`
 * set, a class table of its own and a fly_way built from fly_sea's lane rather
 * than fly_road's carriageway; everything that made a column interesting —
 * where the tier comes from, who is allowed to shoot at it, what it sheds when
 * it burns, what the market does when it fails to arrive — is written once and
 * does not ask what is under the wheels.
 *
 * It is deliberately *not* a pilot. A pilot emits commands because a networked
 * human can take its seat and must not be able to tell the difference; nobody
 * is ever going to be seated in a truck, so a convoy is a thing the world does
 * rather than an actor with intent, and it has a step instead of a mind. What
 * it does share with a pilot is everything that matters at the trigger: a tier
 * that says how hard it is, guns that answer, and a wreck worth walking out to.
 *
 * Tiers are drawn from how dangerous the country the way crosses is, which is
 * the same field the whole loot economy hangs off (see fly_world_danger). The
 * big columns run where it is worth escorting them, and what they shed is worth
 * as much as they were hard — so the ladder is the map, not a difficulty dial.
 *
 * Transient like the traffic in the sky: convoys are not saved. They are
 * rebuilt from the networks and the market the moment the world runs again. */
#ifndef FLY_CONVOY_H
#define FLY_CONVOY_H

#include "fly_line.h"
#include "fly_rng.h"
#include "fly_road.h"
#include "fly_sea.h"
#include "fly_sim.h"
#include "fly_world.h"

#define FLY_CONVOY_MAX 12

typedef enum {
    FLY_CONVOY_DRAY,       /* a pair of flatbeds and a rifle */
    FLY_CONVOY_HAULER,
    FLY_CONVOY_ARMOURED,
    FLY_CONVOY_LEVIATHAN,  /* an ore train with its own air defence */
    FLY_CONVOY_TIER_COUNT
} fly_convoy_tier;

/* And the same ladder afloat. Four rungs again, and deliberately: a column is
 * a column whatever is under it, so the tier a stretch of country deserves,
 * the loot it sheds and the trouble it is to attack all read off one number
 * either way. What differs is the numbers on the rungs — a hull is one vessel
 * rather than a file of trucks, it carries several times the freight, it takes
 * far more killing and it is very much slower, which is the whole difference
 * between robbing a lorry and robbing a ship. */
typedef enum {
    FLY_SHIP_LIGHTER,      /* barges under tow, and nobody aboard armed */
    FLY_SHIP_COASTER,
    FLY_SHIP_FREIGHTER,
    FLY_SHIP_ORE_CARRIER,  /* a bulk hull with its own air defence */
    FLY_SHIP_TIER_COUNT
} fly_ship_tier;

/* What a tier *is*, in one table.
 *
 * Every number a convoy has comes from here, so the thing that spawns them, the
 * thing that shoots at them, the thing that draws them and the thing that
 * decides what falls out are reading one definition instead of four opinions.
 * Read down any column and the ladder is the claim: bigger columns are longer,
 * tougher, better armed, slower, and worth more. */
typedef struct {
    const char *name;
    int trucks;        /* vehicles in the column */
    float hull;        /* hit points per truck */
    float armour;      /* 0..1 of incoming shrugged off */
    float gun_dps;     /* what the escort puts up at aircraft */
    float gun_range;
    float speed_mul;   /* of the road's own speed: weight costs pace */
    float cargo;       /* units of the resource per truck */
    float loot_bias;   /* rarity push on what it sheds, as fly_game_roll_item */
    int loot_rolls;    /* modules thrown clear when it burns */
    /* Metres between one vehicle's nose and the next one's. On the table
     * rather than in a constant because it is the one thing about the shape of
     * a column that a medium changes outright: lorries run at a truck's length
     * and a bit, and a tow runs at a cable's length astern. Everything that
     * asks how long a column is reads this, so the drawing and the spacing
     * rule cannot disagree about where the tail of one is. */
    float gap;
} fly_convoy_class;

/* The rung, by medium and tier. `sea` picks the ladder: there are two tables
 * and one shape, so everything that reads a column — the thing that spawns it,
 * the thing that shoots it, the thing that draws it and the thing that decides
 * what falls out — is reading one definition rather than five opinions. */
const fly_convoy_class *fly_convoy_class_get(int sea, int tier);
const char *fly_convoy_tier_name(int sea, int tier);

/* --- the line a column runs on ---------------------------------------------
 *
 * A convoy does not care what is under it. It needs to know where its line
 * goes, how long it is, how fast the surface will carry it and which two
 * settlements are at the ends — and a carriageway and a shipping lane answer
 * all four. So they are handed over as the same thing, and the column is
 * written once instead of twice.
 *
 * A view rather than a copy: it points at stations the network owns, and it is
 * built fresh at each call from whichever net the column is on. Nothing here
 * outlives the call it was made for. */
typedef struct {
    const fly_line_point *points;
    int count;
    float length;
    float speed;      /* what the surface itself will carry, m/s */
    int from, to;     /* the settlements at either end */
    int sea;          /* which ladder, and which side of the way to keep */
} fly_way;

/* The way a road is, and the way a lane is. Both return 0 when there is no
 * such line, in which case `out` is left zeroed and every call below refuses
 * it in the usual way. */
int fly_road_as_way(const fly_road_net *net, int road, fly_way *out);
int fly_sea_as_way(const fly_sea_net *sea, int lane, fly_way *out);

typedef struct {
    int active;
    uint32_t id;
    char name[20];
    unsigned char tier;
    unsigned char faction;   /* whoever holds the site it left */
    /* Which network, and which line of it. A column on the water is the same
       struct as a column on a road — see fly_way, and the second class table
       above — so this pair is the whole of the difference. */
    unsigned char sea;
    int line;                /* index into that network */
    int reverse;             /* running to->from rather than from->to */
    float distance;          /* metres along the way */
    float speed;             /* m/s it is making now */
    float hp, hp_max;
    int trucks;              /* still rolling; the column shortens as it burns */
    int res;                 /* fly_resource aboard */
    float qty;               /* units still aboard */
    float fire_cooldown;
    /* The launcher on the bed, and what is left in it.
     *
     * Only the top two rungs carry one — see FLY_CONVOY_SAM_TIER — and that is
     * what turns the ladder from "bigger columns hurt more" into "bigger
     * columns are a different problem". Flak is a tax on the seconds you spend
     * over the road and you can simply leave; this follows you, and the answer
     * to it is a flare and a turn rather than a throttle. A finite magazine is
     * what keeps a Leviathan from being a no-fly zone: outlast it and the road
     * is open. */
    int sam_ammo;
    float sam_cooldown;
    float alert;             /* 0..1, decays: shot at recently, so shooting back */
    /* The player has fired on this column. A flag rather than a decay, because
     * "they shot at us" is not something an escort forgets halfway down the
     * road, and because an unaligned operator has no other way of becoming a
     * target — see convoy_will_fire_player. */
    unsigned char provoked;
    uint32_t seed;           /* stable livery and wreck pattern */
    fly_v3 pos, tangent;
} fly_convoy;

/* Put a column on `way`, running whichever way the freight is wanted, with the
 * tier the country deserves. `line` is its index in whichever network the way
 * came out of, which is what the column remembers. Returns 0, or negative when
 * there is nothing worth hauling either way along it. Draws from `rng`. */
int fly_convoy_spawn(fly_convoy *c, uint32_t id, const fly_way *way,
                     const fly_world *world, int line, fly_rng *rng);

/* Which settlement it left and which it is bound for. */
int fly_convoy_origin(const fly_convoy *c, const fly_way *way);
int fly_convoy_destination(const fly_convoy *c, const fly_way *way);

/* Take the freight off the origin, and put it down at the destination. The two
 * halves of a delivery, and the only things here that move the market — so a
 * column that never arrives is stock that never arrives. */
void fly_convoy_load(fly_convoy *c, fly_world *world, const fly_way *way);
void fly_convoy_unload(const fly_convoy *c, fly_world *world, const fly_way *way);

/* Roll it along. Returns 1 on the step it reaches the far end. */
int fly_convoy_step(fly_convoy *c, const fly_way *way, float dt);

/* Take a hit. Returns 1 when this is the one that finishes the column. */
int fly_convoy_damage(fly_convoy *c, float damage);

/* Aim quality 0..1 of an aircraft's guns on the column, or 0 outside the cone.
 * A wider cone than air-to-air on purpose: a column of trucks on a road is a
 * long, slow, straight target, and the difficulty of a strafing run is holding
 * the dive, not finding the aeroplane. */
float fly_convoy_gun_solution(const fly_craft *shooter, const fly_airframe *af,
                              const fly_convoy *c);

/* And the answer: aim quality 0..1 of the escort's air defence on something
 * flying at `pos` with velocity `vel`.
 *
 * Flak tracks, which is why what it does per second is nothing like what an
 * aircraft's guns do — an air-to-air burst needs a firing solution that almost
 * never holds, and this one holds for as long as you are over the road. It
 * falls off with range and with how fast the target is crossing, so a strafing
 * run is survivable and a loiter over the column is not. */
float fly_convoy_air_solution(const fly_convoy *c, fly_v3 pos, fly_v3 vel);
/* What that is worth per second: the whole column's fire, thinned by losses. */
float fly_convoy_threat(const fly_convoy *c);

/* Where the column's `i`th vehicle is, nose first, in its own lane rather than
 * on the centreline — see fly_road_lane and fly_sea_side, which are where the
 * side of the way traffic keeps to is decided. Public because the renderer and
 * the collision-free spacing rule must not disagree about how long a column is
 * or where its tail sits. */
int fly_convoy_truck(const fly_convoy *c, const fly_way *way, int i,
                     fly_v3 *pos, float *yaw);

/* The lowest rung that carries a missile, and how far it will send one.
 *
 * Well beyond the flak's reach on purpose: the whole point of the thing is that
 * a column can answer an aircraft that has stood off outside gun range, which
 * is the tactic flak alone has no reply to. */
#define FLY_CONVOY_SAM_TIER FLY_CONVOY_ARMOURED
#define FLY_CONVOY_SAM_RANGE 2800.0f
/* Rounds on the bed, by tier above the threshold. Two for an armoured column
 * and four for a train. */
#define FLY_CONVOY_SAM_LOAD(tier) (2 * (1 + (int)(tier) - (int)FLY_CONVOY_SAM_TIER))

#endif /* FLY_CONVOY_H */
