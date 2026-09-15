# fly99 — TODO

Everything still open, in one flat list, most impactful first. Impact is read as
what it costs the game to leave undone: things a player meets and things that
block other work come before things only a maintainer would notice.

Nothing delivered is recorded here. The reasoning behind what exists — including
the measurements, the dead ends and the mistakes worth not repeating — is in
[CHANGELOG.md](CHANGELOG.md); the rules a change is held to are in
[AGENTS.md](AGENTS.md). Several entries below cite a section of the changelog
because the argument for the fix is already written down there.

Before starting anything here, read `CHANGELOG.md`'s **Constraints** and
**Measured dead ends** sections. Several of the obvious approaches have already
been built, measured and reverted.

---

- [ ] **[gameplay] A field is somewhere to put an aeroplane down, and nothing
      knows it.** The worked country draws flat, cleared, hedged ground all
      round every settlement, and to the game it is terrain like any other:
      `fly_world_tilth` is read by the renderer and by the woodland field and
      by nothing that decides anything. A parcel is the first alternative to a
      pad the game has ever had — an unplanned arrival that ends in a crop
      rather than in a wood is a different outcome, and the field already knows
      which ground is which. Two things would use it: the crash and salvage
      path, which currently asks only what the terrain is doing, and the
      recovery walk, which is looking for somewhere to walk to. It wants doing
      after **[gameplay] Paid recovery**, which is what decides whether a
      survivable arrival is worth anything.

- [ ] **[world] A deck in open water, and putting down on one.** The lanes are
      laid and the shore is built on — quays, cranes, lit marks, coasters worth
      raiding — but the sea still has nowhere to *land*. Two things are left of
      the original entry and they were deferred together because both are
      cross-cutting rather than hard. The first is the offshore platform:
      `draw_pad` on `draw_pier`, inheriting the whole of the site machinery —
      gates, market, contracts, a beacon — for a deck reachable by nothing but
      a VTOL airframe. It wants to be a `fly_location`, and a location grades
      the heightfield around itself in the C *and* in the GLSL twin (a platform
      must not raise an island under itself, so `pad_z` is its own bed rather
      than `FLY_PAD_MIN_DRY`), answers to `fly_world_approach_survey`, has to be
      kept out of the parish belt and the woodland clearing, and lands in
      `game.autopilot`'s "every reachable site is flown loaded and reaches its
      pad" — which is the gate to check it against and the one that decides
      whether the gating is right. `gpu.ground` covers the half of it nothing
      here can run. It is also what a world with one harbour needs: a platform
      is a lane terminus that does not need a second coast.
      The second is landing on a hull, which is a *moving* deck and a new
      arrival case for `game.touchdown` — the entry's own caution, and still
      the second thing to try rather than the first.

- [ ] **[world] Nothing in the world is alive.** Everything that moves out
      there is machinery: aircraft, trucks, a railcar and a saucer. No birds
      over the coast, nothing in the wood, nothing on the open ground that is
      half the land, nothing in the sea. It is the cheapest immersion left on
      the list and the only one that adds *motion* rather than more geometry,
      and it can be built with no state whatsoever: a flock's position can be a
      closed-form function of seed, cell and time evaluated where it is drawn —
      the discipline the scatter already keeps — so there is nothing to save,
      no command, no `think_rng` draw and nothing on the multiplayer seam. It
      reuses the cell walk in `draw_boulders`, `patch_visible` for the
      projected-size gate, and a three-rung ladder like `tree_lod`'s, because a
      bird at 900 m is one triangle and a bird at 40 m is not. It should read
      the air it is in rather than inventing one: `fly_world_weather` already
      gives wind, turbulence and precipitation at a point, so a flock drifts
      downwind and sits down in a squall, and that alone makes the weather
      visible from outside the instruments. Gameplay: a bird strike is damage
      of a kind the flight model already applies, and it is the first cost low
      fast flying has ever had; game breaking from under the aeroplane's shadow
      and seabirds standing off a shoal are what make a landscape look attended
      to rather than rendered. The care needed is the frame budget — the near
      field is where `render.budget`'s 33 ms rung is spent — so the layer
      prices itself off `lod_for` the way the blade and scrub layers do, and
      thins at `low` rather than being switched off, on the same argument that
      keeps the boulders at every rung.

