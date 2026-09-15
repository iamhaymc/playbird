# PLAYBIRD

An artificial dream of freedom by flight.

![Path-traced world](docs/shots/pipeline_pathtraced_cpu.png)

## Summary

fly99 is a single-seat flying game about earning a living in the air: fly
contracts and cargo between settlements on a procedurally generated planet, fit
your aeroplane out of the modules you own, and keep a hull airworthy against
fuel, wear, weather, pirates and seven powers who all want the map. Fly badly
enough for long enough and the run ends — but the world does not, and the next
operator starts in it exactly where you left it.

It is written in pure C99 with no external source or package dependencies, as
both a library with a clean `fly_*` API and a CLI application. The simulation is
deterministic and every state change goes through a serializable command, so
headless play, replay, the test suite and a later server/client split are all the
same seam. It ships its own renderer — a scalar C reference path, a headless
OpenGL ES 3.1 path, and a path tracer — and its own UI, HUD, YAML, image and font
code.

A C99 compiler and Python 3 are the only headless build tools; windowed builds
also link the operating system's native graphics libraries. Third-party C is
vendored under `ext/` with its licenses.

## Highlights

- **A flight model, not a cursor.** All-up mass including fuel and freight,
  control power against dynamic pressure, a wing that lets go at an angle with
  buffet and autorotation, propeller torque and p-factor, ground effect, wind
  shear, correlated gusts, an undercarriage made of tyre forces, and both
  structural limits. Planes and drones are different machines end to end.
- **A planet, not a heightfield.** 600 km ball, continents and ocean basins,
  orogeny, aridity and a snow line, Keplerian orbits above the air, and a
  wrapping azimuthal-equidistant chart with no seam in it by construction.
- **Water on the land, and it found its own way there.** Rivers are traced by
  steepest descent from the catchments that carry the most country, down a
  surface flooded lowest-first so that no descent can dead-end — and where one
  did, the basin filled to its lowest lip and that is the lake. The channel is
  cut into the heightfield rather than painted on it, so a river is a valley you
  can follow home with the instruments out, a forced landing that is flat and
  obvious and wet, and something both the road network and the rail have to get
  across.
- **Ownership and loot.** Six typed module slots, forty-seven modules, five
  rarity tiers rolled as instances rather than counted as quantities, and one
  danger field deciding both who hunts you and what falls out of them.
- **Seven powers at war with each other while you fly.** Live ownership,
  pressure and diplomacy — all of it arithmetic, none of it random — an oath you
  can swear, and kit nobody else sells.
- **A sky that is not staged.** Twenty-odd AI pilots trade, haul, patrol and prey
  across the whole map, in full six-degree-of-freedom flight near you and on the
  same task machine and economy out of sight.
- **Roads, rail and freight.** A surveyed road network with earthworks and
  cuttings, a rail line on piers you can ride, and convoys worth robbing or
  escorting. The two networks keep out of each other's corridor rather than
  braiding along it, and where a road does have to cross the line, the line goes
  over it on a portal with the headroom a lorry needs — lit, like both ways are
  for their whole length, by lamps whose light lands on the carriageway, the
  verge, the batter and whatever is standing in it rather than on a patch
  painted under the column.
- **A sea with something on it.** A third of the chart is water, and the survey
  that lays the roads lays the shipping on it too — the same search with its
  medium turned over, so water is the cheap ground and land is what the line may
  not cross. Coasters run freight between harbours a lane at a time, worth
  raiding and worth escorting and carrying several times what a road column
  does; where a settlement meets the water there is a quay on piles with a crane
  over the berth; and the fairway between them is marked by lateral buoys that
  occult after dark, which at night is the only thing out there.
- **A worked country, not a wilderness with airfields in it.** Every settlement
  encloses the ground around it: parcels with straight edges and a crop of their
  own, hedged or walled, thinned out of the wood they replace, with farms and
  their lanes standing in the corners of the fields. The pattern is a property
  of the world rather than of the frame — the woodland gives way to it, both
  renderers draw it from the same field, and the far quarters stay empty, which
  is what makes them read as far. Between two parishes, where the belt does not
  reach, a transmission line follows the road: a lattice tower every couple of
  hundred metres of carriageway and five conductors hanging between them, on one
  side of the road for its whole length, spanning the ground it may not be built
  on rather than leaving a gap in itself.
