# fly99 — Changelog

The record of what has been built and why it is built that way: the reasoning
behind each system, the measurements that justify it, and the wrong turns taken
on the way there. It is written to be read by whoever picks the project up next,
so an entry is kept when it explains a decision that is still load-bearing and
dropped when the code has stopped depending on it.

Three other files carry the rest. `TODO.md` is what is left to do, in one flat
list. `AGENTS.md` is the rules any change is held to. `README.md` is the short
version: what the game is and how to build, test and play it.

There are no releases yet, so this is grouped by subsystem rather than by
version. Within a section the entries run from the design the system settled on
to the corrections that got it there.

---

## The game as built

### Style

A juxtaposition of extremes:

- Gritty industrial realism (inspiration: Factorio)
- Childish utopian fanstasy (inspiration: Disney Pixar)

### Pilot

**Categories**: plane, drone

**Modules**: six typed slots (engine, wings, hardpoint, bay, avionics,
hull) with forty-seven YAML-defined modules (`data/modules.yaml`) — twelve an
aeroplane is made of, nine it is armed and defended with, the five-part rocket
stack, and three apiece that the seven powers build only for themselves; each
fit changes the sim (mass, drag, fuel, guns, warheads, vacuum thrust...) AND
the 3D model (pods, rails, dispensers, tanks, domes, extended wings, a nozzle
out the back).

**Ownership**: modules are inventory you buy first, then fit — unfit and refit
them between airframes rather than trading them in. Each airframe is an owned
instance with its own fuel, condition, wear, location and fitted loadout; buy
or build new ones at a hangar.

**Items**: a module is an object, not a quantity. The registry is the *base* —
what a Gun Pod is for — and what you own is an instance of one, with its own
rarity, its own rolls and its own name: two Gun Pods off two different wrecks
are not the same Gun Pod. Rolls move fields the base module already uses, so an
item can sharpen what a module does and cannot hand it a new job; a gun roll on
a fuel tank would be a gun, and the tank has no barrel to put it in. Every item
is identified by an id and never by its position in a list, because a hold that
identifies items by position hands "fit the third thing" to a player looking at
a list that shifted under them — and the failure is silent.

**Autopilot**: flies routes while you are AFK — with real risk. It aims at the
threshold rather than the pad and rolls the last stretch in, because an approach
that arrives over the apron still airborne floats across it and goes around for
ever. It will not accept a route into a site your airframe cannot enter, and it
says so instead of flying you there to find out; and an approach that is not
working ends in a diversion to somewhere you can land, never in a circuit that
runs until the tanks do.

It flies the same aeroplane you do, so it has to know the same things: it
approaches at a third over the stall at today's weight rather than at a
fraction of a number fixed when the airframe was empty, it flares, it takes the
crab off in the last five metres so the tyres are not asked to go sideways, it
holds the ball centred against the propeller, it will not bank steeply near the
ground, it slows down in rough air, and it puts the nose down and leaves it
there rather than mushing at a ridge it cannot out-climb. What it cannot do is
fly a curved approach into a pad standing behind a rise, and at those it goes
around — which is why its patience is fifteen minutes rather than seven, and
why that patience is now counted from the first time it reached the
destination rather than from the last: a go-around leaves the two-kilometre
bubble by design, so a clock that reset on the way out gave an aircraft nine
separate ninety-second clocks and never bounded anything.

**Arriving is a thing that has happened, not a place you are.** Once the
wheels are down at the destination the autopilot taxis, and full power and a
rotation stop being answers it has. They used to be: the taxi was a
nine-hundred-metre box, and an aeroplane blown a little wide on the ground
fell out of it into the takeoff roll, flew a circuit, landed, drifted and did
it again — 24 of 52 legs on seed 4242 took off again after they had already
settled on the ground at the site they were sent to, and the fifteen-minute
timer, which was also running on mere proximity, eventually read a *successful
landing* as a failed approach and flew the aeroplane to a different site with
the cargo still aboard. A taxi that is not getting any closer, or that is
costing structure, now ends in a stop and a line in the log saying how far
short it is — an aeroplane on the ground at the right place, which is the
worst outcome worth having. Over the same 52 legs: 51 end on the ground at the
site asked for against 48, none at the wrong one against two, none taking off
again from ground it had already landed on against 24, and the mean leg is 497
seconds against 549.

**And a taxi is steered at the pad, on all of its nosewheel.** It goes to the
pad and not to a waypoint chosen for an approach it has stopped flying, and the
test for that is the arrival latch rather than the wheels, because a roll over
natural ground skips and a rule that switches off for two tenths of a second at
a time fights the steering at random. The ball is not flown on the ground
either: coordination is an air rule, and applied to a taxi it spent half the
nosewheel on a clamp and fed the crosswind back in against the turn. Both were
worth a hundred and ninety-five seconds and nine hundred metres to an aeroplane
that had landed long and needed to come round.

The `game.autopilot` aspect flies every reachable site in a world, loaded, and
requires all of them to reach the pad: thirteen of thirteen, with the
stopped-short bucket empty. The bucket is kept, because down at the site asked
for and stopped short of the pad is a landing rather than an abandonment and
counting it as "still airborne" was how the old behaviour hid — but it is gated
at zero, so a taxi that cannot finish is a regression and not a tolerance. The
aspect also holds what the leg *cost*, since an arrival is billed as a landing
however hard it was, and that is where a profile that flies the aeroplane
through the hillside in front of the apron shows up.

### Struggle

**Upkeep**: fuel planning, wear and hull repair.

**Airframe life**: every hour flown spends part of the airframe, faster under g
and in weather it is not rated for. A repair restores integrity and gives none
of the lifetime back, so a hull is eventually condemned on the apron and money
has to keep turning into aircraft. It is the one cost that cannot be switched
off, which is what stops careful play from accumulating past the point where
anything can go wrong — and it is a property of the machine, not of a pilot:
nothing here needs feeding, resting or keeping warm.

**Weather**: drifting storm cells and a permanent storm belt; turbulence
wears the airframe, storms tear it. And, between them, days you would choose
to fly on — the surface wind used to be one noise mapped onto a narrow high
band, so there was no calm anywhere in the world at any hour of any day: mean
29 knots with a floor of ten. Every arrival was a strong-wind arrival, and a
six-metre-a-second taxi into a twenty-metre-a-second headwind is an aeroplane
above its stall speed sitting on its own apron. It is a squared curve now, so
a light breeze is the ordinary case and a gale is weather rather than climate
(mean 11.6 m/s against 15.1, 29% of it under 7.5 against 0.6%) — with the
storm terms untouched, so the belt still blows at 24 (`sim.weather`).

**Combat**: pirates hunt flyers in dangerous regions; fit a gun and dogfight
for bounties. Beyond that the powers fight each other, and the rule that keeps
the sky populated is that only *combatants* are shot for their flag: an armed
aircraft of a power yours is at war with, or an outlaw. A patrol will not
machine-gun a rival's courier over a border dispute, which is both the decent
reading and the one that leaves anybody alive to trade with (see **Powers**).

**Weapons**: a gun is *hitscan* — a line drawn instantly, resolved where the
trigger is pulled, and the only question either aeroplane gets to answer is
whether the nose was on. Everything else is an **object in the air** with
several seconds of life in it, and those seconds are the point: a launch is
the beginning of an engagement rather than the end of one.

| | what it is | what it costs you |
|---|---|---|
| **Rocket pod** | salvo of four, flat and fast, 1.4 km | no correction at all: it hits what was already in front of the nose |
| **Bomb rack** | six, gravity and drag, a 46 m blast | nothing whatever against an aeroplane, and most of the hold |
| **Seeker rack** | four heat seekers: 210 m/s over the aeroplane that fired them, three seconds of motor, then coast | four rounds, and five separate ways for the target to get out of it |
| **Dart rail** | radar-guided, twice that again, 4 km | you have to keep pointing: break off and it goes dumb |
| **Beam emitter** | instant, no lead, no falloff, 700 m | a third of a gun's cone, and it cooks the airframe until it locks out |

**Defences**, because a world that could shoot at you and could not be answered
is a worse world rather than a harder one:

| | answers | how |
|---|---|---|
| **Flare dispenser** | heat seekers | a far brighter heat source than an engine, for about three seconds |
| **Chaff pod** | radar darts | a return that looks like an aeroplane stopped dead in the air |
| **Jamming suite** | both | widens the head's error about where you are; a miss distance, not a shield |
| **Countermeasure suite** | heat, and some of everything | forty flares with a jammer under them |

**And five things you fly rather than buy.** No round in the game rolls a die
to see whether it hits: every outcome is geometry, energy and timing, which is
what makes the counterplay learnable. Against a heat seeker you can **turn**
(it pulls a bounded number of g, and after burnout every second costs it more
than it costs you), **run** (it has a life and a drag, and outside its envelope
it simply expires), **go cold** (a plume is what it is looking at, and an
aeroplane at idle is dim from every angle), **mask** (a ridge between the two
of you and the head is looking at a hillside), or **decoy** — and the last one
is a skill rather than a button, because a flare burns down in about three
seconds. Popped at three seconds out it is the brightest thing in the sky while
the seeker is deciding; popped at nine it has gone dark long before the round
arrives and hands you straight back. A radar dart is a different problem with a
different answer: it is beaten by a hard **beam turn**, which drops you into
the Doppler notch, and a flare does nothing for it at all.

The magazines are finite and rearming is service-site work priced in alloy —
a weapon that runs out is a weapon whose use is a decision. Missiles are also
the one threat with an unconditional warning: bearing and seconds-to-impact on
the glass whether or not you are carrying a receiver, on the same rule the
stall buffet exists under. A threat you cannot see coming reads as the game
cheating.

**Columns shoot back with more than flak.** An armoured column and above
carries a launcher on the bed, which turns the top of the road ladder from
"hurts more" into a different problem: flak is a tax on the seconds you spend
over the road and you can always leave, where a missile follows you home. The
magazine is finite, so outlasting one is a real plan.

**Ruin**: the run can be lost. With no airworthy hull and not enough cash,
cargo, spare modules or reachable wreckage to raise the price of the cheapest
one, the operator is grounded and that run is over. The reserve — everything
the estate would fetch, counted in hulls — is on the HUD at all times, because
a fail state nobody can see coming reads as the game cheating.

### Powers

**Seven of them, and they want the map.** The Concord (licences, patrols, the
rule of the air), the Corsairs (outlaws, and a real power rather than a hazard),
the Foundry (ore, alloy, fuel), the Lantern Compact (luxuries, data, light), the
Halcyon Line (routes, mail, schedules), the Cinder Pact (storm-belt salvagers)
and the Meridian Choir (the ruins, and what is buried in them). Everything else
is *unaligned* — a pilot who owes nobody a flag, a field nobody has taken.

**Every site is held, and holdings change hands.** A site has an owner, a grip
on it and the pressure each power is currently exerting there. Pressure is not
a new activity: it is what the world was already doing. A delivery landed at a
field pushes for whoever flew it, an aircraft on station pushes a little, a kill
overhead pushes a lot, and the grip moves on the balance. At zero the site falls
to whoever was pushing hardest — and it falls *weakly held*, so a front line
moves back and forth instead of ratcheting one way. An empire holds each of its
fields more loosely than a small power does, which is the only thing standing
between this and a map with one flag on it. Nothing in any of it draws a random
number: a world nobody watched comes out the same as one somebody did.

**Diplomacy runs while you fly.** Relations are a live matrix, not a table.
Each pair drifts toward what their creeds imply — two axes, outlaw-to-order and
mystic-to-industrial — and the world argues with it: taking ground off somebody
is the largest single thing that happens to a relation, and powers that share a
war warm to each other, which is what turns seven quarrels into two blocs over
an evening. Three to six of the twenty-one pairs open at war and the rest sit
near enough to a threshold that the seed and the session decide them. Piracy is
the one thing outside the matrix: everybody armed goes after outlaws, because a
seed where the law rolled *cold* on the Corsairs would be a seed with no police.

**You can join one.** Swear at one of their own fields, once they think enough
of you, and the world reacts: their prices drop and their rivals' rise, their
enemies' fields stop working on your aircraft, their enemies' patrols start
hunting you, and your own deliveries and kills push *their* grip on the ground
you fly over. Your hull repaints in their colours. Standing is seven opinions
that disagree — a deed for one power spills along the relations, allies taking
half the credit and enemies the whole insult — so there is no way to be
everybody's friend, and trying leaves you welcome nowhere in particular.

**Each power builds kit nobody else sells.** Seven airframes and twenty-one
modules — three apiece: one that sharpens what that power already does, one
that opens something it could not do before, and one that says how it fights.
None of them can be rolled: no wreck, derelict or open shelf ever produces one.
The only route is a counter at that power's own field with your name in good
standing on it. They are sidegrades with teeth — the Foundry's Kiln engine is
+62% thrust and 190 kg of it, the Corsair Ripper out-shoots anything inside
three hundred metres and is useless past it, the Choir's array sees twenty-six
kilometres and costs you the slot a pressure cabin wants. The armed third is
the same bargain: the Corsairs' Swarm Rack throws thirty-six rockets and cannot
aim, and the Concord's Interdiction Rail reaches nearly five kilometres and
carries two.

**And you can see all of it.** Every settlement flies a mast: a pennant in the
holder's colours, a masthead lamp that carries further than the buildings do,
and — when the place is being taken — a second pennant underneath in the
challenger's and a strobe above. Every aircraft wears its power's livery. The
radar colours contacts by what they are *to you* rather than by what they are.
The world chart is a war map: territory washed around the fields that hold it,
contested sites ringed and pulsing, and a legend that is a scoreboard
(`game.factions`, `factions`).

It is also, now, a chart. The terrain under all of that was drawn by a tone
curve calibrated for 2600 m of relief, and no seed puts 2600 m inside the
sixty-kilometre playfield — seed 4242 tops out at 816 — so the whole of the
land came out between levels 7 and 28 of a halved green channel: a black
rectangle whose only feature was the step down to the flat navy of the sea,
with the faction wash the one thing on it anybody could see. Relief is drawn
by *shading* now — the slope between neighbouring samples, lit from the
north-west, which works whatever a seed's relief happens to be and costs no
extra terrain lookups because the row above is already in hand — over a
green-to-tan hypsometric tint, with real bathymetry under the water instead of
one flat blue. The storm belt is feathered rather than a pair of hard
horizontal lines across the whole chart. Land moves 10.2 levels a pixel
against 3.5, and the step across the belt's old edge is 0.3 against 15.6
(`ui.chart`).

### Acquire

**Contracts**: pay by scarcity, distance, and gate risk; deadlines matter.

**Markets**: move stocks drain and refill hourly, prices follow supply.

**Gates**: some sites need VTOL, storm sealing, high-altitude kit, or long
range. Modules and the plane/drone choice open the map.

**Loot**: better kit is harder to get, and "harder" means a place. One danger
field runs the map — distance from where everyone starts, pirate country, the
permanent storm belt, and how much kit the nearest site demands before it will
let you land — and that one number decides both who hunts you there and what
falls out of them. It is deliberately not two fields: the ground that breeds
pirates and the ground that drops good gear have to be the same ground, or the
ladder is decoration. On seed 4242 the home apron sits at item level 20 and the
belt at 99; ten open sites average 34 against fourteen gated ones at 48. The
kit that opens the far sites is the kit that gets you to the shelves and the
wrecks worth having.

Four things drop. **Wrecks**: anyone's hull ending scatters what it was
carrying, rolled at the level of the ground it fell on and the rank of the
pilot flying it — a pilot is as hard as the country it works, so a tier-3
outlaw out in the belt is a fight and a payday. **Shelves**: a site stocks as
well as it is hard to reach. **Contracts**: a job into somewhere gated pays in
kit as well as tokens. **Derelicts**: ten wrecks were already out there when
you signed on, seeded past the gates and nowhere else — the reason to fly
somewhere nobody sent you.

Rarity is Stock, Uprated, Marked, Prototype, One-off, and mechanically it is
only ever how many affixes an item carries; the price, the name and the paint
all follow from that. Nothing is a rule-breaker: a roll never exceeds what a
strong roll of that affix is worth, and six one-off modules at level 99 come
out at 1.38x stock thrust and a quarter less drag. Every pilot in the world
flies this gear too, and the fleet has to stay in the air.

**Scarcity**: the same lever runs both ways, and that is the whole of it. A
shop has a counter and an order book, so it is biased hard *down* the ladder —
less hard the harder the place is to reach — and a One-off is struck off its
list outright, because a one-off is by definition not a thing anybody has a
second of. A rare, high-rank pilot is the other end of the same lever. Measured
on seed 4242: Prototype or better turns up on 0.45% of rolls at the easiest
shelf, 1.57% at the hardest, and 20.2% of what an ace is carrying; One-offs
appear zero times in eight thousand shelf rolls against 4.8% off an ace. Rank
is a tail rather than a gradient — each step needs the country *and* the draw,
so aces are 2.0% of pilots out of the quietest site and 8.5% out of the worst,
and they say so on the radio. The best gear in the game is taken, not bought.

**Surplus**: a hold that only ever fills up turns loot into bookkeeping, so
there are three things to do with the fourth engine you will never fit, all at
a service site and all costing something that is not only money. Break it for
spare parts, which is the resource repairs and new hulls already eat, so
surplus kit becomes airframe life. Pay tokens and parts to redraw its rolls at
its own tier and level, for when the tier is right and the numbers are not. Or
eat three loose items of the same slot to push one a tier — the only thing that
makes a shelf of near-misses worth keeping. Uprating stops one tier below the
top: a One-off is not something a workshop makes.

A build is meant to be legible from outside and from the cockpit. The fitted
geometry takes the paint of the best thing on it — grey factory issue through
anodised to a one-off's polished finish — and the profile page carries a
handling card: climb, stall, never-exceed, endurance, hold and guns, read off
the effective airframe the simulation is actually flying, so it cannot describe
an aeroplane that does not exist.

### Recover

**On foot**: crash sites, disembarking and boarding the rail drop you into a
deterministic first-person walking mode; held jumps bunny-hop, preserving
horizontal speed.

**Going down**: a hull that stops being an aeroplane does it where you can see
it. The fireball blooms in a fifth of a second and burns out inside one, sparks
outlive it on ballistic arcs, and the smoke goes on rising and drifting for five
seconds after there is nothing left alight. The fitted modules are thrown clear
rather than placed — they arc, bounce and stop, and where they stop is where you
walk to collect them. Everyone's hull ends this way, not only yours, so the
attrition the market feels is attrition you can watch happen. The camera stays
with the wreck for a couple of seconds before the operator is walked home; the
airframe, the cargo and the modules are already gone by then, so the delay costs
nothing but gives you the one thing the game used to cut away from.

**Crash recovery**: a crash always destroys the airframe. Whether anything comes
back off it is a question about where it went down, not about how hard it hit —
on dry ground within a walk of a service site the fitted modules scatter across
the terrain and can be collected back into inventory; in the sea, or out past
the last pad, the wreck and everything bolted to it is written off where it
fell. The straight line is often the one that cannot be recovered from.

**Signing on again**: ruin ends a run, never the world. Markets, prices, the
other pilots, the clock and the map are all exactly where they were, including
the modules the last operator scattered across the landscape — those are still
lying there for the next one to walk out and find. Knowledge and standing carry
over; capital does not. The world could not reset in any case: it is meant to
hold more than one person, and a second player is not going to disappear
because the first one lost an aeroplane.

**Other pilots**: the sky is not empty and not staged. Twenty-odd aircraft work
the map — traders buying where a good is cheap and selling where it is dear,
couriers hauling what a site is short of, patrols flying a circuit, outlaws
hunting loaded traders, prospectors out over the unvisited. They fly out of the
sites, park on the aprons between legs, refuel and repair, and their trades move
the same market stock the player buys and sells into: a delivery three hundred
kilometres away shifts the price you are quoted when you land.

They are not spawned around the camera. Everything within seven kilometres flies
the full six-degree-of-freedom simulation; beyond that the same pilots advance
along their leg at cruise, running the same task machine and the same economy.
A trader that took off an hour ago is somewhere between where it left and where
it was going, whether or not anyone watched it leave.

They fly on energy, not on altitude. The throttle is set by how much total
energy the aeroplane wants and the elevator by how it should be divided between
height and speed, which is the difference between an aircraft that climbs and
one that hangs on the pitch stop bleeding airspeed until it mushes into a hill.
It also means the commonest way to destroy an aeroplane — sitting a few knots
over never-exceed for two minutes on a long descent and grinding the airframe to
nothing — cannot happen, because being fast is itself a demand to trade the
speed for height. Over three hours of world the fleet went from losing 42
aircraft an hour to 14, and from spending 7.4% of its airborne life past
never-exceed to 0.2%.

They also read each other. An outlaw picks a mark by working out who would win —
time to kill each way, counting armour and structure on both sides — and leaves
alone anything that beats it, or anything at all when it is shot up or low on
fuel. A patrol makes no such calculation, because a police force that only takes
winnable fights is not one; what it does instead is answer calls, weighting a
distress signal well above mere proximity, so patrols converge on trouble rather
than merely patrolling near it. Being shot at interrupts anybody's errand: those
who can shoot back turn and fight while there is enough airframe left to fight
in, and everyone else runs — and then, once nobody is shooting, goes back to
work.

The architecture is the point as much as the behaviour. A pilot never touches
the world: `fly_pilot_think` is handed a read-only view and answers with a
command, and `fly_pilot_apply` is the only thing that changes anything. Every
pilot decides before any pilot moves, so the world does not depend on the order
they happen to sit in an array — and a command stream arriving from outside,
which necessarily lands before a tick rather than in the middle of one,
produces the same world as the AI that generated it. Every command is journalled
with the actor that issued it, so a network client can stand exactly where the
AI stands. The `game.pilots` aspect proves it rather than asserting it — one run
records its command stream, a second replays that stream with the AI switched
off, and the two worlds have to finish identical.

**Rail**: a one-way pipe on piers links two sites, laid out the way a line would
be — routed around water rather than over it, held to a minimum curve radius so
it has no corners, and arriving down a clearing kept open through each
settlement. Board it anywhere along its route and ride a visible pod to the
destination.

The deck holds a grade rather than following the ground, and the piers are what
a line on piers has for exactly that. It used to sit at a fixed clearance over
whatever was under it — `ground + 6.5 m` at every station — which drapes the
line over every hummock between two settlements and makes the horizon bob from
the rider's seat. On the taut profile the mean gradient is 1.5% against 3.1%,
the worst is 5.4% against 11.0%, and the sharpest change of gradient anywhere on
the line is 3.2% against 10.7%; the piers run about eleven metres and reach
eighteen, which is what holding a grade across broken country costs.

The bents stand thirty metres apart, and that is a separate number from the
survey's. The survey samples the *plan* every hundred and twenty metres, which
is the resolution a curve needs; putting a pier only at each of those stations
left a hundred and twenty metres of thirteen-metre-high pipe supported by
nothing, and from the ground the eye reads the span before it reads anything
else. Each one stands on a pad footing rather than being pushed into the turf.

**Roads**: the rest of the map is joined by road. The network is a Kruskal
forest over the settlements with a degree cap and a second pass that closes
short loops, so it is a road system rather than a minimum spanning tree —
freight that can only ever reach a place one way makes every road the same
road. Each link is surveyed by the same routine that lays the rail: A\* across
the land with water priced high and *climb* priced too, then simplified,
splined and relaxed until nothing on it turns more sharply than a kilometre and
a quarter of radius. Links nobody would build are simply absent — across a
strait, over a mountain further than anyone would drive, out to a skyport —
so an island settlement has a pad and no road, which is what aircraft are for.

The loop pass walks the same list of pairs the tree pass did, and until it was
given one flag to remember what that pass laid, three of the twenty-eight roads
on seed 4242 were a second carriageway laid exactly where the first one is —
each also spending a slot of both ends' degree cap and a share of the cut
budget. The drawing used to hide them, which is the half of the problem you can
see; the network now does not have them. Twenty-seven distinct links reaching
the same 21 of 24 settlements, and `game.convoy` asks the question of the
network rather than of the drawing, because a duplicate that exists has already
been paid for by everything downstream of it. It moves the heightfield — fewer
roads is a different set of stations bidding for the cut budget — which is why
it landed together with the approach work below.

What is laid is then built. The deck is level across its own width and stands
on the highest ground under it, so on a slope the uphill edge meets the hillside
and the downhill edge stands off it on a battered bank: a shelf cut into the
mountain rather than a ribbon lying along the fall line. The profile is the same
taut line the rail's deck runs on — `fly_rail_grade`, the smoothest line that
stays over the shelf and inside the fill it is allowed — so a road holds a long
even gradient, touches down on the ridges and crosses the hollows on made
ground.

That solver used to be half a solver here, and it is why the roads were too
tall. It raised a station toward the mean of its neighbours and never lowered
one, which is a ratchet: over rolling country every station climbs until it
meets the fill cap, and a link crosses the map on a twelve-metre bank the whole
way. Two-sided it converges on the line that spends the least earth — mean fill
4.4 m against 7.2, and the tallest bank anywhere in a seed-4242 network 9.6 m
against 15.3. Both profiles also carry a vertical curve, taken from the
curvature-minimising stencil over five stations rather than from a second taut
line at a longer baseline: mixing two taut lines has a fixed point that zig-zags
between them, and measured it made the worst grade break on a road *worse*
(15.5% to 18.3%) while claiming to be a vertical curve.

**Earthworks**: a road that may only build up is half a road. Surveyors cut as
well as fill, so the profile is solved twice — once with a free hand, allowed to
drop a cutting's depth below the shelf, and once for real. The difference is how
deep a cutting each station wants, and the ones that are granted come back to
the world as `fly_cut`: a box in plan, aligned to the line, with the formation
ramped along it and the sides battered back into the hillside. `fly_world_ground`
takes the lowest of the terrain and any cutting over it, after the landing
aprons and never above them — a cut takes material away and that is all it can
do — and `fly_glsl.h` carries the same loop line for line, because the GPU lifts
its terrain vertices in GLSL and half a heightfield on one backend is two
different planets. `gpu.ground` holds the two together on an apron and on an
earthwork both.

There are far more wanted cuttings than the heightfield can carry, so they are
ranked and the budget is spent on the best two dozen. Ranked by the *gradient*
they remove, not by how deep they are: the deepest cuts are in the flanks of
long climbs where the road was already comfortable, and the first version of
this spent the whole budget there and moved the steepest grade in the network by
nothing at all. What a cutting is for is the crest. Boxes are short for the same
kind of reason — a cut's formation ramps linearly and the profile it follows is
a curve, so a box long enough to span a whole ridge crossing sits above the
profile in the middle and grades nothing useful.

The last piece is who gets re-solved. Roads cross, and several of them run
together down the same valley near a settlement; an earthwork dug for one takes
the hillside out from under everything else standing on it. Re-solving only the
road that asked left another one on an 11.7 m bank in the middle of a cutting
somebody else had dug through it, so the test is the footprint rather than the
requester — every road with a station inside a published box is surveyed again
against the ground as it now is.

What it buys, on a seed-4242 network: the mean change of gradient between
stations falls from 1.03% to 0.92% and the mean fill from 4.4 m to 4.3 m, on
mean grade 2.6% against a bare shelf's 3.5%. What it does not buy is the worst
gradient, which stays at 18.8% — and that one is not an earthworks problem. It
is road 9's approach to a settlement sitting at 627 m above country at 550, four
stations of 13% average climb into a pinned terminus, and the profile with a
free hand everywhere and any depth of cut you like still holds 19% there. A
kilometre-and-a-quarter minimum radius forbids the switchback that would fix it,
which makes it a siting fact rather than a grading one.

The carriageway itself is two lanes wide with a centre line, it
has thickness at its edge, and the woodland is cleared out of its path. Nothing
a settlement builds stands in its right of way, so it arrives through an opening
kept for it exactly as the rail's pipe does, and the settlement lays its avenues
out on the bearing it comes in on.

**And it arrives at something.** The survey stops a road eighty-five metres
short of the pad so the last stretch belongs to the settlement rather than to
the network — and for a long time the settlement built nothing there, so every
road in the world ran out of deck in open grass with a sheared face across it.
The deck now ramps down to grade past its terminus, and the settlement meets it:
a checkpoint on the boundary with a cabin, kerbs and a barrier arm; a marked
parking lot beside it, inside the right of way where nothing else may build; and
the run-in itself, surfaced like the road and carried onto the pad's own apron.
Lighting goes with it, which is the whole of where lamps belong — a column beside
a way it lights, never a pole in a field.

**Convoys**: freight runs on those roads. A column takes what one market has a
surplus of to a market that is short of it and puts it down there, so a column
that never arrives is stock that never arrives. There are four tiers, and the
rung comes off how dangerous the country the road crosses is — the same field
the whole loot economy hangs off, so the columns worth robbing and the columns
worth escorting are the same columns. Bigger is longer, tougher, better armed,
slower and worth more, in that order and without exception: a Dray is a pair of
flatbeds and a rifle, a Leviathan train is six vehicles behind forty per cent
armour with real flak on the lead truck and five rolls at a bias nothing below
it reaches.

Anyone can attack one. Outlaws pick columns as marks when the sky is empty and
press home a shallow diving pass; anybody at war with the power whose flag is on
the freight will do the same; and the player needs no permission at all. What
answers is flak, which is deliberately nothing like an aircraft's guns: it needs
no firing solution, it tracks, and it falls off with range and with how fast you
are crossing — so a strafing run is survivable and a loiter over the column is
not. The column shortens as it burns, its remaining guns thin with it, and what
is left when it stops is on the ground to be walked out to. Burning somebody's
freight is an act of foreign policy, and the ledger says so in every direction:
the power that owned it remembers, and if you are wearing colours at war with
them, so does yours.

### Progress

**Space**: the ground is a ball — 600 km of it — and above about twenty
kilometres that stops being a detail. Gravity is central and falls off with the
square of the distance from the centre; a craft going fast enough sideways
stops coming down; and the air runs out at 140 km. Those are the same fact
written three ways, and orbits are Keplerian because the transverse equation
conserves `r * v_h` as well as the radial one balancing weight against `v^2/r`.
Apoapsis, periapsis, escape speed and the period are the real numbers, not
something that looks orbital (`sim.orbit`).

The playfield is drawn on a *tangent chart* over that ball rather than
re-projected onto it. A sixty-kilometre chart on a six-hundred-kilometre sphere
drops 750 m at its far corner under thirty kilometres of haze, so bending the
heightfield would cost the whole render suite and buy nothing anyone inside the
atmosphere could see. Every place the curvature *is* observable — the weight you
fly against, the speed you need to stay up, the air you have to climb out of —
it is real.

The chart is *azimuthal equidistant*, and what it maps is the whole planet. A
place at chart radius `d` and bearing `theta` is the place `d` of great circle
from home along that bearing, so radial distance is exact everywhere and the
only distortion is tangential, at `sin(d/R)/(d/R)` — 0.042% across the sixty
kilometres the game is played in. Every direction loops at 3770 km rather than
only east, and the one singularity is the antipode at 1885 km rather than two
poles at half that. Terrain is sampled through the chart onto the ball and read
as 3D noise in the volume the direction points into, so a chart that comes back
on itself lands on the same noise and the world has no seam in it by
construction rather than by patching one. Where on the ball you live is chosen
at world-gen — as a rotation of the sphere, which is the only operation that
leaves the terrain a function of the point it is at — and it is searched for a
temperate, watered coast with low ground to build on and high ground behind it,
so every seed is habitable rather than four in five of them.

**And it is a planet, not a heightfield.** Under the 9 km relief is a
continental field five octaves deep, from a quarter of the planet down to 16
km, and it decides one thing: sea floor or continent. About seventy per cent of
the surface is under water — Earth is seventy-one — in basins three kilometres
deep, and the spectrum runs unbroken from 250 km to 46 m, which is what makes a
coastline a coastline rather than a smooth contour with a wobble on it. Ranges
are built where ranges are built, at the seaward edge of a continent and where
two of them meet, so they run in chains with plains between them instead of
being scattered evenly over the land. Climate is two more fields on the same
ball: aridity, which puts deserts under the descending air at 25 degrees and in
continental interiors, and a snow line that falls with latitude until it passes
the waterline and closes a cap over the pole. Both are read at world-gen — home
is somewhere green — and both are what the limb is shaded from, so the planet
you see from orbit is the planet you take off from (`world.planet`).

The instruments read that world and not the chart. A chart position is its own
type with its own member names, so the compiler refuses to subtract two of them;
distance and bearing go through one pair of functions that measure on the
sphere, and an aircraft that flies far enough folds back into the disc. In the
settled world all of that agrees with plane geometry to 0.03% and is invisible.
Fifteen hundred kilometres out it is the difference between 524 km and 2121
(`world.chart`, `world.seam`).

**Getting there** is the end of the ladder, and it is gated by physics rather
than permission. Every engine in the game is an air breather whose thrust goes
as air density, and lift goes as density too, so an aeroplane with unlimited
fuel and its nose up tops out around ten kilometres and stays there. A rocket
motor carries its own oxidiser and does not care. Five parts across five slots
— motor, propellant tank, RCS thrusters, ablative shield, pressure cabin — and
wanting all five at once costs you the gun, the hold, the armour and the
avionics, so a spacecraft is a different build of the same airframe rather than
an aeroplane with something bolted on. The stack is also a spaceframe
conversion: faired, sealed and stressed for it, because a winged hauler with a
rocket strapped to it tears itself apart at max-Q, which is what the first run
of `sim.ascent` did.

The margins are deliberately thin. A converted heavy hauler carries about
3500 m/s of ideal delta-v against the 2250 m/s a hundred-kilometre orbit wants,
and the rest goes on gravity and drag losses — enough that a decent gravity
turn reaches orbit and a bad one does not. Coming back is its own problem:
convective load goes as `rho * V^3`, so an unshielded re-entry lasts about a
second and a shielded one wants a shallow approach.

**The stack is scarce, not merely expensive.** Price is not a gate — tokens
accumulate, so a part on the shelf at the field you start from is a part you
will own. Each of the five carries a floor on the item level of the ground or
shelf it will turn up on, so they appear on the hard half of the map: about a
third of the world's cities and skyports stock them, never the one an operator
starts at, and the wrecks that shed them are in the country that breeds them.
The same three tiers of roll apply to them as to everything else, and they roll
the things a rocket is for — vacuum thrust, propellant, cold-gas authority —
because a Prototype whose tier bought nothing is a badge (`game.space`).

**A build is visible from outside it.** All five change the hull as well as the
sim: the bell hangs out past the tail, the propellant tank fills the belly
where the hold was, the aeroshell is the underside and the cabin is sealed over
the glasshouse. A spaceframe does not look like an aeroplane carrying luggage
(`render.spaceframe`).

**Saucers** live up there and nothing else does. One in the world, above a line
almost nobody can cross, weightless rather than orbiting — they hold station
instead of falling or circling, so they are still where you left them. They are
drawn as what they are rather than as an aircraft with a strange loadout: a
lens with a dome and a rim of lamps running round it, twenty-two metres across,
built out of no physics the rest of the game uses. They are the only source in
the game of drops at that weight, and what comes off one falls to the ground
underneath, so a kill in orbit is an expedition that ends with a walk
(`game.saucer`, `render.spaceframe`).

**Weather stops where the air does.** The weather field is a map, and it used
to be read without its altitude, so a ship in orbit sat in whatever storm was
over the ground beneath it — rain drawn on the canopy at ninety-six kilometres,
and a buffeting force with no density term shaking a spacecraft as hard as an
aeroplane in a squall. There is a layer now: flat to 6 km, gone by 14, which is
the top of the convective layer and above everything the game is flown at
(`sim.weather`).

**Save/load**: Save/load is plain YAML; game state mutates only through serializable
commands, so a server/client split later is a transport problem, not a
rewrite.

### Flying it

The aeroplane is a machine rather than a cursor, and most of what that means
lives in [Flight model](#flight-model). Four things a pilot has to know:

**Trim, or hold the stick all day.** A stable aeroplane has exactly one
attitude it will keep on its own, and the trim wheel (`z`/`x`) is how you
choose which. Fully back trims to just above the stall, fully forward to a
fast descent; the index beside the reticle shows where the wheel is. Cruise is
something you set up and then leave alone.

**Feet.** Rolling into a turn swings the nose the wrong way first (adverse
yaw), and the propeller pulls it left at any power that is not cruise — full
power on a takeoff roll most of all. The ball under the reticle is centred
when the aeroplane is going where it is pointed; a skid costs speed, and on
final it drops a wing.

**The angle, not the speed.** A wing lets go at an angle of attack, and how
fast that is depends on weight, load factor and altitude — so the airspeed
tape is not the stall warning. The `A` scale to the left of the reticle is:
the top of it is the break, the amber last fifth is the buffet, and the
airframe starts shaking before it goes. If the wing does go and the aeroplane
is yawing, it autorotates and the banner reads SPIN — unload it, opposite
rudder, then roll level.

**Weight and the ground.** Fuel and freight are mass: a full aeroplane rolls
lazily, stalls faster and needs more runway, and the same aeroplane four hours
later does not. Inside a wingspan of the ground the induced drag falls away
and a fast approach floats the length of the apron; on a windy day the last
fifty metres of the descent take you out of the wind that was holding you up.
The wheels do not go sideways, so an arrival with drift on it costs structure.

**And when the warning sounds.** `c` releases what is on the rails and `v`
sheds a decoy. The diamond around the reticle closes onto the sight when the
launcher has a solution and stays open when it does not, so a key that seems
to do nothing is an envelope rather than a fault. Inbound, the chevron on the
ring is the bearing it is coming from and the number is seconds: put that
bearing on your wingtip, get the nose down toward whatever high ground is
about, come off the power, and shed a flare at about three seconds. Doing one
of those is usually not enough — a flare dropped by an aeroplane that then
flies straight on is a flare directly astern of it, and the round that takes
the bait arrives up the tail it was going to hit anyway.

---

## Flight and simulation

### Flight model

`fly_sim` is a pure, deterministic, decoupled flight-dynamics engine: it reads
inputs and environment callbacks, mutates only the craft passed to it, keeps no
globals, and touches no I/O. The same inputs always produce the same
trajectory, which is what makes headless testing, replay and a later
server/client split cheap — and what lets `sim.orbit` measure a Kepler orbit to
one part in ten thousand.

The design goal is that flying well is a skill rather than a setting. Every
term below exists because without it some part of being a pilot was free.

**Mass is everything aboard.** Airframe, freight, fuel and rocket propellant.
Fuel used not to count, so a departure with full tanks and an arrival on fumes
flew the same aeroplane; a Condor's tanks are a seventh of its empty weight.
All-up mass drives the stall speed, the climb, the runway needed, and — through
the moments of inertia — how quickly the thing answers the stick.

**Control power goes as dynamic pressure**, not as speed, so authority collapses
slow and at altitude and stiffens fast. **Control accelerations are divided by
the moments of inertia**, referenced to the empty airframe, so what you load
and what you bolt on are felt through the stick. Note that this changes the
roll *acceleration* and not the steady roll rate, which is the control moment
over the damping moment and genuinely has no mass in it.

**The wing lets go at an angle, not at a speed.** The lift curve breaks at
`alpha_stall` with 15% hysteresis, so recovering means unloading rather than
easing off the stop. Buffet runs up from four fifths of the way to the break —
through the instrument, and through the airframe as a rate disturbance — so a
stall is always announced. Past the break a wing that is already rolling stalls
harder on the down-going side, which is autorotation: one term on top of the
ordinary damping, so a spin is not a mode and the recovery is the real one.

**The propeller is attached to the aeroplane.** Torque, p-factor and slipstream
as one coupling, rigged out at cruise power the way a real aeroplane is rigged,
biting at full power and reversing at idle. Thrust also falls away with
airspeed, which is what gives an airframe a level top speed a little under Vne
and makes the red line something you dive to.

**A turn is coordinated or it is not.** Adverse yaw from the ailerons, a side
force on the fuselage, and drag that goes as the square of the sideslip.

**Ground effect** inside a wingspan (Wieselsberger), so a fast approach floats.
**Wind shear**: the free-stream wind decays through the boundary layer, so the
last fifty metres of a descent into wind take the headwind away. **Gusts** are
an Ornstein-Uhlenbeck process on the *wind* rather than a force on the hull —
correlated over a couple of seconds, so weather arrives as air rather than as
grit, and turbulence in a vacuum is impossible by construction rather than by
promise.

**The undercarriage is forces, not a rule.** Tyres that will not go sideways,
a nosewheel that steers only while it is rolling, gear that holds the wings
level as a spring so a wing can be held down into a crosswind, weight-on-wheels
that bleeds off as the wing takes the load, and a bounce. An arrival with drift
on it costs structure.

**Two structural limits.** Vne against equivalent airspeed, and a load factor
the spar is stressed for — because a wing at speed can pull several times what
the airframe can take, which is the whole reason a fast aeroplane is a
dangerous one.

#### Airframes and modules handle differently

Seven numbers on `fly_airframe` — `stability`, `damping`, `inertia`, `spool`,
`torque`, `prop_v`, `g_limit` — say what an aeroplane is *like* as opposed to
what it can do, and every airframe in `data/airframes/` sets them. A Vulture is
0.75 stability, 0.70 inertia and 5.8 g; a Slagback is 1.25, 1.70 and 3.4. They
have never been comparable on a spec sheet and now they are not comparable in
the air either.

A fitted module reaches the same block: `pitch_mul`, `roll_mul`, `yaw_mul`,
`wing_area_mul`, `wing_span_mul`, `cl_max_add`, `alpha_stall_add`,
`stability_mul`, `damping_mul`, `inertia_mul`, `spool_mul`, `torque_mul`,
`g_limit_add`. The wing modules multiply the actual wing, so one number moves
lift, induced drag, stall speed, ground effect and roll inertia together. A gun
pod is 12% off the roll rate; Extended Wings buy a lower stall and cost a third
of the roll; the Bloom Wing is the best climb in the game on an airframe that
will not turn; a Ghost Frame is fast, twitchy and bends a g and a bit earlier
than anything else. Two rarity affixes — roll rate and stall margin — land on
the same fields, so a good item is felt through the stick.

`sim.handling` gates all of it as comparisons rather than as constants: that a
long wing rolls slower than a stripped frame, that a full hold is felt, that
the cushion is real, that the propeller needs rudder, that the wing warns
before it goes and can be flown out afterwards, that trim chooses a speed, and
that gusts are correlated.

### Simulation depth

- **The flight model itself: closed.** `fly_sim` now carries all-up mass
  (fuel and freight included), moments of inertia referenced to the empty
  airframe, control power against dynamic pressure, a stall with hysteresis
  and a buffet that arrives before it, autorotation, propeller torque rigged
  out at cruise, propeller thrust that falls away with airspeed, adverse yaw,
  a fuselage side force, ground effect, wind shear through the boundary layer,
  correlated gusts on the wind rather than forces on the hull, an
  undercarriage made of tyre forces, a pitch trim wheel worth a quarter of the
  elevator, and a structural load-factor limit alongside Vne. Every airframe in
  `data/airframes/` sets the seven-number handling block, and every module in
  the registry sets the thirteen-field one, so a fit changes the machine and
  not only the spec sheet. Gated by `sim.handling` as comparisons rather than
  as constants — see [Flight model](#flight-model) above.

- **Where the model is still deliberately quiet.** Three things were
  considered and left out. There is no *lateral* trim, so the propeller's
  residual is something you hold rather than something you wind off — a second
  wheel for a quarter of a degree of rudder is an instrument nobody would
  touch. The spiral mode is left mildly divergent, which is true of every light
  aeroplane ever built and is the reason hands-off flight is a rest rather than
  a destination. And the elevator remains powerful relative to the pitch
  stability — a third of its travel, *held*, commands an angle of attack past
  the stall. That is now a thing the buffet warns about rather than a thing
  that cannot happen, and rebalancing the ratio would move every gate
  downstream of it for a feel change the trim wheel already delivers.

### Survival and the fail state

Ruin, conditional salvage, liquidation and the airframe-life ratchet are in.
Two rules bind everything added here, and both are worth restating because they
are easy to violate by accident:

- **Nothing that implies the pilot is a body.** If the airframe consumes it —
  fuel, engine hours, structural fatigue, ammunition, parts — it is fair game.
  If a body consumes it — food, water, sleep, warmth, health — it is out. A
  rations subsystem was built once and removed for exactly this reason.
- **The ending terminates an actor, never the world.** A save-wiping death is
  incompatible with the multiplayer direction: the world cannot reset because
  one operator went down. Once the player is a participant like everyone else
  (see Multiplayer, below), `fly_game_restart` should collapse into the same
  code path a pilot's death already takes.

Two things are deliberately not on the TODO list. Random engine failures produce the
same attrition with none of the ownership — the pressure has to come from
decisions taken minutes earlier, or it reads as the game cheating. And a ruin
check that cannot see a recoverable wreck would end runs that were winnable,
which is worse than having no fail state at all.

---

## Pilots and the fleet

- **The speed law now defends never-exceed itself, and it defends the approach
  to it rather than the line.** The total-energy law below answers an overspeed
  — a negative acceleration demand subtracts from the sum and adds to the split,
  so the throttle comes back and the nose comes up — but it answers it as one
  term among several and with a bounded voice: the acceleration demand is
  clamped at 3.5 m/s^2 and the pitch it produces at six tenths, and a height
  demand of twelve metres a second outvotes both. `keep_it_flying` had the last
  word on the *bottom* of the envelope and nothing at all to say about the top.
  What was actually holding the fleet inside it was the cruise demand sitting
  low, and part of how low it sat was `steer_to`'s turbulence term.

  So the guard reads the trend, not the needle. A descent at flight-path angle
  gamma is worth `g*sin(gamma)` of acceleration whatever else the aeroplane is
  doing, both terms are already on the velocity, and four seconds of it is about
  the time an answer takes to work. Past that, the throttle first and hard —
  thrust costs nothing to give up — then the nose, gently and upward, ahead of
  the load limiter so what comes out is the hardest recovery the airframe can
  make rather than the one that breaks it. A level guard cannot do this job: at
  0.94 of vne in a forty-degree dive there are under two seconds left and the
  elevator has a limiter in front of it.

  128 seeds, twenty minutes each, every pilot flown in full — the same
  all-detail window `game.pilots` runs on one seed, widened until the measure
  stops being a lottery:

  | | hp/airborne hour | split halves | hauls | written off |
  |---|---|---|---|---|
  | before | 3.64 | 4.24 / 3.03 | 2489 | 821 |
  | the throttle gate alone | 3.57 | 4.43 / 2.71 | 2473 | 821 |
  | the trend guard alone | 2.58 | 2.54 / 2.62 | 2577 | 815 |
  | both | 2.57 | 2.53 / 2.62 | 2574 | 809 |

  A 29% cut in the structure the fleet destroys by flying too fast, and the
  fleet gets *more* done rather than less: 2574 deliveries against 2489. The
  split halves are the more interesting column. The old law's bill is carried by
  rare large excursions, so two independent halves of the seed set disagree by
  40%; the new one's halves agree to 4%. What the guard removes is the tail.

  The peak moves the wrong way — worst excursion 1.45 of vne before, 1.52 after
  — and that is the same trade `game.pilots` already refuses to gate on: a brief
  overshoot at the bottom of a dive is nearly free, and a bound on the peak is a
  bound against diving.

- **The pull away from the ground stops handing full power to an aeroplane that
  already has too much of it, and on its own that is worth nothing.** The pull
  is right and it stays — a hauler mushing at fifty metres a second needs
  everything the engine has — but it opened the throttle on *any* descent inside
  its window, and 71% of the structure the fleet spent past never-exceed was
  spent in there. That looked like the whole answer and it is not: gated alone
  it moves the bill from 3.64 to 3.57, which is inside the noise. The trend
  guard above is the entirety of the gain, and by the time it has shut the
  throttle there is nothing left for this to save. It is kept because it is the
  right thing for a recovery to do and because it is what stops a *marginal*
  excursion — where the guard's cut is a few tenths — being held open by the
  pull that runs before it, and `game.decisions` gates both halves directly.

- **The wind's 3 m/s floor is a weather number again.** `fly_world_weather`'s
  floor carried a note saying it could not come down until the pilot law stopped
  leaning on turbulence for its speed discipline, on a one-seed measurement:
  three aircraft written off in `game.pilots`'s twelve-minute window against
  nine with the floor at zero, 0.04% of airborne time past never-exceed against
  0.06%, worst 1.06 against 1.43. **That measurement does not survive being
  repeated.** Over 128 seeds and twenty minutes each, the unchanged law spends
  3.64 hp an airborne hour past the line under the shipped weather and 3.37 with
  the floor at zero — smooth air is very slightly *cheaper*, not harder, and the
  original figures were one world's luck.

  What was true underneath it is the structural half, and that is what the guard
  above fixes. Deleting `steer_to`'s turbulence term outright — the direct
  experiment, rather than moving the weather — costs 0.90 hp an airborne hour
  under the old law (3.64 to 4.54) and 0.84 under the new one (2.57 to 3.41). So
  the term was never doing *more* of the work than the speed law; it was doing
  about a fifth of it, and the speed law was doing none of the rest either,
  because it had no term for the job. With the floor at zero the new law spends
  2.95 against 2.57 — below the old law at either floor. The floor is free to
  move now, as a decision about how the world should feel rather than a prop
  under `fly_pilot`; it is left where it is because nothing here is a reason to
  change the weather.

  A fifth of the remaining difference is honest physics and no guard can remove
  it: turbulence penetration means a slower cruise in rough air, a faster cruise
  has less room to vne, and that is the aeroplane being flown correctly. What
  has changed is that the line is defended by a term of its own.

- **The player flies its own air.** Turbulence, the buffet shake and the
  autopilot's incident roll came off the simulation's random stream, which every
  aircraft in the world draws from before the player does — so how hard it was
  blowing over one aeroplane was a function of how many others happened to be
  alive that second. It is the same defect `think_rng` and `convoy_rng` were
  split out for, one layer in, and it had made `game.autopilot`'s
  structure-per-leg measure a lottery rather than a reading of the approach
  profile.

  The control experiment is the entry: moving `fly_pilot`'s turbulence divisor
  from 11.0 to 11.02, 11.05 and 10.98 — a change no crew could tell apart, and
  one that cannot reach the player's autopilot at all — took that measure from
  6.3 of a hundred points a leg to 9.4, 15.1 and 17.1, dropping a landing each
  time. The bounds on it were 10 and 32. They sat below the measure's own noise
  floor and had been passing on luck. With the player on `play_rng` the same
  eight seeds return bit-identical figures under the old pilot law and the new
  one on seven of them, and differ on the eighth only where the fleet actually
  shoots the player down. The aspect's numbers are re-baselined on that stream
  and are not comparable across it; what carries the precise claim about the
  profile is the tracking error it already measures — 24 m above the pad when
  the apron first comes under it, against a bound of 40.

- **The flight law is a total-energy law now, and that was the single largest
  cause of death in the world.** Throttle commands total energy, the elevator
  commands how it is split between height and speed. What it replaced was an
  altitude error with three guards stacked on it, each added against a real
  failure, each working on the failure it was added for, and collectively
  missing this: a three-hour probe found **sixty-six of a hundred and
  twenty-seven** aircraft written off by *structure*, having averaged a hundred
  and forty seconds past never-exceed, with every airborne pilot spending 7.4%
  of its life over the line. Not dives that got away — the peak was 1.12 of
  vne. They cruised slightly too fast for minutes at a time and ground
  themselves to pieces, because the guard could take the throttle to zero and
  there its authority ended, and in a descent gravity supplies the rest.

  Measured over three hours, three seeds, every pilot flown in full:

  | | losses/h | over vne | structure write-offs |
  |---|---|---|---|
  | before | 42.3 / 49.3 / 25.3 | 7.4% / 8.2% / 1.9% | 66 / 79 / 3 |
  | after | 14.3 / 15.0 / 7.0 | 0.2% / 0.2% / 0.1% | 0 / 1 / 0 |

  `game.pilots` gates it on time over never-exceed rather than on the body
  count, because the aircraft take a minute or two to die of it: a six-minute
  window sees the cause clearly and the consequence barely. 18.3% under the old
  law against 0.00% under this one.

- **Interception geometry was tried four ways and every one was worse.** This
  entry used to name it as the remaining win — "cutting the corner toward where
  the mark is going rather than flying at it". It is not. Measured against the
  aim-error lead the code already had (engagements/kills over three hours,
  seed 4242): iterated intercept time 444/3, the same bounded to five seconds
  699/6, an exact straight-line quadratic 715/4, against **1381/11** for
  leading by how far the nose is currently off. Three of those are the textbook
  answer and the textbook answer is for a *ballistic* weapon; this gun is
  hitscan, so pure pursuit is the correct terminal law and any lead at all
  points the nose off a ten-degree cone.

  What actually binds is time in that cone. 1381 engagements at 20 Hz is
  **sixty-nine seconds** of gun solution across three hours and twenty
  aircraft — 0.05% of airborne time. Conversion is a tracking problem: holding
  the nose on a manoeuvring target, not arriving next to one. Anything that
  wants to raise it should measure cone time directly and leave the lead alone.

- **A pirate weighs the fight; a patrol does not, and that is deliberate.**
  `hunt_mark` reckons time-to-kill each way — armour, structure and dps on both
  sides — and declines anything it loses, where it used to apply a flat 0.35 to
  any mark carrying a gun of any description. It also declines when shot up,
  low on fuel or worn. `police_mark` gets none of that. Picking only winnable
  fights is an opportunist's rule and a police force that applies it is not
  one; it also measured worse everywhere — patrols hung back and outlaws lived
  longer and preyed more.

- **Patrols answer distress calls.** `police_mark` was distance and nothing
  else, so an outlaw idling past at two kilometres outranked one shooting a
  courier down at four, and the `distress` the combat step writes on every
  victim was read by nothing but the radio chatter. A call is now worth three
  times the distance term. It costs the authority aircraft and nobody else:
  over three hours patrol losses go from 5 to 11 and outlaw losses from 20 to
  30, traders lose nobody extra, and deliveries on that world go from 128 to
  198.

- **De-stampeding the traders was implemented, measured and reverted, and the
  premise was wrong.** `best_trade` is a deterministic function of the world, so
  two traders on one pad with the same airframe picked the identical run, flew
  it together and arrived together. That looked like an obvious defect and the
  reasoning attached to it — "they sell into a price the first one already
  collapsed" — turned out to be false about this economy. Pricing a run against
  the stock it expects to *find* rather than the stock there now (via a
  `fly_world_price_at` split out of `fly_world_price`) did exactly what it
  claimed: duplication fell from 1.82 loaded pilots per distinct run to 1.18,
  and the worst pile-up from 8 aircraft on one job to 3. Everything else got
  worse. Surviving traders ended three hours holding **1310 tokens each against
  5213**, on 106 deliveries against 362, and per-delivery profit was *lower*
  too (161 tk against 187). The duplicated runs are profitable; the market
  restocks faster than a fleet can glut it. A weight sweep (0.35, 0.55, 1.0) was
  monotone — every step toward less duplication cost more than it returned.

  Anything that revisits this needs to fix the economy's absorption first, and
  should not use deliveries as the measure: a redundant delivery still counts
  as one. Trader wealth is what caught this.

- **Only one of the three old envelope guards was separable at the gate.**
  Kept for the record because it is why the flight law went unfixed for so
  long: removing the never-exceed limiter took six-minute all-detail losses
  from 8 to 13, and removing the pitch cap or the terrain lookahead changed
  nothing measurable in that window. Both of those were real, and the window
  was simply too small to attribute them. A gate that measures the *cause*
  rather than the body count would have found all three, which is what the
  over-vne bound now does.

- **The pilot tick decides for everybody and then moves everybody, and it did
  not used to.** One loop did think-apply-step per pilot, so a command applied
  at slot 6 was visible to slots 7 and up when they stepped and invisible to
  slots 0 to 5: the world depended on array order. That is wrong on its own and
  it quietly falsified the multiplayer seam, because an injected command stream
  necessarily arrives *before* a tick rather than interleaved with it — so
  replaying a recording produced a different world from the one that made it.
  It went unnoticed for as long as it did because both fidelities drove
  altitude hard at a freshly computed target, which washed a one-step ordering
  difference out within a few frames. Rate-limiting the coarse climb made it
  persist and `game.pilots` caught it: 14 of 24 pilots diverged, first at a
  coarse patrol whose mark had docked one slot-index later in the live run than
  in the replay. Deciding first is also the shape a network wants.

- **Coarse pilots can now hit the ground, and almost never do.** The out-of-view
  path could not have a flying accident at all, which is not a simplification
  but a different world: every accident happened inside the seven kilometres
  around the camera. It now flies a rate-limited climb (9 m/s up, 12 down,
  where it used to gain height as fast as the geometry asked) and is written off
  if that leaves it inside a hill. It fires four times in nine hours of world
  (0, 1 and 3 across three seeds), because `cruise_alt` plans 620 m over the
  ridge ahead and a planned altitude is safe by construction. The asymmetry is
  smaller and honestly stated rather than closed: out-of-view aircraft can now
  die of terrain, they just hardly ever do.

- **Ordnance is for the aircraft being flown in full, and only those.** The
  coarse asymmetry below is the reason. A coarse hunter is handed `CRUISE*1.45`
  and turned to point exactly at its mark, which is a perfect firing position
  against a target that cannot break, cannot descend and cannot decoy; handing
  that path a missile would have multiplied an already-known problem rather than
  adding anything. Out of view the world fights with guns exactly as it always
  did, and everything in `fly_ord` happens where somebody can see it and where
  both sides can fly. Convoy launchers follow the same rule, for the same
  reason. If the coarse side is ever brought into line, this gate is the first
  thing that should come off.

- **Pure pursuit is right for the gun and wrong for everything added since.**
  The entry above about interception geometry stands, and its own reasoning
  says why it does not generalise: *"three of those are the textbook answer and
  the textbook answer is for a ballistic weapon; this gun is hitscan."* There
  are ballistic weapons now. `fly_ord` guides on proportional navigation —
  drive the line-of-sight rate to zero, recomputed every step — which is the
  textbook answer to the case the textbook was written about. Nothing about the
  gun's terminal law changed, and nothing should: the two weapons want opposite
  things and the reason is time of flight.

- **A launch has to be survivable by flying, and that had to be the whole
  design rather than a tuning pass.** Nothing in the ordnance module draws a
  random number. A seeker does not roll to see whether a flare fooled it; it
  looks at what is in front of it each step and flies at the brightest thing,
  so a decoy works or fails on where it was dropped and when. Measured on
  `ord`: a seeker fired from astern inside its envelope arrives within 12 m; the
  same shot with a flare shed at 2.6 s *and* a break turn misses by 157 m; the
  same flare shed at 9 s hits anyway, because it has burned down by the time the
  round is deciding. That last one is the mechanic and it is asserted as such.

  Two things this cost, both worth stating. Every guided kind needed its
  envelope cut to what it can actually fly — a 150 m/s seeker with a 3.2 km
  launch range, closing a tail chase at eighty against a fifteen-second life,
  never arrived and nothing on screen said why. And a flare needed to *stop*:
  coupled gently to the air it stayed in formation with the aeroplane that shed
  it and the round arrived at both, missing by seventeen metres, which is not a
  countermeasure.

- **Friendly fire is off for blasts, deliberately.** Rounds are already
  faction-filtered at the seeker so they cannot chase their own side; letting a
  blast hurt them anyway would mean a patrol bombing a column kills the patrol
  beside it, and the world would visibly eat itself over a mechanic nobody asked
  for. If this is ever revisited it should be revisited for the player alone,
  where the mistake is legible and the cost is yours.

- **Combat is far deadlier out of view than in it, and nothing here fixes
  that.** Found while measuring the above and left alone deliberately, because
  it is a rebalance rather than a bug fix. A coarse hunter is handed
  `CRUISE * 1.45` and turned to point exactly at its mark, so `gun_solution`
  succeeds almost immediately: over three hours the coarse world converts
  12-29% of hunts against roughly 1% in the full simulation, and one seed
  recorded 83 kills coarse against 7 flown. Whatever is done about it should be
  done to the *coarse* side — the flown number is the honest one.

- **Attrition is now mostly terrain, and mostly in a fight.** With overspeed
  gone the residue is aircraft flying into hills, and combat is where they do
  it: hunting and evading had no terrain lookahead at all until this round,
  where transit and the approach have had one for a long time. Adding it took
  terrain losses from 53/57/54 over three hours to 28/36/17. What is left is
  that a hunter follows its mark's lead point and an evader flies whichever way
  is away, and eight samples along the track is a coarse guard for either.

---

## The player's loop

- **The three legs that never reached their pad were not landing short. They
  were landing long, and then being steered away from it.**

  `game.autopilot`'s `parked` bucket held three of thirteen on seed 4242 —
  Junthan at 787 m, Cinholm at 1001, Zephgoro at 583 — and the standing reading
  was that the approach put the wheels down five hundred to a thousand metres
  out and the taxi closed most but not all of it. It is worth writing down that
  this was wrong in every particular, because it is what a distance without a
  sign will do to you. All three flew *over* their pad — 62, 98 and 279 m from
  it — at three to five metres up and forty metres a second, floated on across,
  and put the wheels down 431, 574 and 714 m **beyond** it, rolling away. Every
  one of the three then finished further from the pad than it had touched down.

  Which made it the taxi's problem, and the taxi had three separate things
  wrong with it. None was a tuning number.

  - **The aim point was chosen by the wheels, and the wheels flicker.** The
    autopilot flies at `fly_world_approach_aim`'s point in the air and at the
    pad on the ground, and the test for "on the ground" was `on_ground` — which
    a roll over natural ground drops for a couple of tenths of a second at every
    skip. So the demand alternated several times a second between the pad and a
    point out on a final the aeroplane had already flown, and the average of the
    two is neither: measured at Junthan, the taxi was steered 127 degrees off
    the pad. It reads the taxi latch now, which is what the latch is for.
  - **The ball was being flown on the ground.** `in.yaw + slip * 1.6` clamped
    to +-0.6 runs after the phase branches, and it was running for a taxi as
    well as for an aeroplane. That is both of the things the taxi's own steering
    law goes out of its way not to do: it spends half the nosewheel — the moment
    `plane_ground` makes scales with the deflection — and then feeds a quarter
    of a radian of crosswind sideslip back in *against* the turn. A taxi asking
    for full rudder to come round onto its pad was given three tenths of it, and
    took a hundred and ninety-five seconds and nine hundred metres of ground to
    turn round. Coordination is an air rule and now only applies in the air.
  - **The patience clock would not count closing as progress.** "No new closest
    approach in four minutes" reads its record from the moment the latch arms,
    which is on the landing roll — so a landing that rolls out *away* sets the
    record at the far end of its own rollout and has to claw all of it back
    before the measure will admit anything is happening. At Junthan the four
    minutes expired with the aeroplane a kilometre out, closing at nine metres
    a second, eighty seconds from the pad. The clock now pauses while the
    aeroplane is closing on the pad at better than a metre a second. It still
    catches what it was written for: going away, going nowhere, and going round
    in circles all spend half their time not closing.

  13 of 13 land now against 10, and the `parked` bucket is empty — so the gate
  is `parked == 0` rather than a fraction of the legs. The taxi also turned out
  to be *cheaper* than the alternative: structure lost to something other than
  gunfire is 6.3 of a hundred points a leg against 8.7, worst 21 against 29,
  because three aeroplanes were grinding across country for three minutes each
  on a rudder they were being given three tenths of. The aspect gained a case
  that isolates it from the flying — an aeroplane parked seven hundred metres
  past its pad, pointing away, which docks in 52 s and used to give up after 240
  with the pad 1120 m behind it.

  What this did not fix is the float itself: the hold-off still asks for seven
  tenths of a metre a second whatever is ahead of it, so the wheels still come
  down past the pad and the taxi still has to bring them back. Bounding the sink
  by the apron left was measured three ways and rejected — see *Measured dead
  ends* — and the entry is in `TODO.md`.

- **The approach was straight-in, and its terrain floor collapsed against a
  distance rather than against the apron.** Two halves of one defect, and both
  of them are only visible if you ask what a leg *cost* rather than whether it
  ended at the pad.

  The floor first. `autopilot_profile_z` scans the ground between the aeroplane
  and the aim point and holds the profile above it, and the clearance it
  insisted on ramped from sixty metres to nothing over the last two kilometres
  — which is the same instruction as "fly into whatever is there", because the
  law tracks that profile to about twenty metres and the last two kilometres
  are where the ground nearest the pad is. Five of the thirteen legs on seed
  4242 touched down between six hundred metres and six kilometres short, one of
  them arriving with 17 of its 100 points of structure, and every one was
  counted as reached. The clearance now collapses against the pad's own graded
  ring — `FLY_PAD_BLEND_R`, the hundred and fifty metres `fly_world_ground`
  has already taken down to the apron, which is the one place a floor has
  nothing to protect the aeroplane from and everything to hold it off — and
  holds `FLY_APPROACH_CLEAR` anywhere the run in stands above the pad.

  The straight line second, and it is the larger of the two. Both flight laws
  flew at the pad from wherever they happened to be, so a pad standing behind a
  rise could not be reached at all: the floor holds the aircraft high until it
  is too close to descend, and the answer is a go-around — flown again from the
  same direction, into the same rise, for as many attempts as the patience
  setting allows. Nine of them was standing in for an approach that turns.

  So a site is surveyed at world-gen, on sixteen courses, for the worst height
  the ground on that final stands above the slope an aeroplane would fly down
  it (`fly_world_approach_survey`, after the roads, because the cuttings are
  the last thing that moves the ground). Most sites are clear on all sixteen;
  on this seed Cinholm is blocked from 0 to 67 degrees by up to 146 m, Jundros
  and Mirathan on their own arcs. `fly_world_approach_aim` is the whole of the
  interface a flight law sees: it answers with the pad wherever the aeroplane
  is already on a usable course for it — which is most approaches, and is
  bit-identical to flying straight in — and with a point out on the chosen
  final where it is not, so the track bends onto the approach instead of
  arriving across it. The profile is then flown over the distance actually left
  to fly, along the leg and then down the final, rather than over a straight
  line the aeroplane is not on. The AI reads the same answer, so a pad is
  approachable to the fleet exactly when it is approachable to the player.

  Measured over the thirteen legs, loaded, with the road network deduplicated
  in the same change: structure lost to something other than gunfire is 8.7 of
  a hundred points a leg against 18.1, worst 29 against 83, and nothing is
  written off or shot down where one leg used to be. Ten landed on the pad and
  three stopped short of it on the ground, which read at the time as the taxi's
  remaining hundred metres and was not — see the entry above it.
  `game.autopilot` gained the damage
  measure, because the outcome counts cannot see any of this: an arrival is
  billed as a landing however hard it was.

  Two things this deliberately did not do. The offset is not applied inside
  eight hundred metres, where the aeroplane is committed and a gate behind it
  would be an instruction to go around; and the coarse pilots, who fly straight
  lines out of sight and cannot hit anything, were left alone.

- **The arrival was fixed and the *ground* was not.** The approach lands the
  aeroplane; what happened next was a loop. The taxi was a nine-hundred-metre
  box and the branch under it is the takeoff roll, so an aeroplane that had
  landed at its destination and been blown a little wide on the ground opened
  full power, rotated, flew a circuit, landed, drifted and did it again — 24 of
  52 legs on seed 4242. The fifteen-minute divert timer ran on proximity
  rather than on failing to get down, so it eventually read one of those
  successful landings as a failed approach and flew the aeroplane to a
  different site with the cargo aboard; and it was *reset by leaving*, which
  is what a go-around does by definition, so an aircraft that flew eight failed
  approaches had eight ninety-second clocks and the bound never bit at all.
  Four changes, and the last two are the ones with teeth:

  - the taxi steers the *track* on all the rudder there is, rather than
    pointing the nose at the pad on half of it. On the ground with any wind on
    it those are different directions, and an aeroplane that points at the pad
    and travels somewhere else does not arrive;
  - it governs against *airspeed* rather than ground speed. Six metres a second
    over the ground into a twenty-metre-a-second headwind is twenty-six over
    the wing and a Skylark stalls at twenty-five: the governor was commanding a
    takeoff;
  - **arriving is latched.** Once the aeroplane has settled on the ground at
    the destination — under two thirds of the stall, so a skip during a
    go-around is not an arrival — full power and a rotation stop being answers
    the autopilot has, and the latch survives the wheels leaving the ground;
  - **the divert clock runs from the first time it got there.** Fifteen
    minutes since arriving overhead is what "nine attempts" always meant, and
    unlike continuous loiter it cannot be reset by going around.

  A taxi that has not got closer in four minutes, or that is costing
  structure, ends in a stop and a log line saying how far short — which is the
  outcome `game.autopilot` gained a bucket for, and is what saved the shot-up
  hull that used to grind itself apart at Cinholm. Over 52 legs: 51 end on the
  ground at the site asked for against 48, none at the wrong site against two,
  none taking off again from ground they had already landed on against 24, and
  the mean leg is 497 s against 549.

- **The autopilot could not reach a third of the map, and said nothing about
  it.** Flown to every site a starting aeroplane is allowed into, five of
  sixteen were unreachable: a clean, stable approach that arrived over the apron
  twelve metres up — the glide slope carried a constant twelve — floated across
  at forty metres a second, went around, and repeated until the tanks ran dry.
  No landing, no message, no end. Four things were wrong and all four had to go:

  - the slope aimed at the pad centre, so touchdown had to happen inside the
    eleven seconds it takes to cross a 450 m apron. It aims 250 m short now and
    the wheels roll the rest in, which is what wheels are for;
  - the gradient was steeper than the sink the aeroplane is permitted. Short
    final may not touch down faster than 3.2 m/s without `fly_sim` billing it as
    an arrival, so the sink is capped at 3.0 — and 3.0 at 0.80 of vref is one in
    thirteen, not the one in ten the profile asked for. It lagged, arrived high,
    and floated;
  - short final tracked a target computed from *distance*, so an aircraft that
    floated over the pad watched its distance start growing again and was told
    to climb back onto the approach it had just finished flying. A stable limit
    cycle, and aircraft sat in it indefinitely. It aims at the deck and never
    commands a climb now: going around is a decision, not something a
    proportional term stumbles into;
  - "taxi in" applied a quarter throttle with nothing holding the aircraft
    down, which is a takeoff roll. It accelerated back through flying speed,
    lifted off, overshot and went around. Governed to six metres a second with
    the nose held down, and stopping as soon as stopping is enough rather than
    rolling on to 0.6 of the docking reach.

  Loaded to three quarters of the hold, fifteen of sixteen now land and the
  sixteenth diverts and lands elsewhere; at full cargo, sixteen of sixteen. The
  gate flies all of them, because a single representative destination passes
  with the defect in — which is why `game.delivery` never noticed.

- **The divert is not separately gated, and cannot be.** An approach that fails
  for seven minutes inside two kilometres now picks the nearest site the airframe
  can use and goes there, rather than handing an aeroplane back to a player the
  feature assumes is not watching. After the arrival fixes there is no way left
  to provoke it through the commands a player has, so removing it changes
  nothing measurable. It stays as insurance and the entry says so.

- **Gates were only ever enforced by landing.** `check_docking` is the one thing
  that consults `fly_world_gate_check`, and it wants the wheels down first — so
  an aeroplane that cannot land at a site never reached the check that would
  have told it why. The route command refuses now, with the reason, and so does
  contract acceptance: an accepted job loads the cargo and starts the clock, so
  one that can only expire is not a stretch goal but a reputation penalty on a
  timer. The first contract offered to a new operator on seed 4242 was exactly
  that. The offer stays on the board and becomes acceptable when the hangar can
  honour it; `game.autopilot` holds both refusals.

- **Two thirds of worlds had two sites with the same name.** 16 syllables each
  way is 256 names for 24 sites and the birthday bound says 66%; measurement
  said 26 of 40, 41 colliding pairs. Nothing identifies a site by anything but
  its name — routes, contracts, chart, log — so a contract to "Junspire" when
  there are two is a job whose destination cannot be determined, and the
  autopilot flies to whichever index the offer carried. Resolved by walking the
  syllable table rather than redrawing: redrawing consumes a variable number of
  values from `rng`, and the first collision in a world would shift every
  position, economy and gate generated after it. Verified byte-identical across
  40 worlds; only the broken names moved.

- **Not a gap, recorded so it is not "fixed" later.** A drone flying a long leg
  is often destroyed on the way, and it is not the flight model: it is a pirate.
  Twenty-five minutes at sixteen metres a second across open country in an
  unarmed airframe is exactly the exposure the outlaws exist to create. The log
  says "Mirage 27 is on you!" and means it.

- **A wreck is an event now, and it used to be a transaction.** The fitted
  modules were teleported onto a random point on a circle 12 to 90 m from the
  crash and the operator was walked to the nearest city inside the same step:
  the correct *spread*, arrived at instantly, and a camera that cut away on the
  frame the aeroplane hit. Three changes, all of them small:

  - modules are thrown. They leave with 9-23 m/s outward and 7-16 up, carry a
    quarter of the airframe's own momentum so the debris trails the direction of
    travel, bounce once or twice and settle. The spread comes out where the old
    circle put it — 125 m on the reference wreck — but it takes thirteen seconds
    to get there. A piece that comes down in the sea is lost, which is the same
    rule the wreck answers to, applied where the piece actually finishes rather
    than where it started;
  - a blast record carries fire and smoke for five seconds. It draws no random
    numbers and issues no command, so every pilot's crash raises one too without
    the sky the player cannot see costing the world its determinism;
  - the relocation waits 2.6 s. The hull, the cargo and the modules are gone on
    the first step exactly as before — only the walk home is delayed.

- **The explosion is three things at three rates, and had to be.** One flash
  reads as a muzzle. A fireball that blooms in a fifth of a second and is out
  inside one; sparks that outlive it; smoke that is the last thing left. Tuning
  notes worth keeping, because each was a wrong turn first:

  - `draw_light` is the wrong primitive for fire. It is a disc of fixed screen
    radius with a hard edge — right for a nav light, and over emissive geometry
    it drew four white circles laid on the flames, or one blown-out coin when
    reduced to a single halo. Dropped entirely: the fireball's radiance is well
    above one and the post chain's bloom spreads it in the shape of the fire;
  - `blend_shaded` is the wrong primitive for smoke. It orients the normal at
    the *viewer* and lights that, which is exactly right for a propeller disc
    and gives a backlit plume 0.009 of linear grey — drawn, and invisible. The
    new `blend_radiance` takes radiance instead of albedo, so fire is emissive
    and smoke is lit from above whatever the camera is doing;
  - smoke reads by occluding, not by tinting. A dark puff at half opacity over
    bright grass is a smudge nobody notices; it needs to be grey *and* opaque,
    and to thin on a curve rather than linearly or it is gone by three seconds.

---

## Loot and the ladder

- **A module was a quantity and had to become an object.** `module_inventory`
  was a count per registry index: every Gun Pod was the same Gun Pod, an
  aircraft's slots held registry indices, and "sell a Gun Pod" was a decrement.
  Nothing an ARPG does with loot can be built on that. The hold is an array of
  instances now — base, rarity, up to four rolls, an item level and a seed, in
  sixteen bytes — and one array holds everything an operator owns, fitted or
  not, with an airframe's slots as ids into it. The name is generated from the
  instance rather than stored, so it costs nothing per item, cannot disagree
  with the item it names, and needs nothing in the save format.

  Identity is an id and never a position. A hold that identifies items by
  position hands "fit the third thing" to a player looking at a list that
  shifted under them while they read it, and the failure is silent: they fit
  something else and fly away with it.

- **The refactor was committed before anything rolled, on purpose.** Every
  pre-existing gate passing with instances in and randomness out is the proof
  that the change was behaviour-preserving. `hud.layout` earned its keep on
  that commit: the ruin reserve counted the fit twice and read a uid as a
  registry index doing it, which moved the reserve figure and put five ink rows
  in a three-row cluster.

- **One danger field, not two.** The map already had `fly_world_danger` driving
  pirate activity. The loot level is the same function, widened to include the
  permanent storm belt and how many gate bits the nearest site carries, rather
  than a second field painted alongside it. That is the whole ladder: the
  ground that breeds pirates is the ground that drops good gear, so the kit
  that opens the far sites is the kit that gets you to the shelves and the
  wrecks worth having. Seed 4242: home apron item level 20, belt 99, ten open
  sites averaging 34 against fourteen gated ones at 48.

- **A roll may sharpen what a module does and may not hand it a new job.**
  Gun, radar and weather affixes are only in the pool for bases that already
  have a gun, a radar or a rating, and thrust only for bases that already move
  the throttle — a radar that rolled +4% thrust is a radar making the engine
  stronger, which is one of those small nonsenses that quietly tells a player
  the numbers are arbitrary. `game.loot` rolls every base 800 times at maximum
  level and bias and checks that nothing lands out of pool, and that the
  effective module never grows a capability the base did not have.

- **The envelope is the flight model's, not the loot table's.** A roll never
  exceeds `ref` for its affix and `ref` is set at roughly what the registry
  modules are worth, so six one-off modules at level 99 come out at 1.37x stock
  thrust and drag down to 1/1.27 — a better aeroplane, not a different one.
  Every pilot in the world flies this gear, and `game.pilots` holds the fleet
  to staying in the air.

- **Sixty-four ground slots is a budget.** Fourteen aircraft an hour are lost
  across the map, so a world that dropped for every one of them would be a
  world where the pieces of the last hour have pushed out the pieces of this
  one. A wreck drops only when it is inside a walk of a service site *and*
  inside the detail radius — the fights you were in are what the slots are for.
  The draws happen either way, so a loss costs the same slice of the stream
  wherever it lands.

- **Shop restock cannot consume the world stream.** A shelf refilling itself is
  seeded off the quarter hour and the shelf index, not drawn from `rng`, or a
  client that never opens a shop desynchronises from one that does.

- **Scarcity is one signed lever, not two systems.** `bias` on the rarity roll
  multiplies the top of the ladder up and divides it down, per tier, so the
  three tiers a shelf almost never has are the three an ace almost always does.
  A shop is biased down by `-2.6 + 1.7 * danger` and has One-off struck off
  outright — it can get you a good part and it cannot get you a part nobody
  made two of. Seed 4242: Prototype or better on 0.45% of easy-shelf rolls,
  1.57% of hard-shelf rolls, 20.2% of what an ace carries; zero One-offs in
  eight thousand shelf rolls against 4.8% off an ace.

- **Rank had to become a tail and the gate had to be rewritten to see it.**
  The first formula was `floor(danger*3.2 + roll*0.9)`, which is a gradient:
  dangerous country was simply *made of* tier 2. Each step now needs the
  country and the draw, which drops aces to 2.0% of pilots at the quietest
  site and 8.5% at the worst — and the gate that measured it against the
  twenty-two pilots alive in a world at any moment could no longer see the
  difference, because twenty-two samples cannot answer a question about a
  distribution. It spawns three thousand at each end instead.

- **The sink is three verbs, and none of them reaches the top.** Salvage
  breaks an item into the spare parts repairs and hulls already eat; reroll
  pays tokens and parts to redraw the numbers at the same tier and level;
  uprate eats three loose items of the same slot to push one a tier. Uprating
  is capped one tier below the ceiling on purpose — a One-off is something
  somebody was carrying, and letting a workshop make one would undo the whole
  scarcity design in an afternoon. Salvage refuses when the parts would not
  fit in the hold rather than quietly leaving an aeroplane that cannot legally
  take off.

---

## The powers

Ownership, pressure, live diplomacy, the oath, faction kit and the whole visual
layer are in and gated by `game.factions`. Two things bound everything built
here and are worth restating, because both are easy to break by accident:

- **Not one random number.** Ownership at world-gen and the opening relations
  are hashes of the seed; grip, pressure and the relation matrix move on
  arithmetic and elapsed hours. That is what let factions be added to a live
  world without reseeding a single existing one — `game.factions` checks that
  two generations of a seed agree name for name — and it is the same rule
  `think_rng` exists for: a client that never opens the chart has to end up on
  the same map as one that does.
- **Only combatants are shot for their flag.** An outlaw robs anyone; everybody
  else fights armed aircraft of powers they are at war with, and outlaws.
  Without that clause a seven-power war empties the sky of the traders whose
  deliveries are the entire economy, and the attrition `game.pilots` gates goes
  with them.

- **The fleet's numbers are deliberately untouched, and should stay that way
  until somebody wants to pay for it.** The faction lean on a pilot's airframe
  is visual only. It briefly was not — Concord structure, Foundry mass, Corsair
  thrust — and the cost was not a balance problem, it was that changing any
  number the simulation reads reshuffles every gate downstream of it: one of
  `game.autopilot`'s thirteen routes was already surviving a pirate on 24 of
  100 structure, and a tenth on the fleet's stats killed it. Faction
  performance lives in the kit an operator opts into buying, where it changes
  one aeroplane instead of all of them.

Two things are deliberately not on the TODO list. **Territory as a field over the
ground** was considered and rejected: control is a property of sites, nothing
in the simulation says where a writ stops, and drawing a hard border would be
inventing a fact — the chart washes a halo round each holding instead, and says
the true, smaller thing. And **a player-founded faction** is not a small
feature wearing a small hat: it needs pilots that can be hired, an economy that
can be owned and a fail state for losing it, none of which exist.

---

## Roads, rail and convoys

The network, the shelf the deck stands on, the columns, the four tiers, the flak
and what a burnt column sheds are all in.

The lines wander now. The survey used to hand back the shortest thing the grid
search could find, which between two headlands is a ruled line, and a network of
those does not read as roads however well it is drawn. A seeded lateral swing
about the search's answer — `fly_rail_shape.meander`, applied before the
curvature bound so it comes out as arcs — takes seed 4242 from 1.00 to a mean
sinuosity of 1.087, up to 1.17 on the long links, with the worst turn per span
*down* from 9.8 to 8.5 degrees because the survey is sampled at 60 m instead of
150. Amplitude is derived from the wavelength and `FLY_ROAD_MIN_RADIUS` rather
than set independently: the two are one decision, and asking for an amplitude
without reference to the wavelength is asking for a radius by accident.

Two consequences worth knowing about. Cut boxes are shortened where the road
bends, because a `fly_cut` is a rectangle and six hundred metres of a road at
its minimum radius bows a hundred metres off its own chord; and the per-system
frame budget below predates both the road lighting and the tessellated deck, so
its `roads` row is a measurement of something smaller than what is drawn today.
Re-measuring it wants a frame where the road term is above the run-to-run noise,
which the alpine bench is not.

Three later corrections, all of them about the road as a *surface* rather than
as a plan. Columns keep to a lane: `fly_road_lane` is the one place the side of
the road traffic drives on is decided, and everything that puts a vehicle on a
road — the spawn, the step, the drawn trucks — goes through it, so a column no
longer straddles its own centreline and two meeting on the same link pass with
the width of the road between them. The drawn deck is lifted wherever the drawn
terrain would come up through it (`road_bed`): the survey clears the ground at
each station and about the centreline, and the mesh is bilinear over cells the
deck's edge sits outside of, which left about three deck samples in a thousand
under the drawn ground by up to 1.25 m. And the lighting runs the length of the
road rather than the near nine kilometres of it, thinned with distance by
`lamp_stride`, so from altitude at night the lit chain reaches as far as the
carriageway is drawn instead of stopping in open country. `game.convoy` holds
the first two — zero buried deck samples, every truck a lane to the right of its
own travel — and `render.roadlights` holds the third at ten to fifteen
kilometres out.

The eye that aspect compares its two views from stands *between* two columns
rather than at the road's midpoint. It is the darkest place on a lit road, so
what is left is the harder half of the claim; and it is the only place the
comparison means anything, because a midpoint that lands seven metres from a
lantern puts a column in the across-country frame and fills its whole near
field with the lamp's own pool — 7862 warm pixels of country, against 0 from
between the columns.

The rail is the same infrastructure and now reads like it. It was laid as the
shortest line the search could find under a 4 km curve bound, which between two
settlements is a ruler; it holds 1500 m and takes `fly_rail_shape.meander` like
a road, which puts the mean turn per span at 1.2–1.4 degrees against the 0.2 a
straight line came out at. It carries lighting on masts off the flanks of the
pipe, full length and on the same range ladder as the road's (`render.raillights`),
and the thing the player rides is a railcar rather than a box — saddle, shoes,
window band lit from inside after dark, roof cap, grab rails, running lights.

### Two ways across one country

The road survey and the rail survey are the same function over the same ground
between the same settlements, so left alone they answered the same question the
same way. What came out of that was one piece of infrastructure drawn twice: on
seed 7 a link ran within a hundred metres of the guideway for two kilometres and
crossed it three times on the way, at fourteen and sixteen degrees off the line
— which is not a crossing, it is a braid. And every crossing was a carriageway
and a pipe occupying the same ground, because nothing about the road's profile
knew the line was there: measured over five seeds, twelve crossings with as
little as 2.53 m between the tarmac and the underside of the barrel, which is a
guideway through the windscreen of anything driving under it.

**The corridor is priced, not forbidden.** `fly_rail_shape.avoid` hands the
survey the lines already on the ground and a toll for being near them: every
step of the A* inside `FLY_ROAD_KEEP` (260 m) pays `FLY_ROAD_KEEP_TOLL` (1.4)
metres of detour per metre travelled, scaled by a per-point weight on the
existing line. That prices the two halves of a shared corridor very differently
— running *alongside* pays for its whole length, crossing pays for the width of
the corridor and nothing more — so a road goes round when there is a way round
and crosses squarely when there is not. Roads already laid are handed to the
next link at half weight, because a road can be crossed at grade and a line on
piers cannot.

The guideway's weight is where a crossing chooses its place: full price wherever
the deck is too low to get under, and a little over a third where the piers are
tall enough for a lorry to pass beneath. So the surviving crossings land under
the tall spans rather than wherever the two lines happened to meet.

It is one field rather than a distance query, because two stages want the answer
and one of them wants it half a million times: the corridor is rasterised once
per line laid, at 192 cells across the search's own patch of world, and both the
A* and the wander that follows it read it in a lookup. The wander needs it as
much as the plan does — a three-hundred-metre swing laid on top of a route that
was carefully steered round the rail puts it straight back over it, so no
station may swing further into a corridor than the plan already was.

**What is left crosses properly.** Where a road does cross, `fly_road_build`
holds its profile under the barrel: `fly_rail_grade` took a ceiling as well as a
floor, the ceiling is the underside of the pipe less `FLY_ROAD_UNDER_CLEAR`
(5.2 m), and it is hung on the crossing itself rather than on proximity to the
line. That last part is not a detail — the first cut applied it wherever a road
came within 150 m of the rail, which buried the road leaving a settlement under
the guideway's own platform approach and left the drawn deck 3.6 m in the air
where `road_bed` lifted it back out. A station held down by a ceiling also
outbids every other candidate for the cut budget, because the earthwork under a
structure is not optional: a road that has to get under a guideway and did not
get its cutting is a road driving through it.

Two roads that cross meet at one height instead. The later road is pinned to the
earlier one's deck at the two stations the crossing falls between — floor and
ceiling closing on the same number — and the pins are recomputed in the
post-earthwork re-solve, in laying order, so a junction is level with where the
other carriageway actually ended up rather than with where it was when the pin
was taken.

Measured over seeds 4242, 1337, 7, 99 and 1234, before and after:

| | before | after |
| --- | --- | --- |
| road within 130 m of the guideway | 9.77 km of 1095 | 4.78 km of 1092 |
| crossings of the line | 12 | 7 |
| skewest of them | 9.4° | 40.8° |
| least headroom under the barrel | 2.53 m | 5.32 m |
| road-on-road crossings | 22 | 12 |
| worst step between two crossing decks | 6.14 m | 0.59 m |

It costs about a tenth of world generation: the five worlds above take 947 ms
to build against 851 ms, which is 189 ms a world against 170 — nearly all of it
the corridor field, rasterised once per link laid, and none of it per frame.

`world.crossing` holds all of it, and finds the crossings again itself rather
than trusting the record — the record is what the structure is drawn from, so a
crossing the network missed is a pier standing in the near-side lane with
nothing carrying the span.

**And the crossing is a structure.** The line is carried over the road on a
portal: two blade legs on spread footings out on the verges, a haunched girder
between them under the barrel, a parapet either side of the pipe, a hazard band
on the leading face, abutment walls holding the cutting back off the running
lanes, and a lantern under each end of the beam. The legs stand on the
guideway's own line, at the road's half width and a margin divided by the sine
of the crossing angle — so a square crossing gets the shortest span and a skew
one pays for its skew, which is the same arithmetic the corridor toll is trying
to keep the survey away from. The bents that would have stood on the regular
30 m spacing inside that span are not drawn, and neither are the road's own
lamp columns: the carriageway under the bridge is lit off the portal instead.

### Two structures wanting the same sixty metres of road

Where a road passes under the guideway its deck is held under the barrel by
what a loaded lorry needs, and the station that has to get down there comes off
the top of the cutting budget so the earthwork it needs is never one the budget
refused. Two things were quietly stopping that from working, and both only bite
where the road is *turning* — which is where a crossing is, because crossing
squarely is what the survey is priced to do.

**The box was refused for describing the road badly.** A cutting is an oriented
box and the survey shortens it until it follows the line; the shortest it would
go was ninety metres, which is the least an ordinary cutting is worth digging.
Over ninety metres of a turn the line stands 14.8 m off its own chord against a
limit of 9, so every box was refused and the station got no earthwork at all. A
station buying headroom is not spending the budget on gradient, and what it has
to cover is the width of the structure over it: over the crossing's own
`FLY_ROAD_UNDER_R` the same line bows 3.5 m.

**And where a crossroads and a portal wanted the same stations, the crossroads
won.** Two roads meeting are pinned to one height so the junction is a
crossroads rather than one carriageway passing through another, and the pin was
applied as an equality the ceiling never got to see. The two failures are not
the same size — a crossroads a metre out is a bump, and a carriageway inside a
guideway is not a road — so the ceiling clamps the pin.

Together they took the worst crossing on the five seeds `world.crossing` sweeps
from 4.11 m of headroom to 5.31, against the 5.20 the rule is set to buy, with
the worst step between two road decks unchanged at 0.59 m.

### Lighting a way at night

The roads and the guideway are lit end to end, and the light lands on the
ground the frame drew rather than on a shape laid over it.

**It was a shape first, and every fault it had came out of that.** The light
under a lamp was an ellipse of translucent triangles, seated at the surface's
own grade a hand's breadth above it and depth-tested like any other geometry.
A flat ellipse lies on exactly one plane; the ground under a road lamp is
three — the carriageway, the batter the column stands on, and the field beside
it — so the far half of a twenty-metre pool went into the tarmac, its outer half
floated over the verge, and what the depth test left was a bright patch with a
torn edge that stopped where the embankment started. It was mixed into the frame
rather than added to it, so two lamps overlapping came out no brighter than one
and a pool over anything already lit *darkened* it. Its falloff was a linear fan
from a peak to a rim, which is a cone, and a cone reads as an airbrushed disc.
And it needed to be told where the ground was: the road's tangent for a road
lamp, two ground samples either side of a viaduct for the spill under it, two
more either side of a truck for its headlamps — four call sites carrying a
height and a gradient that a lamp has no business knowing.

**So the light is deferred.** A lamp is queued as what it physically is — a
source at a point, throwing a distribution, reaching so far — and once the frame
has a depth buffer, every pixel inside its reach is unprojected to the surface
the frame actually drew there and lit by it. Nothing is laid on anything: the
light falls on the carriageway, the kerb, the batter, the grass, the column, the
truck going past and the girder over the top, because all of those are what the
depth buffer says is in front of the lamp. There is no edge to tear because
there is no polygon, and nothing to z-fight with because nothing is drawn. It is
*added*, which is why two lamps overlap into a brighter patch and why light over
a lit surface no longer dims it. `lamp_light` takes a position, a bearing, the
drop it is quoted at and its reach, and every caller lost its height and its
grade — `prop_lamp`, `road_lamp` and `prop_truck` are all shorter than they
were, and `road_lamp_at` no longer computes a gradient for anybody.

**The distribution is a semi-cutoff lantern's**, which is most of what makes it
read as lighting rather than as a spot. A bare point source over a plane falls
off as the cube of the cosine and lights a disc about a mounting height across;
a real lantern is built to fight exactly that, so intensity climbs off the nadir
(`FLY_LAMP_WING`) and climbs further along the way the lamp is aimed than across
it (`FLY_LAMP_ACROSS`) — which is what lays a long pool down a road instead of a
circle on it. Times cos/d^2 for the surface, and between them they give a bright
core with a skirt that fades to nothing without ever showing a rim. The surface
is taken as horizontal: a normal off the depth buffer is a screen-space
derivative and it is wrong at exactly the silhouettes a frame full of
lamp-posts is made of, and what a lamp lights is overwhelmingly ground, deck and
verge. It costs nothing and it cannot speckle. What it does cost is a vertical
face close under a lantern reading as bright as the ground beside it, which is
the one place the simplification shows.

**Cost is bounded twice**: by the lamp's own screen circle (`lamp_rect`, one
definition, shared with the pass that hands the fragment stage its quad), and
inside it by the one exact rejection a view depth allows — the distance from a
lantern to a surface is at least the difference between their view depths, so a
pixel whose depth is further than `reach` from the lamp's cannot be lit and goes
in two compares. That is every sky pixel and nearly every ground one, which is
why a camera standing among lamps pays for what its pools cover rather than for
what their circles do: on the steepest lit span of seed 4242 at 480x270, from
eleven metres over the carriageway, the whole finish pass — lamps, shafts and
the translucent queue together — is 0.9 ms of a 550 ms frame, and the most any
frame in the suite queues is 429 lamps. It bought range as well as quality: the
old pool cost its fan of triangles on the shared blend list at four kilometres
exactly as it did at twenty metres, so it was cut off at 1400 m and a lit road
ended in mid-country and carried on as a chain of specks. A deferred light at
four kilometres is a dozen pixels, so it now runs as far as the lamps do.

**And it has a GLSL twin**, because a resident GPU frame never comes down:
`FLY_LAMP_FS` reads the same view depth out of the frame's alpha, reconstructs
the same world point off the camera basis, and evaluates the same arithmetic per
fragment, drawn as one additive quad a lamp between the emissive splats and the
translucent pass.

`render.lamplight` holds it, on the steepest-graded lamp of the longest road in
the world: six samples of the drawn deck, three either side of the column, each
read at the pixel whose *depth* is the sample's rather than at the pixel it
projects to. The core of the pool is lit well clear of the frame's own unlit
ground, every one of the six carries at least a twelfth of that core — both
sides, because the uphill half passing while the downhill half is buried is the
signature of the fault and a check on one side alone would have gone straight
through it — the light falls off away from the column on both sides, and the
column is lit three metres up its own shaft, which is a vertical surface
standing *in* the light and nothing laid on the ground could reach it at any
grade. Measured 0.034 0.064 0.132 | 0.112 0.047 0.014 with the column at 0.408
against an unlit 0.014.

The skirt is measured against the pool's own core rather than against the
frame's unlit ground, and that is a correction rather than a concession. Swept
over twelve lamps on one road at one hour, the deck at the edge of a lantern's
throw reads 0.014 to 0.045 — stable, because it is what the lantern puts there
— while the unlit ground beside it reads 0.002 to 0.014, because the weather
over it is what decides how much moonlight the grass gets. A fixed multiple of
that swings sevenfold while the thing it is measuring does not, and half of
those twelve lamps fail it: the half with a clear sky, not the half with a
buried pool. It went unnoticed because the lamp the aspect picks is the
steepest one on the longest road, and on the world it was written against that
lamp happened to stand in the rain. The pool's own core is the same lamp in the
same frame under the same sky, and a pool that ended at the column would read
nothing against it. The floor is still measured, and the core is still held to
four times it — that half of the claim is about the lamp and survives.

The lamp is also chosen at least a kilometre and a half from any settlement,
for the same reason: a settlement lights its own ground for a long way past its
yard, and a lamp nine hundred metres from a city put the frame's "unlit"
reference at five times its open-country value with none of that coming from
the lamp under test.

**The emitter stopped being a sphere.** `light_splat` was two flat discs — one
of radius r at 3.2 and one of 2r at 0.35, each with a one-pixel coverage skirt —
and from anywhere near a lamp-post that is what it looked like: a hard bright
coin inside a slightly larger hard dim one with a step between them. What is
actually around a bright light is a veiling glare and it has no edge anywhere,
so it is a Cauchy squared now, 1/(1+t)^2 in t = (d/r)^2, which integrates over
the plane to exactly pi*r^2 per unit amplitude — the two lobes carry the energy
the two discs did and end in a gradient instead of a rim. The radius ceiling
went from 2.2 px to 3.2 px with it: held where it was, a glare stopped growing
at fifty metres, so the last fifty were a bead of fixed size that read as a
painted dot rather than as a light you were walking up to.

**And the bloom stopped being a grid.** The bloom is built at a quarter of the
frame and the composite read it with a nearest tap, which paints it on in
four-by-four blocks. Nobody could see that on a sun bloom filling a quarter of
the sky; around a lamp, where the halo is a dozen pixels across, it was a
staircase of squares around every light in the world — and with a wider glare
feeding it, it was the loudest thing left in a night frame. `bloom_bilinear` and
its twin `fly_bloom_lin` read it as the continuous field it stands for. The
shaft gather still taps it nearest on both paths: that is a five-sample radial
smear of an already-blurred mask over hundreds of pixels and has nothing to
gain.

The rest stands. The roads are sodium and the guideway is white, so a crossing
reads as two systems meeting rather than as one lamp catalogue used twice. The
arms crank in over the carriageway rather than down the verge. Lamps come on
raggedly — `lamp_dark` jitters each column's dusk threshold off its own
position, so a road lights up over a few minutes the way a road does, and it is
the same lamps first every dusk because the jitter is a property of the world
rather than of the frame. Wet ground takes a brighter, longer throw, off the
`e->wet` every other surface already reads. Trucks throw a patch down the road
in front of them, modelled as a lantern hung over that patch rather than as a
beam out of the cab: a dipped headlamp aims at the surface a dozen metres on and
everything the eye reads off it is *there*, so a source over the patch says the
same thing as a cone out of the lamp in one term instead of a second luminaire
model that exists to be pointed sideways.

The pitch was already even — a lamp every 62 m of carriageway and every 68 m of
guideway, by chainage rather than per station, alternating verges — and
`render.lampspacing` holds it: `fly_render_lamp_probe` is the drawing's own
placement function, so the claim is about the lighting rather than about a
second copy of its arithmetic. Nothing in a frame can show it, which is exactly
why it is worth a test: a chain placed per surveyed station is bunched where the
survey sampled finely and crawls along the verge as the camera moves, and every
still of it looks correct.

---

## One polyline, not two

Three things in this world are a chain of stations somebody surveyed and then
gets asked the same three questions about: the guideway, the roads, and now the
lanes at sea. Where is the line `d` metres along, which way does it point there,
how far off it is this point. Two of those were written twice — `fly_road_eval`
and `fly_road_project` were `fly_rail_eval` and `fly_rail_project` over a point
type with one field fewer — and the rail's *drawing* was a third copy, with the
curve arithmetic inlined at three places in `fly_render.c` for the barrel, the
piers and the lamp masts.

`fly_line` is the merge. One station type (`fly_line_point`: position, chainage,
and the bridge flag a guideway carries and a road leaves zero), one span
evaluator, one projection, and `fly_road_point` and `fly_rail_point` are names
for it. The waste the old objection named — a bridge flag on 560 road stations —
is 62 KB against three copies of the same arithmetic, and the arithmetic is what
was actually costing something: the rail's eval interpolated the *chord* while
its drawing interpolated the *curve*, so the railcar sat a sagitta inside its own
guideway on every bend, and nothing could have caught it because both were
right about their own polyline.

The one thing the merge had to keep is that the two lines do not take the same
curve, so the caller states which it has. A road is resampled to an even pitch
and takes the uniform form; a survey trimmed to a clearance circle keeps a stub
of a few metres against neighbours seventy-five long, and on spacing like that
the uniform form overshoots the terminus and doubles back through itself — see
the guideway note above, where that shipped once as a splayed fan of rings at
the one place a rider walks up to.

---

## A third of the chart is sea

Measured before anything was written: **32.6%** of the chart is water over four
seeds (22.4-40.0), and the only things ever drawn on any of it were the rail's
piers and a road's causeway. It had weather over it, waves on it and nothing in
it — a third of every flight spent over scenery.

What belongs on water is what belongs on a road: freight going somewhere, worth
escorting and worth robbing. So almost none of this is new code.

**The survey is `fly_rail_lay` read the other way round.** The search was always
a shortest path over a grid with one medium priced against another; a lane is
the same search with the sign turned over — water is the cheap ground, land is
what the line may not cross, and the standoff that keeps a road off the beach
keeps a hull off the rocks. That is one predicate (`medium_barred`) and every
rule downstream reads it: what a step costs, how far the chamfered standoff
reaches, what the smoothing may not undo, what the curvature relaxation may not
start. Land is priced at 60x rather than forbidden, so a pair of harbours with
no sea route between them gets an answer the caller can reject rather than a
silent failure — and `lane_lay` rejects it, at the stations *and* three samples
a span between them, because a hundred and thirty metres is wider than a spit
and a lane checked only where it was sampled can step over the one piece of land
in its way.

"Navigable" is asked of the bed and not of the wet field, and that distinction
is the whole of why the lanes are at sea rather than up the rivers: a channel and
a lake are wet ground standing *above* the sea, and no coaster is going up
either. Six metres under the keel, and it is the same number the survey runs
with, a berth is dredged to and the tests measure against.

**The quay is the only genuinely new question.** A road arrives at a settlement's
gate; a lane cannot, because the settlement is on dry land. So the network is
laid quay to quay: 48 rays out from each site, the first that finds the waterline
inside 5 km and a berth at the draught within 900 m of it, with the beam checked
160 m either side so a river mouth and a tidal creek — both deep, neither a
harbour — are refused. The landward end is the last *dry* sample rather than the
last one above sea level, which is not the same thing where a river runs out
across a beach, and it is held to the rule every structure here is held to:
nothing built in a carriageway.

Over six seeds that gives **44 quays and 25 lanes, and all six worlds have a
lane** — 3 to 10 harbours each, lanes from 3.4 to 30.9 km. A world whose sites
all sit inland is entitled to none, and one or two of a dozen are.

**A coaster is a convoy.** `fly_convoy` was already a transient, unsaved column
drawn from the market, tiered off `fly_world_danger`, armed, burnable and worth
loot when it burns; what it needed was a second class table and a line that is
not a road. So the line became `fly_way` — points, length, speed, the two
settlements and which medium — and the column is written once. The ladder afloat
is the road's ladder one medium over and deliberately not the road's *numbers*:
a hull carries three to four times a column, takes several times the killing,
and makes nine metres a second against a road's twenty-one, which is what turns
a strafing run into a decision rather than a reflex. The bottom rung has no guns
at all, which is the point of it — a lighter under tow is the one piece of
freight in this world that cannot answer.

Two numbers had to move for the sea to have anything on it at all. A column will
not leave with less than a quarter of its hold; a hull sails on a tenth of hers,
because a ship on a schedule sails part-loaded and because there are a handful
of lanes against two dozen roads — a lane standing down for want of a full hold
is an empty sea, not one quiet road. And the dispatcher stopped drawing a line
uniformly: the quota budgets a column per four roads and a hull per lane, and
the medium furthest below its own share gets the next column, with the attempts
alternating so a refusal falls back inside the same dispatch. Before those two,
every world came out with **zero** hulls on it: the surpluses a live market
carries are four to eight units, and a ship's hold gated them all out.

**Then the shore gets built on**, out of pieces that already existed. The quay
is a deck in bays running out to the berth, carried on `draw_pier` — the
guideway's own bent, which is exactly what carries a deck over water — with the
yard's `prop_gantry` over the berth, the yard's `prop_container` stacked beside
it, bollards, and lamps on the road lighting's own argument.

The fairway is marked: a lateral buoy every 1100 m of chainage, alternating
sides, red to port and green to starboard, occulting after dark rather than
steady, because what tells a light on the water from a light on the shore is
that it goes out. By chainage rather than per station, which is the road
lighting's rule and for the road lighting's reason — a mark placed per station
crawls as the sampling changes.

And what a hull is worth to burn is not what a column is worth. She sheds the
same wreckage; then the water takes all of it, which is the rule
`step_ground_items` has always applied to anything that comes down wet. So an
attack at sea is denial rather than salvage — the freight never arrives, the
powers notice, and there is nothing on the surface to walk out to. That is the
"salvage that sinks instead of lying there" the entry asked for, and it costs no
code at all: the sea was already doing it to everything else.

`world.sea` holds the survey (every station navigable with the keel's clearance
under it, the curve bound holding at 15 stations in 2250 against a design radius
it cannot always reach where a strait pins it, the marks alternating and standing
half a fairway off, and the whole network reproducing itself from the seed).
`game.coaster` holds the freight: hulls on the lanes at world-gen, each to
starboard of her own course by the same measurement the road's lane rule gets,
the ladder afloat, a voyage that ends with the cargo in somebody's market, and a
hull that burns and shoots back while she does.

What is not here: the offshore platform. A deck in open water reachable by
nothing but a VTOL airframe wants to be a `fly_location`, and a location grades
the heightfield around itself in both the C and the GLSL twin, answers to the
approach survey, the parish belt, the woodland clearing and the autopilot's
"every reachable site is flown loaded" — none of which is hard, all of which is
cross-cutting, and one of which (`gpu.ground`) cannot be run on a machine with
no GL driver. And landing on a *moving* deck is a new arrival case for
`game.touchdown` and is the second thing to try rather than the first.

---

## Water on the land

The planet had a sea and nothing else wet. Measured before anything was written,
over seeds 4242, 7, 91 and 1234: standing water on ground the continental field
calls continent was **0.00-0.94%** of the chart, and what little there was sat on
the coastal shelf. Every valley the orogeny cut was dry — the largest natural
feature the world did not have, and the one a pilot would navigate by.

`fly_river.c` is the survey and `fly_world_ground` is what it changes. Both
halves matter and the second is the expensive one: a river that is drawn and not
cut lies on a hillside.

### The survey is a flood, and the lake falls out of it

Water goes downhill, so the only question is where the downhill goes, and that
is a property of the heightfield rather than of anybody's preference. One pass
over a 192x192 lattice of the chart — 312 m a cell, coarse for a channel and
ample for a catchment, and it is the catchment this is looking for:

1. **Flood it from the outside in, lowest first.** Every cell comes out with the
   height water on it would have to reach before it could leave. On a hillside
   that is the ground itself; in a hollow it is the lowest lip the hollow has.
   That one number is both halves of the entry — it is what makes every descent
   terminate at the sea instead of dead-ending in a pit, *and* it is the lake,
   because a cell whose filled height stands above its ground is a cell under
   water. A lake is therefore the answer to a question the trace already had to
   ask rather than a second placement rule with a scatter of its own.
2. **Accumulate** rainfall down the drainage tree the flood built, with rainfall
   read off `fly_world_aridity` backwards, so a desert catchment carries a
   stream and a watered one carries a river.
3. **Trace** from the sources carrying the most country, by steepest descent, to
   the sea or to a lake. Smooth the lattice's staircase off, snap the line into
   the bottom of its own valley, bound how sharply it may bend, choose the
   stations off what is left, publish.

Over six seeds that is 20 rivers, three or four a world, the longest 4.9 to
11.8 km, and three to six lakes each. Every world has water on it; `world.river`
holds that, and none of the numbers below.

### Where the survey was wrong, three times

**Reading the filled surface as the water level.** It is a bilinear of 312 m
cells, and the bottom of a valley narrower than one is tens of metres below it:
the river came out on an embankment with the country falling away on both sides.
The level is read off the *ground* at the station instead, and the station is
snapped across the line into the lowest ground within about a cell first.

**Running a trace through a filled hollow.** The heightfield is pitted with
hollows a cell across holding half a metre, and stopping at the first of those
put every river's mouth four kilometres from its source in a puddle. Three
metres of standing water ends a river; anything shallower it runs through, which
is what the entry says about basins in the first place. A basin's outlet cell is
a candidate source in its own right, so the reach below a lake gets surveyed as
the river it is.

**Taking the level from the centreline.** The water field has an outer edge and
the ground beyond it is the hillside as found, so a level that stands above the
bank is a *wall of water* with the country falling away behind it. The level is
the lowest ground anywhere across the section instead — twenty-five samples out
to a fifth past the footprint, landing on the footprint's own edge exactly — and
the profile is then solved from the mouth back up: downhill by lowering only,
with each span held under the ground at seven points along it, because the
surface is a straight line between stations and the simplification puts them as
much as a kilometre apart.

### A river meanders; it does not turn corners

The first cut simplified the traced line with Douglas-Peucker, which bounds how
far a chord strays from the line it replaces. That is the wrong measure and it
says so plainly once it is written down: it constrains *offset* and what a
viewer sees is *angle*, and nothing in it stops two kilometre-long straights
meeting at fifty degrees — which is what a wandering river came out as, because
the surface, the channel and both banks are interpolated linearly between
stations and a chord is all there is between them.

Two bounds replace it and they work at different scales.

**The line is relaxed to a minimum radius** before any station is chosen, by
`fly_rail_bend` — which is `route_relax`, lifted out of fly_rail.c and made the
plan-view twin of `fly_rail_grade`. The guideway already had this exact problem
and solved it: smoothing passes and a spline get most of the way and neither
*bounds* anything, so a line that has to make a real turn keeps a real corner,
just a rounder one. Curvature is the property that matters, so it is the
property to constrain. The rail's water veto goes in as a callback and a river
passes NULL, because the rule that veto exists for is about not starting a water
crossing.

**A station goes in wherever the line has turned RIVER_TURN since the last
one**, and every RIVER_SPAN_MAX regardless. That is what spends the budget where
the river actually bends: a straight reach costs two stations whatever its
length, and a hairpin gets four. The two bounds compose — at the tightest bend
the relaxation allows, a turn of RIVER_TURN takes three hundred metres of river,
so a river that bends the whole way costs about what an even pitch would have
cost anyway.

Over six seeds the worst angle between two spans went from around fifty degrees
to **8.8-10.9**, mean **6.3-7.2**, and `world.river` holds it at fourteen. It is
not RIVER_TURN itself because the turn is accumulated at the samples and crossed
between two of them, so a station carries its threshold plus one fine step of
the curvature bound — which is what sets `RIVER_FINE` as much as the shape is.

The diffusion came down with it, and that is the other half. Smoothing the
traced line is for the 312 m staircase the eight-way descent leaves, and the
first cut ran a kilometre of it — diffusion cannot tell a staircase from a bend,
it only loses the smaller one first, so a river smoothed until it was smooth was
a canal. Four hundred metres takes the staircase off and leaves the meander, and
the curvature bound is what keeps the result from turning corners.

### A body of water has to stop somewhere, and it stops under the ground

This is the one that decides whether a river reads as a river. The water field
answers the sea's level past a channel's footprint and past a lake's disc, so
the level cannot be carried flat to that boundary: wherever the bank or the rim
lies below it — which on a heightfield carrying nine metres of relief at a
two-hundred-metre wavelength is often — the drawn water ends in a cliff of
itself. Measured as the largest step in `fly_world_surface` across a section,
the six seeds read **3.0, 5.0, 6.4, 7.3 and 10.8 m**.

So the level is *let down to just under the ground* over the outer third of the
bank (`FLY_RIVER_EDGE`) and over the outer 450 m of a lake's disc
(`FLY_LAKE_EDGE`). The waterline is then wherever the two cross, which is a
contour of the terrain rather than a boundary of the field, and
`fly_world_surface` — the only one of these anything actually stands on — is
continuous across it. The same six seeds now read **0.49 to 1.08 m**, which is
the bank's own slope.

`fly_world_shore` is the other half of that. The letting-down means the water
field sits a fixed distance under the ground for the whole width of the edge
band, and the *ground* is described against the water: the sand under a
waterline, the wet band above it, the treeline, how low a field may be ploughed.
Keyed off the faded field, a lake came out ringed by four hundred metres of
beach with no trees on it. `fly_world_shore` is the level the body *has*, with
no fade, and it is what all four of those read.

### A lake is a disc, and a basin is not

One vec4 — centre, radius, level — and the water is drawn wherever the ground
inside the disc is under the level, so the shoreline is the terrain's own
contour and the disc is only a bound. That makes the bound the whole problem: a
disc drawn out to the basin's reach contains ground that is not the basin's, and
what the 312 m lattice cannot see is a hollow between two cells that both read
clear of the level. On seed 777 that speckled a night frame with three isolated
pools six kilometres out, which is what `render.night` is for and what it
caught.

The radius is therefore the largest disc round the basin's centroid that is
*nothing but* its own cells, plus the edge band. Inside it the only thing under
the level is the lake. It costs the basin's outer arms and it buys a lake that
never floods the next valley.

### What it cost, and what it bought

The carve is a chain of stations — one `vec4` each, against a cutting's three —
and a per-river bounding box in front of them, so a point nowhere near water
rejects every station of every river in four comparisons. `FLY_RIVER_PT_MAX` is
64 stations shared between at most four watercourses, and `FLY_LAKE_MAX` is six
discs: those are what a *uniform array* can carry next to `uCut[72]` and
`uLoc[28]`, not what the survey would like. TODO.md's "More earthworks than a
uniform array holds" is what lifts both, and the number it would move first is
the station budget — which is what decides how closely the drawn line can follow
the ground, and therefore the residual step at the waterline above.

What it bought past the look: the two things written for water that had never
met it. `chord_water` and the road network's ford limit now refuse a long
crossing of a *river* as well as of a bay, and the rail's piers carry the line
over one with the same navigation clearance they give the sea — both by asking
`fly_world_wet` rather than comparing a height to sea level, which is the same
question and the right way round. A wood no longer grows in a channel three
hundred metres up, because the treeline is measured off the water that is
actually there. And the chart draws it, so the river is on the map and under the
nose at the same time.

### Drawing it: a plane for the still water, a line for the river

The sea and the lakes are sampled off the terrain lattice, which is what they
have always been. A river is not: the finest water ring steps 26 m and the
coarse one 240, against a channel twenty to eighty metres across, so sampled on
that lattice a river is caught by some cells and missed by the ones between and
comes out as a chain of dashes that changes as the camera moves. `fly_world_still`
is the field the lattice reads and it leaves the rivers out; `draw_rivers`
builds the ribbon off the polyline instead, fifteen columns across the whole
footprint, and lets the fragment stage clip the shore off the interpolated depth
exactly as it does for a coast.

That ribbon then has to win the depth test against a terrain mesh drawn through
rings that coarsen with distance — and a river surface is below the ground on
both sides of it by construction, so out past a kilometre the 240 m mesh spans
the whole channel and buries it. The path tracer, which has no rings, drew the
same reach as one unbroken line; the rasterizer drew it as pools. The section is
lifted onto the coarse mesh the way a road deck is (`ground_bias`, now taking
the covering ring's own cell), with the *depth* left as the water's, so only the
surface it is painted on is the ring's.

Everything else was already written and gated, and that part of the entry was
right: the water material, the bathymetry, the shore foam and the surf are the
sea's, so a river is the same water as the coast in all three pipelines. The
path tracer needed one thing of its own — a channel is not a plane and cannot be
intersected as one — so the march answers first and the crossing is bisected out
of the segment the ray has already flown.

## The worked country

A country of twenty-four airfields with nothing at all between them reads as an
installation rather than as somewhere people live, and that is what this was.
Measured before anything was written, over seeds 4242, 7, 91 and 1234 on a
300 m grid across the chart: **70.6%** of the dry land inside the chart was more
than two kilometres from a settlement, a road or the rail (67.9–73.7), and
**53.1%** of it grew nothing at all (50.5–59.3). The settlement ladder had a top
and no bottom — a pad, a yard, a gateway and a spire district, and then
wilderness until the next one.

What was missing is the rung a country is mostly made of, and most of it is not
geometry. `fly_world_tilth` is the field: how strongly the ground at a point is
worked, which crop is in its parcel, and how far the parcel's boundary is. Ten
per cent of the land comes back worked, and it is the ten per cent every
approach and every departure is flown over.

### The belt is a settlement's, and it is not a ring

The shape is hung off the quantity the woodland field already computes — how far
out a point is in units of the nearest settlement's *own clearing radius*, so a
city that builds out to 330 m works ground an outpost that builds out to 170
does not. `site_clearings` is that loop, factored out of `forest_open`, and both
fields now read it once rather than each walking the location array.

Three things then narrow the belt, in the order they are cheapest to ask. How
far out the point is, which is already in hand. What the ground is, since
nothing is farmed under water or up on the tops. And which quarters are worked
at all, which is a broad noise — this is the term that keeps it from being a
decoration. Its first version could not reach zero (a floor of 0.5 plus half a
smoothstep) and the result measured **94%** of the belt's own band farmed, which
is a ring painted round every settlement; with a term that reaches zero it is
**82%**, which is worked country with rough ground in it. `world.tilth` holds
that at 90.

The outer bound is then moved by a second noise, so no parish ends on a circle,
and `FLY_TILTH_REACH` is the furthest that can put a field from the settlement
that works it — the largest clearing at the loosest bound. It is not decoration:
a shader is handed a *culled* list of settlements and cannot walk the whole
array, so a field beyond the cull is a field the GPU does not draw and the CPU
does. The terrain rings and the cascade both widen their cull to it, and
`world.tilth` measures the widest field on four seeds against the number
(3527 m against 3746).

### A field is cleared ground

The wood gives way to the plough and not the other way about. `forest_open`
multiplies the woodland density by `1 - smoothstep(0.10, 0.45, work)` and
`fly_canopy` does the same in GLSL, so a stand does not grow through the middle
of a crop and the treeline of a wood lands on a hedge. Measured against matched
ground — the same seed, the same distance band, only where a wood would grow if
nothing stopped it — the canopy over worked ground is **0.000** against
**0.941** beside it.

The first version of the ramp was linear in the work, and at a work of 0.4 it
drew a field with a wood standing in it. Fields are the sharper claim: any
meaningfully worked ground is bare of trees, and partly-worked ground is rough
grazing rather than half a forest.

### Enclosure is a pattern, and the only straight edges in the world

`fly_world_parcel` lays the fields out on a frame that belongs to the settlement
rather than to the world: its bearing, its parcel size and its origin are all
drawn from noise at the site's own position, so two parishes meet at an angle.
The courses are offset half a parcel like brickwork rather than ruled both ways,
and the boundaries wander seven metres over seventy-four, because a hedge laid
by eye is not a straight line.

The crop is one value for a whole parcel, read off the noise at the parcel's own
index — constant across a field and independent between neighbours, which is
what the eye reads a patchwork as. `world.tilth` measures it: the same crop ten
metres away **93%** of the time and three hundred metres away **0%**. A crop
that varied smoothly would be a wash and would read as haze.

Four crops in the rotation, banded rather than ramped. The bands are weighted by
how much of the worked ground each actually covers — 0.202, 0.322, 0.323 and
0.152, measured, because the parcel value is a noise sample and its distribution
is not flat — and that weighting is `FLY_CROP_MEAN`, which is what the parcel
colour fades toward once a field is smaller than a pixel. It fades on projected
size like every other octave in `terrain_surface`; what it never does is fade
*out*, because a farmed country is still a farmed country at twenty kilometres.

Two terms give way to it, and this is why the field is evaluated where it is
rather than at the end: the worn-earth ribbons do not run through a standing
crop, and neither does the needle litter.

### A hedge has to be walked, not searched for

Enclosure painted into the ground and nothing else reads as a pattern printed on
a lawn. What makes a boundary a boundary is that it stands up, throws a shadow
across the crop beside it and hides the foot of the field behind it.

The first attempt scattered candidates in each cell and kept the ones that
landed near a boundary. That is rejection sampling against a band five metres
wide inside a parcel two hundred across: it drew about **one bush per cell**,
which is a hedge nobody can see, and getting to a bush every couple of metres
would have wanted five hundred candidates a cell. So `fly_world_hedge` walks the
boundary instead — in parcel coordinates, where the boundaries are the lines of
the grid — and carries each point back through the same displacement the paint
is drawn with. That last step is one iteration of a fixed point and it is the
whole difference between thorn on the ribbon and thorn beside it: without it the
bushes stand up to seven metres off. Measured, the walk lands **1.62 m** from
the boundary at worst and 41.9 points a cell.

Every parcel edge is walked by every cell that overlaps it and each point is
kept only by the cell it falls in, so the phase is locked to the parcel grid
rather than to the caller's lattice: no double-planted bush where two cells
meet, and no gap either. The one place two points may coincide is a parcel
*corner*, where the walk down one boundary crosses the walk down the other —
0.4% of them, which is a thicker bush at a field corner and is where a hedge is
thicker anyway.

The line thins with distance on the same power-of-two ladder the road lighting
uses (`lamp_stride`), so a hedge two kilometres off costs an eighth of the one
under the wheels and keeps the bushes it had rather than renumbering them. One
standard in sixteen is a tree rather than a bush: a hedge left to itself grows
trees out of it, and a line of them is what carries a boundary at the range the
bushes have already gone.

### A farm, and where it may stand

`fly_steading_at` is the placement, and it is exported because *where* a thing
may be built is a claim about the world rather than about a frame — a rule the
test suite re-derives is a rule nobody is holding. One cell in thirty-two on an
88 m lattice, which is a farm to about every forty hectares, and then the rules
a settlement's own architecture is held to: dry ground, ground flat enough to
build on, off the carriageway, off the guideway, well outside the town whose
fields these are, and against a field boundary rather than out in the middle of
the crop. A candidate that fails is no farm rather than a farm shoved sideways,
which is the same answer `fly_site_footing` gives.

Two of those rules are there because `world.tilth` found them missing. The
verge was 26 m, measured from the *centre*, and a yard is sixteen metres square
— so a corner of one landed 3.4 m from a carriageway; it is 40 m now, which is
the yard's diagonal plus the road's own clearance. And the slope test was a
central difference at the centre, which is the ground under the barn door and
says nothing about the ground thirty metres away: a plot on a break of slope put
a fence post **3.53 m** in the air. The four corners are asked directly now, in
the yard's own frame, and the worst corner step over 169 farms is **2.00 m**.

The farm is square to its own fields, and the bearing is measured off the same
function that drew them — a central difference of the boundary distance, four
parcel evaluations, paid once per steading. A farm at a random angle to the
enclosure around it is a shed dropped on a pattern.

The lane in is a straight run to the nearest carriageway when one is inside
240 m, and nothing at all past that: a lane that needs a survey of its own is a
road, and the network already knows how to lay one.

### What it costs

The belt is two noise taps and a settlement loop, and the parcel frame under it
is five more where there are fields at all, against the eight taps
`terrain_surface` already spends — so the honest way to price it is a frame
filled with the thing it adds. Interleaved A/B, minimum of three runs each:

| frame | before | after | |
| --- | --- | --- | --- |
| alpine vista, 480x270, `render.budget` | 169.7 ms | 171.2 ms | +0.9%, noise |
| cruise over a parish, 960x540 | 573.4 ms | 647.6 ms | +12.9% |
| low pass over farmland, 960x540 | 831.6 ms | 885.1 ms | +6.4% |

The budget-defining frame is unmoved, and that is not luck: `tilth_ground` — the
water and treeline gates, which need only the height the caller already has —
is asked *before* the settlement loop, and every other term is a factor no
greater than one, so ground it rejects is ground the whole field rejects and the
skip is exact. On a mountain frame or a coastal one that is the loop gone from
every pixel. What is left is paid where the fields are, which is where the
frame gained a landscape.

### The GPU has not seen this

`fly_tilth`, `fly_tilth_work`, `fly_tilth_parcel` and `fly_site_clearings` are
GLSL twins of the C, and `gpu.tilth` measures the two against each other over
the whole belt on three channels. Nothing here has a GL driver, so the aspect
skips and the shader has never been compiled: it is written to be a literal
transcription for exactly that reason, and the constants it cannot include from
`fly_world.h` are written out beside `FLY_GLSL_WATER_Z` with the aspect named as
what holds them equal. The early-outs matter as much as the arithmetic — both
sides cut the field to zero below 0.02 — so the parity bound is on how many
samples disagree rather than on the worst one.

### A corridor is not a parish

The belt is a settlement's, so the country *between* two settlements is not in
it — and that country is most of the road network. 61% of the masts the line
now carries stand on ground `fly_world_tilth` calls unworked, which before this
was a carriageway across open ground with nothing built anywhere in it.

A parish is the wrong thing to put there: a second ring of fields halfway
between two towns is one parish drawn twice. What belongs to a *corridor*
rather than to a place is the thing that follows the corridor, and the cheapest
of those is a transmission line beside the road. It is also why it is this and
not the quarry the same entry asked for: the polyline is already surveyed,
already bounded in curvature, already clear of the water and already keeping
its distance from the guideway, so a line beside it costs nothing to route.

`road_pylon_at` is the placement and it is the road lighting's own rule at
220 m instead of 62 — by chainage rather than per station, so a tower never
crawls along the verge as the camera moves, and on one side for the whole
length of a road, drawn from that road's own termini rather than from its index
so the line is a property of the world and not of the order the network was
laid in. The pitch is deliberately not a multiple of the lamps': a tower
landing on a column's chainage every third span is a rhythm the eye picks out
of a moving frame.

### Where a tower may not stand, and what the wire does about it

Four rules, and each of them is a hole rather than a nudge — the answer
`fly_site_footing` and `fly_steading_at` both give. Nothing in water, asked of
all four footings and not of the centre. Nothing inside `FLY_STEADING_KEEPOUT`
of a settlement, which is where a run ends rather than carrying the grid
through a spire district. Nothing inside the structure carrying the guideway
over the carriageway. And nothing inside any road's right of way — asked
through `fly_road_blocked`, the network's own predicate, so a mast is held to
the rule every piece of settlement architecture is held to. That is the one the
offset cannot answer for on its own: the line is set out from its own road at
32 m, and a junction puts a second carriageway under it while a terminus widens
the corridor into the apron the road arrives through.
Over seed 4242 that is 983 masts on 26 roads with 880 of them standing, the
nearest carriageway 23.6 m off and the pitch even to the millimetre end to end.

A hole in a chain of towers would be a gap in a chain of wires, so the wire
spans it instead: three pitches of reach forward to the next mast that stands,
which covers the 300 m of water `FLY_ROAD_FORD` lets a road cross. A span twice
as long sags four times as far — the parabola's own scaling — so a crossing
hangs like a crossing rather than like a stretched version of an ordinary span.

The base is the highest of the four footings and each leg runs down to its own,
which is the rule `fly_steading_at` learned the hard way: a slope test at the
centre says nothing about the ground six metres away. So a tower stands
anywhere the road goes, a break of slope included, with no leg in the air and
none buried in the hill.

### What a wire costs at range

This is the whole of what the entry called the only real question, and the
answer is that a conductor is five centimetres and a lattice member a quarter
of one: both are sub-pixel long before the thing they belong to has stopped
being visible. A chain of sub-pixel triangles does not draw thin. It draws
*dashed*, and it crawls, because the triangles fall between pixel centres and
which of them land moves with the camera.

So everything thin here is held at a floor of about an output pixel, which is
the widening `draw_rail` already gives its barrel and for the same reason. It
is load-bearing and measured as such: at 300 m in a 320x180 frame a tower
covers 21 rows of the frame and its longest unbroken run of them is 21; with
the floor taken out the same tower covers 14 rows and the longest run is 8,
which is a dotted line where a mast should be.

The floor also decides which of the two builds of a tower a frame gets, and
that decision is on projected size and never on distance — the one lesson the
ground detail has already paid for, since the same member is half a pixel at
320 wide and three at 1920. While the legs are wider than a pixel the tower is
the lattice it is made of: four legs off four footings, the panels belted and
braced, two arms across the line and an insulator string hanging off each tip.
Once they are not, it is the envelope that lattice fills — a tapered column of
boxes — because four legs and their bracing each held at a pixel apiece is a
bar of ink several times wider than the tower it stands for, where one column
at the same floor is the honest silhouette and a sixth of the triangles.

The two ranges are then separate, and that separation is the shape of the
answer: the wires are given up at 1600 m and the masts carry on to 3400. Which
is what a line looks like from an aeroplane — the towers march off into the
distance and the wires between them go first. Measured across a window a tenth
of a span wide at mid-span: 350 pixels of conductor at 260 m and none at all at
2400.

### What the line costs

Interleaved A/B on frames chosen to be filled with the thing they are pricing,
minimum over nine alternating pairs of three frames each:

| frame | before | after | |
| --- | --- | --- | --- |
| alpine vista, 480x270, `render.budget` | — | — | no road in it, and nothing here is paid outside `draw_roads` |
| down a corridor from 165 m, 960x540 | 740.9 ms | 753.3 ms | +1.7% |
| a low pass beside a tower, 960x540 | 735.9 ms | 737.2 ms | +0.2% |

The second number is not the one that was expected. The low pass is the only
frame in which any tower is drawn as a lattice — forty members against the
silhouette's six boxes — and it is the frame that moves least, because it holds
one such tower and is otherwise full of near grass; the corridor holds the
whole run receding, masts and spans together, and is where the cost is.

Neither figure is resolved by this host, and the table says so rather than
pretending otherwise: the nine `off` minima of the corridor frame themselves
span 3.4% between best and worst, which is twice the difference being measured.
What the numbers do establish is a ceiling — a line beside every road in the
world does not cost this renderer a fraction of a frame anyone would notice,
even pointed straight down one — and that is the question worth answering here.
Anything finer wants a quiet machine.

The first cut of the far build took four height taps per tower to seat four
legs and then threw them away to draw a column of boxes. It is one tap now, and
that is most of what separates these figures from the first set measured.

One aspect had to move out of the way of this, and what it found is worth
keeping. `world.forest` compares the relief a frame over a wood carries against
a frame over matched open ground, and its open control is chosen for being
*unwooded* rather than for being empty — which on this world is a stretch of
carriageway. Putting a line beside it took that control from 108 pixels in a
thousand standing over six metres off the ground to 160, and the ratio under
its bound. The control was never open ground: it was a road, with lamp columns
already in it. Excluding the corridor from both passes — the move the aspect
already makes for the player's own aeroplane — reads 0 in a thousand for the
meadow against the wood's 369, which is a stronger claim than the one that was
passing before.

There is no GLSL half to this and there is not going to be one. The worked
field needed a shader twin because it is a *field*, evaluated per pixel of
ground on whichever side of the seam drew the frame; a tower is geometry, and
goes down the same triangle path as every other prop in the world.

Two things knowingly left standing. A tower is 32 m off the centreline and
`FLY_ROAD_CLEAR` keeps the wood off only the first 15, so a mast in wooded
country stands in the trees; it reads correctly — a 25 m tower clears a canopy
— and the alternative is to teach the woodland field about a wayleave, which
moves every measurement `world.forest` holds for a defect nobody can see from
the air. And nothing here casts, because `draw_roads` leaves the shadow pass at
the door and every piece of road furniture in the world is already inside it.
That is the right answer as well as the cheap one: a lattice is mostly air and
its shadow is a smudge, and the silhouette the far build draws would cast a
solid slab that is not there.

---

## The chart and the wrap

The wrap is in, and so is the half of it the instruments live on. `ground_raw`
samples the ball, `fly_glsl.h` carries the matching twin, chart positions are a
type of their own that cannot be subtracted, everything that measures measures
on the sphere, and an aircraft that flies past the antipode folds back into the
disc. `world.seam` holds the lot. The suite is green at 10106 checks across 75
aspects and the 81 stills are regenerated.

What the second half took, in the order it had to happen:

- **`fly_wpos`, with members `e` and `n` rather than `x` and `y`.** The rename
  is the mechanism, not decoration: a struct with matching member names lets
  every `a.x - b.x` in the codebase go on compiling, and those are exactly the
  lines that are wrong. Renamed, the compiler enumerates all of them — about
  ninety sites — and each one is then a decision rather than a search. Most of
  them turned out to be local arithmetic that is right as it stands (a pad
  apron 150 m across, a rail route between two neighbours) and are left alone
  and said so; the rest went through `fly_world_delta`.

- **`fly_world_delta` and `fly_world_dist`.** The way from one place to
  another: great-circle distance, and the great circle's own initial bearing
  written in the chart's local frame at the start, which is one scale factor
  (`a/sin a`) off the sphere's. In the settled world it agrees with the
  subtraction it replaced to 0.03%; between two places 1500 km out on opposite
  bearings the chart says 2121 km and the ball says 524.

- **The fold, applied to aircraft.** One line each in `fly_game_step` and
  `fly_pilot_step`. The flight law integrates in the tangent plane and has no
  idea the world comes back on itself.

- **`fly_world_dist` uses `atan2(|u x v|, u.v)`, not `acos(u.v)`.** acos is
  badly conditioned at both ends and both ends are load-bearing here — within a
  rounding of 1 is where every caller in the settled world lives, and within a
  rounding of -1 is the antipode. It showed as a four-kilometre step reading
  4005 m near the fold.

And one thing that was not the plan and had to be, because `world.seam` found
it on the first run: **the chart origin was being added to the chart
coordinates, and it had to be a rotation.** `sphere_dir(p + origin)` translates
a chart, and a chart is not a plane. That was wrong twice. It stretched the
world — the chart's tangential scale at the origin's own radius is `sin(A)/A`,
so a world centred two radians out had its features squeezed by more than half
in one direction and not the other, differently for every seed, which is not
what "only the realization moved" claimed. And it put the seam back: a position
folded at the antipode and the same position unfolded are the same point on the
ball and different chart coordinates, so adding the origin to each landed
somewhere different — 16 m of cliff across 2 m of ground, measured. As a
rotation (`fly_chart_frame`, Rodrigues about the axis across the origin's
bearing, four floats, exact GLSL twin) both go away: the fold now measures
0.032 m, which is float. It re-rolled every world again, which is why the stills
moved a second time.

Two things worth keeping, because both were latent long before the switch and
both were found by it rather than caused by it.

- **The 3D noise was quieter than the 2D noise it replaced.** Trilinear
  interpolation blends eight independent corners where bilinear blends four, so
  the raw fields share a range and not a distribution: standard deviations of
  0.4295 and 0.3714 over 600k samples. Ported term for term with no gain, the
  mountain gate opened where it used to stay shut, the rolling relief lost a
  seventh of its swing and the world's sea cover halved from 2.2% to 1.1%
  across four seeds. `FLY_NOISE3_GAIN` is that ratio and `rng` holds the two
  within 1%; with it, sea cover lands back at 2.3-2.6%.

- **Which landscape a seed got was one draw of one number.** The ridge field
  runs at 15 km and the gate that decides whether there are mountains at all at
  26 km, against a settled area 30 km across — so the whole playable world was
  very nearly a single sample of both. On the chart this replaced, five of
  twelve seeds came out habitable and the rest were highland with no sea in the
  settled area: means of 452, 467 and 893 m, worlds the autopilot flies into.
  The four seeds the suite uses were three good draws and one mountain, which
  is why it never showed. `fly_world.origin` now decides where on the ball the
  chart is centred, searched at world-gen for somewhere with a coast and no
  wall of mountains through it — about one candidate in five qualifies, so it
  ends in a handful of tries. Sixteen of sixteen seeds are habitable now.

What it cost: `fly_world_ground` went from 206 ns to 304 ns a sample, 1.47x,
measured on a 490k-sample sweep rather than off the `bench` rows — the bench
moved in both directions because the camera is in a different world now and it
is measuring what is in frame. Less than the 2x the hash count suggests, and
still the hottest function in the engine. **Caching the mapping per terrain
cell rather than per sample is the obvious next lever and is not done.**

Gates that moved, and why none of them moved for the reason expected. The
estimate here was `world.forest`, `world.canopy`, `render.material`,
`render.coast`, `world.siting` and `render.aa`. `world.canopy` and
`world.siting` never moved at all. What did move was mostly assumptions the
tests had picked up from one particular world and never stated:

- `world.forest` compared a meadow in a downpour against a wood in sunshine —
  the site scan took the first match and that was the corner of its own search
  box, in the middle of the storm belt. Rain streaks are colour edges: 114
  against the wood's 142, and 21 with the precipitation zeroed. It sites both
  shots in clear air now, and reads 8 against 144.
- `render.coast` searched 4.4 km around home for a waterline and `game.ruin`
  wanted open water within salvage range of home's pad. Both held because seed
  4242 happened to have sea 500 m from the origin. Neither aspect is about
  where the water is; they look for it wherever it is now.
- `render.settlement` used the upper third of the frame as its "no props in it"
  control, and this site's flare stack and towers stand well above the skyline:
  168 of 285 moved pixels were in the control. Cut at a fifth, the control
  reads 0 and the ratio bound is 20x rather than 3x.
- `render.ambient` asserted both its sample points were vegetated, which they
  were, and said so apologetically — the shore band used to be grass to the
  waterline, so the bare-surface branch could not run. It runs now (veg 0.31),
  and the two are asserted in opposite directions. Its bounce row asserted sand
  is brighter than grass; it now asserts the bounce follows the ground's own
  albedo, which is what was meant and does not depend on which way round this
  seed's two surfaces sit.
- `render.aa` fell to a 5.9% horizon cut against a 10% bound — on three
  viewpoints, which its own comment says is too few to take a camera-independent
  measurement. Five viewpoints, 12.2%, bound unchanged.
- `render.night` grew one deterministic speck, identical at 1, 2, 4 and 8
  samples, on the terrain/sky boundary 23 km out: a partial pixel carrying the
  sky by coverage while its depth reports the ridge behind it. The horizon ring
  is excluded — 339 px of 34964, and the bug the aspect is named for speckled a
  whole mountain.
- `game.pilots` ran its all-detail window for six minutes and asserted more
  than zero deliveries. Nothing completes a haul in the first eight minutes of
  a cold world: 0 at three, 0 at six, 5 at nine, 15 at twelve. It was returning
  exactly 1 and measuring the clock. Twelve minutes, and more than six.

The projection, unchanged and settled: **azimuthal equidistant centred on the
chart origin**. Radial distance is exact, the tangential scale is
`sin(d/R)/(d/R)` — 0.042% across the content band against equirectangular's
0.125% — every direction loops at 3770 km rather than only east, and the single
singularity is the antipode at 1885 km rather than two poles at 942 km. The
wrap rule past the antipode is `d -> 2*pi*R - d` with bearing `+ 180 deg`.

---

## Space and the planet

The physics was in and the rest of it was not. `sim.orbit` and `sim.ascent`
held the two claims that matter — the field is inverse-square and space is
gated by air density rather than by permission — and underneath them the whole
of *owning* a rocket was unfinished in four separate ways, each of which is the
same mistake: five parts were added to systems that were never told about them.

- **The stack was a third of every shelf in the world.** Every place the game
  rolls loot picked a base uniformly out of the registry, and the registry went
  from twelve entries to seventeen. So five of every seventeen shelf slots,
  world derelicts, wreck drops and payments-in-kind were spaceframe parts —
  including on the apron the player starts on, where tokens accumulate and a
  price is not a gate. `min_ilvl` is the fix, drawn through one
  `fly_module_roll_base` that every one of those four call sites now shares.
  Fifty, chosen off the map rather than picked: item level is `1 + 98*danger`,
  and at fifty every world measured has at least one city or skyport that
  stocks the stack and none of them is the one the player starts at — 11 of 35
  service sites over five seeds. Below it the count is zero, not rare.

  The draw is one `fly_rng_range` whichever way the filter falls, because that
  call consumes one word regardless of its bound. A world generated before the
  floor and one generated after it differ in what is on the shelf and not in
  what every later system draws, which is the difference between a rebalance
  and a reseed.

- **Rarity bought nothing on the five most expensive items in the game.** The
  affix pool is drawn from the fields a base already engages with, and nobody
  added the rocket's. A Prototype Rocket Motor could roll mass, drag, Vne and
  structure — and not one newton of vacuum thrust — so a One-off motor and a
  Stock one were the same spacecraft. `FLY_AFFIX_ROCKET`, `_PROPELLANT` and
  `_RCS` close it, gated by the same rule the gun roll lives under. The two
  ratings the stack also carries are deliberately *not* rollable: `heat_shield`
  and `pressure` are read as thresholds, so a roll on either moves a number
  nothing reads.

  It also exposed a formatting rule that was a proxy for the wrong thing. How
  many decimals a roll needs was keyed off the affix's `scale`, which put the
  cold-gas roll — 0.22 rad/s^2 at its best — in the whole-number branch and
  printed `+0 rad/s2 RCS`. It is keyed off `ref * scale` now, which is how big
  a strong roll actually is, and `game.space` asks every affix in the table for
  its own strongest roll and refuses a zero.

- **Nothing drew the parts.** `FLY_VIS_ROCKET` through `FLY_VIS_CABIN` were
  declared, set by the modules, folded into the fitted airframe and saved with
  it, and no geometry read any of the five. The airframes gallery published a
  plate captioned "loadout B: Rocket Motor, RCS Thrusters, Propellant Tank,
  Pressure Cabin, Ablative Shield" over a picture of a stock Skylark. Each is
  drawn where its own module says it is now — the bell aft because it is the
  engine slot, the tank in the belly because the hold has gone, the aeroshell
  as the underside because it is the hull — out of the same lofted body, box
  and tube the rest of the aeroplane is made of.

- **A saucer was drawn as an aeroplane.** `antigrav` is the one field in the
  flight model with no engineering behind it and it is nonzero on exactly one
  thing in the world, so `draw_craft` had everything it needed to know better.
  It is a lens with a dome about the vertical now, with a rim of lamps running
  round it in a cycle rather than an aircraft's fixed red, fixed green and
  white flash — which is the whole read at the six pixels across it usually
  is. `render.spaceframe` measures it as a shape rather than as a picture:
  overhead its planform fills 0.77 of its bounding box against an aeroplane's
  0.27, and no retint could pass that. The lens is lofted in two halves split
  at the rim — one profile through it has too few stations to curve and a
  crease that gets averaged away, and the rim is the one hard line on the
  object — and it carries its own segment count rather than the shared body
  ring, because ten sides is plenty of a fuselage read from the side and is a
  countable decagon on a disc read broadside.

And one that only showed once there were frames to look at: **it was raining at
96 km.** `fly_world_weather` is a map — it varies in x and y and it never read
the z of the position it was handed — so a ship in orbit sat in whatever storm
was over the ground beneath it. Not cosmetic: turbulence enters `fly_sim_step`
as a force proportional to mass and to nothing else, the one force in the
flight model with no density term, so a spacecraft at 150 km was buffeted
exactly as hard as an aeroplane at 500 m. There is a weather layer now, flat to
6 km and out by 14, which is the top of the convective layer and also every
altitude the game is actually flown and gated at — so nothing below 6 km moved
by a float.

Done. The limb is drawn by both renderers and `render.planet` runs on whichever
backend the build has: the horizon dip is `arccos(R/r)` to 0.0025%, the surface
normal at a hit is the sphere's rather than the chart's, the disc shrinks
between 600 km and 1800 km at ratio 5.28 against the 5.00 `tan^2(asin(R/r))`
predicts — the excess is the atmospheric ring, which is counted with it and is
proportionally wider from further away — and at dawn the frame carries a lit
face, a dark face and black space. `gpu.frame` holds the two halves together at
3.3/255.

`fly__planet_hit` is used twice per sky ray, once to stop the scattering column
at the surface and once to shade what is at the end of it, so the transmittance
the surface is seen through is the transmittance of exactly that path.

**What it is a picture of took longer than how it is drawn.** The limb was
three tones off the heightfield — sea, lowland, highland — and the heightfield
was a 9 km field with nothing under it, which meant the planet had no oceans on
it: the only water anywhere was where the rolling ground dipped below its own
mean, 2.3% of the surface in spots a few kilometres across. From orbit that is
a scatter of speckle, and no shading fixes it. Four things replaced it and
`world.planet` measures all of them on the ball rather than in a frame:

- **Continents.** A five-octave field from 250 km to 16 km decides sea floor
  from continent, with a steep slope between them — 34 km of elevation per unit
  of the field — so the ±380 m the 9 km relief can move takes the coast ±6 km
  and a shoreline is a shoreline with islands off it. Seventy-three per cent of
  the planet is water against Earth's seventy-one, 68-79% across eight seeds,
  and the lift at 150 km says the water is in basins rather than in spots: land
  follows land at 1.74x the base rate of 0.34, against the 1.0 a field of 9 km
  ponds would score and a ceiling of 2.93. Coastline crossings along a great
  circle rise 1.41x between an 8 km sampling step and a 500 m one, which is the
  measurable half of "not a smooth curve".
- **Orogeny.** Ranges are built at the seaward edge of a continent and where two
  meet, both of which the continental field already knows, so mountains cost
  two smoothsteps on a number the terrain has computed anyway. What it buys is
  that the snow line has something to trace: the scattered 26 km mountain field
  put thousands of short white threads over every continent, and a chain a
  thousand kilometres long down a coast is what snow looks like from outside a
  planet.
- **Climate.** Aridity (the horse latitudes, plus continental interiors, plus
  three octaves of noise so it is not striped) and a snow line that falls with
  latitude, rises where it is dry, carries its own noise so a cap is not a
  circle drawn with a compass, and passes the waterline at |sin lat| 0.86 — one
  expression for a mountain's snow and a polar cap, because they are one fact.
  Both live in `fly_world` rather than in the renderer, because world-gen reads
  them: home is chosen temperate and watered, so the coast that is green from
  orbit is the coast you take off from.
- **Weather.** A synoptic cloud field on the ball — bands for the convergence
  at the equator, the clear subtropics and the mid-latitude storm track, five
  octaves for what a front looks like. It is not the cloud deck: that is a
  5.2 km chart-space field which is a fifth of a pixel from orbit and has a
  seam in it besides. The two never appear together, and not by two fades that
  happen to meet — the deck's own haze fade is `exp(-t/38000)` and this is
  weighted by exactly `1 - exp(-t/38000)`.

`terrain_surface` is still the wrong function to call up there, but it is no
longer a different answer: it crosses over into `planet_albedo` as the camera
climbs (see below), and its tones were re-anchored on the ground shader's own
measured albedos — a wood at 0.092, open ground at 0.192 — so the two agree
where they meet.

**The near field is not drawn from space.** The LOD rings, the water, the
scatter, the settlements and the rail are a description of the ground you fly
over, drawn on a flat 38 km square of chart centred under the camera; from 150
km that is a tile of a different world laid over the planet, straight-edged on
all four sides, with trees on it a sixtieth of a pixel across. It is keyed on
the camera's altitude and that is not a stand-in for a pixel rate: the chart is
a *plane*, and at range `d` it stands `d^2/2R` above the sphere it maps — 300 m
at the outermost ring's corner, 3 km at sixty kilometres. So `terrain_surface`
crosses into `planet_albedo` between 25 and 60 km and the passes stop being
drawn at the top of that, by which point they are already the colour of the
planet under them. Every service ceiling is below the crossover and every space
frame is above it.

Navigation lamps needed the same cut for the same reason: `light_splat` floors
a lamp at most of a pixel so a beacon does not flicker at range, and that floor
had no far end, so twenty-four aircraft over the settled world came out as
white specks on a planet seen from 150 km. They fade out between 9 and 26 km,
which is where a real one stops being called.

One more floor with no far end was in `terrain_surface`'s grain — the term that
hands back the contrast the faded octaves took with them, at a wavelength tied
to the projection so it is always about a dozen pixels across and therefore
always resolvable. Its wavelength is capped at 60 m, and past the range where a
pixel outgrows sixty metres the cap breaks the very property the term exists
for: 125 m a pixel at thirty kilometres, 620 m at a hundred and fifty, and a
60 m noise read at that rate is a different random number per sample rather
than a texture. It is dropped now once its wavelength falls under two samples,
which is the least that can carry one, and `render.material`'s own measure of
the thing improves with it — the faded albedo sits 0.0045 from the cell mean
against 0.0069 before and a point sample's 0.0175.

**Still there, and deliberately left:** between about ten and twenty-six
kilometres the CPU rasterizer's near field carries a quilt of 950 m squares.
It is the outermost LOD ring resolving one colour per vertex while the octaves
it samples — the 380 m mottle, the 290 m woodland field — are chosen by the
*pixel* rate, so the mesh is asked to carry fields finer than its own cells.
This is the clamp `terrain_ring` computes as `cell_px` and never applies, noted
below under its own heading; it predates the continents (it is worse on the
code this replaced, which had more contrast in those octaves) and the GPU path
does not have it at all, because that one shades per pixel. Fading those two
octaves by the pixel rule is *not* the fix and was measured: at a 240 m cell a
380 m field is still nearly constant, so fading it to its plane mean is a bias
rather than a blur, and `render.material` catches it immediately — the faded
albedo goes from 0.0045 off the cell mean to 0.0851, five times worse than
point-sampling. The fix is to give the per-vertex path a sampling rate where it
currently passes a pixel rate, which is a change to the LOD ladder rather than
to the shading, and it wants doing with the ladder in front of it.

**What it costs**, measured rather than assumed: `fly_world_ground` went from
312 ns to 469 ns a call and a 480x270 raster frame from 471 ms to 543 ms, which
is +15%. All of it is the continental field's five octaves, and none of it is
recoverable without giving up the thing it is there for — the fine octaves are
precisely the coastline detail, and the third of the cost that sits in the
first two octaves is unavoidable in any form of this. Skipping the fine ones
away from the shore was considered and rejected: it needs a bespoke early-out
fbm with an exact GLSL twin, in the one function the whole engine is hottest
in, to save about a twentieth of a frame.

**The atmospheric ring is in.** The note that used to sit here said the sky
integral just needed to run further. It did need that — from 600 km a ray a
degree above the limb has not reached its perigee for a thousand kilometres and
is still 326 km up when a 320 km integral ends — but lengthening alone was
worse than useless, because `scat_thick` took altitude to be linear in `t`. On
a grazing ray the altitude falls to a perigee and rises again; the linear model
keeps descending, drives the endpoint underground and saturates the clamp into
a black band.

What replaced it is the Chapman column, and it is not only the ring:

- `scat_thick` keeps its signature and integrates on the ball. The radius along
  a ray is exactly `sqrt(r0^2 + 2 r0 c u + u^2)`; to second order that is
  `r0 + c u + (1-c^2) u^2/(2r0)`, so the integrand is a Gaussian times an
  exponential and the integral is an `erfc`. Both limits come out exact — `H`
  straight up, `sqrt(pi H r/2)` along the horizontal — and the erfc's own
  asymptote hands back the flat `1/c` as soon as the ray is steep enough not to
  care. A segment is the difference of two columns-to-space, with a case split
  on where the perigee is that is not cosmetic: subtracting the tail off a
  descending ray would subtract a column that continues underground.
- `fly__erfcx` is `exp(z^2) erfc(z)` to a relative 1.1e-7 — the scaled form,
  because at the elevations a steep ray sits at both factors leave float range
  long before their product does. Numerical Recipes' fit, one `exp` and a
  degree-nine polynomial, with an exact GLSL twin. `erfcf` would do on the C
  side and there is nothing to call on the other, and a shader that disagreed
  by a percent would show as a seam between the two renderers.
- Measured against a numerical integration of the same geometry, the near field
  agrees to 0.05-0.64%; against the `sqrt(2 pi H (R+hp)) exp(-hp/H)` asymptote,
  a grazing ray from 600 km agrees to five figures at every perigee from 0 to
  88 km. `render.atmosphere` holds both, and `render.planet` reads the ring off
  a rendered frame: disc 0.4475, ring 0.5005, space 0.000005.

Three corrections came out of it that are not about the ring at all, and all
three were the flat chart showing through:

- **The horizon was 3.7 times too thick.** A level ray at sea level climbs 333 m
  over its first twenty kilometres and never comes back; the flat model billed
  it for 320 km of air where the ball says 86.8. That was spent on the horizon
  and on the setting sun, and correcting it moved `render.atmosphere`'s 320 km
  transmittance from 0.02 to 0.066 and `render.ambient`'s dawn/noon blue ratio
  from 0.85 to 0.852.
- **The factor three in `FLY_SCAT_SPREAD` is not arbitrary after all.** A
  grazing column is `sqrt(2 pi H R)`: 173.7 km here against Earth's 566, short
  by 3.26 because a small planet curves away that much faster. The horizontal
  compression is very nearly exactly what it takes to make this horizon as thick
  as a real one's while the vertical column, which does not depend on R, stays
  at H.
- **The sun-transmittance sample was taken on the slope.** A ray half a degree
  below level from 400 m never gets below 377 m; the old expression had it a
  kilometre underground by 160 km out, clamped that to zero and lit the far half
  of every shallow ray with sea-level sunlight. That was also what made
  `render.atmosphere`'s storm row read 1.041 for a moment: aerial perspective
  appeared to overshoot the sky because the two were sampling the sun at
  different altitudes. On the ball it reads 0.957.

The 0.035 elevation floor on the sun is gone with it. It existed because the
flat path length ran away below about two degrees; on the ball the horizontal
column is finite and reddens the last light on its own.

The ring exposed one more of the same kind and it is fixed here too: **the haze
had no terminator.** `e.sun` is one direction for the whole chart, which is
right — the sun is far enough away that its rays are parallel — but its
*elevation* is `dot(sun, up)` and `up` turns with the ground. Read as `sun.z` it
says the sun is equally high over every point on the planet, so the air over the
night side scattered daylight. Invisible while a sky ray from orbit came back
black; with the ring drawn it was a halo all the way round a planet at midnight.

Asked instead at the segment's *lowest* point — its perigee when it has one, the
nearer end when it does not, which is where a ray's air is. That point is chosen
so the fix costs nothing anywhere else: inside the atmosphere it is the camera
(a level ray at sea level has its perigee under it), and it is a thousand
kilometres and sixty degrees of arc downrange only for a ray grazing the limb
from orbit, which is exactly the case that needed it. Measured across the whole
suite, nothing inside the air moved past the fourth digit; at dawn from 600 km
the terminator band went from 729 px back to 3166, and the ring reads 0.380
toward the sun against 0.013 away from it.

What is deliberately *not* moved with it is the altitude the sun's own column is
sampled at. The two answer different questions and only one of them is geometry:
`zm` feeds the sun's transmittance, which for a sky ray comes out effectively
unattenuated, and FLY_SCAT_SUN is the free scale calibrated against exactly
that. Sampling it where the air is instead — the same lowest point — is the
better model and a retune of the entire exposure: a zenith ray at noon would
pick up about 8 km of sun column where it now picks up 40 km's worth of nothing,
which darkens the zenith by about a quarter in blue and rather less in red, so
the anchor colour (0.10, 0.24, 0.44) and every constant set against it move
together. Worth doing as its own job, with the noon zenith re-anchored first.

---

## Renderer

### The renderer as it stands

#### Graphics levels

One ladder of four rungs — `low`, `medium`, `high`, `ultra` — and every cost the
renderer can trade hangs off it, priced in a single table (`lod_for`) rather
than as `detail >= 2` scattered through nine thousand lines. That is not
tidiness: a level is a claim about a frame budget, and the way a "low" preset
stops being low is one locally-reasonable `>= 1` at a time.

| | low | medium | high | ultra |
| --- | --- | --- | --- | --- |
| supersampling | 1 | 1 | 2 | 3 |
| GPU multisampling | 4 | 4 | 2 | 2 |
| terrain mesh | sparse | dense | dense | dense |
| shadow cascade | 512 px, no PCF | 768 px | 1024 px | 1024 px |
| traced sky occlusion | off | 12 rays | 16 | 24 |
| body ring / aerofoil points | 10 / 8 | 10 / 8 | 14 / 12 | 18 / 16 |
| woodland reach and LOD | 0.55x | 1x | 1.25x | 1.5x |
| blade layer | none | 300 m, half the cells | 420 m | 560 m, every cell |
| scrub layer | none | 165 m | 231 m | 308 m |
| plume lobes | 4 | 7 | 10 | 14 |
| volumetric sun shafts | off | off | 12 steps | 24 steps |
| software sky | 4 px lattice | every pixel | every pixel | every pixel |

They are ordered and cumulative: rung *n* draws everything rung *n-1* draws, so
a caller can reason about them as a slider, and `render.quality` holds that —
each rung costs more than the one below it and produces a measurably different
picture.

The multisampling row is the one that goes *down*, and it is the row that used
to be read wrong. The two kinds of sampling compose: supersampling renders the
frame at ssaa² the pixels and multisampling stores msaa coverage samples inside
every one of them, so the old top rung was resolving an edge from nine times
eight — seventy-two samples for one output pixel, of which it could see about
eighteen. That is not a free unused knob. A multisample target is memory and
bandwidth: at 1080p the old `ultra` asked a GPU for a 5760x3240 colour buffer at
eight samples of RGBA16F, over a gigabyte before the depth buffer, and every
fragment retired wrote coverage into all eight — while an implementation that
*cannot* make that target silently falls back to one sample, so the rung asking
for the most edge quality was the likeliest to end up with none. The rungs above
the first now buy their edges with the sampling they are already paying for.
`render.quality` gates the composed figure, `ssaa² × msaa`, rather than `msaa`
alone, both for the ordering and for a ceiling.

The aircraft is deliberately not cheapened at `low`. A whole airframe is a few
hundred triangles against tens of thousands in the terrain ring mesh, so the
budget rung keeps the model it has and spends its savings on the ground; what
the higher rungs buy is a rounder hull, not a hull the low rung had to give up.

`low` is the one rung defined by a number rather than by a look: 33 ms — thirty
frames a second — at 480x270 on one CPU core with no GL driver anywhere, because
the software path is the fallback and a fallback nobody times stops being one.
It is a target and not yet a fact, and `render.budget` prints the distance every
run rather than hiding it: on a shared virtual core it renders the alpine vista
(deliberately the most expensive view there is) in about 160 ms. Drawing the sky
last, into only the pixels the ground did not cover, and putting it on a refined
lattice at this rung, took that from 465 ms; asking the frustum before scattering
a wood took it from 230; and sharing one scattering integral across a triangle
whose corners are all at the same range took the rest.

Both of those last two go one grain coarser now, over the scene's objects: a
tree is one point as far as the air and the cloud deck are concerned, so a crown
buys one scattering integral and one deck tap between all thirty of its
triangles instead of thirty of each. What licenses it is not the metres of
spread but the *bearing* across them — the Mie phase function turns far faster
with angle than the air column does with distance — so the group takes the same
hundredth-of-the-range bound a triangle takes, with an absolute cap for the far
field, and `render.fog` prints what it costs at that limit (1.1 of a code value)
and over a real wood (0.1 at worst). On a machine with a GPU the object path
does not pay for the air at all any more: captured vertex colours go up
unfogged and the fragment stage applies aerial perspective per pixel, which is
what the terrain and the water already did. Over closed canopy at 1280x720 that
and a memo on the scatter's ground lookups take the object build from 88 ms to
53; `render.cost` prints it.

Frames render on the GPU wherever one exists, on every platform, through
headless OpenGL ES 3.1 / GLSL — a surfaceless EGL context on POSIX, a WGL
context on a hidden one-pixel window on Windows, where there is no EGL. Either
way there is no window to look at and no display server to need, so the
headless CLI and the test suite are accelerated too. The scalar C renderer is
kept as the reference implementation and the fallback, and the two are gated
against each other by the `gpu.frame` aspect.

On the GPU:

- the **path tracer**, as one fullscreen program;
- the **rasterized environment** — sky, four terrain LOD rings, three water
  rings — as real vertex/index buffers in a depth-tested pass;
- the **sun shadow map**, both built and sampled: the three cascades are
  rendered into offscreen float images the beauty pass then reads, so they
  never touch the CPU. Receivers offset their sample along the surface normal
  rather than biasing its depth, which is what keeps a grazing sun from
  striping open ground without detaching every contact shadow;
- the **volumetric correction** — the sunlight that never reached the air
  between the camera and the world — marched off the view depth the geometry
  pass wrote into the frame's own alpha;
- the **emissive splats and the translucent pass**, laid over that frame;
- **auto-exposure**, reduced to a 1x1 image the post programs sample rather
  than a number the CPU has to be told;
- the **post chain** — bright pass, separable blur, sun shafts, ACES tonemap,
  vignette, the dithered resolve, and the supersample box filter folded into
  its last pass so the frame is downsampled before it is read rather than
  after.

Settlement and craft geometry, the scrub and the trees' boles are still built
and shaded on the CPU and handed to the same depth-tested pass as triangles
rather than composited afterwards. The wood's crowns are not: they go down as
one static template mesh per species, level of detail and baked shape, instanced
once per tree and shaded in the fragment stage — see the work log.

#### The frame does not come back

Everything above is one claim: the picture is the only thing that crosses the
bus. That is not tidiness either — it is the difference between a frame budget
set by the GPU and one set by PCIe.

A frame finished on the CPU crosses three times. The radiance comes down as
floats so the volumetric correction, the splats and the translucent pass can
run in C; it goes back up for the post chain; and the picture comes down. At
1080p on `high` — which supersamples, so the render target is 3840x2160 — that
is 133 MB down, 100 MB up and 33 MB down: **260 MB a frame, 16 GB/s at sixty of
them**, over what PCIe 3.0 x16 delivers. No GPU fixes that, because the GPU is
not what is busy.

Left resident, the same frame moves 19 MB: its geometry up, its picture down.
Nothing that crosses is sized by the render target, so supersampling — the
largest multiplier the renderer has — costs nothing at all here:

| 1080p, `high`, one chase frame | up | down |
| --- | --- | --- |
| resident, ssaa 1 | 10.8 MB | 8.3 MB |
| resident, ssaa 2 | 10.8 MB | 8.3 MB |
| resident, ssaa 3 | 10.8 MB | 8.3 MB |
| read back, ssaa 1 | 44.3 MB | 41.5 MB |
| read back, ssaa 2 | 119.0 MB | 141.0 MB |
| read back, ssaa 3 | 243.4 MB | 306.9 MB |

The 10.8 MB is the frame's *geometry* — the terrain rings, the shadow
occluders, and the settlement and woodland triangles the CPU still builds and
shades. It scales with the scene rather than the resolution, and it is now the
largest thing crossing in either direction, which is the standing renderer entry
in `TODO.md` (draw the wood the way the terrain rings are drawn).

`fly_gpu_traffic` counts it and the `gpu.traffic` aspect gates it, because a
readback is one line, it is correct, it is invisible in every screenshot, and
it costs the frame rate — so it is measured rather than trusted. `gpu.resident`
holds the five stages that moved against the same GL geometry pass read back
and finished in C (`fly_render_force_readback`), which is what keeps the scalar
versions the reference rather than a second opinion.

Two of those stages needed the frame's *depth*, not just its radiance, and
neither uses a depth buffer to get it: multisampling stores depth per sample
and there is no defined way to resolve that into something a later pass can
read. They test against the view depth the geometry pass writes into the
frame's alpha — the same number, in the same metres, that the C versions
compare against. The frame lives in two float targets for the same reason: a
pass that rewrites the frame writes one and samples the other, so it never
reads the texture it is writing.

The star field is a point sample of a hash, which is right for a ray looking at
the sky and wrong for every other place the sky gets sampled — as a fog colour,
as a reflection, or as the light a diffuse bounce brings back. Sampled as a
light source, one star gets multiplied through the albedo of whatever the bounce
ray hit, and at night that speckled entire snow slopes with what looked like sky
showing through the mountain. Direct views get `fly_sky`; everything using the
sky as light or fog gets `fly_sky_amb`, which is the same gradient without it.

Water is one material across all three pipelines. The rasterizer hands it a sky
sample and the tracer hands it a traced reflection ray — that difference is what
the tracer is for — but the waves, the Fresnel, the glint, the surf and the light
coming back off the sea bed are the same function either way, so a coastline is
the same coastline whichever mode drew it. The sea floor is ordinary terrain that
happens to be below the waterline, which is what gives the shallows a gradient to
grade through: the bed shows through the first metre or two on a Beer-Lambert
falloff, tinted, because water absorbs red first.

The sky carries two layers. The cloud deck is a short volumetric march through a
580 m slab of water cloud whose base is flat and whose tops follow the weather;
seven kilometres above it is a sheet of ice, which is one plane intersection and
one field lookup rather than a march, because at that height its own depth is
inside a pixel from anywhere the game is flown. What a ray picks up from either
is Beer-Lambert over the path it actually crossed — `1 - exp(-density * length /
322)` for the deck's four segments, the same form over the slant path for the
sheet — so a ray crossing fifty kilometres of deck sideways is opaque and one
crossing 580 m straight down is not, which used to be the same number.

What that costs to keep honest is resolution, not steps. A ray a degree above
level spends those fifty kilometres inside the layer, so its four taps land
kilometres apart and each stands for a segment of a field whose finest octave is
650 m across. Three things follow, and the exponential makes all three matter
more than the old fixed alpha did — its slope in the density is the segment
length over 322 where the fixed alpha's was 0.45, so at five degrees it
multiplies the field's own sampling error by six. Each tap's density is the mean
of three stations a third of a segment apart, so the four of them walk the
interval at twelve uniform points. Each tap has two footprints rather than one:
across the ray it covers what a pixel covers, along it the whole segment, and
the fine octaves — which are what a cloud mass is made of across the sky — are
fetched at the pixel rate while the collapse to the deck's mean, which is what
stops the base octave aliasing, takes the segment. And the early-out sits at
0.995 rather than 0.98, because what the march returns is the mean colour of the
taps it took, so stopping two per cent short drops a whole tap out of that mean
at whatever elevation the saturation crosses — a step across the sky at one
elevation, which is the artifact the smoothness gate exists for.

What both layers are lit by is the same sun and sky as everything else: albedo
times the sunlight above the layer plus the sky it can see, computed once a
frame at each layer's own altitude rather than kept as constants. Moonlight is
an irradiance on the top, not a colour added to both faces, so a night cloud
base is darker than its top and sits at the same order as the sky it covers
instead of an order above it.

Settlements are seated on the lowest ground under their own footprint rather than
on the height at their centre, so nothing is left with a corner hanging in the
air on a slope. A buried corner is a foundation; a floating one is the single
thing that gives a settlement away as pasted on.

Seating a wall correctly still leaves it meeting grass in a straight line, so
everything a site builds now stands on concrete: a slab wider than the walls,
reaching from below the lowest ground under its own oversailed footprint up to a
hand above the seat. A hut gets a hand's breadth of it and a storage tank gets a
bund. The hardstands work the same way from the other end — they are seated on
the *highest* drawn ground so nothing is buried, and then carried down into the
lowest so nothing floats. Getting only the first half of that right is what left
every apron in the gallery reading as a sheet of card dropped on a lawn: the
underside of a slab levelled on its high corner is in mid-air over the low one.
The two halves also read different surfaces on purpose — the top clears the
*drawn* mesh, which stands above the heightfield over concave ground, and the
underside gets below the heightfield itself.

Every site carries the same kit of small pieces on top of whatever its kind
builds: a fenced yard with a gate, container stacks and a travelling gantry; a
fuelling apron with a shed, bunded tanks and a bowser standing proud of the
ground on its own hardstand; an ops block under a lit control tower; floodlights
on hardstands at the pad rim; and a truck driving a circuit. None of it is placed at a
fixed offset. Each group tries a short list of bearings against the same footing
rule the buildings use — off the pad, out of the rail's right of way, out of the
road's, out of the water — and takes the first that fits, so a site on a ridge lays out differently
from one in a bay without anything being authored twice. A group states how far
its pieces reach back toward the site as well, because the footing rule walks a
plot to find ground and then judges only where its centre ended up: a compound
whose centre clears the deck can still put a corner post on it. Two distance bands drop
the kit entirely past 2.6 km and thin it past 900 m, which is what keeps a world
full of settlements affordable. What the pieces are for is scale: a crate and a
handrail tell you how big the tank behind them is, and four smooth hulls on bare
ground cannot. Graded gravel tracks run from the deck edge to each group and from the apron to
the yard's gate, which is what stops good buildings reading as objects set down
on a field. They sit on the drawn ground rather than on the heightfield: the
terrain mesh's finest cells are twenty-six metres across and the surface
rasterised inside one is the bilinear interpolation of its corners, which over
concave ground — including the graded ring around every landing pad — stands
the better part of a metre above what `fly_world_ground` returns. Every lamp on a
site stands beside one of those ways, along a pavement edge, at the gate or at
the pad rim on a hardstand of its own; there is one function that places them and
it takes the two ends of the thing being lit, because a lamp in the middle of a
field is lighting a field. The kit's first version had a ring of six of them at
the pad radius plus twenty-six metres, which is exactly that. After dark the
same kit is most of what is lit — lamp heads with
pooled light on the ground beneath them, tower glazing with an interior, warning
lights — and the flare, the plumes and the truck are what keep a still frame from
looking like a photograph of a model.

A cascade's texel size is the one number behind both ways a shadow map looks
wrong: it is the amplitude of the staircase along an edge, and — since both the
depth bias and the normal offset are counted in texels — it is also how far a
shadow floats away from the thing casting it. Extent and centre distance are
therefore one decision rather than two. Each cascade is centred a fraction of
its own extent along the view, so the small one covers what is close and the
large one covers what is far, instead of all of them crowding around a single
focus point five kilometres away. Its extent is quantised to a ladder and its
light-plane origin snapped to the resulting texel grid, so the map lands on the
same world texels frame after frame and an edge stops crawling as the camera
moves — which only became visible once the near cascade's texels were small
enough for it to matter.

Surfaces reflect through a microfacet BRDF: GGX for the distribution,
height-correlated Smith for the masking, Schlick for the Fresnel, and the
split-sum DFG term for what a surface reflects of the sky. What it replaced was
a normalised Blinn lobe, and the three ways that was wrong were all visible
rather than academic. It had no masking term, so a rough surface turned edge-on
to the sun could reflect more light than fell on it — which does not show up as
a bright patch but as a quietly wrong exposure, because the auto-exposure is a
log average over the whole frame. Its tails fell off exponentially where real
roughness falls off as a power, so a highlight was a hard bright coin with
nothing around it, and the skirt around a highlight is most of what reads as
*how rough is this*. And its normalisation was only right for a mirror-ish
surface seen head-on, so changing an exponent changed the energy as well as the
tightness and every material had to be re-tuned against every other. Materials
still describe themselves in Blinn exponents, which is what the tables and
`fly_render_material_probe` speak; the conversion happens inside. The claims
worth making about a BRDF are claims about a function and none of them can be
read out of a tonemapped frame, so `fly_render_brdf_probe` exposes the lobe on
its own and `render.brdf` holds it to energy conservation, reciprocity and
monotonic roughness.

Terrain carries a material rather than just a colour. `terrain_surface` reports
what the ground is made of — vegetation, reflectance, a specular exponent —
carried through exactly the mixes its colour is, so a snowfield takes a broad
sheen, bare rock a rough one, and the wet band at a shoreline the tight
highlight that makes wet ground read as wet rather than as darker sand.

Open ground is a set of layers rather than a green. Under the three grass
species and the mottle there is the bare-dirt patch field, and under that a
network of **worn earth tracks**: ridged noise — `1 - |n|` thresholded near its
ridge — whose level sets are *lines* rather than islands, so one tap buys
winding bare-earth ribbons a few tens of metres wide that join up across
kilometres. That is the layer the ground was missing, because a patch field can
make bald spots and cannot make a path. The ribbons run to soil in the middle
and to a paler, grittier **gravel shoulder** at their edges, with a fine
aggregate grain over both. Each of them fades toward its own plane mean once a
pixel outgrows it, the way every other octave here does — a nonlinear function
of a noise tap that is point-sampled past its resolution is a bias, not a blur —
and `render.material` holds the whole chain to landing at least twice as close
to the true cell mean as a point sample does.

**Rain reaches the ground.** The weather had precipitation in it and the
renderer only ever spent it on streaks across the lens, so it rained on a
landscape that stayed as dry as it was at noon in high summer — which is the one
thing that gives a weather system away as an overlay. Water on a surface does
two things and neither of them is a streak: it darkens the albedo, because a
film fills the pores and light that would have scattered straight back out takes
another bounce first; and it replaces a rough surface with a smooth dielectric
one, which is what actually reads as wet — the lobe tighter and the sky in it.
Not the reflectance: water is n = 1.33 and F0 = 0.02, *below* the mineral it
covers, so both surfaces converge downward onto the film's own number from
wherever they started. Vegetation splits both halves, because a blade of grass
is already smooth and sheds the film where soil and rock hold it; a version that
darkened the meadow as hard as the mud made a shower look like dusk, and one
that smoothed it as hard as the mud made a meadow look moulded. What does not
run off collects: **puddles**, gated on slope before anything else, because a
mirror on a hillside would give the whole thing away in one frame, and level,
because a sheet of water takes the shape of the hollow it stands in and has no
relief of its own. `render.material` measures the set — the darkening, both in
albedo and in what the ground actually shades to; that wet meadow and wet rock
end up nearer each other than dry ones do, since a film of water is a film of
water whatever is under it; that bare ground's lobe tightens far harder than a
canopy's; that a puddle takes no hummocks — and that a dry frame is
bit-for-bit unmoved, which is the failure mode a wetness model has.

**Pavement is a surface.** Every square metre of it — the pad deck, the
hardstands, the service tracks and the roads — was one flat colour with one
specular level, and stayed that colour in the rain. `pave_surface` gives it the
lands it was laid in (a 21 m patch field, coarse enough that a deck tessellated
into three-metre segments samples it smoothly instead of faceting on it), the
aggregate showing through, and the weather. The pad deck is drawn in four bands
rather than as one triangle from the rim to the centre — a touchdown circle
where the tyres land and the rubber goes down, the working surface, the run-off,
and the concrete apron at the rim — with sealed radial joints at a fixed count
round the deck however finely it is tessellated. Hardstands get a wearing course
of four-metre bays close in, and the ground where the work happens gets the
stains under it. Roads get a hard shoulder over the outer fifth of the
formation, laid *inside* the kerb line because the deck stands on the highest
ground under its own width and anything built outboard of it would float half
the time. `render.paving` holds the tonal spread of the deck and that it goes
dark when it rains.

Lofted geometry is smooth-shaded. Every body, wing, tube and pod on both
airframes is built by lofting rings, and shading each facet against its own face
normal makes a prism however many sides it has: ten flat tones at ten sides,
eighteen at eighteen, and no side count fixes it. The normal is interpolated
across the facet instead — taken from the loft's own lattice, so it is the
surface's normal rather than an average of the facets standing in for it, which
is what keeps a collapsed ring at a nose cone from picking up a star. It costs
two vector adds a vertex and no triangles at all. The two end caps stay flat,
because the end of a hull is a disc that genuinely has one normal and smoothing
across the rim would round off an edge that is really there. `render.smooth`
measures it across the belly of a fuselage, as the largest step between
neighbouring pixels over the scan's whole tonal range: 79% shaded flat, 9%
interpolated (61% and 7% averaged over the scanned rows).

Aerofoil sections come from the NACA four-digit thickness distribution rather
than from an eight-entry table, cosine-spaced along the chord. The reason is the
table's shape rather than its size: a table can only be resampled between the
points it has, so asking for a finer section got more vertices along the same
straight lines — and the nose, where all the curvature is and where a faceted
wing shows first, is exactly where a table has no point to give.

The air between the camera and the world is marched for sun shafts at the top
two rungs, off the finished depth buffer, so it is identical whichever pipeline
drew the frame. The shape of the term is what makes it a light shaft rather than
a lens flare: it **only ever takes light away**. The frame has already added the
fully-lit in-scatter, so what is missing is the part of it that never happened,
and the beams are what is left standing when the air around them goes down.
Adding a bright streak toward the sun instead would put energy into the frame
that no light source emitted, in the shape of the screen rather than the shape
of the world — which is what the post chain's radial gather does, deliberately
and separately, as a lens effect. It marches the near field only, on a
grid of one ray per two *output* pixels — not per two render-target pixels,
which quietly made this one of the things supersampling multiplies and had the
top rung marching nine times the rays for a picture the box filter interpolates
back out again — dithering each sample's march by a hash of its own pixel so the
step count reads as a little noise rather than as a ring of hard shells around
every occluder; the hash is a pure function of the pixel, so the frame stays
reproducible.

Each step is one unfiltered tap of the shadow map rather than the kernel a
*surface* lookup uses. A surface filters because the shadow's edge is the
pixel; a parcel of air is averaged along the ray, dithered, and bilinearly
upsampled before it is composited, so a kernel underneath all that is the same
softening a third time at up to thirty-six taps a step. And a ray with nothing
between it and the sun is skipped before its in-scatter is evaluated at all —
exactly, not approximately, since the taps are 0 or 1 and a fully lit ray's
correction is a true zero. Between them the pass went from over half of a `high`
frame to a few per cent of one.

How much it is worth depends entirely on what is standing in the light, and
that is worth saying because it is easy to measure in the wrong place. Over open
country the correction is a ten-thousandth of the frame's radiance — not a bug,
just the honest amount of in-scatter a few kilometres of clear air contributes
to a pixel, shadowed or not. Put a city's towers in front of a low sun and the
deepest correction is thirty times that, which is a visible band of dimmer air
between the beams. `render.shafts` holds both the sign and the depth, on a scene
that has occluders in it.
Vegetation is shaded as a canopy rather than as a green plane: light wraps
around blades so the terminator is soft, the surface has a hot spot down-sun
because a canopy hides its own shadows when you look along the light, and it
glows yellower than it reflects when lit from behind.

Woodland is a landscape rather than a texture. `fly_world_forest` is a property
of the world, not of the renderer: two low-frequency octaves say where the
stands are, and the falloffs say where they stop — at the treeline, at the
waterline, on ground too steep to hold soil, and around the clearing a
settlement sits in. Over a wide transect it leaves about half the world open,
a third under closed canopy, and the rest in the edge between them, which is
what makes a wood read as a wood: it has a meadow next to it. The scatter
plants from that field — three species, three levels of detail, a dense cell
carrying ten trees near the camera and fewer, larger ones far away, so canopy
coverage holds up as the triangle budget falls off.

The near model is where a wood stops being a texture, and what it needs is a
silhouette. A crown was one ring lofted to a point, which is a cone from every
angle however its proportions are chosen: standing in a wood you got flat-sided
parasols with a hard rim and a visible flat underside, and a conifer's skirts
were two crossed quads apiece — a bowtie you could see straight through. A
broadleaf crown is now a three-station profile between a point underneath and
the apex, with the widest ring at half the crown's height (a quarter of the way
up makes a mushroom, which is what the first attempt was), and a conifer's
skirts are rings with ragged tips under a leader spike, because a spruce is
read by its point. Every crown vertex is jittered in *height* as well as in
radius, which is the change that actually breaks the outline — a ring jittered
only in radius still lies in a plane, and a plane lofted to a point is still a
cone — and every face carries its own value, so a crown reads as light broken
by the leaves in front of the leaves instead of as a painted solid.

A stand is also *where* its trees are pointing, and for a long time that was
four positions: the crown rings were rotated by `(h & 3) * 0.4` radians on a
face spacing of 0.79, which is not a rotation, it is a small set of poses. From
the air that is exactly what a wood looked like — identical dots on a grid,
because the crowns were not merely the same shape but the same shape facing the
same few ways. Sixty-four positions over the whole turn cost the same one shift.

And a wood is not trunks standing on a lawn. What is actually under and between
them is scrub — whatever the wood is made of coming up where the light gets in
— and its absence is why the scatter read as a grid however good each tree was:
with nothing at the intermediate scale to look at, the eye reads the spacing. A
bush is a two-ring dome on no trunk through the same canopy BRDF the crowns use,
so it wraps and transmits like foliage rather than like a green stone (one ring
lofted to a point is a cone, and a field of cones a metre and a half high reads
as traffic management). It is thickest where the canopy *breaks* rather than
where it is closed, since a closed wood is dark underneath and an edge is where
the light gets in, and it is priced off the blade layer's rung rather than the
woodland's because that is what it costs like — a great many small things close
to the camera — so the budget rung gives up both together.

Trees and boulders get the same ambient contact ring settlements have had since
they were built (`prop_skirt`, below). The sun shadow map handles the sun, and
on the overcast afternoon much of the gallery is shot on there is barely a sun
to cast; a stem occludes most of the sky from the ground at its foot whatever
the weather is doing. It is near-field and hard-limited — forty metres for a
tree, and the skirt's own projected-size gate below that — because the ring
costs a heightfield gradient and a terrain shade at each of fifteen stations and
there are thousands of trees.

The clearing a settlement stands in is sized to what that settlement builds.
One radius for every kind was 170 m bare thinning to 430, which is right for an
outpost — a shed and a mast — and wrong for a city, whose spire district is laid
out on rings at 130 and 235 m: the outer ring of towers stood in ground the
field still called a quarter wooded, with trees between the buildings and across
the ground the aircraft come in over. `fly_world_clearing` gives each kind its
own radius and `world.forest` holds the result at every bearing over every site
in the world — nothing wooded inside a perimeter, and the wood back at four
times it.

All of it is confined to the near rung: 2.8% of a frame standing inside a
broadleaf wood and 4.5% in a conifer stand, measured on the software path.
Extending the same raggedness to the 240–850 m band cost 6% for a shape nobody
at that range can resolve, and was reverted — which is the trade this whole
function is built on. The same field lays needle
litter and canopy shade into `terrain_surface`, because a wood whose floor is
the same green as the grass beside it reads as trees standing on a lawn; the
GLSL twin carries the identical mask, so both backends agree about where the
wood is.

Everything a settlement stands on is darkened where it meets it. A wall
occludes most of the sky from the grass at its foot and none of it a few metres
out, and that gradient is there in every weather — unlike the sun, which the
shadow map handles and which an overcast noon barely has. Without it every prop
in a site had a hard seam against unbroken grass, which is the loudest
"pasted on" cue an outdoor scene has. `prop_skirt` lays a ring of
ground-following triangles round a footprint, sampling the terrain's own
surface at each vertex and ramping only the *ambient* term from dark at the
wall to open sky at the outer edge — so it is a decal rather than a material,
and it matches whatever is underneath it, grass or dirt or the litter under a
wood, at any hour. It is skipped while casting (a shadow that casts a shadow is
an object) and below about twenty-six pixels of footprint, where the whole ring
is thinner than a pixel and all it can do is cost heightfield taps.

Storeys are storeys, too. The window grid used to divide whatever height it was
given into at most eight bands, so a 24 m block and an 88 m spire both got
eight rows — 11 m between floors on the spire, which is why a skyline read as
slabs with a decal on them rather than as buildings of different sizes. A floor
is 3.6 m in a tower and in a shed, and the row count follows from that, bounded
by the projection rather than by a constant: five pixels a storey, below which
a row of windows is a grey smear costing two triangles a pane. That is a cut
and a saving at once — the old cap drew all eight rows on a tower three
kilometres away. A base course and a parapet finish it, because the bottom and
the top are the two places a real facade always does something.

What the storeys are made of took longer to fix, and the loud half of it was
not the pattern. Every window was drawn with `raster_tri_view` — a constant
colour, straight into the frame, with no lighting of any kind — and glass is
the *most* view-dependent surface a settlement has: what you see in a window is
the sky, and which piece of sky depends on where you are standing. Painting it a
constant made every tower a black-and-white checkerboard that stayed exactly as
bright in shadow, at dusk, in a storm and head-on into the sun. Glazing is
shaded now, so a window reflects the same sky the aircraft parked in front of it
does and goes dark when the sun leaves it.

The quiet half is that one pattern is not a city. Three constructions, chosen
off each building's own seed: a **curtain wall** of continuous ribbon glazing
storey by storey, with vertical mullions standing proud and the wall showing
between the ribbons as the spandrel; **precast panel**, with punched openings,
lintels and a recessed joint at every floor so it reads as stacked panels rather
than as one poured surface; and **profiled metal cladding**, vertical ribs the
full height of the wall with a strip window between them, which is what a shed
or a light-industrial block is actually built out of. Openings are *recessed* on
the first two and proud on the third, which is most of the difference between a
hole in a wall and a tile stuck on it, and the frames and joints are the wall's
own colour moved a few per cent rather than a contrast — it was the contrast
that made the old grid read as a checkerboard, not the grid. Curtain wall is
also the cheapest of the three, one quad a storey a face instead of eight panes,
which is why the towers get it.

`render.facade` holds two things about it, taken from above the skyline where
the depth buffer separates buildings from sky exactly: no single 8-bit tone may
hold a quarter of the built surface, which is the direct signature of a constant
fill and does not care about exposure; and the facade answers the light, over
two frames three hours either side of noon where the only thing that differs is
which side of the buildings the sun is on.

The three ground scatters — wood, blade layer, boulders — walk a *disc* of
cells around the camera, because that is what the world looks like and where
you approach a cell from must not change what grows in it. A 16:9 frame covers
about a quarter of that disc, so each of them asks the frustum before it places
anything. That is not the same as walking a smaller disc: the placement stays a
pure function of the cell, and what is skipped is the heightfield sample per
tree and the scattering integral per vertex that used to run before `project`
threw the triangle away for being behind the eye. The bounding volumes are
deliberately generous — a cull a metre too tight does not remove the wood, it
removes the trees at the edge of the frame, which is the hardest thing in a
rendered image to notice — and `render.cull` holds the frame against the middle
of a wider one, pixel for pixel in radiance, so a volume that gets tight fails
rather than ships.

The air is one model. Sky colour, the colour of the sun and what distance does
to a hillside all come out of a single Rayleigh/Mie single-scattering integral
rather than three gradients tuned separately — which is why they can no longer
disagree. Blue is scattered six times as hard as red, so a short path overhead
stays blue and a long one along the horizon arrives warm and washed out; the
sun's disc is the sun seen through that same transmittance, so it reddens
because its light crossed more air and not because a curve said so at that hour;
and aerial perspective is the same integral stopped at the surface — what
survives of a colour, plus what the air in front of it added. Sun shafts are
that in-scatter gated by the cloud deck: air scatters only the light that
reaches it, so a gap lights the beam and a cloud does not. Both the shaft and
the cloud march ask the deck one question per ray, and both stop trusting the
answer once it stops being answerable: over a long path a single tap is a point
sample of a five-kilometre field, so it fades into the deck's average instead —
the shaft over the field's own wavelength, the march over how much deck one
pixel covers. That is what the horizon of every outdoor scene used to be doing
wrong; the row of hard grey slabs along the skyline was the same tap sliding a
whole cloud cell between one pixel and the next, 160 kilometres away. The model
carries one term beyond single scattering: light that has bounced more than once
arrives from everywhere at once, and it matters in proportion to how much of the
path has scattered at all — nothing at a thin noon zenith, most of the light near
the horizon and at dusk, which is exactly where a single-scattering sky comes out
too dark and too grey. The column is integrated on the ball, not on a
slope: a level ray at sea level climbs 333 m over its first twenty kilometres
and never comes back down, so the air out to space along the horizon is
`sqrt(2*pi*H*R)` — 173 km of sea-level-equivalent air, twenty-two times the
vertical column, and 3.7 times less than a flat model bills for the same ray. It
is a Chapman integral in closed form, exact at both limits and handing back the
flat answer as soon as a ray is steep enough not to care. That is also what
draws the *atmospheric ring*: from six hundred kilometres up, the limb is those
twenty-two atmospheres lit end-on, and it comes out as bright as the disc. It
has a terminator, because how high the sun is over a piece of air is
`dot(sun, up)` and `up` turns with the ground — asked at the lowest point of the
ray, which inside the atmosphere is right under you and for a ray grazing the
limb is a thousand kilometres downrange. So the ring follows the day side round
and stops (`render.planet`).

One free constant is not physical — the atmosphere is thickened along the
horizontal, because this world is twenty kilometres across and real sea-level
air barely touches a ridge three kilometres off. Horizontal is the word that
matters: the world is a scale model across and full size upward, so thickening
the vertical column with it would only mean permanent golden hour. The factor
turns out not to be arbitrary either — a small planet's grazing column is short
by exactly the ratio of the two square roots, 3.26, so the compression is very
nearly what it takes to make this horizon as thick as a real one's. And one is a
calibration, the sun's irradiance, set so the noon zenith lands where the old
hand-tuned gradient put it.

What a surface is not lit by directly, it is lit by the sky above it and the
ground below it, and both of those are integrals of things the world already
knows rather than constants. The sky is projected onto nine spherical-harmonic
coefficients once a frame — off the same scattering model the sky itself is
drawn with, with the sun left out because it is already a directional light —
and reconstructed per vertex, so a wall facing a sunrise is lit by the sunrise
and the wall behind it is not. The ground bounce is the terrain's own albedo
under the camera times whatever is falling on it, so an aircraft over a meadow
goes green underneath and one over an estuary goes dark. And reflectance is
capped at grazing incidence by roughness and by vegetation: Schlick alone takes
every surface to a mirror edge-on, which is true of a flat interface and false
of grass, and a meadow reflecting the sky at 1.9 times its head-on brightness
reads as polished rather than as a field.

Ground keeps its texture at every range. Each albedo octave still fades toward
its mean once a pixel can no longer hold it — point-sampling past that is a
bias, not a blur — but the variance it takes with it is no longer simply lost,
which is what turned distant ground to velvet. It comes back as one more
sample whose wavelength is tied to the projection rather than to the world,
about fourteen pixels across at any distance, so it is resolvable by
construction and cannot alias. How much comes back is decided by the
anti-aliasing guarantee rather than by eye: a faded sample must still land
twice as close to the patch mean as point-sampling does, and roughly a third of
the lost amplitude is all that fits inside that.

Water carries detail down to the half-metre when a pixel can hold it: a 1.6 m
ripple and a 0.5 m one, both skipped outright at range rather than multiplied
by zero, and whitecaps that break where a wave has got too steep to hold
together rather than spread evenly in proportion to the wind. What the octaves
stop resolving is not thrown away — the slope variance they carried widens the
glint lobe instead, so a distant sea is rough rather than a mirror.

Place has a cast of its own, which the air cannot supply: a moor is not a
savanna is not a snowfield. The biome under the camera is sampled once a frame
— not per pixel — and turned into a small bounded grade applied just before the
tonemap, gain then saturation. Bounded is the point: a tint that can reach a
colour cast will eventually ship as one.

An aircraft is built out of solids. Every part used to be a bare triangle — a
fin was one triangle standing on edge, a tailplane one delta, a wing two — and
a triangle has no thickness, one normal and no edge: it vanishes when seen
edge-on, it carries one flat tone whichever way it is turned, and it has no
silhouette to catch the rim of sky that makes painted metal read as metal.
Three builders replace that and cover both airframes: a hull lofted through a
run of rings, an aerofoil section to make the rings out of for anything meant
to fly, and a round tube for the struts and the wheels.

Two things the parts read off the airframe asset rather than off a literal.
Its **span**, so a 16.5 m Condor and an 8.2 m Wasp are different aeroplanes and
not the same one twice — the model is drawn to a 5.5 m reference half-span and
scaled by the ratio, so chord, fuselage and tail all grow together. And its
**`gear_height`**, which is the height the sim already parks the body origin at
while the aircraft is on its wheels: the undercarriage is built down to exactly
that, and so is the clearance under anything slung beneath the belly.

Symmetry is a property of the construction, not of the numbers. Each side is
built by negating y over the same planform, ring vertices are laid at half-step
angles so none of them lands on the centreline, the quad split flips across
that centreline so the two halves triangulate the same way, and the wingtip
lamps are held to the same luminance whatever colour they are. `render.airframe`
measures the lot from a head-on studio plate: the aircraft's nose is turned to
the sun's azimuth and the camera put on that line, so geometry and lighting are
both mirror-symmetric and whatever is left over is the model being lopsided.

The one thing in the world that is not built this way is drawn out of the same
three builders anyway. A saucer is a lens with a dome lofted about the vertical
instead of about the nose, dispatched off `antigrav` — the single field in the
flight model with no engineering behind it — so the thing that does not fly
with wings does not get an aeroplane's shape or an aeroplane's navigation
lights. `render.spaceframe` holds that as geometry rather than as paint:
overhead, its planform fills 0.77 of its bounding box against an aircraft's
0.27.

It is lofted in two halves split at the rim, and it carries its own segment
count. Both are the same argument from what the shape is. A hull loft averages
its normals over at most eight stations, so one profile through the rim gets
too few stations to curve *and* a rim whose crease is smoothed into the facets
either side of it; the rim is the one hard line on the object, so splitting
there costs nothing and buys twelve stations of curve. And a fuselage is a tube
read from the side, where ten sides is plenty — a saucer is a disc read
broadside and edge-on, where ten sides is a decagon you can count in the
gallery plate. There is one of these in a world and it is the thing a player
flies over to look at, so it takes the full ring at close range whatever rung
the rest of the frame is on: about six hundred triangles for the whole object,
against tens of thousands in the terrain mesh under it, which is inside the
noise on a frame time.

A propeller is a thing you see through, and it is drawn as one. Both
rasterizers write an opaque colour wherever they pass the depth test, so the
few triangles that need alpha are shaded where they are built, queued, and
composited at the end of the frame against the finished z-buffer without
writing any depth of their own — which is also why the software and GL
pipelines cannot disagree about the disc: neither of them draws it.

The shape then comes out of one line. Time-average the blade over its sweep: at
radius r a chord c occupies c/r radians, and smeared across `sweep` radians it
covers c/(r·sweep) of the exposure. That is the opacity, and three things fall
out of it. It goes as 1/r, so the disc is densest at the root and nearly clear
at the tip — the same blade has more circumference to cover further out. It is
small, a couple of per cent at the tip, which is what a photograph of a
spinning prop looks like. And it saturates to a solid blade as the sweep goes
to nothing, so winding an engine from stopped to full throttle closes the arcs
into a ring with no threshold anywhere. Underneath, at anything slower than a
fast idle, are real blades — tapered, twisted, rounded at the tip — because the
eye has time to look at those. The propeller and a drone's four rotors are the
same code in different planes.

Foliage is lit as foliage. A leaf triangle takes its normal from the crown's
centre rather than from its own billboard — one normal per vertex running out
of the mass of needles, so a crown has a lit side and a shaded side at the same
moment and shades smoothly across a facet instead of resolving to one flat
colour. Leaves then go through the same canopy BRDF the ground uses for grass,
so they wrap around the terminator, surge when you look down-sun, and transmit:
a crown between you and the sun glows rather than going black. Neither costs a
new lighting model; both are the existing one, applied to the right normal.

Trees cast, into the near cascade only. That is the cascade with
tenth-of-a-metre texels, and a crown's shadow on the ground beneath it is worth
having there and nowhere else; the caster draws the mid-detail model, because a
shadow is a silhouette and a spruce's third skirt is not in it.

A canopy also occludes the sky it sits under, and that half is traced rather
than assumed. A ray tracer answers "how much of the hemisphere can this patch of
ground see" against the actual trunks and crowns, and the answer is cached **on
the ground** — a 4 m lattice held in a toroidal window about a kilometre across.
That is the difference between a term that survives flight and one that does
not. The usual way to amortise traced occlusion is to reproject last frame's
pixels into this frame's camera, which throws every sample away at a
disocclusion — precisely the silhouette of a tree line, and precisely what an
aircraft generates continuously. Occlusion here is a property of the world, and
the world is a pure function of position and seed, so a sample belongs to the
ground: fly away and back and it is still there. Arriving somewhere cold costs
about eighty thousand rays; every frame after it traces none, and in a session
only the strip of lattice that just came over the horizon is ever cold — about
twenty cells a frame at cruise. Interactively even that is bought on
instalments, a few thousand rays a frame, so the term converges over about half
a second instead of stalling one. Both backends read the same window, the
software one per pixel and the GPU one as a texture with the same four taps and
the same weights, which is what keeps them within parity.

It replaced a scalar that said `1 - 0.42 * woodland mask`, and the replacement
had to wait for the woods. At the old density — eight trees in a 44 m cell,
crowns three metres across — a wood floor genuinely saw almost all of the sky,
so the tracer disagreed with the scalar and the tracer was right: the scalar was
asserting something false about the world. Denser stands are what made honest
occlusion worth its rays. Past the traced window the scalar is still what
answers, because a wood at half a kilometre is exactly the thing it describes
well, and the two agree closely enough there that the crossover is invisible.
Litter says what the floor is made of, occlusion says how much sky is left to
light it, and albedo alone reads as a patch of brown paint.

Near ground carries relief, as a bump rather than as geometry — two octaves of
value noise summed into one height field and differenced once, so six taps buy
a two-scale gradient.

Everything with a scale to it — the relief, the albedo's octaves, the sea's
waves — fades on **how many pixels it covers**, not on how far away it is. The
same tussock is four pixels across at 320 wide and twenty-four at 1920, so a
distance fade tuned to stop one resolution shimmering strips the other bare. And
a faded octave is a skipped one, since the fade reaches exactly zero: detail
stops costing anything at exactly the range it stops being visible. The scale is
*output* pixels, so `--ssaa` renders the same world more cleanly rather than a
more detailed one.

Fading toward the *mean* of an octave rather than dropping it matters as much as
when. The mixes that turn noise into a colour are nonlinear, so a point sample
and an average are different colours, not the same colour at different
sharpness. Terrain is shaded in the fragment stage on every LOD ring for this
reason — resolving it per vertex on the coarse rings drew a hard square on the
ground where the ring changed. For the sea's glint the same argument runs the
other way: the slope variance an unresolvable wave octave carried is added to
the lobe's width instead of being discarded, because roughness too fine to
resolve geometrically is still roughness.

Geometry is multisampled (`--msaa N`, default 4): coverage and depth are stored
per sample while the fragment shader still runs once per pixel, so silhouettes
are antialiased for a fraction of what supersampling costs. `--ssaa N` remains
the only thing that antialiases *shading* — which is also why the two are not
bought at full price together; see the graphics-level table above. Measured against a 16-sample
reference frame, multisampling cuts the error along the horizon — where the
frame is mostly silhouettes — by 22%, and near the camera, where a pixel is
usually interior to one surface, not measurably at all — that is shading, and
only supersampling touches it. The `render.aa` aspect measures both.

The GPU path is never a hard dependency. `./make.py --no-gpu` builds without it
(the module compiles to stubs), and a build *with* it still falls back at
runtime when no device, context, entry point or shader can be had.
`build/<rid>/fly99 new` prints the active backend:

```sh
./make.py --no-gpu        # build the CPU renderer only
build/<rid>/fly99 new --seed 7  # ...prints `renderer: llvmpipe (LLVM 20.1.2, 256 bits)`
```

Measured on a 4-core host with **no GPU at all** (Mesa llvmpipe), whole raster
frames, GPU pipeline vs the same build pinned to the CPU: 1.7x at 160x90, 3.4x
at 480x270 ssaa2, 4.2x at 640x360 ssaa2. The ratio climbs with resolution
because what remains on the CPU is largely resolution-independent. Real
hardware widens it further.

Neither are threads. When the toolchain has them the CPU path tracer splits its
rows across cores — probed exactly as the GPU is, so a build without them runs
the same tracer on one core and draws the same frame. It draws *the same frame*
literally: each pixel seeds its randomness from its own coordinates, the way
the GLSL tracer always has, so the result does not depend on how the rows were
divided up or on how many cores did the dividing. `./make.py --no-threads`
builds the serial one, and the two are byte-identical.

What the tracer costs is almost entirely the terrain height field: a hybrid
frame at 960x540 evaluates it 141 million times, ~270 samples per pixel, and
96% of the frame is spent inside it. That is why the two things that pay are
not marching the same ray twice and not marching sky that cannot contain
ground — a ray above `FLY_GROUND_CEILING` and still climbing stops there, which
is a termination and not an approximation, so the frame is unchanged to the
bit. Together with the threads, one such frame went from 66 s to 6.6 s here.

Because the backend is chosen per machine, committed stills depend on the host
that generated them; byte-identical frames across machines is explicitly not a
goal. The GPU and CPU pipelines are also not pixel-identical by design — the
GPU shades terrain albedo and the shadow lookup per fragment where the CPU
interpolates per vertex, and its waterline follows the shoreline instead of
stepping along grid cells.

### The hybrid blend

In `mix`, where the two halves disagree about what is *there*, the half that
found something wins rather than the two being averaged: the rastered terrain
rings run out around twenty kilometres and the march reaches twenty-six, so
every hybrid frame has an annulus in which one half is sky and the other is
ground, and an average of ground and sky is neither. By day that reads as a
faint wash; at night the sky in that band has stars in it, and a star is a
point sample, so one landing in the annulus came through as an isolated bright
blue pixel on a dark slope — the sky showing through the ground, literally.
That rule lived only in the reference tracer for a while, and the GPU tracer,
which is the one that runs wherever there is a GL driver, went on averaging;
`render.night` was failing on exactly that pixel and now holds on both paths.

### A claim the storm term no longer has

`render.atmosphere`'s aerial-perspective row used to catch a deleted storm term
at ratio 1.36. It now measures 1.002, because that 1.36 was the underground
integration inflating the haze and its sky reference by different amounts, and
`fly__scatter` now stops the column at the real horizon. With the physics
right, an overshoot without a storm term is arithmetically impossible — the two
figures are the same integral over different lengths.

So nothing currently fails if the storm grey is deleted. The gate that would is
a positive one rather than a bound: a storm must measurably grey the distance —
saturation of a far ridge falling, and the in-scatter converging toward the
grey rather than toward the clear-air sky — which is what the term is actually
for. Worth adding next time this file is open.

### Work log

- **The CPU rasterizer had no fragment stage for terrain.** It resolved one
  colour per vertex of the LOD ring mesh, so on the outermost ring one colour
  stood for a 950 m cell, and that was the whole of what was left of the
  terrain quilt. Handing the mesh its own sampling rate rather than the pixel's
  took the change from one cell to the next from 0.0862 to 0.0815 against
  0.0614 for the true cell averages, and that was as far as a *rate* could take
  it: what is left is the 380 m mottle and the 290 m woodland field, which no
  rate fades and which must not be faded — they are read through nonlinear
  mixes and the woodland field is bimodal, so fading them toward their own
  means lands further from the cell average than point sampling, 0.0851 against
  0.0045. The answer was always going to be per-pixel shading. Both other
  pipelines already had it.

  So the software path has one now, and it is the GPU's function for function:
  `terrain_surface` for the albedo and the material at the pixel's own rate,
  `terrain_detail` for the near-field relief and the blades — which the
  software path simply did not have at all — then `terrain_light` with the
  shadow on the direct half and the occlusion on the ambient one, which is
  exactly the split `FLY_ENV_TERRAIN_FS` makes. `raster_tri_ground` is that
  stage, and `per_pixel` now means what it means on the GPU: the rate the
  *shadow map* is sampled at, not the rate the ground is shaded at.

  Three things stay per vertex, and the same three do on the GPU: the cloud
  shadow, the crevice occlusion, and the aerial perspective. The first two turn
  slowly across a cell. The third is a Chapman integral and a cloud march, and
  re-marching it per pixel would cost more than everything above it put
  together — which is what the GPU's own vertex stage says about it too.

  **What it buys**, measured off frames rather than off the probe. The near
  field: `render.material`'s relief signal, which was gated to the GPU because
  the software path had nothing to measure, now reads 16.1 at 40 m against 15.7
  at 160 m with the framing matched — the same claim, both backends, no
  exemption. The far field: over a band six to sixteen kilometres out, the hue
  curvature at a four-pixel arm is 0.00422 against the path tracer's 0.00574,
  where the per-vertex path reads 0.00200. The tracer is the reference because
  it has shaded the ground per pixel since it existed; the question the gate
  asks is which of the two rasterizer frames agrees with it, and the answer
  moved from 0.35 of it to 0.74.

  Four paths a pixel for that reference and not one: a single sample's own
  Monte Carlo noise is curvature too, and it inflates the number it is being
  compared against — 0.0072 at one sample, 0.0067 at two, 0.0057 at four, while
  neither raster frame moves at all.

  **What it costs** is 24% of an aerial frame at the HIGH rung — 444 ms against
  357, 960x540, interleaved, minimum of six runs of four frames each — and
  nothing at all on the budget rung, which keeps the per-vertex path. That
  split is `fly__lod.ground_px`, and it is the same rule the `sky` field
  already states: LOW is a frame budget on one CPU core and a per-pixel
  `terrain_surface` is the most expensive thing that could be put in front of
  it, while the default rung is the reference — what the GPU pipeline is held
  to and what the gallery is shot at. Measured at LOW: 207 ms against 204,
  inside the noise, with the ground path byte-for-byte the one it had.

  One thing was found by that measurement rather than by reading. The shadow
  lookup at the vertex was being taken on the near rings too, where the pixel
  loop takes its own — the condition had grown a second clause it did not need
  — and it cost the budget rung 20% for a number nothing read.

- **The ice sheet cast nothing, and the deck read the same from above as from
  below.** Two entries in the renderer's TODO, and one job: both are claims
  about which face of a cloud you are looking at, and both move the deck's
  brightness, which had 16% of headroom over the bound `render.atmosphere`
  holds it to.

  **The cast.** `cloud_shadow` was the deck's alone, so seven kilometres of
  cirrus dimmed neither the ground under it nor the deck below it — and going
  soft is the whole visible effect a cirrus veil has on a landscape. The sheet
  is a plane, so its shadow is one crossing and one lookup with the shape the
  deck's shadow already had. What had to be decided was how the two compose,
  and it is not a product.

  Each layer's strength is a claim of its own. The deck's 0.62 is not a
  transmittance — a thick water cloud passes essentially no direct beam, and
  the 0.38 a full deck leaves is the deck's own base standing in for one. The
  ice's is derived: `FLY_CIRRUS_TAU` removes `1 - exp(-0.62)` = 0.462 of the
  beam and 0.62 of that is lost to the ground, the rest arriving as the forward
  scattering that makes a cirrus sky bright around the sun, so 0.286 — a full
  sheet leaves 0.71 of the sun. Measured over 1600 ground points on seed 4242:
  0.856 of the sun under the sheet against 0.9995 with nothing over it, worst
  0.714, which is a sun that has gone soft rather than one that has gone out.

  Multiplying the two visibilities would take a further 29% off that 0.38, and
  that would be counting the same sheet twice: the deck's own light is dimmed
  by the ice where the ice is, and the sky irradiance the ambient half reads is
  sampled through both layers. So the ice shades the ground the deck left
  clear — `1 - cd*0.62 - ci*0.286*(1 - cd)` — which is smooth, reduces to
  either layer alone, and sits above the product everywhere, by 0.108 at the
  worst point where both are on the beam. `fly_render_cloud_sun_probe` reports
  the composition and each layer's cover separately, because in a frame a cloud
  shadow and a hillside in shade are the same pixel.

  **The deck under it.** The beam that lights the cloud top crosses the sheet
  first, and now takes its shadow: `cirrus_shadow` at the deck's own lit top,
  one number a frame at the camera's column. That is not a shortcut for this
  term — `cloud_lit` and `cloud_dark` are already one pair of colours a frame
  taken at exactly that point — but it does stop short of shadow *bands* laid
  across the cloud tops, which is what a per-tap lookup buys. That version was
  built and measured: five field lookups a tap instead of four, **+28% on the
  worst sky frame in the game** (570 ms against 447, interleaved, minimum of
  five runs each, on a camera pointed at the horizon so every sky pixel is a
  grazing one), against the twenty per cent the whole ice layer cost to add.
  Not worth four times the price of the thing it is decorating; left in TODO.
  As shipped the pair costs nothing measurable — 444 ms against 463 for the
  same frame, and 691 ms against 694 for the traced path, both inside the
  noise on this host.

  **The two faces.** The march's colour was `lerp(cloud_lit, cloud_dark,
  cs*0.85 + dens*0.25)`, and neither term knew where in the layer the sample
  sat, so every tap of a column carried its base's shading. An aeroplane above
  the deck read the same number as a pilot underneath it — at night, where
  `cloud_lit` is the moon on the top and `cloud_dark` the eighteen per cent of
  it that leaks through to the base, it looked straight down at a moonlit crown
  and drew the shaded base.

  The pair of colours the lighting already computes *is* the two faces, so what
  the shading term really asks is how much cloud lies between the sample and
  the face the light is on — and that is how near the crown it sits. Keyed on
  that, the front-to-back march delivers the rest: whichever face the eye is on
  is the face its first taps land on, and nothing has to be told which. Under a
  night overcast, over nine places and 24 bearings each, the crown reads 0.00473
  against the base's 0.00343 — 1.38x, where the same measurement with the term
  out is 1.05x. Bounded above as well as below, because a crown with the shading
  taken right off is a white lid: two thirds off is what leaves the towers their
  shape, and over a broken deck the view down onto it still varies by 37% of its
  own mean from bearing to bearing.

  Two things in it are measurements rather than choices, and both were found by
  failing a gate that already existed.

  The height is the sample's in the *layer* and not in its own column, which is
  the obvious key and is already computed two lines above as `hh`. The column's
  own top has the local coverage in its denominator, so keying on it puts that
  field into the tap's colour a second time and far harder than `dens * 0.25`
  does: the elevation kink went from 0.54% to 0.77% against a bound of 0.70%, at
  5.4 degrees, where a grazing ray's four taps span the whole slab. On the
  layer's own depth it is 0.63%. What that costs is the shallow column, whose
  crown sits inside the layer and gets less of this than a tower's does — the
  honest answer anyway, since a low top has the towers around it standing over
  it.

  And the ramp holds off until the top half rather than running from the base.
  A linear ramp puts a quarter of the effect at mid-layer, and mid-layer is
  where a camera *inside* the deck sits: `render.atmosphere`'s wash measure —
  a hillside seen through six hundred metres of cloud — went to 0.71 of the low
  camera's spread against a bound of 0.55. The lit skin of a cloud top is
  thinner than that; one optical depth at `FLY_CLOUD_EXT` is 322 m of the slab's
  580.

  **Keying it on `rd.z` is the obvious move and is wrong**, which is worth
  recording because the TODO entry proposed it. The view direction says a camera
  inside the deck looking down is above the layer, so the cloud between it and a
  hillside came out as sunlit crown and the same wash measure read 1.75 — past
  what it measures with the deck taken out of the aerial perspective altogether,
  so the check could no longer tell the renderer from the artifact it exists to
  catch.

- **There was no dusk.** The sky went from a sunset to a flat, azimuthally
  uniform night in about forty minutes of world time, and everything after that
  was one constant. Measured on seed 4242 with the cloud and the disc left out:
  the horizon toward the sun read 2.77 at sunset, 1.17 six degrees under and
  then fell off a cliff — the arch, the ratio of the sky toward where the sun
  went against the sky away from it, was 3.0 at sunset, 1.6 at seven degrees
  under and **exactly 1.00 from eleven degrees under, at every hour after it,
  for ever**. A sky with an arch of 1.00 has no sunset in it. The gallery's
  `tod_dusk` frame was 19:00, which is fifteen degrees under, and it was a
  black night frame carrying the caption "setting sun, warm horizon".

  The cause was one line: `night = smoothstep(-0.14, 0.05, sun_up)`, a schedule
  standing in for a geometry. It says the air stops being lit when the sun is
  eight degrees under, everywhere, at every altitude, in every direction at
  once — and eight degrees under is where *civil* twilight has only just ended.

  What actually ends twilight is the planet's own shadow. It is a cylinder of
  the planet's radius cast down-sun, so at any moment it has swallowed
  everything below one height and nothing above it, and that height is
  `R(1/sqrt(1-c^2) - 1)` — about 3 km at six degrees under, 13 at twelve, 31 at
  eighteen. The air is exponential in height, so the share of a column still
  lit is `exp(-h_shadow/H)`: two thirds, a fifth, a fiftieth. Civil, nautical
  and astronomical twilight, out of geometry, with no table in it.

  `scat_lit` is that share for a ray rather than a column. A point is in shadow
  when it is behind the terminator plane and within R of the shadow's axis;
  along a ray both are quadratics in the parameter, so the shadowed part is one
  interval and the lit column is the whole column less that interval's share of
  it. One extra Chapman evaluation, and only when there is a shadow to find —
  the quadratic has no root in range for the whole of the day, so daylight pays
  one compare. Rayleigh only: the aerosol is under a kilometre and a half, so
  its lit share is all or nothing and a second Chapman pair would buy the same
  answer at twice the price.

  It gives the arch for free, which is the part a schedule can never do. The
  shadow's boundary is further away along a ray pointed at where the sun went
  than along one pointed away from it, so more of that ray is lit — the glow
  stays in the sunset's own quarter and narrows as the night comes up. Measured
  on the same seed and directions: 2.77 / 2.00 / 1.17 / 0.279 / 0.0147 / 0.0067
  at sunset and 3, 6, 12, 18 and 24 degrees under, against an airglow floor of
  0.0073 — monotone, 159 times the floor at the end of civil twilight, and
  arrived at the floor between eighteen and twenty-four degrees, which is where
  astronomical twilight ends. The arch runs 9.2, 7.9 and 8.7 through nautical
  twilight and is 1.17 by twenty-four degrees under.

  Two other things had to move with it.

  Multiple scattering cannot be gated by the same number. It is light that
  arrived from somewhere else, so after sunset the anti-solar half of the sky —
  shadowed along every ray in it — is lit all the same, by the half that is
  not. Gated on the ray's own lit share the eastern sky fell to the airglow
  floor within fifteen minutes of sunset, 56 times darker than it had been.
  It takes the *column's* lit share instead, which is the vertical case of the
  same geometry and has a closed form with no Chapman in it at all: one rsqrt
  and one exp. The airglow rides on the same number, because emission from a
  whole column fills in as the sky goes out, not as one direction of it does.

  And the sun's own column had a floor at -0.14 for the same reason the gate
  did: past that the number could not be seen, so it did not have to be right.
  It can be seen now — twilight *is* the light that has come down a grazing
  column, and how far that column has run is what makes it the colour it is. At
  -0.35 the last of the glow goes deep red and then out on its own
  transmittance (red/blue 2.1 at three degrees under, 4.5 at twelve, 5.9 at
  eighteen) where at -0.14 it stopped deepening and faded out grey. It cannot
  run away below that: `scat_chapman` has no air under the surface, so the
  column saturates at the two grazing halves however far the sun goes.

  **What it costs.** Nothing by day and about six per cent at night, measured
  against a build of the parent commit in the same tree on the same host, each
  aspect run on its own so the suite's own parallelism is not in the number.
  The bench frames are 441.0/441.8 ms raster against 442.4/442.1, 354.3/352.9
  hybrid against 375.3/356.3, and 266.6/262.4 traced against 265.8/286.5 — at
  or inside the run-to-run spread on all three, because the shadow quadratic
  has no root in range while the whole ray is in the sun and the extra Chapman
  is never reached. The night vista is 6896 ms against 6469 and the 19:00 frame
  6910 against 6514: +6.6% and +6.1%, which is what an extra column evaluation
  costs on the rays that actually have a shadow in them, and it is spent on the
  frames that had no dusk in them at all.

  (The full-suite listing shows every frame 8-15% up, and that is the suite
  and not this: the path tracer runs its rows in parallel, so a hundred and
  fifteen frames back to back on a shared box time differently from one. The
  wall clock for the whole run is 20m21s against 20m18s.)

  Nothing in daylight moved. Every figure in `render.atmosphere` above the
  horizon is bit-identical, because `scat_lit` returns exactly 1 while the
  whole ray is in the sun. What did move is `render.planet`: the lit crescent
  at dawn from 600 km went from 4917 px to 5751, which is the twilight arc
  becoming visible on the limb, and the dark side of the ring is unchanged at
  0.0078 against 0.2530 — the light this adds is on the terminator, not on the
  night side. `render.atmosphere` carries the new claims: it fades, it is where
  the sun went, it reddens while the zenith stays blue, and it ends.

- **Stars came out on a clock, and came out early.** The field was weighted by
  `smoothstep(0.06, -0.12, sun.z)`, so it reached full brightness seven degrees
  under — inside civil twilight — and it did it uniformly, drawn at that
  brightness straight across the gold band along the horizon. With the sky
  above given a real twilight this stopped being subtle: a lit orange horizon
  with a full star field over it.

  A star does not come out. It is there all day and the air in front of it is
  brighter than it is, so what the weight should be measured against is the
  sky's own radiance in that very direction — which is already computed, one
  line above, for nothing. `k^2/(k^2 + L^2)` with k at 0.012: squared because a
  ratio linear in the sky leaves a third of the field standing at sunset, and k
  set at the twilight zenith rather than at the night sky. The field is a
  ten-thousandth of the sky at sunset, a two-thousandth at the end of civil
  twilight, a hundredth at the end of nautical, and a quarter — its full self —
  by the end of astronomical. They arrive at the zenith first, where the air
  goes dark first, while the sunset quarter still has none in them, which is
  where a first star really does show. `render.atmosphere` measures it as what
  the field adds over four thousand directions above ten degrees, against the
  sky it is added to; `fly_render_sky_probe` grew a `STARS` option to ask,
  since a speck count in a rendered frame cannot tell a star from a lit
  hilltop.

- **The moon was a wash, not a light, and the ground never got dark.** Night
  terrain came from a radiance of `(0.04, 0.05, 0.08)` added on top of the
  surface's own colour. Two things follow from that and both showed.

  A radiance added after the albedo is light a surface *emits*. It does not go
  through what the surface is made of, so a snowfield and a lava flow came out
  the same colour once the sun was down — the one thing moonlight never does,
  since all it can do is be reflected by whatever is under it.

  And nothing set its size. Shaded ground measured **0.0569 at half past eleven
  at night against 0.0552 at noon**: the same number. The landscape did not get
  dark when the sun went down. It kept its midday shade under a sky nine tenths
  gone, five or six times brighter than the sky above it, which is why every
  night frame in the gallery was a pale plain with stars over it and why no
  night has ever had a horizon in it that had to be looked for.

  The cloud deck had already had this fixed once, and the fix is the same:
  make it an irradiance and put it through the albedo. `FLY_CLOUD_MOON` becomes
  `FLY_MOON` and both surfaces take it, because there is one moon and every
  surface in the world is under it. Shaded ground now reads 0.22 of the sky at
  twenty degrees — under it rather than over it — and 0.066 of its own noon. Daylight is untouched: the weight is `1 - day`,
  which is zero while the sun is up. `render.ambient` holds all three claims,
  and the third is the one an additive wash cannot pass at any size: snow
  against basalt is 8.6x by day and has to stay 8.6x by moon.

- **Two gates were not measuring what they said, and the second cloud layer is
  what showed it.** Neither is a regression from the work above; both had been
  passing on arithmetic for some time and a small change to the sky moved them
  across the line.

  `render.cine`'s propeller disc had the geometry copied out of the model — the
  hub at 6.55 m forward and the disc at a flat 1.9 m in radius. The radius
  stopped being 1.9 m on every airframe when it was sized to the span and
  clipped for ground clearance, and the copy stayed at 1.9, so every sample the
  gate took landed a third of a radius past the tip in clear sky. What it was
  measuring was the sky's own vertical gradient — the reference sat twenty
  pixels higher up the same ray than the sample, on the argument that "a
  gradient in the sky cancels", which it does not over twenty pixels near a
  horizon. It read -0.05 at the root against -0.03 at the rim, a pure gradient
  with the propeller nowhere in it, and `inner > outer * 1.5` passed because
  two negative numbers happened to stay in order. Fixed twice over: the
  geometry comes from `fly_render_prop_disc`, which is the same function
  `draw_plane_model` builds the disc from, and each sample's reference is now
  the sky on its own screen row out past the disc, which is an isocline of the
  gradient. The corrected reading is 0.0224 at the root, 0.0149 at the rim,
  0.0291 worst — and -0.0001 at idle, which is the check on the check, because
  a stationary propeller should darken nothing at all. Root over rim is 1.50
  where the old bound demanded 1.5, and the bound is 1.25 now rather than the
  ratio being talked up: the band runs from 0.70 to 0.98 of the radius, so both
  halves sit where a 1/r profile has already flattened, and the outer half
  carries the painted tip, which is deliberately the most visible part of the
  disc. What it has to separate is a profile denser at the root from the
  inverted one that made the disc read as a wheel, and inverted scores under
  1.0 by construction.

  `render.ambient`'s "dawn skylight is less blue than noon's" is a claim about
  air measured through whatever cloud was in the way. Skylight is the sky that
  is there, cloud included — that is the right definition and it is what lights
  the world — but cloud is grey, so it desaturates the bluest sky hardest,
  which is noon's. The three entries below took the pair from 3.24 and 2.76 to
  3.11 and 2.83 against a bound that wanted ten per cent between them; the ice
  sheet is most of that, since with it alone taken back out they read 3.18 and
  2.78. The ordering is the physical
  claim and it is held on the light itself; the factor moved to the air, over a
  cosine-weighted hemisphere of `fly_render_sky_probe` with the cloud and the
  disc left out — the same two omissions the irradiance projection makes. The
  air's own figure is 3.05 against 3.41, a ratio of 0.894 against a bound of
  0.95, and the older, wider margin was partly weather: at HEAD the deck
  happened to sit over the dawn side and pushed it to 0.852.

- **The night deck was lit like the ground and looked at against the sky.** Its
  colour carried `(0.05, 0.055, 0.085) * (1 - day)` added to both faces — the
  moonlight floor `lit_surface` gives every surface, borrowed on the argument
  that a cloud is a surface too. It is not the same case. The ground is looked
  at against other ground; the deck is looked at against the sky it covers, and
  that sky is 0.0029 at the zenith and 0.0051 at the horizon in the small
  hours. The floor put the night deck at 0.058, twelve times what it was drawn
  over: a moonlit deck under a moonless sky, the two halves of one night
  disagreeing about whether there is a moon in it.

  It is an irradiance on the top now (`FLY_CLOUD_MOON`), folded into the beam
  before the albedo and the transmission split, so it reaches the underside
  through the same `FLY_CLOUD_THRU` the sun does, so the deck's two colours
  stay a lit face and a shaded one at night instead of both taking the same
  additive wash. (Which side of the layer the eye is on is still not in the
  march's colour at all — see TODO.) The magnitude is set against the
  sky rather than by eye: 0.0100/0.0110/0.0170 puts the base at 0.0070 against
  0.0048 of sky behind it, 1.47x where it was 12x. `render.atmosphere` holds
  both ends — under twice the sky, and above zero, because dropping the floor
  out entirely would pass the first bound and leave a black hole in a night sky
  for anyone flying over the deck.

  On its own this is a small correction to one hour of the day. It is here
  because the entry below could not land without it.

- **The deck's opacity is the path through it, not a share of its density.**
  Alpha per tap was `dens * 0.45`, so a ray crossing fifty kilometres of cloud
  sideways accumulated no more than one crossing 580 m straight down — visible
  as horizon cloud you could see the sky through. It is `1 - exp(-dens * seg /
  L)` now, with `FLY_CLOUD_EXT` at 322 m, which is `145 / 0.45`: the number
  that reproduces the old answer for the one ray the old model was calibrated
  on, straight down through the slab, so the deck is the same deck from
  underneath and only the long rays change. One ray along the layer at 08:00
  measures 0.52 over 300 m, 0.95 over 1200 and 0.99 over 4800, where the fixed
  alpha returns the same number for all three.

  The reason this waited on the entry above is that an honest integral makes a
  grazing ray opaque, and opaque times a night colour twelve times the sky is a
  bright torn strip under the stars.

  The reason it needed more than a one-line change is aliasing. The
  exponential's slope in the density is `seg/L` where the fixed alpha's was
  0.45, so at five degrees of elevation it multiplies whatever error the four
  taps already had by six: `render.atmosphere`'s worst elevation kink went from
  0.36% to 2.26% against a bound of 0.7% with nothing else touched. Four taps
  across a nine-kilometre interval sample a 2.6 km field every 2.2 km, which is
  under Nyquist, and it was always under Nyquist — the fixed alpha was simply
  too flat to show it. Three fixes, measured over the four hours the gate
  sweeps:

  - each tap's density is the mean of three stations a third of a segment
    apart, so the interval is walked at twelve uniform points rather than four;
  - the ramp that collapses the field to the deck's mean takes the *segment* as
    its footprint rather than the pixel slide, at `FLY_CLOUD_SEGW` = 0.65 of
    one, a little over twice the station spacing. The ramp that drops the fine
    octaves keeps the pixel slide, because those octaves are what a cloud mass
    is made of across the sky and the deck's own detail gate at eight degrees
    is exactly that measurement. Both footprints on the segment costs 0.00450
    there against a bound of 0.0045; split, it is 0.00459;
  - and the early-out moved from 0.98 to 0.995, worth 0.55% of kink on its own.
    The march returns `ccol/acc`, the mean colour of the taps it took, so
    stopping two per cent short of opaque does not drop two per cent of the
    light — it drops a whole tap out of that mean, at whatever elevation the
    saturation happens to cross.

  Together: worst kink 0.455 / 0.228 / 0.543 / 0.307 per cent against 0.7, and
  the deck's detail 0.0187 / 0.0046 / 0.0031 against bounds of 0.0100 / 0.0045
  / 0.0020. The trade between the two is monotone in `FLY_CLOUD_SEGW` and 0.65
  is where both have room: at 0.5 the kink is 0.71% and at 0.9 the detail is
  0.00450.

  Three density samples a tap is three times the field lookups, which on a
  horizon-filling 480x270 raster frame is 167 ms to 223. Half of that comes
  back for nothing: `cloud_cover_lod` asked `fly_fbm2` for four octaves and
  then asked it again for two, recomputing the first two to get the second
  answer, where the two-octave value is the four-octave walk caught halfway
  through — same taps, same order, bit for bit. Fused (`cloud_fbm_lod`), the
  frame is 195 ms, so the honest integral costs 17% of the worst frame in the
  game and nothing at all of a frame with no cloud on its rays. With the ice
  sheet below on top of it the same frame is 201 ms against the 168 that build
  measures for HEAD — twenty per cent, interleaved, minimum of three runs
  each, on a camera pointed straight at the horizon so that every sky pixel in
  it is a grazing one.

- **A second cloud layer, and the gates could not tell air from cloud.** The
  deck is one layer of water cloud at one altitude, so on a fair day — the
  coverage field mostly under its threshold — what the player flew under was an
  empty gradient. What is missing on those days is the ice seven kilometres up.

  This was built once before and reverted, and the reason was not the layer.
  Both of `render.atmosphere`'s sky claims read `sky_ambient`, which includes
  whatever cloud is in the direction asked, and both assert *clear-air*
  Rayleigh at a fixed eye and hour: the noon zenith blue/red passed at 2.03
  against a bound of 1.9, seven per cent of margin, and it held only because
  the one deck happened to be clear over that spot. A second layer with its own
  coverage put cloud there and the ratio fell to 1.56, and at 08:00 from 4.92
  to 1.25 — the gate correctly measuring a cloud and incorrectly reporting the
  atmosphere. Thinning the layer until the gates passed left nothing worth
  drawing.

  So the probe came first. `FLY_SKY_NOCLOUD` leaves both layers out and
  `fly_render_sky_probe` exposes it — alongside the `NOSUN` the irradiance
  projection already used — and the two Rayleigh claims ask for the air on its
  own. Nothing in the renderer uses the flag: `sky_ambient`
  still means the sky that is there, cloud included, because that is what
  lights the world. `render.atmosphere` holds the probe to being the same sky
  minus the layers and nothing else — bit-identical where neither layer is on
  the ray, 0.39 apart where one is.

  The sheet itself is a plane, not a slab: at seven kilometres a cirrus deck's
  own depth is inside a pixel from anywhere the game is flown, so a march would
  be four taps spent on nothing. One intersection, one field lookup, and
  Beer-Lambert over `1/|rd.z|` of the vertical crossing, capped at fourteen
  because past four degrees the geometry says a hundred kilometres of sheet and
  a cloud is not a plane. It is stretched along a bearing the world is born
  with — four times longer along it than across — and drifts along that
  bearing. Using `e.wind` as the frame instead is the obvious move and is
  wrong: the wind is the wind at the aeroplane and it turns, so it does not
  advect the sheet, it rotates the whole sky, and in calm air it is flatly zero
  and lays every band in every calm world along the world's own x axis.

  Its albedo is 0.55 against the deck's 0.92 and it has one colour rather than
  a lit/dark pair, because a sheet that thin has no shaded underside — over a
  lattice at noon the ice measures 0.71 against the deck's 0.83, so it reads as
  a veil the sky comes through rather than as a second deck.

  The elevation kink was the other half of the old revert: a sheet that high is
  near edge-on over most of the sky, so its tap slides further per pixel than
  the deck's ever does, and it read 38-51% of the mean against a 0.7% bound
  before the LOD fade was tuned and 4.4-4.9% after. It does not read at all
  now, and the reason is that the ramps are the deck's two at this field's own
  scale and the haze fade does the rest: at three and a half degrees the sheet
  is 114 km away and `exp(-t/38000)` has already taken it to a twentieth. The
  deck's own gate is unmoved and the band the sheet is actually in, ten to
  fifty degrees, measures 0.099% against the same 0.7%.

  What it did not do, when it went in, was cast; that is the entry at the top of
  this log.

- **The per-vertex terrain rate was the pixel's, and the clamp that should have
  said so never fired.** The CPU rasterizer resolves one colour per vertex, so
  on the outermost ring one colour stands for a 950 m cell — but the rate it
  handed `terrain_surface` was `pxscale/dist`, which at twelve kilometres is
  fifty times finer than the mesh. The ring was therefore given the 340 m track
  field and the projection-locked grain and asked to carry them at 950 m
  spacing, which is not detail, it is a different random number per cell.
  `terrain_ring` had the bound written down as `min(px, dist/step)` and it had
  never once applied, because `dist/step` is a *count* where `px` is a rate.
  One sample every `step` metres is the rate; with that in the comparison the
  clamp binds past about a kilometre and the octaves the mesh cannot carry are
  faded rather than point-sampled.

  Measured over 950 m cells at the distance that ring is drawn: the change from
  one cell to the next is 0.0614 for the true cell averages, 0.0862 for the
  pixel rate and 0.0815 for the mesh rate. That is the whole of the
  improvement, and the residual is worth being exact about rather than
  claiming. Most of what is left is the 380 m mottle and the 290 m woodland
  field, which no rate fades and which must not be faded: they are read through
  nonlinear mixes and the woodland field is bimodal, so fading them toward
  their own global means lands *further* from the cell average than point
  sampling — 0.0851 against 0.0045, measured. What would carry them honestly is
  per-pixel shading, which is what both other pipelines already do.

  **The ladder was looked at and left alone.** Once the clamp binds, the near
  field stops carrying the 90 m patch field, because 26 m cells give it three
  and a half samples a wavelength where `detail_amount` wants five. A fifth
  ring at 9 m brings it back and reaches 216 m; at any altitude the game is
  flown at, 216 m of ground is the bottom edge of the frame, and an A/B pair
  from the same camera at 150 m came out indistinguishable. Two and a half
  thousand vertices for the strip under the nose is not the trade.

- **The wood casts, out to the horizon.** `draw_foliage` puts the scatter's real
  crowns through the shadow caster inside the near cascade's footprint and
  nothing at all beyond it — the whole 2.6 km scatter is far too much geometry
  to rasterize three times a frame — so from about 130 m out to the horizon a
  wood cast nothing and was lit as though it were transparent. That is most of
  every frame flown over land, and it is why a wooded landscape read as models
  on a lawn: not that the floor was the wrong colour, but that no wood laid a
  shadow anywhere.

  What stands in for the geometry out there is the wood as a medium — one
  transmittance for how much sun a stand lets past, in `canopy_sun`. The tap is
  taken where the sun ray crosses the crown layer rather than overhead, which
  costs nothing and is the directional half of the effect: an edge throws a
  fringe onto the meadow beside it, and the offset grows from metres at noon to
  tens of metres at dusk, the way a tree's shadow does. It is tapered by height
  above ground, because every primitive the rasterizer shades comes through
  here and a crown lit by the shade of the floor it grows out of is the effect
  upside down.

  `FLY_CANOPY_K` is measured. The crowns are cast into the near cascade and
  nowhere else, so their own answer over 220 points of closed canopy — divided
  by the same points with the foliage caster switched off, to take the terrain's
  share out of it — is what the medium has to reproduce, and over seven sun
  elevations on seed 4242 that is 0.84 of the sun. `render.shadow` now measures
  the same ground both ways with only the camera moved: 0.847 under the crowns,
  0.866 under the medium, against 0.999 on matched open ground beside it, where
  before the last two were the same number.

  **Casting it as a solid lid on the caster grid is a dead end, and it is the
  obvious move.** A shadow map answers in one bit — the receiver is behind the
  caster or it is not — so a lid at any height the depth bias can resolve puts
  the whole wood floor in full shade. Against the crowns' 0.84 it measures 0.00,
  and a height sweep of 3.0 / 4.5 / 5.8 / 7.0 / 9.3 m reads 0.72 / 0.11 / 0.00 /
  0.00 / 0.00. There is no value in it, only a cliff, and the heights that do
  better than the cliff are the ones thin enough for the normal offset to leak
  through — which is tuning a canopy against the noise floor of a depth bias.
  The whole build is in the git history; what is left of it in the tree is
  `fly_world_canopy`, which the transmittance wanted anyway.

  Two things moved out of the way for it. `fly_world_forest` gained
  `forest_open`, the field without the slope cut and without the second
  heightfield sample that cut costs, because the canopy is evaluated per pixel
  and cannot afford it. And `env_ground_uniforms` takes its location-cull reach
  from the caller: the canopy carries the settlement clearing, which reaches
  `FLY_CLEARING_REACH` — five times a pad apron — and culling to the apron's
  reach hands the shader a site list with the clearing missing, which grows a
  wood across the airfield.

  `render.shadow`'s three sample patches were re-sited while this was in.
  Two of the three stood in closed canopy: the woodland field grew denser after
  they were chosen, so the acne loop was counting the wood and saying nothing
  about the depth bias it exists to measure.

- **A settlement occludes the sky.** The traced occlusion term gathered trees
  and marched the heightfield, and that was all of it. A hangar wall, a raised
  pad deck, a tank farm and a sixty-metre tower occluded nothing whatever, so
  the ground at the foot of a building took the same open sky as the meadow
  outside the perimeter, and the ground under a deck took the same as the apron
  beside it. That is the difference between structures standing on the ground
  and structures placed on it, and on the overcast afternoon most of the gallery
  is shot on — where there is barely a sun to cast and the ambient half is the
  picture — it is the only difference there is.

  It goes in as a column grid rather than a list of shapes. The question the
  ambient integral asks is "how high does something stand over there", which is
  what it already asks of the terrain, so the answer takes the same form and
  rides the same march: a tower is a tall column, a fence is a short one.

  The grid is filled by *drawing the site*. `draw_location` is the only thing
  that knows where a settlement puts its buildings, and a second description of
  that would be wrong within a release — so the capture rides the shadow
  caster's own seam, and every rule about what may cast a shadow comes with it
  for free: decals and markings occlude nothing, glass occludes nothing, a prop
  disc occludes nothing, and the near-field detail gates are already bypassed
  for a caster, so what is captured cannot depend on where the camera was. It is
  cached per site and makes its own view and env at the top graphics rung,
  because the cache it feeds is world-space and persistent: a site captured at
  one quality and read back at another would put the frame's settings into a
  cache that outlives the frame.

  **The ground channel only, and the measurement that decided it.** The
  window's upper channel is traced sixteen metres up and its contract is that it
  is terrain occlusion and nothing else — which is what makes it the right
  answer for a surface standing above the ground rather than lying on it, and a
  facade is exactly that surface. Sixteen metres over a tower's footprint is
  *inside the tower*, so columns on that channel put every wall in its own
  shadow: `render.facade` measured one tone going from a ninth of the city's
  glazing and cladding to 56.5% of it, which is a black building. What a wall is
  occluded by is the next building along, and that is a question a field over
  the ground, read at one point, cannot be asked.

  `ao_begin` is split so the ring fill runs after the sites are captured — a
  cell traced before them caches a sky it does not have, and the cache outlives
  the frame. `render.occlusion` measures the apron against itself: 0.135 of the
  sky where something is built over the ground against 0.935 in the open, where
  before the two were the same number.

- **The cloud deck has tops.** It was two parallel planes 580 m apart with one
  triangular profile between them, everywhere. Coverage decided how opaque a
  column was and nothing at all decided how tall it was, so every cloud in the
  world topped out at the same altitude — a lid rather than a sky. From
  underneath that is only a little wrong; at cloud height, where this game is
  flown, it is the difference between weather and a ceiling.

  The base stays flat, and that is the model rather than a simplification: a
  cumulus field has one base, because it is the height the air rising through it
  reaches saturation, and the flat bottoms in a photograph of one are lined up.
  The tops vary, with the same coverage the tap has already paid for, so the
  whole of the deck's third dimension costs two multiplies. The profile is in
  units of the column's own depth — a sheet and a tower are the same shape at
  two sizes rather than a tower and the bottom third of one — with a firm base
  and a soft crown, where the old symmetric triangle faded the underside out
  over as many metres as the top. The underside is the one edge of a cloud that
  really is an edge.

  **Per tap, not per ray, and this is the whole of it.** Resolving the ceiling
  once for a ray and applying it down the ray's length is a step in *elevation*:
  it lands in the same place on every ray that shares it and draws a horizontal
  line across the sky. Measured at 1.9% of the mean on the kink
  `render.atmosphere` watches for, against a bound of 0.7%. It also cost the
  detail it was supposed to add — the four-degree ring fell from 0.01060 to
  0.00753 — because clipping the march to one ceiling throws away the structure
  the ray crosses. Taking it per tap costs nothing extra, has no discontinuity
  to draw, and reads 0.01060.

  Tops now measure a mean of 2634 m with a standard deviation of 81 m over a
  range of 2500 to 2900, correlating with the column's own depth at 0.99, while
  the base under the tallest column and under the shallowest is 2340 m in both.
  A plane measures a standard deviation of zero.

- **The wood is an instance lattice now, and the near-field object build is a
  third smaller than it was.** A crown is the same shape wherever it stands:
  what differs between two trees is where they are, how big they are, what
  colour their leaves are and which way round they are pointing. The capture
  path was sending the GPU every vertex of every one of them, shaded, at
  fourteen floats a vertex — and the shading is the part a geometry cache could
  never have held, because the canopy BRDF's transmission and its down-sun surge
  are functions of the view direction. So the crowns go down the way the terrain
  rings do: one static mesh, instanced, lifted and shaded in the vertex and
  fragment stages.

  Closed canopy at 1280x720, the frame `render.cost` prints, minimum of five:

  | | before | after |
  | --- | --- | --- |
  | objects phase | 61.6 ms | 40.7 ms |
  | vertices built on the CPU | 276,213 | 91,761 |
  | bytes crossing the bus | 15.5 MB | 5.1 MB + 450 KB |

  The 450 KB is the instance lists: 11,245 trees at ten floats each. A
  34% cut in the phase and a 64% cut in the upload, and on the alpine vista the
  build goes 65,613 vertices to 31,221.

  **The template is captured from the model rather than written again**, which
  is the whole reason this was small enough to be worth doing. `draw_tree_model`
  runs unchanged at unit size on the origin with a white leaf colour, and
  `tri_leaf` — whose arguments already *are* a position, a shading hub, an
  albedo and an occlusion term — appends to a template buffer instead of
  drawing. There is one tree model in this renderer, not two, and no parity to
  keep between them. The same trick gives the bole its own pass: `draw_tree_bole`
  is the model again with the leaves switched off.

  Per-tree variety survives, which is the thing that would have made this a
  regression. Eight shapes are baked per species and level of detail from eight
  hashes, so `crown_pt`'s jitter, the lean and `leaf_face_tint`'s per-face
  values are eight distinct trees rather than one; and the vertex stage turns
  each instance about its own axis by an angle off the tree's own hash, which is
  a continuum on top of that. Identical crowns pointing the same few ways is the
  failure this changelog already records fixing once; `world.canopy` measures
  the thing that actually divides a canopy from a texture — tonal spread across
  the crowns, where flat shading collapses to 195 — and it goes **287 to 339**,
  because the shading is per fragment now instead of interpolated across a
  facet.

  The exact check is in `render.cost`, and it is an identity rather than a
  tolerance: the software rasterizer builds every crown and the GPU builds none,
  so `cpu_verts == verts + instanced`, to the vertex. A tree's triangle count is
  a function of its species and its level of detail and of nothing else, so the
  same wood costs the same geometry either way — 31,221 + 34,392 = 65,613 on the
  alpine frame. Nothing else in the frame changed: the scatter walk's frustum
  test, its road verge, its species roll and its fade all still run, the boles
  and the scrub and the ambient skirt still go through the ordinary capture, and
  the shadow cascade still replays the CPU model, because a cast shadow is a
  silhouette and the near cascade's footprint is small.

  What is left un-instanced, and why: the boles are three triangles of thirty
  and they shade through `tri_lit_n`'s specular block, which has no GLSL twin —
  moving them would buy a tenth of the geometry for a new pair of functions to
  keep in step. That block is still carried in `TODO.md` on its own account.

  It is an optimisation with a fallback all the way down. No GL driver, or
  either program failing to build, and `g_wood_collect` is never set and the
  model draws exactly as it did; the software rasterizer and both traced modes
  never take the path at all.


- **The GPU path tracer had been dead, and the fallback hid it.** `FLY_PT_FS`
  declared its own list of uniforms rather than the shared
  `FLY_GLSL_ENV_UNIFORMS` block, and a hand-written list goes stale: `uWet` was
  added to the shading model, `FLY_GLSL_SHADE` started reading it, and the
  program stopped compiling with `'uWet' undeclared`. Nothing said so.
  `pt_environment_gpu` returns -1 on a program that will not build and the CPU
  tracer is the documented fallback, so every traced frame in the project —
  path-traced and hybrid, in the gallery and in the suite — has quietly been the
  reference implementation ever since, on every host with a GL driver.

  Two things it cost beyond the frame time. The gallery's "Path traced, GPU
  backend" plate was the CPU frame under a second name, which is a caption that
  is not true and which `write_manifest`'s no-frame-twice rule failed on the
  moment a GL driver was present. And `gpu.parity`'s traced coverage was
  comparing the CPU tracer against itself.

  The fix is the block, not the missing line: the tracer is one of the
  environment programs and the block exists so they cannot drift apart. On
  llvmpipe at 960x540 the traced coast comes back in 4.0 s against the CPU
  tracer's 7.3, and the two frames agree — the far-field speckle at grazing
  angles is the only visible difference.


- **Cloud was a backdrop, not a medium.** The slab was marched for sky rays and
  for nothing else, so with the camera inside the deck a hillside came out
  crisp against a grey wash — the one thing being inside cloud does not look
  like. It is part of what stands between the eye and a surface, and the fix is
  where it stops: `cloud_slab` now takes the end of the ray, so a sky ray passes
  the far clamp and a surface ray passes the surface, and it folds into
  `fog_pair` the way the air does — what covers the path attenuates the surface,
  what the cloud is lit with is added on. Callers that cross-fade a lit and a
  dark colour through the pair stay exact, because both go through the same T
  and the same s.

  The march itself is unchanged and is now written once instead of twice. What
  did not change with it is the fixed alpha per tap: Beer-Lambert over the step
  is the more honest integral and makes a grazing ray opaque, which is true of
  real cloud, but the deck's night colour floor is well above a moonless sky, so
  opaque horizon cloud comes out as a bright torn strip under the stars. That is
  still a lighting job and still open.

  It costs nothing where there is no cloud on the segment, which is most rays
  most of the time — a ray from a camera below the deck to a hillside never
  reaches 2340 m and the interval test returns. What paid for the rest was
  noticing that `terrain_ring` ran the whole aerial-perspective integral twice
  per vertex, once for the lit colour and once for the dark one, when the two
  differ only in the sun term and travel the same air: one `fog_pair` for the
  pair. Best of three interleaved, llvmpipe: the alpine frame 194.1 -> 192.0 ms
  with its terrain phase 107.0 -> 102.2, and the closed-canopy object phase
  192.2 -> 200.9.

  Gated in `render.atmosphere` by two cameras over one hillside, both looking at
  the same point from the same slant range so the air between them and it is the
  same length and the same air: one just under the deck's base and one in the
  middle of the layer. The contrast the ground still has — the spread of linear
  radiance over the terrain pixels, off the render rather than off the tonemap —
  goes 0.0300 to 0.0130, a ratio of 0.43. The low camera is the control and it
  is doing real work: with the deck taken back out of the aerial perspective the
  same pair measures 0.68, because six hundred metres of ordinary air flattens a
  hillside on its own and a bound on the high frame alone would pass on a
  renderer that had simply gone foggy.

- **Only the terrain read the traced occlusion, and the window is a field over
  the ground.** Everything else in the world stands up out of it. A crown at
  canopy height sees most of the sky the floor beneath it cannot, a tower's
  parapet sees all of it, and one number indexed by ground position gives a
  spruce the shade of the wood it grew out of — which is the same mistake as
  giving the floor open sky, pointed the other way. On the GPU it was worse
  than approximate: `FLY_ENV_OBJ_FS` mixed the lit and dark colours and read
  the window not at all, so a tree in a closed stand was lit as if it stood in
  a field, and the two backends quietly disagreed about a wood.

  The window now holds two numbers per lattice point: what the ground sees, and
  what a point sixteen metres above it sees. Sixteen because that clears the
  tallest crown the scatter builds — a 15.5 m spruce tops out at 16.3 — so the
  upper channel is terrain occlusion and nothing else: a valley wall still
  shades a crown and the wood no longer does. Both are traced through the same
  cosine lobe about the *terrain* normal, so the pair is one question asked
  twice at two heights and the difference between them is exactly what stands
  in between.

  Between the two it is linear in height and clamped above, and that is a
  decision rather than a model. Visibility through a canopy really falls off
  something closer to exponentially and two traced samples cannot tell which;
  two honest ends and a straight line beats a curve fitted to nothing. The
  scalar the window fades to outside its radius lifts with height on the same
  ramp, or a crown at the window's edge would go back to reading the floor's
  darkening the moment the trace faded.

  The upper channel costs a quarter of the lower one's rays, and that is not a
  saving taken carelessly: from above the canopy the integrand is open sky over
  most of the hemisphere with a terrain horizon under it, where the floor's
  answer is decided by which gaps between which crowns a ray happens to find.

  Two things had to be arranged for the read to be affordable. Objects carry
  their height above ground as a vertex attribute, so the GPU's object stage
  reads the same window at the same height with the same four taps as the CPU
  — one shared `fly_ao_at` now, in `FLY_ENV_AO`, rather than a copy in the
  terrain shader — and both channels ride in one RG32F, because unit 3 was the
  only free texture unit. And the height itself is read off the window's own
  4 m lattice and interpolated rather than evaluated under each triangle: the
  heightfield memo is keyed on exact coordinates *deliberately*, so a lookup
  under ninety thousand distinct centroids misses every single time. Measured
  at 484 ms on the closed-canopy object build against 195 for the four lattice
  taps a whole tree shares.

  Cost, best of three interleaved, llvmpipe: the alpine frame 176.5 -> 188.3 ms
  (+7%) and the closed-canopy object phase 160.7 -> 195.1 (+21%).

  `render.occlusion` gains the claim: over closed canopy, sky visibility reads
  0.536 on the floor, 0.681 at five metres, 0.826 at ten and 1.000 at sixteen —
  rising, and rising *gradually*, because a two-valued field read as a step
  would put a hard line across every trunk at canopy height. Over open ground
  the two ends agree to within 0.000, since there is nothing between them to
  occlude anything and a difference there would be the tracer disagreeing with
  itself.

- **The penumbra was a constant width, and a shadow edge is a distance.** The
  PCF kernel was `pcf[c]` texels wide wherever it was read, so a tower's shadow
  was the same crisp line at its foot and at its tip two hundred metres out.
  What the width answers to is the gap between blocker and receiver, and the
  depth map already held it: `dot(P, fwd)` is measured along the light's own
  travel, so the difference between the receiver's and the caster's is exactly
  the distance the sun's disc gets to spread over. `sm_penumbra` is that gap
  times `FLY_SM_SUN_TAN`, which is the *tracer's* disc — `sun_jitter`'s 0.028,
  now shared — so the rasterizer's soft edge and the path tracer's are one
  light rather than two guesses that happen to look alike.

  Three things had to be got right, and each was found by measurement rather
  than by argument.

  *The kernel grows in count, not in spacing.* Spreading a fixed thirty-six
  taps over a four-metre box quantises visibility into thirty-seven levels, and
  because every tap crosses a texel boundary at nearly the same moment they
  cross together — so the ground contours, and the gate read the *steepest*
  step in a scan as the edge. An aeroplane's shadow from 150 m measured
  **sharper** than the same shadow from 16 m, which is the artifact reporting
  itself. Contiguous taps with the box's overlap as their weight instead: at
  `rad == pcf[c]` that derives the fixed filter's fractional end weights rather
  than special-casing them, and the radius is a real number, so the kernel
  fades one row in as it fades the next out.

  *The widening tapers, the base kernel does not.* The search that sizes the
  kernel cannot be as sensitive as the kernel itself without costing exactly as
  much, so a caster crossing the kernel's edge changed the answer by a whole
  tap at once: a clean seven-per-cent rim, one sample wide, all the way round
  every soft shadow. Tapering the outermost texel to nothing fixes it — a
  caster arrives at zero weight and grows. Tapering the *whole* kernel is a
  tent, and a tent is materially tighter than the box of the same support: it
  took `render.shadow`'s lattice-stability figure from 0.0033 to 0.0045 against
  a 0.0040 bound, which is sharper edges handed back as crawl. So the shoulder
  is one texel at the outside and zero wide at the base radius, where the far
  cascade's four taps of bilinear would degenerate to nearest-tap under any
  taper at all.

  *The search is the kernel, plus a ring.* Every tap the base filter takes is
  already a depth compared against the receiver, so the blocker mean falls out
  of it for nothing; what it cannot see is a caster standing just beyond it,
  which is the receiver in the *outer* half of a wide penumbra. Eight taps on a
  circle at the widest kernel's own outer reach cover that, and sit exactly
  where the taper is zero so the hand-over has no step in it. Only a receiver
  that turns out to have something distant over it pays for the second, wider
  kernel.

  Cost, best of three interleaved, llvmpipe: the alpine frame 213.4 -> 227.4 ms
  (+6.6%, terrain +8.5%), and the 1280x720 closed-canopy object phase 160.9 ->
  209.5 ms (+30%) — a wood is the worst case, because it is the one scene where
  every pixel is a shadow lookup and none of them widen. Half of that was the
  weights: the column's are the same down every row of the box, and hoisting
  them out took the canopy figure from 267 to 209.

  Gated by a new positive claim in `render.shadow`. The aeroplane is the only
  caster whose height is ours to set, so it is flown up a ladder of four
  altitudes over one patch of flat ground with the camera pinned, and the outer
  ramp — nine tenths lit to half lit, walked *outward* from the darkest sample
  — has to grow at every rung: 0.96, 1.15, 2.53, 2.65 m. Under an orthographic
  sun the silhouette is identical at all four heights, which was checked by
  forcing the radius to a constant and getting 0.96 four times over. The ladder
  stays inside the band where the width is neither floored at the base kernel
  nor capped by the tap budget; both ends are deliberate and both would flatten
  the measurement into a constant, which is the thing being tested for.

- **The world was dry, flat and checkered, and three of those had one cause.**
  Four surfaces in this renderer were described by a single constant each, and
  each of them is a surface a viewer knows well enough to catch it.

  *Windows were drawn with `raster_tri_view`* — a constant colour, straight
  into the framebuffer, with no lighting of any kind. Glass is the most
  view-dependent surface a settlement has, so a constant made every tower a
  black-and-white checkerboard that stayed exactly as bright in shadow, at
  dusk, in a storm and head-on into the sun. Glazing is shaded now, and the
  facade is one of three constructions — curtain wall, precast panel, profiled
  cladding — chosen off the building's own seed. The contrast was the defect
  rather than the grid: frames, mullions and joints are the wall's own colour
  moved a few per cent, which is what they are in life. `render.facade` bounds
  how much of the built surface any one 8-bit tone may hold.

  *Pavement* — the pad deck, the hardstands, the tracks and the roads — was one
  colour and one specular level everywhere and in every weather. `pave_surface`
  gives it the lands it was laid in and the aggregate showing through, the deck
  is drawn in four bands with a rubbered touchdown circle and sealed joints
  rather than as one triangle from the rim to the centre, and roads carry a
  hard shoulder inside the kerb line. `render.paving` holds the tonal spread
  and the wet response.

  *Rain never reached the ground.* Precipitation was spent on streaks across
  the lens and nothing else, so it rained on a landscape lit and surfaced as it
  was at noon in high summer. `fly__env.wet` now darkens albedo and tightens the
  lobe on ground and pavement alike, split on vegetation because a blade sheds
  the film and soil holds it, with puddles on ground flat enough to hold them.

  *And open ground was a green.* Under the grass species there is now a network
  of worn-earth tracks — ridged noise, whose level sets are lines rather than
  islands, which is the one thing a patch field cannot make — running to soil
  in the middle and a gravel shoulder at the edges. Every new layer fades toward
  its own plane mean, so `render.material`'s coarse-cell bound *improved*
  (0.0048 against a point sample's 0.0140, where it was 0.0056 against 0.0202).

  *And then rain made the ground look like plastic.* The first wet model got
  its physics backwards in a way that only shows in a frame, and the `loc_city`
  still is where it showed: a meadow in a shower rendered as vacuum-formed
  green. Two constants, both of them a guess at what "shinier" means.

  It lerped reflectance to 0.048, which is F0 for n = 1.56 — the Fresnel of
  polished plastic — where a water film is n = 1.33 and F0 = 0.02, so wet
  ground was given roughly twice the reflectance it should have and in the
  wrong direction. And it lerped the Blinn exponent to 85 for *every* material,
  vegetation included, though the darkening on the line above it had always
  known to split on vegetation. At 85 the GGX lobe is tight enough to resolve
  the 7.4 m hummocks `terrain_detail` perturbs the normal with — a bump
  authored to break up a matte diffuse, rendered as a specular relief map — and
  the meadow came out embossed. Between them they made a rained-on meadow shade
  **1.24x brighter** than the same meadow dry, off an albedo 0.78x as bright:
  the specular the smoothing added was worth more than the albedo the film took
  away, so the one thing rain has to do to a landscape, it did not do. It is
  0.88x now, and bare ground, which really is a smooth dielectric under a film,
  is unmoved at 0.68x.

  The puddles were a casualty of the same error rather than a second one. Their
  albedo was deliberately held back from water's, on the grounds that taking it
  all the way down gave black stains that read as scorch marks — true, and not
  the puddles' fault: the meadow around them was a quarter brighter than dry
  ground, so anything as dark as water actually is read as a hole burnt in it.
  With the ground round them shading darker than dry, a puddle can be as dark
  as a puddle. They are also level now. `terrain_detail` takes the whole
  material rather than just its vegetation, and drops the hummock and tussock
  relief on anything the exponent calls a film — the amplitude used to *rise*
  on standing water, because `rough` keys on vegetation and a pool takes the
  vegetation to nearly nothing, so puddles were the most strongly embossed
  surface in the frame. `render.material` now measures the lit radiance and not
  only the albedo, the canopy/bare split in the exponent, and the tilt a
  puddle is allowed (none).

  All four have GLSL twins in `fly_glsl.h`, which is the part that has to be
  remembered: the CPU rasterizer is the reference and the GPU path is what
  ships, and a layer added to one and not the other is a divergence that no
  frame on a machine without EGL can show.

- **A wood was a grid of dots, and the spacing was what the eye was reading.**
  Three things, none of them about the tree model. The crowns were rotated by
  `(h & 3) * 0.4` radians on a face spacing of 0.79 — four poses, not a
  rotation, so a stand was the same shape facing the same few ways. There was
  nothing at the intermediate scale: scrub is a two-ring dome on no trunk
  through the canopy BRDF, thickest where the canopy *breaks* rather than where
  it is closed, priced off the blade layer's rung because that is what it costs
  like. And nothing in the scatter had the ambient contact ring settlements
  have had since they were built, so every tree and boulder met the ground on a
  hard seam.

  The clearing was the fourth: one radius for every kind of settlement, 170 m
  bare thinning to 430, which left a city's outer ring of towers standing in
  quarter-wooded ground with trees across the approach. `fly_world_clearing`
  sizes it per kind and `world.forest` holds it at every bearing over every
  site — nothing wooded inside a perimeter, the wood back at four times it.

- **`render.night`: closed, and the aspect's name was the literal answer.**
  The sky really was showing through the ground, and the mechanism was the
  hybrid blend rather than anything in the march. The rastered terrain rings
  run out around twenty kilometres and `trace_terrain` reaches twenty-six, so
  every MIX frame has a six-kilometre annulus in which the raster half is sky
  and the traced half is ground — and `pt_environment` lerped them together
  regardless. By day that is a slight wash toward the haze nobody would report.
  At night the sky at that range has *stars* in it, and a star is a sub-pixel
  point sample, so one landing in the annulus came through at four tenths
  strength as an isolated blue pixel on a dark slope. The blend now takes
  whichever half found geometry when they disagree, because an average of
  ground and sky is neither.

  **Every clause of the old diagnosis here was wrong, and it is worth saying
  how it went wrong.** It read "absent from the raster path and present
  thirteen times over in the pure path-traced one"; measured pixel by pixel it
  is 0.412 in raster, 0.0626 in traced against a 0.0623 neighbour — present in
  raster and absent from tracing, exactly backwards. It read "the traced half
  stepped over the ridge"; the march returns 21910 m there against 21918 and
  21921 either side, and sixty-four independent bounces through that pixel
  average 0.0630 against neighbours at 0.0624. The continuous depth that looked
  like evidence of a marcher miss was the *tracer's own* depth, written by the
  pass that had already found the ground. Anything revisiting a rendering bug
  should measure the pipelines separately before reasoning about which one is
  at fault; both numbers in the old entry were consistent with a story that was
  not happening.

- **A reversed `clamp` in the terrain march, found on the way and fixed.** The
  step floor grows with distance (`3 + t*0.02`) and the ceiling does not (200),
  so past 9850 m the floor overtook the ceiling and `fly_clampf` was being
  called with `lo > hi` — where it returns the floor. Backwards where it
  mattered most: at 20 km a ray ten metres above the ground, the case wanting
  the finest step, strode 403 m while the ceiling that exists to prevent
  exactly that had stopped applying. Worse in the GLSL twin, where a reversed
  `clamp` range is undefined — `min(max(x,lo),hi)` gives 200 and
  `max(min(x,hi),lo)` gives 403, so the two backends were entitled to disagree
  about where the ground is. Not the cause of the pixel above; a latent bug in
  its own right.

- **`render.planet`: closed, and it was not the multiple-scattering term.**
  That one is already gated by `night` and by the sun's own transmittance and
  goes to nothing across the terminator. What put a floor under the night side
  was the airglow: `(1-night) * (1-exp(-tau*1.6))` times a constant, added to
  the in-scatter and — unlike everything else in that integral — attenuated by
  nothing at all. From the ground it does not matter, because a zenith ray has
  `avg` within a fifth of one. From six hundred kilometres up a limb ray
  saturates `tau` completely and so collected the *full* airglow, the brightest
  the term can be, with nothing taking any of it back: 0.0149 against 0.2530
  toward the sun, or 17x where the aspect wants 20.

  Light emitted or scattered along a path is dimmed by the air between it and
  the eye exactly as the daylight in-scatter above it is, so the term is
  weighted by `avg` now. The dawn limb reads 32x, the lit side is unchanged at
  0.2530, and the ground-level night zenith moves 0.0038 to 0.0035 — eight per
  cent, on the one number that was tuned by eye.

- **Cinematography: closed.** The atmosphere is one Rayleigh/Mie
  single-scattering integral behind sky, sun tint and aerial perspective, with
  shafts as shadowed fog, gated by `render.atmosphere`. Per-biome grading and
  the propeller are in, gated by `render.cine`. Per-time-of-day grading was
  dropped deliberately rather than skipped: the scattering model *is* the time
  of day now, and physically, so a grading curve on top would be fighting it.

  **A marched shaft was measured and rejected.** Three cloud-transmittance taps
  along the segment instead of one cost **+28%** on a raster frame over a wood
  (129.6 ms to 166.2, minimum of three interleaved) to sharpen the edge of a
  beam that is soft by nature. The one-tap version stays. Anything that wants
  to revisit this needs a cheaper cloud field, not more taps.

- **The frame is GPU-resident now, and that is the whole of it.** A raster
  frame is drawn, corrected, composited, exposed, tonemapped and downsampled
  without the radiance leaving GL memory: 260 MB a frame at 1080p/`high` down
  to 19 MB, and nothing that crosses the bus is sized by the render target any
  more, so supersampling is free of it. `fly_gpu_traffic` counts the bytes and
  `gpu.traffic` gates them; `gpu.resident` holds the moved stages against the
  same GL pass read back and finished in C. What the doing found, against what
  the costing above expected:

  - **The emissive splats were not the lynchpin.** The costing assumed a
    depth-tested quad, which tests every fragment where `light_splat` tests the
    splat's centre once — a semantic change, a re-blessed gallery, its own
    commit. None of that was needed. The splat pass samples the *previous*
    frame target's alpha at the centre texel, which is the same point test in
    the same units, so the two backends still agree to 0.01/255 and no
    reference moved. The frame lives in two float targets for exactly this: a
    pass that rewrites the frame writes one and samples the other.
  - **Multisampling took the depth buffer off the table,** which the costing
    did not anticipate. Depth is stored per sample and there is no defined way
    to resolve it into something a later pass can read, so the splats and the
    translucent pass test against the view depth in the frame's alpha instead —
    which is what their C twins compare against anyway.
  - **The shafts had to move first.** They marched the cascades on the CPU, so
    every level that asks for them — the two expensive ones — was forcing the
    shadow map to be rasterized in C as well. Porting the march freed the
    cascades to stay on the GPU at every rung, which was worth more than the
    march itself.
  - **Exposure went to a 1x1 image, not a readback.** The post programs sample
    it, so the frame never stops mid-way to tell the CPU one number.
  - **The supersample resolve folded into the final pass** rather than
    happening after it, which is where the readback shrank from the render
    target to the picture.
  - **`rt->depth` needed no opt-in.** `fly_render_frame` fills the buffers as
    it always did — it reads the resident frame down once everything has been
    laid into it — and `fly_render_frame_aa`, which is what the game and the
    gallery call, never does.

- **The near-field scatter is the wall, and the air in front of it was a
  quarter of the bill.** With the frame resident, the object build is the only
  remaining resolution-independent cost of any size. Measured on the chase view
  over closed canopy at 1280x720 `medium`, which is the frame `render.cost` now
  prints and the one every change to the object path should be measured on —
  the alpine vista it prints above is a long march over bare rock and its
  objects phase is a few per cent of the frame:

  | | was, ms | now, ms |
  | --- | --- | --- |
  | woodland | 89.4 | 53.4 |
  | roads | 11.3 | 9.5 |
  | settlements | 5.6 | 4.8 |
  | rail | 3.4 | 0.7 |
  | blade layer | 1.3 | 0.5 |
  | boulders, convoys, ground items | 0.1 | 0.1 |
  | the phase they add up to | 116.0 | 67.3 |

  That split is one frame each. The headline is the minimum of three, which is
  what `render.cost` prints and what any comparison should use: **88.3 ms
  against 53.5**, a 39% cut. The two protocols disagree by twenty per cent on
  the absolutes and by three points on the ratio, because this is a four-core
  host whose GL driver also runs on the CPU — a single frame's object build
  carries however much of the previous frame's fragment work happened to still
  be in flight. Nothing above the minimum is cost. On the software rasterizer,
  where the fog stays in C and only the sharing and the memo apply, the same
  frame goes 158.4 to 142.6.

  Three things did that, and the first is most of it.

  - **The air moved to the fragment stage.** `fog_tri` was 32 ms of the build —
    a quarter of it, more than roads, settlements, rail, grass and boulders put
    together — because a wood is tens of thousands of small triangles and each
    was buying four Chapman columns and a cloud tap to describe the same air.
    Captured vertex colours are unfogged now and `FLY_ENV_OBJ_FS` applies
    `fly_apply_fog` per pixel, which is what the terrain and the water already
    did. Applying it after the shadow cross-fade instead of before is exact
    rather than approximate: fog is affine in the colour.
  - **One integral for a whole object** — the "whole tree is one point" lead
    below, and it survived contact with a measurement in a shape nobody
    predicted. The obvious bound is metres of spread, on the argument that
    extinction sets the error; that is *wrong*, and `render.fog` says so. The
    error tracks the change in **bearing** across the shared group and almost
    ignores the change in range, because the Mie phase function turns far faster
    with angle than the column does with distance: 29 degrees of spread is 4.2
    code values, 1.1 degrees is 1.1, half a degree is 0.4, at every range from
    100 m to 12 km. So the group takes the same hundredth-of-the-range bound the
    per-triangle rule uses, which keeps it no looser than what already ships,
    with an absolute 26 m cap on top for the far field where a hundredth of the
    range would reach 120 m. At its own limit the sharing is worth 1.13 of a
    code value; over the wood it actually draws, 0.1 at worst and nothing at the
    mean. It still pays on the software rasterizer, which fogs in C.
  - **The scatter's ground lookups go through a memo**, the one the roads
    already use, generalised to hold two functions. A tree is placed
    deterministically per world cell, so a cell in range asks for exactly the
    coordinates it asked for last frame: 15,260 asks a frame, 92% of them hits.
    Exact, keyed on the coordinate bits, no visual term — `render.cost` holds
    both tables against the functions they stand in for.

  **What is left is the shading, and caching geometry will not touch it.** The
  premise this work started from — that 60 fps needs the build cut five to eight
  times, which means caching static geometry — does not survive being measured.
  Stub out `tri_leaf` and `tri_lit_n` and the wood still costs 16.5 ms: the
  scatter walk, the per-tree placement and the crown geometry together are under
  a third of the build, and that is the ceiling on what any chunk cache could
  remove. The other two thirds is per-vertex shading, and a cache cannot hold it
  because the captured colour is still camera-dependent after the fog move —
  `tri_leaf` shades through the canopy BRDF, whose transmission and down-sun
  surge are functions of the view direction, and `tri_lit_n`'s specular lobe is
  too. So a chunk cache is a 16 ms optimisation that costs 6 MB re-read a frame
  and complicates the hottest code in the renderer, and it is not on the way to
  the target.

  The remaining lead was the instance lattice, and it has since been taken —
  see the entry at the head of this log. With the air out of the vertex colours
  the view-dependent BRDF was the single remaining thing between a captured
  buffer and a static one, and moving the shading to the fragment stage is what
  removed it. The fog anchor around a settlement's props and the per-vertex
  specular block in `tri_lit_n` are the smaller two, in that order, and are
  still listed in `TODO.md` with what each is worth.

  Three things measured and rejected, so nobody spends the afternoon again:

  - **Sharing `terrain_light`'s internal `lit_surface` with `tri_leaf`'s dark
    half** — which is exact, since it is the same call with the same arguments
    — is worth **2%** of the woodland. `sky_irradiance` is a nine-term dot
    product; it was never the cost.
  - **The shadow-occluder replay is 1.9 ms**, not the second traversal it looks
    like. The near cascade's footprint is small and it casts the mid-detail
    model.
  - **A per-cell instance cache for the scatter walk.** The walk itself — the
    forest field, the frustum test, the road-verge bit and the per-tree
    placement — is 7.7 ms of the build, and the memo above already took the
    expensive part of it exactly and without a cache to invalidate.

  While reading this: the comment on `tri_leaf` says its dark half "is exactly
  terrain_light's ambient term". That has drifted. `terrain_light`'s ambient
  half also carries the sky-reflection lobe, which `lit_surface` does not, so a
  fully shadowed leaf loses its sheen. Decide whether that is wanted before
  sharing the call, because sharing it would silently change the answer.

- **The gallery costs what hybrid costs.** Every coverage page is one 960x540
  hybrid frame with 4x multisampling. Measured on the `cover` category, 59
  stills: **187 s** with the GPU environment pass, **1295 s** forced onto the
  software rasterizer — about 2.8 seconds a frame against about 22. (This entry
  used to say five to nine seconds a frame and two and a half minutes for a full
  refresh, which cannot both be true of 78 stills and was not true of either.)
  1280x720 was tried and ran 15 s a frame on an alpine vista; the cost does not
  scale with pixels alone, because a mountain view marches much further than a
  coastal one. On hardware the resolution is one constant in `gallery_res`.

  `--shots` does not re-render: `--test` writes `build/<rid>/shots` and `--shots`
  copies it into `docs/` and rebuilds `coverage.md`. Running the two in sequence
  costs one gallery pass, not two.

- **Woodland's price, measured.** Stands, three species, three LOD bands and a
  litter tint on the terrain cost +13.2% on the raster bench, +5.3% hybrid and
  +4.5% path-traced (interleaved A/B, minimum of six runs each). Raster pays
  most because every tree triangle is shaded on the CPU there; hybrid hands
  them to the GPU and pays only for generating them. The scatter does *not*
  cost proportionally more heightfield lookups than the sprinkle it replaced —
  the density field's two octaves reject most cells before `fly_world_ground`
  is called — so the bill is triangles, and the LOD bands are what keeps it to
  one digit. Anything added to the far band should be checked against
  `bench_raster` specifically.

- **Canopy lighting's price, measured over a wood.** Spherical leaf normals,
  the canopy BRDF on leaves, canopy sky-occlusion on the terrain and tree
  casters in the near cascade cost +10.3% raster and +3.7% hybrid on a frame
  over closed canopy (interleaved A/B, minimum of four). Measure this kind of
  change on a forested view: `cover_bench` renders `scene_water`, which has
  almost no trees in it, and reported the same change as 5% *faster* — noise on
  a scene the work does not touch.

- **The atmosphere's price.** One scattering model behind sky, sun and aerial
  perspective, with shafts, costs +6.9% raster and +0.6% hybrid over a wood
  (interleaved A/B, minimum of four). Cheaper than it sounds because of what it
  removed: the old fog faded toward a full sky lookup — gradient, sun halo,
  four-step cloud march — evaluated once per triangle on the CPU and once per
  *vertex* on the GPU. The replacement is a handful of exponentials per
  evaluation. The shaft tap is the one fbm added back.

  Keeping that tap honest at range costs a further **+3.3%** raster on a
  horizon-filling vista (112.4 ms to 116.7, minimum of four interleaved): a lerp
  toward the deck's mean per scatter call, a pixel-footprint smoothstep and two
  more lerps per slab tap, and eighty-one cloud samples once a frame to find the
  mean at all. Measure this one on a frame that is mostly sky — the cost lands
  where the artifact did.

- **The dark marks along a distant skyline were the cloud deck, and are fixed.**
  This entry used to say they were settlements. That was wrong, and the way it
  was wrong is worth keeping: the check that produced it suppressed the *object*
  scatter and found the frames differed only in near rows, which rules out
  trees and buildings and says nothing whatever about the sky. Reading the depth
  buffer along the band would have settled it in a minute — every one of those
  pixels is at the far plane.

  They were the cloud field point sampled at absurd range, twice over. The shaft
  gate in `fly__scatter` asked what the deck was doing at the midpoint of the
  segment, which for a sky ray is 160 km out, so a 5.2 km field landed in a
  different cell for every pixel; and the slab march in `sky_sample` did the
  same with four taps spread over the fifty kilometres a grazing ray spends
  inside a 580 m layer, under a 60 km cutoff that drew a horizontal line across
  the sky at about two degrees for the slabs to hang from. Both now fade to the
  deck's mean as a single tap stops being able to describe it — the shaft over
  the deck's own wavelength, the march over the pixel footprint, which is the
  trade terrain and water already make against `pxscale`. Gated by
  `render.atmosphere`, which fails eight ways on the old code.

  **Beer-Lambert opacity for the march was written, measured and reverted.**
  Alpha per tap is a fixed fraction of the cover, so a ray crossing fifty
  kilometres of cloud accumulates no more than one crossing the 580 m straight
  down, which is wrong and visibly so. Replacing it with `1 - exp(-dens*seg/L)`
  is right, and it makes horizon cloud opaque — at which point the deck's night
  colour floor, well above a moonless sky, turned the skyline of `tod_dusk` into
  a bright torn strip under the stars. The lighting has to be fixed before the
  opacity can be, and that is a different job from this one.

- **The horizontal compression was being applied vertically too, and that was
  most of the sky's colour.** `FLY_SCAT_DENSITY` multiplied the whole atmosphere
  by three so a twenty-kilometre world would have aerial perspective worth
  looking at. It also tripled the vertical column, which is a different claim,
  and the numbers said so: sunlight arrived at noon with a red/blue of **1.94**
  against Earth's 1.22 — Earth's sun at sixty degrees off the zenith, all day —
  and the blue channel saturated out of the zenith at 2.31.

  The compression is horizontal, so it is weighted by how horizontal the ray is:
  three for a level path, one for a vertical one, cosine between. Noon sunlight
  is 1.28 now and the morning zenith 4.9 against 4.1. Aerial perspective is
  *bit-identical* — 0.925/0.865/0.732 over three level kilometres, before and
  after — because a level ray was always getting the factor of three and still
  is. That equality is the check that the change did only what it claimed.

  **Multiple scattering** went in with it, as one isotropic term weighted by
  `1 - exp(-tau)`. Single scattering is not uniformly too dark, it is too dark
  in proportion to optical depth, so leaving it out does not dim a sky evenly —
  it collapses dusk and drains the band above the horizon. At 0.50 it is worth
  about 8% at the zenith at noon and half again near the horizon.

  `FLY_SCAT_SUN` moved 5.5 to 8.4 and is the only free number left. It cannot
  satisfy both ends: thinning the column overhead and refilling the horizon is a
  *redistribution*, so 8.4 splits it, leaving the noon zenith at 85% of its old
  radiance and the hemisphere's irradiance at 120%. The ratio between them, and
  the colour of either, is no longer a choice.

  One knock-on, recorded because it looks like a regression and is not: the
  directional contrast in `render.ambient` fell from 1.5–1.6 to 1.34–1.39. Light
  that arrives from every direction equally makes the sky more uniform, so there
  is less of a bright half to face. A function of n.z alone still returns 1.00.

- **The ambient half of the lighting was three constants; it is three integrals
  now.** The direct term has been physical since the scattering model went in.
  What a surface is *not* lit by directly was still a table: a zenith/horizon
  lerp on n.z for the sky, the literal (0.10, 0.12, 0.07) for the ground bounce,
  and no sky in the specular at all.

  The sky is projected onto nine spherical-harmonic coefficients once a frame
  off the same scattering model the sky is painted with, sixty-four samples, and
  reconstructed per vertex. Second order, not first: first order was measured
  against the exact hemisphere and rings — 9% bright at the zenith, 41% dark
  facing away from the sun, and shaded walls at a blue/red of 2.57 against a
  true 1.89. Second order holds 5% and 8%. The scale needed no fudge; the
  integral landed within six per cent of the old noon value on its own.

  The ground bounce is the terrain's own albedo under the camera times the light
  falling on it, one sample a frame, with a 0.30 form factor set so noon over
  grass lands where the constant did. The specular gains the sky as well as the
  sun, Schlick against the same basis.

  **And the Fresnel had no roughness in it**, which predates all of the above:
  plain Schlick takes every surface to a mirror edge-on, so at eight degrees off
  the ground plane a meadow was 1.91x brighter than head-on and read as
  polished. The grazing ceiling is now (1 - roughness) * (1 - vegetation) — a
  canopy of blades has no coherent reflection at any angle — which puts grass
  back on 0.976.

  Two halves of `render.ambient` are one-sided and say so. There is no bare rock
  or snow at this seed to check the roughness half of that cap against — the
  world tops out near three hundred metres — and the fit gate needs a
  thousand-sample reference integral, which is the aspect's whole runtime.

- **The propeller needed alpha, and the renderer had none.** Every aircraft in
  the gallery wore a black bicycle wheel: an opaque band around the rim with
  opaque ghost blades spoked out of the hub. That shape was not a mistake so
  much as a workaround — a translucent disc was not available, because both
  rasterizers write an opaque colour wherever they pass the depth test, and an
  earlier attempt to fudge it passed an alpha into `tri_shaded`, whose last
  argument is an ambient-occlusion term, and bolted a plate to the nose.

  So there is now a small deferred list of blended triangles: shaded and fogged
  where they are built, composited at the end of the frame against the finished
  z-buffer, writing no depth. Deferring is what makes it work in every pipeline
  — in pure raster the objects live on the GPU and there is no z-buffer to test
  against until the readback — and it also means the prop is drawn by neither
  rasterizer, so the two cannot disagree about it.

  With alpha the shape is one line of physics. Time-average the blade: at radius
  r a chord c occupies c/r radians, smeared over `sweep` radians it covers
  c/(r*sweep) of the exposure, and that is the opacity. It falls as 1/r, so a
  disc is densest at the root and nearly clear at the tip — the exact inverse of
  the band that was there. It saturates to a solid blade as `sweep` goes to
  nothing, so there is no threshold between stopped and spinning. And it is
  small: a couple of per cent at the tip, which is what a photograph of a
  spinning prop shows. One number is not physics — `FLY_ROTOR_GAIN`, 2.1, set by
  eye, because the honest figure renders a prop you cannot find at fifty pixels.

  Cost is under the noise floor on a chase view: 105.6 ms against 103.5,
  minimum of six interleaved runs. The disc drops to two rings and three arc
  segments below nine pixels of radius and disappears below one and a half, so
  a sky full of traffic does not pay for discs nobody can see.

- **A settlement was four hulls on bare ground.** Whatever a refinery of that
  description looks like from a hundred metres, it is not inhabited: there is no
  object smaller than a building to read scale off, nothing between the
  buildings, nothing on them, and after dark nothing but the sky.

  There is now a kit — crate, drum, fence with a gate, lamp with a pooled light
  under it, truck, railing, container, shed, gantry, plume — and three groups
  built out of it that every site gets: a fenced yard, a fuelling apron, an ops
  block under a lit tower. Kinds add their own on top: walkway railings and
  ladders on the refinery's tanks, its flare and its cracking-tower steam, dust
  off a mine's heap, deck railings and a stair and a slewing dish at a skyport.

  **Nothing is placed at a fixed offset,** and that is the part that took the
  time. The first version put the yard at `-axis * 150`, which on the seed used
  for the shots dropped it in the rail corridor, and the floodlights at
  `FLY_PAD_DECK_R + 13`, which is inside the pad's own eighteen-metre keep-clear.
  Each group now tries a short list of bearings through `site_plot` and takes
  the first that fits. The apron's hardstand needed the opposite fix: the
  footing rule seats a piece on the *lowest* ground under it and then buries it,
  which is right for a building and buries a 0.75 m slab completely, so the slab
  is placed standing proud instead.

  **A group is not a box, and `fly_site_footing` treats it as one.** The rule
  *walks* a plot up to `FLY_SITE_WALK` to find ground, and then applies its
  keep-clear to the walked centre. A yard asked for at 132 m off the city at
  seed 4242 came back at 92 — legal by that test, with its far corner standing
  on the landing deck. `render.pad` caught it: the rim ripple at 120 m went 0.269
  to 0.640 px against a bound of 0.5, in a four-degree wedge, which is the gate
  doing exactly the job it was written for. So `site_plot` takes a *reach* — how
  far the group's pieces extend back toward the site — and refuses a candidate
  whose reach crosses the keep-clear, with the nominal radii moved out to suit.
  All three groups still find room at every site of four seeds (96 of 96), and
  96% of floodlight positions survive.

  Held by `render.settlement`, three counts off rendered frames at the coverage
  camera: luminance steps along the scanlines below the skyline (**1574**,
  against 878 before), separate bright runs at 22:30 — emitters, not lit area
  (**296** against 208), and pixels that differ between two frames four seconds
  apart from a fixed camera (**373** against 174, with the sky above the skyline
  as the control at 56). Rain and turbulence are forced off in the gate; both are
  animated and either one swamps all three. Cost is **+4.3%** on that raster
  frame (147.6 ms to 157.4, minimum of three interleaved), which is the close
  band — the kit
  is dropped entirely past 2.6 km and thinned past 900 m, so a world of
  settlements pays for the one you are standing in.

  **Two things it never had, added later.** Everything in a site stood on
  unbroken grass with a hard seam against it: the shadow map handles the sun,
  and an overcast noon barely has one, so what was missing is the *ambient*
  half — a wall takes most of the sky away from the grass at its foot and none
  of it a few metres out. `prop_skirt` is a ring of ground-following triangles
  round a footprint that samples the terrain's own surface at each vertex and
  ramps only the ambient term, so it is a decal rather than a material and
  matches whatever it lands on. And the window grid divided any height into at
  most eight bands, which put 11 m between floors on an 88 m spire; storeys are
  3.6 m now and the count is bounded by the projection at five pixels a storey,
  which draws more of them close up and fewer than the old cap did at three
  kilometres.

  **Three things the first version got wrong, and the gallery is what said so.**
  The counts above all improved while the frames still looked worse in specific
  ways a number cannot see, which is the argument for regenerating the shots
  before believing a visual change is done.

  - *The lamps were black sticks.* A 7.5 m pole 0.44 m across with a 0.8 m head
    is, from two hundred metres, a dark line in grass, and thirty of them read
    as litter rather than as lighting. Galvanised instead of near-black, a
    concrete plinth so it is installed rather than pushed in, a cranked
    cross-arm and a head twice the size.
  - *The skyport stair was a dotted line of floating tiles.* Treads carry only
    from alongside; the stringers under them and the handrail over them are
    what make a flight read from the air, and they were missing.
  - *The yard was out of sight.* Its 58x42 footprint means a half-diagonal of
    72 m, which under the reach rule puts its centre at 145 m minimum — past
    the frame at every gallery camera. At 46x34 it clears at 112 and is in the
    same view as the apron it serves.

  **And the ground between the groups was untouched grass,** which is what makes
  a good building read as an object set down on a field. Each group now has a
  graded gravel track from the deck edge, and the yard's runs from the apron
  rather than from the pad, because that is who it serves.

  **`fly_world_ground` is not where the ground is drawn,** and two attempts at
  the track were spent learning it. The terrain is a mesh whose finest ring has
  twenty-six metre cells, so what gets rasterised between four of its vertices
  is their bilinear interpolation. Over concave ground — a valley floor, or the
  outer shoulder of the smoothstep that grades a landing apron flat, which is
  every settlement — that surface sits *above* the heightfield, by the better
  part of a metre. A ribbon laid at `fly_world_ground + 0.06` disappeared for
  its outer half and looked like it stopped in a field; at `+ 0.15` it still
  did; at `+ 1.0` the whole thing appeared, which is what identified the cause.
  It is now offset by `ground_bias`, the height of the highest chord of a cell
  through the point, sampled four ways because the mesh grid is anchored on the
  camera and its phase is not knowable from here.

  That bias costs nine heightfield taps, so it is evaluated once per
  cross-section rather than per vertex and the stations are ten metres apart
  rather than six — no loss, because the surface being followed is linear across
  a cell, and it measured **+7.3%** the other way against +5.7% for this. Both
  ends overrun their target by four metres so the join is hidden under whatever
  it joins, and the yard's gate moved to the run the approach faces: it was in a
  side wall, so the track arrived at a fence.

  **The apron's hardstand was buried outright,** which is what the remaining gap
  at the hangar turned out to be — not the track at all. The comment above it
  said it was standing proud and the code had it at the footing height, and
  `fly_site_footing` deliberately seats on the *lowest* ground under a piece and
  sinks it a further `FLY_SITE_BURY`: right for a building, fatal for a 0.75 m
  slab, which is entirely inside its own foundation. Slabs go through `pave`
  now, which seats them on the highest drawn ground under their own footprint.

- **The clouds had not been deleted, but you could not see them.** Reported as
  "have the clouds been lost?" against the gallery, and the first thing to
  establish was that they had not: the finished-pixel standard deviation of the
  clear sky was **12.2 before the horizon-slab fix and 15.0 after**, and the mean
  step between neighbouring sky pixels 0.427 against 0.412. The field was
  entirely intact. What was gone was its *structure* below about ten degrees.

  The grazing-ray fade added with the slab fix collapsed the whole cloud field
  to one mean number. That is right about the 650 m octave, which really does
  alias when a tap slides hundreds of metres per pixel, and wrong about the
  5.2 km one, which does not: it took every cloud mass off the horizon along
  with the aliasing. It now drops to a two-octave field first and only reaches
  the mean in the last degree above level.

  **Both fades ramp on the logarithm of the slide, and that is the load-bearing
  detail.** The slide goes as the inverse square of the elevation, so a
  smoothstep on it directly is squeezed into about a degree of sky however far
  apart its thresholds are — and a transition that happens over a degree *is*
  the horizontal line the fade exists to remove. Measured at a 1.5% kink at 2.1
  degrees. Dropping the second fade entirely was tried on the theory that the
  haze fade has cloud down to a third of its weight by 2.8 degrees anyway, and
  it is worse at 3.9%, because a third of a large error is still large. In log
  space it is 0.48 to 1.18%.

  **The cloud's colours were constants** — (0.98,0.96,0.95) lit and
  (0.38,0.39,0.44) shaded, scaled by the hour — while everything else in the
  frame derives from the scattering model, which is the same bug the ambient
  term had. They are now albedo times what falls on the deck: the sun through
  the air above it plus the sky it can see, off `fly__scatter`, once a frame.
  Worth saying plainly: **this is the principled half and the small half.** Away
  from the sun at 08:00 it moves the four-degree detail figure from 0.0089 to
  0.0115 and leaves a cloud at 26 degrees very slightly *dimmer* than the
  constant did. An early measurement that made it look like the larger of the
  two changes was sampling the sun's own aureole — see below.

  Cost is under the noise floor on a sky-filling frame: 164.1 ms against 168.6,
  minimum of three interleaved, which is to say slightly faster.

- **Two gates were got wrong before either was got right, and both mistakes are
  worth keeping.**

  *A cloud measure that included the sun measured the sun.* The first version
  swept a ring of constant elevation and took the spread between its bright and
  dim fifths. The bright fifth is the Mie aureole — far brighter than any cloud
  and nothing to do with the deck — so the figure came back within a per cent of
  itself against both of the failures it was written to catch. Bearings within
  60 degrees of the sun are now dropped, the same reason the neighbouring
  smoothness check drops 25.

  Even with the sun out, the spread does not discriminate: 1.41 for the working
  renderer against 1.41 and 1.39 for the two failures. It is kept at a loose
  bound as an assertion that there is a deck up there and labelled as such. The
  measure that does work is the mean step between neighbouring bearings on the
  four-degree ring: **0.01147** working, **0.00892** with constant cloud colours,
  **0.00558** with the field collapsed to its mean. The eight- and
  twenty-six-degree rings do not separate anything and their bounds say so.

  *And the elevation-smoothness check has been retired, with evidence.* It was
  written to catch the horizontal line the old 60 km cutoff drew, and it worked
  while the band below three degrees had been flattened — a sky with nothing in
  it is very smooth. Measured at its own sampling, the renderer with the cutoff
  scores 1.105 to 2.422 per cent and the renderer with visible cloud 0.479 to
  1.175. Those overlap, so no threshold divides them. Three other shapes of the
  same measure were tried — the second difference averaged across azimuth, the
  worst kink over the median kink, and whether the worst kink shrinks as the
  elevation sampling is refined — and all three overlapped as well. What still
  covers the artifact is the azimuthal step check, which passes at 0.10 to 0.67
  per cent against its bound of five, and the cloud checks above, which fail if
  the deck is flattened at range.

- **The horizon band of "stretched stars" was the cloud march, and the fade was
  keyed on the wrong end of the ray.** Reported against the night and dusk
  frames. The stars are round; what was streaked was a band of hard horizontal
  lines just above the skyline. Suppressing the slab drops that band's
  row-to-row second difference from 0.50 to 0.03, which is what pinned it.

  The fade that stops a grazing ray aliasing was measured at `t0`, where the ray
  *enters* the layer. Wherever the camera is inside the deck — 2848 m against a
  slab from 2340 to 2920, which is every frame of the alpine scene — t0 is zero,
  so the fade never engaged at all while the four taps still spread to the exit
  clamp four hundred kilometres out. Every scanline landed in a different cell.

  Two more attempts before it came right, both recorded because each looked
  reasonable. Keying it on the *last* tap engages the fade and does not fix the
  stripes: the four taps are then in wildly different regimes — six kilometres
  and forty-four for a ray a degree below level inside the deck — and one fade
  either aliases the far tap or flattens the near one. Each tap now carries its
  own. And the floor on `|rd.z|` was 0.02, which is 1.1 degrees, sitting inside
  the band and capping the computed slide at a third of the truth exactly where
  the taps move fastest; it is 0.0015 now, far below the angles the term
  describes.

  The ramp is `s^2/(s^2 + k^2)` rather than a smoothstep between two thresholds,
  because the slide goes as the inverse square of the elevation and a smoothstep
  is squeezed into about a degree of sky however far apart its thresholds are —
  and a transition inside one degree is the horizontal line the whole fade
  exists to remove. It also costs a multiply and a divide where ramping on a
  logarithm cost a `logf` per tap.

  **The elevation-smoothness bound is back on because of this.** It was retired
  two commits ago as undiscriminating — the renderer with the old cutoff scored
  1.105 to 2.422 per cent and the one with visible cloud 0.479 to 1.175, which
  overlap. Fading per tap took the working renderer to 0.099 to 0.409, so 0.7%
  now sits with 41% of margin under it and 58% over the artifact. Most of what
  the retired version was measuring was one fade applied to four taps that
  needed four.

  What is *not* fixed, and is a modelling gap rather than a sampling one: with
  the camera inside the layer the deck is still drawn for sky rays and for
  nothing else, so a mountain top in cloud renders crisp with a grey wash behind
  it. Coherent in-layer extinction means marching the slab for surface rays too,
  which roughly doubles its cost and re-blesses every frame.

- **The sun's obliquity was counted twice.** `lit_surface` multiplies by
  `dot(n, sun)`; the beam it multiplied was `clamp(sun.z * 1.7, 0, 1.25)`, and
  `sun.z` is that same cosine. So a wall held square to a low sun lost its light
  along with the ground beside it, and a frame at low sun dimmed as a whole
  instead of splitting into a lit side and a dark one.

  The beam is now the model's own transmittance along the sun's path, measured
  once a frame across the beam, with the cosine left to the surface. The two do
  not agree: against noon, the beam at 07:00 is 0.420 of it and the sine says
  0.259. Nor did the colour — the hand-lerp to (1, 0.55, 0.3) tops out at a red
  over blue of 3.3 where the air gives 8.5 at fourteen degrees of elevation.
  `FLY_SUN_E0` is set so noon lands exactly where the old constants put it, so
  this changes the shape of the day and not its middle.

  Held by `render.atmosphere` as a ratio of ratios, so the scale cancels: the
  beam must track the air and must *not* track the sine. The old term matched
  the sine by construction and fails the second half outright.

  One knock-on, and it is a correction rather than a regression: the cloud deck
  is dimmer. Its top takes the beam times the cosine like any other horizontal
  surface, where the old term handed it 1.6 times that, so the cloud contrast
  figures in the same aspect moved down with it and the bounds moved with them.
  The deck is lit by the beam at *its* altitude, not the one at the ground — a
  cloud top at 2.9 km has most of the air below it, and reusing the ground value
  cost it another factor of two before the contrast check caught that too.

  **This did not fix the dawn frame, and it is worth saying why.** `tod_dawn` is
  at 06:00, which in this world is the exact instant of sunrise: the sun sits at
  0.00 degrees, so there is no direct light under either the old model or the
  new one and the frame is ambient-only. That is why it has no terminator, and
  the caption's "long shadows" describes something the frame cannot contain. At
  06:18 the two renderers differ by a modest warming of the snow. Moving the
  frame off the exact sunrise is a scene change, not a renderer one.

- **The rippling in the distance of the snow-cap frame is in the hybrid path,
  not the terrain mesh** — and two hours went into the wrong renderer before
  that was established, because the gallery frames are hybrid and the probe was
  raster. Vertical roughness of the ground between five and sixty kilometres:
  **4.91 hybrid against 1.20 raster** on the same camera and hour.

  The mesh does have a real bug next to it, found on the way and deliberately
  not fixed here. `terrain_ring` means to bound the material's footprint by the
  cell — an octave finer than the mesh is point-sampled at the corners and
  smeared across — and the guard is `min(px, dist/step)` where `px` is samples
  per metre and `dist/step` is a count. It has never once bound. Correcting it
  to `min(px, 1/step)` changes the far ground's measured roughness by 0.7% and
  nothing visible, because at grazing angles the ripple is coming from the other
  pipeline; correcting it also flattens the near ground, since the finest ring's
  cells are twenty-six metres and the strictly correct band limit throws away
  every scrap of texture under the aircraft. It wants its own change with its
  own gate, and a decision about whether a per-vertex-coloured mesh should be
  band-limited at all.

- **Traced ambient occlusion ships on, and what is left of it is the objects.**
  `ao_target` defaults to 12 and both rasterizers read one world-space window;
  `render.occlusion` holds five claims and `gpu.frame` holds the parity. Kept
  here for the two things still open and the several that were tried and failed.

  **It fades to the scalar, not to open sky, and that was found the hard way.**
  The traced window is 160 m around the camera and an aircraft at cruise sees
  almost none of the ground it is looking at from inside it. Handing those
  pixels 1.0 deleted woodland occlusion from every aerial view: on the coverage
  gallery's aerial wood, 5796 pixels changed and every single one of them got
  lighter, by 4.7 of 255. The two terms agree closely enough for the crossover
  to be invisible — closed canopy traces to 0.534 against the scalar's 0.58 —
  which is the same coincidence that makes the scalar a bad description up close
  and a perfectly good one at half a kilometre. With the fallback in, the same
  frame differs from the pre-AO gallery in 173 pixels.

  **Only the terrain reads it.** Trees, buildings and craft are shaded by
  `lit_surface` against the same ambient hemisphere and take no occlusion at
  all, so a trunk inside a closed stand is lit as though it stood in a field.
  The window is indexed by ground position, so the read is available to them —
  what is missing is a decision about the height falloff, since an object's
  surface is metres above the lattice point that describes it and a crown at
  canopy height sees more sky than the floor beneath it does. That wants a
  second channel in the window (visibility at height) rather than a fudge.

  **It waited two rounds on the world, not the renderer, and that was right.**
  Canopy closure used to be about a tenth — eight trees in a 44 m cell, crowns
  near three metres — so a cosine-weighted hemisphere from the floor saw almost
  all of the sky and the traced term came out *lighter and flatter* than the
  `1 - 0.42 * forest_mask` scalar it replaces (frame contrast 0.379 to 0.349
  under sun, 0.442 to 0.386 under overcast). The tracer was right and the scalar
  was asserting something false about the world. Denser stands — sixteen to a
  cell, crowns near five metres, gated by `world.forest`'s closure bound — are
  what made the term worth its rays: 0.391 of the sky under a crown against
  0.922 twenty metres off, and 0.534 under closed canopy against 1.000 in the
  open.

  **Keying on ground rather than on last frame's pixels is the whole design.**
  Occlusion of the ambient hemisphere is a property of the world, and this world
  is a pure function of position and seed, so a sample belongs to the ground and
  not to a pixel. Reprojecting a screen-space history discards every sample at a
  disocclusion — precisely the silhouette of a tree line, and precisely what an
  aircraft generates continuously. Fly away and back and the samples are still
  there. The key is exact rather than quantised, which is luck worth writing
  down: `terrain_ring` looks camera-anchored — its origin is
  `floorf(pos/step)*step` — but that means every vertex it emits lands on the
  global lattice `{k*step}`, so the mesh slides by whole cells and the vertices
  under it never move.

  **Warm frames are free on both backends.** At 640x360, best of five, target 12
  against the term off: GPU 218.5 ms cold and 113.1 warm against 115.7 and
  118.2; software 358.8 and 253.6 against 249.1 and 249.0. The cold column is
  the same ~105 ms either way because the tracing is shared CPU work above the
  backend split. The warm column is the read, and on the GPU it comes out
  slightly *ahead* of having no term, because a live traced term lets the vertex
  shader skip the forest-mask fbm it replaces.

  **The interactive path buys it on instalments; stills do not.** `ao_budget`
  defaults to 0 — unlimited, fill everything before drawing — because a still
  has to be the same frame whether the window was warm or cold, and every gate
  and gallery image depends on that. `fly_ui_init` sets 2500, a little over
  three milliseconds, which converges a cold arrival in about half a second
  while the steady state still traces nothing. Unfilled cells read as open sky,
  so the term fades in rather than popping. This entry used to claim there was
  no interactive loop to be progressive against, which was simply false —
  `--window` builds `fly_win.c` over vendored TIGR and runs one.

  **A texture unit that does not exist is not an error you will see.**
  `fly_gpu_img_bind` rejects a unit at or above `FLY_GPU_TEX_MAX`, and the
  rejection leaves the sampler uniform at its default of 0 — which is the near
  shadow cascade. Binding the window to unit 4 on a four-unit limit therefore
  multiplied the terrain by shadow-map depths and drove backend parity to
  **119.5/255** across nearly every block. It reads as a shader bug and is a
  binding bug; the tell is the magnitude, since an AO mismatch cannot be worth
  half the dynamic range. Units 0-2 are the cascades and 4 is the upload
  scratch, so 3 is the only free one.

  **Three measures of "the term has structure" were tried before one worked.**
  The spread of the difference against the flat scalar does not discriminate:
  flatten the whole lattice to one constant and it falls only from 4.4 to 3.5,
  because most of that spread is the two shading regimes diverging slowly across
  a varied frame. High-passing that difference does not either (1.79 flat
  against 2.19), because the difference still carries the albedo it multiplies
  and terrain detail is high-frequency by itself. High-passing the *ratio*
  divides the albedo out and does discriminate, but as an RMS it averages the
  crown edges against every flat pixel in the frame, and the answer then depends
  on how many flat pixels there are. A high percentile of the same quantity asks
  whether the edges are there at all: 10.72% traced against 5.96% flattened.

  **That measure is software-path only, deliberately.** Both backends
  discriminate by the same factor — 10.72 against 5.96 on the software path,
  1.69 against 0.85 on the GPU — but the two scales do not overlap, because the
  GPU shades albedo per fragment and the software path per vertex, so on the GPU
  the ratio is a small signal on a busy background. No single bound spans both.
  `gpu.frame` is what carries the GPU: it holds the backends to 1.8/255, so a
  term with structure on one has structure on the other, and a bound per backend
  would be two numbers where one does.

- **Water's near-field detail ships without a gate of its own,** which is worth
  saying out loud. Two metrics were tried and both measured something else. The
  obvious one — high-frequency contrast near the camera against contrast a
  kilometre out — comes out *inverted*, because contrast on water is dominated
  by grazing-angle glint rather than by ripple octaves, so the band under the
  horizon always wins. Frame-to-frame change does not work either: a half-metre
  ripple genuinely moves a long way in an eighth of a second, so legitimate
  motion and unresolvable speckle look the same. A metric that works probably
  has to hold the sun still — an overcast frame, or the surface sampled through
  a probe rather than through a render. The change is covered meanwhile by
  `render.material`'s open-water check and by `gpu.frame`.

- **The grain is capped by the fade's own guarantee, not by taste.** Only about
  a third of the amplitude an octave loses can be handed to the projection-
  locked grain before a faded sample stops being twice as close to the patch
  mean as a point sample — the figure `render.material` has always required.
  Most of that headroom goes to the coarse octaves the fade does not touch
  (0.0069 of 0.00895 before any grain at all). Anything that wants a stronger
  grain has to buy the headroom first, by giving the coarse variation the same
  treatment, and should not get it by loosening the check.

- **Nothing here has been measured on a GPU, and the gallery has lost its GPU
  half.** Every frame-time figure in this file and in the gallery was llvmpipe,
  a software rasterizer, so they say what
  the algorithms cost and nothing about what the hardware path does — and on
  llvmpipe "GL memory" is main memory, so the residency work above is invisible
  in a wall-clock number here however much it is worth on a card. That is why
  it is gated on bytes over the host/GL boundary (`fly_gpu_traffic`,
  `gpu.traffic`) rather than on milliseconds: the byte count is the same
  wherever it runs, and it is the quantity PCIe charges for.

  The gallery is now weaker than that. The host the cloud work above was done
  on has no EGL or GLES at all, not even llvmpipe, so `make.py --shots` drew
  the whole coverage set through the CPU renderer and dropped the three GPU
  pipeline frames it could not produce — `pipeline_raster_gpu`,
  `pipeline_hybrid_gpu`, `pipeline_pathtraced_gpu` — along with the six-way
  pipeline comparison they were half of. Every timing in `docs/coverage.md` is
  therefore this host's CPU path rather than the llvmpipe figures it used to
  carry, and the numbers moved a long way: the raster reference frame reads
  781 ms where it read 343. Regenerating on a machine with a GL driver puts
  both back; nothing but a rerun is needed.

  What still wants a machine with a card in it: the split between geometry,
  shading and the two CPU traversals of the scene, which is the item above and
  the only remaining resolution-independent cost of any size.

- **The guideway ended in a fan, and the cause was in the parameterisation.**
  Both termini of the rail — the two places on the line a player stands on foot,
  because that is where you board and where you get off — were a splayed cone of
  triangles five times the pipe's width, a good ten metres of it, standing over
  the last pier. It had been there for as long as the tube had been drawn as a
  curve, in every world: measured over twenty-four seeds, every single route
  folded at both ends.

  It was not the cap. `draw_pipeline` mitres each ring into the plane that
  bisects the two directions meeting at it, and widens it by 1/cos of the half
  angle so the barrel holds its diameter through a bend — with a cap of 5 for
  the case where the line turns so hard the mitre would go to infinity. Nothing
  on a 1500 m route turns like that, and the *drawn* line did: at both ends it
  ran past the terminus and came back through itself, so the last few rings each
  hit the cap, opened five-fold, and splayed. The cap was doing its job on
  geometry that should never have reached it.

  The overshoot is uniform Catmull-Rom on controls it is not entitled to.
  `fly_v3spline`'s own note says so — even spacing is the precondition, and a
  survey has it everywhere except at its two ends, where the trim that stops the
  line short of a settlement leaves a stub of five metres against neighbours of
  seventy-five. The uniform form takes half the chord across those neighbours as
  its tangent: forty metres of tangent on a five-metre span, which is a loop.
  Sampled at the four sub-steps a near span is drawn with, the last steps into
  seed 404's far terminus ran 6.4 m out and then 1.0, 1.3 and 0.6 m back, with
  the mitre's own cosine at the joint between them down to 0.069 — a 172-degree
  fold, where the widening is meant to see a couple of degrees.

  `fly_v3spline_cr` is the centripetal form of the same curve — knots spaced by
  the square root of the chord — and it is a proof rather than a damping: a
  centripetal Catmull-Rom cannot cusp or loop whatever the controls do. The
  rail's three samplers (the drawn barrel, the piers, the lamp masts) take it,
  because all three have to agree about where the line is. `rail.line` holds the
  claim on the geometry rather than on pixels: twelve routes, drawn the way the
  renderer draws them, and no step may turn more than a right angle against the
  one before it. The sharpest turn any of them now takes between steps is 4.2
  degrees, against the 176 that was there. The uniform form stays for the road,
  which resamples to an even pitch and is entitled to it — and the test pins that
  too, by asserting that the uniform form does overshoot on the rail's controls.

  With the fold gone the terminus is a flat disc, which is a pipe somebody cut.
  A guideway is a formed thing that stops in a yard, so `draw_pipeline` takes a
  `dome` flag and the rail asks for it: rings of the tube's own frame closing on
  a pole a radius past the end, three or four latitudes deep depending on how
  many sides the barrel already has. Flat stays the default and every other
  caller keeps it, because a wheel, a hub and an exhaust stack all end in a cut
  face and a domed wheel is a pill.

---

## Interface and HUD

The chrome is a box model now: every group is a content rect inflated by one
pad, every cluster is a whole number of rows tall, `fly_hud_layout` reports
those boxes, and `fly_hud_draw` builds the frame from the same call the
`hud.layout` gate measures. Rows go through `icon_row`/`gauge_row` and the type
ladder lives in `fly_hud.h` (`FLY_FS_KEY`, `FLY_FS_VAL`, `FLY_UI_ROW`), so a
new property row is evenly spaced and correctly sized by construction rather
than by a call site copying the offsets next to it. Every row in a cluster
leads with a sprite drawn on the band's own centre line (`FLY_ROW_MID`), which
is what makes the lattice a property of the row primitive rather than of the
artwork: `hud.layout` measures each band's ink against its centre and finds it
exactly on it, where the run-of-ink measurement it replaced could be fooled by
a sprite whose middle is darker than its ends.

Text goes on the same line by the same rule. `fly_text_mid` centres a string on
a point by its *ink* — `fly_text_ink` reports the box the marks actually make —
rather than by the box the typeface reserves, which includes descender room the
string may not use. Every centred glyph in the game goes through it, and
`hud.centre` gates both the metric and a frame.

The surface is one material — `fly_img_frost`, a blur of the frame masked by a
rounded rect or an annulus, *levelled* toward the material's own dark level,
then a thin cool tint and a hairline (`fly_ui_scrim`, `fly_ui_card`,
`fly_ui_glass_disc`, `fly_ui_glass_ring`, `fly_ui_page`). Levelling is
two-sided and solved rather than a one-way shade, which is what collapses the
pane's range from 30..200 to a single value and makes one text colour work in
every weather. The band (`fly_img_frost_ring`) is the exception and is
deliberately not levelled: nothing is printed on a reticle, and pulled to the
material's depth it became a black donut over the aircraft. A new panel uses
one of those five and nothing else; a
hand-rolled opaque rect is a hole cut in the picture and will not survive
`hud.glass`. Corner radii are asked for as an upper bound and clamped to the
box's own half-extent, so short things come out as capsules for free.
Anything added to the HUD
belongs in that function — a widget placed with its own offsets is a widget the
overlap gate cannot see. Two shared constants sit in `fly_hud.h` for the same
reason: `FLY_NAV_*` (the corner buttons `fly_ui` draws) and `FLY_TOUCH_*` (the
throttle column it hit-tests). Both were duplicated per-file before, and both
duplicates had drifted into a collision by the time anyone looked.

The icon set is objects rather than glyphs: filled silhouettes of convex
pieces painted in four roles from a palette keyed to the icon, with a rim and a
contact shadow of their own. `fly_ui_sprite` draws the colour form for menus
and headings, `fly_ui_icon` the same artwork in one colour for instruments,
where the row's colour is already its state. A new icon needs an enum entry, a
palette row and a case in `fly__art`; `ui.sprite` fails if any of the three is
missing, and fails again if the new silhouette is the same shape as one already
there.

Type is spaced for its own contact shadow. The outline `fly_text_sh` lays on
every glyph is one pixel on each side at *every* size, so it always spends two
pixels of the gap the face leaves between one glyph and the next — a gap that
scales with the type while the outline does not. At 26 px the face leaves 2.1
and the outline fits; at `FLY_FS_S` it leaves 0.9, and the outlines of adjacent
digits met: every three-figure number on the HUD was drawn as one connected
shape with notches in it. `fly__tracking` in `fly_font.c` gives the small sizes
those two pixels back, through the measuring path as well as the drawing one so
`fly_hud_layout` still agrees with the pixels, and `hud.type` counts the marks
and the gaps on both faces at every size on the ladder. Anything that adds a
size below 20 px inherits it; anything that adds a *second* outline pass would
need it again.

### The window and the views

**The window is resizable**, and the frame follows it: the game re-renders at
whatever the window becomes rather than scaling a fixed buffer into it, which
is also what keeps a click landing where it was aimed — the backend reports
pointer positions in the window's own pixels, so a frame that is not the
window's size is a frame whose controls are somewhere else. (Blitting a fixed
frame into a shrinking window also indexed the window's buffer by a stride that
was no longer its own, which is a write off the end of it.)

**Views**: the game opens on a splash screen whose circular Play button
launches into the world. While playing, two small circular icons in the top
right open the pilot profile and the guide (tap the active icon or press esc
to return); esc from the world view returns to the splash. Overlay windows
(actions, chart) are centered. A full-screen page is the same material at full
bleed — the world behind it, blurred hard, levelled and tinted a stop deeper
than a corner group — rather than an opaque dark gradient: opening the profile
used to put the game away, and the aeroplane you were flying stopped existing
while you read about it. Deeper because a page is read rather than looked
through and has no edge to say where it is. An overlay window on top of one is
the same glass again at a lighter tint, because on a dark page elevation reads
as lighter, the way a sheet of glass lying on another one does; `hud.glass`
holds the two apart by measurement so the window cannot collapse into the page
it is drawn on. Each page and each list section is headed by the sprite of the
thing it is about — a parcel over the cargo hold, a planform over the airframe,
a chip over the modules — so the profile and the actions menu are visibly
describing the same game.

### Instruments

**Instruments**: the HUD is a set of groups — estate top left, task top right,
heading across the top, speed and altitude tapes down the outer columns, craft
gauges and weather bottom left, radar bottom right, event log centred at the
bottom.

Each group that needs a surface sits on one pane of frosted glass: the frame
behind it blurred, *levelled*, then a thin cool tint over the result, then a
hairline around the edge. The blur is what separates the panel from the world,
so the tint can be thin enough that the ground still moves under every group;
the hairline is what says where the panel ends, because about a third of the
time the thing behind a corner group is a flat sky with no detail in it to be
out of focus, and a pane over an overcast horizon otherwise had no boundary at
all.

Levelling is the material solving for its own surface. Before the tint goes on,
the glass lays one wash whose colour and alpha are computed from the blurred
backdrop's own mean, so the pane lands on the material's level whatever it was
laid over — 57 to 58 across a field running black to white, where the old
one-sided version ran 30 to 200 (`hud.glass` measures it over four fields).
That is what makes one text colour work everywhere: the pane is dark and cool
and white type has 197 levels under it at every hour and in every weather. It
used to be white glass, on the reasonable argument that a white pane lifts what
is behind it rather than cutting a hole in the picture — but it was carrying
white type, and over an overcast noon the pane came out at 200 and the type at
255. Fifty levels is not a contrast; the profile page was grey-on-grey at
exactly the time of day the game looks best.

The centre reticle is the one piece of glass that is neither dark nor levelled.
It is a *band* rather than a disc, because a filled circle in the middle of the
frame hides the aeroplane and the ground it is pointed at, which is the whole of
what the instrument is for — and it wears a pale rim rather than the dark tint,
because levelled to the material's own depth it became a heavy black donut over
the aircraft. Levelling exists to give type a surface, and nothing is printed on
a reticle.

The widgets that do not need a surface do not get one. The two tapes and the
heading strip are a line, some ticks and their numbers straight on the world —
a slab behind each of them was most of what made the HUD read as a row of
boxes. They stay legible because every glyph on the HUD carries its own contact
shadow (`fly_text_sh`: a one-pixel black outline plus a drop shadow), which is
the only thing separating a tape number from a bright sky; `hud.glass` gates
that shadow too, since dropping the panes is only defensible if it holds.

That outline is one pixel on each side at *every* type size, so it always
spends two pixels of whatever gap the typeface leaves between one glyph and
the next — and the gap scales with the type while the outline does not. At
26 px the face leaves 2.1 px and the outline fits inside it; at the 11 px
every tick label, tape number and gauge value is set in, it leaves 0.9, so the
outlines of neighbouring digits met and a three-figure altitude was drawn as
one connected shape with notches in it. `550` read as `55o`. The small sizes
are given those two pixels back as tracking, measured and drawn through the
same function so a laid-out row still measures what it draws; `hud.type`
counts the marks a string of digits makes and the gap between them, on both
faces, at every size on the ladder.

One character is one glyph, whatever it takes to spell it. The glyph atlas is
baked over ASCII and the game's text is UTF-8, and a character outside the
atlas used to be folded a byte at a time — an em-dash is three of them, so
*Selkie 95 went down — wreckage on the deck* drew as `went down ??? wreckage`
and, less visibly and worse, *measured* three glyphs wide, so the layout that
places the next thing along was told the wrong answer. Text is decoded to
codepoints where it is measured and where it is drawn, by the same walk, so the
two cannot disagree about how long a string is; the dashes, quotes and spaces
the game writes in fold to the ASCII that stands in for them at these sizes and
anything else is a single `?` — one hole where the character was, not one per
byte of it. `hud.text` holds both halves, including that a truncated sequence
does not run off the end of the string.

Rows are keyed by sprite, not by word. `FUEL POWR HULL WEAR` was four words the
eye has to read before it can look at the bar it came for; a jerrycan, a bolt,
a shield and an hourglass are read at a glance and take a third of the width.
The sprite is in full colour and the bar carries the state, which is a division
of labour rather than a contradiction: the can says *what* is being measured and
the bar says how much is left and whether that is a problem.

The sprites are objects rather than glyphs. They used to be four or five
hairline strokes each at 13% of the icon's own width, in one colour, no fill —
and that weight cannot win: thin enough to look drawn and the shape disappears
at 11 px against a moving world, heavy enough to survive and it closes up into a
blob. A parcel and a chip drawn in strokes at 10 px are the same small grey
smudge, which is the state the actions menu was in — twenty rows of identical
smudges down the left margin, doing none of the sorting the icons were there to
do. Each is now a filled silhouette built from convex pieces, painted in four
roles (body, lit face, shaded face, trim) from a palette keyed to the icon, and
carrying its own rim and contact shadow so it sits on the panel rather than in
it. Menus draw them in colour, where the hue does half the sorting before a word
is read; instruments draw the same artwork in one colour, because a gauge's
colour is its state and a sprite bringing its own palette to that row would be
arguing with the only thing the row is trying to say. `ui.sprite` measures every
icon in the set at every size the game asks for: that it draws at all, that it
is shaded rather than a flat cut-out, that it is centred on its own ink, and
that no two silhouettes are the same shape — the fuel can and the cell were both
rounded rectangles of the same width, and at 19 px the tank gauge and the
airframe-life gauge were under indistinguishable sprites.

Every instrument sprite is drawn to the same vertical extent so a column of them
sits on one lattice — a row whose topmost lit pixel depends on its own shape
puts each gauge at a different height, which reads as a wobble even when the
rows are exactly evenly spaced. Values are right-aligned across the group, so a
reader scans a column instead of parsing a sentence, and every group is sized
to the content it actually prints rather than to a constant that fitted once.

Glyphs centred on a point are centred on their *marks*. A typeface reserves room
for the descenders of every glyph whether or not this string has one, so `?` —
which has neither a descender nor an ascender that reaches the em top — sat high
in a box centred on its type size, and in a 26 px nav button that is plainly a
question mark stuck to the top of its circle. `fly_text_ink` reports the box a
string's marks actually make and `fly_text_mid` centres on it; every glyph in a
button, a pill or a row band goes through it, and `hud.centre` checks both the
metric and a real frame, including the `?` against its own button's circle.

Every row is a band of one shared pitch with its contents on the band's centre
line, and every property label is one size — the type ladder and the pitch live
in `fly_hud.h` and both files read them, which is what stops a three-row block
from carrying three sizes and three gaps. The
geometry is one function (`fly_hud_layout`) that both the drawing code and the
tests read, which is what keeps "nothing overlaps" a measurement: the layout
adapts to the frame rather than trusting constants, giving width back to the
task summary and length back to the tapes as the frame shrinks, and dropping
the weather rose outright when a frame is too short to hold both it and a
readable tape. The right-hand column is reserved for the touch throttle at
every size, so no instrument is ever drawn under the slider.

Planes and drones are different machines end to end: separate flight models
(lift-curve aerodynamics with stall vs rotor collective with PD attitude
control), different control semantics (surface deflection rates vs commanded
tilt), and different instrument panels (pitch ladder + airspeed + stall flag
vs tilt bubble + groundspeed + VSI + battery endurance).

---

## Names on the chart are placed, not offset

Every site's name was drawn seven pixels right of its dot and six above it.
That is right for a lone outpost and wrong for the thing a chart is mostly made
of: settlements cluster, so sites cluster, and with the whole world charted on
seed 4242 four names printed through each other. The gallery's own `ui_map` had
`Taldine` and `Ororell` sharing pixels — a chart on which you cannot read which
dot is which is not a chart.

So the offset is chosen. Twenty candidate positions round the dot, nearest
first, in the order a cartographer tries them — the two flanks at the dot's own
line, then above and below it, then the same again a row and two rows further
out — and the first that is clear of every name already placed, of every site's
dot, and of the chart's own edge is the one used. Greedy and in world-index
order, so it is deterministic for a seed the way everything else here is. If
nothing is clear the name takes the first position that at least stays on the
chart: a nameless dot is worse than two names close together, and a name in the
margin prints over the card's own frame, which is worse than both.

A name pushed off its own flank gets a leader line back to its dot. Without it
the name belongs to whichever dot it landed nearest, which in a cluster is not
its own — it is the difference between a chart and a word cloud.

`fly_ui_chart_labels` exposes the placement for the reason `fly_hud_layout`
exposes the HUD's boxes: two overlapping names are a handful of pixels
different from two adjacent ones, and reading that back out of a hill-shaded
chart is a worse measurement than reading the rectangles the chart was drawn
from. `ui.chart` holds it over four seeds at two frame sizes with every site
charted, which is the worst case rather than the case a new operator sees: 24
names, 0 overprinting, none outside the chart. Eight candidates left one pair
overlapping on three of the four seeds at 960x540; twenty clears all of them.

## The craft card was never wide enough for its own title

`HUD_CRAFT_W` was a flat 134 px, which is the width the four gauge bars want
and has nothing to do with the title row above them. That row is a sprite
column, the airframe's name, and the condition figures hard right:
25 + 57 + 10 + 54 for the *shortest* airframe in the game. So the name was
elided on every frame the HUD has ever drawn — `Skylar..` is in every
screenshot in the gallery — and a drone was worse, because it carries its
endurance on the same row.

The fix is the one `hud_estate_w` already made for the balance cluster, which
used to be a flat 124 px with sixty pixels of empty glass in it: the card is as
wide as what it has to say, floored at the width the gauges want and capped
like the task summary's so a narrow frame gets its width back and elides
instead. The weather rose under it takes the same number, because the
bottom-left stack is one column and so is one width. The figures themselves
moved into `hud_craft_figures`, printed by the card and measured by the card's
width, for the same reason `hud_craft_rows` exists: a row that is drawn is a
row the box was measured for, and that holds across a row as well as down a
card.

Three pixels of slack, and it is not a fudge: the width is computed from the
same two text measurements the drawing then fits into, so without it a name
lands at exactly the space it is given and whether it is elided is decided by
the last bit of a float. Two airframes of eleven came out elided on a card
sized for them.

`hud.layout` holds it over the whole catalogue — through `fly_app_init`, since
nine of the eleven names live in `data/` and a bare `fly_game_init` knows about
the other two. Measured off the pixels, because the arithmetic is the thing
under test: the name's drawn ink has to reach as far as the same name drawn
with nothing in its way. Two runs of the same walk rather than a run against a
text width, because the walk has to join glyphs across the gaps inside a line of
type and a word space in 13 px type is about as wide as the gap that ends the
line — so whatever the join rule does, it does to both. On the old fixed width
the gate reads `Dragonfly Q4, 36.0 px short`.

## The command line

The CLI is a thin shell around the library — `new`, `play`, `render`, `sim`,
`market` — and it had two faults that only show from outside the process, which
is why nothing linking the library had ever caught them. The `cli` aspect shells
out to the built binary and holds both.

**`--ap` took an index into a world nobody could see.** The chart is generated,
only charted sites are legal, and which ones those are differs on every seed —
so the one number the option takes was the one thing the CLI never printed. The
refusal was `autopilot refused (unknown/undiscovered location?)`, a question
with no way to answer it, and the README's own example was `--ap 3`, which on
most seeds is a site nobody has stood on. `market` now ends with every charted
route, its index, its kind, its distance and its gate — the same three facts the
in-game ROUTES list carries — and every refusal prints the same table after
saying which of the four things went wrong.

The reason is read off the world rather than off the return code, deliberately:
`fly_game_apply` documents "0 ok, negative error" and nothing finer, and two of
these refusals genuinely share a code — a route the airframe cannot enter and a
player who is not in an aeroplane are both -3. And an index past the end of the
world is refused before the command is applied at all, because the command's own
answer to one is to switch the autopilot *off* and report success, which is the
right answer to "AP off" and the wrong one to a typo.

**`sim`'s CSV had a sentence in it.** The README redirects that command's stdout
into a file; the status lines saying which save had been opened went to stdout
with the data, so the file began `loaded run.yaml` and nothing that reads CSV
could read it. Status goes to stderr now — it is what this file says about
itself, not what the command produces — and the aspect checks that every row
after the header starts with a number.

## Build, tests and the gallery

Sources compile to their own objects under `build/<rid>/obj`, in parallel and
shared between the CLI and the test binary, so a rebuild costs the slowest
translation unit rather than the sum of all of them, and touching one file
recompiles one file. Changing the compile flags invalidates the lot.

The same suite doubles as a visual-coverage tool: it plays the game headless at
an accelerated (sim-only) rate and renders a wide gallery across pipelines,
settlements, times of day, weather, in-game events, the roads and their
convoys, the trip to space and back, the game on foot, and every screen.

Each frame appears once. A correctness check that renders the same screen a
coverage aspect already shoots writes its still to `build/<rid>/shots` and stays
out of the manifest, so the document never prints one picture twice.

Every still is **timed as it renders**, so the manifest reports the approximate frame rate of
each path — e.g. how the water surface looks and costs under the hybrid pipeline.

The full gallery, with frame rates, lives in
[docs/coverage.md](docs/coverage.md) (refresh with `./make.py --test && ./make.py --shots`).

`--shots` publishes exactly the stills the manifest names and deletes the rest,
which is the right rule after a full refresh and the wrong one after anything
else — `--test --units` leaves a manifest of the handful of stills the
correctness aspects happen to render. The suite records which kind of run wrote
the manifest, and `--shots` refuses anything but a complete, full-quality one
rather than half-publishing over the gallery.

Aspects can be skipped to, or run in isolation, straight from the test binary:

```sh
build/<rid>/test --list             # every runnable aspect (engine, gameplay, gallery)
build/<rid>/test --units            # correctness aspects only, no rendering
build/<rid>/test --cover            # the whole visual gallery
build/<rid>/test --cover --fast     # ...accelerated (smaller frames, fewer samples)
build/<rid>/test --hq water         # just the hybrid-water hero shot, high quality
build/<rid>/test airframes          # 4-view plates of every airframe in data/
build/<rid>/test locations views    # any subset by aspect or group name
```

The `airframes` aspect discovers every asset under `data/airframes/` at runtime
(no hard-coded list), samples a representative spread of module loadouts derived
from the module registry, and renders each as a four-quadrant plate — top iso,
right iso, front iso and a corner perspective — orbit-framed by the airframe's
own wingspan.

`--fast` shrinks supersampling / sample counts / resolution so the gallery
refreshes quickly while iterating; `--hq` bumps them for hero shots.

---

## The multiplayer seam

The command seam is in place and gated: pilots decide through `fly_pilot_think`
(pure, const view), change the world only through `fly_pilot_apply`, and every
command is journalled by actor. `fly_game_pilot_inject` is where a transport
plugs in, and `game.pilots` proves an AI producer and an injected one are
interchangeable. What is left of it is in `TODO.md`.

- **The player is an actor.** `fly_actor` holds what a participant in this world
  *is* — the hull it is flying, the aeroplane that hull adds up to, the stick it
  is being flown with, its money, its freight and whose colours it wears — and
  `fly_game` holds one of them, `player`. It is the head of `fly_pilot`, field
  for field, and `fly_pilot` then adds the machinery of deciding that a human
  does not need.

  It had been loose in `fly_game` since the beginning, and that was the last
  thing standing between this build and a networked one. Every other participant
  was addressable — a struct whose state can be sent and whose commands can come
  off a socket instead of out of `fly_pilot_think`, which is the argument
  `fly_pilot.h` opens with — and the one seat a remote client could not take was
  the interesting one. What is left to make it a slot in an array is bookkeeping.

  What deliberately stayed in `fly_game` is the operator's ledger rather than
  the actor's state: the hangar, the hold, contracts, standing, experience, the
  fail state. Those outlive the hull, and standing outlives the operator; none
  of them is a thing a participant *is*. It is the line `fly_pilot` already
  draws, which is what makes the two structs comparable.

  Three things this did not touch, each for a reason. The YAML keys are
  unchanged — `craft.fuel` is still `craft.fuel` — so every existing save loads;
  the shape of the file describes the game, not the C layout. `fly_pilot_view`
  still carries `player_pos`, `player_airborne`, `player_threat` and
  `player_faction` as loose scalars rather than an actor pointer, and that is
  the point of it: a decision may see where the player is and what flag it
  flies, and may *not* see how much hull it has left. And `player_faction`
  became `player.faction` while `standing[]` stayed, which is the same split
  again — whose side you are on is the actor's, what the powers make of you is
  the operator's.

  A rename of about twelve hundred call sites across the game, the renderer, the
  HUD, the UI, the CLI and the suite, with no behaviour change intended and none
  measured: every aspect reports the same numbers it did before.

---

## Constraints

These bind anything added here and several are enforced by tests.
- **Pure C99, no external source or package dependencies.** Third-party C is
  vendored under `ext/` with its license.
- **The GPU is never a hard dependency.** Without `-DFLY_GPU` the module
  compiles to stubs; with it, a missing device, context or shader must fall back
  to the CPU renderer rather than fail. `--no-gpu` builds and passes.
- **The GPU and the CPU describe the same world.** Every GLSL port of a C
  function gets a parity aspect. The CPU renderer is the portability fallback
  *and* the determinism reference.
- **`fly_gpu` stays scene-agnostic** (a GL service); `fly_render` owns shader
  source and uniforms, so the dependency stays one-directional.
- **Every mutation goes through a command.** `fly_cmd` for the player,
  `fly_pilot_cmd` for everyone else. Commands validate proximity, ownership,
  stock, slot compatibility and mode. Anything that reaches around them is
  behaviour a networked human could not perform.
- **Decisions draw from `think_rng`, never from `rng`.** A client that does not
  run the AI makes none of those draws; sharing the stream desynchronises it
  within seconds.
- **Serialize catalog references by stable string ID**, never by registry index.
- **Fixed-capacity arrays with documented overflow behaviour.**
- **`build/<rid>/fly.h` is generated by `make.py --roll`** and compiled by it;
  never edit it by hand.

---

## What is already gated

Breaking any of these fails the suite. Listed so a change can be weighed against
what it would cost.
| Aspect | Holds |
| --- | --- |
| `gpu.parity` | GLSL value noise against the C twin, 3.6e-07 |
| `gpu.ground` | terrain height incl. pad blend and road cuttings, 5 cm, on a patch over each |
| `gpu.texture` | exact fp32 across two units |
| `gpu.shadow` | the shadow lookup, 0.0000 on the surface |
| `gpu.mesh` | hardware depth rejection |
| `gpu.cascade` | the GL cascade build against the C rasterizer, texel by texel |
| `gpu.frame` | both pipelines, every settlement kind |
| `render.aa` | error against a 16-sample reference, split horizon vs near field, plus the one camera that reproduces varying extrapolation |
| `render.banding` | no quantisation banding across a sky gradient |
| `render.lod` | no holes at terrain LOD boundaries, off the z-buffer, both backends |
| `render.modes` | raster, hybrid and path-traced each render a frame with a healthy colour spread |
| `render.pad` | deck roundness read off the z-buffer |
| `render.shadow` | the map against a ray-marched ground truth, the cascade ladder's texel sizes and ratios, that the lattice holds still both creeping sideways and climbing, and that a wood shades its own floor the same whether the crowns are cast or the medium stands in for them — 0.847 against 0.866, over open ground beside it at 0.999 |
| `render.night` | the star field stays in the sky: no isolated bright pixel on terrain in a traced night frame |
| `render.coast` | the sea floor descends away from the beach, and the rasterizer and the tracer draw the same water over it |
| `render.material` | ground relief on both backends — 16.1 at 40 m against 15.7 at 160 m with the framing matched, where the software path used to be exempt because it had no fragment stage for terrain — the projected-size LOD rule, coarse-cell albedo against the cell average, the mesh's own sampling rate against the pixel's over 950 m cells (0.0815 of change to the next cell against the pixel rate's 0.0862, on true cell averages that change by 0.0614), the far ground against the path tracer over a band six to sixteen kilometres out (hue curvature 0.00422 against the tracer's 0.00574, where shading it per vertex reads 0.00200), open water against a 16-sample answer, and the vegetation response against a rock control |
| `world.forest` | woodland is a landscape: the field partitions the world into open ground, edge and closed canopy with the edge the smallest of the three; it correlates at 100 m and not at 2 km, so it makes stands rather than static; the floor under canopy is measurably darker than matched open ground beside it; and a frame over a wood carries several times the detail of one over a meadow |
| `render.occlusion` | the traced term answers with the geometry rather than a density field, converges to the same number however it is visited, and knows what is built: sky seen on a settlement's apron is 0.135 where something stands over the ground against 0.935 in the open |
| `render.ambient` | the ambient half is an integral, not a table: a wall facing the sun takes at least 1.35x the skylight of the wall behind it (a function of n.z alone returns exactly 1.00), the nine-coefficient fit is within 9% in level and 12% in hue of the same hemisphere brute-forced over a thousand directions, grass does not brighten by more than 6% as the view goes grazing, the ground bounce is green over a meadow, warmer and stronger over sand and dark blue over water, and dawn skylight is less blue than noon's — held as an ordering on the light the world is lit by, cloud included, and as a factor on the air alone, 3.05 against 3.41 |
| `render.atmosphere` | the air behaves like air: noon sunlight is within a fifth of the colour Earth's is (1.28 against 1.22, where thickening the vertical column put it at 1.94) and a morning zenith is blue at better than 4.4; the zenith is blue and the horizon washes toward white; sunlight reddens monotonically as the sun drops, through the transmittance rather than on a schedule; aerial perspective loses blue first, and as distance grows what survives goes to nothing while what the air adds converges exactly on the sky in that direction; night is under a tenth of noon and not black; and the sky around the horizon is smooth — neighbouring azimuths within 1% of each other outside the sun's lobe (78% on the code that grew the skyline slabs), and no kink up a vertical arc that survives sampling it a hundredth of a degree at a time (0.05% against 3.97%, and the old failures all land within half a degree of where the march's 60 km cutoff crossed the cloud base); and the deck is weather rather than a ceiling — cloud tops vary by 81 m of standard deviation over a 2500-2900 m range and correlate with the column's own depth at 0.99, where the plane they used to sit on measures zero, while the base under the tallest column and under the shallowest is the same 2340 m; the deck is a medium and not a decal — one ray along the layer picks up 0.52 of it over 300 m, 0.95 over 1200 and 0.99 over 4800, where a fixed alpha per tap returns the same number for all three — and it is not a light at night, its base reading 0.0070 against 0.0048 of the sky it covers where the borrowed moonlight floor put it at twelve times that; and there is a second layer seven kilometres up that is present on 13% of columns and absent from the rest, sits above the deck at every bearing from between the two, and holds the same elevation-kink bound in its own band at 0.099%; and it casts — 0.856 of the sun under the sheet against 0.9995 with nothing over it, worst 0.714, so the sun goes soft rather than out, and the two layers do not compose as a product but sit 0.108 above one where both are on the beam; the deck has two faces, a night overcast reading 1.38x brighter looking down onto its crown than up at its base where the same measurement was 1.05x with the term out, bounded above as well so the crown is a cloud top and not a white lid, and still varying by 37% of its own mean across bearings over a broken deck; and the clear-air probe is that sky minus the layers and nothing else, bit-identical where neither is on the ray |
| `render.cine` | the per-biome grade differs by place and is bounded on every channel — a tint that can reach a colour cast eventually ships as one — and a propeller at full throttle is a disc you can see through: measured off the disc's own geometry rather than a copy of it, against the sky on each sample's own screen row, nothing in it darkens the sky behind by more than 3% (bound 20%), it is denser at the root than at the rim by 1.50x rather than the other way about, and it darkens the sky by 0.022 where the same aeroplane at idle darkens it by nothing at all, so deleting the disc cannot pass |
| `world.river` | there is water on the land and it behaves like water: every one of six seeds carries at least one watercourse, each of them descending station by station with no ponded span, ending at the level of the sea or of the lake it runs into, and cut deep enough that the drawn surface is never under the drawn bed (2.4 m of channel at the shallowest); it meanders rather than turning corners, at worst 10.9 degrees between two spans and 6.3-7.2 on average where simplifying on chord error left fifty; and a body of water stops under the ground rather than on it — the largest step in `fly_world_surface` across a section is 0.52 to 0.97 m, the bank's own slope, where the level carried flat to the edge of its own footprint measured 3.0 to 10.8 m |
| `world.tilth` | the country between the settlements is worked: a tenth of the land is farmed and it is a belt rather than a ring (82% of the belt's own band, where the term that could not reach zero read 94%), no field further from its parish than the cull the GPU is handed (3527 m against 3746) and none inside the built clearing, the wood gives way to the plough (canopy 0.000 on worked ground against 0.941 beside it), a crop is constant across a parcel and independent of its neighbour (93% at ten metres, 0% at three hundred), the hedge is walked onto the boundary it is painted on (1.62 m at worst, 41.9 points a cell, no double-planting between cells) and a farm's whole yard stands on ground it can stand on — every corner dry, off the carriageway and within two metres of the ground under the barn |
| `gpu.tilth` | the worked field on both sides of the seam: work, crop and boundary distance against the GLSL twin over the whole belt — unverified, because nothing here has a GL driver |
| `render.pylons` | there is a grid in the country between the parishes, and it survives the range it is seen at: a line beside every road, one side for its whole length, on an even pitch of carriageway to the millimetre end to end and running to the far terminus, every standing mast dry, clear of the settlements and 23.6 m off the nearest carriageway at worst, 61% of them on ground no parish works; and the drawing holds together as it recedes — a tower at 300 m covers 21 rows of a 320x180 frame with a longest unbroken run of 21, where the same tower drawn at its own sub-pixel width covers 14 with a run of 8, and the conductors between two towers are 350 pixels of a mid-span window at 260 m and none at 2400 |
| `world.canopy` | a wood is lit like a wood: sun visibility on the ground under closed canopy is materially lower than in the clearing beside it at matched distance, and the canopy's own pixels carry a gradient across each facet rather than resolving flat |
| `world.siting` | nothing built in water, on the rail or on a deck, and no piece left with a corner in the air; the route curvature-bounded, around water, clear of both pads; and home is somewhere worth settling — the chart origin is searched over the ball for a coast and no wall of mountains, so all four seeds place, route and terminate rather than five in twelve |
| `world.planet` | it is a planet rather than a heightfield: 73.4% of the surface is under water against Earth's 71%, 67.6-78.7% over eight seeds; the water is in basins and not in spots, with land following land at 1.74x the base rate 150 km away against the 1.00 a scatter of ponds scores; the coastline has detail below eight kilometres, crossing 1.41x as often when walked at 500 m as at 8 km; aridity is 0.34 in the horse latitudes against 0.02 at the equator and 0.31 in continental interiors against 0.11 on the coasts; the snow line is 1909 m at the equator — the midpoint of the band the ground shader lays snow over — and passes the waterline at 59 degrees, which is the cap; and every one of sixteen seeds puts home on a temperate, watered coast with land under 300 m mean and a range of at least 450 m behind it |
| `game.market` | buying, selling, refuelling and repair move tokens, cargo and site stock consistently |
| `game.delivery` | a contract can be accepted, flown under autopilot and delivered |
| `game.touchdown` | autopilot arrivals land within the pad's tolerances |
| `game.dogfight` | a gun kill pays a bounty and awards xp |
| `game.saveload` | a game round-trips through YAML unchanged; a save whose indices do not fit the world it names — where the operator is docked, the autopilot's target, the launch site, the player mode, a contract's endpoints and commodity — loads to values inside it and keeps running rather than subscripting off the end of the tables; and a write that fails leaves the previous save where it was, with no scratch file beside it |
| `game.autopilot` | every reachable site is flown loaded and *reaches its pad* — the stopped-short bucket is empty, not merely small — the approach is tracked rather than chased (24 m mean over the pad as the apron arrives), an aeroplane parked seven hundred metres past its pad and pointing away comes round and docks, and the leg is bounded by what it cost the airframe — 6.3 points of structure a leg to something other than gunfire, worst 21, where the straight-in profile spent 18.1 and 83 |
| `game.convoy` | the road network is a network: no link laid twice, every deck above the drawn ground, every truck in its own lane |
| `world.sea` | there is a network on the water: 44 quays and 25 lanes over six seeds and every one of them has a lane; a berth is dredged to the draught the survey ran with and has a harbour's beam either side of it rather than a channel's; a jetty is rooted on dry ground, out of any carriageway, and points at the water; a lane is navigable for its whole length — at every station and three samples a span between them, which is what stops one stepping over a spit — ends at both berths, holds the design curve at all but 15 stations in 2250 and nothing tighter than 400 m where a strait pins it, and is marked by lateral buoys alternating half a fairway off the centreline; and the whole network reproduces itself from the seed |
| `game.coaster` | and there is freight on it: hulls on the lanes at world-gen carrying what the market wants moved, each of them to starboard of her own course by a side and inside the fairway, the ladder afloat a ladder and a different one from the road's, a voyage that ends with the cargo in the destination's own stock, the lanes restocked four hours on, and a hull that burns under the guns, sheds what a column sheds, shoots back while she does, and leaves nothing on the surface a minute later |
| `game.pilots` | a populated world gets deliveries done and moves the market, its command stream replays exactly with the AI off, and the flight model keeps the fleet in the air when every pilot is flown at full fidelity |
| `hud.layout` | at three frame sizes, busy and idle: the event log's glyphs are centred on the frame, every cluster the HUD is built from clears every other by 6 px (the nav buttons included), the ink inside a cluster lands exactly on the shared row pitch, nothing is drawn in the touch throttle's column, and nothing touches a frame edge |
| `hud.glass` | the chrome reads over snow as well as over a night field: over four backdrops from black to white the levelled pane lands within a pixel value of the same place (57.3 to 58.0, against 30 to 200 for the one-sided shade it replaced and 246 on a 244 field for no levelling at all), white type keeps 197 levels over the pane it sits on, the actions window stays 48 levels above the page it lies on rather than collapsing into it, and the widgets that deliberately carry no pane — the tapes, the heading strip — hold their own contrast in the contact shadow under each glyph |
| `hud.centre` | glyphs centred on a point are centred on their marks and not on the box the typeface reserves: eighteen strings over three sizes land within a pixel of the point, and the `?` on the help button — which has no descender, so a box centred on its type size pushed it two pixels up inside a 26 px circle — is measured against that button's own centre off a rendered frame |
| `ui.sprite` | every icon in the set, at every size the game draws one: it renders at all (the palette is keyed by icon and the artwork is a switch, so an icon nobody wired up draws nothing, silently), it is shaded rather than a flat cut-out, it is centred on its own ink rather than on its origin, and no two silhouettes are the same shape — the fuel can and the cell were both rounded rectangles of the same width, which put the tank gauge and the airframe-life gauge under indistinguishable sprites |
| `sim.orbit` | the field is inverse-square to one part in a hundred thousand and the integrator reproduces its closed-form consequences: a circular orbit holds to 70 m over a full revolution at zero g-load, an eccentric one reaches Kepler's apoapsis at Kepler's half-period (26,239 km against 1,833 without the angular-momentum term), escape speed escapes and 6% under it turns round; and weight at cruise is within a tenth of a per mille of the flat 9.81 the aeroplane game was tuned against |
| `sim.ascent` | space is gated by physics, not permission: a stock airframe with unlimited fuel and its nose up for forty minutes tops out at a tenth of the Karman line because thrust and lift both go as air density, while the full rocket stack flies a gravity turn to a 151 km apoapsis with 2325 m/s of horizontal speed against the 2304 circular there — and the turbine adds nothing at all in vacuum while the motor adds 66 m/s in four seconds |
| `game.saucer` | one saucer in a populated world, above the line with nothing else within half of it, weightless and holding station to 0 m of ground track over five minutes where an orbit would have carried it 661 km, and its wreck drops four items at a weight nothing else in the game reaches, on the ground underneath rather than where it was hanging |
| `game.space` | the spaceframe is the end of the ladder and its tiers mean something: five parts across five slots all at the ilvl 50 floor, zero rolls of any of them on the 24 of 35 service sites below it over five worlds and at least one supplier in every world, each part's own affix reachable (a motor to +9% vacuum thrust, a tank to +7%, thrusters to +20%) and never in pool for anything without a candle, and every affix in the table prints a nonzero magnitude for its own strongest roll |
| `render.spaceframe` | a build is visible from outside it: each of the five FLY_VIS_* bits moves at least 0.4% of a studio frame on its own and the stack together 10.7%, the registry's `visual` agrees with the geometry drawn for it, and a saucer is not an aeroplane — overhead its planform fills 0.77 of its bounding box against an aircraft's 0.27, which is a shape and not a paint job |
| `sim.weather` | weather is a property of the air: the storm belt is unchanged at 300 m, 3 km and 6 km, thins above, and at 150 km there is no wind, no rain and no turbulence — so an orbit flown off two different noise streams comes out bit-identical where the ground's storm moved it 3.9 m/s apart |
| `data` | `data/modules.yaml` mirrors the built-in registry: same count, every id present, and slot, cost and `min_ilvl` agreeing on all seventeen |
| `render.planet` | the world is a ball and looks like one: the horizon dip is arccos(R/r) to 0.0025% at four altitudes from 300 m to 150 km, the grazing range matches sqrt(2Rh+h^2), straight down is exactly the altitude, the surface normal at a hit is the sphere's rather than the chart's, the disc shrinks between 600 km and 1800 km at 4.98 against the 5.00 tan^2(asin(R/r)) predicts — which a flat plane cannot fake — and at dawn the frame carries a lit face, a dark face and black space at a mean of 0.0002 |
| `gpu.parity3` | the 3D value noise the wrapping chart is built on reproduces its C twin to 2.6e-07 over 4096 samples of a diagonal through the volume, at four octaves |
| `rng` | the 3D noise can stand in for the 2D one, which is a claim about spread and not about range: `fly_fbm2` and `fly_fbm3` are within 1% of the same standard deviation at one octave and at five, over 32k samples each. Without the normalization they differ by 16%, both still inside [-1, 1], and every terrain constant tuned against the 2D field quietly means something else |
| `world.chart` | the flat chart is a map of the ball: radial distance out from the origin matches the great circle to 4.9e-06, the tangential scale is sin(d/R)/(d/R) to five figures at 30, 500 and 942 km — 0.99958 across the band the game is played in — a full 3770 km lap in twelve different bearings lands within a metre of home, and a position folded past the antipode agrees with its unfolded self on the sphere to 359 m |
| `game.factions` | seven powers with ground, no seed leaving one empty-handed; the same seed generates the same world name for name and owner for owner, so nothing was reseeded; three to six of twenty-one pairs open at war, so it is a landscape rather than a brawl; a held field falls to sustained pressure in hours and falls weakly held; a pact is not a slow takeover of your partner; over six hours of live world the map moves and nobody takes half of it; two identical worlds agree site for site and relation for relation after two hours; the oath is refused below its threshold and costs standing with the sworn power's enemies; and faction kit never turns up in ten thousand open rolls |
| `game.ruin` | the run can end; it never ends while a reachable wreck could still buy a hull; salvage answers to where the crash was, not how hard; repair does not give lifetime back and a spent hull is condemned; a restart clears the estate and leaves the world, its clock and the last operator's wreckage alone |

---

## Measured dead ends — do not retry

Each of these was built, measured and reverted.
- **Raymarching the raster environment** in a fullscreen shader, the way the
  path tracer does. ~40x *slower*: `loc_city` went 206 ms -> 8083 ms at 480x270
  ssaa2. Raster mode is cheap because it rasterizes a few thousand shaded
  triangles; raymarching turns that into a 518k-pixel march evaluating 5-octave
  fbm per step. This is why the GPU rasterizer uses real vertex buffers.

- **Fading procedural detail on distance** rather than on projected pixel size.
  The same tussock is four pixels across at 320 wide and twenty-four at 1920, so
  a fade tuned to stop one resolution shimmering strips the other bare — and it
  put horizon-band error against a 16-sample reference up 32%.

- **One octave of ground relief.** A single frequency at a fixed amplitude reads
  as a regular stipple laid *over* the ground rather than as ground. Worse than
  none in the middle distance.

- **A water budget in the rail's curvature relaxation** (allowing N spans over
  the sea) instead of the adjacency rule. It cannot tell lengthening a committed
  crossing from starting a new one, and traded six percent more sea for four
  degrees of turn.

- **Lifting the rail's water veto wholesale** during relaxation: smooth, and 42%
  of the route over open sea.

- **A bigger shore standoff** for the rail: made the worst corner worse.

- **Terrain-albedo footprint filtering** as a shimmer fix: 0.05 luma of movement
  against ground truth, i.e. nothing.

- **Bounding the autopilot's hold-off by the apron it has left.** The float is
  real — eight metres at the flare's seven tenths of a metre a second is eleven
  and a half seconds, four hundred and sixty metres of ground at approach speed,
  so the wheels come down past the pad. Asking for `flare * closing / apron`
  instead, which is the sink that arrives where the profile means to, was tried
  three ways over the thirteen legs of seed 4242 and every one cost structure
  for no change in outcome: capped at the 3.0 m/s `fly_sim` will still take as
  an arrival it touched down at 4 to 5.5 and lost 45 points a leg; capped by a
  round-out on the height underneath (`flare * 0.8`, then `0.55`) it flipped
  which sites failed and still spent 144 points against 109; applied only past
  the aim point it spent 144. The reason is authority, not the demand: the pitch
  loop saturates at +0.55 and cannot arrest four metres a second in the last
  three, so any demand steep enough to shorten the float is a demand the flare
  cannot round out of. It also digs the aeroplane into terrain standing above
  the pad, which is what the flat seven tenths quietly protects against. The
  lever is the energy at the flare — the approach speed is 1.6 times the stall,
  where airmanship says a third again — and that is a wide number with its own
  sitting.

### Measurements that were wrong, and why

Recorded because the mistake is easy to repeat, not the fix.
- **Mean near-field error as a multisampling metric.** 99% of near-field pixels
  are interior to one surface and differ by one or two of 255 regardless —
  llvmpipe's multisample resolve is about half a percent darker than the
  single-sample path even where all four samples agree. That floor swamps the
  mean; moving one building flipped the sign of the comparison. Count outliers,
  and pin the camera that actually reproduces the artifact.

- **A column or radial profile to find a camera-locked LOD seam.** Across two
  hundred camera positions the best image statistic still could not separate a
  seam from a coastline lying at the right radius. Test the claim the fix rests
  on — that a faded evaluation lands nearer the cell average than a point sample
  — which is a statement about a function and measurable to any precision.

- **"Open sea is featureless, so any pixel variation is aliasing."** False: at
  1500 m the 42 m swell is ten pixels across and supersampling reproduces it
  exactly. Only comparison against a supersampled render separates the octaves
  below a pixel from the ones above.

- **Turning the camera to put the sun behind you, to measure the canopy
  response.** That also changes which slopes you are looking at, and plain
  Lambert already brightens the faces turned toward you: the confound was worth
  27% against the 12% the vegetation terms are worth. Evaluate the function at a
  fixed normal with only the view direction moving, and keep a rock control.

- **Benchmarking on a loaded machine.** Two figures were reported wrong before
  being caught by interleaved A/B runs — a +6% that was really +13%, and a
  "20% speedup" that was noise. Interleave the two builds in one session and
  take the minimum of several runs.

## Windows

The build had never been run on Windows, and two things stopped it the first
time it was. Both are about the platform's headers and shell rather than about
the code, and both are worth recording because they will come back the moment
anyone touches the same seams.

- **`windef.h` defines `far`, `near`, `FAR` and `NEAR` as empty macros.** It is
  pulled in by `windows.h`, which `fly_render.c` includes for the worker
  threads and the phase clock, and the renderer uses all four as identifiers —
  a local `float far`, a `static const int FAR[5]`, a `fly_v3 far`, an `int
  near`. On MinGW that is not a warning, it is a syntax error at every use:
  `float far = ...` expands to `float = ...`. The macros are `#undef`'d right
  after the Windows includes, which is the standard portable answer and the
  only one that does not rename a hundred identifiers. Nothing else in the
  tree includes `windows.h`, so the fix is one file.

- **The `cli` aspect shells out through `system()`, and cmd.exe parses
  `build/win-x64/fly99` as the command `build` with a switch.** `system()` on
  Windows runs `cmd.exe /c`, and cmd treats a forward slash as an option
  introducer even in a command name, so the exe path must use backslashes —
  and mixed separators are rejected too, so the whole command line is
  converted. The redirect targets are fine either way. The aspect had been
  failing every check on Windows with `'build' is not recognized as an
  internal or external command`; with the path fixed it passes 33 checks,
  including that `market` lists the charted routes and that `sim`'s stdout is
  a CSV and nothing else.

  The same pass fixed `discover_airframes`, which listed assets with `ls` —
  a POSIX command that does not exist on Windows — and now uses `dir /b` and
  completes the bare names to loadable paths. The `airframes` gallery aspect
  had been silently finding no airframes at all.

- **Windows renders on the GPU too, over WGL rather than EGL.** There is no
  EGL on Windows, and the first pass at the platform answered that by building
  it CPU-only: a `--window` build could not even link against the headless GPU
  path, because TIGR's Windows GL backend defines its own *global* `gl*`
  function-pointer variables and `libGLESv2.dll.a` exports the same names as
  import stubs, so the two collided with a wall of `multiple definition`
  errors. That made a machine with a real GPU render its frames on one CPU —
  the fallback, not the renderer — and `--no-gpu` is meant to be the only
  thing that does that.

  So `fly_gpu.c` has a second context half. Windows gets a WGL context on a
  hidden one-pixel window (a GL context needs a device context, and a device
  context needs a window; nothing is ever drawn into it, every frame goes to a
  framebuffer object), asked for as a 4.5 then 4.3 core profile through
  `wglCreateContextAttribsARB`, falling back to the context `wglCreateContext`
  gives. Every entry point past OpenGL 1.1 is loaded through
  `wglGetProcAddress` into a table the module declares itself, which settles
  both problems at once: no GL SDK has to be installed (`opengl32.dll` ships
  with the OS, and the LLVM toolchain `setup.ps1` installs carries no GL
  headers), and every name has internal linkage, so TIGR's globals have
  nothing to collide with. `python make.py --window` now links the windowed
  backend and the GPU renderer together.

  Linking was not the whole of it. A windowed build shares one thread with
  TIGR, and TIGR's backends take the thread's GL context for the length of a
  window update and hand it back *released* — `wglMakeCurrent(NULL, NULL)` on
  Windows, `glXMakeCurrent(NULL, 0, 0)` on X11 — so the renderer's next frame
  would issue its GL calls with no context current and quietly draw nothing.
  Every entry point into `fly_gpu` now asks `gpu_live()` rather than reading
  `G.ready`, and that takes the context back when something else has taken it;
  it is a pointer compare when the context is already ours. The X11 half of
  that had been wrong for as long as `--window` has been able to build with
  the GPU path.

  The shaders did not change: they are ES 3.1 on both platforms, which a
  desktop driver compiles through `ARB_ES3_1_compatibility`. That is a driver
  promise rather than a given, so `fly_gpu_init` compiles the smallest shader
  in the dialect before it reports a GPU at all — a driver that cannot take it
  (or one stuck on the GDI software renderer, where the loader finds no
  `glCreateShader` to begin with) fails init with a message and the CPU
  renderer takes the frame, which is what the fallback is for. `gpu.service`
  holds both ends: a live service builds an ES 3.1 program, and an unavailable
  one says why.