- [ ] **[powers] The war is invisible from inside the aeroplane.** Seven powers
      hold ground, sites change hands in hours and `game.factions` measures the
      map moving — and the only thing anywhere in the world that says any of it
      is the flag over the pad (`draw_banner`) and the paint on a hull. A
      contested field looks exactly like a quiet one from the air, and the
      front is drawn nowhere at all, so the system whose whole job is to make
      the map mean something cannot be seen from the cockpit. Put it on the
      ground where the powers meet: dug-in positions and revetments, a
      checkpoint where a road crosses between two powers at war — the network
      gives the crossing and `fly_road_lane` already decides which side of the
      road a vehicle is on — a battery on contested ground that actually
      shoots, which needs no new code to fire because the convoy escort's gun
      is exactly that (`fly_convoy_class.gun_dps`, `gun_range`, and the
      cooldown that paces it), and what a fight leaves behind: burnt hulks,
      cratered ground, a strip nobody uses any more. Two entries below pay for
      the placement. **[powers] The war needs a reason to go somewhere** wants
      `fly__site_owner` read after world-gen, and the same preference table
      says which ground is worth fortifying; **[powers] Faction contracts**
      wants somewhere worth sending a pilot, and a front is that place.
      Gameplay: crossing a contested boundary should cost something, which
      would be the first time `fly_world_danger` answers to the war rather than
      only to pirate country. It has to stay a field and not a wall — a hard
      border turns the route between two sites into a rule, where the whole
      point of the danger field is that the short way is a risk you may take.

- [ ] **[gameplay] Fuel as a stranding pressure.** A reserve requirement, burn
      that responds to altitude and headwind, and no free top-up. Only worth
      doing once being stranded is expensive, which is what the two entries below
      buy.

- [ ] **[gameplay] Paid recovery.** Beyond walking range the wreck needs a tow,
      bought with tokens or reputation, with liquidation as the fallback when
      neither is there. Today the recovery walk starts at the nearest service
      site for free, which is the last remaining free undo.

- [ ] **[gameplay] Contested and decaying salvage.** A wreck should be a race,
      not a guarantee: a claim window, and outlaws or patrols diverting to loot
      an unclaimed one. That is a `SET_LEG` and a task inside the existing pilot
      command vocabulary, so it costs nothing on the multiplayer seam, and it
      makes the world visibly react to the player's worst moment.

- [ ] **[gameplay] Weather that damages rather than merely degrading handling.**
      The storm belt and `fly_world_danger` are the reason not to fly the
      straight line, and neither currently costs anything a repair does not undo.
      Closer than it was — gusts are correlated and act through the angle of
      attack, so a squall on short final can take a wing over the stall by
      itself. What is missing is a cost that survives a repair.

- [ ] **[sim] The hold-off still floats the aeroplane across its own pad.**
      Every leg arrives now, but three of the thirteen on seed 4242 put the
      wheels down 462, 621 and 841 m *past* the pad and taxi back: the flare
      asks for seven tenths of a metre a second whatever the ground ahead is
      doing, and eight metres at that rate is eleven seconds, which at approach
      speed is four hundred and sixty metres. Bounding the sink by the apron
      left was tried three ways and every one of them cost structure for no
      change in outcome — see the changelog's *Measured dead ends*, which has
      the numbers. What is probably wanted is less energy at the flare rather
      than a steeper one, and that is a change to the approach speed, which is
      a wide number. `game.autopilot`'s structure-per-leg measure is the gate,
      and it must not move.

- [ ] **[ui] Interactive world map / mission planner.** Pan and zoom, an info
      card per site (kind, distance, fuel-range reachability, gates met/unmet,
      market snapshot, offered contracts), a fuel-range ring, and toggleable
      overlays for gates, threat (`fly_world_danger`), weather cells and the
      storm belt. Engage autopilot or plot a multi-leg route from the map.

- [ ] **[ui] The chart is legible and is still not a map.** The terrain under the
      war is hill-shaded and hypsometrically tinted (`ui.chart`), and the names
      are placed rather than offset — twenty candidate positions per site,
      first one clear of every other name, of every dot and of the chart's own
      edge, with a leader line whenever a name has been pushed off its own
      flank (`ui.chart` holds no-overprint over four seeds at two frame sizes).
      What is still missing is the map part: it is one 1024-pixel square with
      no pan, no zoom and no scale bar. The planner above is where this goes;
      the entry exists so the shading is not mistaken for the job being
      finished.

- [ ] **[ui] Objective HUD and a unified alert stack.** A persistent objective
      strip (active contract, destination, reward, deadline) plus a prioritized
      master-caution stack with escalating salience, replacing today's scattered
      ad-hoc warnings.