- **Three rendering pipelines that agree.** Rasterizer, path tracer and a hybrid,
  on CPU or GPU, sharing one physically-based atmosphere, one water material and
  one set of GLSL/C twins held together by parity tests — all three shading the
  ground per pixel, so the software fallback draws the same landscape rather
  than a coarser one.
- **Two layers of cloud that behave like cloud.** A cumulus deck marched as a
  medium, with tops that follow the weather and a crown that reads lit from
  above and shaded from below, and a cirrus sheet seven kilometres over it that
  is a sometimes thing rather than a lid — and that casts, so the sun goes soft
  on the landscape under it and on the deck below it.
- **A dusk that comes out of the geometry.** The planet's own shadow decides how
  much of a ray's air is still lit, so civil, nautical and astronomical twilight
  arrive on their own — an arch in the sun's own quarter of the sky that reddens
  and narrows as the night comes up, stars weighed against the sky in front of
  them rather than against the clock, and one moon lighting the ground through
  what the ground is made of.
- **The test suite is the gallery.** The end-to-end suite plays the game headless
  and renders every screen, pipeline, settlement, hour and weather it covers —
  see [docs/coverage.md](docs/coverage.md).

## Quickstart

### Setup

Run the platform setup script from this project's directory (`apps/fly`) before
building:

**Posix:**

```sh
./setup.sh
```

**Windows**

```powershell
./setup.ps1
```

The scripts ensure Python and a C99 compiler (LLVM/Clang by default). On Linux,
`setup.sh` also installs the X11 and OpenGL development libraries (the windowed
backend needs X11/GL; the headless GPU renderer needs EGL and GLES, and both are
optional — the CPU renderer is the fallback and the reference). Windows needs
nothing extra for either: the GPU renderer gets its context from WGL and
`opengl32.dll`, which ship with the system.

### Build

On Linux and macOS, run:

```sh
./make.py                 # library + CLI  -> build/<rid>/fly99
./make.py --test          # + e2e suite (plays the game, renders view stills)
./make.py --window        # + realtime OpenGL window (needs X11/GL dev libs)
./make.py --roll          # amalgamate the library into build/<rid>/fly.h
./make.py --release       # optimized build
./make.py --no-gpu        # build the CPU renderer only
./make.py --no-threads    # CPU path tracer on one core (same frame, slower)
```

On Windows, use the same options through Python, for example
`python make.py --test` or `py -3 make.py --test`.

### Test

```sh
./make.py --test                    # the whole suite, including the gallery
./make.py --shots                   # publish the gallery into docs/
build/<rid>/test --list             # every runnable aspect (engine, gameplay, gallery)
build/<rid>/test --units            # correctness aspects only, no rendering
build/<rid>/test --cover            # the whole visual gallery
build/<rid>/test --cover --fast     # ...accelerated (smaller frames, fewer samples)
build/<rid>/test --hq water         # just the hybrid-water hero shot, high quality
build/<rid>/test airframes          # 4-view plates of every airframe in data/
build/<rid>/test locations views    # any subset by aspect or group name
```

The suite writes stills of every UI view and key in-game moment to
`build/<rid>/shots/*.png`, timing each frame as it renders. `./make.py --test &&
./make.py --shots` refreshes [docs/coverage.md](docs/coverage.md); `--shots`
refuses anything but a complete, full-quality run.

### Play

```sh
# Play a new world
build/<rid>/fly99 new    --seed 7 --save run.yaml
# Resume saved world (windowed)
build/<rid>/fly99 play   --save run.yaml
# Resume saved world (headless autopilot); `market` lists the route indices,
# and so does any refusal
build/<rid>/fly99 play   --save run.yaml --ap 2 --steps 36000
# Chart the world as a still: the map overlay over the saved game's discoveries
build/<rid>/fly99 render --save run.yaml --view map --out map.png
# One frame through the hybrid renderer, four paths a pixel, supersampled 2x
# (no --save: the default save file if there is one, a fresh world if not)
build/<rid>/fly99 render --view play --mode mix --pt-samples 4 --ssaa 2 --out shot.png
# Fly the save headlessly for a minute of game time and dump the telemetry
# (t, position, speed, height above ground, fuel, hull, wear, stall, dock) as CSV
build/<rid>/fly99 sim    --save run.yaml --steps 3600 > telemetry.csv
# Print what the site under the wheels is paying, what is on its shelf, what is
# in the hold, which contracts it is offering, and every charted route out of it
# with the index `--ap` takes
build/<rid>/fly99 market --save run.yaml
```

