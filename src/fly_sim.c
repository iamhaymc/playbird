#include "fly_sim.h"

#include <string.h>

/* mu = g0 R^2, the only gravitational constant this world has */
#define FLY_MU (FLY_G0 * FLY_PLANET_R * FLY_PLANET_R)
/* rho*V^3 that an unshielded hull can just shed; see the heating term */
#define FLY_HEAT_REF 1.0e8f

/* --- reading the handling block -----------------------------------------
 *
 * Four ratios and three quantities, and every one of them has to survive an
 * airframe that has never heard of them: a struct built by memset, a YAML
 * file written before the block existed, a test that fills in six fields and
 * flies it. So nothing reads them directly. Zero means "nobody said", which
 * for a ratio is 1 and for the two derived quantities is the value the rest
 * of the airframe implies — a propeller pitched for a little over Vne, and a
 * spar stressed for what an aeroplane of that kind is stressed for. */
static float fly__ratio(float v) { return v > 0.0f ? v : 1.0f; }

/* --- what the trim wheel is worth ---------------------------------------
 *
 * A quarter of the elevator, and the fraction is the whole design of the
 * control. Trim at full stick authority is not a trim wheel, it is a second
 * elevator you can leave leaning on the stops: this airframe's elevator is
 * powerful enough that a third of it, held, commands an angle of attack past
 * the stall, so a "trimmed" cruise pitched to seventy degrees nose-up,
 * departed, and tumbled — which is a correct simulation of holding full back
 * stick for forty seconds and a useless one of setting the trim.
 *
 * At a quarter, the wheel's range is exactly the range it should have: fully
 * back trims to about the stall, fully forward trims to a fast descent, and
 * the middle of the wheel is the middle of the envelope. */
#define FLY_TRIM_AUTH 0.25f

/* The stick, with the wheel folded in. One function, so the elevator, the
 * drone's attitude command and the cold-gas thrusters cannot end up with
 * three opinions about what the pilot asked for. */
static float fly__pitch_cmd(const fly_controls *in) {
    return fly_clampf(in->pitch + in->trim * FLY_TRIM_AUTH, -1.0f, 1.0f);
}

/* Where the propeller stops pulling. Derived from Vne rather than given,
 * because the two are the same fact from different ends: an airframe's
 * never-exceed is set a little under where its engine could take it. */
static float fly__prop_v(const fly_airframe *af) {
    if (af->prop_v > 1.0f) return af->prop_v;
    return af->vne > 1.0f ? af->vne * 1.7f : 160.0f;
}

float fly_airframe_g_limit(const fly_airframe *af) {
    if (af->g_limit > 0.5f) return af->g_limit;
    return af->kind == FLY_CRAFT_DRONE ? 3.2f : 4.4f;
}

float fly_gravity(float alt_m) {
    float r = FLY_PLANET_R + alt_m, k;
    if (r < 1.0f) r = 1.0f;
    k = FLY_PLANET_R / r;
    return FLY_G0 * k * k;
}

float fly_orbital_speed(float alt_m) {
    float r = FLY_PLANET_R + alt_m;
    if (r < 1.0f) r = 1.0f;
    return sqrtf(FLY_MU / r);
}

float fly_escape_speed(float alt_m) {
    return fly_orbital_speed(alt_m) * 1.4142135624f;
}

float fly_orbital_period(float alt_m) {
    float r = FLY_PLANET_R + alt_m;
    if (r < 1.0f) r = 1.0f;
    return 2.0f * FLY_PI * r / fly_orbital_speed(alt_m);
}

float fly_air_density(float alt_m) {
    if (alt_m < 0.0f) alt_m = 0.0f;
    if (alt_m >= FLY_ATMOS_TOP) return 0.0f;
    return 1.225f * expf(-alt_m / 8500.0f);
}

/* --- what the ball does to a craft ---------------------------------------
 *
 * Two terms, and they are the whole of the difference between a flat world and
 * a round one.
 *
 * Radially: weight, less the centrifugal support of going round. A craft with
 * horizontal speed v at radius r is already turning, and v^2/r of its weight
 * is paid for by that turn. At a hundred knots down low this is 0.15% and you
 * will never notice; at orbital speed it is all of it, which is what an orbit
 * is. Writing it this way means there is no separate "space mode" — the same
 * expression that makes an aeroplane very slightly light makes a spacecraft
 * weightless, and nothing has to decide which one it is looking at.
 *
 * Transversely: angular momentum. r*v_h is conserved, so climbing costs
 * horizontal speed and falling gives it back. Leaving this out is the usual
 * way a flat-chart orbit goes wrong — the craft rises, keeps the horizontal
 * speed it had lower down, gets more centrifugal support than it is entitled
 * to, and flies away. With it, the radial motion is exactly the Kepler radial
 * problem, so apoapsis, periapsis and the period are the real ones rather than
 * something that merely looks orbital. */
static fly_v3 fly__planet_force(const fly_craft *c, const fly_airframe *af,
                                float mass) {
    float r = FLY_PLANET_R + c->pos.z;
    float vh2 = c->vel.x * c->vel.x + c->vel.y * c->vel.y;
    float g = fly_gravity(c->pos.z) * (1.0f - fly_clampf(af->antigrav, 0.0f, 1.0f));
    if (r < 1.0f) r = 1.0f;
    return fly_v3mk(0.0f, 0.0f, mass * (vh2 / r - g));
}

/* d(r*v_h)/dt = 0, applied after the step that changed r. */
static void fly__conserve_momentum(fly_craft *c, float dt) {
    float r = FLY_PLANET_R + c->pos.z, k;
    if (r < 1.0f) r = 1.0f;
    k = 1.0f - (c->vel.z / r) * dt;
    if (k < 0.0f) k = 0.0f;
    c->vel.x *= k;
    c->vel.y *= k;
}

/* ---------------- airframe ---------------- */

void fly_airframe_default_plane(fly_airframe *af) {
    memset(af, 0, sizeof *af);
    strcpy(af->name, "Skylark T1");
    strcpy(af->id, "skylark");
    af->kind = FLY_CRAFT_PLANE;
    af->mass = 950.0f;
    af->wing_area = 16.0f;
    af->wing_span = 11.0f;
    af->cl0 = 0.3f;
    af->cla = 5.5f;
    af->cl_max = 1.5f;
    af->alpha_stall = 15.0f * FLY_DEG2RAD;
    af->cd0 = 0.030f;
    af->k_ind = 0.055f;
    af->thrust_max = 3200.0f;
    af->fuel_cap = 120.0f;
    af->burn_rate = 0.012f;
    af->pitch_auth = 2.4f;
    af->roll_auth = 4.5f;
    af->yaw_auth = 1.2f;
    af->vref = 50.0f;
    af->stability = 1.0f;
    af->damping = 1.0f;
    af->inertia = 1.0f;
    af->spool = 1.0f;
    /* A single tractor propeller, and the aeroplane knows about it: full
     * power at low speed wants left roll and left yaw, and the pilot holds
     * right rudder or watches the runway go past the wingtip. */
    af->torque = 0.6f;
    af->g_limit = 4.4f;
    af->ceiling = 4500.0f;
    af->vne = 95.0f;
    af->structure = 100.0f;
    af->cargo_cap = 220.0f;
    af->gear_height = 1.2f;
    af->vtol = 0;
    af->weather_rating = 0.35f;
}

