/* fly_sim: decoupled flight-dynamics engine.
 *
 * Pure and deterministic: fly_sim_step() reads inputs and environment
 * callbacks, mutates only the passed craft state, performs no I/O and keeps
 * no globals — the same inputs always produce the same trajectory, which is
 * what makes headless testing, replays and a later server/client split cheap.
 *
 * Frames: world is right-handed, z-up, meters, seconds. Body frame is
 * x forward, y left, z up. Positive control pitch raises the nose, positive
 * roll banks right, positive yaw turns right.
 */
#ifndef FLY_SIM_H
#define FLY_SIM_H

#include "fly_math.h"
#include "fly_rng.h"
#include "fly_yaml.h"

typedef enum {
    FLY_CRAFT_PLANE,
    FLY_CRAFT_DRONE
} fly_craft_kind;

/* Airframe: static configuration, loaded from YAML assets and modified by
 * upgrades. All units SI. */
typedef struct {
    char name[40];
    char id[24];
    fly_craft_kind kind;
    float mass;         /* kg, empty */
    float wing_area;    /* m^2 */
    float wing_span;    /* m */
    float cl0, cla;     /* lift curve intercept/slope (per rad) */
    float cl_max;       /* max lift coefficient */
    float alpha_stall;  /* rad */
    float cd0, k_ind;   /* parasitic drag, induced drag factor */
    float thrust_max;   /* N at sea level (plane) */
    float rotor_thrust; /* N total (drone) */
    float fuel_cap;     /* kg fuel, or MJ battery for drones */
    float burn_rate;    /* kg/s (or MJ/s) at full throttle */
    float pitch_auth, roll_auth, yaw_auth; /* control authority, rad/s^2 at Vref */
    float vref;         /* reference speed for control effectiveness */
    /* --- how it handles ------------------------------------------------
     *
     * The seven numbers that decide what an airframe is like to fly, as
     * opposed to what it is capable of. Everything above this block is a
     * performance figure — how fast, how high, how far — and two aeroplanes
     * can agree on every one of them and still be completely different
     * machines. This is where the difference lives, and it is the block a
     * fitted module reaches for when it wants to change the aircraft rather
     * than the spec sheet.
     *
     * The four scales are ratios against the nominal airframe, so 1 is
     * "ordinary" and a module that leaves one alone leaves it at 1. Zero is
     * read as unset and means 1 too: an airframe built by memset, or a YAML
     * file written before this block existed, still flies. */
    float stability;    /* static stability: how hard it wants to fly straight */
    float damping;      /* aerodynamic rate damping: how quickly a rate dies */
    float inertia;      /* mass distribution: pods and plate make it lazy */
    float spool;        /* engine response */
    /* Propeller reaction: torque, p-factor and slipstream as one coupling.
     * Zero for anything that is not turning a disc — rotors have their own
     * term, and a rocket has nothing to react against. */
    float torque;
    /* The airspeed at which the propeller stops making thrust. A prop is
     * pitched for a speed and gives less the faster it is already going,
     * which is what puts a ceiling on level speed and makes Vne a number you
     * dive to rather than cruise at. Zero derives it from Vne. */
    float prop_v;
    /* Load factor the wing is stressed for. Above it the spar starts paying,
     * and at speed the wing can pull far more than the airframe can take:
     * that gap is the whole reason a fast aeroplane is a dangerous one. */
    float g_limit;
    float ceiling;      /* practical ceiling, m */
    float vne;          /* never-exceed speed, m/s */
    float structure;    /* hit points */
    float cargo_cap;    /* kg */
    float gear_height;  /* m, resting height of body origin over ground */
    int vtol;           /* can land/launch vertically */
    /* fitted modules (upgrade slots; zero = not installed) */
    float gun_dps;      /* damage/s while firing on target */
    float gun_range;    /* m */
    /* Heat this weapon puts into the airframe per second of fire, or 0 for an
     * ordinary gun. Non-zero is what makes a gun a *beam*: no travel time worth
     * modelling, so no lead and no dodging it, paid for with a cone a third the
     * width, half the reach, and a hull that will not take more than a few
     * seconds of it at a time. It goes into `fly_craft.heat`, the field a
     * re-entry fills, because an airframe does not care what put the energy in
     * it — and a beam build therefore has a real reason to want a heat shield. */
    float beam_heat;
    float radar_range;  /* m, discovery + contact detection */
    float weather_rating; /* 0..1 tolerance to storms */
    float armor;        /* incoming damage multiplier reduction 0..1 */
    /* --- ordnance ------------------------------------------------------
     *
     * The gun above is hitscan: a line drawn instantly, resolved where the
     * trigger is pulled, and the whole of combat until now. These six are the
     * other kind — a launcher that puts an *object* in the air with several
     * seconds of life in it, which is the difference between a shot you take
     * and a shot the other aeroplane can do something about.
     *
     * `ord_kind` is a `fly_ord_kind` held as an int, because fly_ord.h needs
     * this header and the two cannot both include each other. It is the one
     * place in the flight model that names a thing the flight model does not
     * simulate, and the alternative — six copies of the ordnance ladder, one
     * per launcher module — is worse.
     *
     * Ammunition is deliberately here and deliberately finite, where the gun's
     * is neither. A magazine is something the airframe consumes, like fuel and
     * engine hours, so it is fair game under the rule the TODO sets out; and a
     * weapon that runs out is a weapon whose use is a decision. */
    int ord_kind;         /* fly_ord_kind fitted; 0 = no launcher */
    float ord_range;      /* m: how far the launch envelope reaches */
    float ord_damage;     /* multiplier on the kind's own damage; rarity moves it */
    int ord_ammo;         /* rounds the magazine holds when full */
    /* Countermeasures, which are the same mechanism pointed the other way: a
     * dispenser, a magazine, and a kind that says which guidance it answers.
     * A build carrying both a launcher and a dispenser has given up a bay to
     * do it, which is the trade the whole loadout screen is made of. */
    int cm_kind;          /* fly_ord_kind of countermeasure; 0 = none */
    int cm_ammo;
    /* Jamming, 0..1. Not a shield: it widens the error a seeker's head makes
     * about where the target is, so it degrades tracking rather than
     * preventing a launch, and it works against everything at once because a
     * pilot cannot know which kind is coming until it is. */
    float ecm;
    /* --- the rocket stack ---
     *
     * Five parts across five slots, and none of them is optional. A motor with
     * no propellant is dead weight, propellant with no attitude control is a
     * tumble, and a craft that reaches space without a cabin or a shield gets
     * there and does not come back. Wanting all five at once is the point: it
     * costs the gun, the cargo racks, the armour and the avionics, so going up
     * is a build rather than a purchase.
     *
     * Rocket thrust is deliberately *not* scaled by air density. That single
     * difference is the whole gate on space: every other engine in the game is
     * an air breather whose thrust goes as rho, and lift goes as rho too, so
     * an airframe without a motor cannot reach the Karman line — not because
     * anything forbids it, but because there is nothing up there to push
     * against. Physics, not permission. */
    float rocket_thrust;  /* N, vacuum-capable, density-independent */
    float rocket_ve;      /* m/s exhaust velocity; burn rate = thrust / ve */
    float propellant_cap; /* kg of propellant the tanks hold */
    float rcs_auth;       /* rad/s^2 attitude authority with no air over the surfaces */
    float heat_shield;    /* 0..1 tolerance to re-entry heating */
    float pressure;       /* 0..1 cabin integrity: survivability in vacuum */
    /* The fraction of local gravity this hull simply does not feel. Zero for
     * everything human. It is the one number in the flight model with no
     * engineering behind it, and that is deliberate: whatever is flying the
     * saucers is not doing it with wings, propellant or any of the physics the
     * rest of the game is built out of, and giving them a very large engine
     * instead would have made them a better aeroplane rather than something
     * else. At 1 they hang where they are put. */
    float antigrav;
    uint32_t visual_mask; /* FLY_VIS_* bits: fitted modules change the model */
    /* The best rarity bolted to this airframe, 0..4. The renderer paints the
     * module geometry with it: a build that took a hundred hours to assemble
     * should be visible from outside the aeroplane, and the only honest place
     * to show it is on the parts that are actually there. */
    unsigned char visual_rarity;
} fly_airframe;