- [ ] **[ui] Contextual detail panels and onboarding.** Focusing an actions row
      shows the consequences before committing — module stat deltas, trade-in
      cost, gates met, cargo and fuel effects — plus a short contextual tutorial
      the first time each system is used.

- [ ] **[ui] Trade & market intelligence screen.** Prices for each resource
      across all discovered markets — spreads, best arbitrage routes with
      expected profit against distance and fuel burn, contracts sorted by
      profitability, trend arrows off the supply/demand economy.

- [ ] **[ui] World-space navigation and tactical markers.** Diegetic labels on
      locations, a destination reticle with distance/bearing/ETA, an autopilot
      cue ribbon, and contact callouts drawn in 3D so radar blips get world-space
      echoes.

- [ ] **[powers] The war needs a reason to go somewhere.** Pressure is symmetric
      — every power wants every field the same amount — so the front line wanders
      rather than advancing. Powers should want *particular* ground: the Foundry
      should fight for mines and shrug at skyports. The site-weight table
      `fly__site_owner` already encodes exactly that preference for world-gen and
      nothing reads it afterwards.

- [ ] **[powers] Faction contracts.** A power you have sworn to should be able to
      *ask* for something — a delivery into a field it is trying to take, a
      patrol of a contested one — rather than only paying standing for work you
      were doing anyway. That is a contract kind, not a new system.

- [ ] **[powers] Pilots do not fly their power's hulls.** A Foundry courier gets
      that power's cargo pod and plating painted onto a kind-chosen airframe, not
      a Slagback: `fly_pilot` cannot see the airframe catalogue without reaching
      up a layer. The footing is level now that the player is an actor, so what
      is left is the catalogue reach — and note that it will move combat
      outcomes, which is what the changelog's warning about the fleet's numbers
      is about.

- [ ] **[pilots] Combat is far deadlier out of view than in it.** A coarse hunter
      is handed `CRUISE * 1.45` and turned to point exactly at its mark, so
      `gun_solution` succeeds almost immediately: over three hours the coarse
      world converts 12-29% of hunts against roughly 1% in the full simulation,
      and one seed recorded 83 kills coarse against 7 flown. Whatever is done
      should be done to the *coarse* side — the flown number is the honest one.
      This is a rebalance rather than a bug fix, which is why it was left alone.

- [ ] **[pilots] Lift the ordnance gate once the coarse side is honest.**
      Everything in `fly_ord` is restricted to aircraft flown in full, because
      handing a missile to a coarse hunter in a perfect firing position would
      multiply the asymmetry above rather than add anything. Convoy launchers
      follow the same rule. When the entry above lands, this gate is the first
      thing that should come off.

- [ ] **[pilots] Gun conversion is a tracking problem — measure cone time.**
      1381 engagements at 20 Hz is sixty-nine seconds of gun solution across
      three hours and twenty aircraft, 0.05% of airborne time. Anything that
      wants to raise the kill rate should measure time inside the cone directly
      and leave the lead alone: four interception geometries were tried and every
      one was worse, because the gun is hitscan and pure pursuit is its correct
      terminal law.

- [ ] **[convoys] An escort in the air.** A Leviathan train is the most valuable
      thing on the ground and nothing at all flies over it. A patrol whose leg is
      "follow this column" costs nothing on the multiplayer seam — a `SET_LEG`
      and a task inside the vocabulary pilots already have — and it turns the top
      tier from a hard target into an engagement. Partly answered from the other
      side, since an armoured column and above now carries its own launcher, but
      an escort is the interesting version because a column cannot chase.

- [ ] **[convoys] Somewhere for a column to stop.** Under fire a convoy runs for
      the far end and nothing else, because a road has two ends and one of them
      is home. A halt, a turn-round, or a bolt for the nearest settlement is the
      difference between freight and a train on rails, and the alert flag that
      would drive it is already there.

- [ ] **[world] A crossroads is still only a height.** Two roads that cross now
      meet at one level — the later one is pinned to the earlier one's deck at
      the crossing (see the changelog) — and that is the whole of the junction:
      there is no flare, no give-way marking, no widening and nothing on the
      approach to say a road is about to arrive from the side. Twelve of them
      across five seeds, all in open country. The pieces are all there (the
      crossing is found during the drape, the deck is drawn from a cross-section
      and the paint is already a pass over it); what is missing is a decision
      about what these junctions *are* — a priority crossroads with a stop line
      on the minor arm is the honest answer for a hauling network, and it wants
      the minor arm identified, which the network does not currently rank.

