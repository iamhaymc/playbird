/* fly_game: survival delivery gameplay on top of fly_sim + fly_world.
 *
 * All state mutation flows through fly_game_apply(cmd) + fly_game_step(dt).
 * Commands are plain serializable structs and the step is deterministic,
 * so a later multiplayer build only needs to ship commands to a server
 * and snapshots back. */
#ifndef FLY_GAME_H
#define FLY_GAME_H

#include "fly_sim.h"
#include "fly_ord.h"
#include "fly_world.h"
#include "fly_walk.h"
#include "fly_rail.h"
#include "fly_road.h"
#include "fly_sea.h"
#include "fly_convoy.h"
#include "fly_pilot.h"

#define FLY_MAX_CONTRACTS 6
#define FLY_MAX_PILOTS 24
#define FLY_PILOT_JOURNAL 256
#define FLY_LOG_LINES 8
#define FLY_AIRFRAME_CATALOG_MAX 16
#define FLY_OWNED_AIRFRAME_MAX 12
#define FLY_GROUND_ITEM_MAX 64

/* --- commands --- */

typedef enum {
    FLY_CMD_CONTROLS = 0,  /* controls field */
    FLY_CMD_LAUNCH = 1,    /* leave the docked pad */
    FLY_CMD_AUTOPILOT = 2, /* a = target location, or -1 to disengage */
    FLY_CMD_ACCEPT = 3,    /* a = contract slot */
    FLY_CMD_BUY = 4,       /* a = resource, f = qty */
    FLY_CMD_SELL = 5,      /* a = resource, f = qty */
    FLY_CMD_REFUEL = 6,    /* fill tanks from local market fuel */
    FLY_CMD_REPAIR = 7,    /* fix wear+hp using parts/tokens */
    FLY_CMD_UPGRADE = 8,   /* legacy purchase + fit, a = module index */
    FLY_CMD_SWITCH = 9,    /* legacy buy/select plane or drone */
    FLY_CMD_FIT_MODULE = 11,
    FLY_CMD_UNFIT_MODULE = 12,
    FLY_CMD_SELECT_AIRFRAME = 13,
    FLY_CMD_BUY_MODULE = 14,
    FLY_CMD_BUY_AIRFRAME = 15,
    FLY_CMD_BUILD_AIRFRAME = 16,
    FLY_CMD_WALK = 17,
    FLY_CMD_LOOK = 18,
    FLY_CMD_JUMP = 19,
    FLY_CMD_INTERACT = 20,
    FLY_CMD_COLLECT_MODULE = 21,
    FLY_CMD_DISEMBARK = 22,
    FLY_CMD_BOARD_AIRFRAME = 23,
    FLY_CMD_RAIL_ENTER = 24,
    FLY_CMD_RAIL_EXIT = 25,
    FLY_CMD_SELL_MODULE = 26, /* a = module; liquidate at FLY_RESALE of price */
    FLY_CMD_RESTART = 27,     /* only legal in ruin: new operator, same world */
    /* --- what to do with the surplus ---
     * All three take data.fit.uid and all three are service-site work. They
     * exist because a hold that only ever fills up turns loot into bookkeeping:
     * the fourth Uprated engine has to be worth something other than shelf
     * space, and selling it back is a price, not a decision. */
    FLY_CMD_SALVAGE_MODULE = 28, /* break it for parts */
    FLY_CMD_REROLL_MODULE = 29,  /* pay to redraw its rolls, same tier */
    FLY_CMD_UPRATE_MODULE = 30,  /* eat three of its slot to push it a tier */
    /* Take a power's colours, or hand them back. `a` is the faction; passing
     * FLY_FACTION_FREE resigns. Legal only on the ground at a site that power
     * holds, and only once they think enough of you — see fly_game_pledge_ok
     * for the whole of the rule, which the UI reads rather than reimplements. */
    FLY_CMD_PLEDGE = 31,
    /* Fill the magazines. Its own command rather than a clause inside refuel,
     * because they are refused on different terms: fuel is sold to anybody,
     * including a power at war with you, and ordnance is not. */
    FLY_CMD_REARM = 32
} fly_cmd_kind;

typedef struct {
    fly_cmd_kind kind;
    fly_controls controls;
    int a;
    float f;
    union {
        /* `uid` names a particular item and `a` its base type; a caller that
         * only knows the type (a shop list) passes uid 0 and gets the best it
         * owns, which is what a player picking from a list of types means. */
        struct { int module, airframe; uint32_t uid; } fit;
        struct { float forward, right; int jump; } walk;
        struct { float yaw, pitch; } look;
    } data;
} fly_cmd;

/* --- contracts --- */

typedef struct {
    int state;         /* 0 empty, 1 offered, 2 active */
    int from, to;      /* location indices */
    int res;           /* fly_resource */
    float qty;
    long reward;
    double deadline;   /* game time seconds */
    int cargo_loaded;  /* goods aboard? (loaded on accept at 'from') */
} fly_contract;

/* --- modules: typed slots, each fitted module changes BOTH the sim and the
 * 3D model (visual mask) from one definition; YAML-overridable --- */

typedef enum {
    FLY_SLOT_ENGINE,
    FLY_SLOT_WINGS,
    FLY_SLOT_HARDPOINT,
    FLY_SLOT_BAY,
    FLY_SLOT_AVIONICS,
    FLY_SLOT_HULL,
    FLY_SLOT_COUNT
} fly_slot;