/* visible module geometry flags (renderer reads these off the airframe) */
#define FLY_VIS_GUNPOD 0x001u
#define FLY_VIS_DROPTANK 0x002u
#define FLY_VIS_LONGWING 0x004u
#define FLY_VIS_SLATS 0x008u
#define FLY_VIS_TURBO 0x010u
#define FLY_VIS_CARGOPOD 0x020u
#define FLY_VIS_BELLYTANK 0x040u
#define FLY_VIS_RADARDOME 0x080u
#define FLY_VIS_ANTENNA 0x100u
#define FLY_VIS_ARMOR 0x200u
#define FLY_VIS_ROCKET 0x400u
#define FLY_VIS_PROPTANK 0x800u
#define FLY_VIS_RCS 0x1000u
#define FLY_VIS_SHIELD 0x2000u
#define FLY_VIS_CABIN 0x4000u
/* The armed loadout. Five more, and they exist for the reason the spaceframe's
 * five do: what an aeroplane is carrying should be visible from outside it.
 * Somebody deciding whether to pick a fight can now see the rails. */
#define FLY_VIS_ROCKETPOD 0x8000u
#define FLY_VIS_BOMBRACK 0x10000u
#define FLY_VIS_MISSILE 0x20000u
#define FLY_VIS_DISPENSER 0x40000u
#define FLY_VIS_EMITTER 0x80000u

