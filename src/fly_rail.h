/* fly_rail: deterministic one-way ground-following route queries. */
#ifndef FLY_RAIL_H
#define FLY_RAIL_H

#include "fly_line.h"
#include "fly_world.h"

/* Points in a laid line. Sized for the finest survey any caller asks for —
 * a road at FLY_ROAD_SPACING over the longest link the network accepts — because
 * the working buffers in fly_rail.c are this size and a caller that asked for
 * more would silently get the parts of the survey that fit.
 *
 * The station itself is fly_line_point, which is every laid line's, and this
 * is the name the guideway calls it by. See fly_line.h. */
typedef fly_line_point fly_rail_point;
#define FLY_RAIL_POINT_MAX 576

typedef struct {
    char id[24];
    int from_location;
    int to_location;
    int point_count;
    fly_rail_point points[FLY_RAIL_POINT_MAX];
    float length;
    float clearance;
    float speed;
} fly_rail_route;

typedef struct {
    int route;
    float distance;
    float speed;
    float entry_distance;
    fly_v3 pos;
    fly_v3 tangent;
    int can_exit;
} fly_rail_state;

/* --- keeping clear of a line that is already there -----------------------
 *
 * Two ways across the same country solve the same problem, so left to
 * themselves they answer it the same way: the road the survey draws between
 * two settlements is very nearly the guideway somebody already built between
 * them, and what comes out is a road running a hundred metres off the rail for
 * a kilometre at a time and weaving back and forth over it. That reads as one
 * piece of infrastructure drawn twice rather than as two, and every place the
 * two lines touch is a carriageway and a pipe occupying the same ground.
 *
 * So a line can be told what is already on the ground. Every step of the
 * search inside `reach` of an existing line pays `toll` metres of detour per
 * metre travelled, which prices the two things a shared corridor is made of
 * very differently: running *along* another line pays for its whole length,
 * and crossing one pays for the width of the corridor and nothing more. That
 * is the whole of the rule — no line is forbidden anything, it is priced —
 * and the result is a network that goes round rather than alongside, and
 * crosses square when it has to cross at all.
 *
 * `weight` is per point and scales the toll along the existing line, so a
 * caller with somewhere it would rather be crossed can say so: the road prices
 * the guideway by how much room there is under it, and a crossing lands where
 * the piers are tall enough for a lorry to pass beneath. NULL is a line that
 * is equally unwelcome everywhere. */
typedef struct {
    const fly_v2 *pos;    /* the existing line, in chart metres */
    const float *weight;  /* per point, 0..1, scaling the toll; NULL for all 1 */
    int count;
} fly_rail_line;

#define FLY_RAIL_AVOID_MAX 32

typedef struct {
    const fly_rail_line *line;  /* the lines to keep clear of */
    int count;
    float reach;  /* how wide the corridor is, in metres */
    float toll;   /* metres of detour paid per metre on its centreline */
} fly_rail_avoid;

/* --- laying a line between two settlements ------------------------------
 *
 * The shape of the line, as against what runs along it: how far short of each
 * site's centre it stops, how finely it is sampled, and the tightest curve it
 * is allowed to hold. Everything else about a route — deck height, speed,
 * whether water is bridged or refused — belongs to whoever is laying it.
 *
 * `fly_rail_lay` is that surveyor, and it is public because a road is the same
 * problem: find the way across the land that avoids the water, round the
 * corners the grid left, bound the curvature, and stop clear of the pad at each
 * end so the line arrives at a settlement instead of through it. The rail and
 * the road disagree about width, gradient and what they do at a shoreline, and
 * about nothing at all in the survey — so there is one survey.
 *
 * The answer is a planar centreline in chart metres, which is the frame the
 * whole thing is worked out in: a line joins two settlements, both inside the
 * thirty kilometres the chart is flat across to four decimal places. Draping it
 * on the ground is the caller's job, because the ground is where the two part
 * company. Returns the number of points written, or 0 when there is no line to
 * lay. */
