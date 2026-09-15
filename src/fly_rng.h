/* fly_rng: deterministic PRNG + value/fBm noise for procedural content */
#ifndef FLY_RNG_H
#define FLY_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t s[2]; /* xoroshiro128+ state */
} fly_rng;

void fly_rng_seed(fly_rng *r, uint64_t seed);
uint64_t fly_rng_u64(fly_rng *r);
uint32_t fly_rng_range(fly_rng *r, uint32_t n);       /* [0, n) */
float fly_rng_f01(fly_rng *r);                        /* [0, 1) */
float fly_rng_span(fly_rng *r, float lo, float hi);   /* [lo, hi) */
float fly_rng_gauss(fly_rng *r);                      /* mean 0, sigma 1 */

/* stateless hash-based noise, stable across platforms for a given seed */
uint32_t fly_hash2(uint32_t seed, int32_t x, int32_t y);
float fly_noise2(uint32_t seed, float x, float y);            /* value noise, [-1, 1] */
float fly_fbm2(uint32_t seed, float x, float y, int octaves);
/* The same value noise in three dimensions, for sampling a field on a sphere:
 * a wrapping chart cannot use the 2D pair, because 2D noise is not periodic
 * and the seam shows. There is an exact GLSL twin of each, gated by
 * gpu.parity3.
 *
 * Normalized so the 3D field can stand in for the 2D one. Trilinear
 * interpolation blends eight independent corners where bilinear blends four,
 * which makes the raw 3D field measurably quieter: the two have the same
 * [-1, 1] range but not the same distribution, and everything tuned against
 * the 2D field — an amplitude in metres, a smoothstep's edges — silently
 * means something different when handed the 3D one. Sampled over 600k points
 * the standard deviations are 0.4295 and 0.3714, so the gain is that ratio
 * and `rng` holds the two within 1% of each other.
 *
 * The cost of the gain is that the nominal range widens by the same factor.
 * It is nominal: a value-noise extreme needs all eight corners of a cell to
 * hash to an extreme, which does not happen in a world's worth of samples. */
#define FLY_NOISE3_GAIN 1.1567f
uint32_t fly_hash3(uint32_t seed, int32_t x, int32_t y, int32_t z);
float fly_noise3(uint32_t seed, float x, float y, float z);
float fly_fbm3(uint32_t seed, float x, float y, float z, int octaves); /* fractal sum, ~[-1, 1] */

#endif /* FLY_RNG_H */