typedef struct {
    char id[20];
    char name[28];
    char desc[52];
    int slot;         /* fly_slot */
    int craft_kind;   /* -1 any, else fly_craft_kind it fits */
    long cost;
    /* sim effects (multipliers default 1, adds default 0, ratings set-if->0) */
    float thrust_mul, fuel_mul, cargo_mul;
    float mass_add, ceiling_add, vne_add, cd0_add, structure_add;
    float weather_rating, radar_range, gun_dps, gun_range, armor;
    /* --- what it launches, as opposed to what it shoots ------------------
     *
     * The gun above is the hitscan half of combat and these are the other one:
     * a launcher puts an object in the air that the target has seconds to do
     * something about. A module names a kind and the ordnance ladder in
     * fly_ord.c says what a round of that kind is; all a module carries is how
     * far it will take a shot, how hard its own rounds hit, and how many it
     * holds. The alternative — every launcher module restating a missile's
     * speed, burn, turn rate and fuse — is six copies of one table that will
     * disagree with itself the first time anything is rebalanced.
     *
     * `cm_*` is the same shape pointed the other way, because a dispenser is a
     * launcher whose rounds have no warhead. Keeping them symmetrical is what
     * lets one fit path, one save field and one HUD widget serve both. */
    float beam_heat;      /* >0 turns the gun into a beam; see fly_airframe */
    int ord_kind;         /* fly_ord_kind, 0 = not a launcher */
    float ord_range, ord_damage;
    int ord_ammo;
    int cm_kind;          /* fly_ord_kind of countermeasure, 0 = not a dispenser */
    int cm_ammo;
    float ecm;            /* 0..1: how badly it confuses a seeker's head */
    int vtol;
    /* --- what it does to the aeroplane, as opposed to the spec sheet ------
     *
     * Everything above this line moves a performance figure: faster, higher,
     * further, tougher. A loadout built out of nothing but those is a loadout
     * you evaluate on a screen and then never notice again, because every
     * build lands on the same aeroplane with different numbers printed on it —
     * which is exactly what fitting a module used to be. The renderer knew a
     * gun pod was there and the flight model did not.
     *
     * These are the other half. They change the machine: how fast it rolls,
     * how far the wing lets you pull before it lets go, how much the airframe
     * wants to fly straight, how quickly the engine answers, how lazy the mass
     * hanging off the wings makes it. Two builds with the same top speed and
     * the same range now fly nothing like each other, and a wing module is a
     * decision about what kind of pilot you are going to have to be.
     *
     * The multipliers are unset at zero and read as 1 (see fly__mul), so a
     * module says only what it changes and the registry stays readable. Every
     * one of them lands on a field `fly_sim` already reads, so a module cannot
     * introduce a quantity the flight model has never been tuned against —
     * the same rule the affixes live under. */
    float pitch_mul, roll_mul, yaw_mul;   /* control authority per axis */
    float wing_area_mul, wing_span_mul;   /* the wing itself, not a coefficient */
    float cl_max_add;                     /* how much lift the wing can make */
    float alpha_stall_add;                /* rad: where it lets go */
    float stability_mul;                  /* how hard it wants to fly straight */
    float damping_mul;                    /* how quickly a rate dies away */
    float inertia_mul;                    /* mass distribution: stores are lazy */
    float spool_mul;                      /* engine response */
    float torque_mul;                     /* propeller reaction */
    float g_limit_add;                    /* what the spar is stressed for */
    uint32_t visual;  /* FLY_VIS_* geometry this module adds */
    /* the rocket stack; see fly_airframe for what each one buys */
    float rocket_thrust, rocket_ve, propellant_add, rcs_auth, heat_shield, pressure;
    /* How far up the ladder this base sits: the lowest item level of ground or
     * shelf it will ever turn up on. Zero for everything an aeroplane is made
     * of, which is most of the registry and is why it defaults to nothing.
     *
     * It exists for the rocket stack. A base is picked uniformly wherever the
     * game rolls loot, so five spaceframe parts in a registry of seventeen made
     * a third of every shelf in the world — including the apron the player
     * starts on — a rocket part, and handed away the one build that is supposed
     * to be the end of the ladder before the first delivery. Scarcity by tier
     * cannot fix that: rarity says how good a Rocket Motor is and this says
     * whether there is a Rocket Motor there at all. */
    int min_ilvl;
    /* Who builds it. FLY_FACTION_FREE for everything anybody's workshop can
     * turn out, which is the whole aeroplane registry and the rocket stack;
     * a power for the things that power keeps to itself.
     *
     * Faction kit is deliberately outside the ordinary loot stream rather than
     * merely rare in it. `fly_module_roll_base` never picks one, so no wreck,
     * derelict or open shelf can hand you a Concord interceptor cannon — the
     * only way to one is a counter at one of their own fields with your name
     * in good standing on it. Rarity says how *well* a thing rolled; this says
     * whether there is one there at all, and the two are different questions
     * (the same distinction `min_ilvl` draws for the rocket stack). */
    int faction;
} fly_module;

/* Where the spaceframe starts. Chosen off the map rather than picked: item
 * level is `1 + 98 * danger`, the shelves live at cities and skyports, and at
 * 50 every world measured has at least one service site that stocks the stack
 * and none of them is the apron the player starts on. Lower and the home shop
 * sells space; higher and some seeds cannot buy it at all. */
#define FLY_SPACE_ILVL 50

#define FLY_MODULE_MAX 64
int fly_module_count(void);
const fly_module *fly_module_get(int idx);
int fly_module_find(const char *id); /* -1 if unknown */
/* Pick a base that belongs at this item level, uniformly among those that do.
 * Every shelf, wreck, derelict and payment-in-kind draws through here, so they
 * cannot disagree about where a part can turn up. Exactly one draw off `rng`
 * whatever the registry holds, so adding a module never shifts the stream. */
