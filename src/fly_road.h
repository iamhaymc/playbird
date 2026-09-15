/* fly_road: the ground network between settlements.
 *
 * A rail is one line the player rides. A road is the other thing infrastructure
 * does: it joins the whole map to itself, so that a settlement is somewhere
 * goods arrive from *somewhere else* rather than a pad with a market attached.
 * What runs on it is fly_convoy's business; this module is only the network —
 * where the roads go, how long they are, and where a point is along one.
 *
 * The survey is fly_rail_lay's, because a road and a rail solve the same
 * problem: cross the land rather than the water, round off what the search
 * left, hold a curve a vehicle can take, and stop clear of the pad so the line
 * arrives at a settlement through an opening kept for it instead of running
 * over the top of the deck it serves. They differ in what they do afterwards.
 * A rail is a pipe on piers at a fixed height; a road is laid *on* the ground
 * and follows every rise in it, which is why the two drape their own lines.
 *
 * Everything here is a function of the world and nothing else, so a road
 * network is not saved: it is rebuilt from the seed exactly as it was, the same
 * way the terrain is. */
#ifndef FLY_ROAD_H
#define FLY_ROAD_H

#include "fly_rail.h"
#include "fly_world.h"

/* Roads per world and points per road. A road at FLY_ROAD_SPACING fills its
 * array at 33 km, which is longer than any link the network accepts; a longer
 * one would simply be sampled coarsely rather than truncated.
 *
 * Sixty metres, not a hundred and fifty, and the reason is the curve bound
 * below rather than anything about the plan the search draws. A station turns
 * by about span over radius, so the pitch and the tightest curve are one
 * decision: at 150 m stations a 400 m curve is a twenty-degree crease every
 * span, and no amount of drawing makes a polygon into a road. Sixty puts it
 * under nine, which the arc through the stations rounds off into a curve you
 * cannot pick the stations out of.
 *
 * The profile wanted it too. A taut line over the shelf changes gradient
 * wherever it touches down, and a touch-down between two stations is a kink the
 * vertical curve never gets the chance to round off. */
#define FLY_ROAD_MAX 28
#define FLY_ROAD_POINT_MAX 560
#define FLY_ROAD_SPACING 60.0f
/* The tightest curve a road holds.
 *
 * It used to be 1200 m, which is the radius of a line that is not really
 * turning: at that bound the relaxation flattened every swing the survey asked
 * for and the network came out as two dozen straight lines between settlements,
 * which is the one thing a hauling road is not. Four hundred metres is a real
 * bend — a vehicle takes it at speed and leans — and it is not tight for a
 * road: it is a corner an unloaded truck takes at fifty and a laden column
 * takes at thirty, which is what these are carrying.
 *
 * It is chosen with FLY_ROAD_MEANDER rather than independently of it. The
 * wander's amplitude is derived from this number (see route_meander), so the
 * swing asks for exactly the arc the relaxation will let it keep; the two
 * together are one decision about what a road here looks like, and moving
 * either alone moves the other's answer. */
#define FLY_ROAD_MIN_RADIUS 400.0f
/* How far a road is allowed to swing off the line the search drew. See
 * fly_rail_shape's `meander`, which is where the swing is applied and what
 * bounds it against water and against climbing out of its own valley, and
 * meander_at, which is where this number meets the wavelength it swings on.
 *
 * Two hundred and eighty metres against a two-and-a-half-kilometre swing is a
 * road that visibly wanders without ever looking lost: about a tenth further to
 * drive than the straight line, a broad reversing curve every kilometre and a
 * quarter, and the long snaking line you follow from three thousand feet —
 * which is the whole reason the network is drawn at all. It is also, at that
 * wavelength, an arc of about six hundred metres, which is what
 * FLY_ROAD_MIN_RADIUS is set against. Move one and the other has to move. */
#define FLY_ROAD_MEANDER 300.0f
/* Half the carriageway: two lanes and a shoulder each way, because these are
 * the roads freight meets freight on and a column has to be able to pass one
 * coming the other way. */
#define FLY_ROAD_HALF 7.5f
/* How much the crown of the carriageway stands above its channels. Small — a
 * road sheds water at about one in forty and this is that across 7.5 m — and it
 * is the whole difference between a made surface and a painted strip.
 *
 * It is here rather than in the renderer because the deck is not flat, and
 * everything that puts something *on* the deck has to agree about that: the
 * drawing cambers the surface, and a vehicle keeping to its own lane sits a
 * few centimetres off the crown rather than on it. Two opinions about the
 * shape of a road surface is a truck floating over its own road. */
