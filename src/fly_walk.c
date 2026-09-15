#include "fly_walk.h"

#include <string.h>

#define FLY_WALK_SPEED 7.0f
#define FLY_WALK_ACCEL 34.0f
#define FLY_WALK_AIR_ACCEL 8.0f
#define FLY_WALK_FRICTION 8.0f
#define FLY_WALK_STEP 0.55f
/* --- one planet, one gravity ---------------------------------------------
 *
 * The operator on foot used to fall at a flat 18 m/s^2 while the aeroplane
 * they had just climbed out of fell at 9.81 — two different fields, a metre
 * apart, on a world whose whole point is that the ground is a ball and the
 * field is a consequence of it. Everything else in the game already agrees
 * about that field: the flight model asks `fly_gravity` for the local value,
 * and the short-lived things — an ordnance arc, a dropped module falling to
 * the ground — use the surface constant, which is that same field where they
 * happen. This was the one number with a planet of its own. Asking for the
 * local value also means the walk on a 3 km ridge is the walk that ridge
 * implies.
 *
 * The jump is stated as a height rather than as an impulse for exactly that
 * reason: an impulse means a different hop at every altitude and a different
 * one again if the planet is ever retuned, whereas the height is the thing the
 * hop is *for*. It is deliberately not a human standing jump — a bunny-hop the
 * player can chain is a movement idiom this mode is built around and the docs
 * describe — and it is unchanged from what the old constants produced
 * (7.2 m/s under 18 m/s^2 tops out at 1.44 m), so the mode plays as it did,
 * with the hang time the real field gives it. */
#define FLY_WALK_JUMP_H 1.44f

void fly_walk_init(fly_walker *w, fly_v3 pos, float yaw) {
    memset(w, 0, sizeof *w);
    w->pos = pos;
    w->yaw = yaw;
    w->eye_height = 1.72f;
    w->on_ground = 1;
}

static fly_v3 fly__walk_wish(const fly_walker *w, const fly_walk_input *in) {
    fly_v3 f = fly_v3mk(cosf(w->yaw), sinf(w->yaw), 0.0f);
    fly_v3 r = fly_v3mk(-sinf(w->yaw), cosf(w->yaw), 0.0f);
    fly_v3 wish = fly_v3add(fly_v3scale(f, fly_clampf(in->forward, -1, 1)),
                            fly_v3scale(r, fly_clampf(in->right, -1, 1)));
    float len = fly_v3len(wish);
    return len > 1.0f ? fly_v3scale(wish, 1.0f / len) : wish;
}

void fly_walk_step(fly_walker *w, const fly_walk_input *in,
                   fly_ground_fn ground, void *ground_user, float dt) {
    if (!w || !in || !ground || dt <= 0.0f) return;
    fly_v3 wish = fly__walk_wish(w, in);
    float accel = w->on_ground ? FLY_WALK_ACCEL : FLY_WALK_AIR_ACCEL;
    float target_x = wish.x * FLY_WALK_SPEED, target_y = wish.y * FLY_WALK_SPEED;

    if (w->on_ground && fly_v3len(wish) < 0.01f) {
        float k = fly_clampf(1.0f - FLY_WALK_FRICTION * dt, 0.0f, 1.0f);
        w->vel.x *= k;
        w->vel.y *= k;
    } else {
        float max_change = accel * dt;
        float dx = fly_clampf(target_x - w->vel.x, -max_change, max_change);
        float dy = fly_clampf(target_y - w->vel.y, -max_change, max_change);
        w->vel.x += dx;
        w->vel.y += dy;
    }

    float g = fly_gravity(w->pos.z);
    w->jump_held = in->jump != 0;
    if (w->on_ground && w->jump_held && !w->jump_latch) {
        w->vel.z = sqrtf(2.0f * g * FLY_WALK_JUMP_H);
        w->on_ground = 0;
        w->jump_latch = 1;
    }
    if (!w->jump_held) w->jump_latch = 0;
    if (!w->on_ground) w->vel.z -= g * dt;

    fly_v3 next = fly_v3add(w->pos, fly_v3scale(w->vel, dt));
    float old_ground = ground(ground_user, w->pos.x, w->pos.y);
    float next_ground = ground(ground_user, next.x, next.y);
    if (w->on_ground && next_ground - old_ground > FLY_WALK_STEP) {
        next.x = w->pos.x;
        next.y = w->pos.y;
        w->vel.x = w->vel.y = 0.0f;
        next_ground = old_ground;
    }

    if (next.z <= next_ground || (w->on_ground && next.z - next_ground <= FLY_WALK_STEP)) {
        next.z = next_ground;
        if (w->vel.z < 0.0f) w->vel.z = 0.0f;
        w->on_ground = 1;
    } else {
        w->on_ground = 0;
    }
    w->pos = next;
    w->pitch = fly_clampf(w->pitch, -1.45f, 1.45f);
}