int fly_module_roll_base(int ilvl, fly_rng *rng);
/* The same draw over one power's own catalogue: what its fields sell and what
 * its wrecks shed. Returns -1 when that power builds nothing at this level,
 * which is the caller's cue to fall back to the open registry rather than to
 * put nothing on the shelf. One draw off `rng` whenever it returns a base, and
 * none when there is nothing to draw from, so a shelf that falls back consumes
 * exactly what an ordinary shelf would. */
int fly_module_roll_faction(int ilvl, int faction, fly_rng *rng);
/* Every module a power builds, in registry order: what a hangar list shows. */
int fly_module_faction_count(int faction);
const char *fly_slot_name(int slot);
/* the same, as a fixed-width four-letter tag for lists */
const char *fly_slot_tag(int slot);
/* replace/extend the registry from a YAML file (see data/modules.yaml) */
int fly_modules_load_yaml(const char *path);
/* Fold one module onto an airframe: the whole of what fitting a part does,
 * sim and visuals together. `fly_game_rebuild_airframe` is this once per slot,
 * and anything else that wants to know what an aeroplane with a part on it
 * flies like has to come through here rather than transcribe it. */
void fly_module_apply(fly_airframe *af, const fly_module *m);

/* --- items: a module is an object, not a quantity ---
 *
 * The registry above is the *base*: what a Gun Pod is for. What you own is an
 * instance of one, and two Gun Pods off two different wrecks are not the same
 * Gun Pod. That is the whole difference between shopping and looting, and it
 * is why the inventory is an array of these rather than a count per base.
 *
 * An item is identified by `uid` and never by its position, because positions
 * shift the moment anything is sold: a command that says "fit slot 3 of the
 * inventory" fits whatever slid into 3 while the player was reading the
 * screen. uid 0 is the empty item and is never issued.
 *
 * The name is generated from the base, the rarity and the rolls rather than
 * stored, so it costs nothing per item, cannot disagree with the item it names,
 * and needs nothing in the save format. */

typedef enum {
    FLY_ITEM_STOCK,      /* factory issue: exactly what the registry says */
    FLY_ITEM_TUNED,
    FLY_ITEM_MARKED,
    FLY_ITEM_PROTOTYPE,
    FLY_ITEM_ONEOFF,
    FLY_ITEM_RARITY_COUNT
} fly_item_rarity;

/* Which sim field an affix moves. Every one of these is a field the base
 * modules already use, so an affix cannot introduce a quantity the flight
 * model has never been tuned against. */
typedef enum {
    FLY_AFFIX_NONE = 0,
    FLY_AFFIX_THRUST,
    FLY_AFFIX_MASS,
    FLY_AFFIX_DRAG,
    FLY_AFFIX_VNE,
    FLY_AFFIX_FUEL,
    FLY_AFFIX_CARGO,
    FLY_AFFIX_CEILING,
    FLY_AFFIX_STRUCTURE,
    FLY_AFFIX_ARMOR,
    FLY_AFFIX_GUN_DPS,
    FLY_AFFIX_GUN_RANGE,
    FLY_AFFIX_RADAR,
    FLY_AFFIX_WEATHER,
    /* The spaceframe's own three. They are here for the same reason the gun
     * roll is: rarity has to mean something on the part it is printed on. A
     * Prototype Rocket Motor that could only ever roll mass, drag, Vne and
     * structure was a tier badge on an item whose entire job — thrust with no
     * air to push against — no roll could touch, which made the five most
     * expensive things in the game the five where a good one and a bad one
     * were the same aeroplane.
     *
     * The two ratings the stack also carries, `heat_shield` and `pressure`,
     * are deliberately not here. Both are read as thresholds — a cabin at 0.4
     * kills the pilot exactly as dead as one at 0 — so a roll on either would
     * move a number nothing reads and print a line that means nothing. */
    FLY_AFFIX_ROCKET,
    FLY_AFFIX_PROPELLANT,
    FLY_AFFIX_RCS,
    /* The two handling rolls. They are here because rarity has to be felt
     * where the module is felt, and once a module changes how the aeroplane
     * flies, a Prototype one that could only ever roll mass, drag and Vne
     * would be a tier badge on the half of the part that does not matter.
     *
     * Only two, and deliberately: roll rate and stall margin are the two
     * numbers a pilot can feel inside one turn, so a good roll reads through
     * the stick rather than through the loadout screen. Stability, damping,
     * inertia and spool are not on the list — they are what an airframe *is*,
     * and an item that rolled a twitchier aeroplane would be handing out a
     * different machine rather than a better one.
     *
     * Appended after the spaceframe three rather than filed next to their
     * relatives, because a stored affix is its enum index: inserting one in
     * the middle would silently turn every rolled Vne into a rolled ceiling
     * in every save file in existence. */
    FLY_AFFIX_ROLL,
    FLY_AFFIX_STALL,
    /* The armed loadout's three, appended for the same reason the last two
     * were: a stored affix is its enum index, so inserting one anywhere but the
     * end silently turns every rolled Vne in every existing save into a rolled
     * something else.
     *
     * Three, and they are the three a launcher is actually judged on: what a
     * round does, how far the envelope reaches, and how many of them there are.
     * A magazine roll is the interesting one — a rack that holds six seekers
     * rather than four is a different aeroplane over a long patrol, and it is
     * the only roll in the game whose value depends on how long you stay out.
     *
     * There is deliberately no roll on a countermeasure's *effect*. A flare
     * either takes the seeker or it does not, decided by where it is and when
     * it was dropped, and a percentage on that would put a die back in the one
     * place this whole system was built to keep one out. Dispensers roll their
     * magazine through FLY_AFFIX_MAGAZINE like everything else. */
    FLY_AFFIX_ORD_DAMAGE,
    FLY_AFFIX_ORD_RANGE,
    FLY_AFFIX_MAGAZINE,
    FLY_AFFIX_COUNT
} fly_affix_kind;

