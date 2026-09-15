/* fly_pilot: the other people flying.
 *
 * A pilot is a participant in the world on the same footing as the player —
 * it owns an airframe, a craft, money and cargo, it picks work, flies to a
 * site, trades there and comes back with the profit. What it does *not* own is
 * a way of changing the world that the player does not also have: every
 * decision a pilot makes leaves it as a `fly_cmd`, and one function applies
 * those commands. That is the whole design.
 *
 * The reason is multiplayer. A remote client sends commands; an AI pilot emits
 * commands; if the two go through the same door then a networked human can
 * simply take over a slot and nothing downstream can tell. Any behaviour that
 * reached around the command and poked the state directly would be behaviour a
 * human could not perform, and that is where a world stops being coherent —
 * so `fly_pilot_think` returns intent and never mutates anything, and
 * `fly_pilot_apply` mutates and never decides. The `render.pilots` aspect holds
 * the two apart by recording one run's command stream and replaying it into a
 * second game with the AI switched off; if any behaviour leaks around the
 * command vocabulary, the two runs diverge and the test says so.
 *
 * Pilots exist across the whole map, not in a bubble around the camera. Only
 * the ones close enough to see fly the full six-degree-of-freedom simulation;
 * the rest advance along their leg at cruise speed. Both run the same task
 * machine and both move the same market stock, so a delivery three hundred
 * kilometres away still shifts the price the player is quoted when they land.
 */
#ifndef FLY_PILOT_H
#define FLY_PILOT_H

#include "fly_convoy.h"
#include "fly_ord.h"
#include "fly_sim.h"
#include "fly_world.h"

/* What a pilot is for. Kind picks the work it looks for and who it shoots at;
 * everything after that is the same machine. */
typedef enum {
    FLY_PILOT_TRADER,     /* buys where a good is cheap, sells where it is dear */
    FLY_PILOT_COURIER,    /* hauls what a site is short of, for the delivery fee */
    FLY_PILOT_PATROL,     /* flies a circuit and engages outlaws */
    FLY_PILOT_PIRATE,     /* hunts loaded traders, and the player */
    FLY_PILOT_PROSPECTOR, /* flies the far and the unvisited */
    /* Not one of us. Hangs above the atmosphere where nothing else can
     * reach, does not trade, does not land, and is carrying the only
     * things in the world worth going up for. */
    FLY_PILOT_UFO,
    FLY_PILOT_KIND_COUNT
} fly_pilot_kind;

/* Which power a pilot flies for. A `fly_faction`, and it comes off the site it
 * works out of rather than off its kind: a courier out of a Foundry refinery
 * flies for the Foundry, and if the Choir takes that refinery next week the
 * couriers coming out of it fly for the Choir. Kind says what a pilot *does*;
 * faction says whose side it is on, and the two are deliberately independent.
 *
 * Outlaws are the exception and always Corsairs, because that is not a job
 * somebody at a site hired them for. */

/* Where a pilot is in its errand. A task is a promise about what commands come
 * next, not a place the state can be forced into: `fly_pilot_think` reads the
 * task and the world and answers with one command. */
typedef enum {
    FLY_TASK_IDLE,     /* on a pad with nothing decided yet */
    FLY_TASK_TRADING,  /* on a pad, buying or selling */
    FLY_TASK_SERVICE,  /* on a pad, refuelling or repairing */
    FLY_TASK_TRANSIT,  /* airborne, bound for `dest` */
    FLY_TASK_HUNT,     /* airborne, closing on a mark */
    FLY_TASK_EVADE,    /* airborne, running from one */
    FLY_TASK_SURVEY,   /* airborne, out to look at somewhere remote */
    FLY_TASK_COUNT
} fly_pilot_task;

typedef struct {
    int active;
    uint32_t id;          /* stable across the session; 0 is never used */
    char name[20];
    fly_pilot_kind kind;
    int faction;   /* fly_faction */

    fly_airframe airframe;
    fly_craft craft;
    fly_controls controls;

    long tokens;
    float cargo[FLY_RES_COUNT];

    fly_pilot_task task;
    int home;             /* the site it works out of */
    int origin, dest;     /* the leg it is flying */
    int deal_res;         /* what it is carrying, or -1 */
    int deal_origin;      /* where it bought it: selling is for somewhere else */
    float deal_qty;
    long deal_cost;       /* what it paid, so a sale can be a profit or a loss */
    uint32_t mark;        /* pilot id it is hunting or running from; 0 = none */
    int mark_player;      /* hunting the player rather than a pilot */
    /* Convoy id it is attacking, 0 for none. A third field rather than a kind
     * tag on `mark`, because that is the shape `mark_player` already set and
     * two of a pattern is easier to read than one of each. Exactly one of the
     * three is ever set. */
    uint32_t mark_convoy;

    double leg_start;     /* game time this leg began */
    double leg_eta;       /* game time it expects to arrive (coarse sim) */
    float pad_until;      /* seconds of turnaround left on the pad */
    float think_in;       /* seconds until the next decision */
    float fire_cooldown;
    float distress;       /* 0..1, decays; drives the radio chatter */

    /* Rank, 0..3. Drawn from how dangerous the site it works out of is, so a
     * pilot is as hard as the country it flies in — and what it is carrying to
     * lose is worth as much as it was hard. Named on the radio from tier 2 up,
     * because an ace you cannot see coming is just a trader that killed you. */
    unsigned char tier;
    int detail;           /* set each step: 1 = flown in full, 0 = coarse */
    int hauls, kills;     /* what it has actually got done */
    int haul_done;        /* deliveries since the owner last drained this */
} fly_pilot;