void fly_airframe_default_drone(fly_airframe *af) {
    memset(af, 0, sizeof *af);
    strcpy(af->name, "Dragonfly Q4");
    strcpy(af->id, "dragonfly");
    af->kind = FLY_CRAFT_DRONE;
    af->mass = 42.0f;
    af->wing_area = 0.4f; /* body reference area for drag */
    af->wing_span = 1.6f;
    af->cd0 = 0.9f;
    af->rotor_thrust = 1250.0f;
    af->fuel_cap = 18.0f;   /* MJ battery */
    af->burn_rate = 0.011f; /* MJ/s at hover-ish */
    af->pitch_auth = 9.0f;
    af->roll_auth = 9.0f;
    af->yaw_auth = 4.0f;
    af->vref = 15.0f;
    af->stability = 1.0f;
    af->damping = 1.0f;
    af->inertia = 1.0f;
    /* Electric rotors answer immediately, and four of them cancel each
     * other's torque — which is the reason there are four. */
    af->spool = 1.0f;
    af->torque = 0.0f;
    af->g_limit = 3.2f;
    af->ceiling = 2600.0f;
    af->vne = 42.0f;
    af->structure = 60.0f;
    af->cargo_cap = 60.0f;
    af->gear_height = 0.4f;
    af->vtol = 1;
    af->weather_rating = 0.2f;
}

void fly_airframe_from_yaml(fly_airframe *af, const fly_yaml_node *n) {
    const char *kind = fly_yaml_str(n, "kind", "plane");
    if (strcmp(kind, "drone") == 0) fly_airframe_default_drone(af);
    else fly_airframe_default_plane(af);

    const char *name = fly_yaml_str(n, "name", NULL);
    if (name) { strncpy(af->name, name, sizeof af->name - 1); af->name[sizeof af->name - 1] = 0; }
    const char *id = fly_yaml_str(n, "id", NULL);
    if (id) { strncpy(af->id, id, sizeof af->id - 1); af->id[sizeof af->id - 1] = 0; }

#define NUMF(field, key) af->field = (float)fly_yaml_num(n, key, af->field)
    NUMF(mass, "mass");
    NUMF(wing_area, "wing.area");
    NUMF(wing_span, "wing.span");
    NUMF(cl0, "aero.cl0");
    NUMF(cla, "aero.cla");
    NUMF(cl_max, "aero.cl_max");
    NUMF(cd0, "aero.cd0");
    NUMF(k_ind, "aero.k_induced");
    af->alpha_stall = (float)fly_yaml_num(n, "aero.alpha_stall_deg", af->alpha_stall * FLY_RAD2DEG) * FLY_DEG2RAD;
    NUMF(thrust_max, "engine.thrust");
    NUMF(rotor_thrust, "engine.rotor_thrust");
    NUMF(fuel_cap, "engine.fuel_cap");
    NUMF(burn_rate, "engine.burn_rate");
    NUMF(pitch_auth, "control.pitch");
    NUMF(roll_auth, "control.roll");
    NUMF(yaw_auth, "control.yaw");
    NUMF(vref, "control.vref");
    NUMF(stability, "control.stability");
    NUMF(damping, "control.damping");
    NUMF(inertia, "control.inertia");
    NUMF(spool, "engine.spool");
    NUMF(torque, "engine.torque");
    NUMF(prop_v, "engine.prop_v");
    NUMF(g_limit, "limits.g_limit");
    NUMF(ceiling, "limits.ceiling");
    NUMF(vne, "limits.vne");
    NUMF(structure, "limits.structure");
    NUMF(cargo_cap, "limits.cargo");
    NUMF(gear_height, "limits.gear_height");
    af->vtol = fly_yaml_bool(n, "vtol", af->vtol);
    NUMF(gun_dps, "modules.gun_dps");
    NUMF(gun_range, "modules.gun_range");
    NUMF(radar_range, "modules.radar_range");
    NUMF(weather_rating, "modules.weather_rating");
    NUMF(armor, "modules.armor");
#undef NUMF
}

float fly_airframe_thrust_avail(const fly_airframe *af, const fly_craft *c) {
    float base = af->kind == FLY_CRAFT_DRONE ? af->rotor_thrust : af->thrust_max;
    float density = fly_air_density(c->pos.z) / 1.225f;
    float health = 1.0f - 0.55f * c->wear; /* worn engines lose punch */
    return base * density * health;
}

/* ---------------- craft ---------------- */

void fly_craft_init(fly_craft *c, const fly_airframe *af, fly_v3 pos) {
    memset(c, 0, sizeof *c);
    c->pos = pos;
    c->ori = fly_qident();
    c->fuel = af->fuel_cap;
    c->propellant = af->propellant_cap;
    c->ammo = af->ord_ammo;
    c->cm = af->cm_ammo;
    c->hp = af->structure;
    c->on_ground = 1;
    c->gload = 1.0f;
}

/* --- what the aeroplane weighs ------------------------------------------
 *
 * Everything aboard, and fuel is aboard. It used not to be — only the rocket
 * propellant counted — so a Condor left with 340 kg in the tanks and landed
 * four hours later on fumes flying exactly the same aeroplane both times.
 * That is a third of the empty weight of a Skylark, and it is the difference
 * between a departure that will clear the trees and one that will not.
 *
 * A drone's `fuel` is a battery charge in megajoules and weighs the same
 * whether it is full or flat, which is the whole reason this is a function
 * and not an addition at each call site. */
float fly_craft_mass(const fly_craft *c, const fly_airframe *af, float extra_mass) {
    float m = af->mass + extra_mass + c->propellant;
    if (af->kind != FLY_CRAFT_DRONE) m += c->fuel;
    return m > 1.0f ? m : 1.0f;
}

float fly_craft_stall_speed(const fly_airframe *af, float alt, float mass) {
    float rho = fly_air_density(alt);
    float clmax = af->cl_max > 0.05f ? af->cl_max : 1.0f;
    float denom = 0.5f * rho * af->wing_area * clmax;
    if (af->kind == FLY_CRAFT_DRONE || denom < 1e-4f) return 0.0f;
    return sqrtf(mass * fly_gravity(alt) / denom);
}

