/* fly_math: vectors, quaternions, matrices, scalar helpers (header-only) */
#ifndef FLY_MATH_H
#define FLY_MATH_H

#include <math.h>

#define FLY_PI 3.14159265358979323846f
#define FLY_DEG2RAD (FLY_PI / 180.0f)
#define FLY_RAD2DEG (180.0f / FLY_PI)

typedef struct { float x, y; } fly_v2;
typedef struct { float x, y, z; } fly_v3;
typedef struct { float x, y, z, w; } fly_v4;
typedef struct { float w, x, y, z; } fly_quat;
typedef struct { float m[16]; } fly_m4; /* column-major */

static inline float fly_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fly_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float fly_signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }
static inline float fly_smoothstepf(float e0, float e1, float x) {
    float t = fly_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
/* wrap angle to [-pi, pi] */
static inline float fly_wrap_pi(float a) {
    while (a > FLY_PI) a -= 2.0f * FLY_PI;
    while (a < -FLY_PI) a += 2.0f * FLY_PI;
    return a;
}

static inline fly_v2 fly_v2mk(float x, float y) { fly_v2 v = { x, y }; return v; }
static inline fly_v3 fly_v3mk(float x, float y, float z) { fly_v3 v = { x, y, z }; return v; }
static inline fly_v3 fly_v3zero(void) { return fly_v3mk(0, 0, 0); }
static inline fly_v3 fly_v3add(fly_v3 a, fly_v3 b) { return fly_v3mk(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline fly_v3 fly_v3sub(fly_v3 a, fly_v3 b) { return fly_v3mk(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline fly_v3 fly_v3scale(fly_v3 a, float s) { return fly_v3mk(a.x * s, a.y * s, a.z * s); }
static inline fly_v3 fly_v3mul(fly_v3 a, fly_v3 b) { return fly_v3mk(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float fly_v3dot(fly_v3 a, fly_v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline fly_v3 fly_v3cross(fly_v3 a, fly_v3 b) {
    return fly_v3mk(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float fly_v3len(fly_v3 a) { return sqrtf(fly_v3dot(a, a)); }
static inline fly_v3 fly_v3norm(fly_v3 a) {
    float l = fly_v3len(a);
    return l > 1e-8f ? fly_v3scale(a, 1.0f / l) : fly_v3zero();
}
static inline fly_v3 fly_v3lerp(fly_v3 a, fly_v3 b, float t) {
    return fly_v3mk(fly_lerpf(a.x, b.x, t), fly_lerpf(a.y, b.y, t), fly_lerpf(a.z, b.z, t));
}
static inline float fly_v3dist(fly_v3 a, fly_v3 b) { return fly_v3len(fly_v3sub(a, b)); }
/* Uniform Catmull-Rom: the curve from p1 to p2 that leaves each of them
 * parallel to the chord across its own neighbours, so a chain of these is
 * smooth across the joins and passes exactly through every control point.
 *
 * Here because two things want it and they are not in the same module: a road
 * is a curve through the stations its survey settled on, and so is the rail's
 * deck. Both are polylines somebody sampled at an even pitch and then had to
 * draw as the arc it was a sample of.
 *
 * Uniform rather than centripetal, which matters and is a decision rather than
 * a default: the centripetal form exists because uniform Catmull-Rom overshoots
 * when the control points are unevenly spaced, and it costs a square root per
 * knot to avoid it. Even spacing is exactly what a resampled survey has, and on
 * it the two agree. Anything with wildly uneven controls — a survey trimmed to
 * a clearance circle, a grid search's raw output — wants `fly_v3spline_cr`
 * below and must not use this. */
static inline fly_v3 fly_v3spline(fly_v3 p0, fly_v3 p1, fly_v3 p2, fly_v3 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    float w0 = -0.5f * t3 + t2 - 0.5f * t;
    float w1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
    float w2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    float w3 = 0.5f * t3 - 0.5f * t2;
    return fly_v3mk(p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                    p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3,
                    p0.z * w0 + p1.z * w1 + p2.z * w2 + p3.z * w3);
}

/* Centripetal Catmull-Rom: the same curve, with the knots spaced by the square
 * root of the chord rather than evenly.
 *
 * The uniform form above is right on an evenly sampled survey and wrong the
 * moment the spacing is not even, which is exactly what both surveys hand their
 * termini: the trim that stops a line short of a settlement leaves a stub of a
 * few metres against neighbours seventy-five metres long. The tangent the
 * uniform form takes there is half the chord across those neighbours — forty
 * metres of tangent on a five-metre span — so the curve shoots past the
 * terminus and doubles back on itself. Drawn as a tube that is a fan of splayed
 * rings at the end of the line, because a mitre through a reversal opens to
 * whatever the widening is allowed to reach.
 *
 * Spacing the knots by sqrt(chord) is the standard fix and it is a bound, not a
 * damping: a centripetal curve provably never forms a cusp or a loop, whatever
 * the controls do. It costs the two square roots per knot the note above
 * priced, which is why the uniform form stays for the callers that are entitled
 * to it.
 *
 * Both ends are handled the way every caller here calls it — with the terminus
 * duplicated as its own neighbour. A zero-length knot span has no tangent to
 * take, so the end leaves along its own chord, which is what a line that stops
 * does. */
static inline fly_v3 fly_v3spline_cr(fly_v3 p0, fly_v3 p1, fly_v3 p2, fly_v3 p3, float t) {
    float d0 = sqrtf(fly_v3dist(p0, p1));
    float d1 = sqrtf(fly_v3dist(p1, p2));
    float d2 = sqrtf(fly_v3dist(p2, p3));
    fly_v3 chord = fly_v3sub(p2, p1);
    fly_v3 m1, m2;
    float t2, t3;
    float h00, h10, h01, h11;
    if (d1 < 1e-4f) return p1;   /* the span is a point; there is no curve */
    /* the Barry-Goldman tangents, in the knot spacing above */
    m1 = d0 > 1e-4f
       ? fly_v3scale(fly_v3add(fly_v3sub(fly_v3scale(fly_v3sub(p1, p0), 1.0f / d0),
                                         fly_v3scale(fly_v3sub(p2, p0), 1.0f / (d0 + d1))),
                               fly_v3scale(chord, 1.0f / d1)), d1)
       : chord;
    m2 = d2 > 1e-4f
       ? fly_v3scale(fly_v3add(fly_v3sub(fly_v3scale(chord, 1.0f / d1),
                                         fly_v3scale(fly_v3sub(p3, p1), 1.0f / (d1 + d2))),
                               fly_v3scale(fly_v3sub(p3, p2), 1.0f / d2)), d1)
       : chord;
    /* ...through the Hermite basis on [0,1], so the span still runs p1 to p2 */
    t2 = t * t;
    t3 = t2 * t;
    h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    h10 = t3 - 2.0f * t2 + t;
    h01 = -2.0f * t3 + 3.0f * t2;
    h11 = t3 - t2;
    return fly_v3add(fly_v3add(fly_v3scale(p1, h00), fly_v3scale(m1, h10)),
                     fly_v3add(fly_v3scale(p2, h01), fly_v3scale(m2, h11)));
}

static inline fly_quat fly_qident(void) { fly_quat q = { 1, 0, 0, 0 }; return q; }
static inline fly_quat fly_qmk(float w, float x, float y, float z) { fly_quat q = { w, x, y, z }; return q; }
static inline fly_quat fly_qmul(fly_quat a, fly_quat b) {
    return fly_qmk(
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
}
static inline fly_quat fly_qconj(fly_quat q) { return fly_qmk(q.w, -q.x, -q.y, -q.z); }
static inline fly_quat fly_qnorm(fly_quat q) {
    float l = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-8f) return fly_qident();
    float s = 1.0f / l;
    return fly_qmk(q.w * s, q.x * s, q.y * s, q.z * s);
}
static inline fly_quat fly_qaxis(fly_v3 axis, float angle) {
    fly_v3 a = fly_v3norm(axis);
    float h = angle * 0.5f, s = sinf(h);
    return fly_qmk(cosf(h), a.x * s, a.y * s, a.z * s);
}
/* rotate vector by quaternion */
static inline fly_v3 fly_qrot(fly_quat q, fly_v3 v) {
    fly_v3 u = fly_v3mk(q.x, q.y, q.z);
    fly_v3 t = fly_v3scale(fly_v3cross(u, v), 2.0f);
    return fly_v3add(fly_v3add(v, fly_v3scale(t, q.w)), fly_v3cross(u, t));
}
/* integrate quaternion by body angular velocity omega over dt */
static inline fly_quat fly_qintegrate(fly_quat q, fly_v3 omega, float dt) {
    fly_quat w = fly_qmk(0, omega.x, omega.y, omega.z);
    fly_quat dq = fly_qmul(q, w);
    q.w += 0.5f * dq.w * dt; q.x += 0.5f * dq.x * dt;
    q.y += 0.5f * dq.y * dt; q.z += 0.5f * dq.z * dt;
    return fly_qnorm(q);
}
/* yaw(z) -> pitch(y) -> roll(x) intrinsic, aviation-style; world z-up */
static inline fly_quat fly_qeuler(float roll, float pitch, float yaw) {
    fly_quat qz = fly_qaxis(fly_v3mk(0, 0, 1), yaw);
    fly_quat qy = fly_qaxis(fly_v3mk(0, 1, 0), pitch);
    fly_quat qx = fly_qaxis(fly_v3mk(1, 0, 0), roll);
    return fly_qnorm(fly_qmul(fly_qmul(qz, qy), qx));
}
static inline void fly_qto_euler(fly_quat q, float *roll, float *pitch, float *yaw) {
    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    *roll = atan2f(sinr, cosr);
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    *pitch = fabsf(sinp) >= 1.0f ? copysignf(FLY_PI / 2.0f, sinp) : asinf(sinp);
    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    *yaw = atan2f(siny, cosy);
}

static inline fly_m4 fly_m4ident(void) {
    fly_m4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}
static inline fly_m4 fly_m4mul(fly_m4 a, fly_m4 b) {
    fly_m4 r;
    int c, i;
    for (c = 0; c < 4; ++c)
        for (i = 0; i < 4; ++i)
            r.m[c * 4 + i] = a.m[0 * 4 + i] * b.m[c * 4 + 0] + a.m[1 * 4 + i] * b.m[c * 4 + 1] +
                             a.m[2 * 4 + i] * b.m[c * 4 + 2] + a.m[3 * 4 + i] * b.m[c * 4 + 3];
    return r;
}
static inline fly_v4 fly_m4mulv(fly_m4 m, fly_v4 v) {
    fly_v4 r;
    r.x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w;
    r.y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w;
    r.z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w;
    r.w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w;
    return r;
}
static inline fly_m4 fly_m4perspective(float fovy, float aspect, float znear, float zfar) {
    fly_m4 r = { { 0 } };
    float f = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}
static inline fly_m4 fly_m4lookat(fly_v3 eye, fly_v3 at, fly_v3 up) {
    fly_v3 f = fly_v3norm(fly_v3sub(at, eye));
    fly_v3 s = fly_v3norm(fly_v3cross(f, up));
    fly_v3 u = fly_v3cross(s, f);
    fly_m4 r = fly_m4ident();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -fly_v3dot(s, eye);
    r.m[13] = -fly_v3dot(u, eye);
    r.m[14] = fly_v3dot(f, eye);
    return r;
}

#endif /* FLY_MATH_H */