typedef struct {
    float pitch, roll, yaw; /* -1..1 */
    float throttle;         /* 0..1 */
    int brakes;
    int fire;
    /* Light the candle. A latch rather than a throttle: a rocket that shares
     * the air-breathing throttle would light itself every time you firewalled
     * it in a fight and burn a tank of propellant you cannot buy back. */
    int rocket;
    /* Release ordnance, and shed a countermeasure. Both are "I want one now"
     * rather than latches: what stops a held key from emptying a magazine in a
     * second and a half is the launcher's own reload (`fly_craft.ord_cooldown`,
     * `cm_cooldown`), which is a property of the rack rather than of how the
     * input was edge-detected — and it has to be, because an AI pilot's
     * controls are recomputed every tick and would never produce an edge.
     *
     * They live on the controls rather than in the command vocabulary on
     * purpose. A pilot's commands are a subset of the player's and both go
     * through one door, so putting a launch here means the AI fires exactly
     * the way a human does, the journal already carries it, and the
     * multiplayer seam does not move at all. */
    int launch;
    int decoy;
    /* Pitch trim, added to the stick before anything reads it. An aeroplane
     * that is stable in pitch has exactly one attitude it will hold on its
     * own, and it is almost never the one you want — so without trim, cruise
     * is a hand permanently on the stick and nothing else can be done with
     * it. Trim is what turns "fly the aeroplane" into "set the aeroplane up
     * and then go and do the job", which is the difference between an
     * aircraft and a cursor. */
    float trim;             /* -1..1 */
} fly_controls;

typedef struct {
    fly_v3 wind;       /* m/s, world frame */
    float turbulence;  /* 0..1 */
    float precip;      /* 0..1 rain/ash intensity */
    float visibility;  /* m */
} fly_weather;

/* terrain height callback: world (x, y) -> ground elevation z */
typedef float (*fly_ground_fn)(void *user, float x, float y);