void fly_craft_trim(fly_craft *c, const fly_airframe *af, float alt, float speed) {
    fly_craft_init(c, af, fly_v3mk(0, 0, alt));
    c->on_ground = 0;
    c->vel = fly_v3mk(speed, 0, 0);
    {
        float mass = fly_craft_mass(c, af, 0.0f);
        if (af->kind == FLY_CRAFT_PLANE) {
            /* pitch to the trim angle of attack for level flight */
            float rho = fly_air_density(alt);
            float qd = 0.5f * rho * speed * speed;
            float cl_need = qd > 1.0f ? (mass * fly_gravity(alt)) / (qd * af->wing_area) : af->cl0;
            float alpha = (fly_clampf(cl_need, -af->cl_max, af->cl_max) - af->cl0) / af->cla;
            c->ori = fly_qeuler(0, -alpha, 0); /* negative euler pitch = nose up (z-up frame) */
            c->throttle = 0.55f;
        } else {
            c->throttle = (mass * fly_gravity(alt)) /
                          (af->rotor_thrust > 1.0f ? af->rotor_thrust : 1.0f);
        }
    }
}

float fly_craft_airspeed(const fly_craft *c, const fly_weather *wx) {
    fly_v3 air = c->gust;
    if (wx) air = fly_v3add(air, wx->wind);
    return fly_v3len(fly_v3sub(c->vel, air));
}

/* ---------------- dynamics ---------------- */

/* --- the air this aeroplane is actually in ------------------------------
 *
 * One struct, computed once a step, read by the forces, the moments and the
 * wheels. It used to be computed three times from three slightly different
 * expressions — `plane_forces` and `plane_moments` each derived their own
 * angle of attack off their own copy of the relative wind, and the ground
 * code knew about neither — which is how a stall could be in progress
 * according to the lift curve and not according to the elevator. */
typedef struct {
    fly_v3 vair;  /* velocity through the air, world frame */
    fly_v3 vb;    /* the same in the body frame */
    float V;      /* airspeed */
    float rho;    /* local density */
    float q;      /* dynamic pressure */
    float qr;     /* q as a fraction of q at Vref at sea level: 1 is "normal" */
    float alpha;  /* angle of attack, rad */
    float beta;   /* sideslip: positive with the airflow coming from the right */
    float agl;    /* wheels above the ground, m */
    float ge;     /* induced-drag multiplier in ground effect; 1 is free air */
    float depth;  /* how far past the stall the wing is, 0..1 */
} fly__flow;

/* --- the boundary layer -------------------------------------------------
 *
 * The wind on the chart is the wind a few hundred feet up. At the surface it
 * is zero, because the ground is not moving, and in between it is a gradient
 * an aeroplane descends through in the last twenty seconds of every flight:
 * the headwind that was holding you up bleeds away and the aircraft settles.
 * Every real approach into wind is flown with that in mind, and without it
 * the whole of "landing in weather" is just a number in the corner of the
 * screen. Gentle on purpose — a third of the free-stream, over a wingspan or
 * six — because this is a thing to be respected, not an ambush. */
static float fly__shear(float agl) {
    float t = fly_clampf(agl / 60.0f, 0.0f, 1.0f);
    return 0.65f + 0.35f * sqrtf(t);
}

/* --- ground effect ------------------------------------------------------
 *
 * Inside a wingspan of the ground the wing's own downwash has nowhere to go,
 * the induced drag falls away and the aeroplane will not stop flying. It is
 * why an approach flown five knots fast floats the length of the apron and
 * why an overloaded aircraft can stagger off the deck and then be unable to
 * climb out of the cushion it left on. Wieselsberger's ratio, which is a
 * curve rather than a switch: nine tenths of it happens in the last half
 * span, so it arrives exactly when the pilot is busiest. */
static float fly__ground_effect(const fly_airframe *af, float agl) {
    float b = af->wing_span > 0.5f ? af->wing_span : 10.0f;
    float hb = agl / b, s;
    if (af->kind == FLY_CRAFT_DRONE) return 1.0f;
    if (hb > 1.0f) return 1.0f;
    s = 33.0f * hb * sqrtf(hb);
    return fly_clampf(s / (1.0f + s), 0.35f, 1.0f);
}

static void fly__flow_init(fly__flow *f, const fly_craft *c, const fly_airframe *af,
                           const fly_weather *wx, float gz) {
    float vref = af->vref > 1.0f ? af->vref : 50.0f;
    float qref = 0.5f * 1.225f * vref * vref;
    fly_v3 wind;
    f->agl = c->pos.z - gz;
    if (f->agl < 0.0f) f->agl = 0.0f;
    wind = fly_v3add(fly_v3scale(wx->wind, fly__shear(f->agl)), c->gust);
    f->vair = fly_v3sub(c->vel, wind);
    f->V = fly_v3len(f->vair);
    f->rho = fly_air_density(c->pos.z);
    f->q = 0.5f * f->rho * f->V * f->V;
    f->qr = fly_clampf(f->q / qref, 0.0f, 3.0f);
    f->vb = f->V > 0.5f ? fly_qrot(fly_qconj(c->ori), f->vair) : fly_v3mk(1, 0, 0);
    f->alpha = f->V > 0.5f ? atan2f(-f->vb.z, f->vb.x) : 0.0f;
    f->beta = f->V > 0.5f ? atan2f(-f->vb.y, f->vb.x) : 0.0f;
    f->ge = fly__ground_effect(af, f->agl);
    f->depth = 0.0f;
}

/* --- what a propeller can still pull at speed ---------------------------
 *
 * A propeller is pitched for a speed, and the faster the aeroplane is already
 * going the less bite the blade has left. Thrust that did not care — which is
 * what this model had — is a rocket motor with a spinner on the front: every
 * airframe accelerated to its never-exceed in level flight and sat there, so
 * Vne was a speed limit rather than a structural one and there was no such
 * thing as managing energy. With the fall-off, level top speed lands a little
 * under Vne on most airframes and the only way past it is downhill, which is
 * what a red line is supposed to mean. */
static float fly__prop_eff(const fly_airframe *af, float V) {
    float pv, r;
    if (af->kind == FLY_CRAFT_DRONE) return 1.0f;
    pv = fly__prop_v(af);
    r = V / (pv > 1.0f ? pv : 1.0f);
    return fly_clampf(1.0f - 0.55f * r * r, 0.25f, 1.0f);
}

