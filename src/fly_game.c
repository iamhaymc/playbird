#include "fly_game.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLY_DAY 86400.0
#define FLY_DOCK_RANGE 450.0f

/* ---------------- modules ---------------- */

/* A multiplier a module did not mention is 1, not 0.
 *
 * The registry is written with designated initializers, which leave every
 * unmentioned field zero, and there are now eight multipliers on a module of
 * which a typical one sets none. Requiring each of the forty-eight rows to
 * write out `= 1` eight times to say "this changes nothing about that" would
 * be eight columns of noise per row in a table whose whole point is that a
 * module says only what it does — and the first one anybody forgot would be
 * an aeroplane that cannot roll. So zero means unset, everywhere, and nothing
 * reads a multiplier without coming through here. */
static float fly__mul(float m) { return m > 0.0f ? m : 1.0f; }

/* Built-in registry; data/modules.yaml can override or extend it.
 *
 * Written with designated initializers, and not as a style preference: the
 * struct carries twenty-six fields, of which a typical module sets four, and
 * the positional form this replaced was rows of a dozen bare zeros in which
 * `gun_range` and `armor` were told apart by counting commas. Adding the
 * rocket stack to that would have meant six more columns on every row of a
 * table nobody could read. Here a module says only what it does. */
static fly_module fly__modules[FLY_MODULE_MAX] = {
    { .id = "tuned_engine", .name = "Tuned Engine", .desc = "+25% thrust",
      .slot = FLY_SLOT_ENGINE, .craft_kind = -1, .cost = 340,
      .thrust_mul = 1.25f, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 20,
      .spool_mul = 0.88f, .torque_mul = 1.25f, .visual = FLY_VIS_TURBO },
    { .id = "highalt_turbine", .name = "High-Alt Turbine",
      .desc = "+12% thrust, ceiling +1800 m",
      .slot = FLY_SLOT_ENGINE, .craft_kind = 0, .cost = 420,
      .thrust_mul = 1.12f, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 16,
      .ceiling_add = 1800, .spool_mul = 0.62f, .torque_mul = 1.15f,
      .visual = FLY_VIS_TURBO },
    { .id = "extended_wings", .name = "Extended Wings",
      .desc = "big wing: lower stall, and a lazy roll",
      .slot = FLY_SLOT_WINGS, .craft_kind = 0, .cost = 380,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 12,
      .vne_add = -6, .cd0_add = -0.004f,
      .wing_area_mul = 1.18f, .wing_span_mul = 1.22f, .cl_max_add = 0.12f,
      .alpha_stall_add = 1.5f * FLY_DEG2RAD, .roll_mul = 0.78f,
      .inertia_mul = 1.14f, .damping_mul = 1.15f, .stability_mul = 1.10f,
      .visual = FLY_VIS_LONGWING },
    { .id = "stol_kit", .name = "STOL Kit", .desc = "slats: a much lower stall, VTOL pads open",
      .slot = FLY_SLOT_WINGS, .craft_kind = 0, .cost = 520,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 18,
      .vne_add = -10, .cd0_add = 0.002f, .vtol = 1,
      .wing_area_mul = 1.06f, .cl_max_add = 0.45f,
      .alpha_stall_add = 5.0f * FLY_DEG2RAD, .roll_mul = 0.90f,
      .pitch_mul = 1.10f, .damping_mul = 1.10f, .stability_mul = 1.15f,
      .visual = FLY_VIS_SLATS },
    { .id = "gun_pod", .name = "Gun Pod", .desc = "14 dps autocannon, 900 m",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 450,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 40,
      .vne_add = -3, .cd0_add = 0.002f, .gun_dps = 14, .gun_range = 900,
      .roll_mul = 0.88f, .inertia_mul = 1.15f, .visual = FLY_VIS_GUNPOD },
    { .id = "drop_tanks", .name = "Drop Tanks", .desc = "+50% fuel; tanks on the pylons cost the roll",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 300,
      .thrust_mul = 1, .fuel_mul = 1.5f, .cargo_mul = 1, .mass_add = 26,
      .vne_add = -3, .cd0_add = 0.002f,
      .roll_mul = 0.82f, .inertia_mul = 1.22f, .visual = FLY_VIS_DROPTANK },
    { .id = "cargo_racks", .name = "Cargo Racks", .desc = "+80% cargo capacity",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 300,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1.8f, .mass_add = 25,
      .vne_add = -2, .cd0_add = 0.001f,
      .pitch_mul = 0.95f, .inertia_mul = 1.06f, .visual = FLY_VIS_CARGOPOD },
    { .id = "belly_tank", .name = "Belly Tank", .desc = "+45% fuel capacity",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 320,
      .thrust_mul = 1, .fuel_mul = 1.45f, .cargo_mul = 1, .mass_add = 30,
      .vne_add = -2, .cd0_add = 0.001f,
      .inertia_mul = 1.05f, .damping_mul = 0.95f, .visual = FLY_VIS_BELLYTANK },
    { .id = "radar_array", .name = "Radar Array", .desc = "9 km radar: find sites, contacts",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 380,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 8,
      .cd0_add = 0.001f, .radar_range = 9000,
      .yaw_mul = 0.97f, .stability_mul = 0.96f, .visual = FLY_VIS_RADARDOME },
    { .id = "storm_nav", .name = "Storm Nav Suite",
      .desc = "weather rating 0.7: storm belt opens",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 420,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 10,
      .weather_rating = 0.7f, .damping_mul = 1.15f, .stability_mul = 1.12f,
      .visual = FLY_VIS_ANTENNA },
    { .id = "armor_plating", .name = "Armor Plating", .desc = "-35% incoming, and it rolls like a barn door",
      .slot = FLY_SLOT_HULL, .craft_kind = -1, .cost = 340,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 60,
      .vne_add = -4, .cd0_add = 0.002f, .armor = 0.35f,
      .roll_mul = 0.85f, .pitch_mul = 0.90f, .inertia_mul = 1.20f,
      .damping_mul = 1.10f, .stability_mul = 1.05f, .g_limit_add = 0.4f,
      .visual = FLY_VIS_ARMOR },
    { .id = "light_frame", .name = "Lightened Frame", .desc = "-70 kg: quick, twitchy, and bends sooner",
      .slot = FLY_SLOT_HULL, .craft_kind = -1, .cost = 360,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = -70,
      .vne_add = 3, .structure_add = -15,
      .roll_mul = 1.18f, .pitch_mul = 1.10f, .inertia_mul = 0.85f,
      .damping_mul = 0.88f, .stability_mul = 0.90f, .g_limit_add = -0.8f },

    /* --- the armed loadout -------------------------------------------------
     *
     * Nine parts, and the ladder they climb is `min_ilvl` rather than price.
     * That distinction is the one the rocket stack already draws and it matters
     * more here than anywhere: tokens accumulate, so price alone only delays a
     * purchase, where item level decides whether a thing is on the shelf at
     * all. Rockets, bombs and *flares* sit at zero and are on the home apron
     * from the first hour, because the day the world gets missiles in it is the
     * day a new operator has to be able to buy the answer to one. Everything
     * that follows a target of its own accord starts at 25 and climbs.
     *
     * Read across a row and each one gives up something real. The rocket pod is
     * a gun that cannot correct. The bomb rack is devastating and worthless
     * against anything airborne. The seeker rack carries four rounds. The dart
     * needs the shooter to keep pointing. The emitter cannot be dodged and
     * cooks the aeroplane holding it. And every dispenser is fitted where the
     * cargo would have gone, which is the trade that makes carrying one a
     * decision rather than an upgrade. */
    { .id = "rocket_pod", .name = "Rocket Pod",
      .desc = "salvo of four, 1.4 km, no second chances",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 520,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 52,
      .vne_add = -4, .cd0_add = 0.003f,
      .ord_kind = FLY_ORD_ROCKET, .ord_range = 1400, .ord_damage = 1, .ord_ammo = 24,
      .roll_mul = 0.86f, .inertia_mul = 1.18f, .visual = FLY_VIS_ROCKETPOD },
    { .id = "bomb_rack", .name = "Bomb Rack",
      .desc = "six of them; nothing at all for a fight in the air",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 480,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.55f, .mass_add = 90,
      .vne_add = -6, .cd0_add = 0.003f,
      .ord_kind = FLY_ORD_BOMB, .ord_range = 2600, .ord_damage = 1, .ord_ammo = 6,
      .pitch_mul = 0.90f, .roll_mul = 0.88f, .inertia_mul = 1.22f,
      .visual = FLY_VIS_BOMBRACK },
    { .id = "flare_pod", .name = "Flare Dispenser",
      .desc = "24 flares: what a seeker looks at instead of you",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 360,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.82f, .mass_add = 34,
      .cd0_add = 0.001f, .cm_kind = FLY_ORD_FLARE, .cm_ammo = 24,
      .inertia_mul = 1.04f, .visual = FLY_VIS_DISPENSER },
    { .id = "chaff_pod", .name = "Chaff Pod",
      .desc = "20 clouds: a dart's idea of a parked aeroplane",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 420,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.85f, .mass_add = 28,
      .cd0_add = 0.001f, .cm_kind = FLY_ORD_CHAFF, .cm_ammo = 20,
      .inertia_mul = 1.03f, .visual = FLY_VIS_DISPENSER, .min_ilvl = 20 },
    { .id = "seeker_rack", .name = "Seeker Rack",
      .desc = "four heat seekers: slow, patient, and it follows",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1100,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 78,
      .vne_add = -7, .cd0_add = 0.004f,
      .ord_kind = FLY_ORD_SEEKER, .ord_range = 2400, .ord_damage = 1, .ord_ammo = 4,
      .roll_mul = 0.80f, .pitch_mul = 0.94f, .inertia_mul = 1.26f,
      .visual = FLY_VIS_MISSILE, .min_ilvl = 25 },
    { .id = "ecm_suite", .name = "Jamming Suite",
      .desc = "a seeker's head, told the wrong thing",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 980,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 26,
      .cd0_add = 0.002f, .ecm = 0.55f,
      .yaw_mul = 0.96f, .stability_mul = 0.95f,
      .visual = FLY_VIS_ANTENNA, .min_ilvl = 35 },
    { .id = "dart_rail", .name = "Dart Rail",
      .desc = "4 km, and you have to keep looking at them",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1850,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 96,
      .vne_add = -8, .cd0_add = 0.004f,
      .ord_kind = FLY_ORD_DART, .ord_range = 4200, .ord_damage = 1, .ord_ammo = 3,
      .roll_mul = 0.78f, .pitch_mul = 0.92f, .inertia_mul = 1.30f,
      .visual = FLY_VIS_MISSILE, .min_ilvl = 45 },
    { .id = "decoy_suite", .name = "Countermeasure Suite",
      .desc = "40 flares and a jammer under them",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1560,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.7f, .mass_add = 62,
      .cd0_add = 0.002f, .cm_kind = FLY_ORD_FLARE, .cm_ammo = 40, .ecm = 0.32f,
      .inertia_mul = 1.08f, .visual = FLY_VIS_DISPENSER, .min_ilvl = 55 },
    { .id = "beam_emitter", .name = "Beam Emitter",
      .desc = "30 dps, 700 m, instant — and it cooks the airframe",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 2400,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 120,
      .vne_add = -6, .cd0_add = 0.003f,
      .gun_dps = 30, .gun_range = 700, .beam_heat = 0.22f,
      .roll_mul = 0.82f, .inertia_mul = 1.28f, .stability_mul = 1.04f,
      .visual = FLY_VIS_EMITTER, .min_ilvl = 65 },

    /* --- the rocket stack -------------------------------------------------
     *
     * Five parts, five different slots, and that is the design rather than an
     * accident of where they fit. Going to space costs you the gun, the cargo
     * racks, the armour, the avionics and whatever was in the engine bay: a
     * spacecraft is not an aeroplane with an extra part bolted on, it is a
     * different build of the same airframe, and you keep one or the other.
     *
     * Priced and weighted to be the end of the ladder. The stack is heavier
     * than a Skylark and costs more than most people's second hull, and the
     * propellant alone outmasses the airframe carrying it — which is what a
     * rocket is.
     *
     * The stack is also a spaceframe conversion, and the numbers say so. Every
     * other module in this table trades a little drag and a little Vne for what
     * it does, because it is a thing bolted to an aeroplane. These five are the
     * other way round: faired nacelles, a sealed and pressure-stiffened bay, an
     * aeroshell that is the outer mould line, and mounts stressed for two
     * kilometres a second. Without that, the honest answer is that a winged
     * hauler tears itself apart at max-Q about ninety seconds into the burn —
     * which it did, on the first run of `sim.ascent`. A cargo aircraft with a
     * rocket strapped to it is not a launch vehicle; a cargo aircraft rebuilt
     * around one is.
     *
     * And all five carry `min_ilvl`, which is the other half of being the end
     * of the ladder. Price alone never was one: tokens accumulate, so a part
     * sitting on the home apron's shelf is a part you will own, and with the
     * base picked uniformly the stack was 29% of every shelf in the world from
     * the first minute. FLY_SPACE_ILVL puts them on the hard half of the map —
     * the shelves you need the gated sites to reach, and the country that
     * breeds the wrecks worth walking to. */
    { .id = "rocket_motor", .name = "Rocket Motor", .desc = "110 kN in vacuum, 4500 m/s exhaust",
      .slot = FLY_SLOT_ENGINE, .craft_kind = 0, .cost = 5200,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 260,
      .vne_add = 40, .cd0_add = -0.004f,
      .rocket_thrust = 110000.0f, .rocket_ve = 4500.0f,
      /* The propeller is gone; what is bolted on in its place reacts against
       * nothing. Not zero, because a multiplier a module leaves at zero reads
       * as unset — see fly__mul — and this is the one part in the registry
       * that means it. */
      .torque_mul = 0.02f, .spool_mul = 1.4f, .inertia_mul = 1.10f,
      .visual = FLY_VIS_ROCKET, .min_ilvl = FLY_SPACE_ILVL },
    { .id = "prop_tank", .name = "Propellant Tank", .desc = "3600 kg of propellant; the hold goes",
      .slot = FLY_SLOT_BAY, .craft_kind = 0, .cost = 3800,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.1f, .mass_add = 120,
      .vne_add = 30, .cd0_add = -0.005f, .structure_add = 30,
      .propellant_add = 3600.0f,
      .pitch_mul = 0.85f, .roll_mul = 0.90f, .inertia_mul = 1.25f,
      .stability_mul = 1.10f, .g_limit_add = 1.0f,
      .visual = FLY_VIS_PROPTANK, .min_ilvl = FLY_SPACE_ILVL },
    { .id = "rcs_pack", .name = "RCS Thrusters", .desc = "cold gas: attitude with no air",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = 0, .cost = 2600,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 45,
      .rcs_auth = 1.1f, .inertia_mul = 1.05f, .visual = FLY_VIS_RCS,
      .min_ilvl = FLY_SPACE_ILVL },
    { .id = "heat_shield", .name = "Ablative Shield", .desc = "survives a re-entry, if it is shallow",
      .slot = FLY_SLOT_HULL, .craft_kind = 0, .cost = 3400,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 260,
      .vne_add = 90, .cd0_add = -0.006f, .structure_add = 40,
      .heat_shield = 1.0f,
      .roll_mul = 0.85f, .pitch_mul = 0.88f, .inertia_mul = 1.25f,
      .stability_mul = 1.15f, .g_limit_add = 1.2f,
      .visual = FLY_VIS_SHIELD, .min_ilvl = FLY_SPACE_ILVL },
    { .id = "pressure_cabin", .name = "Pressure Cabin", .desc = "sealed hull: a pilot can live up there",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = 0, .cost = 2900,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 95,
      .vne_add = 20, .cd0_add = -0.002f, .structure_add = 20,
      .pressure = 1.0f, .inertia_mul = 1.05f, .stability_mul = 1.05f,
      .visual = FLY_VIS_CABIN, .min_ilvl = FLY_SPACE_ILVL },

    /* --- what the powers build for themselves ----------------------------
     *
     * Three apiece, and the set is chosen so that flying for somebody is a
     * different aeroplane rather than a better one. Each power gets one part
     * that sharpens what it already does, one that opens something it could
     * not do before, and — further down, with the rest of the armed kit — one
     * that says how it fights. Every one of them is worse than the open
     * registry at something: the Foundry's engine is enormously strong and
     * enormously heavy, the Corsair gun out-shoots everything inside three
     * hundred metres and is useless past it, the Choir's avionics see further
     * than any radar and cost you the pressure cabin's slot.
     *
     * None of them is reachable by rolling. `faction` keeps them out of
     * `fly_module_roll_base` entirely, so they are not rare drops — they are
     * things you are sold, over a counter, by people who have decided they
     * like you. That is the whole reward for an oath, and it has to be kit
     * rather than a discount, because a discount is a number and a wing is an
     * aeroplane.
     *
     * They are priced above the open equivalents on purpose. Standing buys the
     * right to be sold one; it does not buy it cheaply, and a power that armed
     * its friends at a loss would be a power with no friends left. */

    { .id = "concord_cannon", .name = "Writ Cannon", .desc = "22 dps, 1200 m: the law reaches",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1450,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 62,
      .vne_add = -5, .cd0_add = 0.003f, .gun_dps = 22, .gun_range = 1200,
      .roll_mul = 0.85f, .inertia_mul = 1.18f, .stability_mul = 1.08f,
      .visual = FLY_VIS_GUNPOD | FLY_VIS_SLATS, .faction = FLY_FACTION_CONCORD },
    { .id = "concord_writ", .name = "Registry Transponder", .desc = "14 km radar, storm rated 0.5",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 1180,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 18,
      .cd0_add = 0.001f, .weather_rating = 0.5f, .radar_range = 14000,
      .damping_mul = 1.08f, .stability_mul = 1.08f,
      .visual = FLY_VIS_RADARDOME | FLY_VIS_ANTENNA, .faction = FLY_FACTION_CONCORD },

    { .id = "corsair_ripper", .name = "Ripper Battery", .desc = "34 dps, 380 m: knife range only",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1260,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 34,
      .vne_add = -2, .cd0_add = 0.002f, .gun_dps = 34, .gun_range = 380,
      .roll_mul = 0.94f, .yaw_mul = 1.05f, .inertia_mul = 1.08f,
      .visual = FLY_VIS_GUNPOD, .faction = FLY_FACTION_CORSAIR },
    { .id = "corsair_ghost", .name = "Ghost Frame", .desc = "-120 kg, +8 vne, nothing left to hit",
      .slot = FLY_SLOT_HULL, .craft_kind = -1, .cost = 1320,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = -120,
      .vne_add = 8, .cd0_add = -0.003f, .structure_add = -28,
      .roll_mul = 1.30f, .pitch_mul = 1.18f, .yaw_mul = 1.10f,
      .inertia_mul = 0.78f, .damping_mul = 0.78f, .stability_mul = 0.80f,
      .g_limit_add = -1.2f, .faction = FLY_FACTION_CORSAIR },

    { .id = "foundry_kiln", .name = "Kiln Engine", .desc = "+62% thrust, and it weighs what it sounds",
      .slot = FLY_SLOT_ENGINE, .craft_kind = -1, .cost = 1680,
      .thrust_mul = 1.62f, .fuel_mul = 0.85f, .cargo_mul = 1, .mass_add = 190,
      .vne_add = -4, .spool_mul = 0.55f, .torque_mul = 1.9f, .inertia_mul = 1.12f,
      .visual = FLY_VIS_TURBO, .faction = FLY_FACTION_FOUNDRY },
    { .id = "foundry_slab", .name = "Slab Plating", .desc = "-55% incoming, +45 hp, and no turn left",
      .slot = FLY_SLOT_HULL, .craft_kind = -1, .cost = 1540,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 155,
      .vne_add = -9, .cd0_add = 0.004f, .structure_add = 45, .armor = 0.55f,
      .roll_mul = 0.78f, .pitch_mul = 0.86f, .inertia_mul = 1.32f,
      .damping_mul = 1.20f, .stability_mul = 1.12f, .g_limit_add = 0.9f,
      .visual = FLY_VIS_ARMOR, .faction = FLY_FACTION_FOUNDRY },

    { .id = "lantern_bloom", .name = "Bloom Wing", .desc = "-22% drag, +2600 m; and it will not roll",
      .slot = FLY_SLOT_WINGS, .craft_kind = 0, .cost = 1490,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 26,
      .ceiling_add = 2600, .vne_add = -12, .cd0_add = -0.009f,
      .wing_area_mul = 1.12f, .wing_span_mul = 1.35f, .cl_max_add = 0.20f,
      .alpha_stall_add = 2.0f * FLY_DEG2RAD, .roll_mul = 0.62f,
      .inertia_mul = 1.30f, .damping_mul = 1.25f, .stability_mul = 1.20f,
      .visual = FLY_VIS_LONGWING | FLY_VIS_SLATS, .faction = FLY_FACTION_LANTERN },
    { .id = "lantern_hold", .name = "Compact Hold", .desc = "+2.4x cargo, and it is not fast",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1360,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 2.4f, .mass_add = 60,
      .vne_add = -8, .cd0_add = 0.004f,
      .pitch_mul = 0.85f, .roll_mul = 0.85f, .inertia_mul = 1.30f,
      .damping_mul = 1.10f, .visual = FLY_VIS_CARGOPOD,
      .faction = FLY_FACTION_LANTERN },

    { .id = "halcyon_sprint", .name = "Sprint Turbine", .desc = "+34% thrust, +16 vne, thirsty",
      .slot = FLY_SLOT_ENGINE, .craft_kind = -1, .cost = 1420,
      .thrust_mul = 1.34f, .fuel_mul = 0.78f, .cargo_mul = 1, .mass_add = 44,
      .vne_add = 16, .cd0_add = -0.002f, .spool_mul = 1.55f, .torque_mul = 1.25f,
      .visual = FLY_VIS_TURBO, .faction = FLY_FACTION_HALCYON },
    { .id = "halcyon_mailbay", .name = "Mail Bay", .desc = "+80% cargo and +40% fuel at once",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1580,
      .thrust_mul = 1, .fuel_mul = 1.40f, .cargo_mul = 1.8f, .mass_add = 52,
      .vne_add = -4, .cd0_add = 0.002f,
      .pitch_mul = 0.92f, .roll_mul = 0.92f, .inertia_mul = 1.15f,
      .visual = FLY_VIS_BELLYTANK | FLY_VIS_CARGOPOD,
      .faction = FLY_FACTION_HALCYON },

    { .id = "cinder_shroud", .name = "Storm Shroud", .desc = "weather 1.0: the belt is a road",
      .slot = FLY_SLOT_HULL, .craft_kind = -1, .cost = 1620,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 88,
      .vne_add = -6, .cd0_add = 0.003f, .structure_add = 25,
      .weather_rating = 1.0f, .armor = 0.20f,
      .roll_mul = 0.90f, .inertia_mul = 1.12f, .damping_mul = 1.20f,
      .stability_mul = 1.15f, .visual = FLY_VIS_ARMOR,
      .faction = FLY_FACTION_CINDER },
    { .id = "cinder_grapple", .name = "Salvage Grapple", .desc = "VTOL, +60% cargo, ugly with it",
      .slot = FLY_SLOT_WINGS, .craft_kind = 0, .cost = 1380,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1.6f, .mass_add = 72,
      .vne_add = -18, .cd0_add = 0.006f, .vtol = 1,
      .wing_area_mul = 1.05f, .cl_max_add = 0.15f, .roll_mul = 0.80f,
      .inertia_mul = 1.20f, .damping_mul = 0.90f, .stability_mul = 0.95f,
      .visual = FLY_VIS_SLATS | FLY_VIS_CARGOPOD, .faction = FLY_FACTION_CINDER },

    { .id = "meridian_choir", .name = "Choir Array", .desc = "26 km radar; it hears the old world",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 1720,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 30,
      .cd0_add = 0.002f, .radar_range = 26000,
      .yaw_mul = 0.94f, .stability_mul = 0.92f, .visual = FLY_VIS_ANTENNA,
      .faction = FLY_FACTION_MERIDIAN },
    { .id = "meridian_reliquary", .name = "Reliquary Tank", .desc = "+95% fuel, sealed against the dark",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1240,
      .thrust_mul = 1, .fuel_mul = 1.95f, .cargo_mul = 1, .mass_add = 48,
      .vne_add = -6, .cd0_add = 0.003f,
      .roll_mul = 0.76f, .inertia_mul = 1.30f, .visual = FLY_VIS_DROPTANK,
      .faction = FLY_FACTION_MERIDIAN },

    /* --- and what each power arms itself with ------------------------------
     *
     * One apiece, taking the seven from two each to three, because the
     * symmetry is the point: a power that got a missile while its neighbour
     * did not would be a power that wins. Each one is the armed expression of
     * what that power already is — the law reaches furthest, the outlaws fight
     * at knife range, the foundry drops the heaviest thing anyone makes, the
     * lantern burns brightest, the mail runs away, the storm hides, and
     * whatever Meridian dug up does not need to lead its target.
     *
     * Like the rest of the faction kit these never drop and never appear on an
     * open shelf. The only route to one is a counter at one of their own
     * fields with your name in good standing on it, which is what makes an
     * oath a decision about how you are going to fight. */
    { .id = "concord_interdictor", .name = "Interdiction Rail",
      .desc = "two darts, 4.8 km: the writ reaches",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 2450,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 88,
      .vne_add = -6, .cd0_add = 0.003f,
      .ord_kind = FLY_ORD_DART, .ord_range = 4800, .ord_damage = 1.1f, .ord_ammo = 2,
      .roll_mul = 0.82f, .inertia_mul = 1.24f, .stability_mul = 1.06f,
      .visual = FLY_VIS_MISSILE, .faction = FLY_FACTION_CONCORD },
    { .id = "corsair_swarm", .name = "Swarm Rack",
      .desc = "36 rockets, 900 m, and no aiming to speak of",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 1340,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 58,
      .vne_add = -3, .cd0_add = 0.003f,
      .ord_kind = FLY_ORD_ROCKET, .ord_range = 900, .ord_damage = 0.86f, .ord_ammo = 36,
      .roll_mul = 0.90f, .yaw_mul = 1.04f, .inertia_mul = 1.14f,
      .visual = FLY_VIS_ROCKETPOD, .faction = FLY_FACTION_CORSAIR },
    { .id = "foundry_hammer", .name = "Hammer Rack",
      .desc = "four, and each one is worth six of anybody else's",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1720,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.4f, .mass_add = 175,
      .vne_add = -11, .cd0_add = 0.004f,
      .ord_kind = FLY_ORD_BOMB, .ord_range = 2600, .ord_damage = 1.75f, .ord_ammo = 4,
      .pitch_mul = 0.82f, .roll_mul = 0.80f, .inertia_mul = 1.34f,
      .damping_mul = 1.12f, .visual = FLY_VIS_BOMBRACK,
      .faction = FLY_FACTION_FOUNDRY },
    { .id = "lantern_beacon", .name = "Beacon Dispenser",
      .desc = "60 flares; they burn longer than anyone else's",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1280,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.8f, .mass_add = 40,
      .cd0_add = 0.001f, .cm_kind = FLY_ORD_FLARE, .cm_ammo = 60,
      .inertia_mul = 1.05f, .visual = FLY_VIS_DISPENSER,
      .faction = FLY_FACTION_LANTERN },
    { .id = "halcyon_ghostbox", .name = "Ghost Box",
      .desc = "the strongest jammer flying, and it hears nothing",
      .slot = FLY_SLOT_AVIONICS, .craft_kind = -1, .cost = 1460,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 20,
      .cd0_add = 0.001f, .ecm = 0.78f,
      .yaw_mul = 0.98f, .visual = FLY_VIS_ANTENNA,
      .faction = FLY_FACTION_HALCYON },
    { .id = "cinder_censer", .name = "Censer Pod",
      .desc = "36 clouds of foil, thrown out into the ash",
      .slot = FLY_SLOT_BAY, .craft_kind = -1, .cost = 1180,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 0.86f, .mass_add = 36,
      .cd0_add = 0.002f, .cm_kind = FLY_ORD_CHAFF, .cm_ammo = 36, .ecm = 0.2f,
      .inertia_mul = 1.04f, .damping_mul = 1.06f, .visual = FLY_VIS_DISPENSER,
      .faction = FLY_FACTION_CINDER },
    { .id = "meridian_lumen", .name = "Lumen Emitter",
      .desc = "38 dps at 820 m, and it arrives when you press it",
      .slot = FLY_SLOT_HARDPOINT, .craft_kind = -1, .cost = 2680,
      .thrust_mul = 1, .fuel_mul = 1, .cargo_mul = 1, .mass_add = 104,
      .vne_add = -4, .cd0_add = 0.002f,
      .gun_dps = 38, .gun_range = 820, .beam_heat = 0.26f,
      .roll_mul = 0.86f, .inertia_mul = 1.22f, .visual = FLY_VIS_EMITTER,
      .faction = FLY_FACTION_MERIDIAN },
};
static int fly__nmodules = 47;

static fly_airframe_catalog fly__airframes[FLY_AIRFRAME_CATALOG_MAX];
static int fly__nairframes;

int fly_module_count(void) { return fly__nmodules; }

const fly_module *fly_module_get(int idx) {
    return idx >= 0 && idx < fly__nmodules ? &fly__modules[idx] : NULL;
}

int fly_module_find(const char *id) {
    int i;
    for (i = 0; i < fly__nmodules; ++i)
        if (strcmp(fly__modules[i].id, id) == 0) return i;
    return -1;
}

/* Which base turns up here. Uniform over the bases this ground or shelf is
 * allowed to carry, which for most of the map is the whole aeroplane registry
 * and nothing else.
 *
 * One draw, always, whichever way the filter falls: `fly_rng_range` consumes a
 * single word regardless of its bound, so a world generated before the rocket
 * stack had a floor and one generated after it differ in what they put on the
 * shelf and not in what every later system draws. That is the difference
 * between a rebalance and a reseed. */
int fly_module_roll_base(int ilvl, fly_rng *rng) {
    int eligible[FLY_MODULE_MAX];
    int i, n = 0;
    if (!rng || fly__nmodules <= 0) return 0;
    for (i = 0; i < fly__nmodules; ++i)
        if (fly__modules[i].min_ilvl <= ilvl &&
            fly__modules[i].faction == FLY_FACTION_FREE) eligible[n++] = i;
    /* A registry whose every entry is out of reach here is a data error, not a
     * reason to hand back nothing: fall back to the whole table rather than
     * leaving the caller with an item it cannot name. */
    if (n == 0) return (int)fly_rng_range(rng, (uint32_t)fly__nmodules);
    return eligible[fly_rng_range(rng, (uint32_t)n)];
}

int fly_module_faction_count(int faction) {
    int i, n = 0;
    for (i = 0; i < fly__nmodules; ++i)
        if (fly__modules[i].faction == faction) ++n;
    return n;
}

/* One power's own catalogue. Same shape as the open draw and the same
 * one-word-or-nothing discipline: a caller that falls back consumes exactly
 * what it would have consumed if the power had built something. */
int fly_module_roll_faction(int ilvl, int faction, fly_rng *rng) {
    int eligible[FLY_MODULE_MAX];
    int i, n = 0;
    if (!rng || !fly_faction_is_power(faction)) return -1;
    for (i = 0; i < fly__nmodules; ++i)
        if (fly__modules[i].faction == faction && fly__modules[i].min_ilvl <= ilvl)
            eligible[n++] = i;
    if (n == 0) return -1;
    return eligible[fly_rng_range(rng, (uint32_t)n)];
}

const char *fly_slot_name(int slot) {
    static const char *names[FLY_SLOT_COUNT] = {
        "engine", "wings", "hardpoint", "bay", "avionics", "hull"
    };
    return slot >= 0 && slot < FLY_SLOT_COUNT ? names[slot] : "?";
}

/* The same six, as four-letter tags. A column of tags all one width sorts
 * itself by eye; "engine/wings/hardpoint/bay/avionics/hull" ran from three
 * letters to nine and turned a list into a ragged margin. */
const char *fly_slot_tag(int slot) {
    static const char *tags[FLY_SLOT_COUNT] = {
        "ENGN", "WING", "HARD", "BAY ", "AVIO", "HULL"
    };
    return slot >= 0 && slot < FLY_SLOT_COUNT ? tags[slot] : "????";
}

static int fly__slot_from_name(const char *s) {
    int i;
    for (i = 0; i < FLY_SLOT_COUNT; ++i)
        if (strcmp(fly_slot_name(i), s) == 0) return i;
    return -1;
}

/* Copy an optional YAML string into a fixed buffer, leaving the buffer's
 * current contents alone when the key is absent.
 *
 * The obvious spelling — passing the destination as fly_yaml_str's default and
 * snprintf'ing the result back — makes snprintf read the very buffer it writes
 * whenever the key is missing. Overlapping source and destination is undefined
 * (C11 7.21.6.5), and GCC flags it as -Wrestrict once it can see through to the
 * registry, so the aliasing case is skipped explicitly instead. */
static void fly__yaml_str_into(char *dst, size_t cap, const fly_yaml_node *n,
                               const char *path) {
    const char *v = fly_yaml_str(n, path, NULL);
    if (v && v != dst && cap > 0) snprintf(dst, cap, "%.*s", (int)(cap - 1), v);
}

int fly_modules_load_yaml(const char *path) {
    fly_yaml_node *root = fly_yaml_load_file(path);
    if (!root) return -1;
    const fly_yaml_node *seq = fly_yaml_get(root, "modules");
    int n = fly_yaml_count(seq), i, loaded = 0;
    for (i = 0; i < n; ++i) {
        const fly_yaml_node *m = fly_yaml_at(seq, i);
        const char *id = fly_yaml_str(m, "id", NULL);
        int slot = fly__slot_from_name(fly_yaml_str(m, "slot", ""));
        if (!id || slot < 0) continue;
        int idx = fly_module_find(id);
        if (idx < 0) {
            if (fly__nmodules >= FLY_MODULE_MAX) continue;
            idx = fly__nmodules++;
            memset(&fly__modules[idx], 0, sizeof(fly_module));
            fly__modules[idx].thrust_mul = fly__modules[idx].fuel_mul = fly__modules[idx].cargo_mul = 1;
            fly__modules[idx].craft_kind = -1;
        }
        fly_module *d = &fly__modules[idx];
        snprintf(d->id, sizeof d->id, "%s", id);
        fly__yaml_str_into(d->name, sizeof d->name, m, "name");
        fly__yaml_str_into(d->desc, sizeof d->desc, m, "desc");
        d->slot = slot;
        d->craft_kind = fly_yaml_int(m, "kind", d->craft_kind);
        d->cost = (long)fly_yaml_num(m, "cost", (double)d->cost);
        d->thrust_mul = (float)fly_yaml_num(m, "thrust_mul", d->thrust_mul);
        d->fuel_mul = (float)fly_yaml_num(m, "fuel_mul", d->fuel_mul);
        d->cargo_mul = (float)fly_yaml_num(m, "cargo_mul", d->cargo_mul);
        d->mass_add = (float)fly_yaml_num(m, "mass_add", d->mass_add);
        d->ceiling_add = (float)fly_yaml_num(m, "ceiling_add", d->ceiling_add);
        d->vne_add = (float)fly_yaml_num(m, "vne_add", d->vne_add);
        d->cd0_add = (float)fly_yaml_num(m, "cd0_add", d->cd0_add);
        d->structure_add = (float)fly_yaml_num(m, "structure_add", d->structure_add);
        d->weather_rating = (float)fly_yaml_num(m, "weather_rating", d->weather_rating);
        d->radar_range = (float)fly_yaml_num(m, "radar_range", d->radar_range);
        d->gun_dps = (float)fly_yaml_num(m, "gun_dps", d->gun_dps);
        d->gun_range = (float)fly_yaml_num(m, "gun_range", d->gun_range);
        d->beam_heat = (float)fly_yaml_num(m, "beam_heat", d->beam_heat);
        d->armor = (float)fly_yaml_num(m, "armor", d->armor);
        {   /* By name rather than by number, for the reason the faction tag is:
             * "ord: seeker" survives a reordering of `fly_ord_kind`, and a typo
             * lands on "no launcher" rather than on whichever weapon happens to
             * sit at that index. */
            const char *ok = fly_yaml_str(m, "ord", NULL);
            const char *ck = fly_yaml_str(m, "cm", NULL);
            int ki;
            if (ok) {
                d->ord_kind = 0;
                for (ki = 1; ki < FLY_ORD_KIND_COUNT; ++ki)
                    if (strcmp(fly_ord_kind_name(ki), ok) == 0) { d->ord_kind = ki; break; }
            }
            if (ck) {
                d->cm_kind = 0;
                for (ki = 1; ki < FLY_ORD_KIND_COUNT; ++ki)
                    if (strcmp(fly_ord_kind_name(ki), ck) == 0) { d->cm_kind = ki; break; }
            }
        }
        d->ord_range = (float)fly_yaml_num(m, "ord_range", d->ord_range);
        d->ord_damage = (float)fly_yaml_num(m, "ord_damage", d->ord_damage);
        d->ord_ammo = fly_yaml_int(m, "ord_ammo", d->ord_ammo);
        d->cm_ammo = fly_yaml_int(m, "cm_ammo", d->cm_ammo);
        d->ecm = (float)fly_yaml_num(m, "ecm", d->ecm);
        d->rocket_thrust = (float)fly_yaml_num(m, "rocket_thrust", d->rocket_thrust);
        d->rocket_ve = (float)fly_yaml_num(m, "rocket_ve", d->rocket_ve);
        d->propellant_add = (float)fly_yaml_num(m, "propellant", d->propellant_add);
        d->rcs_auth = (float)fly_yaml_num(m, "rcs_auth", d->rcs_auth);
        d->heat_shield = (float)fly_yaml_num(m, "heat_shield", d->heat_shield);
        d->pressure = (float)fly_yaml_num(m, "pressure", d->pressure);
        d->vtol = fly_yaml_bool(m, "vtol", d->vtol);
        /* the handling block: unset stays unset, and fly__mul reads that as 1 */
        d->pitch_mul = (float)fly_yaml_num(m, "pitch_mul", d->pitch_mul);
        d->roll_mul = (float)fly_yaml_num(m, "roll_mul", d->roll_mul);
        d->yaw_mul = (float)fly_yaml_num(m, "yaw_mul", d->yaw_mul);
        d->wing_area_mul = (float)fly_yaml_num(m, "wing_area_mul", d->wing_area_mul);
        d->wing_span_mul = (float)fly_yaml_num(m, "wing_span_mul", d->wing_span_mul);
        d->cl_max_add = (float)fly_yaml_num(m, "cl_max_add", d->cl_max_add);
        /* degrees in the file, radians in the struct: the file is written by
         * somebody rebalancing an aeroplane, not by the flight model */
        d->alpha_stall_add = (float)fly_yaml_num(m, "alpha_stall_add_deg",
                                                 d->alpha_stall_add * FLY_RAD2DEG) * FLY_DEG2RAD;
        d->stability_mul = (float)fly_yaml_num(m, "stability_mul", d->stability_mul);
        d->damping_mul = (float)fly_yaml_num(m, "damping_mul", d->damping_mul);
        d->inertia_mul = (float)fly_yaml_num(m, "inertia_mul", d->inertia_mul);
        d->spool_mul = (float)fly_yaml_num(m, "spool_mul", d->spool_mul);
        d->torque_mul = (float)fly_yaml_num(m, "torque_mul", d->torque_mul);
        d->g_limit_add = (float)fly_yaml_num(m, "g_limit_add", d->g_limit_add);
        d->visual = (uint32_t)fly_yaml_num(m, "visual", (double)d->visual);
        d->min_ilvl = fly_yaml_int(m, "min_ilvl", d->min_ilvl);
        {   /* By tag rather than by number: "faction: FNDY" survives a
             * reordering of the enum, and a typo lands on unaligned rather
             * than on whichever power happens to sit at that index. */
            const char *ftag = fly_yaml_str(m, "faction", NULL);
            if (ftag) {
                int fi;
                d->faction = FLY_FACTION_FREE;
                for (fi = 1; fi < FLY_FACTION_COUNT; ++fi)
                    if (strcmp(fly_faction_tag(fi), ftag) == 0) { d->faction = fi; break; }
            }
        }
        ++loaded;
    }
    fly_yaml_free(root);
    return loaded;
}

/* ---------------- items ----------------
 *
 * An item is an instance of a base module. Everything below is a pure function
 * of the instance: its name, what its rolls read as, and what it is worth. None
 * of it is stored, so an item is 16 bytes and a save file carries the rolls
 * rather than the prose.
 *
 * `mag` is one fixed-point integer per roll and every affix reads it through
 * this table, so a roll cannot be stored in one unit and applied in another —
 * which is the only interesting way a system like this goes wrong. `scale`
 * turns a stored magnitude into the sim's own unit for that field. */
static const struct {
    const char *label;   /* what the field is, for a list line */
    const char *unit;
    float scale;         /* mag * scale = sim units */
    short ref;           /* the magnitude of a strong roll of this affix */
    int percent;         /* the roll reads as a percentage of the base */
    int lower_better;    /* mass and drag: a negative roll is the good one */
    const char *suffix[3];
} fly__affix_tab[FLY_AFFIX_COUNT] = {
    { "",          "",     0.0f,      1,    0, 0, { "", "", "" } },
    { "thrust",    "%",    0.001f,    100,  1, 0, { "of the Gale", "of the Surge", "of the Bellows" } },
    { "mass",      " kg",  0.01f,     2500, 0, 1, { "of the Feather", "of the Wisp", "of Thin Air" } },
    { "drag",      "",     1e-6f,     2000, 0, 1, { "of the Slipstream", "of the Glide", "of Still Water" } },
    { "Vne",       " m/s", 0.01f,     800,  0, 0, { "of the Dive", "of the Bolt", "of the Cataract" } },
    { "fuel",      "%",    0.001f,    120,  1, 0, { "of the Long Haul", "of the Reserve", "of Patience" } },
    { "cargo",     "%",    0.001f,    150,  1, 0, { "of the Hold", "of the Freighter", "of Deep Pockets" } },
    { "ceiling",   " m",   1.0f,      1200, 0, 0, { "of the Summit", "of Thin Sky", "of the Cirrus" } },
    { "structure", " hp",  0.1f,      250,  0, 0, { "of the Anvil", "of the Ribcage", "of Old Iron" } },
    { "armour",    "%",    0.001f,    80,   1, 0, { "of the Bulwark", "of the Shell", "of the Turtle" } },
    { "gun",       " dps", 0.01f,     500,  0, 0, { "of the Hornet", "of the Rasp", "of the Wasp Nest" } },
    { "gun range", " m",   1.0f,      250,  0, 0, { "of the Reach", "of the Long Shot", "of the Horizon" } },
    { "radar",     " m",   1.0f,      2000, 0, 0, { "of the Watch", "of the Open Eye", "of the Lighthouse" } },
    { "weather",   "%",    0.001f,    100,  1, 0, { "of the Squall", "of the Front", "of Foul Weather" } },
    /* The spaceframe three. Sized against the stack they roll on rather than
     * against each other: 9% of 110 kN is 10 kN, 260 kg is 7% of a full tank,
     * and 0.22 rad/s^2 is a fifth of the cold-gas authority. All three land in
     * the same envelope the aeroplane affixes do — a better ship, not a
     * different one — which is what keeps `sim.ascent` a claim about physics
     * rather than about the drop table. */
    { "rocket",     "%",       0.001f, 90,   1, 0, { "of the Candle", "of the Pillar", "of Escape" } },
    { "propellant", " kg",     0.1f,   2600, 0, 0, { "of the Long Burn", "of the Deep Tank", "of the Margin" } },
    { "RCS",        " rad/s2", 0.001f, 220,  0, 0, { "of Cold Gas", "of the Steady Hand", "of the Pirouette" } },
    /* The two you feel. Sized to be worth a turn rather than worth a
     * different aeroplane: a strong roll is a tenth of the roll rate or a
     * degree and a half of extra alpha before the break, which is about a
     * knot and a half off the stall speed. Small numbers, and the only two
     * rolls in the game that a pilot notices without reading anything.
     *
     * The stall roll is the one entry in this table whose unit is not the
     * sim's: it is stored and printed in degrees, because "+1.4 deg stall" is
     * a sentence and "+0.024 rad" is a diagnostic. fly_game_item_effect does
     * the one conversion, and it is the only place allowed to. */
    { "roll rate",  "%",       0.001f, 100,  1, 0, { "of the Snap", "of the Flick", "of Quick Hands" } },
    { "stall",      " deg",    0.01f,  150,  0, 0, { "of the Slow Wing", "of the Hover", "of Patience" } },
    /* And the launcher's three. Sized against what they roll on, the way the
     * spaceframe's are: a tenth on the warhead, six hundred metres of envelope,
     * and a magazine that reads as a percentage because a roll worth two rounds
     * on a seeker rack has to be worth twelve on a rocket pod or the same
     * number would be decisive on one and beneath notice on the other. */
    { "warhead",    "%",       0.001f, 100,  1, 0, { "of the Hammer", "of the Splinter", "of the Thunderhead" } },
    { "envelope",   " m",      1.0f,   600,  0, 0, { "of the Far Shot", "of the Long Arm", "of the Vanishing Point" } },
    { "magazine",   "%",       0.001f, 260,  1, 0, { "of the Deep Rack", "of the Full Belt", "of the Armoury" } }
};

/* "Uprated" rather than "Tuned" because the registry already ships a module
 * called Tuned Engine, and a "Tuned Tuned Engine" is a naming scheme telling
 * you it was not thought through. */
static const char *fly__rarity_names[FLY_ITEM_RARITY_COUNT] = {
    "Stock", "Uprated", "Marked", "Prototype", "One-off"
};

/* What each tier is worth as a multiple of the base module's price. It rises
 * faster than the stat total does, which is the point: the last tier is worth
 * hunting for rather than worth buying. */
static const float fly__rarity_value[FLY_ITEM_RARITY_COUNT] = {
    1.00f, 1.20f, 1.60f, 2.30f, 3.40f
};

const char *fly_item_rarity_name(int rarity) {
    return rarity >= 0 && rarity < FLY_ITEM_RARITY_COUNT ? fly__rarity_names[rarity] : "?";
}

const char *fly_affix_name(int kind) {
    return kind > 0 && kind < FLY_AFFIX_COUNT ? fly__affix_tab[kind].label : "";
}

int fly_item_valid(const fly_item *it) {
    return it && it->uid != 0 && fly_module_get(it->base) != NULL;
}

/* The value of one roll in the sim's units for that field. */
float fly_affix_value(const fly_affix_roll *a) {
    if (!a || a->kind <= 0 || a->kind >= FLY_AFFIX_COUNT) return 0.0f;
    return (float)a->mag * fly__affix_tab[a->kind].scale;
}

int fly_affix_text(const fly_affix_roll *a, char *buf, int n) {
    float v;
    int sign;
    if (!buf || n <= 0) return 0;
    buf[0] = 0;
    if (!a || a->kind <= 0 || a->kind >= FLY_AFFIX_COUNT) return 0;
    v = fly_affix_value(a);
    sign = v > 0.0f ? 1 : v < 0.0f ? -1 : 0;
    /* How many decimals a field needs is a property of how big a strong roll of
     * it is, not of its scale factor: drag's is 0.002 and needs four, the cold
     * gas thrusters' is 0.22 and needs two, and everything an aeroplane is
     * measured in — kilograms, metres, horsepower — is a whole number. Keyed
     * off `ref * scale` so a rebalanced affix cannot start printing "+0". */
    if (fly__affix_tab[a->kind].percent)
        snprintf(buf, (size_t)n, "%+.0f%% %s", (double)(v * 100.0f), fly__affix_tab[a->kind].label);
    else {
        float big = (float)fly__affix_tab[a->kind].ref * fly__affix_tab[a->kind].scale;
        const char *fmt = big < 0.01f ? "%+.4f%s %s" : big < 1.0f ? "%+.2f%s %s" : "%+.0f%s %s";
        snprintf(buf, (size_t)n, fmt, (double)v, fly__affix_tab[a->kind].unit,
                 fly__affix_tab[a->kind].label);
    }
    /* the good direction, not the arithmetic one: less mass is a better roll */
    return fly__affix_tab[a->kind].lower_better ? -sign : sign;
}

/* The affix that most defines the item: the biggest roll, measured against
 * what a big roll of *that* affix is. Comparing the stored magnitudes
 * directly would be comparing metres of ceiling with per cent of thrust, and
 * ceiling would win every name in the game. */
static int fly__item_signature(const fly_item *it) {
    int i, best = 0;
    float bestw = 0.0f;
    for (i = 0; i < it->naff && i < FLY_ITEM_AFFIX_MAX; ++i) {
        float w;
        if (it->aff[i].kind <= 0 || it->aff[i].kind >= FLY_AFFIX_COUNT) continue;
        w = (float)(it->aff[i].mag < 0 ? -it->aff[i].mag : it->aff[i].mag) /
            (float)fly__affix_tab[it->aff[i].kind].ref;
        if (w > bestw) { bestw = w; best = i; }
    }
    return bestw > 0.0f ? best : -1;
}

void fly_item_name(const fly_item *it, char *buf, int n) {
    const fly_module *m;
    int sig;
    if (!buf || n <= 0) return;
    buf[0] = 0;
    m = it ? fly_module_get(it->base) : NULL;
    if (!m) { snprintf(buf, (size_t)n, "(nothing)"); return; }
    if (it->rarity <= FLY_ITEM_STOCK) { snprintf(buf, (size_t)n, "%s", m->name); return; }
    sig = fly__item_signature(it);
    /* Two affixes or more earn a suffix, so a name is a claim about the item:
     * "Marked Gun Pod of the Hornet" has rolled its gun, and one that has not
     * does not get to say so. */
    if (sig >= 0 && it->naff >= 2)
        snprintf(buf, (size_t)n, "%s %s %s", fly_item_rarity_name(it->rarity), m->name,
                 fly__affix_tab[it->aff[sig].kind].suffix[it->seed % 3u]);
    else
        snprintf(buf, (size_t)n, "%s %s", fly_item_rarity_name(it->rarity), m->name);
}

void fly_item_rolls(const fly_item *it, char *buf, int n) {
    int i, at = 0;
    if (!buf || n <= 0) return;
    buf[0] = 0;
    if (!it) return;
    for (i = 0; i < it->naff && i < FLY_ITEM_AFFIX_MAX; ++i) {
        char one[48];
        int len;
        if (!fly_affix_text(&it->aff[i], one, sizeof one) && !one[0]) continue;
        len = (int)strlen(one);
        if (at && at + 2 < n) { buf[at++] = ','; buf[at++] = ' '; buf[at] = 0; }
        if (at + len >= n) break;
        memcpy(buf + at, one, (size_t)len);
        at += len;
        buf[at] = 0;
    }
}

long fly_item_value(const fly_item *it) {
    const fly_module *m = it ? fly_module_get(it->base) : NULL;
    float v, q = 0.0f;
    int i;
    if (!m) return 0;
    v = (float)m->cost * fly__rarity_value[it->rarity < FLY_ITEM_RARITY_COUNT ? it->rarity : 0];
    /* where it came from is part of what it is worth */
    v *= 1.0f + 0.15f * (float)it->ilvl / 99.0f;
    /* and so is how well it rolled: two Marked engines are not one price, or
     * the shop is selling a tier rather than an item */
    for (i = 0; i < it->naff && i < FLY_ITEM_AFFIX_MAX; ++i) {
        int k = it->aff[i].kind;
        if (k <= 0 || k >= FLY_AFFIX_COUNT) continue;
        q += (float)(it->aff[i].mag < 0 ? -it->aff[i].mag : it->aff[i].mag) /
             (float)fly__affix_tab[k].ref;
    }
    if (it->naff) v *= 1.0f + 0.55f * (q / (float)it->naff);
    return (long)v + 1;
}


/* --- rolling ---
 *
 * What separates a shop from a drop. A rolled item is a base with one to four
 * affixes on it, drawn from the fields that base already engages with, at
 * magnitudes scaled by how hard the thing that produced it was.
 *
 * Two rules bound the whole system and both are here rather than spread over
 * the callers:
 *
 *  - an affix may sharpen what a module already does and may not hand it a new
 *    job, so the gun, radar and weather rolls are only in the pool for bases
 *    that already have a gun, a radar or a rating. That is what keeps a build
 *    a set of decisions instead of a pile of everything;
 *  - a roll never exceeds `ref` for its affix, and `ref` is set at roughly what
 *    the base modules themselves are worth. The best possible engine is a
 *    little better than the best module in the registry, not an order of
 *    magnitude better, because the flight model and every pilot in the world
 *    are tuned against that registry and both of them fly this gear too.
 */

/* Rarity from item level, as weights out of a thousand.
 *
 * `bias` is the source's own quality and it runs both ways. Positive multiplies
 * the top of the ladder up and is what a rare, high-rank pilot is for; negative
 * divides it down and is what a shop is for. That symmetry is the whole of the
 * scarcity design: the same three tiers that a shelf almost never has are the
 * three an ace almost always does, so the way to a Prototype is a fight in bad
 * country rather than a walk to a counter with enough money.
 *
 * The lever is per tier, so a bias moves the far end of the ladder much harder
 * than the near end — a shop biased down still sells plenty of Uprated kit and
 * simply stops being where One-offs come from. */
static int fly__roll_rarity(int ilvl, float bias, fly_rng *rng) {
    float w[FLY_ITEM_RARITY_COUNT];
    float t = fly_clampf((float)ilvl / 100.0f, 0.0f, 1.0f), sum = 0.0f, pick;
    float up = bias > 0.0f ? bias : 0.0f, down = bias < 0.0f ? -bias : 0.0f;
    static const float lever[FLY_ITEM_RARITY_COUNT] = { 0.0f, 0.0f, 1.0f, 1.9f, 3.2f };
    int i;
    w[FLY_ITEM_STOCK]     = 1000.0f - 600.0f * t;
    w[FLY_ITEM_TUNED]     = 400.0f;
    w[FLY_ITEM_MARKED]    = 100.0f + 150.0f * t;
    w[FLY_ITEM_PROTOTYPE] = 14.0f + 70.0f * t;
    w[FLY_ITEM_ONEOFF]    = 1.0f + 16.0f * t;
    for (i = 0; i < FLY_ITEM_RARITY_COUNT; ++i)
        w[i] *= (1.0f + up * lever[i]) / (1.0f + down * lever[i]);
    for (i = 0; i < FLY_ITEM_RARITY_COUNT; ++i) sum += w[i];
    pick = fly_rng_span(rng, 0.0f, sum);
    for (i = 0; i < FLY_ITEM_RARITY_COUNT; ++i) {
        pick -= w[i];
        if (pick <= 0.0f) return i;
    }
    return FLY_ITEM_STOCK;
}

/* Which affixes this base may roll. The first block is every module's
 * business — anything bolted to an aeroplane has mass, drag and a share of its
 * thrust — and the rest are only in the pool if the base already does that
 * job. */
static int fly__affix_pool(const fly_module *b, unsigned char *out) {
    int n = 0;
    /* Anything bolted to an aeroplane weighs something, drags something, is
     * mounted to something and lives inside the same never-exceed. */
    out[n++] = FLY_AFFIX_MASS;
    out[n++] = FLY_AFFIX_DRAG;
    out[n++] = FLY_AFFIX_VNE;
    out[n++] = FLY_AFFIX_STRUCTURE;
    /* Thrust comes out of engines. A radar that rolled +4% thrust is a radar
     * making the engine stronger, which is one of those small nonsenses that
     * quietly tells a player the numbers are arbitrary. */
    if (b->thrust_mul != 1.0f) out[n++] = FLY_AFFIX_THRUST;
    /* A bay or a hardpoint carries things; a wing does not become a hold. */
    if (b->fuel_mul != 1.0f) out[n++] = FLY_AFFIX_FUEL;
    if (b->cargo_mul != 1.0f) out[n++] = FLY_AFFIX_CARGO;
    if (b->ceiling_add != 0.0f) out[n++] = FLY_AFFIX_CEILING;
    if (b->armor > 0.0f) out[n++] = FLY_AFFIX_ARMOR;
    if (b->gun_dps > 0.0f) { out[n++] = FLY_AFFIX_GUN_DPS; out[n++] = FLY_AFFIX_GUN_RANGE; }
    if (b->radar_range > 0.0f) out[n++] = FLY_AFFIX_RADAR;
    if (b->weather_rating > 0.0f) out[n++] = FLY_AFFIX_WEATHER;
    /* The same rule one slot further up. A motor may roll a better motor, a
     * tank may hold more, and thrusters may push harder — and none of the
     * three is in the pool for a wing, because a wing has no candle to light. */
    if (b->rocket_thrust > 0.0f) out[n++] = FLY_AFFIX_ROCKET;
    if (b->propellant_add > 0.0f) out[n++] = FLY_AFFIX_PROPELLANT;
    if (b->rcs_auth > 0.0f) out[n++] = FLY_AFFIX_RCS;
    /* And the same rule for the two handling rolls, which is what stops every
     * radar in the world from rolling a better roll rate: a part may only be
     * a better example of the thing it already is. A module that does not
     * touch the ailerons has no roll rate to sharpen, and a module that is
     * not part of the wing has no say in where the wing lets go. */
    if (fly__mul(b->roll_mul) != 1.0f) out[n++] = FLY_AFFIX_ROLL;
    if (b->cl_max_add != 0.0f || b->alpha_stall_add != 0.0f) out[n++] = FLY_AFFIX_STALL;
    /* And the launcher's, under the same rule the gun's live under: a part may
     * only be a better example of the thing it already is. A rack rolls a
     * heavier warhead, a longer envelope and a deeper magazine; a dispenser
     * rolls the magazine and nothing else, because a flare has no warhead and
     * no range worth stating — you drop it where you are. */
    if (b->ord_kind) {
        out[n++] = FLY_AFFIX_ORD_DAMAGE;
        out[n++] = FLY_AFFIX_ORD_RANGE;
        out[n++] = FLY_AFFIX_MAGAZINE;
    } else if (b->cm_kind) {
        out[n++] = FLY_AFFIX_MAGAZINE;
    }
    return n;
}

/* How many affixes each tier carries. This is the whole of what rarity means
 * mechanically; everything else about a tier — its price, its name, its
 * paint — follows from it. */
static const int fly__rarity_affixes[FLY_ITEM_RARITY_COUNT] = { 0, 1, 2, 3, 4 };

fly_item fly_game_roll_item(int base, int ilvl, float bias, fly_rng *rng) {
    const fly_module *b = fly_module_get(base);
    unsigned char pool[FLY_AFFIX_COUNT];
    fly_item it;
    int npool, want, i;
    float lvl;
    memset(&it, 0, sizeof it);
    if (!b || !rng) return it;
    if (ilvl < 1) ilvl = 1;
    if (ilvl > 99) ilvl = 99;
    it.base = (short)base;
    it.ilvl = (unsigned char)ilvl;
    it.rarity = (unsigned char)fly__roll_rarity(ilvl, bias, rng);
    it.seed = (unsigned char)fly_rng_range(rng, 256);
    npool = fly__affix_pool(b, pool);
    want = fly__rarity_affixes[it.rarity];
    if (want > npool) want = npool;
    /* Level buys magnitude as well as tier, but never the whole of it: a
     * level-1 Tuned engine is a real upgrade and a level-99 one is not four
     * times the aeroplane. */
    lvl = 0.40f + 0.60f * (float)ilvl / 99.0f;
    for (i = 0; i < want; ++i) {
        int pick = (int)fly_rng_range(rng, (uint32_t)(npool - i));
        int kind = pool[pick];
        float q = fly_rng_f01(rng);
        float mag;
        pool[pick] = pool[npool - i - 1];   /* draw without replacement: one
                                               affix of a kind per item */
        mag = (float)fly__affix_tab[kind].ref * (0.25f + 0.75f * q) * lvl;
        if (fly__affix_tab[kind].lower_better) mag = -mag;
        it.aff[it.naff].kind = (unsigned char)kind;
        it.aff[it.naff].mag = (short)(mag < 0.0f ? mag - 0.5f : mag + 0.5f);
        if (it.aff[it.naff].mag) ++it.naff;
    }
    return it;
}

/* --- what comes off a wreck ---
 *
 * The drop table. `bias` is the source's own quality — a named outlaw is worth
 * more than a courier — and the item level comes from the ground it fell on,
 * so where a fight happened decides what the fight was worth.
 *
 * Bounded on purpose. Fourteen aircraft an hour are lost across the map and
 * the ground-item array is sixty-four long, so a world that dropped for every
 * one of them would be a world where the pieces of the last hour have pushed
 * out the pieces of this one. `fly_game_launch_ground_item` refuses when full
 * and the caller decides who is worth the slot; see pilots_step. */
int fly_game_scatter_drop(fly_game *g, fly_v3 at, fly_v3 vel, int rolls, float bias,
                          uint32_t source_id) {
    int i, made = 0;
    int ilvl = fly_world_item_level(&g->world, at.x, at.y);
    for (i = 0; i < rolls; ++i) {
        int base = fly_module_roll_base(ilvl, &g->rng);
        float ang = fly_rng_span(&g->rng, 0.0f, 2.0f * FLY_PI);
        float spd = fly_rng_span(&g->rng, 9.0f, 23.0f);
        float rise = fly_rng_span(&g->rng, 7.0f, 16.0f);
        fly_item it = fly_game_roll_item(base, ilvl, bias, &g->rng);
        fly_v3 out = fly_v3mk(cosf(ang) * spd + vel.x * 0.25f,
                              sinf(ang) * spd + vel.y * 0.25f, rise);
        it.uid = source_id * 8u + (uint32_t)i + 1u;   /* only for the ground */
        if (fly_game_launch_ground_item(g, &it, at, out,
                                        FLY_GROUND_SOURCE_CRASH, source_id) >= 0) ++made;
    }
    return made;
}

fly_module fly_game_item_effect(const fly_item *it) {
    const fly_module *b = it ? fly_module_get(it->base) : NULL;
    fly_module m;
    int i;
    if (!b) {
        memset(&m, 0, sizeof m);
        m.slot = -1;
        m.craft_kind = -1;
        m.thrust_mul = m.fuel_mul = m.cargo_mul = 1.0f;
        return m;
    }
    m = *b;
    for (i = 0; i < it->naff && i < FLY_ITEM_AFFIX_MAX; ++i) {
        float v = fly_affix_value(&it->aff[i]);
        switch (it->aff[i].kind) {
        case FLY_AFFIX_THRUST:    m.thrust_mul *= 1.0f + v; break;
        case FLY_AFFIX_MASS:      m.mass_add += v; break;
        case FLY_AFFIX_DRAG:      m.cd0_add += v; break;
        case FLY_AFFIX_VNE:       m.vne_add += v; break;
        case FLY_AFFIX_FUEL:      m.fuel_mul *= 1.0f + v; break;
        case FLY_AFFIX_CARGO:     m.cargo_mul *= 1.0f + v; break;
        case FLY_AFFIX_CEILING:   m.ceiling_add += v; break;
        case FLY_AFFIX_STRUCTURE: m.structure_add += v; break;
        case FLY_AFFIX_ARMOR:     m.armor += v; break;
        /* A roll may sharpen what a module already does and may not hand it a
         * new job: a gun roll on a fuel tank would be a gun, and the tank has
         * no barrel to put it in. The roller draws from the same rule, so this
         * only ever catches a hand-edited save. */
        case FLY_AFFIX_GUN_DPS:   if (b->gun_dps > 0.0f) m.gun_dps += v; break;
        case FLY_AFFIX_GUN_RANGE: if (b->gun_range > 0.0f) m.gun_range += v; break;
        case FLY_AFFIX_RADAR:     if (b->radar_range > 0.0f) m.radar_range += v; break;
        case FLY_AFFIX_WEATHER:   if (b->weather_rating > 0.0f) m.weather_rating += v; break;
        case FLY_AFFIX_ROCKET:    if (b->rocket_thrust > 0.0f) m.rocket_thrust *= 1.0f + v; break;
        case FLY_AFFIX_PROPELLANT: if (b->propellant_add > 0.0f) m.propellant_add += v; break;
        case FLY_AFFIX_RCS:       if (b->rcs_auth > 0.0f) m.rcs_auth += v; break;
        case FLY_AFFIX_ROLL:
            if (fly__mul(b->roll_mul) != 1.0f) m.roll_mul = fly__mul(b->roll_mul) * (1.0f + v);
            break;
        /* Stored in degrees, applied in radians; see the affix table. Both
         * halves of the stall move together because they are one property of
         * one wing — a section that holds on for another degree and a half is
         * a section making more lift at the top end, and a roll that gave the
         * angle without the lift would be a wing that stalls later and sinks
         * anyway. */
        case FLY_AFFIX_STALL:
            if (b->cl_max_add != 0.0f || b->alpha_stall_add != 0.0f) {
                m.alpha_stall_add += v * FLY_DEG2RAD;
                m.cl_max_add += v * 0.035f;
            }
            break;
        /* The launcher's three, under the same "sharpen, never invent" rule.
         * The magazine is a percentage rounded away from zero, so a roll on a
         * four-round rack is worth at least one more round rather than being
         * quietly truncated to nothing — the one place in this table where the
         * unit is an integer and the rounding is therefore a design decision
         * rather than an arithmetic detail. */
        case FLY_AFFIX_ORD_DAMAGE:
            if (b->ord_kind) m.ord_damage = (b->ord_damage > 0.0f ? b->ord_damage : 1.0f) * (1.0f + v);
            break;
        case FLY_AFFIX_ORD_RANGE:
            if (b->ord_kind && b->ord_range > 0.0f) m.ord_range += v;
            break;
        case FLY_AFFIX_MAGAZINE:
            if (b->ord_kind) {
                float add = (float)b->ord_ammo * v;
                m.ord_ammo = b->ord_ammo + (int)(add < 0.0f ? add - 0.5f : add + 0.5f);
                if (m.ord_ammo < 1) m.ord_ammo = 1;
            }
            if (b->cm_kind) {
                float add = (float)b->cm_ammo * v;
                m.cm_ammo = b->cm_ammo + (int)(add < 0.0f ? add - 0.5f : add + 0.5f);
                if (m.cm_ammo < 1) m.cm_ammo = 1;
            }
            break;
        default: break;
        }
    }
    if (m.thrust_mul < 0.1f) m.thrust_mul = 0.1f;
    if (m.fuel_mul < 0.1f) m.fuel_mul = 0.1f;
    if (m.cargo_mul < 0.1f) m.cargo_mul = 0.1f;
    if (m.roll_mul != 0.0f && m.roll_mul < 0.1f) m.roll_mul = 0.1f;
    return m;
}

/* What a shelf at `loc` is allowed to be. A shop is a place with a counter and
 * an order book: it can get you a good part, and it cannot get you a part
 * nobody made two of. So a vendor is biased hard *down* the ladder, less hard
 * the harder the place is to reach — and a One-off is off the list outright,
 * because a one-off is by definition not a thing anybody has a second of. The
 * best gear has to be taken off somebody. */
static float fly__vendor_bias(const fly_world *w, int loc) {
    float d = fly_world_danger(w, w->loc[loc].pos.e, w->loc[loc].pos.n);
    return -2.6f + 1.7f * d;
}

/* Which base belongs on shelf `m` at site `l`.
 *
 * A power's own field keeps its own counter — shelf zero, and only shelf zero,
 * carries something that power builds. One shelf rather than the whole rack
 * because a Foundry city is still a city: it sells the open registry to
 * whoever lands, and the interesting part is the one drawer behind the desk.
 *
 * Exactly one draw off `rng` whichever branch is taken, including the fallback
 * when the power builds nothing at this level. That is the property that keeps
 * the shelf stream aligned across a change to the registry, and it is why
 * `fly_module_roll_faction` returns -1 without drawing rather than picking
 * something wrong. */
static int fly__shelf_base(const fly_world *w, int loc, int m, int ilvl, fly_rng *rng) {
    if (m == 0 && fly_faction_is_power(w->loc[loc].owner)) {
        int b = fly_module_roll_faction(ilvl, (int)w->loc[loc].owner, rng);
        if (b >= 0) return b;
    }
    return fly_module_roll_base(ilvl, rng);
}

fly_item fly_game_vendor_roll(const fly_world *w, int loc, int base, fly_rng *rng) {
    const fly_location *L = &w->loc[loc];
    fly_item it = fly_game_roll_item(base, fly_world_item_level(w, L->pos.e, L->pos.n),
                                     fly__vendor_bias(w, loc), rng);
    if (it.rarity >= FLY_ITEM_ONEOFF) {
        /* rolled the unbuyable tier: it comes in as the tier below, one affix
           short, which is what a shop could actually have sourced */
        it.rarity = FLY_ITEM_PROTOTYPE;
        if (it.naff > 3) it.naff = 3;
    }
    return it;
}

fly_item fly_game_item_stock(int base) {
    fly_item it;
    memset(&it, 0, sizeof it);
    it.base = (short)base;
    it.rarity = FLY_ITEM_STOCK;
    it.ilvl = 1;
    return it;
}

/* --- the hold ---
 *
 * One array holds everything the operator owns, fitted or not; an airframe's
 * slots are uids into it. That is what makes "is this item fitted" a question
 * with one answer, and it is why moving an item between airframes never copies
 * anything. */

static int fly__item_index(const fly_game *g, uint32_t uid) {
    int i;
    if (!uid) return -1;
    for (i = 0; i < g->item_count; ++i)
        if (g->items[i].uid == uid) return i;
    return -1;
}

const fly_item *fly_game_item(const fly_game *g, uint32_t uid) {
    int i = fly__item_index(g, uid);
    return i >= 0 ? &g->items[i] : NULL;
}

const fly_module *fly_game_item_base(const fly_game *g, uint32_t uid) {
    const fly_item *it = fly_game_item(g, uid);
    return it ? fly_module_get(it->base) : NULL;
}

/* Which airframe has this item bolted to it, or -1 for one on the shelf. */
static int fly__item_fitted_to(const fly_game *g, uint32_t uid) {
    int a, s;
    if (!uid) return -1;
    for (a = 0; a < g->owned_count; ++a)
        for (s = 0; s < FLY_SLOT_COUNT; ++s)
            if (g->owned[a].fitted[s] == uid) return a;
    return -1;
}

int fly_game_item_fitted(const fly_game *g, uint32_t uid) {
    return fly__item_fitted_to(g, uid) >= 0;
}

uint32_t fly_game_item_add(fly_game *g, const fly_item *it) {
    fly_item *dst;
    if (!it || !fly_module_get(it->base)) return 0;
    if (g->item_count >= FLY_ITEM_MAX) return 0;   /* a full hold refuses */
    dst = &g->items[g->item_count++];
    *dst = *it;
    if (!g->next_item_uid) g->next_item_uid = 1;
    dst->uid = g->next_item_uid++;
    if (!g->next_item_uid) g->next_item_uid = 1;   /* 0 is the empty item */
    return dst->uid;
}

int fly_game_item_remove(fly_game *g, uint32_t uid) {
    int i = fly__item_index(g, uid);
    if (i < 0) return -1;
    if (fly__item_fitted_to(g, uid) >= 0) return -2;
    g->items[i] = g->items[--g->item_count];
    memset(&g->items[g->item_count], 0, sizeof g->items[0]);
    return 0;
}

int fly_game_item_held(const fly_game *g, int base) {
    int i, n = 0;
    for (i = 0; i < g->item_count; ++i)
        if (g->items[i].base == base && fly__item_fitted_to(g, g->items[i].uid) < 0) ++n;
    return n;
}

/* The worst loose item of a base: what "break up a Gun Pod" should mean when
 * the player has three and asked from a list of types. */
static uint32_t fly__worst_held(const fly_game *g, int base) {
    uint32_t worst = 0;
    long worstv = -1;
    int i;
    for (i = 0; i < g->item_count; ++i) {
        long v;
        if (g->items[i].base != base) continue;
        if (fly__item_fitted_to(g, g->items[i].uid) >= 0) continue;
        v = fly_item_value(&g->items[i]);
        if (worstv < 0 || v < worstv) { worstv = v; worst = g->items[i].uid; }
    }
    return worst;
}

/* A catalog airframe's factory fit: one item per starter slot, added to the
 * hold and bolted straight on. Stock rarity, because that is what a new
 * aeroplane comes with — everything better has to be found. */
static void fly__outfit_starter(fly_game *g, int airframe, int ci);

static void fly__outfit_starter(fly_game *g, int airframe, int ci) {
    int s;
    if (airframe < 0 || airframe >= g->owned_count || ci < 0) return;
    for (s = 0; s < FLY_SLOT_COUNT; ++s) {
        int base = fly__airframes[ci].starter_fitted[s];
        fly_item it;
        g->owned[airframe].fitted[s] = 0;
        if (base < 0 || !fly_module_get(base)) continue;
        it = fly_game_item_stock(base);
        g->owned[airframe].fitted[s] = fly_game_item_add(g, &it);
    }
}

/* The best loose item of a base, by value: what "fit a Gun Pod" should mean
 * when the player has three of them and asked from a list of bases. */
static uint32_t fly__best_held(const fly_game *g, int base) {
    uint32_t best = 0;
    long bestv = -1;
    int i;
    for (i = 0; i < g->item_count; ++i) {
        long v;
        if (g->items[i].base != base) continue;
        if (fly__item_fitted_to(g, g->items[i].uid) >= 0) continue;
        v = fly_item_value(&g->items[i]);
        if (v > bestv) { bestv = v; best = g->items[i].uid; }
    }
    return best;
}

int fly_game_airframe_catalog_count(void) { return fly__nairframes; }

const fly_airframe_catalog *fly_game_airframe_catalog_get(int idx) {
    return idx >= 0 && idx < fly__nairframes ? &fly__airframes[idx] : NULL;
}

int fly_game_airframe_catalog_find(const char *id) {
    int i;
    if (!id) return -1;
    for (i = 0; i < fly__nairframes; ++i)
        if (strcmp(fly__airframes[i].id, id) == 0) return i;
    return -1;
}

static int fly__catalog_add(const fly_airframe *base, long price, int starter,
                            float alloy, float parts, int faction) {
    int idx = fly_game_airframe_catalog_find(base->id);
    if (idx < 0) {
        if (fly__nairframes >= FLY_AIRFRAME_CATALOG_MAX) return -1;
        idx = fly__nairframes++;
    }
    fly_airframe_catalog *c = &fly__airframes[idx];
    memset(c, 0, sizeof *c);
    c->base = *base;
    snprintf(c->id, sizeof c->id, "%s", base->id);
    snprintf(c->name, sizeof c->name, "%s", base->name);
    c->price = price;
    c->starter = starter;
    c->build_alloy = alloy;
    c->build_parts = parts;
    c->faction = fly_faction_is_power(faction) ? faction : FLY_FACTION_FREE;
    {
        int i;
        for (i = 0; i < FLY_SLOT_COUNT; ++i) c->starter_fitted[i] = -1;
    }
    return idx;
}

static void fly__catalog_defaults(void) {
    fly_airframe af;
    if (fly_game_airframe_catalog_find("skylark") < 0) {
        fly_airframe_default_plane(&af);
        int ci = fly__catalog_add(&af, 700, 1, 18, 8, FLY_FACTION_FREE);
        int radar = fly_module_find("radar_array");
        if (ci >= 0 && radar >= 0) fly__airframes[ci].starter_spares[radar] = 1;
    }
    if (fly_game_airframe_catalog_find("dragonfly") < 0) {
        fly_airframe_default_drone(&af);
        int ci = fly__catalog_add(&af, 580, 1, 10, 12, FLY_FACTION_FREE);
        int radar = fly_module_find("radar_array");
        if (ci >= 0 && radar >= 0) fly__airframes[ci].starter_spares[radar] = 1;
    }
}

int fly_game_load_airframe_catalog(const char *path) {
    fly_yaml_node *n = fly_yaml_load_file(path);
    if (!n) return -1;
    fly_airframe af;
    fly_airframe_from_yaml(&af, n);
    if (!af.id[0]) { fly_yaml_free(n); return -2; }
    long price = (long)fly_yaml_num(n, "catalog.price", af.mass * 0.7 + 100.0);
    int starter = fly_yaml_bool(n, "catalog.starter", 0);
    float alloy = (float)fly_yaml_num(n, "catalog.build_alloy", af.mass / 80.0f);
    float parts = (float)fly_yaml_num(n, "catalog.build_parts", 8.0);
    int faction = FLY_FACTION_FREE;
    {   /* By tag, for the same reason the modules are: a data file naming
         * "CNCD" keeps meaning the Concord whatever the enum does next. */
        const char *ftag = fly_yaml_str(n, "catalog.faction", NULL);
        int fi;
        for (fi = 1; ftag && fi < FLY_FACTION_COUNT; ++fi)
            if (strcmp(fly_faction_tag(fi), ftag) == 0) { faction = fi; break; }
    }
    int rc = fly__catalog_add(&af, price, starter, alloy, parts, faction);
    if (rc >= 0) {
        const fly_yaml_node *mods = fly_yaml_get(n, "catalog.starter_modules");
        int i;
        for (i = 0; i < fly_yaml_count(mods); ++i) {
            const fly_yaml_node *v = fly_yaml_at(mods, i);
            int m = v && v->scalar ? fly_module_find(v->scalar) : -1;
            if (m >= 0) fly__airframes[rc].starter_fitted[fly_module_get(m)->slot] = m;
        }
        mods = fly_yaml_get(n, "catalog.starter_spares");
        for (i = 0; i < fly_yaml_count(mods); ++i) {
            const fly_yaml_node *v = fly_yaml_at(mods, i);
            int m = v && v->scalar ? fly_module_find(v->scalar) : -1;
            if (m >= 0 && fly__airframes[rc].starter_spares[m] < 65535)
                ++fly__airframes[rc].starter_spares[m];
        }
    }
    fly_yaml_free(n);
    return rc;
}

static fly_owned_airframe *fly__active(fly_game *g) {
    return g->active_airframe >= 0 && g->active_airframe < g->owned_count
               ? &g->owned[g->active_airframe] : NULL;
}

static const fly_owned_airframe *fly__active_const(const fly_game *g) {
    return g->active_airframe >= 0 && g->active_airframe < g->owned_count
               ? &g->owned[g->active_airframe] : NULL;
}

const uint32_t *fly_game_fitted(const fly_game *g) {
    const fly_owned_airframe *o = fly__active_const(g);
    return o ? o->fitted : NULL;
}

/* base airframe + every fitted module -> effective airframe (sim + visuals) */
/* --- one module onto one airframe ---------------------------------------
 *
 * Extracted rather than inlined into the loop below because it had already
 * been copied: the test suite carried its own transcription of these thirty
 * lines so it could bolt the rocket stack onto an airframe without building a
 * whole game around it, and that copy silently did not know about the handling
 * block the moment the handling block existed. A fold written twice is a fold
 * that will disagree with itself, and the two places it can disagree are "what
 * the game flies" and "what the tests measured". */
void fly_module_apply(fly_airframe *af, const fly_module *m) {
    if (!af || !m) return;
    af->thrust_max *= fly__mul(m->thrust_mul);
    af->rotor_thrust *= fly__mul(m->thrust_mul);
    af->fuel_cap *= fly__mul(m->fuel_mul);
    af->cargo_cap *= fly__mul(m->cargo_mul);
    af->mass += m->mass_add;
    if (af->mass < 20.0f) af->mass = 20.0f;
    af->ceiling += m->ceiling_add;
    af->vne += m->vne_add;
    af->cd0 += m->cd0_add;
    if (af->cd0 < 0.008f) af->cd0 = 0.008f;
    af->structure += m->structure_add;
    if (af->structure < 20.0f) af->structure = 20.0f;
    if (m->weather_rating > af->weather_rating) af->weather_rating = m->weather_rating;
    if (m->radar_range > af->radar_range) af->radar_range = m->radar_range;
    if (m->gun_dps > 0) {
        af->gun_dps = m->gun_dps;
        af->gun_range = m->gun_range;
        af->beam_heat = m->beam_heat;   /* travels with the weapon that has it */
    }
    if (m->armor > af->armor) af->armor = m->armor;
    /* The launcher and the dispenser: the best of each fitted, the way the
     * rocket stack's parts are, rather than a sum. Two racks would be two
     * weapons and there is one trigger — and an aeroplane that quietly fired
     * whichever the fold happened to visit last would be an aeroplane whose
     * loadout screen was lying. Magazines *do* add, because two racks really
     * do hold twice as many rounds and nothing else about the aircraft
     * changes. */
    if (m->ord_kind && m->ord_damage > af->ord_damage) {
        af->ord_kind = m->ord_kind;
        af->ord_range = m->ord_range;
        af->ord_damage = m->ord_damage;
    }
    if (m->ord_kind) af->ord_ammo += m->ord_ammo;
    if (m->cm_kind && !af->cm_kind) af->cm_kind = m->cm_kind;
    if (m->cm_kind) af->cm_ammo += m->cm_ammo;
    if (m->ecm > af->ecm) af->ecm = m->ecm;
    /* The rocket stack. Each part is the best of its kind fitted rather than a
     * sum, except the tanks — two tanks really would hold twice as much, and
     * nothing stops a build carrying one in each of two bays if a later
     * airframe ever has them. */
    if (m->rocket_thrust > af->rocket_thrust) {
        af->rocket_thrust = m->rocket_thrust;
        af->rocket_ve = m->rocket_ve;
    }
    af->propellant_cap += m->propellant_add;
    if (m->rcs_auth > af->rcs_auth) af->rcs_auth = m->rcs_auth;
    if (m->heat_shield > af->heat_shield) af->heat_shield = m->heat_shield;
    if (m->pressure > af->pressure) af->pressure = m->pressure;
    if (m->vtol) af->vtol = 1;
    /* --- and how the result handles ---
     *
     * The half of a fit that used to be missing. Everything above changes what
     * the aeroplane can do; this changes what it is like, and it is the reason
     * a Gun Pod is a decision rather than a purchase: forty kilos on a wing
     * pylon is a slower roll, an extra second to reverse a turn, and a fight
     * you have to fly differently.
     *
     * The wing is multiplied rather than added to, because area and span *are*
     * the wing: fly_sim reads them for lift, for induced drag, for the stall
     * speed, for how deep the ground effect is and for how much of the mass is
     * out at the tips. One number on a wings module moves all five, in the
     * directions they really move together. */
    af->pitch_auth *= fly__mul(m->pitch_mul);
    af->roll_auth *= fly__mul(m->roll_mul);
    af->yaw_auth *= fly__mul(m->yaw_mul);
    af->wing_area *= fly__mul(m->wing_area_mul);
    af->wing_span *= fly__mul(m->wing_span_mul);
    af->cl_max += m->cl_max_add;
    if (af->cl_max < 0.4f) af->cl_max = 0.4f;
    af->alpha_stall += m->alpha_stall_add;
    if (af->alpha_stall < 0.08f) af->alpha_stall = 0.08f;
    af->stability = fly__mul(af->stability) * fly__mul(m->stability_mul);
    af->damping = fly__mul(af->damping) * fly__mul(m->damping_mul);
    af->inertia = fly__mul(af->inertia) * fly__mul(m->inertia_mul);
    af->spool = fly__mul(af->spool) * fly__mul(m->spool_mul);
    af->torque *= fly__mul(m->torque_mul);
    af->g_limit += m->g_limit_add;
    if (af->g_limit < 2.0f) af->g_limit = 2.0f;
    af->visual_mask |= m->visual;
}

int fly_game_rebuild_airframe(fly_game *g) {
    fly_owned_airframe *o = fly__active(g);
    int ci = o ? fly_game_airframe_catalog_find(o->catalog_id) : -1;
    int s2;
    if (!o || ci < 0) return -1;
    g->player.airframe = fly__airframes[ci].base;
    for (s2 = 0; s2 < FLY_SLOT_COUNT; ++s2) {
        const fly_item *it = fly_game_item(g, o->fitted[s2]);
        fly_module eff;
        if (!it) continue;
        eff = fly_game_item_effect(it);
        fly_module_apply(&g->player.airframe, &eff);
        if (it->rarity > g->player.airframe.visual_rarity)
            g->player.airframe.visual_rarity = (unsigned char)it->rarity;
    }
    /* Life used up costs structure and power — an old hull is not the machine
     * it was, and the last flights before a replacement is due are flown on a
     * worse aeroplane than the first. Bounded well short of zero: a condemned
     * airframe should still be recognisably itself, not a brick. */
    if (o->fatigue > 0.0f) {
        float f = fly_clampf(o->fatigue, 0.0f, 1.0f);
        g->player.airframe.structure *= 1.0f - 0.45f * f;
        g->player.airframe.thrust_max *= 1.0f - 0.18f * f;
        g->player.airframe.rotor_thrust *= 1.0f - 0.18f * f;
        /* An old airframe is a loose one. Worn hinges and stretched cables
         * cost the same authority fly_sim already takes off for wear, and a
         * spar that has been cycled forty thousand times is not stressed for
         * what the placard says any more — which is the one place fatigue is
         * felt in the air rather than read on a maintenance screen. */
        g->player.airframe.g_limit = fly__mul(g->player.airframe.g_limit) * (1.0f - 0.20f * f);
        g->player.airframe.stability = fly__mul(g->player.airframe.stability) * (1.0f - 0.15f * f);
    }
    return 0;
}

int fly_game_has_upgrade(const fly_game *g, const char *module_id) {
    const uint32_t *fitted = fly_game_fitted(g);
    int s2;
    if (!fitted) return 0;
    for (s2 = 0; s2 < FLY_SLOT_COUNT; ++s2) {
        const fly_module *m = fly_game_item_base(g, fitted[s2]);
        if (m && strcmp(m->id, module_id) == 0) return 1;
    }
    return 0;
}

/* ---------------- log ---------------- */

void fly_game_log(fly_game *g, const char *fmt, ...) {
    if (g->log_count == FLY_LOG_LINES) {
        memmove(g->log[0], g->log[1], sizeof g->log[0] * (FLY_LOG_LINES - 1));
        --g->log_count;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->log[g->log_count], sizeof g->log[0], fmt, ap);
    va_end(ap);
    ++g->log_count;
}

/* ---------------- helpers ---------------- */

float fly_game_cargo_mass(const fly_game *g) {
    float m = 0;
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r) m += g->player.cargo[r] * fly_resource_mass((fly_resource)r);
    return m;
}

float fly_game_radar_range(const fly_game *g) {
    return g->player.airframe.radar_range > 0.0f ? g->player.airframe.radar_range : 3200.0f;
}

long fly_game_score(const fly_game *g) {
    long v = g->player.tokens + g->xp * 5 + g->reputation * 20;
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r)
        v += (long)(g->player.cargo[r] * fly_resource_base_price((fly_resource)r));
    int i, j;
    for (i = 0; i < g->owned_count; ++i) {
        int ci = fly_game_airframe_catalog_find(g->owned[i].catalog_id);
        if (ci >= 0 && g->owned[i].status == FLY_AIRFRAME_USABLE) v += fly__airframes[ci].price;
        (void)j;
    }
    /* every item once, fitted or not: the hold is the whole estate */
    for (i = 0; i < g->item_count; ++i) v += fly_item_value(&g->items[i]);
    return v;
}

/* How far a place on the chart is from something in the air over it. The
 * altitude is deliberately dropped: every caller is asking a map question —
 * am I within docking range, how far is the wreck — and none of them means
 * "how far would I have to fly through the air to touch it". */
static float dist2d(fly_v3 a, fly_wpos b) {
    return fly_world_dist(fly_wpos_of(a), b);
}

long fly_game_item_price(const fly_game *g, int loc, const fly_item *it) {
    long base;
    int i, same = 0;
    float scarcity, rep;
    if (!it || !fly_module_get(it->base) || loc < 0 || loc >= g->world.nloc) return -1;
    base = fly_item_value(it);
    /* Scarcity is per base type, not per item: a shelf with three gun pods on
     * it prices all three as gun pods, however differently they rolled. */
    for (i = 0; i < FLY_SITE_STOCK_MAX; ++i)
        if (g->stock[loc][i].uid && g->stock[loc][i].base == it->base) ++same;
    scarcity = same ? 1.0f + 0.4f / (float)same : 1.75f;
    rep = 1.0f - fly_clampf((float)g->reputation * 0.005f, 0, 0.2f);
    return (long)((float)base * scarcity * rep * fly_game_faction_markup(g, loc)) + 1;
}

long fly_game_airframe_price(const fly_game *g, int loc, int catalog) {
    const fly_airframe_catalog *c = fly_game_airframe_catalog_get(catalog);
    if (!c || loc < 0 || loc >= g->world.nloc) return -1;
    unsigned stock = g->airframe_stock[loc][catalog];
    float scarcity = stock ? 1.0f + 0.25f / (float)stock : 1.6f;
    return (long)(c->price * scarcity * fly_game_faction_markup(g, loc)) + 1;
}

int fly_game_launch_ground_item(fly_game *g, const fly_item *drop, fly_v3 pos, fly_v3 vel,
                                fly_ground_source source, uint32_t source_id) {
    if (!drop || !fly_module_get(drop->base)) return -1;
    int i;
    for (i = 0; i < FLY_GROUND_ITEM_MAX; ++i)
        if (!g->ground_items[i].active) break;
    if (i == FLY_GROUND_ITEM_MAX) {
        fly_game_log(g, "Ground item capacity reached; module lost");
        return -3;
    }
    {
        uint32_t h = fly_hash2(g->seed + source_id, drop->base, i);
        fly_ground_item *item = &g->ground_items[i];
        float gz = fly_world_ground(&g->world, pos.x, pos.y);
        memset(item, 0, sizeof *item);
        item->active = 1;
        item->item = *drop;
        item->pos = pos;
        item->vel = vel;
        item->yaw = (float)(h & 1023u) / 1024.0f * 2.0f * FLY_PI;
        /* tumble direction from the same hash, so a given wreck always throws
           the same piece the same way */
        item->spin = ((float)((h >> 10) & 255u) / 255.0f - 0.5f) * 9.0f;
        item->source = source;
        item->source_id = source_id;
        /* Something dropped where it already lies is simply lying there. The
           water rule moves to the landing, because a piece thrown off a wreck
           on a shoreline decides which side it is on by where it comes down. */
        if (fly_v3len(vel) < 0.01f) {
            if (gz <= fly_world_water_at(&g->world, pos.x, pos.y, gz)) { item->active = 0; return -2; }
            item->pos = fly_v3mk(pos.x, pos.y, gz + 0.35f);
            item->settled = 1;
            item->spin = 0.0f;
        }
        return i;
    }
}

int fly_game_spawn_ground_item(fly_game *g, const fly_item *drop, fly_v3 pos,
                               fly_ground_source source, uint32_t source_id) {
    return fly_game_launch_ground_item(g, drop, pos, fly_v3zero(), source, source_id);
}

/* Fire, smoke and the pieces going out sideways: a hull ending, made visible.
 *
 * No random draws and no command. A blast is read by the renderer and by
 * nothing else, so it cannot reach the simulation however it is spawned — which
 * is what lets every pilot's crash raise one too, without the sky the player
 * cannot see costing the world its determinism. */
void fly_game_spawn_blast(fly_game *g, fly_v3 pos, float size) {
    int i, slot = -1;
    float oldest = -1.0f;
    for (i = 0; i < FLY_BLAST_MAX; ++i) {
        if (!g->blasts[i].active) { slot = i; break; }
        if (g->blasts[i].age > oldest) { oldest = g->blasts[i].age; slot = i; }
    }
    if (slot < 0) return;
    memset(&g->blasts[slot], 0, sizeof g->blasts[slot]);
    g->blasts[slot].active = 1;
    g->blasts[slot].pos = pos;
    g->blasts[slot].size = size > 1.0f ? size : 1.0f;
    g->blasts[slot].seed = fly_hash2(g->seed ^ 0x9E3779B9u, slot,
                                     (int)(g->time_s * 4.0));
}

/* Everything in the air after a crash comes down.
 *
 * A module used to be teleported to a random point on a circle around the
 * wreck, which reads as inventory appearing rather than as an aeroplane coming
 * apart. It is thrown now and this brings it back: gravity, a bounce or two,
 * and it stops. Landing in the sea loses it, which is the same rule the wreck
 * itself answers to and now answers where the piece actually finishes. */
static void step_ground_items(fly_game *g, float dt) {
    int i;
    for (i = 0; i < FLY_GROUND_ITEM_MAX; ++i) {
        fly_ground_item *it = &g->ground_items[i];
        float gz;
        if (!it->active || it->settled) continue;
        it->vel.z -= 9.80665f * dt;
        it->pos = fly_v3add(it->pos, fly_v3scale(it->vel, dt));
        it->yaw += it->spin * dt;
        gz = fly_world_ground(&g->world, it->pos.x, it->pos.y);
        if (it->pos.z > gz + 0.35f) continue;
        /* gone in the water — a river as much as the sea */
        if (gz <= fly_world_water_at(&g->world, it->pos.x, it->pos.y, gz)) { it->active = 0; continue; }
        it->pos.z = gz + 0.35f;
        if (it->vel.z < -3.0f) {         /* bounce, hard-damped */
            it->vel.z = -it->vel.z * 0.30f;
            it->vel.x *= 0.45f;
            it->vel.y *= 0.45f;
            it->spin *= 0.5f;
        } else {
            it->settled = 1;
            it->vel = fly_v3zero();
            it->spin = 0.0f;
        }
    }
}

static void step_blasts(fly_game *g, float dt) {
    int i;
    for (i = 0; i < FLY_BLAST_MAX; ++i) {
        if (!g->blasts[i].active) continue;
        g->blasts[i].age += dt;
        if (g->blasts[i].age > FLY_BLAST_LIFE) g->blasts[i].active = 0;
    }
}

/* ---------------- salvage, reserve and ruin ---------------- */

/* The site a wreck at `p` could be walked out from, or -1 if there is none.
 *
 * Whether anything comes back off a wreck is a question about where it went
 * down, not about how hard it hit: recovery is a walk, so the rule is dry
 * ground with a service site within walking reach of it. That one rule is what
 * makes route choice a survival decision, because the straight line — over open
 * water, or across the gap between the last two pads — is exactly the one that
 * cannot be recovered from. */
static int fly__salvage_site(const fly_world *w, fly_v3 p) {
    if (fly_world_wet(w, p.x, p.y)) return -1;
    int i, best = -1;
    float bd = FLY_SALVAGE_RANGE;
    for (i = 0; i < w->nloc; ++i) {
        fly_loc_kind k = w->loc[i].kind;
        if (k != FLY_LOC_CITY && k != FLY_LOC_SKYPORT) continue;
        float d = dist2d(p, w->loc[i].pos);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

long fly_game_hull_floor(const fly_game *g) {
    long best = -1;
    int i;
    (void)g;
    for (i = 0; i < fly_game_airframe_catalog_count(); ++i) {
        long p = fly_game_airframe_catalog_get(i)->price;
        if (p > 0 && (best < 0 || p < best)) best = p;
    }
    return best < 0 ? 0 : best;
}

long fly_game_reserve(const fly_game *g) {
    long v = g->player.tokens;
    int i, j;
    for (i = 0; i < FLY_RES_COUNT; ++i) {
        if (g->player.cargo[i] <= 0.0f) continue;
        /* airborne there is no counter to sell across, so value it at base */
        float px = g->docked >= 0 ? fly_world_price(&g->world, g->docked, (fly_resource)i)
                                  : fly_resource_base_price((fly_resource)i);
        v += (long)(g->player.cargo[i] * px * 0.92f);
    }
    /* every item once, fitted or not; the airframe loop below adds only hulls */
    for (i = 0; i < g->item_count; ++i)
        v += (long)((float)fly_item_value(&g->items[i]) * FLY_RESALE);
    /* Modules lying in the field count only while they can still be fetched.
     * A check blind to a reachable wreck would end runs that were winnable,
     * which is a worse failure than having no fail state at all. */
    for (i = 0; i < FLY_GROUND_ITEM_MAX; ++i) {
        const fly_ground_item *it = &g->ground_items[i];
        if (!it->active || !fly_module_get(it->item.base)) continue;
        if (fly__salvage_site(&g->world, it->pos) < 0) continue;
        v += (long)((float)fly_item_value(&it->item) * FLY_RESALE);
    }
    for (i = 0; i < g->owned_count; ++i) {
        if (g->owned[i].status != FLY_AIRFRAME_USABLE) continue;
        int ci = fly_game_airframe_catalog_find(g->owned[i].catalog_id);
        if (ci >= 0) v += (long)((float)fly__airframes[ci].price * FLY_RESALE);
        /* the fit is already counted: it is in the hold, above. Reading the
           slots here would count it twice — and would read a uid as a
           registry index while doing it. */
        (void)j;
    }
    return v;
}

float fly_game_reserve_hulls(const fly_game *g) {
    long floor_price = fly_game_hull_floor(g);
    return floor_price > 0 ? (float)fly_game_reserve(g) / (float)floor_price : 0.0f;
}

/* Cheap in the common case: the first airworthy hull returns before any of the
 * scanning happens, so this can sit on the end of every command. */
static void fly__check_ruin(fly_game *g) {
    if (g->ruin) return;
    int i;
    for (i = 0; i < g->owned_count; ++i)
        if (g->owned[i].status == FLY_AIRFRAME_USABLE) return;
    if (fly_game_reserve(g) >= fly_game_hull_floor(g)) return;
    g->ruin = FLY_RUIN_GROUNDED;
    g->autopilot = 0;
    g->autopilot_target = -1;
    fly_game_log(g, "GROUNDED after %d hull%s. Nothing left to fly, nothing left to buy one.",
                 g->hulls_lost, g->hulls_lost == 1 ? "" : "s");
}

/* ---------------- allegiance ----------------
 *
 * The operator's side of the war: which flag they fly, what each power makes
 * of them, and the handful of places a deed turns into pressure on the ground.
 *
 * The rules live here and only here. Every one of them is read by at least
 * three surfaces — the command that acts, the row that offers it, and the shop
 * that prices it — and the first version of this had the standing threshold
 * written out in two of the three. */

/* What a kill or a delivery is worth as pressure at the site it happened over.
 * A kill is worth about six deliveries: violence is the fast way to take
 * ground and it is supposed to be, because it is also the way that gets you
 * shot at. */
#define FLY_KILL_PUSH 0.085f
#define FLY_PLAYER_TRADE_PUSH (1.0f / 6000.0f)
/* How far from a site something has to happen to count as happening there. Two
 * pad reaches and a bit: a fight directly over the apron is plainly about the
 * apron, and one twelve kilometres out over open country is about nothing. */
#define FLY_PUSH_RANGE 6000.0f

int fly_game_standing(const fly_game *g, int faction) {
    if (!g || faction < 0 || faction >= FLY_FACTION_COUNT) return 0;
    return g->standing[faction];
}

void fly_game_credit_faction(fly_game *g, int faction, int points) {
    int f;
    if (!g || !fly_faction_is_power(faction) || !points) return;
    for (f = 1; f < FLY_FACTION_COUNT; ++f) {
        float share = fly_faction_standing_spill(&g->world.dip, faction, f);
        int d = (int)(share * (float)points + (share * (float)points >= 0.0f ? 0.5f : -0.5f));
        if (!d) continue;
        g->standing[f] = (int)fly_clampf((float)(g->standing[f] + d),
                                         -(float)FLY_STAND_MAX, (float)FLY_STAND_MAX);
    }
    /* An oath you have fallen far enough out of favour to lose is one you have
     * lost. Said out loud, because a player whose kit quietly stopped being on
     * the shelf would think the shop was broken. */
    if (g->player.faction != FLY_FACTION_FREE &&
        g->standing[g->player.faction] < FLY_STAND_HOSTILE) {
        fly_game_log(g, "The %s strike you from the register.",
                     fly_faction_name(g->player.faction));
        g->player.faction = FLY_FACTION_FREE;
    }
}

/* Business done at the field the operator is standing on, as standing.
 *
 * Deliberately slow — a few hundred tokens of trade is worth a point — because
 * the shortcut it would otherwise open is obvious and dreadful: buy and sell
 * the same fuel back and forth on one pad until a power loves you. A point per
 * several hundred tokens *of margin the market already took* means the grind
 * costs more than the standing is worth, and the honest routes are faster
 * anyway. */
static void fly__trade_credit(fly_game *g, long value) {
    static const long per_point = 260;
    long acc;
    int owner = fly_game_site_owner(g, g->docked);
    if (!fly_faction_is_power(owner) || value <= 0) return;
    acc = value / per_point;
    if (acc > 3) acc = 3;
    if (acc > 0) fly_game_credit_faction(g, owner, (int)acc);
}

int fly_game_site_owner(const fly_game *g, int loc) {
    if (!g || loc < 0 || loc >= g->world.nloc) return FLY_FACTION_FREE;
    return (int)g->world.loc[loc].owner;
}

/* Pressure at whatever site a thing happened over, or nowhere if it happened
 * over nowhere. Deliberately the *nearest* site rather than every site in
 * range: a fight belongs to one place, and spreading it thins it to nothing.
 */
static void fly__site_push_at(fly_game *g, fly_v3 at, int faction, float amount) {
    int li;
    if (!fly_faction_is_power(faction)) return;
    li = fly_world_nearest(&g->world, fly_wpos_of(at), 0);
    if (li < 0) return;
    if (fly_world_dist(g->world.loc[li].pos, fly_wpos_of(at)) > FLY_PUSH_RANGE) return;
    fly_faction_push(&g->world, li, faction, amount);
}

/* A kill between two pilots, as it lands on the map. The winner gains where it
 * happened; the loser's power loses its grip a little, which is what makes a
 * front line a place aircraft are actually lost over. */
static void fly__kill_push(fly_game *g, const fly_pilot *killer, const fly_pilot *killed) {
    fly__site_push_at(g, killed->craft.pos, killer->faction, FLY_KILL_PUSH);
    if (fly_faction_is_power(killed->faction)) {
        int li = fly_world_nearest(&g->world, fly_wpos_of(killed->craft.pos), 0);
        if (li >= 0 && g->world.loc[li].owner == killed->faction &&
            fly_world_dist(g->world.loc[li].pos, fly_wpos_of(killed->craft.pos)) <= FLY_PUSH_RANGE)
            g->world.loc[li].grip = fly_clampf(g->world.loc[li].grip - 0.035f, 0.0f, 1.0f);
    }
}

int fly_game_pledge_ok(const fly_game *g, int faction) {
    if (!g) return -1;
    if (faction == FLY_FACTION_FREE) return 0;   /* resigning needs no permission */
    if (!fly_faction_is_power(faction)) return -1;
    if (faction == g->player.faction) return -4;
    if (g->docked < 0 || g->world.loc[g->docked].owner != (unsigned char)faction) return -2;
    if (g->standing[faction] < FLY_STAND_PLEDGE) return -3;
    return 0;
}

/* What the flag on the mast does to a price.
 *
 * The scale is deliberately wide enough to be a reason to fly somewhere else —
 * a quarter off at a power that likes you against half again at one that does
 * not — and deliberately not wide enough to close a market: everywhere will
 * still sell you fuel, and the storm belt is still the storm belt. */
float fly_game_faction_markup(const fly_game *g, int loc) {
    int owner = fly_game_site_owner(g, loc);
    float s;
    if (!g || !fly_faction_is_power(owner)) return 1.0f;
    s = (float)g->standing[owner] / (float)FLY_STAND_MAX;
    if (g->player.faction == owner) s += 0.35f;
    else if (g->player.faction != FLY_FACTION_FREE)
        s += fly_diplomacy_rel(&g->world.dip, g->player.faction, owner) * 0.45f;
    return fly_clampf(1.0f - 0.30f * s, 0.72f, 1.55f);
}

/* Take the oath, or hand it back.
 *
 * Resigning is always legal and always costs: a power you walked out on thinks
 * less of you than one you never joined, and its enemies do not thank you for
 * it either — the spill sees to that. Swearing costs standing with everyone
 * they are at odds with, immediately and visibly, which is the whole of what
 * makes it a decision rather than a menu. */
static int fly__pledge(fly_game *g, int faction) {
    int rc = fly_game_pledge_ok(g, faction), f;
    if (rc != 0) {
        switch (rc) {
        case -2: fly_game_log(g, "Take the oath at one of their own fields."); break;
        case -3: fly_game_log(g, "The %s want %d standing; you have %d.",
                              fly_faction_name(faction), FLY_STAND_PLEDGE,
                              fly_game_standing(g, faction)); break;
        case -4: fly_game_log(g, "You already fly for the %s.",
                              fly_faction_name(faction)); break;
        default: fly_game_log(g, "No such power."); break;
        }
        return rc;
    }
    if (faction == FLY_FACTION_FREE) {
        int was = g->player.faction;
        if (was == FLY_FACTION_FREE) return -4;
        g->player.faction = FLY_FACTION_FREE;
        g->pledged_at = g->time_s;
        fly_game_credit_faction(g, was, -20);
        fly_game_log(g, "You hand back your %s papers.", fly_faction_name(was));
        return 0;
    }
    if (g->player.faction != FLY_FACTION_FREE)
        fly_game_credit_faction(g, g->player.faction, -20);
    g->player.faction = faction;
    g->pledged_at = g->time_s;
    /* The enemies of your new friends, told at once. Not a spill off a deed —
     * this is the oath itself, and it lands hardest on whoever they are
     * actually at war with. */
    for (f = 1; f < FLY_FACTION_COUNT; ++f) {
        float r;
        if (f == faction) continue;
        r = fly_diplomacy_rel(&g->world.dip, faction, f);
        if (r >= 0.0f) continue;
        g->standing[f] = (int)fly_clampf((float)g->standing[f] + r * 22.0f,
                                         -(float)FLY_STAND_MAX, (float)FLY_STAND_MAX);
    }
    fly_game_log(g, "Sworn to the %s. %s", fly_faction_name(faction),
                 fly_faction_creed(faction));
    return 0;
}

int fly_game_service_ok(const fly_game *g, int loc) {
    int owner = fly_game_site_owner(g, loc);
    if (!g || !fly_faction_is_power(owner)) return 0;
    if (g->standing[owner] <= FLY_STAND_HOSTILE) return -1;
    if (g->player.faction != FLY_FACTION_FREE &&
        fly_faction_hostile(&g->world, g->player.faction, owner)) return -2;
    return 0;
}

/* The quarter-hour of the war. Everything in it is a deterministic function of
 * world state and elapsed hours — not one random number, because a client that
 * does not simulate the politics has to end up on the same map as one that
 * does, and `think_rng` exists for exactly this class of mistake.
 *
 * Presence is worked out here rather than pushed by the pilots themselves
 * because it is not a decision anybody takes: an aircraft sitting over a place
 * is pressure on that place whether or not it does anything, and counting them
 * once an hour is both cheaper and less arbitrary than having each one file a
 * claim. Cargo aircraft count for a little and gunships for a lot, which is
 * the difference between a trade route and an occupation. */
static void fly__faction_tick(fly_game *g, float dt_hours) {
    fly_faction_event evt[8];
    int n, i, li;

    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        const fly_pilot *p = &g->pilots[i];
        float w;
        if (!p->active || !fly_faction_is_power(p->faction)) continue;
        li = fly_world_nearest(&g->world, fly_wpos_of(p->craft.pos), 0);
        if (li < 0) continue;
        if (fly_world_dist(g->world.loc[li].pos, fly_wpos_of(p->craft.pos)) > FLY_PUSH_RANGE)
            continue;
        w = fly_pilot_is_combatant(p) ? 0.055f : 0.016f;
        fly_faction_push(&g->world, li, p->faction, w * dt_hours * 4.0f);
    }
    /* And the operator, who is one of the aircraft over the place. Wearing a
     * flag is not a badge: it is why the front line moves where you fly. */
    if (g->player.faction != FLY_FACTION_FREE && g->mode == FLY_MODE_FLIGHT)
        fly__site_push_at(g, g->player.craft.pos, g->player.faction,
                          (g->player.airframe.gun_dps > 0.0f ? 0.075f : 0.030f) * dt_hours * 4.0f);

    n = fly_faction_step(&g->world, dt_hours, evt, (int)(sizeof evt / sizeof evt[0]));
    for (i = 0; i < n; ++i) {
        switch (evt[i].kind) {
        case FLY_FEVT_SEIZED:
            ++g->seizures_seen;
            fly_game_log(g, "%s takes %s from %s", fly_faction_name(evt[i].a),
                         g->world.loc[evt[i].loc].name,
                         evt[i].b == FLY_FACTION_FREE ? "no one"
                                                      : fly_faction_name(evt[i].b));
            /* Losing a field you were welcome at hurts, and gaining one you
             * helped take is the payoff for having flown there all week. */
            if (evt[i].a == g->player.faction) fly_game_credit_faction(g, evt[i].a, 6);
            break;
        case FLY_FEVT_WAR:
            fly_game_log(g, "%s and %s are at war", fly_faction_name(evt[i].a),
                         fly_faction_name(evt[i].b));
            break;
        case FLY_FEVT_PACT:
            fly_game_log(g, "%s and %s come to terms", fly_faction_name(evt[i].a),
                         fly_faction_name(evt[i].b));
            break;
        default: break;
        }
    }
}

/* ---------------- contracts ---------------- */

static void offer_contracts(fly_game *g) {
    if (g->docked < 0) return;
    int slot, made = 0;
    for (slot = 0; slot < FLY_MAX_CONTRACTS; ++slot) {
        fly_contract *ct = &g->contracts[slot];
        if (ct->state == 2) continue; /* keep active */
        ct->state = 0;
        if (made >= 3) continue;
        /* offer: haul what the destination is short of */
        int tries;
        for (tries = 0; tries < 12; ++tries) {
            int to = (int)fly_rng_range(&g->rng, (uint32_t)g->world.nloc);
            if (to == g->docked) continue;
            const fly_location *dst = &g->world.loc[to];
            /* mostly discovered destinations; rumors open new sites */
            int rumor = !dst->discovered;
            if (rumor && fly_rng_f01(&g->rng) < 0.7f) continue;
            int r = (int)fly_rng_range(&g->rng, FLY_RES_COUNT);
            float need = dst->target[r] - dst->stock[r];
            if (need < 5.0f || dst->cons[r] <= 0.0f) continue;
            float qty = fly_clampf(need * fly_rng_span(&g->rng, 0.3f, 0.6f), 4.0f, 40.0f);
            float dist = fly_world_dist(dst->pos, g->world.loc[g->docked].pos);
            /* pay scales with scarcity at destination, distance and gates */
            float px = fly_world_price(&g->world, to, (fly_resource)r);
            float gate_bonus = 1.0f + 0.45f * (float)(!!dst->gates) + (rumor ? 0.5f : 0.0f);
            float rep_bonus = 1.0f + 0.015f * (float)g->reputation;
            long reward = (long)(qty * px * (1.1f + dist / 14000.0f) * gate_bonus * rep_bonus);
            ct->state = 1;
            ct->from = g->docked;
            ct->to = to;
            ct->res = r;
            ct->qty = floorf(qty);
            ct->reward = reward;
            ct->deadline = g->time_s + 3600.0 * (2.0 + dist / 9000.0 * 3.0);
            ct->cargo_loaded = 0;
            ++made;
            break;
        }
    }
}

static void complete_deliveries(fly_game *g) {
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i) {
        fly_contract *ct = &g->contracts[i];
        if (ct->state != 2 || ct->to != g->docked) continue;
        if (!ct->cargo_loaded || g->player.cargo[ct->res] + 0.01f < ct->qty) continue;
        g->player.cargo[ct->res] -= ct->qty;
        g->world.loc[g->docked].stock[ct->res] += ct->qty;
        g->player.tokens += ct->reward;
        g->xp += 10 + (int)(ct->reward / 40);
        g->reputation += 1;
        /* The job was somebody's job. Standing with whoever holds the field it
         * landed at, and — if the operator is wearing colours — grip on the
         * ground it landed on: a delivery is the ordinary way a power takes a
         * place, and there is no reason the player should be exempt from being
         * one of the aircraft doing it. */
        {
            int owner = fly_game_site_owner(g, g->docked);
            fly_game_credit_faction(g, owner, 3 + (int)(ct->reward / 400));
            if (g->player.faction != FLY_FACTION_FREE)
                fly_faction_push(&g->world, g->docked, g->player.faction,
                                 (float)ct->reward * FLY_PLAYER_TRADE_PUSH);
        }
        fly_game_log(g, "Delivered %.0f %s: +%ld tk", ct->qty,
                     fly_resource_name((fly_resource)ct->res), ct->reward);
        /* A job into somewhere hard pays in kit as well as in tokens, rolled
         * at that somewhere's own level. The tokens are what keeps an operator
         * flying; this is what makes a particular run worth choosing. Gated
         * destinations and the biggest fees only — a milk run stays a milk
         * run, or the whole ladder collapses into "do the nearest thing". */
        {
            const fly_location *L = &g->world.loc[ct->to];
            int lvl = fly_world_item_level(&g->world, L->pos.e, L->pos.n);
            int rich = ct->reward >= 900;
            if ((L->gates || rich) && g->item_count < FLY_ITEM_MAX) {
                fly_item it = fly_game_roll_item(
                    fly_module_roll_base(lvl, &g->rng),
                    lvl, L->gates ? -0.5f : -1.4f, &g->rng);
                char nm[64];
                fly_item_name(&it, nm, sizeof nm);
                if (fly_game_item_add(g, &it)) fly_game_log(g, "Payment in kind: %s", nm);
            }
        }
        ct->state = 0;
    }
}

static void expire_contracts(fly_game *g) {
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i) {
        fly_contract *ct = &g->contracts[i];
        if (ct->state == 2 && g->time_s > ct->deadline) {
            g->reputation -= 2;
            if (g->reputation < 0) g->reputation = 0;
            fly_game_log(g, "Contract to %s expired (-rep)", g->world.loc[ct->to].name);
            ct->state = 0;
        }
    }
}

/* ---------------- init ---------------- */

void fly_game_init(fly_game *g, uint32_t seed, fly_craft_kind start_kind) {
    memset(g, 0, sizeof *g);
    fly__catalog_defaults();
    g->seed = seed;
    fly_rng_seed(&g->rng, seed * 2654435761u + 13u);
    fly_rng_seed(&g->think_rng, seed * 40503u + 7u);
    fly_rng_seed(&g->play_rng, seed * 22695477u + 29u);
    fly_rng_seed(&g->convoy_rng, (uint64_t)seed * 0x9e3779b97f4a7c15ULL + 0x63u);
    fly_world_gen(&g->world, seed);
    g->time_s = 8.0 * 3600.0;
    g->player.tokens = 250;
    g->docked = 0; /* home city */
    /* Unaligned, and owing the home field a little goodwill for the hangar
     * space. Nothing else: an operator starts as somebody the powers have not
     * heard of, and the first thing standing does is go up because they did a
     * job, not because the game handed them a side. */
    g->player.faction = FLY_FACTION_FREE;
    g->pledged_at = g->time_s;
    if (fly_faction_is_power(g->world.loc[0].owner))
        g->standing[g->world.loc[0].owner] = 8;
    g->autopilot_target = -1;
    g->active_airframe = 0;
    g->owned_count = 1;
    g->next_airframe_instance_id = 2;
    g->owned[0].instance_id = 1;
    g->owned[0].status = FLY_AIRFRAME_USABLE;
    g->owned[0].location = 0;
    g->owned[0].condition = 1.0f;
    {
        int ci = fly_game_airframe_catalog_find(start_kind == FLY_CRAFT_DRONE ? "dragonfly" : "skylark");
        if (ci < 0) ci = 0;
        snprintf(g->owned[0].catalog_id, sizeof g->owned[0].catalog_id, "%.23s", fly__airframes[ci].id);
        fly__outfit_starter(g, 0, ci);
    }
    fly_game_rebuild_airframe(g);
    fly_location *home = &g->world.loc[0];
    fly_v3 pos = fly_wpos_at(home->pos,
                             fly_world_ground(&g->world, home->pos.e, home->pos.n)
                             + g->player.airframe.gear_height);
    fly_craft_init(&g->player.craft, &g->player.airframe, pos);
    g->owned[0].fuel = g->player.craft.fuel;
    g->mode = FLY_MODE_FLIGHT;
    g->launch_site = 0;
    g->launch_pos = pos;
    fly_walk_init(&g->walker, fly_v3mk(pos.x + 4.0f, pos.y, home->pad_z), 0.0f);
    fly_rail_generate(&g->rail_route, &g->world, 0, g->world.nloc > 1 ? 1 : 0);
    /* The roads come out of the world the way the terrain does — same seed,
       same network — so nothing about them is stored and nothing about them
       needs to be, saves included: a load regenerates from the seed. What runs
       on them is filled in below, with the rest of the world's traffic.

       This also *changes* the world: the cuttings the network needs go into
       `world.cut` and fly_world_ground honours them from here on. It is the
       last thing that touches the heightfield, so everything sited before it —
       the rail's piers above all — stands on the ground as it was found. A cut
       can only lower ground, so the worst that costs is a pier a few metres
       taller where a road passes beneath the line, which is what happens where
       a road passes beneath a line.

       And the network is handed the guideway, because it was laid first and is
       on the ground the roads have to cross: they keep out of its corridor
       where there is a way round it, duck under it with headroom where there is
       not, and hand back every crossing so the line is carried over the
       carriageway on a structure rather than through it. See fly_road_build. */
    fly_road_build(&g->roads, &g->world, &g->rail_route);
    /* And the water, which is the same argument one medium over: a function of
       the world alone, so nothing about it is stored either. After the roads
       because a jetty is a structure and stands under the rule every structure
       stands under — nothing built in a carriageway — and after the cuttings
       for the reason below, since a quay is sited against the drawn ground and
       an earthwork moves it. See fly_sea.h. */
    fly_sea_build(&g->lanes, &g->world, &g->roads);
    /* And now that nothing else will move the ground, ask every site which ways
       into it are flyable. It has to be after the cuttings for the same reason
       the roads had to be last: a course surveyed over a ridge an earthwork is
       about to take out is a course refused for a hill that is not there. */
    fly_world_approach_survey(&g->world);
    /* --- derelicts ---
     *
     * Wrecks that were already out there when this operator signed on. They
     * are the reason to fly somewhere nobody sent you, and they are seeded
     * where nobody sends you: out past the sites, in the belt, on the far side
     * of the gates. Rolled at the ground's own level, so the ones worth the
     * trip are exactly the ones that are a trip. Placed off a stream of their
     * own so the number of them cannot shift the world that follows. */
    {
        fly_rng dr;
        int n = 0, tries;
        fly_rng_seed(&dr, (uint64_t)seed * 0x9e3779b97f4a7c15ULL + 0xd3u);
        for (tries = 0; tries < 400 && n < 10; ++tries) {
            float x = fly_rng_span(&dr, -0.92f, 0.92f) * FLY_WORLD_HALF;
            float y = fly_rng_span(&dr, -0.92f, 0.92f) * FLY_WORLD_HALF;
            float gz = fly_world_ground(&g->world, x, y);
            int lvl;
            fly_item it;
            if (gz <= fly_world_water_at(&g->world, x, y, gz) + 2.0f) continue;
            if (fly_world_danger(&g->world, x, y) < 0.45f) continue;
            lvl = fly_world_item_level(&g->world, x, y);
            it = fly_game_roll_item(fly_module_roll_base(lvl, &dr), lvl, 0.35f, &dr);
            it.uid = 0x40000000u + (uint32_t)n;
            if (fly_game_spawn_ground_item(g, &it, fly_v3mk(x, y, gz + 0.35f),
                                           FLY_GROUND_SOURCE_WORLD, 0) >= 0) ++n;
        }
    }
    {
        fly_rng stock_rng;
        uint32_t stock_uid = 0;   /* shelf uids are local to the shelf */
        int l, m, c;
        fly_rng_seed(&stock_rng, (uint64_t)seed * 0x9e3779b97f4a7c15ULL + 0x51u);
        for (l = 0; l < g->world.nloc; ++l) {
            /* A shelf is as good as the place is hard to reach. That is the
               whole of the vendor ladder: the kit that opens the far sites is
               also the kit that gets you to the shelves worth buying from —
               and past FLY_SPACE_ILVL it decides *what is on the shelf* and
               not only how well it rolled. */
            int service = g->world.loc[l].kind == FLY_LOC_CITY || g->world.loc[l].kind == FLY_LOC_SKYPORT;
            int shelf_lvl = fly_world_item_level(&g->world, g->world.loc[l].pos.e,
                                                 g->world.loc[l].pos.n);
            for (m = 0; m < FLY_SITE_STOCK_MAX; ++m) {
                memset(&g->stock[l][m], 0, sizeof g->stock[l][m]);
                if (!service || m >= 4 + (int)fly_rng_range(&stock_rng, 4)) continue;
                g->stock[l][m] = fly_game_vendor_roll(&g->world, l,
                    fly__shelf_base(&g->world, l, m, shelf_lvl, &stock_rng), &stock_rng);
                g->stock[l][m].uid = ++stock_uid;
            }
            for (c = 0; c < fly_game_airframe_catalog_count(); ++c) {
                const fly_airframe_catalog *cat = fly_game_airframe_catalog_get(c);
                int city = g->world.loc[l].kind == FLY_LOC_CITY;
                /* A power's own hull sits on its own apron and nowhere else,
                 * and it does not consume a draw: the four open hulls keep the
                 * stream they always had, so adding seven aircraft to the
                 * catalogue does not reseed every shelf behind them. */
                if (cat && cat->faction != FLY_FACTION_FREE) {
                    g->airframe_stock[l][c] =
                        (unsigned short)(city && (int)g->world.loc[l].owner == cat->faction);
                    continue;
                }
                g->airframe_stock[l][c] = city
                                               ? (unsigned short)(1 + fly_rng_range(&stock_rng, 2)) : 0;
            }
        }
    }
    {
        int ci = fly_game_airframe_catalog_find(g->owned[0].catalog_id), m;
        if (ci >= 0)
            for (m = 0; m < fly_module_count(); ++m) {
                int k, n = fly__airframes[ci].starter_spares[m];
                for (k = 0; k < n; ++k) {
                    fly_item it = fly_game_item_stock(m);
                    fly_game_item_add(g, &it);
                }
            }
    }
    fly_world_discover(&g->world, home->pos, 6000.0f);
    offer_contracts(g);
    fly_game_populate(g);
    fly_game_convoys_populate(g);
    fly_game_log(g, "Docked at %s. Fly safe.", home->name);
}

/* ---------------- commands ---------------- */

static int cmd_buy(fly_game *g, int res, float qty, int selling) {
    if (g->docked < 0 || g->mode == FLY_MODE_RAIL ||
        (g->mode == FLY_MODE_WALK && dist2d(g->walker.pos, g->world.loc[g->docked].pos) > FLY_DOCK_RANGE) ||
        res < 0 || res >= FLY_RES_COUNT || qty <= 0) return -1;
    fly_location *L = &g->world.loc[g->docked];
    /* Both sides of the counter feel the flag, and in opposite directions:
     * you buy dear and sell cheap where you are unwelcome. One markup, applied
     * as a multiplier going out and a divisor coming in, so a round trip
     * through a hostile field is a loss rather than a wash. */
    float markup = fly_game_faction_markup(g, g->docked);
    float px = fly_world_price(&g->world, g->docked, (fly_resource)res);
    if (selling) {
        if (g->player.cargo[res] < qty) return -2;
        long gain = (long)(px * qty * 0.92f / markup); /* market takes a cut */
        g->player.cargo[res] -= qty;
        L->stock[res] += qty;
        g->player.tokens += gain;
        /* Turning stock over at somebody's field is business done with them,
         * and business is how standing is actually earned — the contract board
         * is the fast lane, not the only one. */
        fly__trade_credit(g, gain);
        fly_game_log(g, "Sold %.0f %s: +%ld tk", qty, fly_resource_name((fly_resource)res), gain);
        return 0;
    }
    if (L->stock[res] < qty) return -3;
    long cost = (long)(px * qty * markup) + 1;
    if (g->player.tokens < cost) return -4;
    float mass = fly_game_cargo_mass(g) + qty * fly_resource_mass((fly_resource)res);
    if (mass > g->player.airframe.cargo_cap) return -5;
    g->player.tokens -= cost;
    L->stock[res] -= qty;
    g->player.cargo[res] += qty;
    fly__trade_credit(g, cost / 2);
    fly_game_log(g, "Bought %.0f %s: -%ld tk", qty, fly_resource_name((fly_resource)res), cost);
    return 0;
}

static int cmd_accept(fly_game *g, int slot) {
    if (slot < 0 || slot >= FLY_MAX_CONTRACTS) return -1;
    fly_contract *ct = &g->contracts[slot];
    if (ct->state != 1 || g->docked != ct->from || g->mode == FLY_MODE_RAIL ||
        (g->mode == FLY_MODE_WALK && dist2d(g->walker.pos, g->world.loc[g->docked].pos) > FLY_DOCK_RANGE)) return -2;
    float mass = fly_game_cargo_mass(g) + ct->qty * fly_resource_mass((fly_resource)ct->res);
    if (mass > g->player.airframe.cargo_cap) { fly_game_log(g, "Too heavy: need racks or a lighter hold"); return -3; }
    /* Do not take work that cannot be delivered.
     *
     * Gated sites pay a premium and that premium is the reason to want a VTOL
     * or a high-altitude kit — but a contract to one is not a stretch goal, it
     * is a trap: the cargo loads, the deadline runs, and the only outcome is an
     * expiry and the reputation that goes with it. The very first job a new
     * operator was offered on seed 4242 was exactly this. The offer stays on
     * the board; it becomes acceptable the moment the hangar can honour it. */
    {
        uint32_t gate = fly_world_gate_check(&g->world, ct->to, &g->player.airframe);
        if (gate) {
            fly_game_log(g, "%s needs %s: cannot deliver that",
                         g->world.loc[ct->to].name, fly_gate_name(gate));
            return -4;
        }
    }
    if (fly_game_service_ok(g, g->docked) != 0) {
        fly_game_log(g, "The %s board is closed to you here.",
                     fly_faction_name(fly_game_site_owner(g, g->docked)));
        return -5;
    }
    ct->state = 2;
    ct->cargo_loaded = 1;
    g->player.cargo[ct->res] += ct->qty;
    if (!g->world.loc[ct->to].discovered) {
        g->world.loc[ct->to].discovered = 1;
        fly_game_log(g, "Rumored site marked: %s", g->world.loc[ct->to].name);
    }
    fly_game_log(g, "Contract: %.0f %s to %s (%ld tk)", ct->qty,
                 fly_resource_name((fly_resource)ct->res), g->world.loc[ct->to].name, ct->reward);
    return 0;
}

static int cmd_refuel(fly_game *g) {
    if (g->docked < 0 || g->mode != FLY_MODE_FLIGHT) return -1;
    fly_location *L = &g->world.loc[g->docked];
    float need = g->player.airframe.fuel_cap - g->player.craft.fuel;
    if (need <= 0.5f) return 0;
    float avail = L->stock[FLY_RES_FUEL];
    float qty = need < avail ? need : avail;
    if (qty < 0.5f) { fly_game_log(g, "%s has no fuel in stock", L->name); return -2; }
    long cost = (long)(qty * fly_world_price(&g->world, g->docked, FLY_RES_FUEL)) + 1;
    if (g->player.tokens < cost) {
        qty = qty * (float)g->player.tokens / (float)cost;
        cost = g->player.tokens;
        if (qty < 0.5f) return -3;
    }
    g->player.tokens -= cost;
    L->stock[FLY_RES_FUEL] -= qty;
    g->player.craft.fuel += qty;
    fly_game_log(g, "Refueled %.0f: -%ld tk", qty, cost);
    return 0;
}

static void fly__sync_active(fly_game *g);

/* Fill the magazines.
 *
 * Priced in alloy rather than in fuel, and deliberately: a round is metal and
 * explosive that somebody had to make, so rearming competes with building
 * hulls for the same commodity, and a player who has spent an afternoon
 * shooting has an afternoon's worth of alloy to buy back. It is refused where
 * repair is refused and for the same reason — a power at war with you will not
 * hand you ordnance — while fuel is never refused, because stranding somebody
 * at a hostile field is a fail state they cannot see coming.
 *
 * Partial fills are legal and are what makes the sink bite: an operator who can
 * afford six flares gets six flares and has to decide whether that is enough to
 * go back out on. */
static int cmd_rearm(fly_game *g) {
    int want_ord, want_cm, got_ord = 0, got_cm = 0;
    float alloy_have, alloy_need, alloy_used;
    long labor;
    if (g->docked < 0 || g->mode != FLY_MODE_FLIGHT) return -1;
    if (fly_game_service_ok(g, g->docked) != 0) {
        fly_game_log(g, "%s will not arm you.", g->world.loc[g->docked].name);
        return -4;
    }
    want_ord = g->player.airframe.ord_ammo - g->player.craft.ammo;
    want_cm = g->player.airframe.cm_ammo - g->player.craft.cm;
    if (want_ord < 0) want_ord = 0;
    if (want_cm < 0) want_cm = 0;
    if (want_ord + want_cm <= 0) return 0;
    /* A round of ordnance is worth four countermeasures: a warhead is a
     * different kind of object from a magnesium candle, and pricing them the
     * same made flares the expensive part of a loadout that is mostly flares. */
    alloy_need = (float)want_ord * 0.8f + (float)want_cm * 0.2f;
    alloy_have = g->player.cargo[FLY_RES_ALLOY];
    alloy_used = alloy_need < alloy_have ? alloy_need : alloy_have;
    labor = (long)((alloy_need - alloy_used) * 26.0f) + 8;
    if (g->player.tokens < labor) {
        /* Buy what the money reaches rather than refusing outright, so being
         * short is a smaller magazine and not a locked counter. */
        float afford = alloy_used + (float)g->player.tokens / 26.0f;
        if (afford < 0.2f) return -2;
        alloy_need = afford;
        alloy_used = alloy_need < alloy_have ? alloy_need : alloy_have;
        labor = (long)((alloy_need - alloy_used) * 26.0f);
    }
    {
        float share = alloy_need > 0.001f ? alloy_need : 1.0f;
        float unit = (float)want_ord * 0.8f + (float)want_cm * 0.2f;
        float frac = unit > 0.001f ? fly_clampf(share / unit, 0.0f, 1.0f) : 0.0f;
        got_ord = (int)((float)want_ord * frac);
        got_cm = (int)((float)want_cm * frac);
        /* Anything at all buys at least one of whichever is short, so a nearly
         * broke operator leaves with a round rather than with a rounding
         * error. */
        if (got_ord == 0 && got_cm == 0 && frac > 0.0f) {
            if (want_cm > 0) got_cm = 1; else got_ord = 1;
        }
    }
    g->player.cargo[FLY_RES_ALLOY] -= alloy_used;
    if (g->player.cargo[FLY_RES_ALLOY] < 0.0f) g->player.cargo[FLY_RES_ALLOY] = 0.0f;
    g->player.tokens -= labor;
    g->player.craft.ammo += got_ord;
    g->player.craft.cm += got_cm;
    fly__sync_active(g);
    if (g->player.airframe.ord_kind && g->player.airframe.cm_kind)
        fly_game_log(g, "Rearmed: +%d %s, +%d %s (-%.0f alloy, -%ld tk)",
                     got_ord, fly_ord_kind_name(g->player.airframe.ord_kind),
                     got_cm, fly_ord_kind_name(g->player.airframe.cm_kind), alloy_used, labor);
    else if (g->player.airframe.ord_kind)
        fly_game_log(g, "Rearmed: +%d %s (-%.0f alloy, -%ld tk)", got_ord,
                     fly_ord_kind_name(g->player.airframe.ord_kind), alloy_used, labor);
    else
        fly_game_log(g, "Rearmed: +%d %s (-%.0f alloy, -%ld tk)", got_cm,
                     fly_ord_kind_name(g->player.airframe.cm_kind), alloy_used, labor);
    return 0;
}

static int cmd_repair(fly_game *g) {
    if (g->docked < 0 || g->mode != FLY_MODE_FLIGHT) return -1;
    /* A shop that is at war with you does not lend you its spanners. Fuel it
     * will still sell — see fly_game_service_ok for why that one is never
     * refused — so a hostile field is a bad place to be, not a trap. */
    if (fly_game_service_ok(g, g->docked) != 0) {
        fly_game_log(g, "%s will not work on your aircraft.",
                     g->world.loc[g->docked].name);
        return -4;
    }
    float wear = g->player.craft.wear;
    float dmg = 1.0f - g->player.craft.hp / g->player.airframe.structure;
    if (wear < 0.02f && dmg < 0.02f) return 0;
    /* each spare part fixes a chunk; remainder billed as shop labor */
    float parts_needed = ceilf((wear + dmg) * 6.0f);
    float parts_have = g->player.cargo[FLY_RES_PARTS];
    float parts_used = parts_needed < parts_have ? parts_needed : parts_have;
    long labor = (long)((parts_needed - parts_used) * 30.0f) + 15;
    if (g->player.tokens < labor) return -2;
    g->player.tokens -= labor;
    g->player.cargo[FLY_RES_PARTS] -= parts_used;
    g->player.craft.wear = 0.0f;
    g->player.craft.hp = g->player.airframe.structure;
    /* Deliberately does not touch fatigue: a shop restores integrity, it does
     * not give an airframe its life back. This is the ratchet. */
    fly_game_log(g, "Repaired (%.0f parts, %ld tk labor)", parts_used, labor);
    return 0;
}

static int fly__service_site(const fly_game *g) {
    if (g->docked < 0) return 0;
    if (g->mode == FLY_MODE_WALK && dist2d(g->walker.pos, g->world.loc[g->docked].pos) > FLY_DOCK_RANGE)
        return 0;
    if (g->mode == FLY_MODE_RAIL) return 0;
    return g->world.loc[g->docked].kind == FLY_LOC_CITY ||
           g->world.loc[g->docked].kind == FLY_LOC_SKYPORT;
}

/* A magazine can never hold more than the rack it is in.
 *
 * Called wherever the fitted loadout changes, because that is exactly when the
 * capacity moves: unfitting a forty-flare suite and fitting a twenty-four-round
 * pod has to leave the aeroplane with twenty-four, and an aeroplane with no
 * dispenser at all has to leave with none. Without this the counters were free
 * storage — fit the big one, load it, fit the small one back, and the rounds
 * stayed. */
static void fly__clamp_magazines(fly_game *g) {
    if (g->player.craft.ammo > g->player.airframe.ord_ammo) g->player.craft.ammo = g->player.airframe.ord_ammo;
    if (g->player.craft.cm > g->player.airframe.cm_ammo) g->player.craft.cm = g->player.airframe.cm_ammo;
    if (g->player.craft.ammo < 0) g->player.craft.ammo = 0;
    if (g->player.craft.cm < 0) g->player.craft.cm = 0;
}

static void fly__sync_active(fly_game *g) {
    fly_owned_airframe *o = fly__active(g);
    if (!o) return;
    o->fuel = g->player.craft.fuel;
    o->ammo = g->player.craft.ammo;
    o->cm = g->player.craft.cm;
    o->wear = g->player.craft.wear;
    o->condition = g->player.airframe.structure > 0.0f
                       ? fly_clampf(g->player.craft.hp / g->player.airframe.structure, 0.0f, 1.0f) : 0.0f;
    if (g->docked >= 0) o->location = g->docked;
}

/* `uid` names a particular item; a caller that only knows a base type (the
 * shop list does) passes 0 for the uid and the base index instead, and gets
 * the best one it owns. */
static int fly__fit(fly_game *g, uint32_t uid, int base, int airframe) {
    const fly_item *it;
    const fly_module *m;
    if (!fly__service_site(g) || airframe < 0 || airframe >= g->owned_count) return -1;
    if (!uid) uid = fly__best_held(g, base);
    it = fly_game_item(g, uid);
    m = it ? fly_module_get(it->base) : NULL;
    if (!m) return -5;
    fly_owned_airframe *o = &g->owned[airframe];
    int ci = fly_game_airframe_catalog_find(o->catalog_id);
    if (o->status != FLY_AIRFRAME_USABLE || ci < 0 || o->location != g->docked) return -2;
    if (m->craft_kind >= 0 && (fly_craft_kind)m->craft_kind != fly__airframes[ci].base.kind) return -4;
    if (fly__item_fitted_to(g, uid) >= 0) return -5;   /* already on something */
    if (o->fitted[m->slot] == uid) return -3;
    o->fitted[m->slot] = uid;
    if (airframe == g->active_airframe) {
        float ff = g->player.airframe.fuel_cap > 0 ? g->player.craft.fuel / g->player.airframe.fuel_cap : 0;
        float hf = g->player.airframe.structure > 0 ? g->player.craft.hp / g->player.airframe.structure : 0;
        fly_game_rebuild_airframe(g);
        g->player.craft.fuel = ff * g->player.airframe.fuel_cap;
        g->player.craft.hp = hf * g->player.airframe.structure;
        fly__clamp_magazines(g);
        fly__sync_active(g);
    }
    {
        char nm[64];
        fly_item_name(it, nm, sizeof nm);
        fly_game_log(g, "Fitted %s to #%u", nm, o->instance_id);
    }
    return 0;
}

static int fly__unfit(fly_game *g, int slot, int airframe) {
    if (!fly__service_site(g) || slot < 0 || slot >= FLY_SLOT_COUNT ||
        airframe < 0 || airframe >= g->owned_count) return -1;
    fly_owned_airframe *o = &g->owned[airframe];
    uint32_t uid = o->fitted[slot];
    const fly_item *it = fly_game_item(g, uid);
    if (o->status != FLY_AIRFRAME_USABLE || o->location != g->docked || !it) return -2;
    float ff = g->player.airframe.fuel_cap > 0 ? g->player.craft.fuel / g->player.airframe.fuel_cap : 0;
    float hf = g->player.airframe.structure > 0 ? g->player.craft.hp / g->player.airframe.structure : 0;
    o->fitted[slot] = 0;
    if (airframe == g->active_airframe) {
        fly_game_rebuild_airframe(g);
        if (fly_game_cargo_mass(g) > g->player.airframe.cargo_cap) {
            o->fitted[slot] = uid;
            fly_game_rebuild_airframe(g);
            return -4;
        }
        g->player.craft.fuel = ff * g->player.airframe.fuel_cap;
        g->player.craft.hp = hf * g->player.airframe.structure;
        fly__clamp_magazines(g);
    }
    {
        char nm[64];
        fly_item_name(fly_game_item(g, uid), nm, sizeof nm);
        fly_game_log(g, "Unfitted %s", nm);
    }
    return 0;
}

/* `a` is the shelf position at this site. A shelf holds particular items, so
 * "buy a Gun Pod" is not a thing a player can ask for any more — they buy the
 * one they are looking at. */
/* Will they sell it to you?
 *
 * Open kit: always. A power's own: only if that power thinks well enough of
 * you — and being sworn to them is not required, which is deliberate. An oath
 * gets you there faster and standing is what actually buys the part, so a
 * player who has spent a month flying Foundry ore can buy a Kiln engine
 * without swearing to anybody, and a player who swore this morning cannot.
 * Logged rather than silently unavailable, because a row that is greyed out
 * for a reason nobody states reads as a bug. */
static int fly__kit_refused(fly_game *g, int faction) {
    if (!fly_faction_is_power(faction)) return 0;
    if (g->standing[faction] >= FLY_STAND_KIT) return 0;
    fly_game_log(g, "%s kit is for %s hands: standing %d of %d",
                 fly_faction_tag(faction), fly_faction_name(faction),
                 g->standing[faction], FLY_STAND_KIT);
    return 1;
}

static int fly__buy_module(fly_game *g, int shelf) {
    fly_item *on;
    long price;
    char nm[64];
    if (!fly__service_site(g) || shelf < 0 || shelf >= FLY_SITE_STOCK_MAX) return -1;
    on = &g->stock[g->docked][shelf];
    if (!on->uid || !fly_module_get(on->base)) return -2;
    if (fly__kit_refused(g, fly_module_get(on->base)->faction)) return -5;
    if (g->item_count >= FLY_ITEM_MAX) { fly_game_log(g, "The hold is full"); return -4; }
    price = fly_game_item_price(g, g->docked, on);
    if (g->player.tokens < price) return -3;
    fly_item_name(on, nm, sizeof nm);
    g->player.tokens -= price;
    fly_game_item_add(g, on);
    memset(on, 0, sizeof *on);
    fly_game_log(g, "Bought %s (-%ld tk)", nm, price);
    return 0;
}

/* Liquidation. The gap between this and fly__buy_module is what makes losing an
 * aircraft cost something instead of being a round trip through inventory, and
 * it is the only way a hull-less player turns a shelf of modules back into the
 * price of an airframe. */
/* `a` is a uid, or 0 with `base` set to liquidate the *worst* loose one of a
 * base — which is what a player clearing the hold from a list of types means. */
static int fly__sell_module(fly_game *g, uint32_t uid, int base) {
    const fly_item *it;
    long price;
    char nm[64];
    int shelf;
    if (!fly__service_site(g)) return -1;
    if (!uid) {
        long worst = -1;
        int i;
        for (i = 0; i < g->item_count; ++i) {
            long v;
            if (g->items[i].base != base) continue;
            if (fly__item_fitted_to(g, g->items[i].uid) >= 0) continue;
            v = fly_item_value(&g->items[i]);
            if (worst < 0 || v < worst) { worst = v; uid = g->items[i].uid; }
        }
    }
    it = fly_game_item(g, uid);
    if (!it) return -2;
    if (fly__item_fitted_to(g, uid) >= 0) return -3;   /* unfit it first */
    price = (long)((float)fly_game_item_price(g, g->docked, it) * FLY_RESALE);
    if (price < 1) price = 1;
    fly_item_name(it, nm, sizeof nm);
    /* it goes on the shelf, where it can be bought back at the full price —
       the gap between the two is what makes losing an aircraft cost something */
    for (shelf = 0; shelf < FLY_SITE_STOCK_MAX; ++shelf)
        if (!g->stock[g->docked][shelf].uid) {
            g->stock[g->docked][shelf] = *it;
            break;
        }
    fly_game_item_remove(g, uid);
    g->player.tokens += price;
    fly_game_log(g, "Sold %s (+%ld tk)", nm, price);
    return 0;
}


/* --- the surplus sink ---
 *
 * A hold that only ever fills up turns loot into bookkeeping. By the time an
 * operator has flown a few contracts they have four engines they will never
 * fit, and selling them back is a price rather than a decision — the tokens
 * are fungible and the choice is not interesting.
 *
 * Three things to do with them instead, all at a service site, all costing
 * something that is not only money:
 *
 *  - salvage breaks an item into spare parts, which is the resource repairs
 *    and new hulls already eat, so surplus kit becomes airframe life;
 *  - reroll pays to redraw an item's rolls at its own tier and level, which is
 *    what you reach for when the tier is right and the numbers are not;
 *  - uprate eats three loose items of the same slot to push one a tier, which
 *    is the only thing that makes a shelf of near-misses worth keeping.
 *
 * Uprating stops below the top on purpose. A One-off is not something a
 * workshop makes; it is something somebody was carrying, and the only way to
 * one is still to take it off them.
 */

static long fly__item_parts(const fly_item *it) {
    long v = fly_item_value(it) / 55;
    return v < 1 ? 1 : v > 14 ? 14 : v;
}

int fly_game_salvage_yield(const fly_game *g, uint32_t uid, float *parts, long *tokens) {
    const fly_item *it = fly_game_item(g, uid);
    float p;
    if (!fly__service_site(g)) return -1;
    if (!it) return -2;
    if (fly_game_item_fitted(g, uid)) return -3;
    p = (float)fly__item_parts(it);
    if (parts) *parts = p;
    if (tokens) *tokens = 0;
    /* Parts have mass and the hold has a limit; breaking something up must not
     * quietly leave an aeroplane that cannot legally take off. */
    if (fly_game_cargo_mass(g) + p * fly_resource_mass(FLY_RES_PARTS) >
        g->player.airframe.cargo_cap) return -4;
    return 0;
}

static int fly__salvage(fly_game *g, uint32_t uid, int base) {
    const fly_item *it;
    float parts = 0.0f;
    char nm[64];
    int rc;
    if (!uid) uid = fly__worst_held(g, base);
    rc = fly_game_salvage_yield(g, uid, &parts, NULL);
    if (rc != 0) {
        if (rc == -4) fly_game_log(g, "No room in the hold for the parts");
        return rc;
    }
    it = fly_game_item(g, uid);
    fly_item_name(it, nm, sizeof nm);
    g->player.cargo[FLY_RES_PARTS] += parts;
    fly_game_item_remove(g, uid);
    fly_game_log(g, "Broke up %s for %.0f parts", nm, parts);
    return 0;
}

int fly_game_reroll_cost(const fly_game *g, uint32_t uid, float *parts, long *tokens) {
    const fly_item *it = fly_game_item(g, uid);
    float p;
    long tk;
    if (!fly__service_site(g)) return -1;
    if (!it) return -2;
    if (fly_game_item_fitted(g, uid)) return -3;
    if (it->naff == 0) return -5;          /* nothing to redraw */
    p = 2.0f + (float)it->rarity * 2.0f;
    tk = fly_item_value(it) / 2 + 40;
    if (parts) *parts = p;
    if (tokens) *tokens = tk;
    if (g->player.cargo[FLY_RES_PARTS] < p) return -6;
    if (g->player.tokens < tk) return -7;
    return 0;
}

static int fly__reroll(fly_game *g, uint32_t uid, int base) {
    fly_item *it;
    float parts = 0.0f;
    long tk = 0;
    char before[64], after[64];
    int idx, rc;
    if (!uid) uid = fly__best_held(g, base);
    rc = fly_game_reroll_cost(g, uid, &parts, &tk);
    if (rc != 0) return rc;
    idx = fly__item_index(g, uid);
    if (idx < 0) return -2;
    it = &g->items[idx];
    fly_item_name(it, before, sizeof before);
    g->player.cargo[FLY_RES_PARTS] -= parts;
    g->player.tokens -= tk;
    {
        /* Same base, same tier, same level: only the numbers move. Drawn from
           the world stream because it is the consequence of a command, and
           commands are what a replay carries. */
        fly_item fresh = fly_game_roll_item(it->base, it->ilvl, 0.0f, &g->rng);
        int want = it->naff;
        it->seed = (unsigned char)fly_rng_range(&g->rng, 256);
        it->naff = 0;
        {
            /* the fresh roll may have fewer affixes than the tier owes, so
               top it up from its own pool rather than shipping a downgrade */
            int guard = 0;
            while (it->naff < want && guard++ < 8) {
                fly_item more = fly_game_roll_item(it->base, it->ilvl, 0.0f, &g->rng);
                int k;
                for (k = 0; k < more.naff && it->naff < want; ++k) {
                    int dup = 0, q;
                    for (q = 0; q < it->naff; ++q)
                        if (it->aff[q].kind == more.aff[k].kind) dup = 1;
                    if (!dup) it->aff[it->naff++] = more.aff[k];
                }
                for (k = 0; k < fresh.naff && it->naff < want; ++k) {
                    int dup = 0, q;
                    for (q = 0; q < it->naff; ++q)
                        if (it->aff[q].kind == fresh.aff[k].kind) dup = 1;
                    if (!dup) it->aff[it->naff++] = fresh.aff[k];
                }
            }
        }
    }
    fly_item_name(it, after, sizeof after);
    fly_game_log(g, "Reworked %s -> %s (-%ld tk, -%.0f parts)", before, after, tk, parts);
    return 0;
}

/* Loose items of `slot` at `floor` tier or better, excluding `keep`. */
static int fly__fodder(const fly_game *g, int slot, int floor_tier, uint32_t keep,
                       uint32_t *out, int max) {
    int i, n = 0;
    for (i = 0; i < g->item_count && n < max; ++i) {
        const fly_item *x = &g->items[i];
        const fly_module *m = fly_module_get(x->base);
        if (!m || m->slot != slot) continue;
        if (x->uid == keep || fly_game_item_fitted(g, x->uid)) continue;
        if ((int)x->rarity < floor_tier) continue;
        if (out) out[n] = x->uid;
        ++n;
    }
    return n;
}

int fly_game_uprate_cost(const fly_game *g, uint32_t uid, int *fodder, long *tokens) {
    const fly_item *it = fly_game_item(g, uid);
    const fly_module *m;
    long tk;
    int have;
    if (!fly__service_site(g)) return -1;
    if (!it) return -2;
    if (fly_game_item_fitted(g, uid)) return -3;
    m = fly_module_get(it->base);
    if (!m) return -2;
    if (it->rarity >= FLY_UPRATE_CEILING) return -5;   /* the top is not for sale */
    tk = fly_item_value(it) + 120;
    have = fly__fodder(g, m->slot, (int)it->rarity, uid, NULL, FLY_UPRATE_FODDER);
    if (fodder) *fodder = have;
    if (tokens) *tokens = tk;
    if (have < FLY_UPRATE_FODDER) return -6;
    if (g->player.tokens < tk) return -7;
    return 0;
}

static int fly__uprate(fly_game *g, uint32_t uid, int base) {
    uint32_t eat[FLY_UPRATE_FODDER];
    const fly_module *m;
    fly_item *it;
    long tk = 0;
    char before[64], after[64];
    int idx, rc, i, want;
    if (!uid) uid = fly__best_held(g, base);
    rc = fly_game_uprate_cost(g, uid, NULL, &tk);
    if (rc != 0) return rc;
    idx = fly__item_index(g, uid);
    if (idx < 0) return -2;
    it = &g->items[idx];
    m = fly_module_get(it->base);
    if (fly__fodder(g, m->slot, (int)it->rarity, uid, eat, FLY_UPRATE_FODDER) <
        FLY_UPRATE_FODDER) return -6;
    fly_item_name(it, before, sizeof before);
    g->player.tokens -= tk;
    for (i = 0; i < FLY_UPRATE_FODDER; ++i) fly_game_item_remove(g, eat[i]);
    idx = fly__item_index(g, uid);          /* the array compacted under us */
    if (idx < 0) return -2;
    it = &g->items[idx];
    ++it->rarity;
    /* a tier is an affix, so the new one is drawn from the base's own pool */
    want = it->naff + 1;
    {
        int guard = 0;
        /* Rising quality, and that is a fix rather than a flourish. Drawing the
         * filler at quality zero means most draws come back Stock, and a Stock
         * roll carries no affixes at all — so the loop could spin its guard out
         * and hand back an uprated item with the affix count it started with.
         * It only ever showed up as a flake: the odds of eight Stock draws in a
         * row are low enough that the gate passed for months and then failed
         * because an unrelated change moved the shared rng along by one. Ramp
         * the quality with the attempt and the last try cannot be empty. */
        while (it->naff < want && it->naff < FLY_ITEM_AFFIX_MAX && guard < 8) {
            fly_item more = fly_game_roll_item(it->base, it->ilvl,
                                               (float)guard / 7.0f, &g->rng);
            ++guard;
            int k;
            for (k = 0; k < more.naff && it->naff < want; ++k) {
                int dup = 0, q;
                for (q = 0; q < it->naff; ++q)
                    if (it->aff[q].kind == more.aff[k].kind) dup = 1;
                if (!dup) it->aff[it->naff++] = more.aff[k];
            }
        }
    }
    fly_item_name(it, after, sizeof after);
    fly_game_log(g, "Uprated %s -> %s (-%ld tk, -%d parts bin)", before, after, tk,
                 FLY_UPRATE_FODDER);
    return 0;
}

static int fly__create_airframe(fly_game *g, int catalog) {
    const fly_airframe_catalog *c = fly_game_airframe_catalog_get(catalog);
    if (!c || g->owned_count >= FLY_OWNED_AIRFRAME_MAX || g->docked < 0) return -1;
    fly_owned_airframe *o = &g->owned[g->owned_count++];
    memset(o, 0, sizeof *o);
    o->instance_id = g->next_airframe_instance_id++;
    snprintf(o->catalog_id, sizeof o->catalog_id, "%s", c->id);
    o->status = FLY_AIRFRAME_USABLE;
    o->location = g->docked;
    o->condition = 1.0f;
    o->fuel = c->base.fuel_cap;
    /* Armed as it leaves the hangar, the way it leaves with full tanks. The
       ceiling is whatever ends up fitted, and fly__clamp_magazines applies it
       the moment the hull is selected — a bare airframe therefore starts with
       nothing, which is the correct answer for one with no racks on it. */
    o->ammo = o->cm = 9999;
    int s;
    for (s = 0; s < FLY_SLOT_COUNT; ++s) o->fitted[s] = 0;
    return g->owned_count - 1;
}

static int fly__buy_airframe(fly_game *g, int catalog) {
    if (!fly__service_site(g) || !fly_game_airframe_catalog_get(catalog)) return -1;
    if (!g->airframe_stock[g->docked][catalog]) return -2;
    if (fly__kit_refused(g, fly_game_airframe_catalog_get(catalog)->faction)) return -5;
    long price = fly_game_airframe_price(g, g->docked, catalog);
    if (g->player.tokens < price) return -3;
    int idx = fly__create_airframe(g, catalog);
    if (idx < 0) return -4;
    g->player.tokens -= price;
    --g->airframe_stock[g->docked][catalog];
    fly_game_log(g, "Bought %s #%u", fly__airframes[catalog].name, g->owned[idx].instance_id);
    return 0;
}

static int fly__build_airframe(fly_game *g, int catalog) {
    const fly_airframe_catalog *c = fly_game_airframe_catalog_get(catalog);
    if (!fly__service_site(g) || !c) return -1;
    if (g->player.cargo[FLY_RES_ALLOY] < c->build_alloy || g->player.cargo[FLY_RES_PARTS] < c->build_parts) return -2;
    int idx = fly__create_airframe(g, catalog);
    if (idx < 0) return -3;
    g->player.cargo[FLY_RES_ALLOY] -= c->build_alloy;
    g->player.cargo[FLY_RES_PARTS] -= c->build_parts;
    fly_game_log(g, "Built %s #%u", c->name, g->owned[idx].instance_id);
    return 0;
}

static int fly__select_airframe(fly_game *g, int idx) {
    if (!fly__service_site(g) || idx < 0 || idx >= g->owned_count) return -1;
    fly_owned_airframe *o = &g->owned[idx];
    if (o->status != FLY_AIRFRAME_USABLE || o->location != g->docked) return -2;
    fly__sync_active(g);
    int previous = g->active_airframe;
    fly_airframe previous_af = g->player.airframe;
    fly_craft previous_craft = g->player.craft;
    g->active_airframe = idx;
    if (fly_game_rebuild_airframe(g) != 0) return -3;
    fly_v3 pos = fly_wpos_at(g->world.loc[g->docked].pos,
                             g->world.loc[g->docked].pad_z + g->player.airframe.gear_height);
    fly_craft_init(&g->player.craft, &g->player.airframe, pos);
    g->player.craft.fuel = fly_clampf(o->fuel, 0, g->player.airframe.fuel_cap);
    g->player.craft.ammo = o->ammo;
    g->player.craft.cm = o->cm;
    fly__clamp_magazines(g);
    g->player.craft.wear = o->wear;
    g->player.craft.hp = o->condition * g->player.airframe.structure;
    if (fly_game_cargo_mass(g) > g->player.airframe.cargo_cap) {
        g->active_airframe = previous;
        g->player.airframe = previous_af;
        g->player.craft = previous_craft;
        return -4;
    }
    g->crash_handled = 0;
    fly_game_log(g, "Selected %s #%u", g->player.airframe.name, o->instance_id);
    return 0;
}

static int cmd_upgrade(fly_game *g, int idx) {
    const fly_module *m = fly_module_get(idx);
    if (!fly__service_site(g) || !m) return -1;
    if (!fly__service_site(g)) {
        fly_game_log(g, "Modules fit only at cities/skyports");
        return -2;
    }
    fly_owned_airframe *o = fly__active(g);
    if (!o) return -1;
    if (m->craft_kind >= 0 && (fly_craft_kind)m->craft_kind != g->player.airframe.kind) {
        fly_game_log(g, "%s does not fit this airframe", m->name);
        return -4;
    }
    const fly_item *oldit = fly_game_item(g, o->fitted[m->slot]);
    const fly_module *old = oldit ? fly_module_get(oldit->base) : NULL;
    if (oldit && oldit->base == idx) return -3; /* already fitted */
    long net = m->cost - (old ? old->cost / 2 : 0); /* trade-in at half value */
    if (g->player.tokens < net) return -5;
    if (g->item_count >= FLY_ITEM_MAX) return -6;
    g->player.tokens -= net;
    {
        fly_item it = fly_game_item_stock(idx);
        o->fitted[m->slot] = fly_game_item_add(g, &it);
    }
    float fuel_frac = g->player.craft.fuel / g->player.airframe.fuel_cap;
    float hp_frac = g->player.craft.hp / g->player.airframe.structure;
    fly_game_rebuild_airframe(g);
    g->player.craft.fuel = fuel_frac * g->player.airframe.fuel_cap;
    g->player.craft.hp = hp_frac * g->player.airframe.structure;
    fly__clamp_magazines(g);
    if (old) fly_game_log(g, "Swapped %s for %s (-%ld tk)", old->name, m->name, net);
    else fly_game_log(g, "Fitted %s [%s] (-%ld tk)", m->name, fly_slot_name(m->slot), net);
    return 0;
}

static int cmd_switch(fly_game *g, int kind) {
    if (g->docked < 0 || g->world.loc[g->docked].kind != FLY_LOC_CITY) return -1;
    int ci = fly_game_airframe_catalog_find(kind ? "dragonfly" : "skylark");
    int i;
    for (i = 0; i < g->owned_count; ++i)
        if (fly_game_airframe_catalog_find(g->owned[i].catalog_id) == ci &&
            g->owned[i].status == FLY_AIRFRAME_USABLE && g->owned[i].location == g->docked)
            return fly__select_airframe(g, i);
    if (fly__buy_airframe(g, ci) != 0) return -2;
    return fly__select_airframe(g, g->owned_count - 1);
}

static int cmd_launch(fly_game *g) {
    fly_owned_airframe *o = fly__active(g);
    if (g->docked < 0 || g->mode != FLY_MODE_FLIGHT || !o || o->status != FLY_AIRFRAME_USABLE) return -1;
    if (fly_game_cargo_mass(g) > g->player.airframe.cargo_cap) return -2;
    if (g->player.craft.fuel < g->player.airframe.fuel_cap * 0.04f)
        fly_game_log(g, "Warning: launching on fumes");
    g->launch_site = g->docked;
    g->launch_pos = g->player.craft.pos;
    g->crash_handled = 0;
    g->docked = -1;
    g->launch_guard = 1;
    g->player.craft.on_ground = 1; /* rolls/lifts off the pad */
    fly_game_log(g, "Airborne from %s", g->world.loc[fly_world_nearest(&g->world,
                 fly_wpos_of(g->player.craft.pos), 1)].name);
    return 0;
}

static int fly__collect(fly_game *g, int item_idx) {
    if (g->mode != FLY_MODE_WALK || item_idx < 0 || item_idx >= FLY_GROUND_ITEM_MAX) return -1;
    fly_ground_item *item = &g->ground_items[item_idx];
    if (!item->active || fly_v3dist(g->walker.pos, item->pos) > 2.6f) return -2;
    if (g->item_count >= FLY_ITEM_MAX) { fly_game_log(g, "The hold is full"); return -3; }
    {
        char nm[64];
        fly_item_name(&item->item, nm, sizeof nm);
        fly_game_item_add(g, &item->item);
        fly_game_log(g, "Recovered %s", nm);
    }
    memset(item, 0, sizeof *item);
    return 0;
}

static int fly__disembark(fly_game *g) {
    if (g->mode != FLY_MODE_FLIGHT || g->docked < 0) return -1;
    float yaw, roll, pitch;
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    fly_walk_init(&g->walker, fly_v3mk(g->player.craft.pos.x + 4.0f, g->player.craft.pos.y,
                                       fly_world_ground(&g->world, g->player.craft.pos.x + 4.0f, g->player.craft.pos.y)), yaw);
    g->mode = FLY_MODE_WALK;
    fly_game_log(g, "On foot at %s", g->world.loc[g->docked].name);
    return 0;
}

static int fly__board(fly_game *g) {
    if (g->mode != FLY_MODE_WALK || g->docked < 0 || fly_v3dist(g->walker.pos, g->player.craft.pos) > 12.0f) return -1;
    fly_owned_airframe *o = fly__active(g);
    if (!o || o->status != FLY_AIRFRAME_USABLE) return -2;
    g->mode = FLY_MODE_FLIGHT;
    memset(&g->walk_input, 0, sizeof g->walk_input);
    fly_game_log(g, "Boarded %s", g->player.airframe.name);
    return 0;
}

static int fly__rail_enter(fly_game *g) {
    if (g->mode != FLY_MODE_WALK) return -1;
    float d, separation;
    if (fly_rail_project(&g->rail_route, g->walker.pos, &d, &separation) != 0 || separation > 14.0f) return -2;
    memset(&g->rail, 0, sizeof g->rail);
    g->rail.route = 0;
    g->rail.distance = g->rail.entry_distance = d;
    g->rail.speed = g->rail_route.speed;
    fly_rail_eval(&g->rail_route, d, &g->rail.pos, &g->rail.tangent);
    g->walker.yaw = g->walker.pitch = 0.0f;
    g->mode = FLY_MODE_RAIL;
    fly_game_log(g, "Rail engaged: %s", g->world.loc[g->rail_route.to_location].name);
    return 0;
}

static int fly__rail_exit(fly_game *g) {
    if (g->mode != FLY_MODE_RAIL || !g->rail.can_exit) return -1;
    fly_v3 p = g->rail.pos;
    float offsets[] = { 5, -5, 10, -10, 18, -18 };
    int i;
    for (i = 0; i < (int)(sizeof offsets / sizeof offsets[0]); ++i) {
        fly_v3 side = fly_v3norm(fly_v3cross(fly_v3mk(0, 0, 1), g->rail.tangent));
        fly_v3 q = fly_v3add(p, fly_v3scale(side, offsets[i]));
        if (fly_world_ground_safe(&g->world, q.x, q.y, 40.0f * FLY_DEG2RAD)) {
            q.z = fly_world_ground(&g->world, q.x, q.y);
            fly_walk_init(&g->walker, q, atan2f(g->rail.tangent.y, g->rail.tangent.x));
            g->mode = FLY_MODE_WALK;
            int near = fly_world_nearest(&g->world, fly_wpos_of(q), 0);
            g->docked = near >= 0 && dist2d(q, g->world.loc[near].pos) < FLY_DOCK_RANGE ? near : -1;
            fly_game_log(g, "Exited rail");
            return 0;
        }
    }
    return -2;
}

static int fly__apply(fly_game *g, const fly_cmd *cmd) {
    switch (cmd->kind) {
    case FLY_CMD_CONTROLS:
        if (g->mode != FLY_MODE_FLIGHT) return -1;
        g->player.controls = cmd->controls; return 0;
    case FLY_CMD_LAUNCH: return cmd_launch(g);
    case FLY_CMD_AUTOPILOT:
        if (g->mode != FLY_MODE_FLIGHT) return -3;
        if (cmd->a < 0 || cmd->a >= g->world.nloc) {
            g->autopilot = 0;
            g->autopilot_target = -1;
            fly_game_log(g, "Autopilot off");
            return 0;
        }
        if (!g->world.loc[cmd->a].discovered) return -2;
        /* Refuse a route the aircraft cannot finish, and say why.
         *
         * Whether a site will have you is knowable before takeoff — the ROUTES
         * list has always shown it — but the autopilot took the job anyway and
         * the refusal only ever surfaced from `check_docking`, which needs the
         * wheels on the ground first. An aeroplane that cannot land there never
         * gets that far, so the player was flown to a site, held over it, and
         * told nothing at all. */
        {
            uint32_t blocked = fly_world_gate_check(&g->world, cmd->a, &g->player.airframe);
            if (blocked) {
                fly_game_log(g, "%s needs %s: no route", g->world.loc[cmd->a].name,
                             fly_gate_name(blocked));
                return -3;
            }
        }
        g->autopilot = 1;
        g->autopilot_target = cmd->a;
        g->autopilot_near_s = 0.0f;
        g->autopilot_taxi_s = 0.0f;
        fly_game_log(g, "Autopilot: %s", g->world.loc[cmd->a].name);
        if (g->docked >= 0) {
            int rc = cmd_launch(g);
            if (rc != 0) { g->autopilot = 0; g->autopilot_target = -1; return rc; }
        }
        return 0;
    case FLY_CMD_ACCEPT: return cmd_accept(g, cmd->a);
    case FLY_CMD_BUY: return cmd_buy(g, cmd->a, cmd->f, 0);
    case FLY_CMD_SELL: return cmd_buy(g, cmd->a, cmd->f, 1);
    case FLY_CMD_REFUEL: return cmd_refuel(g);
    case FLY_CMD_REPAIR: return cmd_repair(g);
    case FLY_CMD_REARM: return cmd_rearm(g);
    case FLY_CMD_UPGRADE: return cmd_upgrade(g, cmd->a);
    case FLY_CMD_SWITCH: return cmd_switch(g, cmd->a);
    case FLY_CMD_BUY_MODULE: return fly__buy_module(g, cmd->a);
    case FLY_CMD_FIT_MODULE:
        return fly__fit(g, cmd->data.fit.uid, cmd->a,
                        cmd->data.fit.airframe >= 0 ? cmd->data.fit.airframe : g->active_airframe);
    case FLY_CMD_UNFIT_MODULE:
        return fly__unfit(g, cmd->a, cmd->data.fit.airframe >= 0 ? cmd->data.fit.airframe : g->active_airframe);
    case FLY_CMD_SELECT_AIRFRAME: return fly__select_airframe(g, cmd->a);
    case FLY_CMD_BUY_AIRFRAME: return fly__buy_airframe(g, cmd->a);
    case FLY_CMD_BUILD_AIRFRAME: return fly__build_airframe(g, cmd->a);
    case FLY_CMD_WALK:
        if (g->mode != FLY_MODE_WALK) return -1;
        g->walk_input.forward = cmd->data.walk.forward;
        g->walk_input.right = cmd->data.walk.right;
        g->walk_input.jump = cmd->data.walk.jump;
        return 0;
    case FLY_CMD_LOOK:
        if (g->mode == FLY_MODE_FLIGHT) return -1;
        g->walker.yaw = fly_wrap_pi(g->walker.yaw + cmd->data.look.yaw);
        g->walker.pitch = fly_clampf(g->walker.pitch + cmd->data.look.pitch, -1.45f, 1.45f);
        return 0;
    case FLY_CMD_JUMP:
        if (g->mode != FLY_MODE_WALK) return -1;
        g->walk_input.jump = cmd->a != 0; return 0;
    case FLY_CMD_COLLECT_MODULE: return fly__collect(g, cmd->a);
    case FLY_CMD_DISEMBARK: return fly__disembark(g);
    case FLY_CMD_BOARD_AIRFRAME: return fly__board(g);
    case FLY_CMD_RAIL_ENTER: return fly__rail_enter(g);
    case FLY_CMD_RAIL_EXIT: return fly__rail_exit(g);
    case FLY_CMD_SELL_MODULE: return fly__sell_module(g, cmd->data.fit.uid, cmd->a);
    case FLY_CMD_SALVAGE_MODULE: return fly__salvage(g, cmd->data.fit.uid, cmd->a);
    case FLY_CMD_REROLL_MODULE: return fly__reroll(g, cmd->data.fit.uid, cmd->a);
    case FLY_CMD_UPRATE_MODULE: return fly__uprate(g, cmd->data.fit.uid, cmd->a);
    case FLY_CMD_RESTART:
        if (!g->ruin) return -1;
        fly_game_restart(g);
        return 0;
    case FLY_CMD_PLEDGE: return fly__pledge(g, cmd->a);
    case FLY_CMD_INTERACT:
        if (g->mode == FLY_MODE_RAIL) return fly__rail_exit(g);
        if (g->mode == FLY_MODE_FLIGHT) return fly__disembark(g);
        {
            int i;
            for (i = 0; i < FLY_GROUND_ITEM_MAX; ++i)
                if (g->ground_items[i].active && fly_v3dist(g->walker.pos, g->ground_items[i].pos) < 2.6f)
                    return fly__collect(g, i);
        }
        if (fly__board(g) == 0) return 0;
        return fly__rail_enter(g);
    default: return -1;
    }
}

int fly_game_apply(fly_game *g, const fly_cmd *cmd) {
    int rc = fly__apply(g, cmd);
    /* Any command that moves money, cargo or hulls can be the last one. Free
     * unless the player is already hull-less, which is the only case it looks
     * past the first owned airframe. */
    fly__check_ruin(g);
    return rc;
}

/* ---------------- autopilot ---------------- */

/* The altitude the approach profile asks for at a point on the track: a
 * terrain-safe cruise, a glide slope onto the pad, and a floor that keeps the
 * aeroplane off whatever stands between that point and the aim point.
 *
 * A function of a position rather than of the craft, because the law below
 * evaluates it twice — here, and a second's flying further down the track — and
 * flies the difference between them. See `tgt_rate`.
 *
 * Two distances, because the track has a corner in it. `leg` is how far the
 * point is from where this leg ends — the aim point the approach is being flown
 * to, which is the pad on a straight-in approach and a point out on the chosen
 * final on an offset one — and `path` is that plus whatever final is left
 * beyond it. The scan is `leg`'s, because that is the ground about to go under
 * the aeroplane; the slope is `path`'s, because that is how much flying there
 * is left to lose the height in. On a straight-in approach they are the same
 * number and this is the profile it always was.
 *
 * The scan looks at the ground *between here and there*, not at the one point
 * twelve hundred metres ahead this used to sample. A single tap is twelve
 * seconds of warning at cruise, which is not enough to climb over anything, and
 * it cannot see a ridge standing behind a nearer dip at all. The AI pilots were
 * given a nine-sample scan out to 4.5 km when they were found flying into the
 * hill in front of a valley pad; the autopilot the player actually uses was left
 * on the single tap, and the first world that put a ridge on a final approach
 * wrote off a loaded aeroplane on the way into Skythan.
 *
 * It runs as far as the aim point, which is where the profile means to have the
 * wheels down — not 800 m short of the pad, which is where it used to stop.
 * Stopping short was meant to keep the scan off the pad's own apron, on the
 * theory that finding it would hold the aeroplane sixty metres over the thing it
 * is trying to land on. That failure is real, but the cutoff is not what
 * prevents it — the clearance is, by going to nothing as the aim point arrives.
 * What the cutoff actually did was switch the terrain protection off for the
 * last eight hundred metres of every approach in the game, and at a pad in a
 * valley that is exactly the eight hundred metres that matters. */
static float autopilot_profile_z(const fly_game *g, float pad_z, float px, float py,
                                 float leg, float path, float ux, float uy) {
    /* How much flying is left before the wheels are down — along the leg to the
       aim point and then along whatever final is left beyond it, never the
       straight line to the pad. On a straight-in approach the two are the same
       number; on an offset one they are not, and it is the flown distance the
       slope has to be spread over or the profile arrives at the gate already
       at pad height. */
    float aim = path - FLY_DOCK_RANGE * 0.55f;
    /* The scan runs to where this leg ends, which is the aim point — and when
       the aim point is the pad, to where the wheels are meant to be rather than
       to the pad itself. Everything past the aim point is the next leg's ground
       and is scanned when the aeroplane is flying it. */
    float scan = path > leg ? leg : leg - FLY_DOCK_RANGE * 0.55f;
    float reach = scan > 4500.0f ? 4500.0f : scan;
    /* seeded with the ground here and only ever raised, so it is the highest
       thing on the run in whether or not the scan below finds anything */
    float ahead = fly_world_ground(&g->world, px, py);
    float glide, cruise, tgt;
    if (reach > 1.0f && leg > 1.0f) {
        int k;
        for (k = 1; k <= 8; ++k) {
            float t = reach * (float)k / 8.0f;
            float h = fly_world_ground(&g->world, px + ux * t, py + uy * t);
            if (h > ahead) ahead = h;
        }
    }
    cruise = ahead + 450.0f;
    /* Fly a constant glide slope down to the pad rather than holding cruise
     * until 2.5 km out: from 450 m up, a 6 m/s sink cap needs ~75 s (over 4 km
     * of ground track) to lose the height, so the old profile arrived ~190 m
     * high and had to dive, stalling into the ground short of the apron.
     * Descent now begins wherever the slope meets cruise (~5 km out).
     *
     * The slope aims at the *threshold*, not at the pad, and that is the whole
     * difference between landing and not. It used to carry a constant twelve
     * metres all the way in, so the aircraft arrived over the apron still
     * twelve metres up with nothing left in the profile to take it down: it
     * floated across, went around, and did it again. Five of the sixteen sites
     * a starting aeroplane can reach were unreachable by autopilot for that
     * reason — a third of the map, for the feature the game leads with.
     *
     * Aiming at the pad centre is not enough either. The apron is 450 m across
     * and the approach crosses it in eleven seconds, so touchdown has to happen
     * inside a window no fixed sink rate can hit at every site. Aim to be on
     * the ground 250 m short and roll the rest in, which is what the wheels and
     * the taxi branch are for.
     *
     * The gradient is not a taste. Short final approaches at 0.80 of vref and
     * may not touch down faster than 3.2 m/s without fly_sim billing it as an
     * arrival, so the sink is capped at 3.0 — and a slope steeper than that cap
     * is a slope the aeroplane is forbidden to fly. It lags, arrives high, and
     * floats across the apron, which is precisely the failure this profile
     * exists to prevent. See FLY_APPROACH_SLOPE, which is where the gradient
     * and the reason for it are written down — and which the site survey scores
     * a course against, so the way in the world offers is the way the law
     * flies. */
    glide = pad_z + (aim > 0.0f ? aim : 0.0f) * FLY_APPROACH_SLOPE;
    tgt = glide < cruise ? glide : cruise;
    /* And the profile never goes below what is between here and the aim point —
     * but the clearance it insists on shrinks as the pad comes up. Sixty metres
     * is right while there is still room to climb over something and get out of
     * the way; a floor that stayed at sixty all the way in is what held
     * aeroplanes over their own apron until the divert timer took them somewhere
     * else.
     *
     * What it collapses against is the *apron*, not the distance. Ramping it to
     * nothing over the last two kilometres is the same instruction as "fly into
     * whatever is there", because the law tracks this profile to about twenty
     * metres and the last two kilometres are where the ground nearest the pad
     * is: wherever that ground stands above the pad, the ramp and the tracking
     * lag cross and the aeroplane arrives through the hillside. It is not one
     * pad's bad luck — on seed 4242, five of the thirteen touched down between
     * six hundred metres and six kilometres short, one of them at 17 of its 100
     * points of structure, and every one of them was still scored as a landing
     * because the outcome counts do not ask what it cost.
     *
     * So a dozen metres is held wherever anything on the run in stands above
     * the pad, and the clearance goes to nothing only over the pad's own graded
     * ring — the hundred and fifty metres fly_world_ground has already taken
     * down to the apron, which is the one place a floor has nothing to protect
     * the aeroplane from and everything to hold it off. The ramp collapses
     * against that ring rather than against a distance somebody picked, so it
     * is the same length wherever the pad is and it means the same thing.
     *
     * Below FLY_DOCK_RANGE * 0.55 the aim point is behind the aeroplane,
     * `reach` goes to nothing with it and no floor applies at all: the flare is
     * the short-final branch's business and not this one's. */
    if (reach > 1.0f) {
        float clear = 60.0f * fly_clampf(aim / 2000.0f, 0.0f, 1.0f);
        if (ahead > pad_z) {
            float hold = FLY_APPROACH_CLEAR * fly_clampf(aim / FLY_PAD_BLEND_R, 0.0f, 1.0f);
            if (clear < hold) clear = hold;
        }
        if (tgt < ahead + clear) tgt = ahead + clear;
    }
    return tgt;
}

static void autopilot_controls(fly_game *g, float dt) {
    const fly_location *T = &g->world.loc[g->autopilot_target];
    int arrived = 0;
    fly_v2 tq = fly_world_delta(T->pos, fly_wpos_of(g->player.craft.pos));
    fly_v3 to = fly_v3mk(tq.x, tq.y, 0);
    float dist = fly_v3len(to);
    /* How fast the pad itself is coming up, positive closing.
     *
     * `closure` below is the same quantity taken along the leg to the aim
     * point, and on an offset approach those are two different directions.
     * The taxi's patience wants this one: whether a taxi is getting anywhere
     * is a question about the pad and not about a waypoint on the way to it. */
    float closing = dist > 1.0f
                        ? (g->player.craft.vel.x * to.x + g->player.craft.vel.y * to.y) / dist
                        : 0.0f;
    /* Is this aeroplane taxiing? The latch rather than the wheels, and for the
     * reason the latch exists at all: a roll over natural ground skips, and on
     * every skip `on_ground` is false for a couple of tenths of a second. Three
     * things below ask the question and all three want the same answer through
     * a skip — see the aim point immediately underneath, which is the one that
     * used to get it wrong. */
    int taxiing = g->autopilot_taxi_s > 0.0f && dist < 2000.0f;
    /* --- where the aeroplane is actually being flown ---
     *
     * Not at the pad. The world knows which ways into a site are flyable — see
     * fly_world_approach_aim — and hands back a point to fly to and how much
     * final is left beyond it: the pad itself wherever the aeroplane is already
     * on a usable course for it, which is most approaches, and a point out on
     * the chosen final where it is not. That is the difference between a pad
     * behind a rise being nine go-arounds and being an approach flown from the
     * side the ground allows.
     *
     * `dist` stays the distance to the pad throughout, because everything that
     * asks whether the aeroplane has *arrived* — the short-final gate, the taxi
     * latch, docking, the patience timers — is asking about the pad and not
     * about a waypoint on the way to it. What the aim point changes is only
     * where the nose points and how far there is left to fly. */
    float along = 0.0f;
    /* ...and never once the wheels are down. A taxi goes to the pad; an
       aeroplane rolling across a field toward a waypoint chosen for an
       approach it is no longer flying is a taxi taking the scenic route.
     *
     * And the test for that is the latch, not the wheels. Written as
     * `on_ground` alone it was true between the skips and false during them,
     * so the demand the taxi steered to alternated — a few tenths at the pad,
     * a few tenths at a point out on a final the aeroplane had already flown —
     * several times a second, and the average of the two is neither. On seed
     * 4242 the aeroplane that landed long at Junthan spent the next three
     * minutes being steered a hundred and twenty-seven degrees off the pad,
     * driving out to 1372 m before it turned round: the scenic route, and it
     * was not the approach point's fault for being where it was. */
    fly_wpos aim_p = (g->player.craft.on_ground || taxiing)
                         ? T->pos
                         : fly_world_approach_aim(&g->world, g->autopilot_target,
                                                  fly_wpos_of(g->player.craft.pos), &along);
    fly_v2 aq = fly_world_delta(aim_p, fly_wpos_of(g->player.craft.pos));
    float leg = hypotf(aq.x, aq.y);        /* to the aim point */
    float path = leg + along;              /* and on down the final to the pad */
    float roll, pitch, yaw;
    fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
    float want_yaw = leg > 1.0f ? atan2f(aq.y, aq.x) : atan2f(to.y, to.x);
    float yerr = fly_wrap_pi(want_yaw - yaw);

    float gz = fly_world_ground(&g->world, g->player.craft.pos.x, g->player.craft.pos.y);
    /* Touch down on the apron the pad is actually leveled to, not on `elev`:
     * that field is a gate threshold, and skyports override it to 1700-2400 m
     * to drive FLY_GATE_HIGHALT while their pad still sits on the terrain.
     * Aiming at `elev` made the autopilot climb away from such a site and then
     * flare hundreds of metres above the ground, pancaking it into the dirt. */
    float pad_z = T->pad_z + g->player.airframe.gear_height;
    float ux = leg > 1.0f ? aq.x / leg : 1.0f;
    float uy = leg > 1.0f ? aq.y / leg : 0.0f;
    float tgt_z = autopilot_profile_z(g, pad_z, g->player.craft.pos.x, g->player.craft.pos.y,
                                      leg, path, ux, uy);
    float zerr = tgt_z - g->player.craft.pos.z;
    /* How fast the profile itself is going down, and the reason the aeroplane
     * ever reaches the pad.
     *
     * `vz_want` below was a plain proportional term on `zerr`, and a plain
     * proportional term cannot track a ramp: it only produces a sink rate by
     * holding an altitude error, so the aeroplane settles wherever the error is
     * big enough to generate the sink the profile is descending at, and flies
     * the whole approach that far above it. At the 0.075 slope and cruise speed
     * that is a standing fifty metres — measured across seed 4242, every one of
     * the thirteen reachable sites was flown between 33 and 79 m above its own
     * profile — and since the profile ends at the pad, fifty metres above the
     * profile is fifty metres above the pad with the apron already underneath.
     *
     * Most sites got away with it. The slope keeps descending, the aeroplane
     * keeps chasing, and somewhere inside 700 m it drops under `pad_z + 60` and
     * the short-final branch below takes over and lands it. Cinholm is the one
     * that does not: the ground falls 374 m over the six kilometres in front of
     * the pad, so its profile descends faster than anywhere else, so the
     * standing error is bigger — 122 m above the pad at 700 m out. Short final
     * never armed, the cruise law flew a loaded aeroplane straight over the
     * apron at 47 m, and it put down a kilometre and a half beyond.
     *
     * So the law flies the rate the profile is asking for and uses the error
     * only to close on it. The rate comes from evaluating the profile a second's
     * flying further down the track and differencing — one second because the
     * answer has to be the *profile's* gradient rather than the terrain's
     * roughness, and because the whole construction (slope, cruise, terrain
     * floor and its fade) has a gradient no closed form covers. Sampling it is
     * both simpler and correct for all four. */
    float closure = g->player.craft.vel.x * ux + g->player.craft.vel.y * uy;  /* m/s along the leg */
    /* one second of that closure, so the difference is already a rate */
    float tgt_rate = autopilot_profile_z(g, pad_z, g->player.craft.pos.x + ux * closure,
                                         g->player.craft.pos.y + uy * closure,
                                         leg - closure, path - closure, ux, uy) -
                     tgt_z;

    fly_controls in;
    memset(&in, 0, sizeof in);
    if (g->player.airframe.kind == FLY_CRAFT_PLANE) {
        float V = fly_craft_airspeed(&g->player.craft, &g->weather);
        /* All-up mass and the speed the wing gives up at, computed once for
         * the whole law. Both move with the load and with the altitude now,
         * so every phase that used to work off a fraction of Vref — a number
         * fixed at the airframe's empty weight — was working off the wrong
         * one whenever there was anything in the hold or the tanks. */
        float all_up = fly_craft_mass(&g->player.craft, &g->player.airframe, fly_game_cargo_mass(g));
        float v_stall = fly_craft_stall_speed(&g->player.airframe, g->player.craft.pos.z, all_up);
        /* A climb the aeroplane can actually fly. Five metres a second is
         * more than a loaded hauler has at altitude with a propeller that
         * gives less the faster it goes, and asking for it over a ridge is
         * how an autopilot mushes into one. */
        float vz_want = fly_clampf(tgt_rate + zerr * 0.08f, -6.0f, 3.5f);
        in.pitch = fly_clampf((vz_want - g->player.craft.vel.z) * 0.14f + 0.06f, -0.5f, 0.6f);
        /* coordinated turn: bank right (positive roll) to chase a negative yaw error */
        float roll_des = fly_clampf(-yerr * 1.2f, -0.65f, 0.65f);
        in.roll = fly_clampf((roll_des - roll) * 1.6f, -0.7f, 0.7f);
        in.yaw = fly_clampf(-yerr * 0.25f, -0.3f, 0.3f);
        /* Turbulence penetration. The load a gust puts through a wing goes
         * with the speed it is met at, so the answer to rough air is to slow
         * down — it is the first thing a real crew does and the reason the
         * placard exists. Without it the autopilot crossed the storm belt at
         * cruise and arrived with the spar half used up, or did not arrive. */
        float rough = fly_clampf(g->weather.turbulence *
                                     (1.0f - 0.7f * g->player.airframe.weather_rating),
                                 0.0f, 1.0f);
        float v_want = dist < 1800.0f ? g->player.airframe.vref * 0.78f
                                      : g->player.airframe.vref * (1.05f - 0.25f * rough);
        in.throttle = fly_clampf(0.55f + (v_want - V) * 0.05f + zerr * 0.004f, 0.0f, 1.0f);
        /* 900 m of taxi and no more. Further out was tried and is worse: an
           aeroplane rolling cross-country over natural ground at ten metres a
           second bounces, and enough bounces write it off. Beyond this the
           branch below runs it up for another circuit, which is what a landing
           that lands a kilometre short deserves. */
        /* Once it is down it stays down. A landing rollout is not perfectly
         * continuous contact — the wheels skip on natural ground, and every
         * skip used to hand the aeroplane back to the short-final controller,
         * which saw a metre of air under it and a sink rate and pulled. That
         * is a porpoise: fly, touch, bounce, pull, fly, and each cycle over
         * eight tenths of a second is billed as a fresh arrival. Aircraft
         * chipped themselves to death over half a kilometre of apron with
         * every individual contact inside limits. A skip is measured in
         * tenths of a second; anything under a second and a half of air is
         * still the same landing. */
        /* Arriving is a thing that has happened, not a place you are.
         *
         * `dist < 900` was a box, and a taxi that drifted out of it fell
         * through to the branch below — which is the takeoff roll. So an
         * aeroplane that had landed at its destination, been blown a little
         * wide on the ground and was rolling back in, opened full throttle,
         * rotated, flew a circuit, landed, drifted, and did it again.
         * Measured over 52 autopilot legs of seed 4242, 24 of them took off
         * again after they had already settled on the ground at the site they
         * were sent to.
         *
         * So the taxi window stays 900 m — a taxi is a taxi, and an aeroplane
         * that puts down a kilometre and a half short is better off going
         * round than grinding across country on its tyres — but *having
         * arrived* is latched, and once it is latched full power and a
         * rotation stop being an answer the autopilot has. `autopilot_taxi_s`
         * is that latch: it starts the moment the wheels are down inside the
         * destination's reach and only clears when the aeroplane is somehow
         * two kilometres away from it again. */
        if ((((g->player.craft.on_ground || g->player.craft.air_time < 1.5f) && dist < 900.0f) || taxiing)) {
            in.pitch = 0.0f;
            in.roll = 0.0f;
            /* Stop as soon as stopping is enough. Docking wants to be inside
               FLY_DOCK_RANGE and under 3 m/s, and this used to keep taxiing
               until 0.6 of that — so an aircraft sitting happily at 384 m of a
               450 m reach rolled on at thirteen metres a second, wandered on
               weak rudder authority, and left again. */
            if (dist > FLY_DOCK_RANGE * 0.85f) {
                /* Landed short of the pad: taxi in. Braking to a stop here left
                 * the aircraft parked outside docking range with the autopilot
                 * satisfied, so the run never completed.
                 *
                 * A taxi is not a takeoff roll, and a quarter throttle with
                 * nothing holding the aircraft down is a takeoff roll: it
                 * accelerated straight back through flying speed, lifted off,
                 * overshot the pad and went around, which is the loop this was
                 * meant to end. Govern the ground speed and keep the nose
                 * down. */
                float gs = fly_v3len(fly_v3mk(g->player.craft.vel.x, g->player.craft.vel.y, 0));
                /* Slow. The ground outside an apron is not a taxiway, and at
                   ten metres a second into a headwind the wing is still making
                   lift: it skipped off every undulation and each arrival back
                   on the surface was billed as a landing, which wrote one
                   aircraft off before it had rolled four hundred metres. */
                /* Against the *air*, not against the ground. Six metres a
                 * second of ground speed into a headwind of twenty is
                 * twenty-six over the wing, which is above the stall — the
                 * governor was quietly commanding a takeoff. Hold the taxi
                 * under two thirds of the speed the wing gives up at, and use
                 * the room that leaves on a calm day: nine metres a second
                 * closes the last kilometre in two minutes instead of three
                 * and gives the nosewheel, whose authority runs with speed,
                 * something to steer with. */
                float v_taxi = fly_clampf(v_stall * 0.34f, 4.0f, 9.0f);
                float over = fly_craft_airspeed(&g->player.craft, &g->weather) - v_stall * 0.66f;
                if (over > 0.0f) v_taxi = fly_clampf(v_taxi - over, 2.0f, v_taxi);
                in.throttle = gs < v_taxi ? 0.30f : 0.0f;
                in.brakes = gs > v_taxi + 3.0f;
                /* Nose down only while there is a nosewheel to hold down.
                 *
                 * A skip on rough ground takes the wheels off the surface for
                 * half a second, and a fifth of forward stick held through it
                 * at thirty-eight metres a second is not a taxiing aeroplane —
                 * it is a wing at minus eleven degrees, making lift downwards,
                 * flying itself into the ground at six metres a second. It
                 * bounced, was pushed back down harder, and wrote itself off
                 * over four hundred metres of apron in four cycles. In the air,
                 * however briefly, the only thing to do is arrest the sink and
                 * let it settle. */
                in.pitch = g->player.craft.on_ground
                               ? -0.2f
                               : fly_clampf((-0.7f - g->player.craft.vel.z) * 0.15f, -0.05f, 0.35f);
                /* Steer the *track*, on all the rudder there is.
                 *
                 * Two things were wrong with `clamp(-yerr * 0.8, ±0.5)`. It
                 * spends half the nosewheel — `plane_ground` scales its
                 * steering moment by the deflection, so a permanent half-stop
                 * is a permanent half a nosewheel — and it aims the *nose* at
                 * the pad, which on the ground is not where the aeroplane is
                 * going. Taxiing at six metres a second across a field with
                 * any wind on it, the ground track and the heading are
                 * different directions, and an aeroplane that points at the
                 * pad and travels somewhere else never arrives: it wandered
                 * out to seven hundred metres and circled there until the
                 * divert timer took it away.
                 *
                 * So the demand is on the track, and the nose only comes into
                 * it as damping — enough to stop the correction oscillating,
                 * not enough to fly the aeroplane sideways. Below a metre a
                 * second there is no track to speak of and the heading is all
                 * there is. */
                {
                    float terr = yerr;
                    if (gs > 1.0f) {
                        float trk = atan2f(g->player.craft.vel.y, g->player.craft.vel.x);
                        terr = fly_wrap_pi(want_yaw - trk);
                    }
                    in.yaw = fly_clampf(-terr * 1.4f - yerr * 0.3f, -1.0f, 1.0f);
                }
            } else {
                /* close enough to dock: stop */
                in.throttle = 0.0f;
                in.yaw = 0.0f;
                in.brakes = 1;
            }
        } else if (g->player.craft.on_ground && !taxiing) {
            /* takeoff roll: run up into the wind, stay flat to rotation speed */
            /* Rotation speed off the stall speed the flight model actually
             * uses, at the weight the aeroplane actually is. This used to
             * roll its own square root over the empty airframe plus the
             * cargo, which stopped being the weight the day fuel became mass
             * — a Condor departing with three hundred kilos in the tanks
             * rotated at a speed the wing could not fly at. */
            float m = fly_craft_mass(&g->player.craft, &g->player.airframe, fly_game_cargo_mass(g));
            float vr = fly_craft_stall_speed(&g->player.airframe, g->player.craft.pos.z, m) * 1.18f;
            float wind_speed = fly_v3len(g->weather.wind);
            float run_yaw = wind_speed > 4.0f
                                ? atan2f(-g->weather.wind.y, -g->weather.wind.x)
                                : want_yaw;
            float ryerr = fly_wrap_pi(run_yaw - yaw);
            in.throttle = 1.0f;
            in.roll = fly_clampf(-roll * 1.5f, -0.5f, 0.5f);
            in.yaw = fly_clampf(-ryerr * 0.8f, -0.5f, 0.5f);
            in.pitch = V > vr ? 0.42f : 0.03f;
        } else if (dist < 700.0f && g->player.craft.pos.z < pad_z + 60.0f) {
            /* Short final. The old profile chopped the throttle to 0.12 and
             * held the brakes on while still airborne 700 m out, which bled the
             * airspeed away and stalled the aircraft into the ground short of
             * the apron. Instead: fly a powered approach at a safe speed, keep
             * the sink gentle, and only round out in the last few metres. */
            /* Short final: fly a powered approach at a safe speed and keep the
             * sink gentle. The old profile chopped the throttle to 0.12 and
             * held the brakes on while still airborne 700 m out, bleeding the
             * airspeed away and stalling the aircraft into the ground short of
             * the apron. Wings are levelled here too — leaving the cruise turn
             * logic in command banked it into the ground on arrival. */
            float agl = g->player.craft.pos.z - pad_z;
            /* Approach speed as airmanship states it: a third again over the
             * stall, at today's weight and today's altitude. A fixed fraction
             * of Vref is a number that was right for one loading of one
             * airframe — and with fuel and freight now in the mass, and the
             * stall moving with both, it is the wrong number for every other
             * one. Floored at the old figure so a light aeroplane still
             * arrives with enough energy to flare. */
            float v_app = fly_clampf(v_stall * 1.60f, g->player.airframe.vref * 0.80f,
                                     g->player.airframe.vref * 1.10f);
            /* Roll the wings level for touchdown. fly_sim rejects a contact
             * with |roll| > 0.45 rad, and the cruise turn logic was still
             * commanding its full +-0.65 bank at the moment of landing — by far
             * the single largest cause of arrival crashes. Only in the last
             * few tens of metres though: levelling across the whole final made
             * a misaligned arrival unable to turn back, so it overshot and
             * orbited the pad instead of landing. Rudder still trims heading. */
            if (agl < 25.0f) in.roll = fly_clampf(-roll * 2.5f, -0.6f, 0.6f);
            in.throttle = fly_clampf(0.32f + (v_app - V) * 0.06f, 0.0f, 1.0f);
            /* Fly the slope, not a sink rate.
             *
             * Short final used to command a fixed -2.5 m/s above fifteen metres
             * and -0.9 below, and neither number knows where the pad is. At
             * forty metres a second, nine tenths of a metre a second is a float
             * that crosses the whole apron and keeps going: a clean, stable,
             * entirely correct approach flown straight over the pad and round
             * again, for ever. The slope `tgt_z` already ends at the pad, so
             * track it — the closer in, the lower it asks for, and it reaches
             * zero exactly where the aeroplane should be on the ground. */
            /* Short final aims at the pad and only ever descends.
             *
             * The slope above is a function of distance, so an aircraft that
             * floats over the pad sees its distance start *growing* again and
             * is promptly told to climb back onto the approach it has just
             * finished flying. That is a stable limit cycle, and aircraft sat
             * in it indefinitely — three hundred to eight hundred metres out,
             * twenty to forty metres up, going round and round. Once low and
             * close the aeroplane is committed: target the deck, clamp the sink
             * at -3.0 (fly_sim bills a touchdown that arrives faster than 3.2)
             * and never command a climb. Going around is a decision, not
             * something a proportional term should stumble into. */
            float sink_want = fly_clampf((pad_z - g->player.craft.pos.z) * 0.22f, -3.0f, 0.0f);
            /* The flare, measured off the ground actually underneath rather
               than off the pad — an approach that arrives a little short
               arrives over terrain, and a hold-off referenced to the apron
               either digs in or floats depending on which way the ground went.

               This is the piece that was missing. The profile above flies a
               slope onto the deck, which is fine on paper and arrives at
               three metres a second — and fly_sim bills anything over 3.2 as
               a landing rather than an arrival, so the whole approach lived
               on two tenths of a metre a second of margin. A gust, a metre of
               shear or a wing's worth of ground effect and it was gone. The
               last few metres are a different manoeuvre: hold a little power
               so the speed does not decay into the stall, ask for a sink a
               tenth of what the slope was carrying, and let the aeroplane
               settle onto the wheels. */
            float flare = g->player.craft.pos.z - (gz + g->player.airframe.gear_height);
            if (flare < 8.0f) {
                sink_want = -0.7f;
                /* Power comes off in the flare, and not one knot before: an
                 * aeroplane that arrives over the numbers already slow needs
                 * the engine, and a throttle cap that does not ask how fast it
                 * is going is how a good approach becomes a stall in the last
                 * eight metres. */
                if (V > v_app * 0.98f) in.throttle = fly_clampf(in.throttle, 0.0f, 0.22f);
            } else if (dist < FLY_DOCK_RANGE * 0.75f) {
                /* Inside the apron, close the throttle and let it come down
                   onto the slope. A powered approach that holds four fifths
                   of vref will not descend. */
                in.throttle = 0.0f;
            }
            /* and a firmer hand on the pitch, because the elevator at four
               fifths of Vref now has under two thirds of its cruise authority */
            in.pitch = fly_clampf((sink_want - g->player.craft.vel.z) * 0.18f + 0.05f, -0.25f, 0.55f);
            in.brakes = 0; /* brakes are for the rollout, not the air */
        }
        /* Speed is life, and it outranks every phase of the profile.
         *
         * The same rule the AI pilots fly by, and here for the same reason: a
         * loaded aeroplane on final at four fifths of Vref is a few knots and
         * one gust from the wrong side of the curve, and the phase controllers
         * are all asking for something else — a slope, a sink rate, a pad. If
         * the wing is inside a fifth of giving up, nothing else matters:
         * everything the engine has, and the nose comes down.
         *
         * Above ten metres only. Below that the aeroplane is *supposed* to be
         * decaying through this speed — that is what a hold-off is — and a
         * guard that firewalls the throttle at four metres does not save a
         * landing, it floats the aeroplane the length of the apron and sends
         * it round again. Two of thirteen routes diverted for exactly that
         * before the height test was added. */
        if (!g->player.craft.on_ground && v_stall > 1.0f && V < v_stall * 1.22f &&
            g->player.craft.pos.z - gz > 10.0f) {
            in.throttle = 1.0f;
            in.pitch = fly_clampf(in.pitch - (v_stall * 1.22f - V) * 0.08f, -1.0f, 0.15f);
        }

        /* Nobody banks steeply near the ground.
         *
         * The cruise turn is allowed 37 degrees, and with control power now
         * measured against dynamic pressure an approach at four fifths of
         * Vref has under two thirds of the roll authority it would have at
         * cruise — so a late correction onto the pad overbanks, the lift
         * vector goes sideways, the aeroplane falls out of the turn, and the
         * wings-level logic that used to wait for the last twenty-five metres
         * gets 1.6 seconds to fix seventy degrees of bank. It does not. This
         * is the same rule a pilot flies by and it is a function of height,
         * because that is what you have to trade: forty degrees at four
         * hundred feet, seven on the numbers.
         *
         * Placed with the stall protection, after the phase branches, so no
         * phase can command an attitude the height cannot afford. */
        if (!g->player.craft.on_ground) {
            float agl = g->player.craft.pos.z - gz;
            float lim = fly_clampf(agl / 110.0f, 0.18f, 0.65f);
            if (fabsf(roll) > lim) {
                float over = roll - fly_signf(roll) * lim;
                in.roll = fly_clampf(-over * 4.0f, -1.0f, 1.0f);
            }
        }

        /* Stall protection runs after the phase branches so none of them can
         * command an attitude that stalls the aircraft.
         *
         * A subtraction alone is not enough any more, and the reason is the
         * dynamic-pressure term: a mushing aeroplane at three quarters of
         * Vref has half the elevator authority it has at cruise, so a
         * correction sized for cruise arrives at half strength exactly when
         * it is needed at full. Worse, the phase controllers can saturate at
         * +0.6 while trying to out-climb a ridge the aeroplane cannot
         * out-climb, and the subtraction only ever brings that back toward
         * neutral. Past three quarters of the way to the break the stick goes
         * *forward* and stays there until the wing is flying again — which is
         * the one instruction that is always right and the one no phase
         * controller is allowed to override. */
        if (!g->player.craft.on_ground) {
            float amax = g->player.airframe.alpha_stall * 0.72f;
            float over = g->player.craft.alpha - amax;
            if (over > 0.0f) {
                /* With height in hand the stick goes forward and stays there:
                 * nothing above the circuit is worth a stalled wing. In the
                 * last sixty metres it is only pushed back toward neutral,
                 * because a hold-off *is* a deliberate approach to the buffet
                 * and an autopilot that answers it by shoving the nose down
                 * arrives nose-first on the apron — which is how this guard,
                 * written as one rule for the whole flight, wrote off two
                 * aircraft that had otherwise flown a good approach. */
                float ceiling = (g->player.craft.pos.z - gz) > 60.0f ? 0.0f : 0.22f;
                in.pitch = fly_clampf(in.pitch - over * 6.0f, -1.0f, ceiling);
            }
        }
        /* And the ball, for the same reason the AI pilots got one: adverse
         * yaw and propeller torque are both new, both permanent, and both
         * push the nose out of the turn. An autopilot that flew every leg in
         * a skid would arrive late on the drag alone, and would put a wing
         * down first on every landing.
         *
         * In the air, though, and nowhere else. The ball is an air
         * instrument: what it reads on the ground is what a crosswind makes
         * of an aeroplane rolling at nine metres a second, and that is not
         * something a nosewheel should be chasing. Applied to a taxi it did
         * both of the things the taxi's own steering law is written to avoid.
         * It clamped the rudder to +-0.6 — the half a nosewheel `plane_ground`
         * charges for, since it scales the steering moment by the deflection —
         * and then fed a quarter of a radian of crosswind slip back in against
         * the turn, so a taxi that had commanded the full stop to come round
         * onto its pad was given three tenths of it. On seed 4242 that is a
         * hundred and ninety-five seconds to turn an aeroplane round, over
         * nine hundred metres of ground, and it is most of why the legs that
         * landed long ran out of patience facing the wrong way.
         *
         * The latch rather than the wheels, because a taxi over natural ground
         * skips and a rule that switches off for two tenths of a second at a
         * time is a rule that fights the steering at random. */
        if (!g->player.craft.on_ground && !taxiing)
            in.yaw = fly_clampf(in.yaw + g->player.craft.slip * 1.6f, -0.6f, 0.6f);

        /* And in the last few metres, the crab comes off.
         *
         * An approach into a crosswind is flown pointing into it, so the
         * aeroplane arrives going one way and pointing another — and the
         * tyres have an opinion about that now. They do not go sideways, so
         * the whole of the drift is scrubbed through the gear in one contact
         * and charged for: eleven metres a second of it wrote off an aircraft
         * that had otherwise flown a textbook approach and touched down at
         * half a metre a second.
         *
         * A rule for the height rather than a step in the profile, and that
         * matters: an aeroplane that arrives eight hundred metres short is
         * outside the short-final branch and inside the rollout branch's
         * patience, so a de-crab written into either of them is a de-crab
         * that does not happen on exactly the arrivals that need it most.
         * In the last five metres, whatever the profile thinks it is doing,
         * the nose comes round onto the track — and not a metre earlier,
         * because an aeroplane that stops crabbing stops holding the
         * centreline and starts going downwind, which at fifteen metres is
         * enough to lose the pad and go round again. */
        if (!g->player.craft.on_ground && g->player.craft.pos.z - gz < 5.0f && V > 1.0f) {
            float track = atan2f(g->player.craft.vel.y, g->player.craft.vel.x);
            in.yaw = fly_clampf(fly_wrap_pi(track - yaw) * 1.8f, -0.7f, 0.7f);
        }
    } else {
        float hover = (g->player.airframe.mass + fly_game_cargo_mass(g)) * 9.80665f /
                      (g->player.airframe.rotor_thrust > 1 ? g->player.airframe.rotor_thrust : 1);
        in.throttle = fly_clampf(hover + zerr * 0.02f - g->player.craft.vel.z * 0.03f, 0.0f, 1.0f);
        in.yaw = fly_clampf(-yerr * 0.8f, -0.7f, 0.7f);
        float v_want = dist > 400.0f ? 16.0f : dist * 0.03f;
        fly_v3 vb = fly_qrot(fly_qconj(g->player.craft.ori), g->player.craft.vel);
        in.pitch = fly_clampf(-(v_want - vb.x) * 0.06f, -0.55f, 0.55f);
        in.roll = fly_clampf(-roll * 1.2f, -0.4f, 0.4f);
    }
    g->player.controls = in;

    /* An approach that is not working has to end in something.
     *
     * Nothing bounded this. An autopilot that could not get down — because the
     * site refuses the airframe, because the wind is wrong, because the profile
     * does not suit that pad — simply flew circuits until the tanks ran dry,
     * silently, with the destination a few hundred metres away the whole time.
     * That is the worst shape a failure can take: no landing, no message, no
     * end, and an aeroplane lost to something the player was never told about.
     * Fifteen minutes inside two kilometres without the wheels down is a
     * failed approach; hand the aircraft back and say so. It is deliberately
     * generous — a circuit at approach speed is a minute and a half, so this
     * is nine attempts, and it bounds the pathology without second-guessing
     * the flying.
     *
     * It was seven minutes, and seven was calibrated against a flight model in
     * which an approach either worked or did not. It is not that any more: the
     * wind shears through the last fifty metres, the gusts are correlated
     * enough to arrive as a lump on short final, and the wing lets go at an
     * angle rather than a speed — so a go-around is an ordinary event rather
     * than a symptom, and two of the thirteen routes in seed 4242 were
     * diverting after four attempts at pads they land at on the sixth. */
    /* Two things were wrong with that as written, and both let the pathology
     * through the bound that exists to catch it.
     *
     * It ran on *proximity*, so an approach that had worked — wheels down at
     * the destination, taxiing in — went on accumulating failure, and a taxi
     * long enough to reach fifteen minutes ended with the aeroplane opening
     * the throttle, leaving the site it had landed at and flying to a
     * different one. On seed 4242 that is how a delivery to Junthan came back
     * to Junral with the cargo still aboard and the contract still running.
     *
     * And it was reset by leaving, which is the hole. A go-around takes the
     * aeroplane outside two kilometres by design, so an aircraft that flew
     * eight failed approaches had eight separate clocks of ninety seconds and
     * never reached fifteen minutes of anything: it circled until the tanks
     * ran dry, which is the exact failure the timer was written to prevent.
     * The measure is time since it *first* got there, which is what "nine
     * attempts" always meant, and only leaving on a divert restarts it. The
     * clock stops while the wheels are down, because an arrival is not a
     * failed approach and the taxi has a bound of its own below. */
    if (dist < 2000.0f && g->autopilot_near_s <= 0.0f)
        g->autopilot_near_s = 1e-4f;   /* armed: it has reached the destination */
    if (g->autopilot_near_s > 0.0f && !g->player.craft.on_ground) {
        g->autopilot_near_s += dt;
        if (g->autopilot_near_s > 900.0f) {
            /* Divert; do not simply let go.
             *
             * Handing an aeroplane back in mid-air is only a handover if
             * somebody is there to take it, and the entire premise of this
             * feature is that nobody is — "flies routes while you are AFK". A
             * bare disengage over an unlandable pad is a crash with extra
             * steps, and that is what it did. Pick the nearest other site this
             * airframe can actually use and go there instead. */
            int alt = -1;
            float best = 1e30f;
            int k;
            for (k = 0; k < g->world.nloc; ++k) {
                float d;
                if (k == g->autopilot_target || !g->world.loc[k].discovered) continue;
                if (fly_world_gate_check(&g->world, k, &g->player.airframe)) continue;
                d = dist2d(g->player.craft.pos, g->world.loc[k].pos);
                if (d < best) { best = d; alt = k; }
            }
            g->autopilot_near_s = 0.0f;
            if (alt >= 0) {
                fly_game_log(g, "No approach into %s: diverting to %s", T->name,
                             g->world.loc[alt].name);
                g->autopilot_target = alt;
            } else {
                fly_game_log(g, "No approach into %s and nowhere to divert", T->name);
                g->autopilot = 0;
            }
            return;
        }
    }

    /* The taxi has its own patience, and it ends in a stop.
     *
     * Not diverting is the point. The aeroplane is down, in one piece, at the
     * place it was sent to; whatever is keeping it off the pad — a wind it
     * cannot out-taxi, ground it cannot cross — is not answered by opening the
     * throttle and flying somewhere else, which is what routing this case
     * through the divert above did.
     *
     * And the bound is progress rather than a clock, because that is what the
     * question actually is. A taxi from the far end of the approach is a
     * couple of minutes and is going somewhere; a taxi that has not got any
     * closer in four is not going to, and a flat ten-minute allowance spent
     * the remaining six grinding a shot-up hull across country until it came
     * apart. Four minutes without a new closest approach, then stop where it
     * stands and say so, so the operator comes back to an aeroplane on the
     * ground at the right site rather than to a wreck or to one that has
     * quietly gone somewhere else.
     *
     * Four rather than two, measured: at two the taxi gives up on legs it
     * would have finished — 49 of 52 ended on the ground at the destination
     * against 51 at four, and three ran out of approach entirely against
     * one. */
    /* What arms it is an arrival, not a touch. A go-around at a rough pad
     * skips the wheels on at forty metres a second and takes them off again,
     * and latching on that put an aeroplane that was in the middle of a
     * perfectly good circuit into taxi mode with the nose down and the
     * throttle shut. Under two thirds of the stall it is not flying any more,
     * whatever the wheels are doing this instant.
     *
     * The clock, once armed, runs on the latch rather than on the wheels: a
     * taxi over natural ground skips too, and counting only the steps with
     * weight on them ran the timer at half speed — which is how an aeroplane
     * wandered a kilometre the wrong way inside a hundred-and-twenty second
     * allowance. */
    {
        float gsp = fly_v3len(fly_v3mk(g->player.craft.vel.x, g->player.craft.vel.y, 0));
        float vs = fly_craft_stall_speed(&g->player.airframe, g->player.craft.pos.z,
                                         fly_craft_mass(&g->player.craft, &g->player.airframe,
                                                        fly_game_cargo_mass(g)));
        arrived = g->player.craft.on_ground && dist < 2000.0f && gsp < vs * 0.66f;
    }
    if (g->autopilot_taxi_s > 0.0f || arrived) {
        if (g->autopilot_taxi_s <= 0.0f) g->autopilot_taxi_hp = g->player.craft.hp;
        if (g->autopilot_taxi_s <= 0.0f || dist < g->autopilot_taxi_best - 10.0f) {
            g->autopilot_taxi_best = dist;
            g->autopilot_taxi_s = 1e-4f;
        }
        /* And the clock does not run while the aeroplane is closing.
         *
         * "No new closest approach in four minutes" reads the record from the
         * moment the latch arms, which is on the landing roll — so a landing
         * that rolls out away from the pad sets its record at the far end of
         * its own rollout and has to claw all of it back before the measure
         * will admit the taxi is working. At Junthan that was 930 m at nine
         * metres a second against a clock that had already been running
         * throughout the turn: the patience expired with the aeroplane a
         * kilometre out and closing steadily, eighty seconds from the pad, and
         * shut it down for want of progress it was visibly making.
         *
         * Closing is progress whether or not it has beaten the record yet, so
         * the clock pauses for it. What the bound still catches is what it was
         * written for — an aeroplane going away, going nowhere, or going round
         * in a circle, which spends half of every lap not closing and reaches
         * four minutes of it soon enough. A metre a second rather than zero so
         * that drift and the odd skip do not read as a taxi. */
        if (closing < 1.0f) g->autopilot_taxi_s += dt;
        /* And it stops if it is costing the aeroplane. The ground outside an
         * apron is not a taxiway and a long roll over it is paid for in
         * structure; on a full hull that is a few points and worth the pad,
         * on one a pirate has already taken to fourteen it is the end of the
         * airframe. Nobody taxis a broken aeroplane a kilometre — they shut
         * down where they stopped. Only ever on the ground: giving up in the
         * air is the one thing this whole section exists to prevent. */
        if (g->player.craft.on_ground &&
            (g->autopilot_taxi_s > 240.0f || g->player.craft.hp < g->autopilot_taxi_hp - 5.0f)) {
            g->autopilot_taxi_s = 0.0f;
            g->autopilot = 0;
            g->player.controls.throttle = 0.0f;
            g->player.controls.brakes = 1;
            fly_game_log(g, "Down at %s, %.0f m short of the pad", T->name, dist);
            return;
        }
    } else if (dist >= 2000.0f) {
        /* Only distance clears the latch, and never a skip: a wheel that
         * leaves rough ground for half a second has not un-arrived the
         * aeroplane, and clearing on `!on_ground` handed it straight back to
         * the takeoff roll, which is the loop this exists to close. Two
         * kilometres of ground from the pad is a different situation and the
         * aeroplane may as well fly — but the stop above gets there first,
         * because a taxi that is going the wrong way runs out of patience
         * inside a hundred and fifty metres of that. */
        g->autopilot_taxi_s = 0.0f;
    }

    /* autopilot risk: rough air + worn airframes + danger => incidents */
    g->autopilot_cooldown -= dt;
    if (g->autopilot_cooldown <= 0.0f) {
        g->autopilot_cooldown = 25.0f;
        float danger = fly_world_danger(&g->world, g->player.craft.pos.x, g->player.craft.pos.y);
        float risk = 0.03f + g->weather.turbulence * 0.10f + g->player.craft.wear * 0.16f + danger * 0.05f;
        if (fly_rng_f01(&g->play_rng) < risk) {
            switch (fly_rng_range(&g->play_rng, 3)) {
            case 0:
                g->player.craft.omega = fly_v3add(g->player.craft.omega, fly_v3mk(fly_rng_gauss(&g->play_rng) * 0.8f,
                                                                    fly_rng_gauss(&g->play_rng) * 0.5f, 0));
                fly_game_log(g, "AP upset: hit rough air");
                break;
            case 1:
                g->player.craft.wear = fly_clampf(g->player.craft.wear + 0.02f, 0, 1);
                fly_game_log(g, "AP strain: wear +2%%");
                break;
            default:
                g->player.craft.fuel *= 0.965f;
                fly_game_log(g, "AP fault: fuel venting");
                break;
            }
        }
    }

    /* arrived? handled by docking check in step (on_ground near pad) */
    (void)dt;
}

/* ---------------- hostiles / dogfights ---------------- */

/* ---------------- the other people flying ----------------
 *
 * The world is populated once, at the sites, and topped back up as pilots are
 * lost. They are not spawned around the camera and they are not despawned
 * behind it: a trader that took off from Ororell an hour ago is somewhere
 * between Ororell and wherever it was going, whether or not anyone watched it
 * leave. Only the fidelity changes with distance.
 *
 * Everything a pilot does arrives here as a fly_pilot_cmd and goes through
 * journal_apply, so the AI and a future socket are the same producer as far as
 * the world is concerned. */

/* Full flight dynamics inside this radius of the player, cruise beyond it.
 * Comfortably past the range anything reads as an aircraft rather than a dot,
 * and past the gun ranges, so nothing that matters is decided coarsely. */
#define FLY_PILOT_DETAIL_R 7000.0f

static int journal_apply(fly_game *g, fly_pilot *p, const fly_pilot_cmd *cmd) {
    fly_pilot_journal_entry *e;
    int rc;
    if (cmd->kind == FLY_PCMD_NONE) return 0;
    rc = fly_pilot_apply(p, &g->world, g->time_s, cmd);
    if (rc != 0) return rc;
    e = &g->journal[g->journal_head];
    e->time = g->time_s;
    e->actor = p->id;
    e->cmd = *cmd;
    g->journal_head = (g->journal_head + 1) % FLY_PILOT_JOURNAL;
    ++g->journal_total;
    return 0;
}

int fly_game_pilot_inject(fly_game *g, uint32_t actor, const fly_pilot_cmd *cmd) {
    int i;
    if (!g || !cmd) return -1;
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active && g->pilots[i].id == actor)
            return journal_apply(g, &g->pilots[i], cmd);
    return -1;
}

const fly_pilot *fly_game_pilot_find(const fly_game *g, uint32_t id) {
    int i;
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active && g->pilots[i].id == id) return &g->pilots[i];
    return NULL;
}

/* How many of each kind the world wants, scaled by how many sites it has. The
 * mix is the point: mostly people moving goods, a few police, a few outlaws,
 * so most encounters are commerce and the violent ones are an event. */
static int pilot_quota(const fly_game *g, fly_pilot_kind k) {
    int n = g->world.nloc;
    switch (k) {
    case FLY_PILOT_TRADER:  return n / 4 + 2;
    case FLY_PILOT_COURIER: return n / 5 + 1;
    case FLY_PILOT_PATROL:  return n / 10 + 1;
    case FLY_PILOT_PIRATE:  return n / 10 + 1;
    /* One. Not one per ten sites, not one per region — one, in the whole
       world, above a line almost nobody can cross. Everything else about the
       saucers is tuned on the assumption that meeting a second one in the same
       session is unusual. */
    case FLY_PILOT_UFO:     return 1;
    default:                return n / 12 + 1;
    }
}

static int pilot_live(const fly_game *g, fly_pilot_kind k) {
    int i, n = 0;
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active && g->pilots[i].kind == k) ++n;
    return n;
}

/* Pick a home for a new pilot: outlaws base themselves at ruins and mines and
 * the far corners, everyone else at whatever trades. */
static int pilot_home(fly_game *g, fly_pilot_kind k) {
    int i, pick = -1, seen = 0;
    for (i = 0; i < g->world.nloc; ++i) {
        int outlaw_ok = g->world.loc[i].kind == FLY_LOC_RUINS ||
                        g->world.loc[i].kind == FLY_LOC_MINE;
        int civil_ok = g->world.loc[i].kind != FLY_LOC_RUINS;
        if (k == FLY_PILOT_PIRATE ? !outlaw_ok : !civil_ok) continue;
        if (fly_rng_range(&g->rng, (uint32_t)(++seen)) == 0) pick = i;
    }
    if (pick < 0 && g->world.nloc > 0)
        pick = (int)fly_rng_range(&g->rng, (uint32_t)g->world.nloc);
    return pick;
}

static void pilots_restock(fly_game *g, float dt) {
    int k, slot;
    g->pilot_restock -= dt;
    if (g->pilot_restock > 0.0f) return;
    g->pilot_restock = 45.0f;
    for (k = 0; k < FLY_PILOT_KIND_COUNT; ++k) {
        if (pilot_live(g, (fly_pilot_kind)k) >= pilot_quota(g, (fly_pilot_kind)k)) continue;
        for (slot = 0; slot < FLY_MAX_PILOTS; ++slot)
            if (!g->pilots[slot].active) break;
        if (slot == FLY_MAX_PILOTS) return;
        {
            int home = pilot_home(g, (fly_pilot_kind)k);
            if (home < 0) return;
            fly_pilot_spawn(&g->pilots[slot], ++g->next_pilot_id, (fly_pilot_kind)k,
                            home, &g->world, &g->rng);
            ++g->pilot_count;
        }
        return; /* one per tick: the sky fills up over minutes, not at once */
    }
}

void fly_game_populate(fly_game *g) {
    int k, n, slot = 0;
    for (k = 0; k < FLY_PILOT_KIND_COUNT; ++k)
        for (n = 0; n < pilot_quota(g, (fly_pilot_kind)k); ++n) {
            int home;
            while (slot < FLY_MAX_PILOTS && g->pilots[slot].active) ++slot;
            if (slot >= FLY_MAX_PILOTS) return;
            home = pilot_home(g, (fly_pilot_kind)k);
            if (home < 0) return;
            fly_pilot_spawn(&g->pilots[slot], ++g->next_pilot_id, (fly_pilot_kind)k,
                            home, &g->world, &g->rng);
            ++g->pilot_count;
        }
}

/* returns aim quality 0..1 if target within gun cone+range */
static float gun_solution(const fly_craft *shooter, const fly_airframe *af, const fly_craft *target) {
    return fly_pilot_gun_solution(shooter, af, target);
}

/* Would `a` open fire on `b`? The same rule `fly_pilot_think` picks marks
 * with, applied at the trigger — the two have to agree or a pilot chases
 * something it will not shoot at, or worse, shoots something it never decided
 * to hunt.
 *
 * An outlaw robs anyone it is not at terms with; everybody else fights
 * combatants of a power theirs is at war with. That second clause is the one
 * holding the world together: with seven powers and a live relation matrix,
 * "shoot anything with the wrong flag" empties the sky of the traders whose
 * deliveries are the entire economy. */
static int pilots_will_fire(const fly_game *g, const fly_pilot *a, const fly_pilot *b) {
    if (a->faction == b->faction) return 0;
    if (b->faction == FLY_FACTION_FREE && a->kind != FLY_PILOT_PIRATE) return 0;
    if (fly_diplomacy_state(&g->world.dip, a->faction, b->faction) >= FLY_REL_PACT) return 0;
    if (a->kind == FLY_PILOT_PIRATE) return 1;
    if (!fly_pilot_is_combatant(a)) return 0;
    /* Outlaws are everybody's business — see prey_ok in fly_pilot.c for why
     * this is structural and not a matter of who is at war with whom. The two
     * predicates have to agree or a patrol chases something it will not fire
     * on. */
    if (b->kind == FLY_PILOT_PIRATE) return 1;
    if (!fly_faction_hostile(&g->world, a->faction, b->faction)) return 0;
    return fly_pilot_is_combatant(b);
}

/* And at the player, who wears a flag now. Unaligned, this is what it always
 * was: outlaws and nobody else. Pledged, every power at war with yours has a
 * reason to be interested — which is the price of the oath and is stated on
 * the pledge row before it is taken. */
static int pilots_will_fire_player(const fly_game *g, const fly_pilot *a) {
    int pf = g->player.faction;
    if (a->faction == pf && pf != FLY_FACTION_FREE) return 0;
    if (pf != FLY_FACTION_FREE &&
        fly_diplomacy_state(&g->world.dip, a->faction, pf) >= FLY_REL_PACT) return 0;
    if (a->kind == FLY_PILOT_PIRATE) return 1;
    return fly_faction_hostile(&g->world, a->faction, pf);
}

/* --- what a kill costs, once, wherever it came from ----------------------
 *
 * Both of these used to live inline in the gun step, which was fine while a gun
 * was the only thing in the world that could kill anybody. It is not any more:
 * a seeker, a bomb, a rocket and a column's own missile all end the same way,
 * and four transcriptions of "who is pleased and who is not" would have drifted
 * apart within a release. A kill is an act of foreign policy and the ledger has
 * to say so in every direction at once, whatever pulled the trigger. */

static void fly__pilot_kill(fly_game *g, fly_pilot *a, fly_pilot *b) {
    b->craft.crashed = 1;
    ++a->kills;
    ++g->pilot_kills;
    /* Ground is taken with guns as well as with cargo. A kill pushes for the
     * shooter at whatever site it happened over, which is what makes a patrol
     * worth flying and what turns a contested refinery into somewhere aircraft
     * actually die. */
    fly__kill_push(g, a, b);
    if (fly_v3dist(b->craft.pos, g->player.craft.pos) < fly_game_radar_range(g))
        fly_game_log(g, "Radar: %s (%s) shot down %s (%s)", a->name,
                     fly_faction_tag(a->faction), b->name,
                     fly_faction_tag(b->faction));
}

/* And when it was the player. Whoever wanted this one dead is pleased, whoever
 * it flew for is not, and if the player is wearing colours the ground
 * underneath moves too. The bounty survives unchanged for outlaws, because
 * shooting a pirate is the one thing every power agrees about. */
static void fly__player_kill(fly_game *g, fly_pilot *b) {
    int killed = b->faction;
    b->craft.crashed = 1;
    if (b->kind == FLY_PILOT_PIRATE) {
        long bounty = 80 + (long)fly_rng_range(&g->rng, 60);
        g->player.tokens += bounty;
        g->xp += 25;
        g->reputation += 2;
        fly_game_log(g, "Splash one! Bounty +%ld tk", bounty);
    } else {
        g->reputation -= 6;
        if (g->reputation < 0) g->reputation = 0;
        fly_game_log(g, "You shot down %s of the %s. Word travels.",
                     b->name, fly_faction_name(killed));
    }
    fly_game_credit_faction(g, killed, -14);
    if (g->player.faction != FLY_FACTION_FREE &&
        fly_faction_hostile(&g->world, g->player.faction, killed)) {
        fly_game_credit_faction(g, g->player.faction, 9);
        fly__site_push_at(g, b->craft.pos, g->player.faction, FLY_KILL_PUSH);
    }
}

static int fly__player_in_play(const fly_game *g);

/* --- the beam's price -----------------------------------------------------
 *
 * Holding the trigger down cooks the emitter, and the emitter is bolted to the
 * airframe: the energy goes into `fly_craft.heat`, the same field a re-entry
 * fills, because a hull does not care what put it there. At 0.85 the weapon is
 * locked out until it has cooled back to 0.45 (see fly_sim_step, which does the
 * cooling with everything else that decays), so an overheat is a few seconds
 * you have to fly without it rather than a stutter at the limit. It is also why
 * a beam build is the one build in the game with a reason to bolt an ablative
 * shield to an aeroplane that is never going to space.
 *
 * Charged in one pass before anything is resolved, because the trigger is read
 * by two separate steps — aircraft in `pilots_combat`, columns in
 * `convoys_combat` — and a charge inside each would bill a strafing run twice
 * for one second of fire. */
static void beam_charge(fly_craft *c, const fly_airframe *af, int firing, float dt) {
    if (af->beam_heat <= 0.0f || !firing || c->beam_locked) return;
    c->heat = fly_clampf(c->heat + af->beam_heat * dt *
                             (1.0f - 0.55f * fly_clampf(af->heat_shield, 0.0f, 1.0f)),
                         0.0f, 1.0f);
    if (c->heat >= 0.85f) c->beam_locked = 1;
}

/* And the question every trigger asks: will this weapon answer at all? */
static int weapon_ready(const fly_craft *c, const fly_airframe *af) {
    return af->beam_heat <= 0.0f || !c->beam_locked;
}

/* Guns. Everyone shoots by faction — outlaws at whoever is loaded, powers at
 * the armed aircraft of powers they are at war with, and anybody at whoever
 * shot them. The player watching a Marshal chase a Vulture off a Drover is the
 * whole reason for this; the player watching two liveries they have never
 * flown for settle something over a refinery is the new one. */
static void pilots_combat(fly_game *g, float dt) {
    int i, j;
    /* One thermal pass first, for everyone holding a trigger. */
    beam_charge(&g->player.craft, &g->player.airframe,
                g->player.controls.fire && fly__player_in_play(g) && !g->player.craft.on_ground, dt);
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active)
            beam_charge(&g->pilots[i].craft, &g->pilots[i].airframe,
                        g->pilots[i].controls.fire && !g->pilots[i].craft.on_ground, dt);
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        fly_pilot *a = &g->pilots[i];
        if (!a->active || a->craft.on_ground || a->airframe.gun_dps <= 0.0f) continue;
        if (a->fire_cooldown > 0.0f || !a->controls.fire) continue;
        if (!weapon_ready(&a->craft, &a->airframe)) continue;

        /* on the player */
        if (pilots_will_fire_player(g, a) && g->mode == FLY_MODE_FLIGHT &&
            g->docked < 0 && !g->player.craft.on_ground) {
            float aim = gun_solution(&a->craft, &a->airframe, &g->player.craft);
            if (aim > 0.0f) {
                g->player.craft.hp -= a->airframe.gun_dps * aim * dt * 6.0f * (1.0f - g->player.airframe.armor);
                g->hit_flash = 0.6f;
                a->fire_cooldown = 0.4f;
                if (fly_rng_f01(&g->rng) < 0.12f)
                    fly_game_log(g, "%s is on you!", a->name);
                if (g->player.craft.hp <= 0.0f) g->player.craft.crashed = 1;
                continue;
            }
        }
        /* on each other */
        for (j = 0; j < FLY_MAX_PILOTS; ++j) {
            fly_pilot *b = &g->pilots[j];
            float aim;
            if (i == j || !b->active || b->craft.on_ground) continue;
            if (!pilots_will_fire(g, a, b)) continue;
            aim = gun_solution(&a->craft, &a->airframe, &b->craft);
            if (aim <= 0.0f) continue;
            ++g->pilot_engagements;
            b->craft.hp -= a->airframe.gun_dps * aim * dt * 6.0f * (1.0f - b->airframe.armor);
            b->distress = 1.0f;
            b->mark = a->id;
            b->mark_player = 0;
            a->fire_cooldown = 0.4f;
            if (b->craft.hp <= 0.0f) fly__pilot_kill(g, a, b);
            break;
        }
    }

    /* the player's guns */
    if (g->player.controls.fire && g->mode == FLY_MODE_FLIGHT && !g->player.craft.on_ground &&
        weapon_ready(&g->player.craft, &g->player.airframe)) {
        for (i = 0; i < FLY_MAX_PILOTS; ++i) {
            fly_pilot *b = &g->pilots[i];
            float aim;
            if (!b->active || b->craft.on_ground) continue;
            aim = gun_solution(&g->player.craft, &g->player.airframe, &b->craft);
            if (aim <= 0.0f) continue;
            b->craft.hp -= g->player.airframe.gun_dps * aim * dt * 6.0f * (1.0f - b->airframe.armor);
            b->distress = 1.0f;
            b->mark = 0;
            b->mark_player = 1;
            g->gun_flash = 0.2f;
            if (b->craft.hp <= 0.0f) {
                /* A kill is now an act of foreign policy, and the ledger has to
                 * say so in every direction at once: whoever wanted this one
                 * dead is pleased, whoever it flew for is not, and if the
                 * player is wearing colours the ground underneath moves too.
                 * The bounty survives unchanged for outlaws, because shooting
                 * a pirate is the one thing every power agrees about. */
                fly__player_kill(g, b);
            }
            break;
        }
    }
}

/* ---------------- convoys ----------------
 *
 * The roads' traffic, on the same footing as the sky's: it moves the market,
 * it can be shot at by anybody who has a reason to, it shoots back, and what
 * is left of it is on the ground to be walked out to. See fly_convoy.h for
 * what a tier is and why the big ones run where they do.
 *
 * Everything below is the *world's* half of that — who is allowed to open fire
 * on whom, and what a burnt column costs. The column itself has no opinions. */

/* Where the player is, whatever they are doing. Convoy country is ground
 * country, so a column is just as interested in somebody walking up the road
 * as in somebody diving on it. */
static fly_v3 fly__player_pos(const fly_game *g) {
    return g->mode == FLY_MODE_WALK ? g->walker.pos
         : g->mode == FLY_MODE_RAIL ? g->rail.pos
                                    : g->player.craft.pos;
}

/* Would this pilot open fire on this column?
 *
 * The same shape as pilots_will_fire, and for the same reason: an outlaw robs
 * anything that is not its own, everybody else shoots the freight of a power
 * theirs is at war with, and a pact is a pact. A column under nobody's flag is
 * fair game to outlaws and to nobody else — which is what makes the unaligned
 * hauliers the ones that mostly get through. */
static int convoy_will_fire(const fly_game *g, const fly_pilot *a, const fly_convoy *c) {
    if (!a->active || !c->active || a->airframe.gun_dps <= 0.0f) return 0;
    if (a->faction == c->faction && c->faction != FLY_FACTION_FREE) return 0;
    if (fly_diplomacy_state(&g->world.dip, a->faction, c->faction) >= FLY_REL_PACT) return 0;
    if (a->kind == FLY_PILOT_PIRATE) return 1;
    if (!fly_pilot_is_combatant(a)) return 0;
    if (c->faction == FLY_FACTION_FREE) return 0;
    return fly_faction_hostile(&g->world, a->faction, c->faction);
}

/* And at the player. Unaligned, a column has no quarrel with you until you
 * give it one — which is exactly what `provoked` records. Pledged, the freight
 * of a power at war with yours is a target and you are one to it. */
static int convoy_will_fire_player(const fly_game *g, const fly_convoy *c) {
    int pf = g->player.faction;
    if (c->provoked) return 1;
    if (c->faction == FLY_FACTION_FREE || pf == FLY_FACTION_FREE) return 0;
    if (c->faction == pf) return 0;
    if (fly_diplomacy_state(&g->world.dip, c->faction, pf) >= FLY_REL_PACT) return 0;
    return fly_faction_hostile(&g->world, c->faction, pf);
}

/* A column comes apart: fire, and what it was carrying strewn along the road.
 *
 * The loot is rolled at the ground's own item level with the tier's bias, which
 * is the same rule a wreck follows — so a Leviathan burning in bad country is
 * the best thing on the ground for kilometres, and a Dray outside a farm town
 * is a crate of spares. `by_player` decides who wears the consequences: a kill
 * is an act of foreign policy here exactly as it is in the air. */
static void convoy_destroyed(fly_game *g, fly_convoy *c, int by_player) {
    const fly_convoy_class *k = fly_convoy_class_get(c->sea, c->tier);
    fly_v3 at = c->pos;
    float gz = fly_world_ground(&g->world, at.x, at.y);
    int near = fly_v3dist(at, fly__player_pos(g)) < FLY_PILOT_DETAIL_R;
    if (at.z < gz + 1.0f) at.z = gz + 1.0f;
    fly_game_spawn_blast(g, at, 9.0f + 2.2f * (float)k->trucks);
    /* Only where somebody could plausibly come and get it. A column burnt out
       of sight on the far side of the map would otherwise fill the ground-item
       table with loot nobody will ever walk to — the same rule a shot-down
       pilot's wreckage follows.

       A hull sheds the same way and keeps none of it: what comes off her lands
       in the water and the water takes it (see step_ground_items, which has
       always done that to anything that comes down wet). That is not an
       oversight about the sea, it is what the sea is for here — burning a
       coaster is denial rather than salvage, and what you are left with is the
       freight that never arrived, the standing, and whatever the powers make
       of it. */
    if (near)
        fly_game_scatter_drop(g, at, fly_v3scale(c->tangent, c->speed * 0.5f),
                              k->loot_rolls, k->loot_bias, c->id);
    ++g->convoys_lost;
    if (by_player) {
        int owner = c->faction;
        if (owner == FLY_FACTION_FREE) {
            g->reputation -= 4;
            if (g->reputation < 0) g->reputation = 0;
            fly_game_log(g, "You burnt %s. Hauliers talk.", c->name);
        } else if (g->player.faction != FLY_FACTION_FREE &&
                   fly_faction_hostile(&g->world, g->player.faction, owner)) {
            fly_game_credit_faction(g, g->player.faction, 7);
            fly__site_push_at(g, at, g->player.faction, FLY_KILL_PUSH);
            fly_game_log(g, "%s stopped — %s freight, and it was not going to arrive",
                         c->name, fly_faction_name(owner));
        } else {
            g->reputation -= 6;
            if (g->reputation < 0) g->reputation = 0;
            fly_game_log(g, "You burnt %s of the %s. Word travels.",
                         c->name, fly_faction_name(owner));
        }
        if (owner != FLY_FACTION_FREE) fly_game_credit_faction(g, owner, -11);
        g->xp += 8 + 6 * (int)c->tier;
    } else if (near) {
        fly_game_log(g, "Radar: %s burning on the %s", c->name,
                     c->sea ? "water" : "road");
    }
    memset(c, 0, sizeof *c);
    if (g->convoy_count > 0) --g->convoy_count;
}

/* Guns, both ways. Aircraft on the column and the column on aircraft, with the
 * player on both sides of it. */
static void convoys_combat(fly_game *g, float dt) {
    int i, j;
    for (i = 0; i < FLY_CONVOY_MAX; ++i) {
        fly_convoy *c = &g->convoys[i];
        float threat;
        if (!c->active) continue;

        /* the player's guns */
        if (g->player.controls.fire && g->mode == FLY_MODE_FLIGHT && !g->player.craft.on_ground &&
            weapon_ready(&g->player.craft, &g->player.airframe)) {
            float aim = fly_convoy_gun_solution(&g->player.craft, &g->player.airframe, c);
            if (aim > 0.0f) {
                g->gun_flash = 0.2f;
                c->provoked = 1;
                if (fly_convoy_damage(c, g->player.airframe.gun_dps * aim * dt * 6.0f)) {
                    convoy_destroyed(g, c, 1);
                    continue;
                }
            }
        }
        /* everybody else's */
        for (j = 0; j < FLY_MAX_PILOTS; ++j) {
            fly_pilot *a = &g->pilots[j];
            float aim;
            if (!a->active || a->craft.on_ground || !a->controls.fire) continue;
            if (a->fire_cooldown > 0.0f) continue;
            if (!weapon_ready(&a->craft, &a->airframe)) continue;
            if (!convoy_will_fire(g, a, c)) continue;
            aim = fly_convoy_gun_solution(&a->craft, &a->airframe, c);
            if (aim <= 0.0f) continue;
            a->fire_cooldown = 0.4f;
            ++g->pilot_engagements;
            if (fly_convoy_damage(c, a->airframe.gun_dps * aim * dt * 6.0f)) {
                ++a->kills;
                convoy_destroyed(g, c, 0);
                break;
            }
        }
        if (!c->active) continue;

        /* And the answer, from whatever is left of the column: the guns are
           counted after this step's damage, not before, so a truck that has
           just burned does not fire back on its way out. Flak does not need a
           firing solution, which is why what it does per second is a fraction
           of what a gun on a nose does — see fly_convoy_air_solution. */
        threat = fly_convoy_threat(c);
        if (threat > 0.0f) {
            if (convoy_will_fire_player(g, c) && g->mode == FLY_MODE_FLIGHT &&
                g->docked < 0 && !g->player.craft.on_ground) {
                float aim = fly_convoy_air_solution(c, g->player.craft.pos, g->player.craft.vel);
                if (aim > 0.0f) {
                    g->player.craft.hp -= threat * aim * dt * (1.0f - g->player.airframe.armor);
                    g->hit_flash = 0.6f;
                    if (c->fire_cooldown <= 0.0f) {
                        c->fire_cooldown = 3.0f;
                        fly_game_log(g, "%s is shooting back", c->name);
                    }
                    if (g->player.craft.hp <= 0.0f) g->player.craft.crashed = 1;
                }
            }
            for (j = 0; j < FLY_MAX_PILOTS; ++j) {
                fly_pilot *b = &g->pilots[j];
                float aim;
                if (!b->active || b->craft.on_ground) continue;
                if (!convoy_will_fire(g, b, c)) continue;   /* it shoots its shooters */
                aim = fly_convoy_air_solution(c, b->craft.pos, b->craft.vel);
                if (aim <= 0.0f) continue;
                b->craft.hp -= threat * aim * dt * (1.0f - b->airframe.armor);
                b->distress = 1.0f;
                /* Being shot at by the thing it is attacking is what makes the
                   attack a decision: `distress` plus a mark is what sends a
                   pilot into HUNT or EVADE, and flak with nobody's name on it
                   would be weather. */
                b->mark = 0;
                b->mark_player = 0;
                b->mark_convoy = c->id;
                if (b->craft.hp <= 0.0f) b->craft.crashed = 1;
            }
        }
    }
}

/* ---------------- ordnance ----------------
 *
 * The other half of combat, and the half a target can do something about.
 *
 * A gun is resolved where the trigger is pulled: the line is drawn, the damage
 * lands, and the only question either aeroplane ever gets to answer is whether
 * the nose was on. Everything below puts an *object* in the air instead, with
 * several seconds of life in it — and those seconds are the whole point. A
 * launch is the beginning of an engagement rather than the end of one, and the
 * five answers to it (turn, run, go cold, mask, decoy) are things you fly
 * rather than things you buy.
 *
 * fly_ord.c owns the flying and the seeking and knows nothing about this game;
 * this section owns who may shoot at whom, what a hit costs, and the one
 * snapshot of the world the rounds are stepped against. The split is the same
 * one `fly_convoy` makes, and for the same reason: the interesting rules are
 * testable without a world, and the world's rules live where the world does.
 */

/* Everything a round could be interested in at once: the player, the sky, the
 * roads and every decoy currently burning. */
#define FLY__ORD_CONTACT_MAX (1 + FLY_MAX_PILOTS + FLY_CONVOY_MAX + FLY_ORD_MAX)

static fly_ord_ref fly__ord_ref(int kind, uint32_t id) {
    fly_ord_ref r;
    r.kind = (unsigned char)kind;
    r.id = id;
    return r;
}

static int fly__ref_eq(fly_ord_ref a, fly_ord_ref b) {
    return a.kind == b.kind && a.id == b.id;
}

/* Is the player a thing in the world right now? Docked, walking or riding, the
 * aeroplane is not somewhere a missile can reach it — which is deliberate:
 * losing a hull to a seeker fired at a parked aircraft you were not flying is
 * an outcome with no decision anywhere in it. */
static int fly__player_in_play(const fly_game *g) {
    return g->mode == FLY_MODE_FLIGHT && g->docked < 0 && !g->player.craft.crashed;
}

/* One snapshot, built once per step and handed to every round.
 *
 * Once per step rather than once per round, because a round that saw a world
 * already advanced by the round before it would make the outcome depend on
 * array order — the same defect the pilot tick was fixed for, and the same
 * reason this is assembled up front. */
static int fly__ord_contacts(const fly_game *g, fly_ord_contact *out) {
    int n = 0, i;
    if (fly__player_in_play(g)) {
        fly_ord_contact *c = &out[n++];
        memset(c, 0, sizeof *c);
        c->ref = fly__ord_ref(FLY_ORD_ACTOR_PLAYER, 0);
        c->faction = (unsigned char)g->player.faction;
        c->pos = g->player.craft.pos;
        c->vel = g->player.craft.vel;
        c->nose = fly_qrot(g->player.craft.ori, fly_v3mk(1, 0, 0));
        c->throttle = g->player.craft.throttle;
        c->mass = fly_craft_mass(&g->player.craft, &g->player.airframe, fly_game_cargo_mass(g));
        c->radius = 7.0f;
        c->ecm = g->player.airframe.ecm;
        c->airborne = !g->player.craft.on_ground;
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        const fly_pilot *p = &g->pilots[i];
        fly_ord_contact *c;
        if (!p->active) continue;
        c = &out[n++];
        memset(c, 0, sizeof *c);
        c->ref = fly__ord_ref(FLY_ORD_ACTOR_PILOT, p->id);
        c->faction = (unsigned char)p->faction;
        c->pos = p->craft.pos;
        c->vel = p->craft.vel;
        c->nose = fly_qrot(p->craft.ori, fly_v3mk(1, 0, 0));
        c->throttle = p->craft.throttle;
        c->mass = fly_craft_mass(&p->craft, &p->airframe, fly_pilot_cargo_mass(p));
        c->radius = 7.0f;
        c->ecm = p->airframe.ecm;
        c->airborne = !p->craft.on_ground;
    }
    for (i = 0; i < FLY_CONVOY_MAX; ++i) {
        const fly_convoy *v = &g->convoys[i];
        fly_ord_contact *c;
        if (!v->active) continue;
        c = &out[n++];
        memset(c, 0, sizeof *c);
        c->ref = fly__ord_ref(FLY_ORD_ACTOR_CONVOY, v->id);
        c->faction = v->faction;
        c->pos = v->pos;
        c->vel = fly_v3scale(v->tangent, v->speed);
        c->nose = v->tangent;
        /* A column is a big cold thing on a road. It is easy to hit and
         * impossible to lock: no seeker in the game will take a truck, which is
         * what keeps the air-to-air weapons out of the ground-attack business
         * and leaves that job to the bombs it belongs to. */
        c->mass = 6000.0f;
        c->radius = 14.0f;
        c->airborne = 0;
    }
    /* And the decoys, which are contacts like any other. That is the whole
     * implementation of a countermeasure: no special case in the seeker, no
     * roll to see whether it worked — just a very bright thing, briefly, in a
     * place the seeker is looking. */
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        const fly_ord *o = &g->ord[i];
        fly_ord_contact *c;
        if (!o->active) continue;
        if (o->decoy_heat <= 0.0f && o->decoy_radar <= 0.0f) continue;
        c = &out[n++];
        memset(c, 0, sizeof *c);
        c->ref = fly__ord_ref(FLY_ORD_ACTOR_DECOY, o->id);
        c->faction = o->faction;
        c->pos = o->pos;
        c->vel = o->vel;
        c->nose = fly_v3norm(o->vel);
        c->heat_decoy = o->decoy_heat;
        c->radar_decoy = o->decoy_radar;
        c->radius = 0.0f;
        c->airborne = 1;
    }
    return n;
}

/* A free slot, or the oldest round in the pool.
 *
 * Evicting rather than refusing: a round that was never created is invisible
 * and costs the shooter a magazine it can rearm, where a full pool that
 * refused would silently disarm whoever launched last — and the oldest round
 * is by construction the one nearest the end of its life anyway. */
static fly_ord *fly__ord_slot(fly_game *g) {
    int i, pick = 0;
    float oldest = -1.0f;
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        if (!g->ord[i].active) return &g->ord[i];
        if (g->ord[i].age > oldest) { oldest = g->ord[i].age; pick = i; }
    }
    return &g->ord[pick];
}

/* Who this shooter would launch at: the best solution in the envelope right
 * now, which is what a lock is when there is no radar dish to slew.
 *
 * The same function serves the AI's trigger and the player's HUD cue, so the
 * pipper cannot promise a shot the launcher will refuse — and it is the same
 * arrangement `gun_solution` already has with `pilots_will_fire`. */
/* Would this shooter open fire on this contact *at all*?
 *
 * The launcher has to answer exactly the question the gun answers, or a patrol
 * that will not machine-gun a rival's courier will happily put a missile
 * through one — which is the same class of defect the codebase already guards
 * against between `fly_pilot_think` and `pilots_will_fire`, and it is worse
 * here because a missile crosses two kilometres to commit the mistake.
 *
 * The player is deliberately not filtered. Their guns are not either: an
 * operator may shoot at whatever they like and wear the consequences, and the
 * consequences are the mechanic. */
static int fly__ord_may_fire(const fly_game *g, fly_ord_ref shooter, fly_ord_ref mark) {
    const fly_pilot *a = NULL;
    int i;
    if (shooter.kind == FLY_ORD_ACTOR_PLAYER) return 1;
    if (shooter.kind != FLY_ORD_ACTOR_PILOT) return 1;   /* a column's own rules */
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active && g->pilots[i].id == shooter.id) { a = &g->pilots[i]; break; }
    if (!a) return 0;
    if (mark.kind == FLY_ORD_ACTOR_PLAYER) return pilots_will_fire_player(g, a);
    if (mark.kind == FLY_ORD_ACTOR_PILOT) {
        for (i = 0; i < FLY_MAX_PILOTS; ++i)
            if (g->pilots[i].active && g->pilots[i].id == mark.id)
                return pilots_will_fire(g, a, &g->pilots[i]);
        return 0;
    }
    if (mark.kind == FLY_ORD_ACTOR_CONVOY) {
        for (i = 0; i < FLY_CONVOY_MAX; ++i)
            if (g->convoys[i].active && g->convoys[i].id == mark.id)
                return convoy_will_fire(g, a, &g->convoys[i]);
        return 0;
    }
    return 0;
}

static int fly__ord_pick_target(const fly_game *g, fly_ord_ref shooter, int faction,
                                fly_v3 pos, fly_v3 vel, int kind, float range,
                                fly_ord_ref *out, fly_v3 *out_pos) {
    fly_ord_contact cs[FLY__ORD_CONTACT_MAX];
    int n = fly__ord_contacts(g, cs), i, found = 0;
    float best = 1.0e30f;
    int want_air = !fly_ord_is_ballistic(kind);
    for (i = 0; i < n; ++i) {
        const fly_ord_contact *c = &cs[i];
        float d;
        if (fly__ref_eq(c->ref, shooter)) continue;
        if (c->radius <= 0.0f) continue;                    /* a decoy is not a mark */
        if (c->faction == faction && faction != FLY_FACTION_FREE) continue;
        if (want_air && !c->airborne) continue;             /* seekers do not take trucks */
        if (!want_air && c->airborne) continue;             /* and bombs do not take aeroplanes */
        if (!fly__ord_may_fire(g, shooter, c->ref)) continue;
        if (!fly_ord_launch_ok(kind, range, pos, vel, c->pos, c->vel)) continue;
        d = fly_v3dist(c->pos, pos);
        if (d < best) { best = d; *out = c->ref; if (out_pos) *out_pos = c->pos; found = 1; }
    }
    return found;
}

/* Put a salvo in the air.
 *
 * Rounds come off the rack one at a time and each one costs a round, so a
 * four-shot rocket pod empties six times and a four-round seeker rack empties
 * once. The spread is derived from the round's index rather than drawn, which
 * keeps a salvo deterministic and — more usefully — keeps it a *pattern*: a
 * rocket salvo walks across a target rather than landing in one place, which is
 * what makes it worth having against a column and nearly worthless against a
 * turning aeroplane. */
static int fly__ord_fire(fly_game *g, fly_ord_ref who, int faction, fly_craft *c,
                         const fly_airframe *af, fly_ord_ref target) {
    const fly_ord_class *k = fly_ord_class_get(af->ord_kind);
    fly_v3 nose, right, up;
    int shot, fired = 0;
    if (!k || c->ammo <= 0 || c->ord_cooldown > 0.0f) return 0;
    nose = fly_qrot(c->ori, fly_v3mk(1, 0, 0));
    right = fly_qrot(c->ori, fly_v3mk(0, -1, 0));
    up = fly_qrot(c->ori, fly_v3mk(0, 0, 1));
    for (shot = 0; shot < k->salvo && c->ammo > 0; ++shot) {
        fly_ord *o = fly__ord_slot(g);
        fly_v3 at, v;
        float side = ((shot & 1) ? 1.0f : -1.0f) * (0.5f + 0.5f * (float)(shot / 2));
        at = fly_v3add(c->pos, fly_v3add(fly_v3scale(right, side * 1.6f),
                                         fly_v3scale(up, -0.8f)));
        if (fly_ord_is_ballistic(af->ord_kind)) {
            /* Released, not fired: it leaves with the aeroplane's own velocity
             * and a shove downward, which is why a bomb run is a thing you fly
             * rather than a thing you aim. */
            v = fly_v3add(c->vel, fly_v3scale(up, -2.5f));
        } else {
            v = fly_v3add(c->vel, fly_v3scale(nose, 45.0f));
            /* The unguided ones get the spread; a guided round is steering
             * from the first frame and would only correct it back out. */
            if (!k->turn) v = fly_v3add(v, fly_v3scale(right, side * 5.0f));
        }
        fly_ord_launch(o, ++g->next_ord_id, af->ord_kind, faction, who, target,
                       at, v, af->ord_damage > 0.0f ? af->ord_damage : 1.0f);
        --c->ammo;
        ++fired;
    }
    if (fired) c->ord_cooldown = k->reload;
    return fired;
}

/* And shed a decoy. The same machine with no warhead in it — which is exactly
 * how it is implemented, because a countermeasure that had its own subsystem
 * would be a countermeasure that could disagree with the seeker about what it
 * looks like. */
static int fly__ord_decoy(fly_game *g, fly_ord_ref who, int faction, fly_craft *c,
                          const fly_airframe *af) {
    const fly_ord_class *k = fly_ord_class_get(af->cm_kind);
    fly_v3 up, back;
    int shot, fired = 0;
    if (!k || c->cm <= 0 || c->cm_cooldown > 0.0f) return 0;
    up = fly_qrot(c->ori, fly_v3mk(0, 0, 1));
    back = fly_qrot(c->ori, fly_v3mk(-1, 0, 0));
    for (shot = 0; shot < k->salvo && c->cm > 0; ++shot) {
        fly_ord *o = fly__ord_slot(g);
        fly_v3 at = fly_v3add(c->pos, fly_v3scale(up, -1.2f));
        /* Down and back, away from the aeroplane that shed it. It has to
         * separate, and quickly: a flare that stayed with the aircraft would be
         * a flare a seeker flies into on its way through. */
        fly_v3 v = fly_v3add(c->vel,
                             fly_v3add(fly_v3scale(up, -9.0f - 3.0f * (float)shot),
                                       fly_v3scale(back, 6.0f)));
        fly_ord_launch(o, ++g->next_ord_id, af->cm_kind, faction,
                       who, fly__ord_ref(FLY_ORD_ACTOR_NONE, 0), at, v, 1.0f);
        --c->cm;
        ++fired;
    }
    if (fired) c->cm_cooldown = k->reload;
    return fired;
}

/* What a burst does where it goes off.
 *
 * Everything within the blast takes damage falling off with distance, armour
 * included, and whoever fired it wears the consequence — a kill by seeker is
 * the same act of foreign policy a kill by gun is, and goes through the same
 * two functions.
 *
 * Friendly fire is off, and that is a decision rather than an oversight. The
 * rounds are already faction-filtered at the seeker so they cannot chase their
 * own side; letting a blast hurt them anyway would mean a patrol that bombed a
 * column killed the patrol next to it, and the world would visibly eat itself
 * over a mechanic no player ever asked for. */
static void fly__ord_detonate(fly_game *g, fly_ord *o) {
    int i;
    fly_pilot *owner = NULL;
    if (o->damage <= 0.0f) return;   /* a spent flare is not an explosion */
    if (o->owner.kind == FLY_ORD_ACTOR_PILOT)
        for (i = 0; i < FLY_MAX_PILOTS; ++i)
            if (g->pilots[i].active && g->pilots[i].id == o->owner.id) {
                owner = &g->pilots[i];
                break;
            }

    if (fly__player_in_play(g) &&
        !(o->faction == g->player.faction && g->player.faction != FLY_FACTION_FREE) &&
        o->owner.kind != FLY_ORD_ACTOR_PLAYER) {
        float dmg = fly_ord_blast_damage(o, fly_v3dist(g->player.craft.pos, o->pos));
        if (dmg > 0.0f) {
            g->player.craft.hp -= dmg * (1.0f - g->player.airframe.armor);
            g->hit_flash = 0.6f;
            fly_game_log(g, "Hit — %s", fly_ord_kind_name(o->kind));
            if (g->player.craft.hp <= 0.0f) g->player.craft.crashed = 1;
        }
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        fly_pilot *p = &g->pilots[i];
        float dmg;
        if (!p->active || p->craft.crashed) continue;
        if (owner == p) continue;
        if (p->faction == o->faction && o->faction != FLY_FACTION_FREE) continue;
        dmg = fly_ord_blast_damage(o, fly_v3dist(p->craft.pos, o->pos));
        if (dmg <= 0.0f) continue;
        p->craft.hp -= dmg * (1.0f - p->airframe.armor);
        p->distress = 1.0f;
        /* Being hit is what makes it a fight. A round with nobody's name on it
         * would be weather, and the mark is what sends a pilot into HUNT or
         * EVADE — the same rule the column's flak lives under. */
        if (o->owner.kind == FLY_ORD_ACTOR_PLAYER) { p->mark = 0; p->mark_player = 1; }
        else if (owner) { p->mark = owner->id; p->mark_player = 0; }
        if (p->craft.hp <= 0.0f) {
            if (o->owner.kind == FLY_ORD_ACTOR_PLAYER) fly__player_kill(g, p);
            else if (owner) fly__pilot_kill(g, owner, p);
            else p->craft.crashed = 1;
        }
    }
    for (i = 0; i < FLY_CONVOY_MAX; ++i) {
        fly_convoy *v = &g->convoys[i];
        float dmg;
        if (!v->active) continue;
        if (v->faction == o->faction && o->faction != FLY_FACTION_FREE) continue;
        dmg = fly_ord_blast_damage(o, fly_v3dist(v->pos, o->pos));
        if (dmg <= 0.0f) continue;
        if (o->owner.kind == FLY_ORD_ACTOR_PLAYER) v->provoked = 1;
        if (fly_convoy_damage(v, dmg)) {
            if (owner) ++owner->kills;
            convoy_destroyed(g, v, o->owner.kind == FLY_ORD_ACTOR_PLAYER);
        }
    }
    /* Something to see. Sized off the warhead rather than off the airframe, so
     * a bomb is a landmark and a seeker's proximity burst is a puff. */
    fly_game_spawn_blast(g, o->pos, 2.0f + o->blast * 0.30f);
}

/* The columns' own missiles.
 *
 * An armoured column and above carries one, and it changes what strafing them
 * is: flak is a tax you pay for the seconds you spend over the road, and this
 * is a thing that follows you home. It is the clearest place in the game where
 * a flare pays for the bay it cost — and it is why the top tier is worth
 * escorting rather than merely worth robbing.
 *
 * A column will not launch at somebody it would not shoot at, which is the same
 * predicate its guns already use, so the missile cannot be the thing that
 * starts a war the flak would not have. */
static void convoy_sam(fly_game *g, fly_convoy *c, float dt) {
    fly_ord_ref me = fly__ord_ref(FLY_ORD_ACTOR_CONVOY, c->id);
    fly_ord_ref target = fly__ord_ref(FLY_ORD_ACTOR_NONE, 0);
    fly_v3 from;
    const fly_ord_class *k = fly_ord_class_get(FLY_ORD_SAM);
    float best = 1.0e30f;
    int i, found = 0;
    if (c->tier < FLY_CONVOY_SAM_TIER || c->trucks <= 0 || !k) return;
    c->sam_cooldown -= dt;
    if (c->sam_cooldown > 0.0f || c->sam_ammo <= 0) return;
    from = fly_v3mk(c->pos.x, c->pos.y, c->pos.z + 3.0f);
    if (fly__player_in_play(g) && !g->player.craft.on_ground && convoy_will_fire_player(g, c)) {
        float d = fly_v3dist(g->player.craft.pos, from);
        if (d < FLY_CONVOY_SAM_RANGE) {
            best = d;
            target = fly__ord_ref(FLY_ORD_ACTOR_PLAYER, 0);
            found = 1;
        }
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        const fly_pilot *p = &g->pilots[i];
        float d;
        if (!p->active || p->craft.on_ground) continue;
        /* The same rule the aircraft launch under: a coarse pilot cannot break
         * or decoy, so firing a missile at one is an execution rather than an
         * engagement. */
        if (!p->detail) continue;
        if (!convoy_will_fire(g, p, c)) continue;
        d = fly_v3dist(p->craft.pos, from);
        if (d < FLY_CONVOY_SAM_RANGE && d < best) {
            best = d;
            target = fly__ord_ref(FLY_ORD_ACTOR_PILOT, p->id);
            found = 1;
        }
    }
    if (!found) return;
    {
        /* Straight up out of the bed. A column does not manoeuvre and cannot
         * point anything, so the launch is not a shot that has to be aimed —
         * the round does all of the work, which is also why it is slower and
         * turns worse than anything fired off a wing. */
        fly_ord *o = fly__ord_slot(g);
        fly_v3 v = fly_v3mk(0, 0, 55.0f);
        fly_ord_launch(o, ++g->next_ord_id, FLY_ORD_SAM, c->faction, me, target,
                       from, v, 1.0f);
        --c->sam_ammo;
        c->sam_cooldown = k->reload;
        c->alert = 1.0f;
        if (fly_v3dist(from, fly__player_pos(g)) < fly_game_radar_range(g))
            fly_game_log(g, "%s has launched", c->name);
    }
}

int fly_game_ord_solution(const fly_game *g, fly_v3 *out) {
    fly_ord_ref target = fly__ord_ref(FLY_ORD_ACTOR_NONE, 0);
    fly_ord_ref me = fly__ord_ref(FLY_ORD_ACTOR_PLAYER, 0);
    fly_v3 tpos;
    const fly_ord_class *k;
    if (!g->player.airframe.ord_kind || !fly__player_in_play(g) || g->player.craft.on_ground) return 0;
    k = fly_ord_class_get(g->player.airframe.ord_kind);
    if (!k) return 0;
    /* An unguided round is aimed rather than locked, so what the glass has to
     * answer is not "is something in the envelope" but "would releasing now
     * put it on something". Solve the trajectory the round will actually fly —
     * the same integration, so the cue cannot promise a throw the bomb will
     * not make — and then ask whether it lands on anything worth hitting.
     *
     * Without that second half the cue is on permanently: a bomb dropped from
     * an aeroplane always reaches the ground somewhere, so "there is an impact
     * point" is true from the moment the wheels leave the apron and tells a
     * bomber precisely nothing. */
    if (!k->turn) {
        fly_v3 nose = fly_qrot(g->player.craft.ori, fly_v3mk(1, 0, 0));
        fly_v3 v = fly_ord_is_ballistic(g->player.airframe.ord_kind)
                       ? g->player.craft.vel
                       : fly_v3add(g->player.craft.vel, fly_v3scale(nose, k->speed));
        fly_v3 at;
        int c;
        if (!fly_ord_impact_point(g->player.airframe.ord_kind, g->player.craft.pos, v,
                                  fly_world_ground_cb, (void *)&g->world, 20.0f, &at))
            return 0;
        if (out) *out = at;
        /* Within the blast, not within a metre: a bomb that lands at the edge
         * of the column still burns trucks, and a pipper that only lit on a
         * direct hit would be a pipper nobody could use. */
        for (c = 0; c < FLY_CONVOY_MAX; ++c) {
            const fly_convoy *cv = &g->convoys[c];
            if (!cv->active) continue;
            if (cv->faction == g->player.faction &&
                g->player.faction != FLY_FACTION_FREE) continue;
            if (fly_v3dist(cv->pos, at) < k->blast) return 1;
        }
        /* Aircraft too, for the rockets: an unguided salvo at something in
         * front of the nose is the shot a rocket pod is for. */
        if (!fly_ord_is_ballistic(g->player.airframe.ord_kind)) {
            fly_ord_ref target = fly__ord_ref(FLY_ORD_ACTOR_NONE, 0);
            return fly__ord_pick_target(g, me, g->player.faction, g->player.craft.pos,
                                        g->player.craft.vel, g->player.airframe.ord_kind,
                                        g->player.airframe.ord_range, &target, out);
        }
        return 0;
    }
    if (!fly__ord_pick_target(g, me, g->player.faction, g->player.craft.pos, g->player.craft.vel,
                              g->player.airframe.ord_kind, g->player.airframe.ord_range,
                              &target, &tpos)) return 0;
    if (out) *out = tpos;
    return 1;
}

float fly_game_ord_inbound(const fly_game *g, fly_v3 *from, int *kind) {
    float soonest = -1.0f;
    int i;
    if (!fly__player_in_play(g)) return -1.0f;
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        const fly_ord *o = &g->ord[i];
        fly_v3 to;
        float d, closing;
        if (!o->active || o->damage <= 0.0f) continue;
        if (o->owner.kind == FLY_ORD_ACTOR_PLAYER) continue;
        if (o->faction == g->player.faction && g->player.faction != FLY_FACTION_FREE)
            continue;
        to = fly_v3sub(g->player.craft.pos, o->pos);
        d = fly_v3len(to);
        if (d < 1.0f || d > 3000.0f) continue;
        to = fly_v3scale(to, 1.0f / d);
        closing = fly_v3dot(o->vel, to);
        /* The same geometry `fly_pilot_ord_threat` uses, and deliberately so:
         * the warning the player gets and the warning an AI acts on have to be
         * the same warning, or one of them is playing a different game. */
        if (closing < 20.0f) continue;
        if (fly_v3dot(fly_v3norm(o->vel), to) < 0.90f) continue;
        if (soonest < 0.0f || d / closing < soonest) {
            soonest = d / closing;
            if (from) *from = o->pos;
            if (kind) *kind = o->kind;
        }
    }
    return soonest;
}

/* Everything in the air, once, against one snapshot. */
static void ord_step(fly_game *g, float dt) {
    fly_ord_contact cs[FLY__ORD_CONTACT_MAX];
    int n = fly__ord_contacts(g, cs), i;
    for (i = 0; i < FLY_ORD_MAX; ++i) {
        fly_ord *o = &g->ord[i];
        int rc;
        if (!o->active) continue;
        rc = fly_ord_step(o, cs, n, fly_world_ground_cb, (void *)&g->world,
                          g->weather.wind, dt);
        if (rc == -1) continue;
        /* A struck contact and a spent round end the same way: the thing goes
         * off where it is, and what that costs is worked out from where the
         * blast fell rather than from what it happened to touch. One path, so a
         * near miss and a direct hit differ by a distance rather than by a
         * branch. */
        fly__ord_detonate(g, o);
        o->active = 0;
    }
}

/* The trigger, for everyone who has one.
 *
 * Launching is a control latch, so this reads exactly the same field for the
 * player and for every pilot — which is the whole reason the latch is on the
 * controls rather than in a command of its own. */
static void ord_triggers(fly_game *g, float dt) {
    int i;
    (void)dt;
    if (fly__player_in_play(g) && !g->player.craft.on_ground) {
        if (g->player.controls.launch && g->player.airframe.ord_kind && g->player.craft.ammo > 0 &&
            g->player.craft.ord_cooldown <= 0.0f) {
            fly_ord_ref target = fly__ord_ref(FLY_ORD_ACTOR_NONE, 0);
            fly_ord_ref me = fly__ord_ref(FLY_ORD_ACTOR_PLAYER, 0);
            fly_v3 tpos;
            int have = fly__ord_pick_target(g, me, g->player.faction, g->player.craft.pos,
                                            g->player.craft.vel, g->player.airframe.ord_kind,
                                            g->player.airframe.ord_range, &target, &tpos);
            /* An unguided round does not need a mark and is released whenever
             * asked; a guided one without a solution would be a round thrown
             * away, so the trigger simply does not answer and the HUD has
             * already said why. */
            if (have || !fly_ord_class_get(g->player.airframe.ord_kind)->turn) {
                if (fly__ord_fire(g, me, g->player.faction, &g->player.craft, &g->player.airframe,
                                  target) > 0) {
                    /* Shooting at a column is what makes it your enemy, whatever
                     * flag either of you is wearing. */
                    if (target.kind == FLY_ORD_ACTOR_CONVOY) {
                        int c;
                        for (c = 0; c < FLY_CONVOY_MAX; ++c)
                            if (g->convoys[c].active && g->convoys[c].id == target.id)
                                g->convoys[c].provoked = 1;
                    }
                }
            }
        }
        if (g->player.controls.decoy && g->player.airframe.cm_kind && g->player.craft.cm > 0)
            fly__ord_decoy(g, fly__ord_ref(FLY_ORD_ACTOR_PLAYER, 0), g->player.faction,
                           &g->player.craft, &g->player.airframe);
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        fly_pilot *p = &g->pilots[i];
        fly_ord_ref me;
        if (!p->active || p->craft.on_ground || p->craft.crashed) continue;
        /* Ordnance is for the aircraft being flown in full, and only those.
         *
         * A coarse pilot is moved along its leg and turned to point at its
         * mark, which is why the TODO records coarse combat converting ten to
         * thirty times as often as flown combat does. A missile handed to that
         * path would be a missile fired from a perfect firing position at a
         * target that cannot break, cannot descend and cannot decoy — the
         * existing asymmetry, multiplied. So out of view the world fights with
         * guns, exactly as it did, and everything added here happens where
         * somebody can see it and where both sides can fly. */
        if (!p->detail) continue;
        me = fly__ord_ref(FLY_ORD_ACTOR_PILOT, p->id);
        if (p->controls.launch && p->airframe.ord_kind && p->craft.ammo > 0 &&
            p->craft.ord_cooldown <= 0.0f) {
            fly_ord_ref target = fly__ord_ref(FLY_ORD_ACTOR_NONE, 0);
            fly_v3 tpos;
            int have = fly__ord_pick_target(g, me, p->faction, p->craft.pos, p->craft.vel,
                                            p->airframe.ord_kind, p->airframe.ord_range,
                                            &target, &tpos);
            if (have || !fly_ord_class_get(p->airframe.ord_kind)->turn) {
                if (fly__ord_fire(g, me, p->faction, &p->craft, &p->airframe, target) > 0)
                    ++g->pilot_engagements;
            }
        }
        if (p->controls.decoy && p->airframe.cm_kind && p->craft.cm > 0)
            fly__ord_decoy(g, me, p->faction, &p->craft, &p->airframe);
    }
    for (i = 0; i < FLY_CONVOY_MAX; ++i)
        if (g->convoys[i].active) convoy_sam(g, &g->convoys[i], dt);
}

/* The way a column is on, whichever network that is. Every call into
 * fly_convoy goes through here, so the two nets are looked up in one place and
 * a column that has outlived its line — which cannot happen, since neither net
 * changes while a world runs, but which would be a subscript off the end if it
 * did — comes back as a way with no points in it and is refused. */
static int convoy_way(const fly_game *g, const fly_convoy *c, fly_way *out) {
    return c->sea ? fly_sea_as_way(&g->lanes, c->line, out)
                  : fly_road_as_way(&g->roads, c->line, out);
}

/* How many columns the world's networks want on them at once.
 *
 * A road gets a column every fourth road and a lane gets a hull each, which is
 * not the same rule twice by accident: there are two dozen roads and a handful
 * of lanes, and a sea with one ship on it every third crossing is a sea with
 * nothing on it. A hull is also worth several columns to the market, so the
 * count that reads low is carrying the most freight in the world. */
static int convoy_quota(const fly_game *g) {
    int n = g->roads.count / 4 + g->lanes.count + 1;
    return n > FLY_CONVOY_MAX ? FLY_CONVOY_MAX : n;
}

/* Put one column on a line that has none. Returns 1 if it managed it.
 *
 * Which medium first is a comparison rather than a coin. The quota above
 * budgets a column per four roads and a hull per lane, so the medium that is
 * furthest below its own share is the one that gets the next column — and
 * because a refusal is common (a line with nothing worth hauling on it today
 * says so), the attempts alternate, so a sea that cannot fill a hold falls
 * back to the roads inside the same dispatch instead of leaving the slot
 * empty. A world with no lanes in it never takes the sea branch at all and
 * makes exactly the draws it made before there were any.
 *
 * `line` is the index within the chosen network, which is what the column
 * remembers along with which network it was. */
static int convoy_dispatch(fly_game *g) {
    int slot, tries, i;
    int live[2], want[2], prefer;
    if (g->roads.count <= 0 && g->lanes.count <= 0) return 0;
    for (slot = 0; slot < FLY_CONVOY_MAX; ++slot) if (!g->convoys[slot].active) break;
    if (slot == FLY_CONVOY_MAX) return 0;
    live[0] = live[1] = 0;
    for (i = 0; i < FLY_CONVOY_MAX; ++i)
        if (g->convoys[i].active) ++live[g->convoys[i].sea ? 1 : 0];
    want[0] = g->roads.count / 4 + 1;
    want[1] = g->lanes.count;
    /* Cross-multiplied rather than divided: which of live/want is smaller,
       without a division by a quota that can be zero. */
    prefer = want[1] > 0 && live[1] * want[0] < live[0] * want[1];
    for (tries = 0; tries < 12; ++tries) {
        int busy = 0, sea = (tries & 1) ? !prefer : prefer;
        int n = sea ? g->lanes.count : g->roads.count, line;
        fly_way way;
        if (n <= 0) continue;
        line = (int)fly_rng_range(&g->convoy_rng, (uint32_t)n);
        for (i = 0; i < FLY_CONVOY_MAX; ++i)
            if (g->convoys[i].active && g->convoys[i].sea == (unsigned char)sea &&
                g->convoys[i].line == line) busy = 1;
        if (busy) continue;
        if (!(sea ? fly_sea_as_way(&g->lanes, line, &way)
                  : fly_road_as_way(&g->roads, line, &way))) continue;
        if (fly_convoy_spawn(&g->convoys[slot], ++g->next_convoy_id, &way,
                             &g->world, line, &g->convoy_rng) != 0) continue;
        fly_convoy_load(&g->convoys[slot], &g->world, &way);
        ++g->convoy_count;
        return 1;
    }
    return 0;
}

static void convoys_step(fly_game *g, float dt) {
    int i;
    for (i = 0; i < FLY_CONVOY_MAX; ++i) {
        fly_convoy *c = &g->convoys[i];
        fly_way way;
        if (!c->active) continue;
        if (!convoy_way(g, c, &way)) continue;
        if (fly_convoy_step(c, &way, dt)) {
            fly_convoy_unload(c, &g->world, &way);
            ++g->convoys_arrived;
            memset(c, 0, sizeof *c);
            if (g->convoy_count > 0) --g->convoy_count;
        }
    }
    convoys_combat(g, dt);
    g->convoy_restock -= dt;
    if (g->convoy_restock > 0.0f) return;
    g->convoy_restock = 40.0f;
    if (g->convoy_count < convoy_quota(g)) convoy_dispatch(g);
}

/* Fill the roads and the lanes, the way fly_game_populate fills the sky. */
void fly_game_convoys_populate(fly_game *g) {
    int n = convoy_quota(g), i;
    for (i = 0; i < n; ++i) convoy_dispatch(g);
}

static void pilots_step(fly_game *g, float dt) {
    fly_pilot_view view;
    int i;
    view.world = &g->world;
    view.time_s = g->time_s;
    view.player_pos = g->mode == FLY_MODE_FLIGHT ? g->player.craft.pos : g->walker.pos;
    view.player_airborne = g->mode == FLY_MODE_FLIGHT && g->docked < 0 && !g->player.craft.on_ground;
    view.player_threat = g->player.airframe.gun_dps;
    view.player_faction = g->player.faction;
    view.peers = g->pilots;
    view.peer_count = FLY_MAX_PILOTS;
    view.convoys = g->convoys;
    view.convoy_count = FLY_CONVOY_MAX;
    view.ord = g->ord;
    view.ord_count = FLY_ORD_MAX;

    /* Everybody decides, and then everybody moves.
     *
     * These used to be one loop, so a command applied at slot 6 was visible to
     * slots 7 and up when they stepped and invisible to slots 0 to 5 — the
     * world depended on array order. That is wrong on its own, and it made the
     * multiplayer seam a lie: an injected command stream necessarily arrives
     * before the tick rather than interleaved with it, so replaying a recording
     * produced a *different* world from the one that recorded it. It went
     * unnoticed because both fidelities drove altitude hard toward a freshly
     * computed target, which washed a one-step ordering difference out within a
     * few frames; rate-limiting the coarse climb made it persist, and the
     * replay gate caught it. Deciding first is also the shape the network
     * wants. */
    if (!g->pilots_remote) {
        for (i = 0; i < FLY_MAX_PILOTS; ++i) {
            fly_pilot *p = &g->pilots[i];
            fly_pilot_cmd cmd;
            if (!p->active || p->think_in > 0.0f) continue;
            /* think is handed a const view and cannot reach the world, so a bug
               in a decision can only ever produce a bad *command*, which apply
               then refuses. */
            if (fly_pilot_think(p, &view, &g->think_rng, dt, &cmd)) journal_apply(g, p, &cmd);
            p->think_in = cmd.kind == FLY_PCMD_CONTROLS ? 0.10f : 0.55f;
        }
    }
    for (i = 0; i < FLY_MAX_PILOTS; ++i) {
        fly_pilot *p = &g->pilots[i];
        int detail;
        if (!p->active) continue;
        if (p->haul_done) { g->pilot_hauls += p->haul_done; p->haul_done = 0; }
        detail = g->pilots_all_detail ||
                 fly_v3dist(p->craft.pos, view.player_pos) < FLY_PILOT_DETAIL_R;
        fly_pilot_step(p, &view, &g->weather, &g->world, &g->rng, detail, dt);
        if (p->craft.crashed) {
            /* Everyone's hull ends the same way. A world where only your own
               crashes make fire is one where the other twenty aircraft are
               scenery — and this is the cheapest possible way to show that the
               attrition the market feels is happening to somebody. */
            fly_game_spawn_blast(g, p->craft.pos,
                                 p->airframe.kind == FLY_CRAFT_DRONE ? 4.0f : 7.0f);
            /* What it was carrying goes where it fell. Only for a wreck the
               player could actually walk out to, and only for one they were
               near enough to see go down: sixty-four ground slots across a map
               this size is a budget, and the fights you were in are what it is
               for. The draws happen either way so a loss costs the same slice
               of the stream wherever it lands. */
            {
                /* Rank is nearly the whole of what a wreck is worth. A common
                   raider drops junk; the rare ace three ranks up is where a
                   One-off actually comes from, and there is no other route to
                   one. That is the design: the best gear is taken. */
                int rolls = 1 + (int)(p->tier > 2 ? 2 : p->tier);
                float bias = -1.2f + 1.15f * (float)p->tier;
                fly_v3 at = p->craft.pos;
                float gz = fly_world_ground(&g->world, at.x, at.y);
                int worth = fly__salvage_site(&g->world, at) >= 0 &&
                            fly_v3dist(at, view.player_pos) < FLY_PILOT_DETAIL_R;
                /* A saucer is the top of the ladder and its wreck says so:
                   the most rolls in the game at a bias nothing else reaches,
                   so a One-off is a likely outcome rather than a rare one.
                   Nothing else in the world drops at this weight, and nothing
                   else costs a rocket and a re-entry to reach.
                   It is also the one wreck that is always worth collecting.
                   The usual rule wants a wreck near a service site, which for
                   something shot down a hundred and fifty kilometres over open
                   country would mean the whole expedition paid nothing — so
                   what comes off a saucer falls, and it is on the ground
                   underneath by the time you are. */
                if (p->kind == FLY_PILOT_UFO) {
                    rolls = 4;
                    bias = 5.0f;
                    worth = 1;
                    at.z = gz + 1.0f;
                }
                if (at.z < gz + 1.0f) at.z = gz + 1.0f;
                if (!worth) rolls = 0;
                fly_game_scatter_drop(g, at, p->craft.vel, rolls, bias, p->id);
                if (rolls && fly_v3dist(at, view.player_pos) < fly_game_radar_range(g))
                    fly_game_log(g, "%s went down — wreckage on the deck", p->name);
                else if (fly_v3dist(p->craft.pos, view.player_pos) < fly_game_radar_range(g))
                    fly_game_log(g, "%s went down", p->name);
            }
            if (p->detail) ++g->pilots_lost_detail;
            memset(p, 0, sizeof *p);
            ++g->pilots_lost;
            if (g->pilot_count > 0) --g->pilot_count;
        }
    }
    pilots_combat(g, dt);
    pilots_restock(g, dt);
}

/* ---------------- step ---------------- */

static void check_docking(fly_game *g) {
    if (g->docked >= 0) return;
    if (g->launch_guard) {
        int near0 = fly_world_nearest(&g->world, fly_wpos_of(g->player.craft.pos), 0);
        int clear = near0 < 0;
        if (!clear) {
            const fly_location *L0 = &g->world.loc[near0];
            /* Only genuinely clear of the pad re-arms docking: either well away
             * horizontally, or high enough above the deck that coming back down
             * would be a real landing. Merely losing ground contact is not
             * enough — rolling along a raised pad deck flickers on_ground, and
             * treating that as "launched" let the craft re-dock at its origin
             * a second later, stranding an autopilot run on the pad forever. */
            float speed = fly_v3len(fly_v3mk(g->player.craft.vel.x, g->player.craft.vel.y, 0));
            clear = dist2d(g->player.craft.pos, L0->pos) > FLY_DOCK_RANGE ||
                    g->player.craft.pos.z > L0->pad_z + 40.0f ||
                    /* launch abandoned: stopped on the deck with the throttle
                     * closed. Without this the guard would hold forever — e.g.
                     * autopilot engaged toward the pad already occupied, which
                     * brakes to a stop and could then never re-dock. */
                    (g->player.craft.on_ground && speed < 1.0f && g->player.controls.throttle < 0.05f);
        }
        if (!clear) return;
        g->launch_guard = 0;
    }
    if (!g->player.craft.on_ground) return;
    float speed = fly_v3len(fly_v3mk(g->player.craft.vel.x, g->player.craft.vel.y, 0));
    if (speed > 3.0f) return;
    int near = fly_world_nearest(&g->world, fly_wpos_of(g->player.craft.pos), 0);
    if (near < 0) return;
    fly_location *L = &g->world.loc[near];
    if (dist2d(g->player.craft.pos, L->pos) > FLY_DOCK_RANGE) return;
    uint32_t blocked = fly_world_gate_check(&g->world, near, &g->player.airframe);
    if (blocked) {
        fly_game_log(g, "%s refuses: %s", L->name, fly_gate_name(blocked));
        return;
    }
    g->docked = near;
    fly__sync_active(g);
    g->autopilot = 0;
    L->discovered = 1;
    g->player.craft.vel = fly_v3zero();
    g->xp += 2;
    fly_game_log(g, "Docked at %s (%s)", L->name, fly_loc_kind_name(L->kind));
    complete_deliveries(g);
    offer_contracts(g);
    /* A save carries the player's world, not its traffic: pilots are transient
       and their positions would be stale the moment the clock moved. The sky is
       repopulated from the same rules that filled it the first time, so a
       reloaded world is busy immediately rather than filling up over the next
       quarter of an hour. The roads fill on the same terms and for the same
       reason. */
    fly_game_populate(g);
    fly_game_convoys_populate(g);
}

/* Hulls that have run out of life are written off once they are back on the
 * ground and at the site they are parked at. Doing it on the pad rather than in
 * the air is the whole point: fatigue is a maintenance decision the player is
 * warned about and can act on, not a random failure — a hull that fell out of
 * the sky at 100% life would be attrition nobody owns. The modules come off
 * into inventory, because this is a hangar and not a crash site. */
static void fly__condemn_expired(fly_game *g) {
    int i, s, any = 0;
    for (i = 0; i < g->owned_count; ++i) {
        fly_owned_airframe *o = &g->owned[i];
        if (o->status != FLY_AIRFRAME_USABLE || o->fatigue < 1.0f) continue;
        if (o->location != g->docked) continue;
        /* the hull is scrap; what was bolted to it is not, and it is already
           in the hold — unbolting it is the whole of the transaction */
        for (s = 0; s < FLY_SLOT_COUNT; ++s) o->fitted[s] = 0;
        o->status = FLY_AIRFRAME_DESTROYED;
        o->condition = o->fuel = 0.0f;
        ++g->hulls_lost;
        fly_game_log(g, "#%u condemned: airframe life expired", o->instance_id);
        if (i == g->active_airframe) any = 1;
    }
    if (any) {
        fly_game_rebuild_airframe(g);
        if (g->player.craft.hp > g->player.airframe.structure) g->player.craft.hp = g->player.airframe.structure;
    }
    fly__check_ruin(g);
}

/* Walk the operator out, once there has been time to watch. */
static void fly__crash_relocate(fly_game *g) {
    int site = g->crash_site;
    int recoverable = g->crash_recoverable;
    fly_v3 crash = g->player.craft.pos;
    if (site < 0) {
        /* Nowhere to walk out from. The operator still gets home — it is the
         * estate that is at risk here, not a person — but the wreck and
         * everything bolted to it is written off where it fell. */
        site = g->launch_site >= 0 && g->launch_site < g->world.nloc ? g->launch_site : 0;
        {
            fly_loc_kind k = g->world.loc[site].kind;
            if (k != FLY_LOC_CITY && k != FLY_LOC_SKYPORT) {
                float best = 1e30f;
                int j;
                for (j = 0; j < g->world.nloc; ++j) {
                    fly_loc_kind jk = g->world.loc[j].kind;
                    if (jk != FLY_LOC_CITY && jk != FLY_LOC_SKYPORT) continue;
                    {
                        float d = fly_world_dist(g->world.loc[j].pos,
                                                 g->world.loc[site].pos);
                        if (d < best) { best = d; site = j; }
                    }
                }
            }
        }
    }
    {
        fly_wpos lp = fly_wpos_step(g->world.loc[site].pos, fly_v2mk(5.0f, 0.0f));
        fly_walk_init(&g->walker,
                      fly_wpos_at(lp, fly_world_ground(&g->world, lp.e, lp.n)), 0);
    }
    g->mode = FLY_MODE_WALK;
    g->docked = site;
    if (recoverable)
        fly_game_log(g, "Airframe destroyed; wreck lies %.1f km from %s",
                     dist2d(crash, g->world.loc[site].pos) / 1000.0f, g->world.loc[site].name);
    else
        fly_game_log(g, "Airframe destroyed; wreck unrecoverable, cargo and modules written off");
    fly__check_ruin(g);
}


void fly_game_restart(fly_game *g) {
    int i, m;
    /* The estate goes; the world does not. Markets, prices, the pilot
     * population, the clock and the map stay exactly where the last operator
     * left them — including that operator's modules, still lying in the field
     * where they fell, for this one to walk out and find. A world that reset
     * would also be a world no second player could be standing in. */
    memset(g->owned, 0, sizeof g->owned);
    memset(g->items, 0, sizeof g->items);
    g->item_count = 0;
    memset(g->player.cargo, 0, sizeof g->player.cargo);
    memset(g->contracts, 0, sizeof g->contracts);
    memset(&g->player.controls, 0, sizeof g->player.controls);
    /* And anything the last operator had in the air. A round outlives the
     * aeroplane that fired it by design — that is most of what makes a launch
     * a commitment — but it must not outlive the *operator*, or a new one is
     * seated into somebody else's engagement with a missile arriving from a
     * fight they were not in. */
    memset(g->ord, 0, sizeof g->ord);
    g->owned_count = 0;
    g->active_airframe = 0;
    g->autopilot = 0;
    g->autopilot_target = -1;
    g->launch_guard = 0;
    g->crash_handled = 0;
    g->ruin = FLY_RUIN_NONE;
    ++g->operator_seq;
    /* Knowledge and standing carry, capital does not: the second run starts
     * with the map it earned and none of the money, so it is a shorter run to
     * the same place rather than a softer one. */
    g->reputation /= 2;
    g->player.tokens = 120;
    /* An oath is a person's, not an estate's. The new operator signs on
     * unaligned into the same war, keeping half of what the powers thought of
     * the last one — the name on the licence changed, the reputation of the
     * outfit did not entirely. */
    {
        int f;
        if (g->player.faction != FLY_FACTION_FREE)
            fly_game_log(g, "Your %s papers die with the outfit.",
                         fly_faction_name(g->player.faction));
        g->player.faction = FLY_FACTION_FREE;
        g->pledged_at = g->time_s;
        for (f = 0; f < FLY_FACTION_COUNT; ++f) g->standing[f] /= 2;
    }

    int site = g->docked >= 0 && g->docked < g->world.nloc &&
               (g->world.loc[g->docked].kind == FLY_LOC_CITY ||
                g->world.loc[g->docked].kind == FLY_LOC_SKYPORT) ? g->docked : -1;
    for (i = 0; site < 0 && i < g->world.nloc; ++i)
        if (g->world.loc[i].kind == FLY_LOC_CITY) site = i;
    if (site < 0) site = 0;
    g->docked = site;

    int ci = fly_game_airframe_catalog_find("skylark");
    if (ci < 0) ci = 0;
    int idx = fly__create_airframe(g, ci);
    if (idx < 0) return;
    fly__outfit_starter(g, idx, ci);
    for (m = 0; m < fly_module_count(); ++m) {
        int k, n = fly__airframes[ci].starter_spares[m];
        for (k = 0; k < n; ++k) {
            fly_item it = fly_game_item_stock(m);
            fly_game_item_add(g, &it);
        }
    }
    g->active_airframe = idx;
    fly_game_rebuild_airframe(g);

    fly_location *L = &g->world.loc[site];
    fly_v3 pos = fly_wpos_at(L->pos, L->pad_z + g->player.airframe.gear_height);
    fly_craft_init(&g->player.craft, &g->player.airframe, pos);
    g->owned[idx].fuel = g->player.craft.fuel;
    g->owned[idx].location = site;
    g->mode = FLY_MODE_FLIGHT;
    g->launch_site = site;
    g->launch_pos = pos;
    fly_walk_init(&g->walker, fly_v3mk(pos.x + 4.0f, pos.y, L->pad_z), 0.0f);
    offer_contracts(g);
    fly_game_log(g, "New operator signed on at %s. Aircraft #%u, %ld tk.",
                 L->name, g->owned[idx].instance_id, g->player.tokens);
}

static void fly__crash_recover(fly_game *g) {
    fly_owned_airframe *o = fly__active(g);
    if (!o || g->crash_handled) return;
    g->crash_handled = 1;
    g->autopilot = 0;
    memset(&g->player.controls, 0, sizeof g->player.controls);
    int i;
    /* Outlaws lose interest in a wreck, but they do not evaporate: whoever was
       chasing you is still up there, and will be when you get airborne again. */
    for (i = 0; i < FLY_MAX_PILOTS; ++i)
        if (g->pilots[i].active && g->pilots[i].mark_player) {
            g->pilots[i].mark_player = 0;
            g->pilots[i].task = FLY_TASK_TRANSIT;
            g->pilots[i].dest = g->pilots[i].home;
        }
    fly_v3 crash = g->player.craft.pos;
    /* Salvage is earned, not owed. Somewhere a walk can reach, the fitted
     * modules scatter and can be collected back; in the sea or out past the
     * last pad they go down with the hull. The draws happen either way so that
     * the crash costs the same slice of the stream wherever it lands. */
    int site = fly__salvage_site(&g->world, crash);
    /* The hull goes up where it stopped. */
    fly_game_spawn_blast(g, crash, g->player.airframe.kind == FLY_CRAFT_DRONE ? 4.0f : 7.0f);
    for (i = 0; i < FLY_SLOT_COUNT; ++i) {
        uint32_t uid = o->fitted[i];
        const fly_item *held = fly_game_item(g, uid);
        fly_item thrown;
        float angle = fly_rng_span(&g->rng, 0, 2.0f * FLY_PI);
        float speed = fly_rng_span(&g->rng, 9.0f, 23.0f);
        float rise = fly_rng_span(&g->rng, 7.0f, 16.0f);
        if (!held) { o->fitted[i] = 0; continue; }
        thrown = *held;
        o->fitted[i] = 0;
        fly_game_item_remove(g, uid);   /* it is out there now, not in the hold */
        if (site < 0) continue;
        {
            /* Thrown, not placed. The old code dropped each module straight
               onto a random point on a circle 12 to 90 m out, which is the same
               spread this produces ballistically and reads completely
               differently: pieces leave the wreck, arc, bounce and stop. A
               quarter of the airframe's own momentum goes with them so the
               debris trails the direction of travel rather than forming a ring
               around a stationary point. */
            float gz = fly_world_ground(&g->world, crash.x, crash.y);
            fly_v3 from = fly_v3mk(crash.x, crash.y,
                                   crash.z > gz + 1.0f ? crash.z : gz + 1.0f);
            fly_v3 vel = fly_v3mk(cosf(angle) * speed + g->player.craft.vel.x * 0.25f,
                                  sinf(angle) * speed + g->player.craft.vel.y * 0.25f, rise);
            fly_game_launch_ground_item(g, &thrown, from, vel,
                                        FLY_GROUND_SOURCE_CRASH, o->instance_id);
        }
    }
    o->status = FLY_AIRFRAME_DESTROYED;
    o->condition = o->fuel = 0.0f;
    ++g->hulls_lost;
    g->crash_site = site;
    g->crash_recoverable = site >= 0;
    /* Stay with it for a beat.
     *
     * The wreck used to be destroyed and the operator walked to the nearest
     * city inside the same step, so the one thing a player never saw was their
     * own aeroplane coming apart: the camera cut on the frame it happened. The
     * hull, the cargo and the modules are already gone by this line — only the
     * relocation waits, so nothing about the cost of a crash changes. */
    g->crash_hold = 2.6f;
    fly__check_ruin(g);
}

void fly_game_step(fly_game *g, float dt) {
    g->time_s += dt;
    g->hit_flash = g->hit_flash > dt ? g->hit_flash - dt : 0.0f;
    g->gun_flash = g->gun_flash > dt ? g->gun_flash - dt : 0.0f;

    /* economy ticks in coarse steps */
    {
        static const float tick = 0.25f; /* hours per tick at 900 s */
        double prev = g->time_s - dt;
        if ((long)(prev / 900.0) != (long)(g->time_s / 900.0)) {
            fly_world_economy_step(&g->world, tick);
            fly__faction_tick(g, tick);
            int l, m, c;
            for (l = 0; l < g->world.nloc; ++l) {
                int service = g->world.loc[l].kind == FLY_LOC_CITY || g->world.loc[l].kind == FLY_LOC_SKYPORT;
                if (!service) continue;
                for (m = 0; m < FLY_SITE_STOCK_MAX; ++m) {
                    uint32_t h;
                    fly_rng shelf;
                    if (g->stock[l][m].uid) continue;
                    if (((long)(g->time_s / 900.0) + l + m) % 16 != 0) continue;
                    /* Restock is seeded off the quarter hour and the shelf, not
                       drawn from the world stream: a shop refilling itself must
                       not consume it, or a client that never looks at a shop
                       desynchronises from one that does. */
                    int lvl = fly_world_item_level(&g->world, g->world.loc[l].pos.e,
                                                   g->world.loc[l].pos.n);
                    h = fly_hash2(g->seed + 0x5701u, (int)(g->time_s / 900.0) + l * 31, m);
                    fly_rng_seed(&shelf, (uint64_t)h * 0x9e3779b97f4a7c15ULL + 0x9au);
                    /* The base comes off the shelf's own generator rather than
                       off `h` directly, because "which bases may appear here"
                       is a filtered draw and a modulo of a hash cannot be one.
                       Still not the world stream: `shelf` is the quarter hour
                       and the shelf index, which is the property that matters. */
                    g->stock[l][m] = fly_game_vendor_roll(&g->world, l,
                        fly__shelf_base(&g->world, l, m, lvl, &shelf), &shelf);
                    g->stock[l][m].uid = 1u + (h >> 8);
                }
                if (g->world.loc[l].kind == FLY_LOC_CITY)
                    for (c = 0; c < fly_game_airframe_catalog_count(); ++c) {
                        const fly_airframe_catalog *cat = fly_game_airframe_catalog_get(c);
                        /* A power's hull restocks only while that power still
                         * holds the field. Ground that changes hands changes
                         * what is parked on it, which is the most concrete
                         * thing a takeover does to a player. */
                        int cap = !cat || cat->faction == FLY_FACTION_FREE ? 2
                                : (int)g->world.loc[l].owner == cat->faction ? 1 : 0;
                        if (g->airframe_stock[l][c] < cap &&
                            ((long)(g->time_s / 900.0) + l * 3 + c) % 64 == 0)
                            ++g->airframe_stock[l][c];
                    }
            }
        }
    }
    expire_contracts(g);

    fly_v3 weather_pos = g->mode == FLY_MODE_WALK ? g->walker.pos :
                         g->mode == FLY_MODE_RAIL ? g->rail.pos : g->player.craft.pos;
    fly_world_weather(&g->world, weather_pos, g->time_s, &g->weather);

    /* Debris and fire advance before the player's own mode is considered, and
       that placement is the point: the moment you most want to watch a wreck
       burn and its pieces come down is on foot beside it, and every mode below
       this line returns early. */
    step_ground_items(g, dt);
    step_blasts(g, dt);
    /* Ordnance before the pilots think, and for the same reason the debris is
       here: a round in the air is part of the world whatever the player is
       doing, and every mode below this line returns early. It also has to
       advance *before* the decisions that react to it, or a pilot would be
       breaking away from where a missile was a frame ago. */
    ord_step(g, dt);
    /* Before the player's own mode is considered: the sky is busy whether the
       player is flying, walking, riding the rail or parked on a pad. A world
       that only runs while you are in the air is a diorama. */
    pilots_step(g, dt);
    /* And the roads, for exactly the same reason: the freight moves whether or
       not anyone is watching it, and a market that only shifted while the
       player was airborne would be a market with the player at the centre. */
    convoys_step(g, dt);
    /* And the triggers, after everyone has moved. Deciding first and acting
       afterwards is the shape the pilot tick already has: a launch resolved
       mid-loop would be visible to the aircraft after it in the array and
       invisible to the ones before, and the world would depend on slot
       order. */
    ord_triggers(g, dt);
    if (g->mode == FLY_MODE_WALK) {
        fly_walk_step(&g->walker, &g->walk_input, fly_world_ground_cb, &g->world, dt);
        return;
    }
    if (g->mode == FLY_MODE_RAIL) {
        g->rail.distance = fly_rail_advance(&g->rail_route, g->rail.distance, g->rail.speed, dt);
        fly_rail_eval(&g->rail_route, g->rail.distance, &g->rail.pos, &g->rail.tangent);
        g->rail.can_exit = g->rail.distance >= g->rail_route.length - 0.01f ||
                           fly_world_ground_safe(&g->world, g->rail.pos.x, g->rail.pos.y, 40.0f * FLY_DEG2RAD);
        return;
    }
    /* parked: world keeps turning, craft rests — but a hull whose life ran out
     * on the way in is written off on the apron, not the next time it flies */
    if (g->docked >= 0) { fly__condemn_expired(g); return; }

    if (g->autopilot && g->autopilot_target >= 0) autopilot_controls(g, dt);

    fly_sim_step(&g->player.craft, &g->player.airframe, &g->player.controls, &g->weather, fly_world_ground_cb,
                 (void *)&g->world, fly_game_cargo_mass(g), &g->play_rng, dt);
    /* Fold the chart. The flight law integrates in the tangent plane, which is
     * the right frame for it and has no idea the world comes back on itself; a
     * long enough leg walks the position past the antipode and out the far side
     * of a disc it should have re-entered. The terrain never noticed, because
     * fly_world_sphere_dir is periodic on its own — but everything that
     * measures, from the map to the contract distances, would have gone on
     * counting outward for ever. One step is far smaller than the disc, so this
     * fires once per lap and is a no-op the rest of the time. */
    {
        fly_wpos w = fly_world_chart_wrap(fly_wpos_of(g->player.craft.pos));
        g->player.craft.pos.x = w.e;
        g->player.craft.pos.y = w.n;
    }
    if (g->player.craft.crashed) {
        fly__crash_recover(g);           /* idempotent: crash_handled guards it */
        if (g->crash_hold > 0.0f) {
            g->crash_hold -= dt;
            if (g->crash_hold <= 0.0f) fly__crash_relocate(g);
        }
        return;
    }

    /* Vacuum.
     *
     * The motor and the tanks get you up there; the cabin is what lets you
     * arrive alive. This is the one part of the space gate that is a rule
     * rather than an equation, and it is a rule because the alternative is
     * modelling a cockpit's leak rate to say something the game already knows:
     * an unsealed hull at a hundred kilometres is a coffin. It bites well
     * above anything an air breather can reach, so no ordinary flight can
     * touch it — you have to have got there on purpose, with four of the five
     * parts and not the fifth. */
    if (g->player.craft.pos.z > FLY_KARMAN * 0.75f && g->player.airframe.pressure < 0.5f) {
        float over = (g->player.craft.pos.z - FLY_KARMAN * 0.75f) / (FLY_KARMAN * 0.25f);
        g->player.craft.hp -= fly_clampf(over, 0.0f, 1.0f) * 18.0f * dt;
        if (g->player.craft.hp <= 0.0f) {
            g->player.craft.hp = 0.0f;
            g->player.craft.crashed = 1;
            fly_game_log(g, "Cabin failed. The hull is venting.");
        }
    }

    /* Airframe life. The only cost in the game a repair cannot undo, so it is
     * the one that makes standing still lose slowly: money has to keep turning
     * into hulls. Hard flying spends it faster — g beyond one, and weather the
     * fitted kit is not rated for. */
    {
        fly_owned_airframe *o = fly__active(g);
        if (o && !g->player.craft.on_ground && o->fatigue < 1.0f) {
            float g_excess = fabsf(g->player.craft.gload) > 1.0f ? fabsf(g->player.craft.gload) - 1.0f : 0.0f;
            float exposed = g->weather.turbulence * (1.0f - fly_clampf(g->player.airframe.weather_rating, 0, 1));
            float rate = (1.0f + g_excess * 0.8f + exposed * 1.5f) / (FLY_HULL_LIFE_HOURS * 3600.0f);
            float before = o->fatigue;
            o->fatigue = fly_clampf(o->fatigue + rate * dt, 0.0f, 1.0f);
            if (before < 0.85f && o->fatigue >= 0.85f)
                fly_game_log(g, "#%u airframe life 85%%: line up a replacement", o->instance_id);
        }
    }

    /* exploration */
    int found = fly_world_discover(&g->world, fly_wpos_of(g->player.craft.pos),
                                   fly_game_radar_range(g));
    if (found) {
        g->xp += 15 * found;
        fly_game_log(g, "Discovered %d new site%s", found, found > 1 ? "s" : "");
    }

    if (g->player.craft.crashed) { fly__crash_recover(g); return; }

    check_docking(g);
    if (g->docked >= 0) fly__condemn_expired(g);

    fly__sync_active(g);
}

/* ---------------- save / load ---------------- */

/* One item, as a map entry in a flat list. Rolls go out as numbers and the
 * name is not written at all — it is generated, so a save that carried it
 * could only ever disagree with the item. The base goes out by its string id,
 * because a registry index is a position in a table the player can edit. */
static void fly__save_item(FILE *f, const char *ind, const fly_item *it) {
    const fly_module *m = fly_module_get(it->base);
    int a;
    /* The first key follows the caller's "- " and so carries no indent of its
     * own; everything after it is indented under the entry. */
    fprintf(f, "uid: %u\n%s  base: %s\n%s  rarity: %u\n%s  ilvl: %u\n%s  seed: %u\n",
            it->uid, ind, m ? m->id : "-", ind, (unsigned)it->rarity,
            ind, (unsigned)it->ilvl, ind, (unsigned)it->seed);
    fprintf(f, "%s  affix: [", ind);
    for (a = 0; a < it->naff && a < FLY_ITEM_AFFIX_MAX; ++a)
        fprintf(f, "%s%u, %d", a ? ", " : "", (unsigned)it->aff[a].kind, (int)it->aff[a].mag);
    fprintf(f, "]\n");
}

/* Returns 0, or negative if the base is not in the registry — a save naming a
 * module this build does not have is not a save this build can honour, and
 * silently dropping the item would quietly rewrite the player's aircraft. */
static int fly__load_item(const fly_yaml_node *n, fly_item *out) {
    const fly_yaml_node *aff;
    int a, base;
    memset(out, 0, sizeof *out);
    base = fly_module_find(fly_yaml_str(n, "base", ""));
    if (base < 0) return -1;
    out->uid = (uint32_t)fly_yaml_num(n, "uid", 0);
    out->base = (short)base;
    out->rarity = (unsigned char)fly_yaml_int(n, "rarity", 0);
    if (out->rarity >= FLY_ITEM_RARITY_COUNT) out->rarity = 0;
    out->ilvl = (unsigned char)fly_yaml_int(n, "ilvl", 1);
    out->seed = (unsigned char)fly_yaml_int(n, "seed", 0);
    aff = fly_yaml_get(n, "affix");
    for (a = 0; a * 2 + 1 < fly_yaml_count(aff) && a < FLY_ITEM_AFFIX_MAX; ++a) {
        const fly_yaml_node *k = fly_yaml_at(aff, a * 2), *v = fly_yaml_at(aff, a * 2 + 1);
        int kind = k && k->scalar ? (int)strtol(k->scalar, NULL, 10) : 0;
        if (kind <= 0 || kind >= FLY_AFFIX_COUNT) continue;
        out->aff[out->naff].kind = (unsigned char)kind;
        out->aff[out->naff].mag = (short)(v && v->scalar ? strtol(v->scalar, NULL, 10) : 0);
        ++out->naff;
    }
    return 0;
}

/* --- writing the save ----------------------------------------------------
 *
 * Through a temporary beside it, and then a rename. The save is the whole of
 * what a player has, it is rewritten every time they dock, and a write that
 * runs out of disk or is interrupted half way through used to leave the
 * truncated remains of the new save where the old one had been — the run is
 * then gone, and it is gone at exactly the moment the game promised to keep
 * it. A rename over an existing file is atomic where the platform has any
 * atomicity to offer, so either the previous save is there or the new one is,
 * and never a half of either.
 *
 * The remove-then-rename fallback is for the platforms whose rename refuses an
 * existing destination. It is not atomic and cannot be made so in C99; it is
 * still strictly better than writing into the file itself, because the content
 * being renamed into place is already known to be complete.
 */
int fly_game_save(const fly_game *g, const char *path) {
    char tmp[512];
    FILE *f;
    int n = snprintf(tmp, sizeof tmp, "%s.new", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    f = fopen(tmp, "w");
    if (!f) return -1;
    /* v4 adds the war: who holds what, who is speaking to whom, and whose
     * colours the operator wears. A v3 save still loads — every field below
     * has a default, and the default is the world world-gen would have made
     * anyway, so an old save resumes into the politics its seed implies rather
     * than into a blank one. */
    fprintf(f, "# fly99 save\nversion: 4\nseed: %u\ntime: %.17g\n", g->seed, g->time_s);
    fprintf(f, "rng: [%llu, %llu]\n", (unsigned long long)g->rng.s[0], (unsigned long long)g->rng.s[1]);
    fprintf(f, "player:\n  kind: %d\n  tokens: %ld\n  xp: %d\n  rep: %d\n  modules: [",
            (int)g->player.airframe.kind, g->player.tokens, g->xp, g->reputation);
    {
        int si;
        for (si = 0; si < FLY_SLOT_COUNT; ++si) {
            const uint32_t *fitted = fly_game_fitted(g);
            const fly_module *m = fitted ? fly_game_item_base(g, fitted[si]) : NULL;
            fprintf(f, "%s%s", si ? ", " : "", m ? m->id : "-");
        }
        fprintf(f, "]\n");
    }
    fprintf(f, "next_item_uid: %u\nitems:\n", g->next_item_uid);
    {
        int i;
        for (i = 0; i < g->item_count; ++i) {
            fprintf(f, "  - ");
            fly__save_item(f, "  ", &g->items[i]);
        }
    }
    fprintf(f, "owned:\n");
    {
        int oi, si;
        for (oi = 0; oi < g->owned_count; ++oi) {
            const fly_owned_airframe *o = &g->owned[oi];
            fprintf(f, "  - instance: %u\n    catalog: %s\n    status: %d\n    location: %d\n"
                       "    condition: %.9g\n    fuel: %.9g\n    wear: %.9g\n    fatigue: %.9g\n"
                       "    ammo: %d\n    cm: %d\n"
                       "    fitted: [",
                    o->instance_id, o->catalog_id, (int)o->status, o->location,
                    o->condition, o->fuel, o->wear, o->fatigue, o->ammo, o->cm);
            for (si = 0; si < FLY_SLOT_COUNT; ++si)
                fprintf(f, "%s%u", si ? ", " : "", o->fitted[si]);
            fprintf(f, "]\n");
        }
    }
    fprintf(f, "active_airframe: %d\nnext_airframe_instance: %u\n", g->active_airframe, g->next_airframe_instance_id);
    /* The magazines are saved and the rounds already in the air are not. A
       round mid-flight was going to resolve within eight seconds and restoring
       one would restore a threat whose origin the player never saw; what is in
       the racks is the aeroplane's own state, exactly like the fuel above it. */
    fprintf(f, "craft:\n  pos: [%.3f, %.3f, %.3f]\n  vel: [%.3f, %.3f, %.3f]\n"
               "  ori: [%.6f, %.6f, %.6f, %.6f]\n  fuel: %.3f\n  hp: %.3f\n  wear: %.5f\n"
               "  ammo: %d\n  cm: %d\n  on_ground: %d\n",
            g->player.craft.pos.x, g->player.craft.pos.y, g->player.craft.pos.z,
            g->player.craft.vel.x, g->player.craft.vel.y, g->player.craft.vel.z,
            g->player.craft.ori.w, g->player.craft.ori.x, g->player.craft.ori.y, g->player.craft.ori.z,
            g->player.craft.fuel, g->player.craft.hp, g->player.craft.wear,
            g->player.craft.ammo, g->player.craft.cm, g->player.craft.on_ground);
    fprintf(f, "docked: %d\nautopilot: %d\nap_target: %d\nmode: %d\nlaunch_site: %d\n"
               "launch_pos: [%.9g, %.9g, %.9g]\ncrash_handled: %d\n"
               "ruin: %d\noperator: %d\nhulls_lost: %d\n",
            g->docked, g->autopilot, g->autopilot_target, (int)g->mode, g->launch_site,
            g->launch_pos.x, g->launch_pos.y, g->launch_pos.z, g->crash_handled,
            g->ruin, g->operator_seq, g->hulls_lost);
    fprintf(f, "walker:\n  pos: [%.9g, %.9g, %.9g]\n  vel: [%.9g, %.9g, %.9g]\n"
               "  yaw: %.9g\n  pitch: %.9g\n  on_ground: %d\n",
            g->walker.pos.x, g->walker.pos.y, g->walker.pos.z,
            g->walker.vel.x, g->walker.vel.y, g->walker.vel.z,
            g->walker.yaw, g->walker.pitch, g->walker.on_ground);
    fprintf(f, "rail:\n  distance: %.9g\n  speed: %.9g\n  entry: %.9g\n\n",
            g->rail.distance, g->rail.speed, g->rail.entry_distance);
    fprintf(f, "ground_items:\n");
    {
        int gi;
        for (gi = 0; gi < FLY_GROUND_ITEM_MAX; ++gi) if (g->ground_items[gi].active) {
            const fly_ground_item *it = &g->ground_items[gi];
            fprintf(f, "  - pos: [%.9g, %.9g, %.9g]\n    yaw: %.9g\n"
                       "    source: %d\n    source_id: %u\n    ",
                    it->pos.x, it->pos.y, it->pos.z, it->yaw,
                    (int)it->source, it->source_id);
            fly__save_item(f, "  ", &it->item);
        }
    }
    fprintf(f, "stock:\n");
    {
        int l, m;
        for (l = 0; l < g->world.nloc; ++l)
            for (m = 0; m < FLY_SITE_STOCK_MAX; ++m) {
                if (!g->stock[l][m].uid) continue;
                fprintf(f, "  - site: %d\n    shelf: %d\n    ", l, m);
                fly__save_item(f, "  ", &g->stock[l][m]);
            }
    }
    fprintf(f, "airframe_stock:\n");
    {
        int l, c;
        for (l = 0; l < g->world.nloc; ++l) {
            fprintf(f, "  - values: [");
            for (c = 0; c < fly_game_airframe_catalog_count(); ++c)
                fprintf(f, "%s%u", c ? ", " : "", (unsigned)g->airframe_stock[l][c]);
            fprintf(f, "]\n");
        }
    }
    fprintf(f, "cargo: [");
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r)
        fprintf(f, "%s%.3f", r ? ", " : "", g->player.cargo[r]);
    fprintf(f, "]\n");
    fprintf(f, "contracts:\n");
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i) {
        const fly_contract *ct = &g->contracts[i];
        fprintf(f, "  - state: %d\n    from: %d\n    to: %d\n    res: %d\n"
                   "    qty: %.2f\n    reward: %ld\n    deadline: %.2f\n    loaded: %d\n",
                ct->state, ct->from, ct->to, ct->res, ct->qty, ct->reward,
                ct->deadline, ct->cargo_loaded);
    }
    fprintf(f, "allegiance:\n  faction: %d\n  pledged_at: %.17g\n  seizures: %d\n"
               "  standing: [", g->player.faction, g->pledged_at, g->seizures_seen);
    for (r = 0; r < FLY_FACTION_COUNT; ++r)
        fprintf(f, "%s%d", r ? ", " : "", g->standing[r]);
    fprintf(f, "]\n  relations:\n");
    {
        int a, b;
        /* The upper triangle only. The matrix is symmetric by construction and
         * writing both halves would let a hand-edited save disagree with
         * itself — a state the code has no way to represent and no rule for
         * resolving. */
        for (a = 1; a < FLY_FACTION_COUNT; ++a)
            for (b = a + 1; b < FLY_FACTION_COUNT; ++b)
                fprintf(f, "    - [%d, %d, %.6f]\n", a, b, g->world.dip.rel[a][b]);
    }
    fprintf(f, "world:\n  locations:\n");
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_location *L = &g->world.loc[i];
        fprintf(f, "    - discovered: %d\n      owner: %d\n      grip: %.5f\n"
                   "      push: [", L->discovered, (int)L->owner, L->grip);
        for (r = 0; r < FLY_FACTION_COUNT; ++r)
            fprintf(f, "%s%.5f", r ? ", " : "", L->push[r]);
        fprintf(f, "]\n      stock: [");
        for (r = 0; r < FLY_RES_COUNT; ++r)
            fprintf(f, "%s%.3f", r ? ", " : "", L->stock[r]);
        fprintf(f, "]\n");
    }
    if (ferror(f)) { fclose(f); remove(tmp); return -2; }
    if (fclose(f) != 0) { remove(tmp); return -2; }
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) { remove(tmp); return -2; }
    }
    return 0;
}

static void seq_floats(const fly_yaml_node *seq, float *out, int n) {
    int i;
    for (i = 0; i < n; ++i) {
        const fly_yaml_node *v = fly_yaml_at(seq, i);
        if (v && v->scalar) out[i] = (float)strtod(v->scalar, NULL);
    }
}

/* A location index that came off the disk, or `dflt` if it names no place on
 * this chart. Every index in a save is a subscript into a fixed-capacity array
 * somewhere downstream, and a truncated, hand-edited or foreign save has no
 * obligation to hold one that fits. */
static int fly__loc_index(const fly_game *g, int idx, int dflt) {
    return (idx >= 0 && idx < g->world.nloc) ? idx : dflt;
}

int fly_game_load(fly_game *g, const char *path) {
    fly_yaml_node *root = fly_yaml_load_file(path);
    if (!root) return -1;
    uint32_t seed = (uint32_t)fly_yaml_num(root, "seed", 1);
    int version = fly_yaml_int(root, "version", 1);
    int kind = fly_yaml_int(root, "player.kind", 0);
    fly_game_init(g, seed, kind ? FLY_CRAFT_DRONE : FLY_CRAFT_PLANE);

    g->time_s = fly_yaml_num(root, "time", g->time_s);
    g->player.tokens = (long)fly_yaml_num(root, "player.tokens", g->player.tokens);
    g->xp = fly_yaml_int(root, "player.xp", 0);
    g->reputation = fly_yaml_int(root, "player.rep", 0);
    if (version < 2) {
        const fly_yaml_node *mods = fly_yaml_get(root, "player.modules");
        int mi, nm = fly_yaml_count(mods);
        for (mi = 0; mi < nm && mi < FLY_SLOT_COUNT; ++mi) {
            const fly_yaml_node *mv = fly_yaml_at(mods, mi);
            if (mv && mv->scalar && strcmp(mv->scalar, "-") != 0) {
                int idx = fly_module_find(mv->scalar);
                const fly_module *m = fly_module_get(idx);
                if (m) g->owned[0].fitted[m->slot] = idx;
            }
        }
    }
    if (version >= 2) {
        const fly_yaml_node *owned = fly_yaml_get(root, "owned");
        int oi, count = fly_yaml_count(owned);
        if (count <= 0 || count > FLY_OWNED_AIRFRAME_MAX) { fly_yaml_free(root); return -2; }
        g->owned_count = count;
        for (oi = 0; oi < count; ++oi) {
            const fly_yaml_node *on = fly_yaml_at(owned, oi);
            fly_owned_airframe *o = &g->owned[oi];
            memset(o, 0, sizeof *o);
            o->instance_id = (uint32_t)fly_yaml_num(on, "instance", 0);
            const char *cid = fly_yaml_str(on, "catalog", "");
            if (fly_game_airframe_catalog_find(cid) < 0) { fly_yaml_free(root); return -3; }
            snprintf(o->catalog_id, sizeof o->catalog_id, "%s", cid);
            o->status = (fly_owned_airframe_status)fly_yaml_int(on, "status", 0);
            o->location = fly_yaml_int(on, "location", 0);
            o->condition = (float)fly_yaml_num(on, "condition", 1);
            o->fuel = (float)fly_yaml_num(on, "fuel", 0);
            /* Missing keys default to full, not to empty. A save written by a
               build with no ordnance in it describes an aeroplane whose racks
               were never fired, and loading it into a world that has them
               should hand back an armed aircraft rather than a disarmed one —
               fly__clamp_magazines then cuts it back to whatever is actually
               fitted, which for every one of those saves is nothing. */
            o->ammo = fly_yaml_int(on, "ammo", 9999);
            o->cm = fly_yaml_int(on, "cm", 9999);
            o->wear = (float)fly_yaml_num(on, "wear", 0);
            o->fatigue = (float)fly_yaml_num(on, "fatigue", 0);
            int s;
            for (s = 0; s < FLY_SLOT_COUNT; ++s) o->fitted[s] = 0;
            const fly_yaml_node *fits = fly_yaml_get(on, "fitted");
            for (s = 0; s < fly_yaml_count(fits) && s < FLY_SLOT_COUNT; ++s) {
                const fly_yaml_node *v = fly_yaml_at(fits, s);
                o->fitted[s] = v && v->scalar ? (uint32_t)strtoul(v->scalar, NULL, 10) : 0;
            }
        }
        g->active_airframe = fly_yaml_int(root, "active_airframe", 0);
        g->next_airframe_instance_id = (uint32_t)fly_yaml_num(root, "next_airframe_instance", count + 1);
        if (g->active_airframe < 0 || g->active_airframe >= count) { fly_yaml_free(root); return -5; }
        /* The hold is loaded before anything resolves a uid, because an
         * airframe's slots are references into it. */
        const fly_yaml_node *its = fly_yaml_get(root, "items");
        int m, ni = fly_yaml_count(its);
        g->item_count = 0;
        if (ni > FLY_ITEM_MAX) { fly_yaml_free(root); return -10; }
        for (m = 0; m < ni; ++m) {
            if (fly__load_item(fly_yaml_at(its, m), &g->items[g->item_count]) != 0) {
                fly_yaml_free(root); return -4;
            }
            if (g->items[g->item_count].uid) ++g->item_count;
        }
        g->next_item_uid = (uint32_t)fly_yaml_num(root, "next_item_uid", 1);
        if (!g->next_item_uid) g->next_item_uid = 1;
    }
    fly_game_rebuild_airframe(g);

    float v4[4] = { 1, 0, 0, 0 };
    float v3[3] = { 0, 0, 0 };
    seq_floats(fly_yaml_get(root, "craft.pos"), v3, 3);
    g->player.craft.pos = fly_v3mk(v3[0], v3[1], v3[2]);
    seq_floats(fly_yaml_get(root, "craft.vel"), v3, 3);
    g->player.craft.vel = fly_v3mk(v3[0], v3[1], v3[2]);
    seq_floats(fly_yaml_get(root, "craft.ori"), v4, 4);
    g->player.craft.ori = fly_qnorm(fly_qmk(v4[0], v4[1], v4[2], v4[3]));
    g->player.craft.fuel = (float)fly_yaml_num(root, "craft.fuel", g->player.airframe.fuel_cap);
    g->player.craft.hp = (float)fly_yaml_num(root, "craft.hp", g->player.airframe.structure);
    g->player.craft.wear = (float)fly_yaml_num(root, "craft.wear", 0);
    g->player.craft.ammo = fly_yaml_int(root, "craft.ammo", g->player.airframe.ord_ammo);
    g->player.craft.cm = fly_yaml_int(root, "craft.cm", g->player.airframe.cm_ammo);
    fly__clamp_magazines(g);
    g->player.craft.on_ground = fly_yaml_int(root, "craft.on_ground", 1);
    /* --- indices off the disk ---------------------------------------------
     *
     * Every one of these is read straight into a subscript somewhere: `docked`
     * indexes the location table, the shelf and the airframe stock; the
     * autopilot's target and the launch site index the locations; `mode`
     * selects a camera and a control path. A file is not a promise — the same
     * rule the owner field above already keeps — and a save with `docked: 99`
     * in it read four arrays off the end of themselves before printing a
     * market with no name on it. Out of range falls back to the neutral value
     * rather than to zero: "nowhere" is a state the game already handles, and
     * silently docking the operator at the first site on the chart would hide
     * the corruption instead of surviving it. */
    g->docked = fly__loc_index(g, fly_yaml_int(root, "docked", 0), -1);
    g->autopilot = fly_yaml_int(root, "autopilot", 0);
    /* on-disk key stays "ap_target": renaming the C field must not
     * invalidate saves written by earlier builds */
    g->autopilot_target = fly__loc_index(g, fly_yaml_int(root, "ap_target", -1), -1);
    if (g->autopilot_target < 0) g->autopilot = 0;
    {
        int mode = fly_yaml_int(root, "mode", FLY_MODE_FLIGHT);
        if (mode < FLY_MODE_FLIGHT || mode > FLY_MODE_RAIL) mode = FLY_MODE_FLIGHT;
        g->mode = (fly_player_mode)mode;
    }
    g->launch_site = fly__loc_index(g, fly_yaml_int(root, "launch_site",
                                                   g->docked >= 0 ? g->docked : 0), 0);
    seq_floats(fly_yaml_get(root, "launch_pos"), v3, 3);
    g->launch_pos = fly_v3mk(v3[0], v3[1], v3[2]);
    g->crash_handled = fly_yaml_int(root, "crash_handled", 0);
    g->ruin = fly_yaml_int(root, "ruin", 0);
    g->operator_seq = fly_yaml_int(root, "operator", 0);
    g->hulls_lost = fly_yaml_int(root, "hulls_lost", 0);

    if (version >= 2) {
        seq_floats(fly_yaml_get(root, "walker.pos"), v3, 3);
        g->walker.pos = fly_v3mk(v3[0], v3[1], v3[2]);
        seq_floats(fly_yaml_get(root, "walker.vel"), v3, 3);
        g->walker.vel = fly_v3mk(v3[0], v3[1], v3[2]);
        g->walker.yaw = (float)fly_yaml_num(root, "walker.yaw", 0);
        g->walker.pitch = (float)fly_yaml_num(root, "walker.pitch", 0);
        g->walker.eye_height = 1.72f;
        g->walker.on_ground = fly_yaml_int(root, "walker.on_ground", 1);
        g->rail.distance = (float)fly_yaml_num(root, "rail.distance", 0);
        g->rail.speed = (float)fly_yaml_num(root, "rail.speed", g->rail_route.speed);
        g->rail.entry_distance = (float)fly_yaml_num(root, "rail.entry", 0);
        if (g->rail.distance < 0 || g->rail.distance > g->rail_route.length) {
            fly_yaml_free(root); return -6;
        }
        fly_rail_eval(&g->rail_route, g->rail.distance, &g->rail.pos, &g->rail.tangent);

        const fly_yaml_node *items = fly_yaml_get(root, "ground_items");
        int gi, ng = fly_yaml_count(items);
        if (ng > FLY_GROUND_ITEM_MAX) { fly_yaml_free(root); return -7; }
        for (gi = 0; gi < ng; ++gi) {
            const fly_yaml_node *itn = fly_yaml_at(items, gi);
            if (fly__load_item(itn, &g->ground_items[gi].item) != 0) {
                fly_yaml_free(root); return -8;
            }
            seq_floats(fly_yaml_get(itn, "pos"), v3, 3);
            if (fly_world_wet(&g->world, v3[0], v3[1])) {
                fly_yaml_free(root); return -9;
            }
            g->ground_items[gi].active = 1;
            g->ground_items[gi].settled = 1;
            g->ground_items[gi].pos = fly_v3mk(v3[0], v3[1], v3[2]);
            g->ground_items[gi].yaw = (float)fly_yaml_num(itn, "yaw", 0);
            g->ground_items[gi].source = (fly_ground_source)fly_yaml_int(itn, "source", 0);
            g->ground_items[gi].source_id = (uint32_t)fly_yaml_num(itn, "source_id", 0);
        }
        const fly_yaml_node *ms = fly_yaml_get(root, "stock");
        int l, m, ns = fly_yaml_count(ms);
        memset(g->stock, 0, sizeof g->stock);
        for (m = 0; m < ns; ++m) {
            const fly_yaml_node *sn = fly_yaml_at(ms, m);
            int site = fly_yaml_int(sn, "site", -1), shelf = fly_yaml_int(sn, "shelf", -1);
            if (site < 0 || site >= FLY_MAX_LOC || shelf < 0 || shelf >= FLY_SITE_STOCK_MAX) continue;
            if (fly__load_item(sn, &g->stock[site][shelf]) != 0) { fly_yaml_free(root); return -11; }
        }
        (void)l;
        const fly_yaml_node *as = fly_yaml_get(root, "airframe_stock");
        int c;
        for (l = 0; l < g->world.nloc && l < fly_yaml_count(as); ++l) {
            const fly_yaml_node *vals = fly_yaml_get(fly_yaml_at(as, l), "values");
            for (c = 0; c < fly_game_airframe_catalog_count() && c < fly_yaml_count(vals); ++c) {
                const fly_yaml_node *v = fly_yaml_at(vals, c);
                g->airframe_stock[l][c] = v && v->scalar ? (unsigned short)strtoul(v->scalar, NULL, 10) : 0;
            }
        }
        const fly_yaml_node *rng = fly_yaml_get(root, "rng");
        const fly_yaml_node *rv0 = fly_yaml_at(rng, 0), *rv1 = fly_yaml_at(rng, 1);
        if (rv0 && rv0->scalar && rv1 && rv1->scalar) {
            g->rng.s[0] = (uint64_t)strtoull(rv0->scalar, NULL, 10);
            g->rng.s[1] = (uint64_t)strtoull(rv1->scalar, NULL, 10);
        }
    }

    seq_floats(fly_yaml_get(root, "cargo"), g->player.cargo, FLY_RES_COUNT);

    const fly_yaml_node *cts = fly_yaml_get(root, "contracts");
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS && i < fly_yaml_count(cts); ++i) {
        const fly_yaml_node *c = fly_yaml_at(cts, i);
        fly_contract *ct = &g->contracts[i];
        /* A contract is three subscripts and a quantity: both endpoints index
         * the location table and the resource indexes the cargo hold, which is
         * written to on delivery. A slot that does not name two real places and
         * one real commodity is not a contract this world can honour, so it
         * loads as an empty slot rather than as a live job pointed off the end
         * of an array. */
        int from = fly__loc_index(g, fly_yaml_int(c, "from", -1), -1);
        int to = fly__loc_index(g, fly_yaml_int(c, "to", -1), -1);
        int res = fly_yaml_int(c, "res", -1);
        int state = fly_yaml_int(c, "state", 0);
        memset(ct, 0, sizeof *ct);
        if (state < 0 || state > 2 || from < 0 || to < 0 ||
            res < 0 || res >= FLY_RES_COUNT)
            continue;
        ct->state = state;
        ct->from = from;
        ct->to = to;
        ct->res = res;
        ct->qty = fly_clampf((float)fly_yaml_num(c, "qty", 0), 0.0f, 1e6f);
        ct->reward = (long)fly_yaml_num(c, "reward", 0);
        ct->deadline = fly_yaml_num(c, "deadline", 0);
        ct->cargo_loaded = fly_yaml_int(c, "loaded", 0) != 0;
    }

    const fly_yaml_node *locs = fly_yaml_get(root, "world.locations");
    for (i = 0; i < g->world.nloc && i < fly_yaml_count(locs); ++i) {
        const fly_yaml_node *L = fly_yaml_at(locs, i);
        int owner = fly_yaml_int(L, "owner", (int)g->world.loc[i].owner);
        g->world.loc[i].discovered = fly_yaml_int(L, "discovered", g->world.loc[i].discovered);
        /* A file is not a promise. An owner outside the roster would let a
         * hand-edited or truncated save index the livery table off the end of
         * itself, so it falls back to what world-gen decided rather than to
         * zero: unaligned is a real state and quietly assigning it would hide
         * the corruption instead of surviving it. */
        if (owner >= 0 && owner < FLY_FACTION_COUNT) g->world.loc[i].owner = (unsigned char)owner;
        g->world.loc[i].grip = fly_clampf((float)fly_yaml_num(L, "grip", g->world.loc[i].grip),
                                          0.0f, 1.0f);
        seq_floats(fly_yaml_get(L, "push"), g->world.loc[i].push, FLY_FACTION_COUNT);
        seq_floats(fly_yaml_get(L, "stock"), g->world.loc[i].stock, FLY_RES_COUNT);
    }

    {   /* allegiance: the operator's flag, their ledger and the war they
         * loaded into. All optional — a v3 save has none of it and resumes
         * into the politics its own seed generates. */
        int pf = fly_yaml_int(root, "allegiance.faction", FLY_FACTION_FREE);
        const fly_yaml_node *rels = fly_yaml_get(root, "allegiance.relations");
        int n = fly_yaml_count(rels), k;
        g->player.faction = (pf > FLY_FACTION_FREE && pf < FLY_FACTION_COUNT) ? pf
                                                                             : FLY_FACTION_FREE;
        g->pledged_at = fly_yaml_num(root, "allegiance.pledged_at", g->time_s);
        g->seizures_seen = fly_yaml_int(root, "allegiance.seizures", 0);
        {
            const fly_yaml_node *st = fly_yaml_get(root, "allegiance.standing");
            for (k = 0; k < FLY_FACTION_COUNT && k < fly_yaml_count(st); ++k) {
                const fly_yaml_node *v = fly_yaml_at(st, k);
                if (v && v->scalar)
                    g->standing[k] = (int)fly_clampf((float)strtod(v->scalar, NULL),
                                                     -(float)FLY_STAND_MAX,
                                                     (float)FLY_STAND_MAX);
            }
        }
        for (k = 0; k < n; ++k) {
            const fly_yaml_node *row = fly_yaml_at(rels, k);
            const fly_yaml_node *ea = fly_yaml_at(row, 0), *eb = fly_yaml_at(row, 1),
                                *ev = fly_yaml_at(row, 2);
            int a, b;
            if (!ea || !eb || !ev || !ea->scalar || !eb->scalar || !ev->scalar) continue;
            a = (int)strtol(ea->scalar, NULL, 10);
            b = (int)strtol(eb->scalar, NULL, 10);
            if (a <= FLY_FACTION_FREE || a >= FLY_FACTION_COUNT) continue;
            if (b <= FLY_FACTION_FREE || b >= FLY_FACTION_COUNT) continue;
            g->world.dip.rel[a][b] = g->world.dip.rel[b][a] =
                fly_clampf((float)strtod(ev->scalar, NULL), -1.0f, 1.0f);
        }
    }

    fly_yaml_free(root);
    fly_game_log(g, "Game loaded");
    return 0;
}