#define FLY_ITEM_AFFIX_MAX 4

/* One roll. `mag` is fixed point at 1/1000 of the affix's own unit, which
 * keeps an item 28 bytes and a save file readable. */
typedef struct {
    unsigned char kind;   /* fly_affix_kind */
    short mag;
} fly_affix_roll;

typedef struct {
    uint32_t uid;
    short base;              /* registry index; serialized by the base's id */
    unsigned char rarity;    /* fly_item_rarity */
    unsigned char naff;
    unsigned char ilvl;      /* 1..99: how hard the thing that dropped it was */
    unsigned char seed;      /* picks the name, so two identical rolls read apart */
    fly_affix_roll aff[FLY_ITEM_AFFIX_MAX];
} fly_item;

/* How much an operator can carry, how much one site can have on the shelf. */
#define FLY_ITEM_MAX 64
#define FLY_SITE_STOCK_MAX 8

int fly_item_valid(const fly_item *it);
/* "Marked Gun Pod of the Gale" — generated, never stored */
void fly_item_name(const fly_item *it, char *buf, int n);
const char *fly_item_rarity_name(int rarity);
const char *fly_affix_name(int kind);
/* the roll as it should be shown: "+7% thrust", "-14 kg". Returns the sign so
 * a caller can colour it without parsing the text back. */
int fly_affix_text(const fly_affix_roll *a, char *buf, int n);
/* Every roll on one line: "+7% thrust, -14 kg". Empty for a Stock item. */
void fly_item_rolls(const fly_item *it, char *buf, int n);
/* What the item is worth: the base cost scaled by rarity and by what it rolled.
 * One function, so the shop, the resale and the ruin reserve cannot disagree. */
long fly_item_value(const fly_item *it);

/* one roll in the sim's own unit for that field */
float fly_affix_value(const fly_affix_roll *a);
/* The base module as this instance of it actually behaves: the registry entry
 * with the rolls folded in. One place, so the shop's numbers, the loadout
 * screen and the flight model cannot disagree about what an item does. */
fly_module fly_game_item_effect(const fly_item *it);

typedef struct {
    char id[24];
    char name[40];
    fly_airframe base;
    long price;
    int starter;
    float build_alloy;
    float build_parts;
    /* Whose hangar builds it. FLY_FACTION_FREE for the four anybody can buy;
     * a power for the seven it keeps for its own. A faction hull is stocked
     * only at that power's own cities and sold only to somebody they think
     * well enough of — which is why the airframe list is a reason to take an
     * oath rather than a longer shopping page. */
    int faction;
    int starter_fitted[FLY_SLOT_COUNT];
    unsigned short starter_spares[FLY_MODULE_MAX];
} fly_airframe_catalog;

typedef enum {
    FLY_AIRFRAME_USABLE,
    FLY_AIRFRAME_DESTROYED
} fly_owned_airframe_status;

typedef struct {
    uint32_t instance_id;
    char catalog_id[24];
    fly_owned_airframe_status status;
    int location;
    float condition;
    float fuel;
    /* What is in the magazines when this hull is parked. Beside `fuel` because
     * it is the same kind of thing: a consumable that belongs to this
     * particular airframe rather than to the operator, so switching to a second
     * aeroplane does not hand it the first one's flares. Rearmed with
     * FLY_CMD_REARM at any site that will serve you. */
    int ammo, cm;
    float wear;
    /* Airframe life used up, 0..1. Unlike wear this is a ratchet: a repair
     * restores integrity, it does not restore lifetime. It is the only sink in
     * the economy that money cannot switch off, which is what stops careful
     * play from accumulating past the point where anything can go wrong. Metal
     * and engine hours, not the pilot — nothing here implies a body. */
    float fatigue;
    /* uid of the item in each slot, 0 for empty. A uid rather than an index,
     * so selling something out of the hold cannot silently re-slot the rest of
     * the aircraft. */
    uint32_t fitted[FLY_SLOT_COUNT];
} fly_owned_airframe;

/* Hours of flying a hull lasts at benign loads; g, turbulence and storms all
 * multiply the rate up. */
#define FLY_HULL_LIFE_HOURS 40.0f
/* What a fitted or stocked module fetches when it is sold back. The gap
 * between this and the buy price is what makes rebuilding after a loss cost
 * something rather than being a round trip. */
#define FLY_RESALE 0.55f
/* How far from a service site a wreck can lie and still be walked out to. A
 * crash beyond this, or in water, is a total loss. */
#define FLY_SALVAGE_RANGE 4500.0f

/* Why the run ended. Ruin is a property of the operator's estate, never of the
 * world: the markets and the other pilots carry on across a restart. */
typedef enum {
    FLY_RUIN_NONE = 0,
    FLY_RUIN_GROUNDED  /* no airworthy hull, and nothing left to raise one with */
} fly_ruin_reason;

typedef enum {
    FLY_GROUND_SOURCE_WORLD,
    FLY_GROUND_SOURCE_CRASH
} fly_ground_source;