/* --- what it takes to point it -----------------------------------------
 *
 * The three control authorities in `fly_airframe` are accelerations, which is
 * only honest for one loading: a torque divided by a moment of inertia that
 * never changes. So an aeroplane rolled at the same rate empty on the apron
 * and full of fuel, freight and three tonnes of rocket propellant — which is
 * the single most arcade thing the flight model did, because it means nothing
 * you load and nothing you bolt on can be felt through the stick.
 *
 * This returns the ratio of the moments of inertia to the nominal ones, so
 * every existing authority number keeps meaning exactly what it meant at the
 * airframe's empty weight, and everything above that weight costs you.
 *
 * Roll takes the whole of the mass-distribution term and the other two take a
 * share, because what a module hangs on an aeroplane it hangs on the wing,
 * and mass out towards the tip counts for far more in roll than it does in
 * pitch. A gun pod and a pair of drop tanks are half a second on the roll
 * rate; armour plate is a different aeroplane. */
static fly_v3 fly__inertia(const fly_airframe *af, float mass) {
    float m0 = af->mass > 1.0f ? af->mass : 1.0f;
    float mr = mass / m0;
    float k = fly__ratio(af->inertia);
    if (mr < 1.0f) mr = 1.0f;   /* nothing flies lighter than its own airframe */
    return fly_v3mk(mr * k, mr * (1.0f + (k - 1.0f) * 0.35f),
                    mr * (1.0f + (k - 1.0f) * 0.55f));
}

/* --- forces -------------------------------------------------------------
 *
 * Thrust, lift, drag and the side force, and nothing else. Weight is added by
 * the caller: keeping the specific force separate from the field it falls
 * through is what lets an accelerometer read zero in orbit. */
static fly_v3 plane_forces(fly_craft *c, const fly_airframe *af, fly__flow *f) {
    fly_v3 fwd = fly_qrot(c->ori, fly_v3mk(1, 0, 0));
    fly_v3 up = fly_qrot(c->ori, fly_v3mk(0, 0, 1));
    fly_v3 force = fly_v3scale(fwd, c->throttle * fly_airframe_thrust_avail(af, c) *
                                        fly__prop_eff(af, f->V));
    float astall = af->alpha_stall > 0.01f ? af->alpha_stall : 0.26f;
    float mag, cl, cd, cy, qS;
    fly_v3 vn, side, lift_dir;

    if (f->V < 0.5f) {
        c->alpha = c->slip = c->buffet = c->spin = 0.0f;
        c->stalled = 0;
        return force;
    }
    c->alpha = f->alpha;
    c->slip = f->beta;
    mag = fabsf(f->alpha);

    /* --- the lift curve, and the far end of it ---
     *
     * With hysteresis, which is the whole difference between a stall and a
     * flicker: a wing that has let go does not take hold again at the angle
     * it let go at, so recovering means actually unloading rather than easing
     * a hair off the back stop. Without it the flag chattered on and off
     * every other frame at the break and neither the pilot nor the AI could
     * tell whether they were flying. */
    c->stalled = mag > (c->stalled ? astall * 0.85f : astall);
    cl = af->cl0 + af->cla * f->alpha;
    if (c->stalled) {
        float over = mag - astall * 0.85f;
        float drop = fly_clampf(over * 3.0f, 0.0f, 0.85f);
        f->depth = fly_clampf(over / (astall * 0.7f), 0.0f, 1.0f);
        cl = fly_signf(f->alpha) * af->cl_max * (1.0f - drop);
    } else {
        cl = fly_clampf(cl, -af->cl_max, af->cl_max);
    }
    /* The buffet is the part worth modelling. A stall you were warned about
     * is a mistake and a stall you were not is a bug, so the airframe starts
     * talking at about four fifths of the way to the break and is shouting by
     * the time the wing goes — through the airframe, through the instrument
     * and through the stick. */
    c->buffet = fly_clampf((mag / astall - 0.80f) / 0.20f, 0.0f, 1.0f);
    cl *= 1.0f + 0.10f * (1.0f - f->ge);   /* the cushion holds it up too */

    cd = af->cd0 + af->k_ind * cl * cl * f->ge;
    if (c->stalled) cd += 0.06f + 0.12f * f->depth;
    /* A slipping aeroplane is a barn door, and until now it was a free one:
     * sideslip moved the moments and cost nothing, so a skidding turn was
     * as good as a coordinated one and the ball on the panel would have been
     * decoration. */
    cd += 0.75f * f->beta * f->beta;

    qS = f->q * af->wing_area;
    vn = fly_v3scale(f->vair, 1.0f / f->V);
    side = fly_v3cross(vn, up);
    lift_dir = fly_v3norm(fly_v3cross(side, vn));
    force = fly_v3add(force, fly_v3scale(lift_dir, qS * cl));
    force = fly_v3add(force, fly_v3scale(vn, -qS * cd));
    /* The fuselage and the fin as a very bad wing: they push back against the
     * slip, which is what makes a sideslip a way down as well as a way to
     * line up with a runway that is not where the wind wants you. Positive
     * beta is airflow from the right, so the force is to the left. */
    cy = 1.1f * f->beta;
    force = fly_v3add(force, fly_v3scale(fly_qrot(c->ori, fly_v3mk(0, 1, 0)), qS * cy));
    return force;
}

/* --- moments ------------------------------------------------------------
 *
 * Everything that turns the aeroplane, as accelerations at the nominal
 * loading, divided by the inertia ratio at the end. Control power goes as
 * dynamic pressure rather than as speed, which is the change that makes the
 * bottom of the envelope feel like the bottom of the envelope: at four
 * fifths of Vref the elevator has under two thirds of the authority it has at
 * Vref, not four fifths of it, and at altitude it has whatever the thin air
 * leaves. That is why an approach is flown on the numbers. */
