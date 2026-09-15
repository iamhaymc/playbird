/* fly_river: the survey that puts water on the land.
 *
 * One entry point, called once from fly_world_gen, before anything else has
 * been laid across the country. It fills `world.river`, `world.river_pt` and
 * `world.lake` — the shapes are fly_world.h's, because fly_world_ground is
 * what has to read them per sample and a heightfield cannot include the module
 * that surveyed it.
 *
 * Deterministic in the seed and in nothing else: the same world generates the
 * same watercourses station for station, and no draw is made from any stream
 * the rest of world-gen uses, so a world generated before there were rivers in
 * it generates the same terrain, the same sites and the same names as one
 * generated after. */
#ifndef FLY_RIVER_H
#define FLY_RIVER_H

#include "fly_world.h"

void fly_river_lay(fly_world *w);

#endif /* FLY_RIVER_H */