typedef struct {
    int active;
    /* The whole item, not a reference to one. What is lying on the hillside is
     * the thing that came off the wreck, rolls and all, and the inventory it
     * came out of may not exist any more by the time somebody walks out to it. */
    fly_item item;
    fly_v3 pos;
    float yaw;
    fly_ground_source source;
    uint32_t source_id;
    /* A module coming off a wreck is thrown, not placed. It carries velocity
       and a tumble until it hits something; `settled` is what the collector and
       the renderer read to know it has stopped. Items placed by the world start
       settled, which is what a zero launch velocity produces on the first step
       anyway. */
    fly_v3 vel;
    float spin;
    int settled;
} fly_ground_item;

/* An airframe coming apart, for as long as there is something to see.
 *
 * Purely a consequence of the simulation — nothing reads a blast back, so it
 * carries no authority over anything and needs no command. It is spawned for
 * every hull lost, the player's and anyone else's, because a world where only
 * your own crashes are visible is a world where nobody else is really flying. */
#define FLY_BLAST_MAX 8
#define FLY_BLAST_LIFE 5.0f

typedef struct {
    int active;
    fly_v3 pos;
    float age;      /* seconds since it went up */
    float size;     /* metres across at full bloom; scales with the airframe */
    uint32_t seed;  /* stable soot and debris pattern for this one blast */
} fly_blast;

typedef enum {
    FLY_MODE_FLIGHT,
    FLY_MODE_WALK,
    FLY_MODE_RAIL
} fly_player_mode;

/* --- the other people flying ---
 *
 * Every command any pilot applies is journalled with the time and the actor it
 * came from. That is not a debug facility: it is the shape a multiplayer build
 * needs, where the same entries arrive from a socket instead of from
 * fly_pilot_think, and `pilots_remote` is the switch that says so. The
 * `game.pilots` aspect uses exactly that path to prove the two are
 * interchangeable. */

typedef struct {
    double time;
    uint32_t actor;      /* pilot id */
    fly_pilot_cmd cmd;
} fly_pilot_journal_entry;

/* --- the player, as a participant ---------------------------------------
 *
 * The one slot in the world that is not a `fly_pilot`, and everything it owns
 * that a pilot also owns: the hull it is flying, the aeroplane that hull adds
 * up to, the stick it is being flown with, its money and its freight, and
 * whose colours it wears. `fly_pilot` carries exactly this set at its head and
 * then adds the machinery of deciding — a task, a leg, a mark, a think timer —
 * which is the half a human does not need.
 *
 * It lived loose in `fly_game` until now, and that was the last thing standing
 * between this build and a networked one. Every other participant in the world
 * is addressable: it has a struct, its state can be sent, and a command stream
 * can drive it whether the commands came out of `fly_pilot_think` or off a
 * socket — which is the whole argument fly_pilot.h opens with. The local
 * player was the exception, so the one seat a remote client could not take was
 * the interesting one. Packaged, it is on the same footing as the rest, and
 * what is left to make it a slot in an array is bookkeeping rather than a
 * design question.
 *
 * What stays in `fly_game` is the operator's ledger rather than the actor's
 * state — the hangar, the hold, contracts, standing, experience, the fail
 * state. Those outlive the hull and, for standing, the operator; none of them
 * is a thing a participant *is*. The line is the one `fly_pilot` already
 * draws, so the two structs can be compared field for field. */
typedef struct {
    /* Which power's colours this one wears, FLY_FACTION_FREE for an unaligned
     * operator — which is what everybody starts as: the oath is a thing you go
     * and take. Actor state rather than world state because it does not
     * survive the operator: a new one signs on unaligned into the same war. */
    int faction;

    /* The airframe is the active owned hull with its fitted modules applied: a
       derived cache, rebuilt by fly_game_rebuild_airframe and never edited
       directly. The order below is `fly_pilot`'s, so the two can be read side
       by side. */
    fly_airframe airframe;
    fly_craft craft;
    fly_controls controls;

    long tokens;
    float cargo[FLY_RES_COUNT];
} fly_actor;

/* --- game --- */

