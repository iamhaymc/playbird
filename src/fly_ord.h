/* fly_ord: the things in the air between two aircraft.
 *
 * Everything the world shoots that is not hitscan. A gun is a line drawn
 * instantly between a nose and a target and it is resolved where the trigger is
 * pulled; a rocket, a bomb, a seeker and a flare are *objects*, with a position,
 * a velocity and several seconds of life in which the situation can change out
 * from under them. That difference is the whole point of this module: it is what
 * makes a shot something you can be inside of and get out of.
 *
 * It is deliberately not an actor. Like a convoy, ordnance is a thing the world
 * does rather than something with intent — nobody is ever going to be seated in
 * a missile — so it has a step instead of a mind, and it emits no commands. The
 * decision to fire is a control latch on the aircraft that fired (`launch`,
 * `decoy`), which is the same door the player and the AI already share, so
 * nothing here touches the command vocabulary or the multiplayer seam.
 *
 * Two rules bind everything below:
 *
 * 1. **No random numbers, anywhere.** A seeker does not roll to see whether a
 *    flare fooled it; it looks at what is in front of it and flies at the
 *    brightest thing. Every outcome is geometry, energy and timing, which is
 *    what makes the counterplay learnable rather than a coin the game flips
 *    behind your back — and it also means ordnance cannot perturb the
 *    simulation's rng stream, so a client that draws none of it stays in step
 *    with one that draws all of it.
 *
 * 2. **A launch is survivable by flying.** Every guided round burns for a few
 *    seconds and then coasts, pulls a bounded number of g, and loses track of
 *    what it cannot see. Out-turn it, out-run it, go cold, put a ridge between
 *    the two of you, or spend a flare — five answers, and none of them is a
 *    number on the loadout screen.
 *
 * Transient like the traffic: ordnance in flight is not saved. A round that was
 * mid-air when the game was written out was going to hit or miss within eight
 * seconds, and a save that restored it would be restoring a threat the player
 * cannot see the origin of. Magazines *are* saved, because they are what the
 * airframe is carrying (see fly_owned_airframe).
 */
#ifndef FLY_ORD_H
#define FLY_ORD_H

#include "fly_math.h"
#include "fly_sim.h"

/* What is in the air.
 *
 * The order is the ladder the registry follows and the order the HUD lists
 * them in: guns first (not here — hitscan), then the unguided, then the guided,
 * then the beam, and the countermeasures last because they are the answer to
 * the two in the middle rather than a weapon of their own. */
typedef enum {
    FLY_ORD_NONE = 0,
    /* Unguided, fired in a salvo, fast and flat. No lead problem the shooter
     * cannot solve by pointing, and no answer for a target that turns. */
    FLY_ORD_ROCKET,
    /* Dropped, not fired. Gravity and drag, a fat blast where it lands, and
     * nothing at all to offer against an aeroplane. */
    FLY_ORD_BOMB,
    /* The slow one. Follows heat, and everything about it is built so that the
     * launch is the beginning of the engagement rather than the end. */
    FLY_ORD_SEEKER,
    /* The fast one. Follows the shooter's radar, which means the shooter has to
     * keep looking at you — and a shooter that keeps looking at you is a
     * shooter flying predictably. */
    FLY_ORD_DART,
    /* Ground-launched heat, off a column that has had enough of being strafed.
     * The same guidance as a seeker with a heavier motor and no aeroplane
     * behind it. */
    FLY_ORD_SAM,
    /* Burning metal, thrown clear. Not a weapon: a brighter heat source than
     * an engine for about as long as it takes to turn. */
    FLY_ORD_FLARE,
    /* A cloud of foil, which to a radar looks like an aeroplane that has
     * stopped dead in the air. */
    FLY_ORD_CHAFF,
    FLY_ORD_KIND_COUNT
} fly_ord_kind;

/* Who a round belongs to, and what it is chasing. A tag plus an id rather than
 * a pointer, because the thing it was aimed at may be gone by the time it
 * arrives — which is a normal outcome and not an error. */
typedef enum {
    FLY_ORD_ACTOR_NONE = 0,
    FLY_ORD_ACTOR_PLAYER,
    FLY_ORD_ACTOR_PILOT,   /* id = fly_pilot.id */
    FLY_ORD_ACTOR_CONVOY,  /* id = fly_convoy.id */
    /* A burning flare or a cloud of foil, which a seeker can perfectly well
     * decide to fly at and therefore has to be able to *name*. Its own kind
     * rather than ACTOR_NONE: with the decoys filed under "nobody", a round
     * that took the bait stored a target it could never look up again, lost
     * guidance entirely and flew straight on. That reads almost the same from
     * outside — the aeroplane lives — and it is not the same mechanic. A
     * missile is supposed to visibly go and chase the flare. */
    FLY_ORD_ACTOR_DECOY    /* id = the decoy round's own fly_ord.id */
} fly_ord_actor;

typedef struct {
    unsigned char kind;    /* fly_ord_actor */
    uint32_t id;
} fly_ord_ref;

/* One round in flight.
 *
 * Kept flat and copyable: the pool is a plain array on the game, the same shape
 * `blasts` and `ground_items` already use, so there is no ownership question
 * and nothing to free. */