/* Commands a pilot can issue. Deliberately a subset of the player's — a pilot
 * that could do something the player cannot would be a pilot the player cannot
 * be replaced by. Values are shared with fly_cmd_kind so a journal entry means
 * the same thing whichever side produced it. */
typedef enum {
    FLY_PCMD_NONE = 0,
    FLY_PCMD_CONTROLS,  /* fly: controls */
    FLY_PCMD_LAUNCH,    /* leave the pad */
    FLY_PCMD_DOCK,      /* a = location; only legal within the pad's reach */
    FLY_PCMD_BUY,       /* a = resource, f = qty */
    FLY_PCMD_SELL,      /* a = resource, f = qty */
    FLY_PCMD_REFUEL,
    FLY_PCMD_REPAIR,
    FLY_PCMD_SET_LEG,   /* a = destination site, and the task that goes with it */
    FLY_PCMD_KIND_COUNT
} fly_pilot_cmd_kind;

typedef struct {
    fly_pilot_cmd_kind kind;
    fly_controls controls;
    int a;
    float f;
    int task;             /* SET_LEG: the task to fly it under */
    uint32_t mark;        /* SET_LEG: who it concerns */
    int mark_player;
    uint32_t mark_convoy;
} fly_pilot_cmd;

/* Everything a decision is allowed to look at. Passed by value so `think`
 * cannot reach anything it should not: it sees the world, the clock, where the
 * player is and who else is flying, and nothing writable. */
typedef struct {
    const fly_world *world;
    double time_s;
    fly_v3 player_pos;
    int player_airborne;
    float player_threat;   /* the player's gun dps: pirates weigh this */
    /* Whose colours the player is wearing. A pilot cannot see how much hull
     * the player has left, but it can certainly see the flag — so an oath is
     * something the sky reacts to, and pledging to a power is the act that
     * makes that power's enemies interested in you. */
    int player_faction;
    const fly_pilot *peers;
    int peer_count;
    /* The freight on the roads. Visible to a decision for the same reason the
     * other aircraft are: a column is a fat, slow, loaded thing crossing open
     * country, and an outlaw that could not see one would be an outlaw ignoring
     * the best mark on the map. May be NULL in a world with no roads. */
    const fly_convoy *convoys;
    int convoy_count;
    /* What is in the air that is not an aeroplane. Visible for exactly one
     * reason: a pilot that could not see a missile could not do anything about
     * one, and a world where only the player gets to break and decoy is a world
     * where the AI is scenery with a gun. It is also the honest limit of what a
     * pilot may know — a round's position and heading, which is what a pair of
     * eyes and a warning receiver give you, and not who it is chasing. May be
     * NULL, in which case nobody defends and everybody still flies. */
    const fly_ord *ord;
    int ord_count;
} fly_pilot_view;

/* Is this round about to be `p`'s problem?
 *
 * A geometric test rather than a lookup of the round's own target, and that is
 * deliberate: a pilot cannot read a seeker's mind, it can see something small
 * and fast whose nose is pointed at it and closing. Which means it will
 * sometimes break and burn flares for a missile aimed at somebody else — and
 * that is the correct behaviour, not a bug to fix.
 *
 * Returns seconds to impact if it is a threat, or a negative number if it is
 * not, so a caller can weigh "in four seconds" against "in one". */
float fly_pilot_ord_threat(const fly_pilot *p, const fly_ord *o);

/* Decide. Pure: reads `p` and `v`, writes `out`, touches nothing. Returns 1 if
 * it wants a command applied this step, 0 for "carry on". `rng` advances, so
 * two identical worlds decide identically, which is what makes a replay exact. */
int fly_pilot_think(const fly_pilot *p, const fly_pilot_view *v, fly_rng *rng,
                    float dt, fly_pilot_cmd *out);

/* Apply. The only way a pilot's state or the market changes. `world` is
 * mutable because trading moves stock — that is the point of having them.
 * Returns 0 on success, negative if the command was not legal here. */
int fly_pilot_apply(fly_pilot *p, fly_world *world, double time_s,
                    const fly_pilot_cmd *cmd);

/* Advance one pilot. `detail` selects the fidelity: non-zero flies the full
 * simulation, zero moves it along its leg at cruise. Combat and the market are
 * the same either way. */
void fly_pilot_step(fly_pilot *p, const fly_pilot_view *v, fly_weather *wx,
                    fly_world *world, fly_rng *rng, int detail, float dt);

/* Set a pilot up at `home` with an airframe suited to its kind. */
void fly_pilot_spawn(fly_pilot *p, uint32_t id, fly_pilot_kind kind, int home,
                     const fly_world *world, fly_rng *rng);

const char *fly_pilot_kind_name(fly_pilot_kind k);
/* Does this one shoot at people, or is it cargo with a pilot in it? The
 * combat rules ask constantly, and the answer has to be one function: a world
 * where patrols machine-gun rival couriers is a different world from this one,
 * and the difference is exactly this predicate. */
int fly_pilot_is_combatant(const fly_pilot *p);
const char *fly_pilot_task_name(fly_pilot_task t);
const char *fly_pilot_cmd_name(fly_pilot_cmd_kind k);
/* aim quality 0..1 of `shooter` on `target`, or 0 outside the gun's cone */
float fly_pilot_gun_solution(const fly_craft *shooter, const fly_airframe *af,
                             const fly_craft *target);
/* the cargo a pilot is carrying, in kg — pirates pick the fattest mark */
float fly_pilot_cargo_mass(const fly_pilot *p);

#endif /* FLY_PILOT_H */