- [ ] **[render] The crossing portal is a straight beam under a curved line.**
      `draw_rail_portal` seats its girder on the two points the guideway is at
      either side of the road and spans them with one box. That is right to a
      couple of centimetres at the spans a square crossing needs and visibly
      wrong at the 46 m cap, where the line's own curve and grade put the barrel
      a little off the beam's crown in the middle. The fix is the same one the
      barrel itself uses — subdivide along the drawn curve — and it is not done
      because every crossing in the seeds surveyed so far is inside 30 m of
      half-span, which is where the error is under the pipe's own radius.

- [ ] **[render] A lamp casts no shadow and reads every surface as level.**
      `lamp_light` is a point source evaluated against the depth buffer, and it
      knows two things it should not: it lights the ground on the far side of
      anything standing in the pool — a column, a truck, a shed wall — because
      nothing occludes it; and it takes the surface as horizontal, so a vertical
      face a couple of metres under a lantern comes out about three times too
      bright. The second is the cheaper of the two to fix and the more visible:
      a normal off the depth buffer is one cross product of two screen-space
      derivatives, and what stopped it is that a derivative is wrong at exactly
      the silhouettes a frame full of lamp-posts is made of, so it wants an
      edge-aware pick (the smaller of the two one-sided differences per axis)
      and the same pick in `FLY_LAMP_FS`, where `dFdx` offers no choice. The
      first wants a shadow the lamp does not have and is worth doing only if
      lamps ever grow past street lighting.

- [ ] **[convoys] The player should be able to be robbed by the ground.** Columns
      are attacked from the air by everybody and from the ground by nobody. An
      outlaw column — a tier that hunts freight rather than carrying it — is the
      same struct with a different table row, and would make a road something to
      watch rather than only something to shoot at.

- [ ] **[loot] A saucer wreck's item level is the ground it fell on.** The drop
      is the best in the game by rarity, at bias 5.0, but the level comes from
      `fly_world_item_level` at the dirt underneath — so the one thing you need a
      rocket to reach can shed its four items at the level of a quiet meadow.
      Giving the sky its own level is a change to what `game.saucer` measures and
      wants doing deliberately.

- [ ] **[world] More earthworks than a uniform array holds.** Cuttings reach the
      GPU as `uCut[72]` — twenty-four boxes of three vec4 — and that cap is the
      only thing stopping the network from grading itself properly: on seed 4242
      the mean change of gradient between stations goes 0.92% at 24 boxes, 0.82%
      at 48, 0.69% at 96 and 0.59% at 192. Ninety-six boxes is past what GLES 3.0
      guarantees a vertex stage, and the path tracer uploads the world unculled
      into a fragment stage with a smaller guarantee still. Lifting it means
      moving the corridor list into a texture read with `texelFetch` and giving
      `fly_ground` the spatial cull the footprinted draws already get from
      `env_ground_uniforms`.
      The water on the land is now up against the same wall and pressing on it
      harder. `uRiver`, `uRiverBox`, `uRiverPt` and `uLake` are another
      seventy-eight rows beside the cuttings' seventy-two, and what they buy is
      `FLY_RIVER_PT_MAX` — sixty-four stations shared between at most four
      watercourses in a whole world, which is a station every few hundred
      metres. That spacing is what decides how closely a drawn channel can
      follow the ground it was surveyed against, and it is the whole of the
      residual `world.river` measures: a chord held under the section at both
      ends and at seven points between them still steps by up to a metre at the
      waterline where a dip falls between two stations it could not afford. The
      texture read would buy longer rivers, more of them, and a station density
      that takes that number to nothing.

- [ ] **[world] A working is a hole, and there are no holes left to dig.** The
      corridor half of the worked country is in — `road_pylon_at` puts a mast
      every 220 m of carriageway beside every road and hangs five conductors
      between two of them, which is what carries the long stretches between
      parishes where the belt does not reach. The other half of that entry is a
      **quarry or a spoil tip**, and it is blocked rather than merely undone: a
      working is a hole, and every mechanism the heightfield has for making one
      is already spent — `fly_cut` capped at `FLY_CUT_MAX` and taken in full by
      the road network, and the channel stations capped at `FLY_RIVER_PT_MAX`
      and taken in full by the rivers. A working painted flat on the ground is a
      grey field, so there is nothing worth building here until **[world] More
      earthworks than a uniform array holds** above is done.