typedef struct {
    uint32_t seed;
    fly_world world;
    fly_rng rng;
    /* A second stream, used only by fly_pilot_think and by nothing that moves
     * the world. It has to be separate: a client that does not run the AI does
     * not make those draws, and if decisions shared the simulation's stream
     * every turbulence sample after the first would land differently and the
     * two machines would drift apart within seconds. The replay gate found
     * this by failing. */
    fly_rng think_rng;
    /* And a third, for the air the *player* is flying in.
     *
     * Turbulence, the buffet shake and the autopilot's own incident roll used
     * to come off the simulation's stream, and every aircraft in the world
     * draws from that stream before the player does — so how hard it was
     * blowing over one aeroplane was a function of how many others happened to
     * be alive that second. Nothing about that is a weather model. It made
     * `game.autopilot`'s structure-per-leg measure a lottery rather than a
     * reading of the approach profile: moving `fly_pilot`'s turbulence divisor
     * by two parts in a thousand — a change no crew could tell apart — took
     * that measure from 6.3 of a hundred points a leg to 9.4, 15.1 and 17.1
     * across three such perturbations, and dropped a landing. The gate was
     * measuring the fleet.
     *
     * Same argument as `think_rng` and `convoy_rng` above, one layer in: a
     * stream is shared only by things that belong to the same question. */
    fly_rng play_rng;
    double time_s;      /* game time, starts at 08:00 day 0 */

    /* The player, as one participant among the rest. See fly_actor. */
    fly_actor player;
    fly_weather weather; /* weather at the player, refreshed each step */

    fly_owned_airframe owned[FLY_OWNED_AIRFRAME_MAX];
    int owned_count;
    int active_airframe;
    uint32_t next_airframe_instance_id;
    /* The hold. Compacted, so `item_count` is the end; identity is `uid`, so
     * nothing outside here may hold an index across a sale. */
    fly_item items[FLY_ITEM_MAX];
    int item_count;
    uint32_t next_item_uid;
    /* What each site has on the shelf, as instances — a vendor sells a
     * particular Gun Pod, not the idea of one, and a player has to be able to
     * see what they are buying before they buy it. */
    fly_item stock[FLY_MAX_LOC][FLY_SITE_STOCK_MAX];
    unsigned short airframe_stock[FLY_MAX_LOC][FLY_AIRFRAME_CATALOG_MAX];

    fly_player_mode mode;
    fly_walker walker;
    fly_walk_input walk_input;
    fly_ground_item ground_items[FLY_GROUND_ITEM_MAX];
    fly_blast blasts[FLY_BLAST_MAX];
    /* Everything in the air that is not an aeroplane. Transient like the
     * traffic and not saved: a round mid-flight was going to resolve within
     * eight seconds, and restoring one would restore a threat whose origin the
     * player never saw. What *is* saved is the magazine it came out of. */
    fly_ord ord[FLY_ORD_MAX];
    uint32_t next_ord_id;
    fly_rail_route rail_route;
    fly_rail_state rail;
    /* --- the ground network ---
     *
     * The roads are a function of the world alone and the convoys on them are
     * traffic, so neither is saved: the network is rebuilt from the seed with
     * the terrain, and the columns are repopulated from the market exactly as
     * the sky's pilots are. See fly_road.h and fly_convoy.h.
     *
     * And the same thing on the water. A lane is laid after the roads because
     * a jetty may not be built across a carriageway, and for no other reason:
     * nothing at sea is surveyed against anything on land. See fly_sea.h. */
    fly_road_net roads;
    fly_sea_net lanes;
    /* A third stream, for the roads alone.
     *
     * Dispatching a column draws — which road, what rung of the ladder, what it
     * is called — and if those draws came off the simulation's own stream then
     * every turbulence sample after the first would land differently for the
     * want of a convoy, and a build that ran the roads would drift from one
     * that did not. It is the same reason `think_rng` exists and the same
     * reason the derelicts are seeded off a stream of their own. */
    fly_rng convoy_rng;
    fly_convoy convoys[FLY_CONVOY_MAX];
    int convoy_count;
    uint32_t next_convoy_id;
    float convoy_restock;
    long convoys_arrived;  /* freight delivered by road, ever: a health signal */
    long convoys_lost;     /* columns burnt, by anybody's guns */
    int launch_site;
    fly_v3 launch_pos;
    int crash_handled;
    /* Seconds the camera stays with the wreck before the operator is walked
       home. The hull is already gone by then — this delays the relocation, not
       the consequences. */
    float crash_hold;
    int crash_site;
    int crash_recoverable;

    int xp;
    int reputation;

    /* --- allegiance ---
     *
     * Whose colours the operator wears, and what each power makes of them.
     * `reputation` above is the trade's opinion of you and is one number for
     * the whole world; this is seven opinions that disagree, which is the
     * point. Standing runs -100..100 and every deed spills across the
     * relations — see fly_faction_standing_spill — so there is no way to be
     * everybody's friend, and a player who tries ends up welcome nowhere in
     * particular.
     *
     * Whose colours the operator actually wears is the actor's — see
     * `fly_actor.faction`, which is where a participant's side belongs and
     * where a pilot's already was. What stays here is what the powers think of
     * the operator, which outlives any one hull. */
    int standing[FLY_FACTION_COUNT];
    double pledged_at;    /* game time the current oath was taken */
    int seizures_seen;    /* sites that changed hands while this run watched */

    /* --- the fail state ---
     *
     * Ruin ends the run, not the world. When the player has no airworthy hull
     * and cannot raise the price of the cheapest one — counting cash, cargo,
     * sellable modules and any wreck still lying somewhere it can be walked out
     * to — there is no sequence of legal commands that gets back into the air,
     * and the honest thing is to say so rather than leave a save running that
     * cannot progress. FLY_CMD_RESTART then seats a new operator in the same
     * world: same markets, same pilots, same clock, and the previous
     * operator's scattered modules still on the ground where they fell. */
    int ruin;            /* fly_ruin_reason */
    int operator_seq;    /* how many operators this world has been through */
    int hulls_lost;      /* the player's own attrition, across operators */

    fly_contract contracts[FLY_MAX_CONTRACTS];
    int docked; /* location index, -1 airborne */

    int autopilot;      /* engaged? */
    int autopilot_target;   /* location index */
    float autopilot_cooldown;
    /* how long the autopilot has been loitering at its destination without
       getting down; an approach that cannot be flown has to end in something */
    float autopilot_near_s;
    /* and how long it has been rolling around on the ground there without
       getting any closer to the pad, against the closest it has managed. A
       different failure with a different answer: the approach worked, so the
       thing to do is stop, not to take off again and fly somewhere else. All
       three are transient and none is saved — a loaded game re-earns its
       patience. */
    float autopilot_taxi_s;
    float autopilot_taxi_best;
    float autopilot_taxi_hp;
    int launch_guard;   /* suppress re-docking until clear of the pad */

    fly_pilot pilots[FLY_MAX_PILOTS];
    int pilot_count;
    uint32_t next_pilot_id;
    float pilot_restock;
    int pilots_remote;   /* 1 = do not think; commands arrive from outside */
    /* Fly everyone at full fidelity regardless of range. Only the handful of
     * aircraft near the camera normally get the six-degree-of-freedom model, so
     * a short run with the player parked barely exercises it — and that is
     * where every interesting way to lose an aeroplane lives. The pilot aspect
     * sets this to put the flight model under load on purpose. */
    int pilots_all_detail;
    int pilots_lost;     /* aircraft written off, ever: a health signal */
    int pilots_lost_detail; /* of those, how many were being flown in full */
    /* Work done by the whole population, ever. Per-pilot tallies die with the
     * pilot, so summing the living ones understates the world by exactly the
     * attrition — which is the number a health check most wants to see. */
    long pilot_hauls;
    long pilot_kills;
    /* Frames in which somebody's guns actually bore on somebody. Kills are a
     * slow rate — a dozen in three hours of world — and too rare to hold a
     * check on over a short run; a firing solution is the thing pursuit is
     * *for*, happens far more often, and is what "closes but never converts"
     * was a shortage of. */
    long pilot_engagements;
    fly_pilot_journal_entry journal[FLY_PILOT_JOURNAL];
    int journal_head;    /* next slot to write */
    long journal_total;  /* commands ever applied; detects ring overflow */

    char log[FLY_LOG_LINES][96];
    int log_count;

    /* transient combat feedback for HUD (not saved) */
    float hit_flash, gun_flash;
} fly_game;