static void plane_moments(fly_craft *c, const fly_airframe *af, const fly_controls *in,
                          const fly__flow *f, float mass, float dt) {
    float vref = af->vref > 1.0f ? af->vref : 50.0f;
    float wear = 1.0f - 0.45f * c->wear;      /* worn linkages lose bite */
    float ctl = f->qr * wear;
    float stab = fly__ratio(af->stability) * fly_clampf(f->qr, 0.0f, 1.5f);
    float dmp = fly__ratio(af->damping) *
                fly_clampf((f->rho / 1.225f) * (f->V / vref), 0.0f, 1.2f);
    float pitch_in = fly__pitch_cmd(in);
    fly_v3 acc, iner = fly__inertia(af, mass);

    /* body y is left, z is up: positive stick pitch = nose up (-y rotation),
     * positive roll = bank right (+x rotation), positive yaw = nose right (-z) */
    acc.y = -pitch_in * af->pitch_auth * ctl;
    acc.x = in->roll * af->roll_auth * ctl;
    acc.z = -in->yaw * af->yaw_auth * ctl;
    /* Adverse yaw. The down-going aileron drags more than the up-going one,
     * so rolling right swings the nose left and a turn entered on ailerons
     * alone starts by pointing the wrong way. This is the reason aeroplanes
     * have a third control and the reason the ball is worth watching. */
    acc.z += in->roll * af->yaw_auth * 0.22f * ctl;
    /* A stalled wing does not answer its aileron — the one control a pilot
     * reaches for in a wing drop is the one that makes it worse. */
    if (f->depth > 0.0f) acc.x *= 1.0f - 0.65f * f->depth;

    /* Static stability: weathervane in yaw, restore alpha toward trim,
     * dihedral roll out of the slip. Beta is positive with the airflow coming
     * from the right, so the fin swings the nose right (negative acc.z) and
     * the upwind wing rises. */
    acc.y += f->alpha * 2.2f * stab;   /* nose-down restoring moment for +alpha */
    acc.z += -f->beta * 1.8f * stab;   /* yaw into the wind */
    acc.x += f->beta * 0.9f * stab;    /* dihedral roll */

    /* --- what the propeller does to the aeroplane ---
     *
     * Torque, p-factor and slipstream, as one coupling because they arrive
     * together and always in the same direction: full power at low speed
     * rolls left and yaws left, hard. It is the reason a takeoff roll needs
     * right rudder, the reason a go-around from a trimmed approach tries to
     * put you off the side of the runway, and the reason the throttle is a
     * flight control rather than a speed setting.
     *
     * Measured against cruise power rather than against zero, because that is
     * how an aeroplane is *rigged*: the fin is offset and the trim tabs are
     * set so that the thing flies straight at the power it spends its life
     * at, and every hour of straight and level would otherwise be an hour of
     * holding rudder. Written the other way — proportional to power outright
     * — a hands-off cruise rolled slowly left for ever and spiralled into the
     * ground in half a minute, which is a correct simulation of an aeroplane
     * nobody rigged. So it is zero at cruise, it bites at full power, and it
     * reverses at idle, which is exactly when a pilot has other things to
     * think about.
     *
     * Constant with power rather than scaled by dynamic pressure, so it
     * dominates where the surfaces are weakest, and it fades with density
     * along with the engine that causes it. */
    if (af->torque > 0.0f) {
        float pw = (c->throttle - 0.5f) * 2.0f * fly_clampf(f->rho / 1.225f, 0.0f, 1.0f);
        float slow = fly_clampf(1.2f - f->qr, 0.30f, 1.2f);
        /* Squared about the rigging point, keeping its sign. A rigging that
         * only cancels at one exact throttle setting is a rigging that leaves
         * a slow wander at every other one, and "cruise" is a band rather
         * than a number; this gives it the band and keeps the whole of the
         * effect at the ends, where the aeroplane is being asked for
         * everything it has. */
        pw *= fabsf(pw);
        acc.x -= af->torque * 0.75f * pw;
        acc.z += af->torque * 0.55f * pw * slow;
    }

    /* --- autorotation ---
     *
     * A stalled wing that is already rolling stalls harder on the down-going
     * side and less on the up-going one, so the roll pays for itself and the
     * nose follows it round. That is a spin, and it is not a separate mode or
     * a state machine — it is this one term, sitting on top of the ordinary
     * damping and beating it once the wing is far enough past the break.
     * Which means the recovery is the real one: unload, and `depth` goes to
     * zero, and the term goes with it. Bounded so that a deep stall is a
     * handful rather than a coin toss. */
    if (f->depth > 0.0f) {
        acc.x += fly_clampf(c->omega.x, -2.0f, 2.0f) * 1.6f * f->depth;
        acc.z += -c->omega.x * 0.55f * f->depth;
    }

    /* aerodynamic damping */
    acc = fly_v3sub(acc, fly_v3scale(c->omega, 2.6f * (0.4f + dmp)));

    acc.x /= iner.x;
    acc.y /= iner.y;
    acc.z /= iner.z;
    c->omega = fly_v3add(c->omega, fly_v3scale(acc, dt));
    c->spin = fly_clampf(f->depth * fabsf(c->omega.x) / 1.6f, 0.0f, 1.0f);
}

static fly_v3 drone_forces(fly_craft *c, const fly_airframe *af, const fly__flow *f) {
    fly_v3 up = fly_qrot(c->ori, fly_v3mk(0, 0, 1));
    fly_v3 force;
    /* rotor inflow: climbing steals thrust, descending recovers a little */
    float climb = fly_v3dot(c->vel, up);
    float inflow = fly_clampf(1.0f - 0.04f * climb, 0.6f, 1.25f);
    force = fly_v3scale(up, c->throttle * inflow * fly_airframe_thrust_avail(af, c));
    if (f->V > 0.1f)
        force = fly_v3add(force,
                          fly_v3scale(f->vair, -0.5f * f->rho * f->V * af->cd0 * af->wing_area));
    c->alpha = 0.0f;
    c->slip = 0.0f;
    c->buffet = 0.0f;
    c->spin = 0.0f;
    return force;
}

static void drone_moments(fly_craft *c, const fly_airframe *af, const fly_controls *in,
                          float mass, float dt) {
    /* stick commands a target attitude; PD controller drives body rates */
    float max_tilt = 32.0f * FLY_DEG2RAD;
    float roll, pitch, yaw;
    float eff = 1.0f - 0.45f * c->wear;
    float tgt_pitch, tgt_roll;
    fly_v3 acc, iner = fly__inertia(af, mass);
    fly_qto_euler(c->ori, &roll, &pitch, &yaw);
    tgt_pitch = fly__pitch_cmd(in) * max_tilt;
    tgt_roll = in->roll * max_tilt;

    /* drive euler attitude toward the commanded tilt; positive pitch input =
     * nose up (matches the plane), so forward flight is negative pitch */
    acc.y = ((-tgt_pitch) - pitch) * af->pitch_auth * 0.6f * eff;
    acc.x = (tgt_roll - roll) * af->roll_auth * 0.6f * eff;
    acc.z = -in->yaw * af->yaw_auth * eff;

    acc = fly_v3sub(acc, fly_v3scale(c->omega, 5.0f));
    /* A loaded quadrotor is a slower quadrotor, for the same reason a loaded
     * aeroplane is: the flight controller can only ask, and the mass decides.
     * Gentler than the plane's — four rotors at the corners are most of this
     * airframe's inertia whatever it is carrying. */
    acc.x /= iner.x;
    acc.y /= iner.y;
    acc.z /= iner.z;
    c->omega = fly_v3add(c->omega, fly_v3scale(acc, dt));
}

