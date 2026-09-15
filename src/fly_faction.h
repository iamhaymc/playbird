/* fly_faction: the powers, what they believe, who they are fighting, and
 * which ground they hold.
 *
 * Three things live here and they are one thing. A power has an identity —
 * a name, a livery, a creed. Two powers have a relation, and that relation
 * moves: this is not a fixed hostility table but a matrix that drifts toward
 * what their creeds imply and is pushed around by what actually happens, so a
 * world can end up at war along a line the last world was at peace on. And a
 * site has an owner and a grip on it, which is what the whole argument is
 * about.
 *
 * Two rules bind everything in this file.
 *
 * **Nothing here draws a random number.** Not one. Initial ownership and the
 * opening relations are hashes of the seed, the relations move on accumulated
 * pressure and elapsed hours, and a site changes hands when the arithmetic
 * says so. That is not fastidiousness: world-gen already consumed its stream
 * and every existing world would have been reseeded by a single extra draw,
 * and `think_rng` exists precisely because a client that does not run a system
 * must not fall out of step with one that does. A war nobody watched has to
 * come out the same as a war somebody did.
 *
 * **State lives where the thing it describes lives.** Who holds a site is a
 * property of the site, next to `discovered` and `stock`, so it saves with the
 * world and a query about a place is answered by the place. The relation
 * matrix is a property of the world. What the *player* thinks of anyone is in
 * `fly_game`, because that is the operator's opinion and it does not survive
 * them.
 */
#ifndef FLY_FACTION_H
#define FLY_FACTION_H

#include <stdint.h>

#include "fly_math.h"

/* Forward declarations rather than an include: fly_world.h includes *this*
 * header for the fields it puts on a location, so the dependency has to run
 * one way. The implementation includes fly_world.h and sees the whole thing. */
struct fly_world;

/* The powers.
 *
 * `FREE` is not one of them and is deliberately zero: it is the absence of a
 * flag — an unaligned pilot, a site nobody has taken yet — and zero-initialised
 * state should mean "nobody", never "the Concord". The numbering after it is
 * chosen so the three constants this replaced keep their values: civil was 0,
 * authority 1 and outlaw 2, so an old comparison that survived a rename still
 * means what it used to.
 *
 * Seven powers is a deliberate number. Three is a triangle and reads as
 * rock-paper-scissors; a dozen is a list nobody can hold in their head while
 * looking at a map. Seven leaves room for two blocs and a spoiler. */
typedef enum {
    FLY_FACTION_FREE = 0,     /* unaligned: the trade that answers to nobody */
    FLY_FACTION_CONCORD,      /* licences, patrols, the rule of the air */
    FLY_FACTION_CORSAIR,      /* the outlaws — and they want the map too */
    FLY_FACTION_FOUNDRY,      /* the industrial combine: ore, alloy, fuel */
    FLY_FACTION_LANTERN,      /* the utopian compact: luxuries, data, light */
    FLY_FACTION_HALCYON,      /* the courier line: routes, mail, schedules */
    FLY_FACTION_CINDER,       /* storm-belt salvagers, wreckers, scrap kings */
    FLY_FACTION_MERIDIAN,     /* the ruin-digging choir: old data, older gods */
    FLY_FACTION_COUNT
} fly_faction;

/* How two powers stand. Ordered, so a comparison against a threshold is legal
 * and the UI can colour by rank rather than by a switch. */
typedef enum {
    /* Combatants engage each other on sight — never the unarmed, whatever the
     * flag on them; see `prey_ok` in fly_pilot.c for the whole rule. */
    FLY_REL_WAR,
    /* Nobody shoots, and they are still taking each other's fields. Most of
     * the map spends most of its time here, and the band is deliberately wide
     * enough that an evening's events can push a pair either way. */
    FLY_REL_COLD,
    FLY_REL_PEACE,
    FLY_REL_PACT,     /* holdings are not contested and pilots are welcome */
    FLY_REL_COUNT
} fly_relation;

/* What comes out of a step, for whoever wants to say so on the radio. The
 * module logs nothing itself — it has no business knowing what a log is. */
typedef enum {
    FLY_FEVT_NONE = 0,
    FLY_FEVT_SEIZED,   /* `loc` changed hands: `a` took it from `b` */
    FLY_FEVT_WAR,      /* `a` and `b` fell to war */
    FLY_FEVT_PACT      /* `a` and `b` came to terms */
} fly_faction_event_kind;

typedef struct {
    int kind;   /* fly_faction_event_kind */
    int a, b;
    int loc;
} fly_faction_event;

/* The relation matrix. Symmetric, -1..1, and the diagonal is meaningless — a
 * power is not at war with itself and nothing may ask. */
typedef struct {
    float rel[FLY_FACTION_COUNT][FLY_FACTION_COUNT];
} fly_diplomacy;

/* --- identity --- */

const char *fly_faction_name(int f);   /* "The Foundry" */
const char *fly_faction_tag(int f);    /* "FNDY" — four letters, always */
const char *fly_faction_creed(int f);  /* one line of what they are for */
/* Livery, linear RGB. `colour` is the hull, `trim` the flashes, fin and
 * pennant border — the second colour is what stops four powers reading as four
 * shades of the same aeroplane at two kilometres. */