void fly_game_init(fly_game *g, uint32_t seed, fly_craft_kind start_kind);
/* apply a command; returns 0 ok, negative error, and logs the outcome */
int fly_game_apply(fly_game *g, const fly_cmd *cmd);
/* advance the world+sim by dt seconds (call with fixed small steps) */
void fly_game_step(fly_game *g, float dt);

void fly_game_log(fly_game *g, const char *fmt, ...);
float fly_game_cargo_mass(const fly_game *g);
float fly_game_radar_range(const fly_game *g);

/* --- what the pilot is allowed to be told about ordnance ------------------
 *
 * Both of these exist because a threat you cannot see coming reads as the game
 * cheating, which is the same rule the stall buffet was built under: a stall
 * you were told about is a mistake and a stall you were not is a bug. A missile
 * is the harshest case of it in the game — several seconds of warning is the
 * difference between a mechanic and an ambush — so the warning is unconditional
 * and does not depend on carrying a receiver.
 *
 * Both read state and change none, and both are the same functions the trigger
 * and the AI use, so the cue on the glass cannot promise a shot the launcher
 * will refuse or stay quiet about a round that is about to arrive. */

/* Does the player have a launch solution right now, and where is it? Returns 1
 * with `out` set to the mark's position, or 0. Unguided launchers always answer
 * 1 with the impact point, because pointing *is* the solution for those. */
int fly_game_ord_solution(const fly_game *g, fly_v3 *out);
/* The soonest round inbound on the player: seconds to impact, or a negative
 * number when nothing is. `from` receives where it is now, so the HUD can put a
 * bearing on it — knowing something is coming and not which way is worse than
 * useless, because it makes you turn at random. */
float fly_game_ord_inbound(const fly_game *g, fly_v3 *from, int *kind);
int fly_game_has_upgrade(const fly_game *g, const char *module_id);
/* The active airframe's slots, as item uids (0 = empty), or NULL if there is
 * no active airframe. */
const uint32_t *fly_game_fitted(const fly_game *g);
/* The item with this uid, from the hold or from any airframe's slots; NULL for
 * uid 0 or an unknown one. */
const fly_item *fly_game_item(const fly_game *g, uint32_t uid);
/* The base registry entry behind a uid, or NULL. Shorthand for the display
 * code, which asks this far more often than it asks for the rolls. */
const fly_module *fly_game_item_base(const fly_game *g, uint32_t uid);
/* Put an item in the hold. Returns its uid, or 0 if the hold is full — the
 * documented overflow: a full hold refuses, it does not drop the oldest. */
uint32_t fly_game_item_add(fly_game *g, const fly_item *it);
/* Take one out. Returns 0 ok, negative if it is not there or is fitted. */
int fly_game_item_remove(fly_game *g, uint32_t uid);
/* How many of `base` are loose in the hold: what the shop list shows. */
int fly_game_item_held(const fly_game *g, int base);
/* Is this item bolted to something? Display code asks constantly, because the
 * hold list is "everything I own that is not on an aeroplane". */
int fly_game_item_fitted(const fly_game *g, uint32_t uid);
int fly_game_airframe_catalog_count(void);
const fly_airframe_catalog *fly_game_airframe_catalog_get(int idx);
int fly_game_airframe_catalog_find(const char *id);
int fly_game_load_airframe_catalog(const char *path);
int fly_game_rebuild_airframe(fly_game *g);
/* What a site asks for one item, and what it pays for one. Both scale the
 * item's own value by the site's markup and the player's standing. */
long fly_game_item_price(const fly_game *g, int loc, const fly_item *it);
/* --- the surplus sink, as three questions the UI asks before offering it ---
 * Each returns 0 when the operation is legal here and now, or a negative
 * reason; `parts` and `tokens` receive the price when non-NULL. One place, so
 * the button and the command cannot disagree about what something costs. */
int fly_game_salvage_yield(const fly_game *g, uint32_t uid, float *parts, long *tokens);
int fly_game_reroll_cost(const fly_game *g, uint32_t uid, float *parts, long *tokens);
/* Uprating eats `FLY_UPRATE_FODDER` loose items of the same slot at the
 * target's own tier or better. Capped below the top: a One-off is not a thing
 * a workshop makes, it is a thing somebody was carrying. */