typedef struct {
    float clear;      /* stop this far short of each terminus centre */
    float spacing;    /* metres between sampled points */
    float min_radius; /* tightest curve the finished line may hold */
    int grid;         /* survey resolution, cells per side; 0 for the finest */
    /* Metres of route the survey will spend to avoid a metre of climb. Zero
     * ignores the rise entirely, which is what a line on piers wants and what
     * the search has always done; anything that is laid on the ground wants a
     * number here, or it walks over whatever is in the way. */
    float climb_toll;
    /* How far the finished line is allowed to swing off the survey's own
     * answer, in metres of lateral amplitude. Zero is the shortest line the
     * search could find, which is what a guideway wants: a pipe on piers is
     * built by somebody who would rather not build any more of it than they
     * have to, and it goes where the search said.
     *
     * A road is the other case. The search answers "which side of the bay",
     * and between two headlands its answer is a straight line — but nothing
     * anybody has ever driven on runs straight for twenty kilometres. Roads
     * wander: round the shoulder of a rise, along the inside of a valley, out
     * and back for a river crossing. Reproducing every reason for that would
     * be a survey nobody asked for; reproducing the *result* is a smooth,
     * seeded swing about the line the search drew, which is what this is.
     *
     * It is applied before the curvature bound and before the trim, so the
     * wander is arcs rather than corners by construction and the termini still
     * land exactly on their clearance circles with the line's own tangent. */
    float meander;
    /* What is already on the ground here, and how far the line will go to keep
     * off it. Zeroed is an empty country: nothing to keep clear of, which is
     * what the first line laid across a world always has. */
    fly_rail_avoid avoid;
    /* --- which medium the line is laid in ---
     *
     * Zero is a line across the land, which is what a road and a guideway are:
     * dry ground is cheap, water is dear and the shore is stood off.
     *
     * One is the same search read the other way round, and it is the whole of
     * what a sea lane needs from the survey. Water is the cheap ground, land
     * is what the line may not cross, and the standoff that used to keep a
     * road off the beach now keeps a hull off the rocks. Nothing else about
     * the search changes, because nothing else about the problem does: a lane
     * between two harbours is still the shortest way across a medium with an
     * edge to it, and the edge is still the expensive place to be.
     *
     * `draught` is how much water a hull wants under its keel before a cell
     * counts as navigable, in metres. It is asked of the bed rather than of
     * the wet field, because wet is not the question: a river's channel and a
     * lake are wet ground standing above the sea, and no coaster is going up
     * either. Ignored on land, where the wet field is exactly the question. */
    int sea;
    float draught;
} fly_rail_shape;

int fly_rail_lay(const fly_world *world, fly_wpos from, fly_wpos to,
                 const fly_rail_shape *shape, fly_v2 *out, int max);

/* --- the grade line ------------------------------------------------------
 *
 * The other half of a survey, and the half both the rail and the road were
 * doing badly in two different ways. A route's *plan* is where it goes; its
 * *profile* is how it climbs, and a line that takes the ground's own profile
 * is a track across a field rather than a made thing. What a surveyor draws
 * instead is a taut string laid over the ground: straight wherever the ground
 * lets it be, bending only where something pushes it up, held to a gradient a
 * vehicle can hold and to a rate of change of gradient a vehicle can take.
 *
 * That is the obstacle problem, and it solves by relaxation: each station is
 * pulled toward the straight line between its neighbours — the zero-curvature
 * answer — and clamped into the band between the floor it must clear and the
 * most fill it is allowed to stand on. Chainage-weighted, because the stations
 * are not evenly spaced once the trim has taken the stubs off the ends.
 *
 * `floor` is the lowest the line may sit at each station (the shelf for a road,
 * the ground plus a clearance for a line on piers) and `fill` how far above
 * that it may be carried. The end stations are pinned: they are the yard at
 * each settlement and their height is not the surveyor's to choose.
 *
 * `ceiling` is the other bound and is optional (NULL for none): the highest the
 * line may sit at each station, which is what a road passing under something
 * has. Where a ceiling is below the floor it wins — the line has to get under
 * whatever is over it, and the ground in the way is an earthwork's problem
 * rather than a reason to drive through a structure.
 *
 * The relaxation has to be two-sided, and that was the road's bug. Its version
 * only ever raised a station — "if the midpoint of my neighbours is higher than
 * me, come up to it" — which is a ratchet: over rolling country every station
 * climbs until it hits the fill cap and the road crosses the map on a
 * twelve-metre bank. Letting a station come back down as well converges on the
 * taut line instead, which is the one that spends the least earth. */