Commands that take a `--save` resume it when it is there and start a new world
when it is not, saying which — on stderr, so `sim`'s CSV on stdout is a CSV and
nothing else. A save file that exists and cannot be read is an
error rather than a new world.

Views: `play` (world + HUD), `self` (pilot profile & loadout), `help` (guide),
`edit` (actions/market/upgrades overlay), `map` (world chart).

Rendering modes: `raster`, `pt` (path traced), `mix` (traced environment +
rastered objects through shared depth). `--quality low|medium|high|ultra`
(default `medium`) picks detail and sampling together; `--ssaa N` and `--msaa N`
override the level's choice where given.

### Controls

**Flight**

| | |
|---|---|
| `*arrows*` | pitch/roll |
| `q` \| `e` | yaw |
| `w` \| `s` | throttle |
| `z` \| `x` | pitch trim |
| `b` | brakes |
| `f` | gun |
| `c` | release ordnance |
| `v` | shed a countermeasure |
| `a` | autopilot |
| `tab` | edit overlay |
| `m` | chart |
| `1` \| `2` \| `3` | views |
| `enter` \| `esc` | select \| back |

**On foot** (crash recovery, disembarking, boarding the rail): `w`|`s` move,
`*arrows*` strafe, `space` jump (hold to bunny-hop), `g` interact, `tab` actions.

**Mouse / touch**: drag the left half of the play view to fly a virtual stick
(drag down = nose up); the right-edge strip is an absolute throttle; the action
column beside it grows a pad per weapon fitted. On foot, drag anywhere to look.

**Joypad**: analog axes drive pitch/roll/yaw with an absolute throttle axis
(`FLY_EV_AXIS` events; the Linux backend reads `/dev/input/js0` — axes 0/1/2/3 =
roll/pitch/yaw/throttle, buttons 0-5 = fire/launch/decoy/brake/autopilot/edit).

The in-game guide (`help`, the `?` button) covers trim, the slip ball, the angle
of attack scale and what to do when a missile warning sounds.

### Use it as a library

`./make.py --roll` produces `build/<rid>/fly.h`, a single-file library:

```c
#define FLY_IMPLEMENTATION
#include "fly.h"

fly_app app;
fly_app_init(&app, 960, 540, seed, FLY_CRAFT_PLANE);
fly_app_step(&app, dt);
fly_app_shot(&app, "frame.png");
```

Modules (also usable directly from `src/`): `fly_math`, `fly_rng`, `fly_img`,
`fly_yaml`, `fly_sim`, `fly_ord`, `fly_world`, `fly_river`, `fly_line`,
`fly_rail`, `fly_road`, `fly_sea`, `fly_convoy`, `fly_game`, `fly_render`,
`fly_hud`, `fly_ui`, `fly_app`.

## Where things are

| Path                | What                                                           |
| ------------------- | -------------------------------------------------------------- |
| `CHANGELOG.md`      | What has been built and why — the reasoning, numbers and dead ends |
| `TODO.md`           | What is left, one flat list sorted by impact                   |
| `AGENTS.md`         | The rules any change to this project is held to                |
| `docs/coverage.md`  | Generated visual-coverage gallery with per-shot frame rates    |
| `docs/shots/`       | Checked-in gallery stills (refresh with `./make.py --shots`)   |
| `data/airframes/`   | YAML airframe assets, one per catalogue entry                  |
| `data/modules.yaml` | The module registry, mirroring the built-in table              |
| `data/fonts/`       | Bundled UI and monospace TTF assets                            |
| `src/fly_*.c/h`     | Library modules; each header opens with what it is for         |
| `src/fly_win.c`     | Optional window backend                                        |
| `src/main.c`        | Primary CLI application entry                                  |
| `src/test.c`        | E2E suite (unit + gameplay + stills) & visual-coverage harness |
| `ext/`              | Vendored source plus license notices                           |
| `make.py`           | Build CLI: compile, test, run, roll, refresh shots             |
| `pyproject.toml`    | Project configuration for Python tools                         |
| `setup.sh`, `setup.ps1` | Install the Posix / Windows build tools                    |
| `build/<rid>/fly.h` | Amalgamated single-file API library (generated)                |
| `.github/workflows/` | CI: build matrix, unit suite, sanitizers, CLI smoke test      |
