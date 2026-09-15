/* fly_sea: the network on the water, and where the land meets it.
 *
 * A third of this chart is sea. Until there was a lane on it the only things
 * ever drawn out there were the rail's piers and a road's causeway. The water
 * had weather over it, waves on it and nothing in it, which is a third of
 * every flight spent over scenery.
 *
 * What belongs on water is what belongs on a road: freight going somewhere,
 * worth escorting and worth robbing. So a lane is the road network's argument
 * run at sea, and almost none of it is new. The survey is fly_rail_lay's, with
 * its medium turned over — water is the cheap ground, land is what the line
 * may not cross, and the standoff that keeps a road off the beach keeps a hull
 * off the rocks (see fly_rail_shape's `sea`). The line itself is fly_line's,
 * the same stations and the same arithmetic the carriageway and the guideway
 * are read with. What runs along it is fly_convoy, with a class table of its
 * own: a coaster is a column of trucks that floats, and everything that made a
 * column interesting — a tier off the danger field, guns that answer, freight
 * that moves the market, loot when it burns — is already written and does not
 * care what is under the wheels.
 *
 * What is new is the two ends. A road arrives at a settlement's gate; a lane
 * cannot, because a settlement stands on dry land and the lane does not reach
 * it. So the network is laid quay to quay: for every site that can reach the
 * water, the bearing on which it does, where the jetty leaves the land, and
 * where a hull can lie alongside it. That point is the terminus, and it is
 * also where the shore gets built on.
 *
 * Everything here is a function of the world alone, so nothing is saved: the
 * lanes are rebuilt from the seed with the terrain exactly as the roads are,
 * and the ships on them are repopulated from the market exactly as the columns
 * are. */
#ifndef FLY_SEA_H
#define FLY_SEA_H

#include "fly_line.h"
#include "fly_rail.h"
#include "fly_road.h"
#include "fly_world.h"

/* Lanes per world and stations per lane. A lane at FLY_LANE_SPACING fills its
 * array at 41 km, which is longer than any link the network accepts.
 *
 * Coarser than a road's sixty metres, and the reason is the same one that set
 * that number: the pitch and the tightest curve are one decision, because a
 * station turns by about span over radius. At 130 m against a 900 m radius
 * that is eight degrees a span, which the arc through the stations rounds off
 * — and there is nothing on open water for the pitch to have to resolve, no
 * shelf to touch down on and no gradient to kink. */
#define FLY_LANE_MAX 10
#define FLY_LANE_POINT_MAX 320
#define FLY_LANE_SPACING 130.0f
/* The tightest curve a lane holds. A loaded hull turns on its own length
 * several times over and does it slowly; nine hundred metres is a wide,
 * unhurried alteration of course rather than a corner, which is what a lane
 * drawn between two headlands should look like from three thousand feet. */
#define FLY_LANE_MIN_RADIUS 900.0f
/* Half the fairway: how far off the centreline a hull keeps. Vessels pass
 * port to port, so a ship stands to starboard of its own lane exactly as a
 * lorry keeps to the near side of its own carriageway — one convention,
 * written down once, so that two coasters meeting do not sail through each
 * other. */
#define FLY_LANE_HALF 90.0f
#define FLY_LANE_SIDE 45.0f
/* Nobody ships further than this to the next harbour, and no lane is laid
 * shorter than the offing at both ends put together. */
#define FLY_LANE_MAX_LENGTH 34000.0f
#define FLY_LANE_MIN_LENGTH 2600.0f
/* Lanes meeting at one harbour. The road network's number, for the road
 * network's reason: three is a junction and more is a knot. */
#define FLY_LANE_DEGREE_MAX 3
/* What a hull wants under its keel before water counts as navigable. It is
 * the number the survey is run with, the number a berth is dredged to and the
 * number the tests measure the finished lane against, so it is written down
 * once. */
#define FLY_SEA_DRAUGHT 6.0f
/* How far short of the quay head a lane stops. The berth is where a hull ties
 * up and the lane is where it runs; the last two hundred metres are the
 * approach, and a lane laid onto the quay itself would put a coaster alongside
 * at fourteen knots. */
#define FLY_SEA_CLEAR 240.0f
/* What a lane will carry, in metres a second. A coaster makes about eighteen
 * knots and a laden one rather less — see the class table in fly_convoy.c,
 * which is where the difference between rungs is decided. */
#define FLY_SEA_SPEED 9.2f

/* --- where a settlement meets the water ------------------------------------
 *
 * The quay, and it is the whole of the join between the two networks. `root`
 * is where the jetty leaves dry land, `head` is the berth at the end of it —
 * on the water, in water deep enough to lie in — and `yaw` is the bearing from
 * one to the other, which is the direction everything built here faces.
 *
 * A site with no water inside FLY_QUAY_REACH has no quay, which is most of
 * them: a mine in the interior is not a port and should not grow a crane. */