#define FLY_ROAD_CAMBER 0.19f
/* Where the near-side lane's centre is, in metres off the crown.
 *
 * A carriageway is 7.5 m each side of the centreline, and traffic keeps to the
 * right of it — see fly_road_lane, which is the one place the rule is written
 * down. Half the near-side carriageway puts a truck in the middle of its own
 * side with room for the shoulder outboard of it, and leaves the far half of
 * the road for whatever is coming the other way. */
#define FLY_ROAD_LANE 3.6f
/* How thick the slab is. A road is a made thing sitting on the ground rather
 * than a colour painted on it, and the edge of it is the part that says so:
 * on the flat this is the kerb, and on a hillside it is the top of a wall. */
#define FLY_ROAD_DECK 0.55f
/* How far out the formation is measured: the deck, its shoulders and the strip
 * either side that has to be level with them for the road to sit on a shelf
 * rather than on a knife edge. `road_shelf` takes the highest ground inside
 * this, which makes it the width a cutting has to take down as well — a cut
 * narrower than the shelf is measured over leaves the shelf standing at its own
 * old height and the profile never learns the earthwork happened. That was
 * exactly the first version's bug: an 11.5 m cut against a 13 m shelf dug a
 * trench beside the road and left the road on a bank in the middle of it. */
#define FLY_ROAD_SHELF_R 13.0f
/* How high the deck may be carried above the ground it crosses: the tallest
 * embankment the profile is allowed to build. A road that holds its gradient
 * across broken country does it by building them, so this is not one or two
 * metres — but it used to be a dozen, and with a relaxation that could only
 * raise a station it *reached* a dozen nearly everywhere, so the network read
 * as a system of dams. The taut line spends far less, and six is where a bank
 * stops looking like earthworks. */
#define FLY_ROAD_FILL 6.0f
/* How far the fill spreads for every metre it stands: the batter of the
 * embankment. Tipped material finds an angle and stays at it; a vertical face
 * would be a wall somebody built on purpose, which most of a road is not. */
#define FLY_ROAD_BATTER 0.85f
/* How wide a swathe is cleared for it. Nothing grows inside this. */
#define FLY_ROAD_CLEAR 15.0f
/* The right of way, and the wider apron at the settlement end. Nothing is built
 * inside either — that is what makes a road arrive through a gap in the
 * architecture rather than between two towers, the same way the rail does. */
#define FLY_ROAD_ROW 20.0f
#define FLY_ROAD_GATE 42.0f
/* How far out the apron reaches from the terminus, in metres of road. */
#define FLY_ROAD_GATE_RUN 110.0f
/* The longest run of water a road will cross. A causeway over a creek is a
 * road; four kilometres of open sea is a ferry, and the network simply does
 * not go there. */
#define FLY_ROAD_FORD 300.0f
/* Nobody drives further than this to the next settlement, so the network
 * stops rather than laying a road across the whole chart. */
#define FLY_ROAD_MAX_LENGTH 32000.0f
/* Roads meeting at one settlement. Three is a junction; more is a spaghetti
 * interchange in the middle of a mining camp. */
#define FLY_ROAD_DEGREE_MAX 3
/* --- what is already on the ground -----------------------------------------
 *
 * How wide a berth a road gives a line that is already there — the guideway,
 * and every road laid before it — and what it will pay for one. See
 * fly_rail_shape's `avoid`, which is where the price is charged.
 *
 * Two hundred and sixty metres is a corridor rather than a clearance: inside it
 * two ways across the same country read as one piece of infrastructure drawn
 * twice, which is what the network used to do — a road running a hundred metres
 * off the rail for a kilometre at a time, weaving over it three times on the
 * way. The toll is what makes that expensive and a crossing merely dear: at 1.4
 * a kilometre spent alongside costs a kilometre and a half of detour, and
 * crossing square costs the width of the corridor, so a road goes round when
 * there is a way round and crosses squarely when there is not. */
#define FLY_ROAD_KEEP 260.0f
#define FLY_ROAD_KEEP_TOLL 1.4f
/* And what a road already laid is worth against what the guideway is worth. A
 * road can be crossed — at grade, at one height, which is a crossroads — and a
 * guideway cannot, so the two are not equally unwelcome. Half. */
#define FLY_ROAD_KEEP_ROAD 0.5f
/* Headroom under the guideway: what is left between the carriageway and the
 * underside of the barrel where a road passes beneath the line. A loaded
 * vehicle on these roads is four metres and a bit; five and a fifth is that
 * with the margin a structure is built to, and it is the number the profile is
 * held under and the number the portal is drawn to. */
#define FLY_ROAD_UNDER_CLEAR 5.2f
/* How near the guideway the headroom rule binds, and how far out it is relaxed
 * again. Inside the first the road ducks; between the two it comes back up at a
 * gradient the vertical curve can hold, so an underpass is an approach and a
 * dip rather than a step in the road. */