/* --- the wheels ---------------------------------------------------------
 *
 * What this replaces was a quaternion assignment: on contact the aircraft's
 * roll was lerped to zero and its pitch into a band, directly, every frame.
 * It worked, in the sense that aeroplanes ended up flat on the ground, and it
 * meant the entire ground half of flying — the crosswind you have to hold off,
 * the swing under power, the rudder that stops being a nosewheel as the tail
 * comes up, the landing that arrives sideways and pays for it — did not
 * exist, because the undercarriage was not a set of forces, it was a rule
 * that the aeroplane must be level.
 *
 * Now it is forces. Three of them, all scaled by how much of the aeroplane
 * the tyres are still carrying:
 *
 *   - tyres do not go sideways, so drift is scrubbed off and a landing with
 *     drift on scrubs hard enough to be worth avoiding;
 *   - the nosewheel steers, and only while it is rolling, so it does nothing
 *     at a standstill and hands over to the fin as speed builds;
 *   - the gear holds the aeroplane level and the nose inside the band between
 *     a tail strike and a nose-over, stiffly, the way an undercarriage does —
 *     but as a spring, so a wing can be held down into a crosswind.
 */
static void plane_ground(fly_craft *c, const fly_airframe *af, const fly_controls *in,
                         const fly__flow *f, float mass, float dt) {
    float roll, pitch, yaw;
    float speed = fly_v3len(fly_v3mk(c->vel.x, c->vel.y, 0));
    fly_v3 acc = fly_v3zero(), iner = fly__inertia(af, mass);
    float wow = 1.0f;
    fly_qto_euler(c->ori, &roll, &pitch, &yaw);

    /* Weight on wheels: how much of the aeroplane the wing is *not* holding
     * up, from the lift the wing is actually making at the attitude it is
     * actually at. The wing takes the aeroplane off the tyres long before it
     * takes it off the ground, which is why the brakes stop working at the
     * far end of a landing roll and why the rudder takes over from the
     * nosewheel by itself.
     *
     * It was first written against the stall speed, and that is a different
     * quantity: the stall speed is the speed at which the wing can carry the
     * aeroplane at *maximum* lift, with the stick on the back stop. An
     * aeroplane rolling along the runway is at a few degrees with the nose
     * held down and is making a third of that, so reading it off the stall
     * speed said the tyres carried nothing at any respectable landing speed —
     * no friction, no steering and no gear holding the wings level, which is
     * exactly what a rollout needs most. Aircraft skipped down the runway
     * with a wing rising and wrote themselves off on the second contact. */
    {
        float cl = fly_clampf(af->cl0 + af->cla * f->alpha, -af->cl_max, af->cl_max);
        float lift = f->q * af->wing_area * cl;
        float weight = mass * fly_gravity(c->pos.z);
        if (weight > 1.0f) wow = 1.0f - fly_clampf(lift / weight, 0.0f, 1.0f);
    }
    c->wow = wow;
    c->buffet = 0.0f;
    c->spin = 0.0f;

    /* rolling resistance and brakes, on whatever the tyres still carry */
    {
        float fr = in->brakes ? 1.8f : 0.03f;
        float k = fly_clampf(1.0f - fr * wow * dt, 0.0f, 1.0f);
        c->vel.x *= k;
        c->vel.y *= k;
    }
    /* tyres do not go sideways */
    {
        fly_v3 side = fly_qrot(c->ori, fly_v3mk(0, 1, 0));
        float sv, k;
        side.z = 0.0f;
        side = fly_v3norm(side);
        sv = side.x * c->vel.x + side.y * c->vel.y;
        k = fly_clampf(4.5f * wow * dt, 0.0f, 1.0f);
        c->vel.x -= side.x * sv * k;
        c->vel.y -= side.y * sv * k;
    }

    /* nosewheel steering, gear roll stiffness, and the pitch band */
    acc.z += -in->yaw * 1.5f * wow * fly_clampf(speed / 7.0f, 0.0f, 1.0f);
    acc.x += (-roll * 8.0f - c->omega.x * 5.0f) * wow;
    {
        float lo = -0.38f, hi = 0.04f;
        float over = pitch < lo ? pitch - lo : pitch > hi ? pitch - hi : 0.0f;
        acc.y += (-over * 14.0f - c->omega.y * 4.0f) * wow;
    }
    acc.x /= iner.x;
    acc.y /= iner.y;
    acc.z /= iner.z;
    c->omega = fly_v3add(c->omega, fly_v3scale(acc, dt));
    c->omega.z *= fly_clampf(1.0f - 1.1f * wow * dt, 0.0f, 1.0f);

    /* Parked. At a walking pace on the wheels there is nothing left for the
     * model to say, and a hull that is about to be docked, boarded, walked
     * away from and drawn on an apron should be sitting flat while that
     * happens rather than resting on one wingtip. */
    if (speed < 1.0f && wow > 0.9f) {
        c->ori = fly_qeuler(0, 0, yaw);
        c->omega = fly_v3zero();
    }
}

/* A drone's legs are not wheels: it arrives vertically, it does not roll, and
 * the flight controller levels it because that is what a flight controller is
 * for. The old contact rule was always the right one here. */
static void drone_ground(fly_craft *c, const fly_controls *in, float dt) {
    float roll, pitch, yaw;
    float lvl = fly_clampf(6.0f * dt, 0.0f, 1.0f);
    float fr = in->brakes ? 1.8f : 0.6f;
    float k = fly_clampf(1.0f - fr * dt, 0.0f, 1.0f);
    fly_qto_euler(c->ori, &roll, &pitch, &yaw);
    c->vel.x *= k;
    c->vel.y *= k;
    c->wow = 1.0f;
    c->ori = fly_qeuler(fly_lerpf(roll, 0.0f, lvl), fly_lerpf(pitch, 0.0f, lvl), yaw);
    c->omega = fly_v3scale(c->omega, 1.0f - lvl);
}

/* --- gusts --------------------------------------------------------------
 *
 * Turbulence used to be a fresh random force every frame, which is white
 * noise: at sixty steps a second it averages out inside the time constant of
 * the airframe and arrives as grit rather than as air. Real turbulence is
 * correlated — a gust is a lump of moving air with a size, and you fly
 * through it over a second or two, and it is a wind while you are in it.
 *
 * So the gust is state, and it is an Ornstein-Uhlenbeck process: it decays
 * toward zero with a time constant of a couple of seconds and is kicked by
 * noise scaled so the variance is stationary whatever the step. And it is
 * added to the *wind*, not to the forces, which is the important part: a gust
 * changes the angle of attack and the sideslip, so it lifts a wing, unloads
 * the aeroplane, and can take a wing over the stall on short final — all of
 * which fall out of the aerodynamics rather than being simulated separately.
 *
 * Deterministic on a seed, and zero without an rng, which is what keeps
 * replays and the orbital aspects exact.
 *
 * It also closes a whole class of bug by construction. Turbulence used to be
 * a force, and a force does not care whether there is any air to make it —
 * so a spacecraft coasting at a hundred and fifty kilometres, handed the
 * ground's storm by a caller that had no business handing it anything, was
 * shaken by weather in a vacuum. That was patched in the weather model, which
 * is the wrong place: it made "no turbulence up there" a promise one function
 * had to keep. A gust that is a *wind* cannot do it at all, whatever anybody
 * passes in, because the force it makes is q times a coefficient and q is
 * zero when there is nothing to be dynamic. */
