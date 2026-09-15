#include "fly_line.h"

#include <float.h>

/* The curve across span `i`, in whichever form the line has. Both are handed
 * the terminus duplicated as its own neighbour at either end, which is what
 * makes a line that stops leave along its own chord instead of inventing a
 * tangent out of a station that is not there. */
static fly_v3 line_arc(const fly_line_point *p, int n, fly_line_curve curve,
                       int i, float t) {
    fly_v3 a = p[i > 0 ? i - 1 : 0].pos, b = p[i].pos;
    fly_v3 c = p[i + 1].pos, d = p[i + 2 < n ? i + 2 : n - 1].pos;
    return curve == FLY_LINE_CHORD ? fly_v3spline_cr(a, b, c, d, t)
                                   : fly_v3spline(a, b, c, d, t);
}

int fly_line_span(const fly_line_point *p, int n, fly_line_curve curve,
                  int i, float t, fly_v3 *pos, fly_v3 *tangent) {
    if (!p || i < 0 || i + 1 >= n) return 0;
    t = fly_clampf(t, 0.0f, 1.0f);
    if (pos) *pos = line_arc(p, n, curve, i, t);
    if (tangent) {
        /* The derivative by difference rather than in closed form: the two
           agree to the width of a lane and this one cannot get out of step
           with the curve above when either is edited. */
        const float h = 0.02f;
        float t0 = t - h < 0.0f ? 0.0f : t - h;
        float t1 = t + h > 1.0f ? 1.0f : t + h;
        fly_v3 e = fly_v3sub(line_arc(p, n, curve, i, t1),
                             line_arc(p, n, curve, i, t0));
        *tangent = fly_v3len(e) > 1e-6f
                 ? fly_v3norm(e)
                 : fly_v3norm(fly_v3sub(p[i + 1].pos, p[i].pos));
    }
    return 1;
}

int fly_line_eval(const fly_line_point *p, int n, float length,
                  fly_line_curve curve, float d, fly_v3 *pos, fly_v3 *tangent) {
    int i;
    float span, t;
    if (!p || n < 2) return 0;
    d = fly_clampf(d, 0.0f, length);
    for (i = 0; i < n - 2 && p[i + 1].distance < d; ++i) {}
    span = p[i + 1].distance - p[i].distance;
    t = span > 0.0f ? (d - p[i].distance) / span : 0.0f;
    /* `t` is chord fraction read off the arc's parameter, which is not exactly
       arc fraction on a curve — the error is the difference between a chord and
       its arc, under a centimetre at these radii and spacings, and it costs
       nothing to be that far along a line you are centred on anyway. */
    return fly_line_span(p, n, curve, i, t, pos, tangent);
}

int fly_line_project(const fly_line_point *p, int n, fly_v3 q,
                     float *distance, float *separation) {
    float best = FLT_MAX, best_d = 0.0f;
    int i;
    if (!p || n < 2) return 0;
    for (i = 0; i + 1 < n; ++i) {
        fly_v3 a = p[i].pos, ab = fly_v3sub(p[i + 1].pos, a);
        float den = fly_v3dot(ab, ab);
        float u = den > 0.0f ? fly_clampf(fly_v3dot(fly_v3sub(q, a), ab) / den, 0, 1) : 0;
        float sep = fly_v3dist(q, fly_v3add(a, fly_v3scale(ab, u)));
        if (sep < best) {
            best = sep;
            best_d = fly_lerpf(p[i].distance, p[i + 1].distance, u);
        }
    }
    if (distance) *distance = best_d;
    if (separation) *separation = best;
    return 1;
}