#define FLY_ROAD_UNDER_R 55.0f
#define FLY_ROAD_UNDER_RUN 150.0f
/* Where a road passes under the guideway.
 *
 * Collected once, when the network is laid, because everything that cares —
 * the structure carrying the line over the road, the piers that must not stand
 * in the carriageway, the lamps that must not either, and the tests that ask
 * how square the crossing is — is asking about the same handful of points and
 * none of them can afford to find them again per frame. */
typedef struct {
    int road;        /* which road passes under */
    int station;     /* the span of it the crossing falls on */
    float t;         /* where along that span, 0..1 */
    fly_v3 pos;      /* on the carriageway centreline, at its own surface */
    float yaw;       /* the guideway's heading here */
    float deck;      /* the guideway's axis height here */
    float headroom;  /* underside of the barrel, less the carriageway */
    float angle;     /* how square it is: pi/2 is a right-angle crossing */
} fly_road_cross;

/* A world with more than a few of these has a guideway laid down the middle of
 * its road network, which is the thing the corridor toll exists to prevent. */
#define FLY_ROAD_CROSS_MAX 24

/* A station of the carriageway. The station itself is fly_line_point — every
 * laid line in this world has the same one, see fly_line.h — and this is the
 * name the network calls it by: `pos` is on the surface of the road, which is
 * on the ground, and `distance` is metres from the `from` end. A road leaves
 * `bridge` alone — a causeway here is read off the water under the station
 * rather than written down — and carrying the flag anyway is the whole price
 * of there being one station type instead of three. */
typedef fly_line_point fly_road_point;

typedef struct {
    int from, to;         /* location indices; a road is undirected */
    int point_count;
    float length;
    float speed;          /* what the surface will carry, m/s */
    /* The bearing of each terminus from the settlement it serves. Kept rather
     * than derived because a settlement's layout wants it every frame and the
     * net does not otherwise carry where the settlements are. */
    float bearing_from, bearing_to;
    fly_v2 lo, hi;        /* bounds, so a query rejects a road it is nowhere near */
    fly_road_point points[FLY_ROAD_POINT_MAX];
} fly_road;

/* A coarse mask of the chart cells a road passes through, dilated by one cell
 * so that anything within a cell of a carriageway is inside it.
 *
 * It exists because the two hottest questions about the network are asked
 * about points that are nowhere near it: every piece of settlement
 * architecture asks whether its plot is on a road, and the tree scatter asks
 * the same of every cell of woodland it draws. Both are answered `no` for
 * almost every point on the map, and answering that by walking two thousand
 * spans is the difference between a query and a frame. One bit says it
 * instead, and only the points that survive it pay for the real answer. */
#define FLY_ROAD_MASK 192
#define FLY_ROAD_MASK_CELL (2.0f * FLY_WORLD_HALF / (float)FLY_ROAD_MASK)

typedef struct {
    int count;
    fly_road road[FLY_ROAD_MAX];
    int cross_count;
    fly_road_cross cross[FLY_ROAD_CROSS_MAX];
    unsigned char mask[FLY_ROAD_MASK * FLY_ROAD_MASK / 8];
} fly_road_net;

/* The deepest cutting the surveyor will ask for, and the least one worth
 * registering. A road that may not cut goes over a ridge at whatever gradient
 * the ridge has, which is where the network's steepest grades were; a road that
 * may cut without limit drives a canyon through the middle of the map. Six
 * metres is a cutting, and anything under a metre and a half is a scrape the
 * embankment either side would have swallowed anyway. */
#define FLY_ROAD_CUT 6.0f
#define FLY_ROAD_CUT_MIN 1.5f

/* Lay the network, and grade the ground it needs graded.
 *
 * The world is mutable because the second half of laying a road is telling the
 * terrain about it: the profile is surveyed against the ground as it was found,
 * and only then are the cuttings it asked for published into `world->cut`,
 * where fly_world_ground will start honouring them. That ordering is the whole
 * of why this is not circular. Every road in the network is surveyed against
 * natural ground first; then one bounded budget is spent across all of them at
 * once; then every road standing on ground that moved — not only the road that
 * asked — is surveyed a second time against the ground as it now is. Nothing is
 * ever laid against a half-graded world.
 *
 * The surveyor keeps working state to do it with — the shelf each station
 * stands on, the chainage, and how deep a cutting it wanted — because the
 * budget cannot be spent one road at a time and the second survey needs to know
 * what the first one asked for. That state used to live on `fly_road`, where it
 * was two thirds of the size of a road and travelled with it into every copy of
 * the game state for the rest of the session. It is a quarter of a megabyte of
 * scratch that stops mattering the moment this function returns, so it lives in
 * fly_road.c instead and `fly_road_net` is the network rather than the network
 * plus the notes somebody made while laying it.
 *
 * `rail` is the guideway, and it is an input for the same reason the world is:
 * it was laid first and it is on the ground the roads are about to cross. The
 * network keeps off it where it can — see FLY_ROAD_KEEP — ducks under it with
 * headroom where it cannot, and writes down every place it had to, so the
 * structure carrying the line over the carriageway is built where the survey
 * actually put the crossing. NULL is a world with no line in it.
 *
 * Deterministic in the world and the guideway alone: same seed, same roads,
 * same cuttings, same crossings. */