void fly_rail_grade(float *z, const float *floor, const float *ceiling,
                    const float *chain, int n, float fill, float curve, int passes);

/* --- and the same thing in plan -----------------------------------------
 *
 * `fly_rail_grade` bounds how sharply a line may bend *vertically*; this bounds
 * how sharply it may bend across the ground. Two callers want it and neither is
 * the other's special case — the guideway, because a minimum radius is a fact
 * about a vehicle, and a watercourse, because a river that turns a corner is
 * not a river. It is here rather than in either of them for the reason
 * `fly_rail_grade` is: fly_road.c already reaches for that one.
 *
 * Smoothing passes and a spline get most of the way and neither *bounds*
 * anything: they soften whatever is there, so a line that has to make a real
 * turn keeps a real corner, just a rounder one. Curvature is the property that
 * matters, so it is the property to constrain. Each pass finds the points whose
 * turn implies too tight a radius and pulls them toward the midpoint of their
 * neighbours, which is the move that reduces the turn there and at both
 * neighbours at once; repeated with a small step it converges on the shortest
 * line through the fixed ends that respects the bound. Only the two ends are
 * pinned.
 *
 * `allow` vetoes a move — it is handed the point's two neighbours, where the
 * point is and where the pull would put it, and answers whether that is
 * allowed. The neighbours are there because the guideway's rule is about what
 * the move *is* rather than about where it lands. NULL is no veto, which is
 * what a river passes: the rule the guideway needs it for is about not starting
 * a water crossing, and a river is one. */
typedef int (*fly_bend_allow)(void *ctx, fly_v2 prev, fly_v2 from,
                              fly_v2 to, fly_v2 next);
void fly_rail_bend(fly_v2 *p, int n, float min_radius, int iters,
                   fly_bend_allow allow, void *ctx);

/* What a line on piers will accept under its deck, and the tallest pier worth
 * building. The first is headroom — below it the deck is a pipe lying in the
 * grass; the second is what bounds the taut profile, because past about this
 * the structure stops being a viaduct and starts being a bridge somebody would
 * have routed around instead. */
#define FLY_RAIL_CLEAR_MIN 5.0f
#define FLY_RAIL_PIER_MAX 13.0f
/* The barrel's radius. The survey does not care, and everything that has to
 * pass under the line does: the deck height is the axis of the pipe, so the
 * underside anybody clears is that less this. It is here rather than in the
 * renderer because the drawing and the road that ducks beneath it have to be
 * the same number, and two opinions about how thick the pipe is would be a
 * lorry driving through a guideway in one of them. */
#define FLY_RAIL_PIPE_R 1.15f

int fly_rail_generate(fly_rail_route *route, const fly_world *world,
                      int from_location, int to_location);
int fly_rail_eval(const fly_rail_route *route, float distance,
                  fly_v3 *position, fly_v3 *tangent);
int fly_rail_project(const fly_rail_route *route, fly_v3 point,
                     float *distance, float *separation);
float fly_rail_advance(const fly_rail_route *route, float distance,
                       float speed, float dt);

#endif /* FLY_RAIL_H */