typedef struct {
    fly_v3 pos, vel;   /* world frame */
    fly_quat ori;      /* body -> world */
    fly_v3 omega;      /* body frame angular velocity, rad/s */
    float throttle;    /* actual (spooled) throttle */
    float fuel;        /* kg or MJ remaining */
    float propellant;  /* kg of rocket propellant remaining */
    /* Thermal load, 0..1. Earned by pushing air at speed and shed by not
     * doing so; at 1 the airframe is coming apart. Re-entry is the only thing
     * in the game that generates it in quantity, which is why a heat shield
     * is a space part and not a general upgrade. */
    float heat;
    float hp;          /* structure remaining */
    float wear;        /* 0..1 accumulated degradation */
    /* What is left in the magazines, and how long until either will answer
     * again. Consumables on the craft beside fuel and propellant, for the same
     * reason those are: they belong to the aeroplane in flight rather than to
     * the airframe on the shelf, and a pilot and the player carry them
     * identically because both fly one of these. */
    int ammo, cm;
    float ord_cooldown, cm_cooldown;
    /* Where the beam weapon's waste heat goes: into `heat` above, the same
     * field a re-entry fills. One thermal state, because an airframe does not
     * care what put the energy in it — and it means a beam build has a reason
     * to want a shield, which is a synergy worth having rather than a rule.
     * This is the latch that keeps it off until it has cooled far enough back
     * down, so an overheat is a lockout rather than a flicker at the limit. */
    int beam_locked;
    float gload;       /* last-step load factor */
    float alpha;       /* last-step angle of attack (plane) */
    /* Sideslip, radians, positive with the nose left of the airflow. The
     * number behind the slip ball, and the reason there is one: with adverse
     * yaw, propeller torque and a side force on the fuselage all in the
     * model, a turn is now something you coordinate rather than something you
     * ask for. */
    float slip;
    /* How close the wing is to letting go, 0..1, and then how hard it has let
     * go. Runs up before the stall (the buffet you feel through the airframe
     * a few knots early) rather than switching on at it, because the warning
     * is the interesting part — a stall you were told about is a mistake and
     * a stall you were not is a bug. */
    float buffet;
    /* Autorotation, 0..1. A stalled wing that is already rolling stalls
     * further on the down-going side and less on the up-going one, so the
     * roll feeds itself. This is that feedback, made visible: it is what
     * turns a stall into a spin, and letting go of the back pressure is what
     * turns it back into a stall. */
    float spin;
    /* Weight on wheels, 0..1: how much of the aircraft the gear is carrying
     * rather than the wing. Runs from 1 at rest to 0 at flying speed, and
     * every tyre force is scaled by it — which is why the rudder takes over
     * from the nosewheel by itself as the tail comes up. */
    float wow;
    /* The gust field this aircraft is sitting in, world frame m/s. Carried on
     * the craft rather than drawn fresh each step: weather that is re-rolled
     * every frame is grit, not air. See the gust model in fly_sim.c. */
    fly_v3 gust;
    float air_time;    /* seconds since leaving the ground */
    int on_ground;
    int stalled;
    int crashed;
} fly_craft;

/* --- the planet ---------------------------------------------------------
 *
 * The ground is a ball, and above about twenty kilometres that stops being a
 * detail. Gravity falls off with the square of the distance from the centre, a
 * craft going fast enough sideways stops coming down, and the air runs out.
 * All three are the same fact — the surface curves away — and this is where
 * that fact lives.
 *
 * The playfield is a tangent chart on this ball, not a re-projection of it.
 * The terrain, the renderer, the shadow cascades, the siting and every GLSL
 * twin are written in (x, y) metres, and a sixty-kilometre chart on a
 * six-hundred-kilometre sphere drops 750 m at its far corner under thirty
 * kilometres of haze — so re-projecting the heightfield costs the whole render
 * suite and buys nothing anyone inside the atmosphere could see. Every place
 * the curvature *is* observable it is real: the weight you fly against, the
 * speed you need to stay up, the air you have to climb out of, and the limb of
 * the planet once you are above it.
 *
 * The radius is not arbitrary. At 600 km the geometric horizon from a
 * three-hundred-metre cruise is 19 km and from three thousand it is 60 — which
 * is how far the renderer already draws before haze takes over, so the chart
 * and the ball agree about where the world ends.
 *
 * FLY_KARMAN is where the air stops being able to hold anything up: below it
 * you fly, above it you are on a trajectory. FLY_ATMOS_TOP is where the model
 * calls the density exactly zero — at 140 km the exponential is already
 * 6.5e-08 of sea level, so the step is far below anything measurable and it
 * makes "there is no air up here" true rather than nearly true. */