void fly_road_build(fly_road_net *net, fly_world *world, const fly_rail_route *rail);

/* Where the line is `t` of the way along span `i`, and which way it points
 * there. Either output may be NULL; returns 0 when there is no such span.
 *
 * The stations are where the survey settled, and the line *between* two of them
 * is a curve rather than the chord. That distinction did not matter while a
 * road was a straight line sampled every 150 m and it matters now that one
 * winds: at a 600 m radius the chord across a span sits a couple of centimetres
 * inside the arc, and — far more visibly — the crease at every station is a
 * flat spot the eye follows all the way to the horizon.
 *
 * So there is one definition of where a road is, and everything uses it: the
 * carriageway is drawn along it, the lamps are stood beside it, and the
 * convoys drive down it. Two of those disagreeing is a column cutting the
 * corner off its own road, which is exactly what a chord-driven truck does. */
int fly_road_span(const fly_road *road, int i, float t, fly_v3 *pos, fly_v3 *tangent);

/* Where `distance` along a road is, and which way it points there. */
int fly_road_eval(const fly_road *road, float distance, fly_v3 *pos, fly_v3 *tangent);

/* The same point, moved off the centreline into the lane something heading
 * `heading` keeps to. `heading` is the direction of travel and need not be
 * unit; a point on a road with no line to speak of comes back untouched.
 *
 * Traffic here drives on the right. That is a convention rather than a fact
 * about the world, but it has to be *a* convention and it has to be written
 * down once: a column drawn down the centreline is a column driving over the
 * white line, and two columns passing on the same road drive through each
 * other. Everything that puts a vehicle on a road goes through here, so the
 * near side is one decision instead of one per caller.
 *
 * The camber comes with it. The deck is a crown, not a plane, so a lane centre
 * is a few centimetres below the middle of the road — small, and exactly the
 * sort of small that reads as a truck hovering when it is missing. */
fly_v3 fly_road_lane(fly_v3 centre, fly_v3 heading);

/* The nearest carriageway to a point: the road index, or -1 if the network is
 * empty. `distance` receives how far along that road the nearest point is and
 * `separation` how far off it the query was. Either may be NULL. */
int fly_road_project(const fly_road_net *net, fly_v3 p, float *distance, float *separation);

/* The roads meeting at a settlement, written into `out`; returns how many. */
int fly_road_at(const fly_road_net *net, int loc, int *out, int max);

/* Where road `road` stops at settlement `loc`, and which way it was heading
 * when it got there.
 *
 * A road is undirected and stops FLY_PAD_DECK_R and a bit short of the pad at
 * both ends, so "the end of this road at this place" is one of two points and
 * the tangent at it points two ways. Whoever builds the gate has to agree with
 * whoever drew the carriageway about both, and a settlement laid out against
 * the wrong end of a road puts its checkpoint at the far settlement. So it is
 * one function: `pos` receives the terminus and `inward` a unit heading from
 * it toward the settlement it serves. Either may be NULL. Returns 0 when the
 * road does not reach this settlement or has no line to speak of. */
int fly_road_end(const fly_road_net *net, int road, int loc, fly_v3 *pos, fly_v3 *inward);

/* Could there be a road anywhere near this point? One bit; see the mask above.
 * A 0 is certain and a 1 only means "go and look". */
int fly_road_near(const fly_road_net *net, fly_wpos p);

/* Metres from the nearest carriageway centreline, saturating at
 * FLY_ROAD_REACH — the query is for verges and clearances, so an exact answer
 * half a kilometre out would be work nobody wants done. The tree scatter reads
 * it to keep a wood off the road; anything else that wants a verge reads it
 * too. */
#define FLY_ROAD_REACH 512.0f
float fly_road_gap(const fly_road_net *net, fly_wpos p);

/* Is this plot standing in a road's right of way?
 *
 * The layout rule asks it of every piece of settlement architecture, which is
 * why it is a predicate rather than a distance: the answer to a plot on the
 * carriageway is that there is no plot, never a shove sideways. Roads whose
 * bounds do not reach the point are rejected without being walked. */
int fly_road_blocked(const fly_road_net *net, fly_wpos p);

/* The bearing a road leaves `loc` on, or 0 with no roads there. Settlement
 * layouts point their gate at it, so the road arrives down an avenue. */
int fly_road_bearing(const fly_road_net *net, int loc, float *out);

#endif /* FLY_ROAD_H */
