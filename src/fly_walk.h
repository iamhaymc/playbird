/* fly_walk: deterministic first-person ground locomotion. */
#ifndef FLY_WALK_H
#define FLY_WALK_H

#include "fly_sim.h"

typedef struct {
    fly_v3 pos;
    fly_v3 vel;
    float yaw;
    float pitch;
    float eye_height;
    int on_ground;
    int jump_held;
    int jump_latch;
} fly_walker;

typedef struct {
    float forward;
    float right;
    int jump;
} fly_walk_input;

void fly_walk_init(fly_walker *w, fly_v3 pos, float yaw);
void fly_walk_step(fly_walker *w, const fly_walk_input *input,
                   fly_ground_fn ground, void *ground_user, float dt);

#endif /* FLY_WALK_H */