#define FLY_G0 9.80665f            /* surface gravity, m/s^2 */
#define FLY_PLANET_R 600000.0f     /* planet radius, m */
#define FLY_KARMAN 100000.0f       /* the line: above it, nothing flies */
#define FLY_ATMOS_TOP 140000.0f    /* above this the density model returns 0 */

/* Gravity at an altitude above the datum. */
float fly_gravity(float alt_m);
/* Speed for a circular orbit at that altitude, and the speed to leave. Both
 * closed form off the same mu = g0 R^2, so the sim and anything that quotes a
 * number to the player cannot drift apart. */
float fly_orbital_speed(float alt_m);
float fly_escape_speed(float alt_m);
/* Orbital period of a circular orbit at that altitude, seconds. */
float fly_orbital_period(float alt_m);

/* --- environment --- */
float fly_air_density(float alt_m);           /* ISA-like exponential model */

/* --- airframe --- */
void fly_airframe_default_plane(fly_airframe *af);
void fly_airframe_default_drone(fly_airframe *af);
/* load from a YAML map node (see data/airframes yaml files); missing keys keep
 * the kind's defaults */
void fly_airframe_from_yaml(fly_airframe *af, const fly_yaml_node *n);
/* wear/damage-adjusted effective quantities */
float fly_airframe_thrust_avail(const fly_airframe *af, const fly_craft *c);

/* --- craft lifecycle --- */
void fly_craft_init(fly_craft *c, const fly_airframe *af, fly_v3 pos);
/* place in trimmed level flight heading +x at given speed/altitude */
void fly_craft_trim(fly_craft *c, const fly_airframe *af, float alt, float speed);

/* advance one step; rng only feeds turbulence jitter (pass NULL for calm
 * deterministic playback), extra_mass = cargo aboard */
void fly_sim_step(fly_craft *c, const fly_airframe *af, const fly_controls *in,
                  const fly_weather *wx, fly_ground_fn ground, void *guser,
                  float extra_mass, fly_rng *rng, float dt);

/* Airspeed: how fast the aircraft is going through the air it is actually in,
 * gust included. It ignores the boundary layer the step applies within a wing
 * or two of the ground, which is the only place this and the sim's own number
 * differ and is worth a metre a second on short final. */
float fly_craft_airspeed(const fly_craft *c, const fly_weather *wx);

/* The load factor the airframe is stressed for, with the airframe's own
 * answer when it has not been given one. The instruments mark it, the AI
 * stays under it and the tests measure against it, and every one of them
 * having its own idea of what a wing can take is how a g-meter ends up
 * disagreeing with the spar. */
float fly_airframe_g_limit(const fly_airframe *af);

/* Stall speed in level flight at this altitude and all-up mass: the speed
 * below which the wing cannot hold the aeroplane up whatever the pilot does.
 * The instruments want it, the AI wants it, and every one of them wanting its
 * own copy is how three parts of the game end up disagreeing about where the
 * bottom of the envelope is. */
float fly_craft_stall_speed(const fly_airframe *af, float alt, float mass);

/* All-up mass: airframe, cargo, fuel and propellant. Fuel is mass on a plane
 * and a battery charge on a drone, which is the whole of why this is not one
 * addition at the call site. */
float fly_craft_mass(const fly_craft *c, const fly_airframe *af, float extra_mass);

#endif /* FLY_SIM_H */