#define FLY_UPRATE_FODDER 3
#define FLY_UPRATE_CEILING FLY_ITEM_PROTOTYPE
int fly_game_uprate_cost(const fly_game *g, uint32_t uid, int *fodder, long *tokens);
long fly_game_airframe_price(const fly_game *g, int loc, int catalog);
/* Throw an item clear of a wreck: same thing, with velocity and a tumble. */
int fly_game_launch_ground_item(fly_game *g, const fly_item *it, fly_v3 pos, fly_v3 vel,
                                fly_ground_source source, uint32_t source_id);
/* Put an airframe's worth of fire and smoke at `pos`. Draws nothing from the
   command stream and no random numbers, so it cannot move the simulation. */
void fly_game_spawn_blast(fly_game *g, fly_v3 pos, float size);
int fly_game_spawn_ground_item(fly_game *g, const fly_item *it, fly_v3 pos,
                               fly_ground_source source, uint32_t source_id);
/* A factory-issue instance of a base: what a starter airframe comes with and
 * what a `Stock` shop shelf holds. */
fly_item fly_game_item_stock(int base);
/* Roll one. `ilvl` (1..99) is how hard the thing that produced it was and sets
 * both the rarity odds and the magnitudes; `bias` shifts the rarity ladder up
 * for a source that deserves it — a named outlaw, a gated site's shelf. Draws
 * from `rng`, so the caller decides which stream this event belongs to. */
fly_item fly_game_roll_item(int base, int ilvl, float bias, fly_rng *rng);
/* What a shelf at `loc` is allowed to be: the same roller, biased hard down
 * the ladder and with the top tier struck off outright. A shop has a counter
 * and an order book — it can get you a good part and it cannot get you a part
 * nobody made two of. Public so a test can hold the scarcity claim. */
fly_item fly_game_vendor_roll(const fly_world *w, int loc, int base, fly_rng *rng);
/* Throw `rolls` items clear of a wreck at `at`, rolled at that ground's own
 * item level with the source's `bias`. Returns how many made it onto the
 * ground — fewer than asked when the array is full or the pieces come down in
 * the water. */
int fly_game_scatter_drop(fly_game *g, fly_v3 at, fly_v3 vel, int rolls, float bias,
                          uint32_t source_id);
/* net worth-ish score for the profile page */
long fly_game_score(const fly_game *g);

/* --- allegiance, as the questions the UI and the commands both ask ---
 *
 * Every one of these is a pure read except `credit`, and they exist so that
 * the pledge row, the shop price and the command that acts on them cannot
 * hold three different opinions about the same rule. */

/* Standing with a power, clamped to the legal range. */
int fly_game_standing(const fly_game *g, int faction);
/* Move it, and let the deed spill along the relations: allies take half the
 * credit, enemies take the whole insult. The one entry point — nothing writes
 * `standing` directly, because a change that did not spill would quietly make
 * pleasing everybody possible. */
void fly_game_credit_faction(fly_game *g, int faction, int points);
/* May the operator swear to `faction` right now? 0 yes, negative why not:
 * -1 no such power, -2 not on the ground at one of their fields, -3 they do
 * not think enough of you yet, -4 already theirs. Resigning (FLY_FACTION_FREE)
 * only needs to be somewhere. */
int fly_game_pledge_ok(const fly_game *g, int faction);
/* What a site charges the operator, as a multiplier on every price it quotes:
 * cheap where you are welcome, dear where you are not, and a power at war with
 * yours will not sell you anything but fuel at all — see fly_game_service_ok. */
float fly_game_faction_markup(const fly_game *g, int loc);
/* Will this site do work for the operator — repair, rearm, contracts, kit? 0
 * yes, negative if the flag on the mast says no. Fuel is deliberately never
 * refused: stranding somebody at a hostile field is a fail state they cannot
 * see coming, and the game already has one of those. */
int fly_game_service_ok(const fly_game *g, int loc);
/* The power a site belongs to, and how the operator stands with it. Shorthand,
 * because every surface that draws a site asks both. */
int fly_game_site_owner(const fly_game *g, int loc);

/* What the player could raise right now without flying anywhere: cash, cargo
 * at the local cut, modules in inventory at resale, and the modules on any
 * wreck still standing somewhere reachable. Deliberately counts the wreck —
 * a check that could not see it would end runs that were winnable, which is
 * worse than having no fail state. */
long fly_game_reserve(const fly_game *g);
/* Price of the cheapest hull in the catalog: the bar `reserve` has to clear. */
long fly_game_hull_floor(const fly_game *g);
/* Reserve in hulls. The number the HUD shows, because a fail state nobody can
 * see coming reads as the game cheating. */
float fly_game_reserve_hulls(const fly_game *g);
/* Seat a new operator in the same world. Knowledge and standing carry, capital
 * does not. Safe to call at any time; FLY_CMD_RESTART is the command form. */
void fly_game_restart(fly_game *g);

/* Apply a command to a pilot from outside the AI — the seam a network client
 * plugs into, and what the replay gate drives. Returns fly_pilot_apply's code,
 * or -1 if no live pilot carries that id. */
int fly_game_pilot_inject(fly_game *g, uint32_t actor, const fly_pilot_cmd *cmd);
/* fill the world with its working population; called by fly_game_init */
void fly_game_populate(fly_game *g);
/* and the roads with theirs. Separate because the sky and the ground fill from
 * different things — a pilot needs a site to work out of, a column needs a road
 * and a market that wants something moved along it. */
void fly_game_convoys_populate(fly_game *g);
/* live pilot by id, or NULL */
const fly_pilot *fly_game_pilot_find(const fly_game *g, uint32_t id);

/* save/load whole game as YAML text; return 0 on success */
int fly_game_save(const fly_game *g, const char *path);
int fly_game_load(fly_game *g, const char *path);

#endif /* FLY_GAME_H */