- [ ] **[renderer] The ice casts no bands on the cloud tops.** The sheet dims
      the ground under it and the deck below it, but the deck's share is one
      number a frame — `cirrus_shadow` at the camera's own column, which is the
      fidelity the rest of the deck's lighting already has. What is missing is
      the shadow moving *across* the tops, and that wants the lookup per march
      tap. It was built and measured and is not worth its price at four field
      lookups a tap: +28% on the worst sky frame in the game, against the 20%
      the whole ice layer cost to add. Something cheaper than a field lookup —
      the shadow rasterized once a frame onto a coarse world lattice and read
      back bilinearly, the way the occlusion window is — is the version worth
      trying, and it has to be anchored to a world lattice or it will crawl.

- [ ] **[renderer] The budget rung still shades the ground at its vertices.**
      Every rung above it resolves the albedo, the material and the light per
      pixel (`raster_tri_ground`), which is what took the terrain quilt out; LOW
      keeps the per-vertex path because it is a 33 ms frame budget on one CPU
      core and the fragment stage costs 24% of an aerial frame. So the quilt is
      still there on the cheapest rung, and the thing that would fix it without
      paying for it is a cheaper `terrain_surface` rather than a different rate
      — the mottle and the woodland field are two lookups, and the rest of that
      function is skipped at range already.

- [ ] **[renderer] The GPU shades terrain without the canopy medium.**
      `FLY_ENV_TERRAIN_FS` takes `fly_shadow_sample` where the CPU's
      `terrain_sun` takes the map *and* `canopy_sun`, so out past the near
      cascade — where the wood is a medium rather than crowns — a GPU frame
      lights a forest floor as though the wood were transparent. `fly_sun_at`
      is the GLSL twin and is written; nothing calls it. Unverified on hardware,
      because nothing here has a GL driver, and `world.canopy` measures the
      claim through a CPU probe so it would not catch this.

- [ ] **[renderer] Distant shadows are limited by the caster grid, not the map.**
      The far cascade rasterizes the heightfield at about 160 m per cell into
      12 m texels, so a mountain's shadow edge is a thirteenth of the resolution
      the cascade could record. Raising the cell count is the obvious move and
      the wrong one: the grid is rasterized once per cascade and is what a
      cascade costs, measured at +29% going from 112 cells to 160. Something
      adaptive, or a separate coarse silhouette pass, is the real answer.

- [ ] **[renderer] Sample the sun's own column where the air is.** `zm` feeds the
      sun's transmittance and is not taken at the segment's lowest point, unlike
      everything else since the terminator fix. Moving it is the better model and
      a retune of the entire exposure: a zenith ray at noon would pick up about
      8 km of sun column where it now picks up 40 km's worth of nothing, which
      darkens the zenith by about a quarter in blue and less in red, so the
      anchor colour (0.10, 0.24, 0.44) and every constant set against it move
      together. Its own job, with the noon zenith re-anchored first.

- [ ] **[perf] Cache the chart mapping per terrain cell.** `fly_world_ground` is
      the hottest function in the engine and went from 206 ns to 304 ns a sample
      with the wrap, then to 469 ns with the continental field. The mapping is
      currently recomputed per sample; caching it per terrain cell rather than
      per sample is the obvious next lever and is not done.

- [ ] **[perf] The fog anchor around a settlement's props.** `fog_anchor_begin`
      is general and nothing but the wood and the scrub opens one. A shed, a
      tank, a crate and a gantry are all inside 26 m, and settlements are 4.7 ms
      of the object build. There is no single choke point to put it at — there
      are two dozen `prop_*` builders — which is the only reason it is not done.

- [ ] **[perf] The specular block in `tri_lit_n` runs per vertex on a flat
      triangle**, where the only thing varying across it is the view direction.
      Sharing it has the same shape as the fog anchor and a worse failure mode:
      the sun lobe is sharp on a glossy surface, so it wants its own gate and its
      own measurement rather than the fog one.

- [ ] **[ui] A per-airframe tuning page.** The numbers exist and are
      per-airframe; what does not exist is a page that lets an operator move them
      — prop pitch, wing incidence, a flap schedule, rotor size, fuel/cargo
      balance, overboost — with live predicted performance curves and visible
      geometry following the setup. A UI feature on a simulation that is ready
      for it, rather than a simulation feature.

- [ ] **[net] `fly_game_restart` should collapse into the pilot death path.**
      Now that the player is an actor, seating a new operator and replacing a
      lost pilot are the same operation written twice.