typedef struct {
    int active;
    uint32_t id;
    unsigned char kind;    /* fly_ord_kind */
    unsigned char faction; /* whose it is, for who it may hurt */
    fly_ord_ref owner;     /* who fired it: never its own target */
    fly_ord_ref target;    /* what it is guiding on, or ACTOR_NONE for unguided */
    fly_v3 pos, vel;
    float age;             /* seconds since launch */
    float life;            /* seconds until it gives up and self-destructs */
    float burn;            /* seconds of motor left; drag alone after that */
    /* The speed the motor will take it to: the launcher's own contribution on
     * top of whatever the platform was already doing. Stored rather than looked
     * up because it is a property of *this* shot — a rocket fired from something
     * fast really does arrive sooner — and because everything that decays after
     * burnout is measured against it. A round at half its own top speed pulls
     * half the g, and that ratio is meaningless without knowing what the top
     * was for this one. */
    float vmax;
    float damage;          /* at the centre of the blast */
    float blast;           /* metres: radius over which that damage falls to 0 */
    /* How hard it can turn, m/s^2 of lateral acceleration. Zero for anything
     * unguided, and the single number that decides whether a target can simply
     * out-turn the shot. */
    float turn;
    /* Seeker head half-angle, radians. Outside it the round is blind, which is
     * what makes a flare popped across the line of sight work and a flare
     * popped straight ahead of the missile useless. */
    float seek_cone;
    /* Signature of this round *as a target*, for the countermeasures. A flare
     * is a very bright heat source and a chaff cloud is a very bright radar
     * one; everything else is 0 and cannot be locked on to. */
    float decoy_heat, decoy_radar;
    /* Set once the round has flown far enough from the muzzle to be dangerous.
     * A proximity fuse that armed instantly would detonate a bomb inside the
     * bay it left and a seeker on the aeroplane that fired it. */
    int armed;
    uint32_t seed;         /* stable smoke and flame pattern for the renderer */
} fly_ord;

/* How many rounds the world will track at once. Generous: a four-round salvo
 * from each of two aircraft, a stick of bombs, and both sides' countermeasures
 * are about thirty, and the pool evicts the oldest rather than refusing — a
 * dropped round is invisible, where a stuck one is a permanent threat. */
#define FLY_ORD_MAX 64

/* What a launcher is, in one table.
 *
 * Every number a round carries comes from here, so the thing that fires them,
 * the thing that draws them and the thing that decides what they do to an
 * airframe are reading one definition instead of three opinions — the same
 * arrangement `fly_convoy_class` makes for the columns. A module names a kind
 * and the kind says the rest; a module's own rolls then scale damage and
 * magazine, which is what rarity on a launcher means. */
typedef struct {
    const char *name;
    const char *tag;       /* four characters, for the HUD's magazine readout */
    float speed;           /* m/s at burnout, or muzzle speed for the unguided */
    float burn;            /* seconds of motor */
    float life;            /* seconds before it gives up */
    float turn;            /* m/s^2 of lateral pull; 0 = unguided */
    float seek_cone;       /* radians, half-angle; 0 = not a seeker */
    float damage;
    float blast;           /* metres */
    float arm_time;        /* seconds before the fuse is live */
    float drag;            /* 1/m: speed lost to the air, per metre flown */
    int salvo;             /* rounds per pull of the trigger */
    float reload;          /* seconds between pulls */
} fly_ord_class;

const fly_ord_class *fly_ord_class_get(int kind);
const char *fly_ord_kind_name(int kind);
/* Is this kind guided by heat, or by the shooter's radar? The two
 * countermeasures ask constantly, and the answer has to be one function or a
 * flare ends up decoying a radar dart. */
int fly_ord_is_heat(int kind);
int fly_ord_is_radar(int kind);
/* Does this kind fall out of the sky rather than fly? Bombs arc and everything
 * else drives, which changes the launch envelope, the aim and the drawing. */
int fly_ord_is_ballistic(int kind);

/* --- what a target looks like to a seeker -------------------------------
 *
 * The two functions the whole counterplay hangs off, and both are pure geometry
 * and airmanship rather than a die.
 *
 * Heat is the engine, seen from where the seeker is: a plume is enormous from
 * behind and nearly nothing head-on, and an aeroplane at idle is dim from every
 * angle. So the reliable shot is from the rear quarter, and pulling the
 * throttle back is a real answer to being shot at — expensive, because it is
 * also how you keep flying.
 *
 * `throttle` is the spooled setting and `aspect` is where the seeker is sitting
 * relative to where the target is pointing: 1 is dead astern, looking up the
 * tailpipe, and -1 is head-on. */
float fly_ord_heat_signature(float throttle, float speed, float aspect, float alt);
/* And the radar one, which does not care where the tail is pointing — it cares
 * how big the thing is and how fast it is crossing. A target flying straight at
 * or away from the emitter is the easy one to hold; a target crossing hard is
 * the one that falls out of the gate, which is why a hard beam turn is the
 * answer to a dart and a flare is not. */
