#include "fly_rng.h"

#include <math.h>

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void fly_rng_seed(fly_rng *r, uint64_t seed) {
    r->s[0] = splitmix64(&seed);
    r->s[1] = splitmix64(&seed);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t fly_rng_u64(fly_rng *r) {
    uint64_t s0 = r->s[0], s1 = r->s[1];
    uint64_t result = s0 + s1;
    s1 ^= s0;
    r->s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16);
    r->s[1] = rotl(s1, 37);
    return result;
}

uint32_t fly_rng_range(fly_rng *r, uint32_t n) {
    return n ? (uint32_t)(fly_rng_u64(r) % n) : 0;
}

float fly_rng_f01(fly_rng *r) {
    return (float)((fly_rng_u64(r) >> 40) * (1.0 / 16777216.0));
}

float fly_rng_span(fly_rng *r, float lo, float hi) {
    return lo + (hi - lo) * fly_rng_f01(r);
}

float fly_rng_gauss(fly_rng *r) {
    /* Box-Muller */
    float u1 = fly_rng_f01(r), u2 = fly_rng_f01(r);
    if (u1 < 1e-7f) u1 = 1e-7f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

uint32_t fly_hash2(uint32_t seed, int32_t x, int32_t y) {
    uint32_t h = seed;
    h ^= (uint32_t)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h *= 0xC2B2AE35u;
    h ^= (uint32_t)y * 0x27D4EB2Fu;
    h = (h << 15) | (h >> 17);
    h *= 0x165667B1u;
    h ^= h >> 16;
    return h;
}

static float grid01(uint32_t seed, int32_t x, int32_t y) {
    return (float)(fly_hash2(seed, x, y) & 0xFFFFFF) * (1.0f / 16777215.0f);
}

float fly_noise2(uint32_t seed, float x, float y) {
    int32_t xi = (int32_t)floorf(x), yi = (int32_t)floorf(y);
    float xf = x - (float)xi, yf = y - (float)yi;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float a = grid01(seed, xi, yi), b = grid01(seed, xi + 1, yi);
    float c = grid01(seed, xi, yi + 1), d = grid01(seed, xi + 1, yi + 1);
    float ab = a + (b - a) * u, cd = c + (d - c) * u;
    return (ab + (cd - ab) * v) * 2.0f - 1.0f;
}

float fly_fbm2(uint32_t seed, float x, float y, int octaves) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    int i;
    for (i = 0; i < octaves; ++i) {
        sum += amp * fly_noise2(seed + (uint32_t)i * 0x9E37u, x * freq, y * freq);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* --- three dimensions of the same value noise -----------------------------
 *
 * Written to mirror fly_hash2/fly_noise2/fly_fbm2 term for term, because the
 * pair have to be readable against each other and because there is a GLSL twin
 * of each that has to agree to float rounding.
 *
 * What this is for: a chart that wraps cannot be sampled with 2D noise, because
 * 2D noise is not periodic and the seam would show as a wall of discontinuous
 * terrain. Sampling a *sphere* embedded in 3D has no seam anywhere by
 * construction — go all the way round and you arrive at the same point in the
 * noise domain because you have arrived at the same point, full stop. The
 * mapping from the flat chart onto that sphere is the caller's business; this
 * only has to be a well-behaved field in three dimensions. */
uint32_t fly_hash3(uint32_t seed, int32_t x, int32_t y, int32_t z) {
    uint32_t h = seed;
    h ^= (uint32_t)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h *= 0xC2B2AE35u;
    h ^= (uint32_t)y * 0x27D4EB2Fu;
    h = (h << 15) | (h >> 17);
    h *= 0x165667B1u;
    h ^= (uint32_t)z * 0x9E3779B1u;
    h = (h << 11) | (h >> 21);
    h *= 0x7FEB352Du;
    h ^= h >> 16;
    return h;
}

static float grid01_3(uint32_t seed, int32_t x, int32_t y, int32_t z) {
    return (float)(fly_hash3(seed, x, y, z) & 0xFFFFFF) * (1.0f / 16777215.0f);
}

float fly_noise3(uint32_t seed, float x, float y, float z) {
    int32_t xi = (int32_t)floorf(x), yi = (int32_t)floorf(y), zi = (int32_t)floorf(z);
    float xf = x - (float)xi, yf = y - (float)yi, zf = z - (float)zi;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float w = zf * zf * (3.0f - 2.0f * zf);
    float a = grid01_3(seed, xi, yi, zi),         b = grid01_3(seed, xi + 1, yi, zi);
    float c = grid01_3(seed, xi, yi + 1, zi),     d = grid01_3(seed, xi + 1, yi + 1, zi);
    float e = grid01_3(seed, xi, yi, zi + 1),     f = grid01_3(seed, xi + 1, yi, zi + 1);
    float g = grid01_3(seed, xi, yi + 1, zi + 1), h = grid01_3(seed, xi + 1, yi + 1, zi + 1);
    float ab = a + (b - a) * u, cd = c + (d - c) * u;
    float ef = e + (f - e) * u, gh = g + (h - g) * u;
    float lo = ab + (cd - ab) * v, hi = ef + (gh - ef) * v;
    return ((lo + (hi - lo) * w) * 2.0f - 1.0f) * FLY_NOISE3_GAIN;
}

float fly_fbm3(uint32_t seed, float x, float y, float z, int octaves) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    int i;
    for (i = 0; i < octaves; ++i) {
        sum += amp * fly_noise3(seed + (uint32_t)i * 0x9E37u, x * freq, y * freq, z * freq);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