- [ ] **[net] Snapshot delta encoding of `fly_game_state`.**

- [ ] **[net] Self-contained command stream over TCP**, lockstep or
      server-authoritative. Any approved transport source must be vendored with
      its license.

- [ ] **[tests] A positive gate for the storm grey.** Nothing currently fails if
      the storm term is deleted: `render.atmosphere`'s aerial-perspective row
      used to catch it at ratio 1.36, and with the column stopping at the real
      horizon that overshoot is arithmetically impossible. The gate that would
      catch it is a positive one — a storm must measurably grey the distance,
      saturation of a far ridge falling and the in-scatter converging toward the
      grey rather than toward the clear-air sky.

- [ ] **[tests] Attribute the difficulty knobs.** `game.ruin` holds
      reachability, the no-false-ruin rule, the salvage geometry and the fatigue
      ratchet. It does not yet hold a claim about the *rate* — how many hours of
      ordinary play a hull survives — because that needs a long probe of the kind
      the pilot attrition numbers came from.

- [ ] **[tests] Attribute the pace of the war.** `game.factions` bounds the
      number of seizures and the largest holding over six hours, which says the
      map is neither frozen nor decided. It does not hold a claim about how long
      a *front* lasts. Seed 4242 measured 7 seizures in six hours and 7 in
      twenty-four, which is not a straight line and nobody has explained why.

- [ ] **[tests] Water's near-field detail has no gate of its own.** Two metrics
      were tried and both measured something else: high-frequency contrast near
      the camera comes out inverted, because contrast on water is dominated by
      grazing-angle glint, and frame-to-frame change cannot tell a half-metre
      ripple genuinely moving from unresolvable speckle. A metric that works
      probably has to hold the sun still — an overcast frame, or the surface
      sampled through a probe rather than through a render. Covered meanwhile by
      `render.material`'s open-water check and by `gpu.frame`.

- [ ] **[tests] The gallery has lost its GPU half.** `docs/coverage.md` used to
      carry three GPU pipeline frames beside the three CPU ones, and the host
      the cloud work was last done on has no EGL or GLES at all — so
      `make.py --shots` drew everything through the CPU renderer, dropped
      `pipeline_raster_gpu`, `pipeline_hybrid_gpu` and `pipeline_pathtraced_gpu`,
      and replaced every llvmpipe timing with this host's. Nothing is wrong with
      the code; it wants one rerun on a machine with a GL driver.

- [ ] **[tests] Nothing here has been measured on a GPU.** Every frame-time
      figure in the changelog and the gallery is llvmpipe, so they say what the
      algorithms cost and nothing about what the hardware path does — and on
      llvmpipe "GL memory" is main memory, so the residency work is invisible in
      a wall-clock number here however much it is worth on a card. What wants a
      machine with a card in it is the split between geometry, shading and the
      two CPU traversals of the scene.

- [ ] **[tests] The caster grid snap is not separately gated.** Snapping the
      terrain grid's origin to a world lattice is the same fix as snapping the
      light-plane origin and is right for the same reason, but removing it
      changes neither lattice sweep. It is kept on the argument rather than on a
      measurement.

- [ ] **[ui] There is no view of the chart as a disc.** `fly_ui`'s map is a
      linear plot of chart coordinates over `+-FLY_WORLD_HALF`, which is right
      for the settled world and says nothing about the rest of the planet.
      Nothing is wrong with it; there is simply no view of the whole disc, and
      the wrap is now the sort of thing a player could see if there were.

- [ ] **[arch] A route leg is not a straight line any more.**
      `fly_world_delta` gives the initial bearing and the callers that fly it
      re-measure every step, which is correct. Anything that wants the *path*
      rather than the next heading — a drawn great circle on a map, an ETA over a
      long leg — would want the leg sampled rather than interpolated. Nothing
      needs it yet.

- [ ] **[renderer] A stronger ground grain has to buy its headroom first.** Only
      about a third of the amplitude an octave loses can be handed to the
      projection-locked grain before a faded sample stops being twice as close to
      the patch mean as a point sample — the figure `render.material` has always
      required, and most of that headroom already goes to the coarse octaves the
      fade does not touch. Anything wanting a stronger grain has to give the
      coarse variation the same treatment, and must not get it by loosening the
      check.

- [ ] **[renderer] Residual grazing-sun noise** in terrain shading, from
      self-shadowing micro-relief. Arguably correct rather than an artifact;
      listed so the next person does not spend a day deciding that independently.