typedef struct {
    int loc;          /* the settlement it serves, or -1 for none */
    fly_v3 root;      /* on the ground, at the landward end */
    fly_v3 head;      /* on the water, at the seaward end */
    float yaw;        /* bearing from the land out to sea */
    float depth;      /* metres of water under the head */
    float run;        /* metres from root to head */
} fly_quay;

/* How far a settlement will go to reach the water, how far past the waterline
 * a berth may be dredged, and how finely the shore is walked for it. The reach
 * is generous — a harbour town is not built on the beach and the road down to
 * the water is part of the place — and the run is not, because a jetty two
 * kilometres long is a causeway and this is not that. */
#define FLY_QUAY_REACH 5000.0f
#define FLY_QUAY_RUN 900.0f
#define FLY_QUAY_STEP 25.0f
#define FLY_QUAY_BEARINGS 48
/* How wide the water at a berth has to be. A river mouth and a tidal creek are
 * both water deep enough to float a hull and neither is a harbour: asking the
 * same question a hundred and sixty metres either side of the berth is what
 * separates an arm of the sea from a channel across it. */
#define FLY_QUAY_BEAM 160.0f
/* The deck of the quay over the water, and how far it stands out to either
 * side of its own centreline. */
#define FLY_QUAY_DECK 3.6f
#define FLY_QUAY_HALF 9.0f

typedef struct {
    int from, to;        /* location indices; a lane is undirected */
    int point_count;
    float length;
    float speed;         /* what the lane will carry, m/s */
    fly_v2 lo, hi;       /* bounds, so a query rejects a lane it is nowhere near */
    fly_line_point points[FLY_LANE_POINT_MAX];
} fly_lane;

typedef struct {
    int count;
    fly_lane lane[FLY_LANE_MAX];
    /* One per settlement, indexed by location, `loc` negative where the site
       never reached the water. Kept dense-by-index rather than compacted
       because every caller has a location in hand and wants its quay. */
    fly_quay quay[FLY_MAX_LOC];
} fly_sea_net;

/* Lay the network. Deterministic in the world and the roads alone: same seed,
 * same quays, same lanes.
 *
 * The roads are an input for one reason — a jetty may not be built across a
 * carriageway, which is the rule every other structure in this world is held
 * to (see fly_road_blocked). NULL is a world with no roads in it.
 *
 * Nothing here mutates the world. A quay is a structure standing on the ground
 * as found, not an earthwork: there is no sea-level equivalent of a cutting
 * and a lane asks the terrain for nothing at all. */
void fly_sea_build(fly_sea_net *sea, const fly_world *world,
                   const fly_road_net *roads);

/* The quay serving a settlement, or NULL where it has none. */
const fly_quay *fly_sea_quay(const fly_sea_net *sea, int loc);

/* Where `distance` along a lane is, and which way it points there. */
int fly_sea_eval(const fly_lane *lane, float distance, fly_v3 *pos, fly_v3 *tangent);

/* The same point, moved off the centreline into the water something heading
 * `heading` keeps to.
 *
 * Vessels pass port to port, so a hull stands to starboard of the middle of
 * its own lane — the same convention as fly_road_lane's near side and written
 * down in the same one place, because two coasters meeting on a centreline
 * both drawn down the middle of it is two coasters sailing through each
 * other. There is no camber at sea, so unlike a carriageway the height does
 * not move: the water is flat and everything on it floats at the same level. */
fly_v3 fly_sea_side(fly_v3 centre, fly_v3 heading);

/* The lanes meeting at a settlement, written into `out`; returns how many. */
int fly_sea_at(const fly_sea_net *sea, int loc, int *out, int max);

/* The nearest lane to a point: the lane index, or -1 when there are none.
 * `distance` receives how far along that lane the nearest point is and
 * `separation` how far off it the query was. Either may be NULL. */
int fly_sea_project(const fly_sea_net *sea, fly_v3 p, float *distance, float *separation);

/* --- what marks the lane after dark ----------------------------------------
 *
 * A buoy every FLY_SEA_BUOY_PITCH metres of lane, by chainage rather than per
 * station, for the reason the road's lamps are placed that way: a mark placed
 * per station crawls along the lane as the sampling changes, and a mark placed
 * by chainage is at a fixed place in the world.
 *
 * `k` counts from the `from` end. Returns 0 once the lane has run out.
 * `pos` is the buoy's own float, on the water; `port` says which side of the
 * fairway it marks, so the pair either side of the channel are told apart by
 * the colour they are lit — which is the entire point of a lateral mark. */
#define FLY_SEA_BUOY_PITCH 1100.0f
int fly_sea_buoy(const fly_lane *lane, int k, fly_v3 *pos, int *port);

#endif /* FLY_SEA_H */