fly_v3 fly_faction_colour(int f);
fly_v3 fly_faction_trim(int f);
/* The power's colour *as a mark*: whichever of the two carries more hue.
 *
 * A livery and a map key are different jobs and one colour cannot do both. On
 * an aeroplane the hull is seen against sky and value separates it, so the
 * Concord flies near-white and reads perfectly. As a two-pixel dot on a chart,
 * a wash over terrain, or a flag against a hazy sky, near-white is nothing at
 * all — the first banner drew a white pennant that could not be seen from
 * three hundred metres, and the first chart washed the country round a Concord
 * city into what looked like a light leak.
 *
 * So everything that *marks* rather than paints goes through here, and every
 * power ends up with something that has a hue in it whatever its hull is. */
fly_v3 fly_faction_mark(int f);
/* The other one, for the border or second colour of whatever this is drawn
 * on: always the one `fly_faction_mark` did not return. */
fly_v3 fly_faction_countermark(int f);
/* Where a power sits on the two axes its creed is built from: `law` runs
 * outlaw (-1) to order (+1), `make` runs mystic (-1) to industrial (+1). The
 * opening relations are a function of these and nothing else, which is what
 * makes the diplomacy legible: powers fall out along the axis they differ on.
 */
float fly_faction_law(int f);
float fly_faction_make(int f);
/* Is this a power that can hold ground? False only for FREE. */
int fly_faction_is_power(int f);

/* --- diplomacy ---
 *
 * `fly_faction_temper` is where a pair *belongs* — the relation their creeds
 * imply, which is what the matrix drifts back toward once whatever happened
 * stops happening. Everything else is the world arguing with that. */
float fly_faction_temper(int a, int b);
void fly_diplomacy_init(fly_diplomacy *d, uint32_t seed);
float fly_diplomacy_rel(const fly_diplomacy *d, int a, int b);
int fly_diplomacy_state(const fly_diplomacy *d, int a, int b);
const char *fly_relation_name(int state);
/* Move a pair. Symmetric by construction — there is no way to push one side of
 * a relation and not the other, because there is no such thing. */
void fly_diplomacy_nudge(fly_diplomacy *d, int a, int b, float amount);

/* --- the world's factions ---
 *
 * `fly_faction_world_init` seeds ownership and relations. It is called from
 * `fly_world_gen` and it draws nothing: ownership is a hash of the seed and
 * the site, leaned by what the site is and how dangerous its ground is, so
 * every world that existed before this module still generates the same terrain,
 * the same sites and the same names. */
void fly_faction_world_init(struct fly_world *w);

/* Pressure. `amount` is in grip-units: 1.0 is roughly what it takes to prise a
 * firmly held site out of a power's hands, and everything the world does
 * arrives here in fractions of a percent of that. Ignored for FREE, which
 * cannot push — nobody campaigns on behalf of nobody. */
void fly_faction_push(struct fly_world *w, int loc, int faction, float amount);

/* Advance the argument by `dt_hours`. Pressure decays, grip moves on the
 * balance between the owner's push and its strongest rival's, sites change
 * hands when grip runs out, and relations drift toward temper while the fights
 * that are actually happening pull them away from it.
 *
 * Writes up to `max` events and returns how many. Overflow is documented and
 * boring: the excess is dropped, because an event here is a line on the radio
 * and a step that produced more than a handful is a step nobody could read. */
int fly_faction_step(struct fly_world *w, float dt_hours,
                     fly_faction_event *out, int max);

/* --- queries --- */

/* How many sites a power holds. */
int fly_faction_holdings(const struct fly_world *w, int f);
/* The strongest challenger at `loc` and how hard it is pushing, or FREE and 0
 * when the place is quiet. A site is *contested* when a rival is pushing hard
 * enough to be moving the grip, which is exactly when it is worth drawing a
 * second pennant under the first. */
int fly_faction_challenger(const struct fly_world *w, int loc, float *pressure);
int fly_faction_contested(const struct fly_world *w, int loc);
/* Would `a` shoot `b`? Pure relation: whether anybody actually opens fire also
 * depends on being armed and on what kind of pilot it is. */
int fly_faction_hostile(const struct fly_world *w, int a, int b);

/* The grip a site is held at, 0..1, and what that reads as. */
const char *fly_faction_grip_name(float grip);

/* --- the player's standing ---
 *
 * Standing is an integer in -100..100 and it is the operator's own ledger, so
 * it lives in fly_game; these are the rules it obeys, kept here so the command
 * that changes it and the shop that reads it cannot disagree.
 *
 * `FLY_STAND_PLEDGE` is what a power wants before it will take an oath, and
 * `FLY_STAND_KIT` before it will sell what it builds for itself. The gap
 * between them is deliberate: joining is the start of the relationship, not
 * the end of it. */
#define FLY_STAND_MAX 100
#define FLY_STAND_PLEDGE 25
#define FLY_STAND_KIT 45
#define FLY_STAND_HOSTILE (-40)

/* What a deed with one power does to your standing with another, per point
 * earned: allies share credit, enemies take offence, and the size of it is the
 * relation itself. This is the whole of the "you cannot please everybody"
 * rule, and it is one function so nothing can implement half of it. */
float fly_faction_standing_spill(const fly_diplomacy *d, int deed_with, int other);

#endif /* FLY_FACTION_H */