static void fly__gust_step(fly_craft *c, const fly_airframe *af, const fly_weather *wx,
                           fly_rng *rng, float dt) {
    float turb = wx->turbulence * (1.0f - 0.7f * fly_clampf(af->weather_rating, 0.0f, 1.0f));
    float a = expf(-dt / 2.6f);
    if (rng && turb > 0.001f) {
        float s = turb * 7.5f * sqrtf(1.0f - a * a);
        c->gust = fly_v3mk(c->gust.x * a + fly_rng_gauss(rng) * s,
                           c->gust.y * a + fly_rng_gauss(rng) * s,
                           c->gust.z * a + fly_rng_gauss(rng) * s * 0.8f);
    } else {
        c->gust = fly_v3scale(c->gust, a);
    }
}

void fly_sim_step(fly_craft *c, const fly_airframe *af, const fly_controls *in,
                  const fly_weather *wx, fly_ground_fn ground, void *guser,
                  float extra_mass, fly_rng *rng, float dt) {
    static const fly_weather calm = { { 0, 0, 0 }, 0, 0, 20000.0f };
    fly__flow f;
    float mass, rho, gz, floor_z, bounce = 0.0f;
    int lit = 0;
    if (!wx) wx = &calm;
    if (c->crashed || dt <= 0.0f) return;

    /* Propellant is mass, and most of it. A fuelled rocket stack is heavier
     * than the airframe carrying it, so the ascent gets lighter as it burns —
     * which is the whole reason the rocket equation is logarithmic and the
     * reason a tank that counted as fixed weight would never reach orbit. And
     * so is the fuel: see fly_craft_mass. */
    mass = fly_craft_mass(c, af, extra_mass);
    rho = fly_air_density(c->pos.z);
    gz = ground ? ground(guser, c->pos.x, c->pos.y) : 0.0f;

    /* the air first, so the forces, the moments and the wheels all read the
     * same gust, the same angle of attack and the same height off the deck */
    fly__gust_step(c, af, wx, rng, dt);
    fly__flow_init(&f, c, af, wx, gz);

    /* throttle spool + fuel */
    {
        float tgt = c->fuel > 0.0f ? fly_clampf(in->throttle, 0.0f, 1.0f) : 0.0f;
        float spool = (af->kind == FLY_CRAFT_DRONE ? 6.0f : 1.6f) * fly__ratio(af->spool);
        float burn;
        c->throttle += fly_clampf(tgt - c->throttle, -spool * dt, spool * dt);
        burn = af->burn_rate * (0.12f + 0.88f * c->throttle) * dt;
        c->fuel = c->fuel > burn ? c->fuel - burn : 0.0f;
    }

    /* the candle */
    if (in->rocket && af->rocket_thrust > 0.0f && af->rocket_ve > 0.0f &&
        c->propellant > 0.0f) {
        float pb = af->rocket_thrust / af->rocket_ve * dt;
        lit = 1;
        c->propellant = c->propellant > pb ? c->propellant - pb : 0.0f;
    }

    /* forces */
    fly_v3 force = af->kind == FLY_CRAFT_DRONE ? drone_forces(c, af, &f)
                                               : plane_forces(c, af, &f);
    /* The rocket pushes along the nose whatever is or is not outside. */
    if (lit)
        force = fly_v3add(force, fly_v3scale(fly_qrot(c->ori, fly_v3mk(1, 0, 0)),
                                             af->rocket_thrust));

    /* moments */
    if (af->kind == FLY_CRAFT_DRONE) drone_moments(c, af, in, mass, dt);
    else plane_moments(c, af, in, &f, mass, dt);

    /* The buffet, felt rather than read. An airframe on the edge shakes, and
     * the shake is what tells a pilot the wing is about to go without them
     * having to be looking at an instrument. Only with an rng, so a calm
     * deterministic playback is still exactly calm and deterministic. */
    if (rng && c->buffet > 0.05f && !c->on_ground) {
        float s = c->buffet * 0.9f * sqrtf(dt);
        c->omega = fly_v3add(c->omega, fly_v3mk(fly_rng_gauss(rng) * s,
                                                fly_rng_gauss(rng) * s * 0.6f,
                                                fly_rng_gauss(rng) * s * 0.5f));
    }

    /* Reaction control. Aerodynamic surfaces need air over them, and
     * plane_moments already scales their authority by dynamic pressure —
     * which in vacuum is a craft with a control column connected to nothing.
     * Cold gas does not care, so RCS authority is flat, and it is the only
     * thing pointing the ship once the air runs out. Deliberately an order of
     * magnitude weaker than the elevator: it turns you round for a burn, it
     * does not let you dogfight in orbit. */
    if (af->rcs_auth > 0.0f) {
        fly_v3 rcs;
        float damp = fly_clampf(1.0f - 1.2f * dt, 0.0f, 1.0f);
        rcs.y = -fly__pitch_cmd(in) * af->rcs_auth;
        rcs.x = in->roll * af->rcs_auth;
        rcs.z = -in->yaw * af->rcs_auth;
        c->omega = fly_v3add(c->omega, fly_v3scale(rcs, dt));
        /* and something to stop with, or a nudge becomes a permanent tumble */
        if (rho < 0.01f && fabsf(in->pitch) + fabsf(in->roll) + fabsf(in->yaw) < 0.02f)
            c->omega = fly_v3scale(c->omega, damp);
    }

    /* G-load is the specific force an accelerometer reads: everything the
     * airframe and the air do, and nothing the field does. It used to be
     * computed from the total acceleration with a gravity term added back,
     * which is the same number down low and the wrong one anywhere the field
     * is not 9.81 — a craft coasting in orbit came out at 1 g. */
    c->gload = fly_v3len(force) / (mass * FLY_G0);

    /* integrate */
    {
        fly_v3 acc = fly_v3scale(fly_v3add(force, fly__planet_force(c, af, mass)), 1.0f / mass);
        c->vel = fly_v3add(c->vel, fly_v3scale(acc, dt));
        c->pos = fly_v3add(c->pos, fly_v3scale(c->vel, dt));
    }
    fly__conserve_momentum(c, dt);
    c->ori = fly_qintegrate(c->ori, c->omega, dt);

    /* ground contact */
    gz = ground ? ground(guser, c->pos.x, c->pos.y) : 0.0f;
    floor_z = gz + af->gear_height;
    if (c->pos.z <= floor_z) {
        float sink = -c->vel.z;
        float speed = fly_v3len(fly_v3mk(c->vel.x, c->vel.y, 0));
        float roll, pitch, yaw;
        float max_speed = af->kind == FLY_CRAFT_DRONE ? 9.0f : af->vne * 0.6f;
        fly_qto_euler(c->ori, &roll, &pitch, &yaw);
        if (!c->on_ground) {
            /* impact damage scales with how badly limits were blown; brief
             * skips during a takeoff/landing roll are harmless */
            if (c->air_time > 0.8f) {
                /* How much of the arrival was sideways. An aeroplane put down
                 * with drift on lands on the side of its tyres, and until the
                 * gear had side friction there was nothing to charge for it —
                 * a crosswind landing was the same landing with the scenery
                 * moving. */
                float drift = 0.0f;
                float dmg = 0.0f;
                if (af->kind != FLY_CRAFT_DRONE && speed > 1.0f) {
                    fly_v3 side = fly_qrot(c->ori, fly_v3mk(0, 1, 0));
                    drift = fabsf(side.x * c->vel.x + side.y * c->vel.y);
                }
                if (sink > 3.2f) dmg += 6.0f + (sink - 3.2f) * 7.0f;
                if (speed > max_speed) dmg += (speed - max_speed) * 0.8f;
                if (fabsf(roll) > 0.45f) dmg += 12.0f;
                if (pitch > 0.28f) dmg += 15.0f + speed * 0.5f; /* nose-first */
                if (drift > 6.0f) dmg += (drift - 6.0f) * 2.5f;
                c->hp -= dmg;
                if (c->hp <= 0.0f) { c->hp = 0.0f; c->crashed = 1; }
            }
            /* An undercarriage gives some of it back. A firm arrival bounces,
             * which is a thing to fly out of rather than a thing that has
             * silently happened to you, and it is how a landing that was very
             * nearly good enough tells you it was not. */
            if (sink > 1.5f) bounce = sink * 0.22f;
            c->on_ground = 1;
        }
        c->air_time = 0.0f;
        c->pos.z = floor_z;
        if (c->vel.z < 0.0f) c->vel.z = bounce;
        if (af->kind == FLY_CRAFT_DRONE) drone_ground(c, in, dt);
        else plane_ground(c, af, in, &f, mass, dt);
    } else {
        c->on_ground = 0;
        c->wow = 0.0f;
        c->air_time += dt;
    }

    /* Over-speed structural stress, against dynamic pressure rather than
     * against speed.
     *
     * Vne is a load, not a velocity: what bends an airframe is 1/2 rho V^2, so
     * the limit is an *equivalent* airspeed and a craft may legitimately exceed
     * it in true airspeed by climbing. Reading it off raw speed was survivable
     * while the ceiling was four kilometres and fatal the moment anything went
     * higher — a spacecraft coasting through vacuum at two kilometres a second
     * tore itself apart in four steps against air that was not there. */
    {
        float eas = f.V * sqrtf(rho / 1.225f);
        if (eas > af->vne) c->hp -= (eas - af->vne) * 0.25f * dt;
    }

    /* --- the other limit ---
     *
     * Vne was the only structural number the model had, and it is the one a
     * pilot has to work at to exceed. The one they hit by accident is this
     * one: a wing at speed can pull several times what the spar is stressed
     * for, so the aeroplane is perfectly capable of tearing itself apart in a
     * turn it had no business making.
     *
     * Quadratic past the limit, and the shape is doing real work. A gust in a
     * storm can put a spike of a g and a half over the limit through the wing
     * for a fifth of a second, and a sustained pull can hold three over for
     * ten seconds; those are not the same event and a linear rate charges
     * them as if they were. Squared, the spike costs a fifth of a point and
     * the pull costs the airframe — so an aeroplane can be flown through
     * weather, which is what a rate steep enough to punish the turn was
     * quietly making impossible, and still cannot be flown at the edge of the
     * envelope for free. */
    {
        float glim = fly_airframe_g_limit(af);
        if (c->gload > glim) {
            float over = c->gload - glim;
            c->hp -= over * over * 0.9f * dt;
            c->wear = fly_clampf(c->wear + over * 0.00012f * dt, 0.0f, 1.0f);
        }
    }

    /* Re-entry heating.
     *
     * Convective load goes as rho*V^3, which is why coming back is a harder
     * problem than going up: the same kinetic energy has to leave as heat, and
     * the only place to put it is the air. Referenced against FLY_HEAT_REF so
     * that ordinary flight sits below the rate the hull sheds at — a Condor at
     * Vne down low earns 0.01 a second against a 0.06 loss, so nothing in the
     * aeroplane game ever warms up — while an unshielded entry at two
     * kilometres a second is an order of magnitude over it and lasts about a
     * second. A shield buys a factor of ten, which turns "impossible" into
     * "come in shallow". */
    {
        float q = rho * f.V * f.V * f.V / FLY_HEAT_REF;
        q *= 1.0f - 0.90f * fly_clampf(af->heat_shield, 0.0f, 1.0f);
        c->heat = fly_clampf(c->heat + (q - 0.06f) * dt, 0.0f, 1.0f);
        if (c->heat >= 1.0f) c->hp -= 30.0f * dt;
    }

    /* The magazines' clocks, and the beam's.
     *
     * Here rather than in the combat step because every aeroplane in the world
     * runs this function and only the handful in a fight run that one: a
     * reload that ticked at the trigger would leave a pilot who broke off
     * mid-reload holding a launcher that never came back. The lockout releases
     * well below where it engaged, so an overheated emitter is out of the fight
     * for a few seconds rather than stuttering on and off at the limit. */
    if (c->ord_cooldown > 0.0f) c->ord_cooldown -= dt;
    if (c->cm_cooldown > 0.0f) c->cm_cooldown -= dt;
    if (c->beam_locked && c->heat < 0.45f) c->beam_locked = 0;

    /* degradation: time, g-load stress, storms/rain, stall buffet (per second) */
    {
        float wear_rate = 0.0000027f;
        wear_rate += fly_clampf(c->gload - 2.5f, 0.0f, 6.0f) * 0.000015f;
        wear_rate += wx->precip * (1.0f - af->weather_rating) * 0.00002f;
        if (c->stalled) wear_rate += 0.000008f;
        c->wear = fly_clampf(c->wear + wear_rate * dt, 0.0f, 1.0f);
    }

    if (c->hp <= 0.0f) { c->hp = 0.0f; c->crashed = 1; }
}
