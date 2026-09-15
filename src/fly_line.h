/* fly_line: a line laid across the ground, and the arithmetic on one.
 *
 * Three things in this world are a chain of stations somebody surveyed and
 * then has to be asked questions about — the guideway, the roads, and the
 * lanes at sea — and the questions are the same three every time: where is the
 * line `d` metres along, which way does it point there, and how far off it is
 * this point. They were written twice before the lanes existed and would have
 * been written a third time; the differences between the callers are what a
 * line is *made of* and what runs on it, and none of them are here.
 *
 * What does differ, and is the one thing the merge has to keep, is the curve
 * between two stations. A road is resampled to an even pitch and takes the
 * uniform form; a survey trimmed to a clearance circle keeps a stub of a few
 * metres against neighbours seventy-five long, and on spacing like that the
 * uniform form overshoots the terminus and doubles back through itself. So the
 * curve is a property of the line and the caller states which it has — see
 * fly_v3spline and fly_v3spline_cr, where the two are argued out.
 *
 * The stations are where the survey settled and the curve passes exactly
 * through every one of them, so nothing here moves anything the survey
 * decided: it only fills in where the line is between what was written down.
 * That matters because several things read the same line and disagreeing about
 * it is visible — a column cutting the corner off its own road, a railcar
 * floating beside its own pipe. */
#ifndef FLY_LINE_H
#define FLY_LINE_H

#include "fly_math.h"

/* A station. `bridge` is the one flag a laid line carries that is not
 * geometry: the deck here is carried over what is underneath rather than lying
 * on it, which is a causeway on a road and a span between piers on a guideway.
 * A line whose medium has no such thing simply leaves it zero. */
typedef struct {
    fly_v3 pos;
    float distance;   /* metres from the first station */
    int bridge;
} fly_line_point;

typedef enum {
    FLY_LINE_EVEN,   /* stations at an even pitch: the uniform curve */
    FLY_LINE_CHORD   /* stubs at the termini: the centripetal one */
} fly_line_curve;

/* Where the line is `t` of the way along span `i`, and which way it points
 * there. Either output may be NULL; returns 0 when there is no such span. */
int fly_line_span(const fly_line_point *p, int n, fly_line_curve curve,
                  int i, float t, fly_v3 *pos, fly_v3 *tangent);

/* The same question by chainage rather than by span. `length` is the line's
 * own, which is `p[n-1].distance` for every caller here and is passed in
 * because the containers all keep it. Returns 0 when there is no line. */
int fly_line_eval(const fly_line_point *p, int n, float length,
                  fly_line_curve curve, float d, fly_v3 *pos, fly_v3 *tangent);

/* The nearest point of the line to `q`: how far along it that is and how far
 * off it the query was. Either may be NULL; returns 0 when there is no line.
 *
 * Against the chords rather than against the curve, which is the same
 * distinction the note above draws and the opposite decision: a projection is
 * a search over every span and the curve would cost a spline evaluation per
 * subdivision per span, for an answer that differs by the sagitta — a couple
 * of centimetres at these radii, against separations nobody asks about below
 * the metre. */
int fly_line_project(const fly_line_point *p, int n, fly_v3 q,
                     float *distance, float *separation);

#endif /* FLY_LINE_H */