float fly_ord_radar_signature(float mass, fly_v3 to_emitter, fly_v3 vel);

/* --- the step -----------------------------------------------------------
 *
 * Ordnance is advanced in one pass, against one snapshot of everything it might
 * be guiding on. The caller assembles that snapshot; this module never reaches
 * into the game, which is what keeps it testable on its own and is why the
 * seeker logic can be measured without building a world around it. */

/* One thing a round can be flying at or into. The caller fills an array of
 * these each step from the player, the pilots, the columns and the other
 * rounds — a flare is a target like any other, and that is exactly how a
 * decoy works here rather than through a special case.
 *
 * What it carries is the *cause* of a signature rather than a signature: a
 * throttle setting and a nose, not a number. It has to, because how bright a
 * target is depends on where the round asking is sitting — an aeroplane is an
 * inferno from behind and almost nothing from in front — and one precomputed
 * number per contact would have to pick an answer before knowing the question.
 * Each round works out for itself what it can see, which is also why two
 * missiles converging from different quarters can disagree about which is the
 * better mark. */
typedef struct {
    fly_ord_ref ref;
    unsigned char faction;
    fly_v3 pos, vel;
    fly_v3 nose;        /* unit heading: aspect is measured against this */
    float throttle;     /* 0..1, spooled: what the engine is actually doing */
    float mass;         /* kg, for the radar cross-section */
    /* A decoy's whole contribution, and the only thing it has. Non-zero here
     * replaces the computed signature outright — burning metal has no throttle
     * setting and no tail to hide. */
    float heat_decoy, radar_decoy;
    float radius;       /* metres; 0 = nothing solid here to hit (a decoy) */
    int airborne;       /* may a seeker lock it: a truck on a road is cold */
    /* Jamming, 0..1: how wrong this contact can make a seeker's idea of where
     * it is. Not a shield and not a probability — it displaces the *apparent*
     * position along the track, so a jammed target is one the round arrives
     * near rather than at, and a proximity fuse still does something about it.
     * That is deliberate: an answer that sometimes deleted the missile outright
     * would be the die this whole module is built without, where a bigger miss
     * distance is a thing the player can see happening and can add a turn to. */
    float ecm;
} fly_ord_contact;

/* Line of sight between two points, against the terrain.
 *
 * A seeker that can see through a hill is a seeker with no counterplay in the
 * one place a pilot instinctively looks for it. Sixteen samples along the
 * segment: the relief that hides an aeroplane is a ridge line, hundreds of
 * metres across, and this runs for a handful of rounds a frame. */
int fly_ord_line_of_sight(fly_ground_fn ground, void *guser, fly_v3 a, fly_v3 b);

/* Advance one round against the contacts.
 *
 * Returns the index of the contact it struck, or -1 if it is still flying, or
 * -2 if this is the step it expired or hit the ground. On -1 the round may
 * still have changed its mind about what it is chasing: re-targeting onto a
 * flare happens here and is visible in `o->target` afterwards, which is what
 * the tests read.
 *
 * `wind` moves the light things — a flare hangs in the air and drifts, which is
 * how a decoy separates from the aeroplane that dropped it. */
int fly_ord_step(fly_ord *o, const fly_ord_contact *contacts, int contact_count,
                 fly_ground_fn ground, void *guser, fly_v3 wind, float dt);

/* Damage this round does at a distance from its burst: full at the centre,
 * nothing at the rim, and a curve rather than a step so a near miss is a near
 * miss rather than a clean escape. */
float fly_ord_blast_damage(const fly_ord *o, float dist);

/* Set a round up. `speed` is the launcher's own contribution; the platform's
 * velocity is added by the caller, because a rocket fired from something doing
 * eighty already has eighty. */
void fly_ord_launch(fly_ord *o, uint32_t id, int kind, int faction, fly_ord_ref owner,
                    fly_ord_ref target, fly_v3 pos, fly_v3 vel, float damage_scale);

/* --- what an aircraft is carrying ---------------------------------------
 *
 * A launcher is fitted, and then it is loaded. The two are separate because
 * they fail differently: an aeroplane with no rack cannot ever fire, and an
 * aeroplane with an empty rack can be rearmed at any field that will serve it.
 * Both live on the airframe rather than here — see fly_airframe.ord_kind and
 * fly_craft.ammo — and these are the questions the HUD, the AI and the trigger
 * all ask about them. */

/* Is `target` inside the envelope of a `kind` launched from `from` at `vel`?
 * The launch decision, and the same one for the player's HUD cue and the AI's
 * trigger, so a pilot cannot take a shot the player is told is impossible. */
int fly_ord_launch_ok(int kind, float range, fly_v3 from, fly_v3 vel,
                      fly_v3 target, fly_v3 target_vel);
/* Where an unguided round fired now would arrive, given the drop and the drag:
 * the continuously computed impact point the bombsight draws and the AI aims
 * with. Returns 0 if it never comes down inside `max_t` seconds. */
int fly_ord_impact_point(int kind, fly_v3 from, fly_v3 vel, fly_ground_fn ground,
                         void *guser, float max_t, fly_v3 *out);

#endif /* FLY_ORD_H */
