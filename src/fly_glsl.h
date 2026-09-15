/* fly_glsl: fly99's shared GLSL source fragments.
 *
 * These strings are the GPU half of functions that also exist in C, and they
 * are deliberately line-for-line ports so both renderers agree. The hashing
 * relies on GLSL's uint having the same wraparound semantics as uint32_t, so
 * the value noise reproduces fly_noise2 to float rounding (~1e-7) — which is
 * what lets the GPU and CPU renderers describe the same world.
 *
 * Anything added here must keep that property; the `gpu.parity` test aspect
 * compares the two implementations directly and fails if they drift. */
#ifndef FLY_GLSL_H
#define FLY_GLSL_H

/* FLY_PLANET_R, as a shader literal. One place, so the ball the terrain is
 * sampled on, the ball the sky draws and the ball the flight model falls
 * toward cannot drift apart. */
#define FLY_GLSL_PLANET_R "600000.0"

/* FLY_MOON, as a shader literal, and here for the same reason: one moon, and
 * the ground the GPU shades under it has to be the ground the CPU shades. */
#define FLY_GLSL_MOON "vec3(0.0100, 0.0110, 0.0170)"

/* Value noise: exact port of fly_hash2 / grid01 / fly_noise2 (fly_rng.c). */
#define FLY_GLSL_NOISE \
    "uint fly_hash2(uint s, int x, int y){\n"                                  \
    "  uint h = s;\n"                                                          \
    "  h ^= uint(x) * 0x85EBCA6Bu; h = (h << 13) | (h >> 19); h *= 0xC2B2AE35u;\n" \
    "  h ^= uint(y) * 0x27D4EB2Fu; h = (h << 15) | (h >> 17); h *= 0x165667B1u;\n" \
    "  h ^= h >> 16; return h;\n"                                              \
    "}\n"                                                                      \
    "float fly_grid01(uint s, int x, int y){\n"                                \
    "  return float(fly_hash2(s, x, y) & 0xFFFFFFu) * (1.0 / 16777215.0);\n"   \
    "}\n"                                                                      \
    "float fly_noise2(uint s, float x, float y){\n"                            \
    "  int xi = int(floor(x)), yi = int(floor(y));\n"                          \
    "  float xf = x - float(xi), yf = y - float(yi);\n"                        \
    "  float u = xf * xf * (3.0 - 2.0 * xf);\n"                                \
    "  float v = yf * yf * (3.0 - 2.0 * yf);\n"                                \
    "  float a = fly_grid01(s, xi, yi),     b = fly_grid01(s, xi + 1, yi);\n"  \
    "  float c = fly_grid01(s, xi, yi + 1), d = fly_grid01(s, xi + 1, yi + 1);\n" \
    "  float ab = a + (b - a) * u, cd = c + (d - c) * u;\n"                    \
    "  return (ab + (cd - ab) * v) * 2.0 - 1.0;\n"                             \
    "}\n"                                                                      \
    "float fly_fbm2(uint s, float x, float y, int octaves){\n"                 \
    "  float sum = 0.0, amp = 0.5, freq = 1.0, norm = 0.0;\n"                  \
    "  for (int i = 0; i < octaves; ++i) {\n"                                  \
    "    sum += amp * fly_noise2(s + uint(i) * 0x9E37u, x * freq, y * freq);\n" \
    "    norm += amp; amp *= 0.5; freq *= 2.0;\n"                              \
    "  }\n"                                                                    \
    "  return norm > 0.0 ? sum / norm : 0.0;\n"                                \
    "}\n"                                                                      \
    /* and the same in three dimensions: exact port of fly_hash3 / grid01_3 /
       fly_noise3 / fly_fbm3. Sampling a sphere embedded in 3D is how a wrapping
       chart gets terrain with no seam in it — 2D noise is not periodic, so any
       chart that comes back on itself shows the join. */ \
    "uint fly_hash3(uint s, int x, int y, int z){\n"                           \
    "  uint h = s;\n"                                                          \
    "  h ^= uint(x) * 0x85EBCA6Bu; h = (h << 13) | (h >> 19); h *= 0xC2B2AE35u;\n" \
    "  h ^= uint(y) * 0x27D4EB2Fu; h = (h << 15) | (h >> 17); h *= 0x165667B1u;\n" \
    "  h ^= uint(z) * 0x9E3779B1u; h = (h << 11) | (h >> 21); h *= 0x7FEB352Du;\n" \
    "  h ^= h >> 16; return h;\n"                                              \
    "}\n"                                                                      \
    "float fly_grid01_3(uint s, int x, int y, int z){\n"                       \
    "  return float(fly_hash3(s, x, y, z) & 0xFFFFFFu) * (1.0 / 16777215.0);\n" \
    "}\n"                                                                      \
    "float fly_noise3(uint s, float x, float y, float z){\n"                   \
    "  int xi = int(floor(x)), yi = int(floor(y)), zi = int(floor(z));\n"      \
    "  float xf = x - float(xi), yf = y - float(yi), zf = z - float(zi);\n"    \
    "  float u = xf * xf * (3.0 - 2.0 * xf);\n"                                \
    "  float v = yf * yf * (3.0 - 2.0 * yf);\n"                                \
    "  float w = zf * zf * (3.0 - 2.0 * zf);\n"                                \
    "  float a = fly_grid01_3(s, xi, yi, zi),     b = fly_grid01_3(s, xi+1, yi, zi);\n" \
    "  float c = fly_grid01_3(s, xi, yi+1, zi),   d = fly_grid01_3(s, xi+1, yi+1, zi);\n" \
    "  float e = fly_grid01_3(s, xi, yi, zi+1),   f = fly_grid01_3(s, xi+1, yi, zi+1);\n" \
    "  float g = fly_grid01_3(s, xi, yi+1, zi+1), h = fly_grid01_3(s, xi+1, yi+1, zi+1);\n" \
    "  float ab = a + (b - a) * u, cd = c + (d - c) * u;\n"                    \
    "  float ef = e + (f - e) * u, gh = g + (h - g) * u;\n"                    \
    "  float lo = ab + (cd - ab) * v, hi = ef + (gh - ef) * v;\n"              \
    /* FLY_NOISE3_GAIN: normalized to fly_noise2's spread — see fly_rng.h */   \
    "  return ((lo + (hi - lo) * w) * 2.0 - 1.0) * 1.1567;\n"                  \
    "}\n"                                                                      \
    "float fly_fbm3(uint s, float x, float y, float z, int octaves){\n"        \
    "  float sum = 0.0, amp = 0.5, freq = 1.0, norm = 0.0;\n"                  \
    "  for (int i = 0; i < octaves; ++i) {\n"                                  \
    "    sum += amp * fly_noise3(s + uint(i) * 0x9E37u,\n"                      \
    "                            x * freq, y * freq, z * freq);\n"             \
    "    norm += amp; amp *= 0.5; freq *= 2.0;\n"                              \
    "  }\n"                                                                    \
    "  return norm > 0.0 ? sum / norm : 0.0;\n"                                \
    "}\n"

/* World sampling: exact ports of fly_smoothstepf (fly_math.h),
 * fly_world_sphere_dir, ground_raw and fly_world_ground (fly_world.c), and
 * trace_terrain (fly_render.c).
 *
 * GLSL's built-in smoothstep is undefined when edge0 > edge1, which the pad
 * blend relies on, so the C version is ported explicitly rather than reused.
 *
 * Expects these uniforms to be declared by the including shader:
 *   uniform int  uSeed;        world seed
 *   uniform int  uLocCount;    discovered+undiscovered location count
 *   uniform vec4 uLoc[FLY_GLSL_MAX_LOC];   xy = pad centre, z = pad_z
 *
 * and declares two of its own, because they belong to the ground function
 * rather than to any one scene and there are eight shaders that sample
 * terrain, so they are declared here once instead of eight times:
 *
 *   uniform vec4 uChartFrame;  fly_world's `frame`, where the chart's centre
 *                              sits on the ball — part of the mapping
 *   uniform int  uCutCount;    earthworks in uCut, already radius-culled
 *   uniform vec4 uCut[FLY_GLSL_MAX_CUT * 3];   see below
 *
 * Every one of them has to set all of these: left at their defaults the GPU
 * draws a different planet's terrain than the CPU does, which is what
 * `gpu.ground` is there to catch.
 *
 * A cutting is nine floats (fly_cut in fly_world.h) and a uniform row is four,
 * so each takes three rows and the last one carries a single value:
 *
 *   uCut[i*3 + 0] = vec4(x, y, dx, dy)
 *   uCut[i*3 + 1] = vec4(half, wide, feather, z0)
 *   uCut[i*3 + 2] = vec4(z1, 0, 0, 0)
 *
 * Wasting three floats per cut beats reconstructing dy from dx: the sign would
 * have to be smuggled into another field and the square root would not round
 * the way the C does, and this loop has to agree with fly_world_ground to five
 * centimetres. */
#define FLY_GLSL_MAX_LOC 28
/* Must equal FLY_CUT_MAX in fly_world.h; fly_render.c asserts that it does.
 * Written out rather than included because this header is the shader source
 * and knows nothing about the game's structs. */
#define FLY_GLSL_MAX_CUT 24
/* And the water on the land: must equal FLY_RIVER_MAX, FLY_RIVER_PT_MAX and
 * FLY_LAKE_MAX in fly_world.h, which fly_render.c asserts. Written out for the
 * same reason the cut cap is.
 *
 *   uRiver[r]    = vec4(first station, station count, bed depth, bank batter)
 *   uRiverBox[r] = vec4(x0, y0, x1, y1) — everything that river touches
 *   uRiverPt[i]  = vec4(x, y, surface, channel half width)
 *   uLake[i]     = vec4(x, y, radius, surface)
 *
 * One row a station, which is what makes a watercourse affordable at all: a
 * cutting takes three. The per-river box is what makes the *loop* affordable —
 * water covers a fraction of a per cent of a chart, so four comparisons reject
 * every station of every river for almost every sample the shader takes. */
#define FLY_GLSL_MAX_RIVER 4
#define FLY_GLSL_MAX_RIVER_PT 64
#define FLY_GLSL_MAX_LAKE 6
/* twins of FLY_RIVER_EDGE and FLY_RIVER_DRY in fly_world.h */
#define FLY_GLSL_RIVER_EDGE "0.35"
#define FLY_GLSL_RIVER_DRY "0.5"
/* twins of fly_river_batter and FLY_RIVER_LIP in fly_world.h */
#define FLY_GLSL_RIVER_BATTER_K "2.0"
#define FLY_GLSL_RIVER_BATTER_C "26.0"
#define FLY_GLSL_RIVER_LIP "3.0"
/* twin of FLY_LAKE_EDGE in fly_world.h */
#define FLY_GLSL_LAKE_EDGE "450.0"

/* The array bound goes into the shader as an expression rather than as the
 * number 72, so the two cannot drift apart when the cap moves. */
#define FLY_GLSL__STR2(x) #x
#define FLY_GLSL__STR(x) FLY_GLSL__STR2(x)

#define FLY_GLSL_WORLD \
    "uniform vec4 uChartFrame;\n"                                              \
    "uniform int uCutCount;\n"                                                 \
    "uniform vec4 uCut[" FLY_GLSL__STR(FLY_GLSL_MAX_CUT) " * 3];\n"            \
    "uniform int uRiverCount;\n"                                               \
    "uniform vec4 uRiver[" FLY_GLSL__STR(FLY_GLSL_MAX_RIVER) "];\n"            \
    "uniform vec4 uRiverBox[" FLY_GLSL__STR(FLY_GLSL_MAX_RIVER) "];\n"         \
    "uniform vec4 uRiverPt[" FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) "];\n"       \
    "uniform int uLakeCount;\n"                                                \
    "uniform vec4 uLake[" FLY_GLSL__STR(FLY_GLSL_MAX_LAKE) "];\n"              \
    "float fly_smoothstep(float e0, float e1, float x){\n"                     \
    "  float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);\n"                     \
    "  return t * t * (3.0 - 2.0 * t);\n"                                      \
    "}\n"                                                                      \
    /* the chart-to-ball mapping: exact port of fly_world_sphere_dir
       (fly_world.c), written in the same order of operations so the two
       renderers round the same way */                                         \
    "vec3 fly_sphere_dir(vec2 p){\n"                                           \
    "  float d = sqrt(p.x * p.x + p.y * p.y);\n"                               \
    "  float a = d / " FLY_GLSL_PLANET_R ";\n"                                 \
    "  float sa = sin(a), ca = cos(a);\n"                                      \
    "  if (d < 1e-6) return vec3(0.0, 0.0, 1.0);\n"                            \
    "  float ux = p.x / d, uy = p.y / d;\n"                                    \
    "  return vec3(sa * ux, sa * uy, ca);\n"                                   \
    "}\n"                                                                      \
    /* twin of fly_world_ball_dir: the same mapping, turned by where the chart's
       centre sits on the ball. Rodrigues about the axis across the origin's
       bearing — a rotation, not an offset added to p, for the two reasons set
       out at fly_chart_frame */                                               \
    "vec3 fly_ball_dir(vec2 p){\n"                                            \
    "  vec3 v = fly_sphere_dir(p);\n"                                         \
    "  float kv = uChartFrame.x * v.y - uChartFrame.y * v.x;\n"               \
    "  float w = kv * (1.0 - uChartFrame.w);\n"                               \
    "  return vec3(v.x * uChartFrame.w + uChartFrame.x * v.z * uChartFrame.z\n" \
    "                - uChartFrame.y * w,\n"                                  \
    "              v.y * uChartFrame.w + uChartFrame.y * v.z * uChartFrame.z\n" \
    "                + uChartFrame.x * w,\n"                                  \
    "              v.z * uChartFrame.w\n"                                     \
    "                - (uChartFrame.x * v.x + uChartFrame.y * v.y)\n"         \
    "                  * uChartFrame.z);\n"                                   \
    "}\n"                                                                     \
    /* twins of fly_world_continent, continent_z, fly_world_aridity and
       fly_world_snowline (fly_world.c): the planet the chart is drawn on —
       where its oceans are and what its climate does — as functions of the
       direction on the ball and nothing else */                              \
    "float fly_continent(uint s, vec3 u){\n"                                   \
    "  return fly_fbm3(s + 3u, u.x*2.4, u.y*2.4, u.z*2.4, 5) - 0.17;\n"        \
    "}\n"                                                                      \
    "float fly_continent_z(float c){\n"                                        \
    "  return -3400.0 + 3400.0*fly_smoothstep(-0.13, 0.0, c)\n"                \
    "                 +  450.0*fly_smoothstep(0.0, 0.40, c);\n"                \
    "}\n"                                                                      \
    "float fly_aridity(uint s, vec3 u){\n"                                     \
    "  float t = (abs(u.z) - 0.40)/0.20;\n"                                    \
    "  float n = 0.5 + 0.5*fly_fbm3(s + 61u, u.x*1.7, u.y*1.7, u.z*1.7, 3);\n" \
    "  float inland = fly_smoothstep(0.10, 0.42, fly_continent(s, u));\n"      \
    "  return clamp(n + 0.55*exp(-t*t) + 0.30*inland - 0.68, 0.0, 1.0);\n"     \
    "}\n"                                                                      \
    "float fly_snowline(uint s, vec3 u){\n"                                    \
    "  return 2050.0 - 2400.0*fly_smoothstep(0.60, 0.93, abs(u.z))\n"          \
    "       + 320.0*fly_fbm3(s + 63u, u.x*3.0, u.y*3.0, u.z*3.0, 2);\n"        \
    "}\n"                                                                      \
/* Twins of planet_albedo and planet_cloud (fly_render.c): the face of the
   planet is its water, its climate, its ice and the weather on top — four
   fields fly_world owns, at the only scale four hundred kilometres of air
   leaves anything of. */ \
    "vec3 fly_planet_albedo(vec3 u, float gh){\n" \
    "  float arid = fly_aridity(uint(uSeed), u);\n" \
    "  float snow = fly_snowline(uint(uSeed), u) + arid*800.0;\n" \
    "  vec3 c;\n" \
    "  if (gh <= " FLY_GLSL_WATER_Z ") {\n" \
    "    c = mix(vec3(0.052, 0.098, 0.120), vec3(0.008, 0.020, 0.052),\n" \
    "            fly_smoothstep(15.0, 700.0, " FLY_GLSL_WATER_Z " - gh));\n" \
    "  } else {\n" \
    "    c = mix(vec3(0.042, 0.098, 0.030), vec3(0.205, 0.180, 0.088),\n" \
    "            fly_smoothstep(0.22, 0.62, arid));\n" \
    "    c = mix(c, vec3(0.400, 0.305, 0.165), fly_smoothstep(0.55, 0.90, arid));\n" \
    "    c = mix(c, vec3(0.135, 0.125, 0.115),\n" \
    "            fly_smoothstep(snow - 500.0, snow + 400.0, gh));\n" \
    "  }\n" \
    "  return mix(c, vec3(0.62, 0.66, 0.72),\n" \
    "             fly_smoothstep(snow, snow + 900.0, max(gh, " FLY_GLSL_WATER_Z ")));\n" \
    "}\n" \
    /* twin of orogeny: ranges are built at the edges of continents and where
       two of them meet, not scattered evenly over the land */                \
    "float fly_orogeny(float c){\n"                                            \
    "  float coastal = fly_smoothstep(-0.01, 0.12, c)\n"                       \
    "                * (1.0 - fly_smoothstep(0.16, 0.40, c));\n"               \
    "  return clamp(coastal + fly_smoothstep(0.34, 0.60, c), 0.0, 1.0);\n"     \
    "}\n"                                                                      \
    "float fly_ground_raw(uint s, vec2 p){\n"                                  \
    "  vec3 u = fly_ball_dir(p);\n"                                \
    "  float c = fly_continent(s, u);\n"                                       \
    "  float bs = " FLY_GLSL_PLANET_R " / 9000.0;\n"                           \
    "  vec3 b = vec3(u.x * bs, u.y * bs, u.z * bs);\n"                         \
    "  float base = fly_fbm3(s, b.x, b.y, b.z, 5);\n"                          \
    "  float ridge = 1.0 - abs(fly_noise3(s + 77u, b.x * 0.6, b.y * 0.6, b.z * 0.6));\n" \
    "  float mnt = ridge * ridge *\n"                                          \
    "      fly_smoothstep(0.1, 0.7, fly_noise3(s + 5u, b.x * 0.35, b.y * 0.35, b.z * 0.35))\n" \
    "      * fly_orogeny(c);\n"                                                \
    "  float h = fly_continent_z(c)\n"                                         \
    "          + 120.0 + base * 260.0 + mnt * 2400.0;\n"                       \
    "  float d1 = " FLY_GLSL_PLANET_R " / 210.0, d2 = " FLY_GLSL_PLANET_R " / 46.0;\n" \
    "  float det = fly_noise3(s + 9u, u.x * d1, u.y * d1, u.z * d1) * 9.0 +\n" \
    "              fly_noise3(s + 10u, u.x * d2, u.y * d2, u.z * d2) * 2.2;\n" \
    "  h += det * fly_smoothstep(3.0, 40.0, h);\n"                             \
    /* no clamp at zero: the sea floor is terrain, and the water needs its
     * depth — see ground_raw in fly_world.c */                                \
    "  return h;\n"                                                            \
    "}\n"                                                                      \
    /* pad aprons are leveled and graded back to natural terrain, matching
     * FLY_PAD_FLAT_R / FLY_PAD_BLEND_R in fly_world.c */                      \
    /* twin of river_span in fly_world.c: how far off one station's span a point
       is, and where along it that lands. Written in the same order so the two
       round the same way — gpu.ground holds them to five centimetres. */      \
    /* twin of fly_river_batter in fly_world.h */                              \
    "float fly_river_batter(float wide){\n"                                   \
    "  return " FLY_GLSL_RIVER_BATTER_K "*wide + " FLY_GLSL_RIVER_BATTER_C ";\n" \
    "}\n"                                                                     \
    "bool fly_river_span(vec4 a, vec4 b, float pad, vec2 p, out float d, out float m){\n" \
    "  vec2 e = b.xy - a.xy;\n"                                               \
    "  vec2 r = p - a.xy;\n"                                                  \
    "  float len2 = dot(e, e);\n"                                             \
    "  float reach = max(a.w, b.w) + pad;\n"                                  \
    "  d = 0.0; m = 0.0;\n"                                                   \
    "  if (p.x < min(a.x, b.x) - reach || p.x > max(a.x, b.x) + reach) return false;\n" \
    "  if (p.y < min(a.y, b.y) - reach || p.y > max(a.y, b.y) + reach) return false;\n" \
    "  if (len2 < 1e-3) return false;\n"                                      \
    "  m = clamp(dot(r, e) / len2, 0.0, 1.0);\n"                              \
    "  vec2 c = r - e * m;\n"                                                 \
    "  d = sqrt(dot(c, c));\n"                                                \
    "  return true;\n"                                                        \
    "}\n"                                                                     \
    /* twin of river_carve: the channel taken out of the ground, a min so it
       can only ever lower it */                                               \
    "float fly_river_carve(vec2 p, float h){\n"                               \
    "  for (int r = 0; r < uRiverCount; ++r) {\n"                             \
    "    vec4 bb = uRiverBox[r];\n"                                           \
    "    if (p.x < bb.x || p.x > bb.z || p.y < bb.y || p.y > bb.w) continue;\n" \
    "    int first = int(uRiver[r].x), n = int(uRiver[r].y);\n"               \
    "    float bed = uRiver[r].z, batter = uRiver[r].w;\n"                    \
    "    for (int i = 0; i < " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) "; ++i) {\n" \
    "      if (i + 1 >= n || first + i + 1 >= " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) ") break;\n" \
    "      vec4 a = uRiverPt[first + i], b = uRiverPt[first + i + 1];\n"      \
    "      float d, t;\n"                                                     \
    "      if (!fly_river_span(a, b, batter, p, d, t)) continue;\n"           \
    "      float wide = mix(a.w, b.w, t);\n"                                  \
    "      float bt = fly_river_batter(wide);\n"                              \
    "      float k = fly_smoothstep(wide + bt, wide, d);\n"                   \
    "      if (k <= 0.0) continue;\n"                                         \
    "      float z = mix(a.z, b.z, t) - bed\n"                                \
    "              + (bed + " FLY_GLSL_RIVER_LIP ")*fly_smoothstep(wide, wide + bt, d);\n" \
    "      z = mix(h, z, k);\n"                                               \
    "      if (z < h) h = z;\n"                                               \
    "    }\n"                                                                 \
    "  }\n"                                                                   \
    "  return h;\n"                                                           \
    "}\n"                                                                     \
    /* twin of fly_world_still: the sea, and a lake inside its own disc, let
       down to under the ground over the rim — see FLY_LAKE_EDGE */            \
    "float fly_still(vec2 p, float gz){\n"                                    \
    "  float wz = " FLY_GLSL_WATER_Z ";\n"                                    \
    "  for (int i = 0; i < uLakeCount; ++i) {\n"                              \
    "    vec2 q = p - uLake[i].xy;\n"                                         \
    "    if (abs(q.x) > uLake[i].z || abs(q.y) > uLake[i].z) continue;\n"     \
    "    float d = length(q);\n"                                              \
    "    if (d > uLake[i].z) continue;\n"                                     \
    "    wz = max(wz, mix(gz - " FLY_GLSL_RIVER_DRY ", uLake[i].w,\n"         \
    "                     fly_smoothstep(uLake[i].z, uLake[i].z - " FLY_GLSL_LAKE_EDGE ", d)));\n" \
    "  }\n"                                                                   \
    "  return wz;\n"                                                          \
    "}\n"                                                                     \
    "float fly_ground(vec2 p){\n"                                             \
    "  float h = fly_ground_raw(uint(uSeed), p);\n"                           \
    /* the rivers first: they are what the country was found with, and the
       aprons are graded and the cuttings dug into ground that already has its
       valleys — see fly_world_ground, which this is the twin of */            \
    "  h = fly_river_carve(p, h);\n"                                          \
    "  for (int i = 0; i < uLocCount; ++i) {\n"                                \
    "    vec2 d = p - uLoc[i].xy;\n"                                           \
    "    if (abs(d.x) > 150.0 || abs(d.y) > 150.0) continue;\n"                \
    "    float dist = sqrt(d.x * d.x + d.y * d.y);\n"                          \
    "    if (dist >= 150.0) continue;\n"                                       \
    "    h = mix(h, uLoc[i].z, fly_smoothstep(150.0, 75.0, dist));\n"          \
    "  }\n"                                                                    \
    /* the cuttings, line for line with the second loop of fly_world_ground:
       after the aprons, and a min rather than a blend so a cut can only ever
       take material away */                                                   \
    "  for (int i = 0; i < uCutCount; ++i) {\n"                                \
    "    vec4 ln = uCut[i * 3];\n"                                             \
    "    vec4 bx = uCut[i * 3 + 1];\n"                                         \
    "    vec2 r = p - ln.xy;\n"                                                \
    "    float u = r.x * ln.z + r.y * ln.w;\n"                                 \
    "    float v = r.x * -ln.w + r.y * ln.z;\n"                                \
    "    float au = abs(u), av = abs(v);\n"                                    \
    "    if (au > bx.x + bx.z || av > bx.y + bx.z) continue;\n"                \
    "    float wu = fly_smoothstep(bx.x + bx.z, bx.x, au);\n"                  \
    "    float wv = fly_smoothstep(bx.y + bx.z, bx.y, av);\n"                  \
    "    if (wu * wv <= 0.0) continue;\n"                                      \
    "    float t = bx.x > 1e-3 ? (u / bx.x + 1.0) * 0.5 : 0.5;\n"              \
    "    float z = mix(bx.w, uCut[i * 3 + 2].x, clamp(t, 0.0, 1.0));\n"        \
    "    z = mix(h, z, wu * wv);\n"                                            \
    "    if (z < h) h = z;\n"                                                  \
    "  }\n"                                                                    \
    "  return h;\n"                                                            \
    "}\n"                                                                      \
    /* twin of fly_world_water_at: the still water, and the rivers let down to
       under the ground over the outer part of the bank — see FLY_RIVER_EDGE in
       fly_world.h for why the level is not carried flat to the footprint's own
       edge. After fly_ground rather than before it, because that is what it is
       let down against. */                                                    \
    "float fly_water_at(vec2 p, float gz){\n"                                 \
    "  float wz = fly_still(p, gz);\n"                                        \
    "  for (int r = 0; r < uRiverCount; ++r) {\n"                             \
    "    vec4 bb = uRiverBox[r];\n"                                           \
    "    if (p.x < bb.x || p.x > bb.z || p.y < bb.y || p.y > bb.w) continue;\n" \
    "    int first = int(uRiver[r].x), n = int(uRiver[r].y);\n"               \
    "    float batter = uRiver[r].w;\n"                                       \
    "    for (int i = 0; i < " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) "; ++i) {\n" \
    "      if (i + 1 >= n || first + i + 1 >= " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) ") break;\n" \
    "      vec4 a = uRiverPt[first + i], b = uRiverPt[first + i + 1];\n"      \
    "      float d, t;\n"                                                     \
    "      if (!fly_river_span(a, b, batter, p, d, t)) continue;\n"           \
    "      float wide = mix(a.w, b.w, t);\n"                                  \
    "      float bt = fly_river_batter(wide);\n"                              \
    "      if (d > wide + bt) continue;\n"                                    \
    "      float edge = fly_smoothstep(wide + bt,\n"                          \
    "                                  wide + " FLY_GLSL_RIVER_EDGE "*bt, d);\n" \
    "      wz = max(wz, mix(gz - " FLY_GLSL_RIVER_DRY ", mix(a.z, b.z, t), edge));\n" \
    "    }\n"                                                                 \
    "  }\n"                                                                   \
    "  return wz;\n"                                                          \
    "}\n"                                                                     \
    /* twin of fly_world_water: the same, taking the ground lookup itself and
       only where there is water to let down against */                        \
    /* twin of fly_world_shore: the level the body has, without the letting-down
       at its edge — what the ground is described against */                    \
    "float fly_shore(vec2 p){\n"                                              \
    "  float wz = " FLY_GLSL_WATER_Z ";\n"                                    \
    "  for (int i = 0; i < uLakeCount; ++i) {\n"                              \
    "    vec2 q = p - uLake[i].xy;\n"                                         \
    "    if (dot(q, q) > uLake[i].z*uLake[i].z) continue;\n"                  \
    "    wz = max(wz, uLake[i].w);\n"                                         \
    "  }\n"                                                                   \
    "  for (int r = 0; r < uRiverCount; ++r) {\n"                             \
    "    vec4 bb = uRiverBox[r];\n"                                           \
    "    if (p.x < bb.x || p.x > bb.z || p.y < bb.y || p.y > bb.w) continue;\n" \
    "    int first = int(uRiver[r].x), n = int(uRiver[r].y);\n"               \
    "    float batter = uRiver[r].w;\n"                                       \
    "    for (int i = 0; i < " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) "; ++i) {\n" \
    "      if (i + 1 >= n || first + i + 1 >= " FLY_GLSL__STR(FLY_GLSL_MAX_RIVER_PT) ") break;\n" \
    "      vec4 a = uRiverPt[first + i], b = uRiverPt[first + i + 1];\n"      \
    "      float d, t;\n"                                                     \
    "      if (!fly_river_span(a, b, batter, p, d, t)) continue;\n"           \
    "      float wide = mix(a.w, b.w, t);\n"                                  \
    "      if (d > wide + fly_river_batter(wide)) continue;\n"                \
    "      wz = max(wz, mix(a.z, b.z, t));\n"                                 \
    "    }\n"                                                                 \
    "  }\n"                                                                   \
    "  return wz;\n"                                                          \
    "}\n"                                                                     \
    "float fly_water(vec2 p){\n"                                              \
    "  for (int r = 0; r < uRiverCount; ++r) {\n"                             \
    "    vec4 bb = uRiverBox[r];\n"                                           \
    "    if (p.x >= bb.x && p.x <= bb.z && p.y >= bb.y && p.y <= bb.w)\n"     \
    "      return fly_water_at(p, fly_ground(p));\n"                          \
    "  }\n"                                                                   \
    "  for (int i = 0; i < uLakeCount; ++i) {\n"                              \
    "    vec2 q = p - uLake[i].xy;\n"                                         \
    "    if (dot(q, q) <= uLake[i].z*uLake[i].z) return fly_water_at(p, fly_ground(p));\n" \
    "  }\n"                                                                   \
    "  return " FLY_GLSL_WATER_Z ";\n"                                        \
    "}\n"                                                                     \
    "vec3 fly_ground_normal(vec2 p, float e){\n"                               \
    "  float hx = fly_ground(p + vec2(e, 0.0)) - fly_ground(p - vec2(e, 0.0));\n" \
    "  float hy = fly_ground(p + vec2(0.0, e)) - fly_ground(p - vec2(0.0, e));\n" \
    "  return normalize(vec3(-hx / (2.0 * e), -hy / (2.0 * e), 1.0));\n"       \
    "}\n"                                                                      \
    /* adaptive height-field march; returns hit distance or -1 (trace_terrain) */ \
    "float fly_trace_terrain(vec3 ro, vec3 rd, float tmax){\n"                 \
    "  float t = 2.0;\n"                                                       \
    "  float last_dh = ro.z - fly_ground(ro.xy);\n"                            \
    "  float last_t = 0.0;\n"                                                  \
    "  for (int i = 0; i < 512; ++i) {\n"                                      \
    "    if (t >= tmax) return -1.0;\n"                                        \
    "    vec3 p = ro + rd * t;\n"                                              \
    /* Above FLY_GROUND_CEILING and still climbing, so nothing is left to hit
     * and the remaining steps would all answer "sky". The constant is repeated
     * from fly_world.h the way every constant in this file is, and the CPU twin
     * carries the arithmetic that proves it bounds the field. */ \
    "    if (rd.z >= 0.0 && p.z > 3300.0) return -1.0;\n"                      \
    "    float dh = p.z - fly_ground(p.xy);\n"                                 \
    "    if (dh < 0.0)\n"                                                      \
    "      return last_t + (t - last_t) * (last_dh / (last_dh - dh + 1e-6));\n" \
    "    last_dh = dh; last_t = t;\n"                                          \
    /* Distance-scaled floor, so the loop reaches tmax inside its budget — see
     * trace_terrain in fly_render.c for what running out of it looked like,
     * and for why the floor is held under the ceiling with min(). Reversing
     * clamp()'s range is undefined in GLSL: min(max(x,lo),hi) answers 200 and
     * max(min(x,hi),lo) answers 403, so past 9850 m this and its CPU twin were
     * entitled to disagree about where the ground is. */ \
    "    t += clamp(dh * 0.4, min(3.0 + t*0.02, 200.0), 200.0);\n"             \
    "  }\n"                                                                    \
    /* Budget exhausted with the ray still above ground. It cannot happen with
     * the floor above, and if it ever does the honest answer is "something is
     * there" — reporting a miss punches a hole clean through the world. */ \
    "  return last_t;\n"                                                       \
    "}\n"                                                                      \
    /* twin of fly_world_forest_mask: the same two octaves the scatter plants
       stands from, so the litter on the ground, the trees standing in it and
       the lid the cascades cast it as all agree about where the wood is.

       It lives with the ground rather than with the shading because it is a
       field of the world and three shaders that never shade anything want it —
       the caster grid among them. */                                          \
    /* `patch` is a reserved word in GLSL ES — tessellation — so the octaves
       are named differently here than in the C twin, which is the one place
       these two are allowed to disagree */                                    \
    "float fly_forest_mask(uint s, vec2 p){\n"                                 \
    "  float wide  = 0.5 + 0.5*fly_noise2(s + 91u, p.x/1100.0, p.y/1100.0);\n" \
    "  float clump = 0.5 + 0.5*fly_noise2(s + 92u, p.x/290.0, p.y/290.0);\n"   \
    "  return fly_smoothstep(0.48, 0.66, wide*0.85 + clump*0.15);\n"           \
    "}\n"                                                                      \
    /* Twin of fly_world_canopy: how far a stand's crown mass stands above the
       ground, and 0 where nothing grows. `h` is the caller's own fly_ground —
       every caller has just taken it, and taking it again here is the one
       expense this function must not have.

       The clearing loop is uLoc, so a site's ground has to be inside the array
       the caller uploaded or the lid grows over the airfield: see
       env_ground_uniforms, whose location cull reaches FLY_CANOPY_REACH for
       exactly this. uLoc[i].w carries that site's own clearing radius, which
       is what fly_world_clearing returns for its kind.

       Twin of forest_open, not of fly_world_forest: the slope cut on the end
       of the C field needs a second fly_ground and is left out of both sides
       — see forest_open in fly_world.c.

       `hgt` is FLY_CANOPY_H, passed in rather than written here: this header
       is shader source and cannot include fly_world.h, and a second copy of
       the number is a second thing to keep true. Zero for a caller that wants
       the field but not the lid. */                                           \
    /* Twin of site_clearings: whose ground this is and how far out on it, in
       units of that settlement's own clearing radius. `x` is the distance and
       `y` the winning row of uLoc, as a float because that is what a caller
       indexes uLoc with anyway. Both the clearing and the worked belt hang off
       it and neither may pay for its own loop. */                             \
    "vec2 fly_site_clearings(vec2 p){\n"                                       \
    "  float best = 1e30, won = -1.0;\n"                                       \
    "  for (int i = 0; i < uLocCount; ++i) {\n"                                \
    "    vec2 q = p - uLoc[i].xy;\n"                                           \
    "    float u = dot(q, q) / (uLoc[i].w * uLoc[i].w);\n"                     \
    "    if (u < best) { best = u; won = float(i); }\n"                        \
    "  }\n"                                                                    \
    "  return vec2(sqrt(best), won);\n"                                        \
    "}\n"                                                                      \
    /* Twin of tilth_work in fly_world.c: how strongly the ground `u` clearings
       out from its settlement is worked. Gate for gate and early-out for
       early-out with the C, including the two `< 0.02` cuts, which are what
       keep the noise taps off the ninety per cent of the world that is not
       farmland. The bounds are fly_world.h's; see the FLY_GLSL_TILTH_* notes
       above for why they are written out again here. */                       \
    /* Twin of tilth_ground: whether the ground could carry a field at all,
       before anything is known about whose it is. Free, and it is what lets
       fly_tilth skip the settlement loop on a mountain or over water — every
       other term is a factor no greater than one, so the skip is exact. */   \
    "float fly_tilth_ground(float wz, float h){\n"                            \
    "  return fly_smoothstep(wz + 2.0, wz + 12.0, h)\n"                       \
    "       * (1.0 - fly_smoothstep(540.0, 880.0, h));\n"                      \
    "}\n"                                                                      \
    "float fly_tilth_work(uint s, vec2 p, float h, float u){\n"                \
    "  if (u > " FLY_GLSL_TILTH_OUT " / " FLY_GLSL_TILTH_RAGGED ") return 0.0;\n" \
    "  float t = fly_tilth_ground(fly_shore(p), h);\n"                          \
    "  t *= fly_smoothstep(" FLY_GLSL_TILTH_IN ", " FLY_GLSL_TILTH_FULL ", u);\n" \
    "  if (t < 0.02) return 0.0;\n"                                            \
    "  float parish = 0.5 + 0.5*fly_noise2(s + 45u, p.x/1450.0, p.y/1450.0);\n" \
    "  t *= fly_smoothstep(0.15, 0.46, parish);\n"                             \
    "  if (t < 0.02) return 0.0;\n"                                            \
    "  float ragged = 0.5 + 0.5*fly_noise2(s + 46u, p.x/620.0, p.y/620.0);\n"  \
    "  t *= 1.0 - fly_smoothstep(" FLY_GLSL_TILTH_FADE ", " FLY_GLSL_TILTH_OUT ",\n" \
    "        u * (" FLY_GLSL_TILTH_RAGGED " + (1.0 - " FLY_GLSL_TILTH_RAGGED ")*2.0*ragged));\n" \
    "  return t < 0.02 ? 0.0 : t;\n"                                           \
    "}\n"                                                                      \
    /* Twin of fly_world_parcel: which field, and how far its boundary is.
       `li` is a float index into uLoc for the reason fly_site_clearings
       returns one. abs(mod(fy, 2.0)) is fabsf(fmodf(fy, 2.0)) on the C side —
       the two disagree on the sign of a negative remainder and agree on its
       magnitude, which is all this uses. */                                   \
    "vec2 fly_tilth_parcel(uint s, float li, vec2 p){\n"                       \
    "  vec2 site = uLoc[int(li)].xy;\n"                                        \
    "  float ang = " FLY_GLSL_PI " * fly_noise2(s + 47u, site.x/1000.0, site.y/1000.0);\n" \
    "  float ca = cos(ang), sa = sin(ang);\n"                                  \
    "  float pw = " FLY_GLSL_PARCEL_MIN " + " FLY_GLSL_PARCEL_VAR "\n"         \
    "           * (0.5 + 0.5*fly_noise2(s + 48u, site.y/1000.0, site.x/1000.0));\n" \
    "  float pl = pw * " FLY_GLSL_PARCEL_LONG ";\n"                            \
    "  vec2 d = p - site;\n"                                                   \
    "  float qx = d.x*ca + d.y*sa, qy = -d.x*sa + d.y*ca;\n"                   \
    "  qx += 7.0*fly_noise2(s + 49u, p.x/74.0, p.y/74.0);\n"                   \
    "  qy += 7.0*fly_noise2(s + 49u, p.y/74.0 + 31.0, p.x/74.0 - 17.0);\n"     \
    "  float fy = floor(qy / pl);\n"                                           \
    "  qx += abs(mod(fy, 2.0)) * pw * 0.5;\n"                                  \
    "  float fx = floor(qx / pw);\n"                                           \
    "  float tx = qx/pw - fx, ty = qy/pl - fy;\n"                              \
    "  float ex = (tx < 0.5 ? tx : 1.0 - tx) * pw;\n"                          \
    "  float ey = (ty < 0.5 ? ty : 1.0 - ty) * pl;\n"                          \
    "  float crop = 0.5 + 0.5*fly_noise2(s + 50u, (fx + 0.5)*0.8137, (fy + 0.5)*0.6529);\n" \
    "  return vec2(crop, min(ex, ey));\n"                                      \
    "}\n"                                                                      \
    /* Twin of fly_world_tilth: (work, crop, edge). The parcel frame is three
       noise taps and open country must not pay them, so it is behind the same
       gate the C keeps it behind. */                                          \
    "vec3 fly_tilth(uint s, vec2 p, float h){\n"                               \
    "  if (fly_tilth_ground(fly_shore(p), h) < 0.02) return vec3(0.0);\n"       \
    "  vec2 sc = fly_site_clearings(p);\n"                                     \
    "  if (sc.y < 0.0) return vec3(0.0);\n"                                    \
    "  float work = fly_tilth_work(s, p, h, sc.x);\n"                          \
    "  if (work <= 0.0) return vec3(0.0);\n"                                   \
    "  return vec3(work, fly_tilth_parcel(s, sc.y, p));\n"                     \
    "}\n"                                                                      \
    "float fly_canopy(uint s, vec2 p, float h, float hgt){\n"                  \
    "  float d = fly_forest_mask(s, p);\n"                                     \
    "  if (d < 0.03) return 0.0;\n"                                            \
    /* nothing grows in the water, measured off the water that is here — twin
       of the band forest_open opens with */                                   \
    "  float wz = fly_shore(p);\n"                                             \
    "  d *= fly_smoothstep(wz + 1.0, wz + 8.0, h);\n"                         \
    "  d *= 1.0 - fly_smoothstep(640.0, 990.0, h);\n"                          \
    "  if (d < 0.03) return 0.0;\n"                                            \
    "  vec2 sc = fly_site_clearings(p);\n"                                     \
    "  d *= fly_smoothstep(1.0, 2.53, sc.x);\n"                                \
    "  if (d < 0.03) return 0.0;\n"                                            \
    /* and a field is cleared ground — see forest_open in fly_world.c */        \
    "  d *= 1.0 - fly_smoothstep(0.10, 0.45, fly_tilth_work(s, p, h, sc.x));\n" \
    "  return d < 0.03 ? 0.0 : d * hgt;\n"                                     \
    "}\n"


/* Raster shading model: port of lit_surface from fly_render.c — hemispheric
 * sky ambient, direct sun, a green-tinted ground bounce on downward faces and
 * a cool moonlight floor. Shared by the GPU raster environment so its terrain
 * shades identically to the CPU rasterizer's. */
#define FLY_GLSL_LIT \
    /* twin of sky_irradiance: the sky's own light on a surface facing n, from
       four coefficients the CPU projects off the scattering model once a frame
       — see make_env in fly_render.c */ \
    "vec3 fly_sky_irradiance(vec3 n){\n" \
    "  const float a0 = 1.0, a1 = 2.0/3.0, a2 = 0.25;\n" \
    "  vec3 c = uSH[0]*(a0*0.282095);\n" \
    "  c += uSH[1]*(a1*0.488603*n.y) + uSH[2]*(a1*0.488603*n.z)\n" \
    "     + uSH[3]*(a1*0.488603*n.x);\n" \
    "  c += uSH[4]*(a2*1.092548*n.x*n.y);\n" \
    "  c += uSH[5]*(a2*1.092548*n.y*n.z);\n" \
    "  c += uSH[6]*(a2*0.315392*(3.0*n.z*n.z - 1.0));\n" \
    "  c += uSH[7]*(a2*1.092548*n.x*n.z);\n" \
    "  c += uSH[8]*(a2*0.546274*(n.x*n.x - n.y*n.y));\n" \
    "  return max(c, vec3(0.0));\n" \
    "}\n" \
    "vec3 fly_lit_surface(vec3 alb, vec3 n, float shadow, float ao){\n" \
    "  float ndl = clamp(dot(n, uSun), 0.0, 1.0);\n" \
    "  vec3 c = alb*(fly_sky_irradiance(n)*ao);\n" \
    "  c += alb*ndl*shadow*uSunLight;\n" \
    /* the ground's own light on the lower half of the hemisphere — see
       make_env in fly_render.c; uGround already carries the form factor */ \
    "  float down = 0.5 - 0.5*clamp(n.z, -1.0, 1.0);\n" \
    "  c += alb*uGround*(down*ao);\n" \
    /* Moonlight: through the albedo, because it is light falling on the surface
       rather than light coming off it — see FLY_MOON in fly_render.c. Clamped
       at nothing: a light term that goes negative is not a light term, and
       below n.z = -2/3 this one was subtracting. */ \
    "  c += alb*" FLY_GLSL_MOON "*((1.0 - uDay)*clamp(0.4 + 0.6*n.z, 0.0, 1.0));\n" \
    "  return c;\n" \
    "}\n" \
    /* terrain_light in fly_render.c: the wrap, hot spot and transmission that
     * make a canopy shade like blades, plus a material Blinn lobe */ \
    "vec3 fly_terrain_light(vec3 alb, vec3 mat, vec3 n, vec3 vdir, float shadow, float ao){\n" \
    "  float veg = mat.x;\n" \
    "  float raw = dot(n, uSun);\n" \
    "  float wrap = 0.35*veg;\n" \
    "  float ndl = clamp((raw + wrap)/(1.0 + wrap), 0.0, 1.0);\n" \
    "  float align = clamp(dot(vdir, uSun), 0.0, 1.0);\n" \
    "  float back = clamp(-dot(vdir, uSun), 0.0, 1.0);\n" \
    "  vec3 hv = normalize(vdir + uSun);\n" \
    "  float nh = clamp(dot(n, hv), 0.0, 1.0);\n" \
    "  float vh = 1.0 - clamp(dot(vdir, hv), 0.0, 1.0);\n" \
    "  float vh2 = vh*vh;\n" \
    /* grazing reflectance is capped by roughness and by vegetation: Schlick
       alone takes a meadow to a mirror edge-on — see terrain_light */ \
    "  float rough = sqrt(2.0/(mat.z + 2.0));\n" \
    "  float fcap = max((1.0 - rough)*(1.0 - clamp(veg, 0.0, 1.0)), mat.y);\n" \
    "  float fres = mat.y + (fcap - mat.y)*vh2*vh2*vh;\n" \
    "  float lobe = pow(nh, mat.z)*(mat.z + 8.0)*0.03979;\n" \
    "  vec3 c = fly_lit_surface(alb, n, 0.0, ao);\n" \
    /* the sky this surface reflects, in the ambient half so the whole thing
       stays linear in `shadow` — see terrain_light in fly_render.c */ \
    "  vec3 refl = 2.0*dot(n, vdir)*n - vdir;\n" \
    "  float om = 1.0 - clamp(dot(n, vdir), 0.0, 1.0);\n" \
    "  float fe = mat.y + (fcap - mat.y)*om*om*om*om*om;\n" \
    "  c += fly_sky_irradiance(normalize(refl))*(fe*ao);\n" \
    "  vec3 direct = alb*ndl*uSunLight;\n" \
    "  direct *= 1.0 + veg*0.38*align*align;\n" \
    "  direct += alb*vec3(1.15,1.30,0.55)*uSunLight*\n" \
    "            (veg*0.55*back*back*back*(1.0 - clamp(raw, 0.0, 1.0)));\n" \
    "  if (raw > 0.0) direct += uSunLight*(lobe*fres);\n" \
    "  return c + direct*shadow;\n" \
    "}\n"


/* Shading: ports of grass_region, terrain_surface, cloud_cover/cloud_shadow,
 * sky_radiance, the fog model and pt_shade from fly_render.c. GLSL cannot
 * recurse, so pt_shade's one indirect bounce is flattened into an explicit
 * no-bounce helper rather than a self-call.
 *
 * Expects, in addition to FLY_GLSL_WORLD's uniforms:
 *   uniform vec3  uSun; uniform float uDay, uDusk, uStorm, uWet, uTime;
 *   uniform vec3  uWind; uniform float uPxScale; */
#define FLY_GLSL_CLOUD_Z "2600.0"
/* twins of FLY_CLOUD_BASE / FLY_CLOUD_RISE / FLY_CLOUD_MIN in fly_render.c,
   where the argument for a flat base and a varying top is written out */
#define FLY_GLSL_CLOUD_BASE "260.0"
#define FLY_GLSL_CLOUD_RISE "320.0"
#define FLY_GLSL_CLOUD_MIN "0.42"
/* squared, because the ramp is s^2/(s^2 + k^2): twins of FLY_CLOUD_FINE and
   FLY_CLOUD_FLAT in fly_render.c */
#define FLY_GLSL_CLOUD_FINE "62500.0"
#define FLY_GLSL_CLOUD_FLAT "3240000.0"
/* twins of FLY_CLOUD_EXT and FLY_CLOUD_SEGW in fly_render.c: the metres of
   unit-density cloud that attenuate by 1/e, and how much of its own segment a
   tap averages over before the field may keep its coarse octaves */
#define FLY_GLSL_CLOUD_EXT "322.0"
#define FLY_GLSL_CLOUD_SEGW "0.65"
/* twins of the FLY_CIRRUS_* constants in fly_render.c, where the argument for
   a sheet rather than a slab is written out */
#define FLY_GLSL_CIRRUS_Z "7000.0"
#define FLY_GLSL_CIRRUS_TAU "0.62"
#define FLY_GLSL_CIRRUS_SLANT "14.0"
#define FLY_GLSL_CIRRUS_LEN "34000.0"
#define FLY_GLSL_CIRRUS_WID "9000.0"
/* squared, because the ramps are s^2/(s^2 + k^2) */
#define FLY_GLSL_CIRRUS_FINE "176400.0"
#define FLY_GLSL_CIRRUS_FLAT "9610000.0"
/* twins of FLY_CLOUD_SHADE and FLY_CIRRUS_SHADE, where what
   each layer takes out of the sunbeam and why the two do not multiply is
   written out */
#define FLY_GLSL_CLOUD_SHADE "0.62"
#define FLY_GLSL_CIRRUS_SHADE "0.286"
/* twins of FLY_CLOUD_FACE / FLY_CLOUD_FACE_LO: what the deck's crown is worth
   against its base, and where the ramp to it starts */
#define FLY_GLSL_CLOUD_FACE "0.65"
#define FLY_GLSL_CLOUD_FACE_LO "0.35"
#define FLY_GLSL_WATER_Z "2.0"
/* --- the worked ground's constants ----------------------------------------
 *
 * Twins of FLY_TILTH_* and FLY_PARCEL_* in fly_world.h, and of FLY_PI in
 * fly_math.h. Written out rather than included for the reason the cut cap is:
 * this header is shader source and cannot include either. `gpu.tilth` is what
 * holds them equal — it evaluates both sides of the field over the same
 * ground, so a constant that drifts here shows up as a disagreement about
 * where the fields are rather than as a silent difference between the two
 * renderers. */
#define FLY_GLSL_PI "3.14159265"
#define FLY_GLSL_TILTH_IN "1.05"
#define FLY_GLSL_TILTH_FULL "1.60"
#define FLY_GLSL_TILTH_FADE "4.60"
#define FLY_GLSL_TILTH_OUT "8.40"
#define FLY_GLSL_TILTH_RAGGED "0.74"
#define FLY_GLSL_PARCEL_MIN "118.0"
#define FLY_GLSL_PARCEL_VAR "116.0"
#define FLY_GLSL_PARCEL_LONG "1.55"
/* And the terrain shader's own: the plane means the parcel and hedge octaves
 * fade toward, and the crop palette, all of them terrain_surface's in
 * fly_render.c. */
#define FLY_GLSL_HEDGE_MEAN "0.0492"
#define FLY_GLSL_HEDGE_W "4.2"
#define FLY_GLSL_CROP_MEAN "vec3(0.291, 0.295, 0.125)"
#define FLY_GLSL_CROP_MEAN_VEG "0.5533"
/* twins of FLY_LAMP_* in fly_render.c: a semi-cutoff lantern's distribution */
#define FLY_GLSL_LAMP_WING "3.4"
#define FLY_GLSL_LAMP_ACROSS "0.22"
#define FLY_GLSL_LAMP_SOFT "0.75"
/* twins of FLY_GLARE_* in fly_render.c: the veiling glare around an emitter */
#define FLY_GLSL_GLARE_CORE "3.55"
#define FLY_GLSL_GLARE_HALO "0.17"
#define FLY_GLSL_GLARE_WIDE "2.4"
#define FLY_GLSL_GLARE_SPAN "5.0"
/* twins of FLY_SCAT_* in fly_render.c */
#define FLY_GLSL_SCAT_SPREAD "3.0"
#define FLY_GLSL_SCAT_MS "0.50"
#define FLY_GLSL_SCAT_SUN "8.4"
#define FLY_GLSL_SCAT_G "0.76"
#define FLY_GLSL_SCAT_STORM "0.55"
/* twin of FLY_WATER_BED in fly_render.c: the sand under the shore band and
 * under shallow water, one colour so the two sides of the waterline agree */
#define FLY_GLSL_WATER_BED "vec3(0.55, 0.50, 0.36)"

#define FLY_GLSL_SHADE \
    FLY_GLSL_LIT \
    "float fly_grass_region(uint s, vec2 p){ return fly_noise2(s + 34u, p.x/2600.0, p.y/2600.0); }\n" \
    "float fly_detail_amount(float ws, float px){ return fly_smoothstep(5.0, 13.0, ws*px); }\n" \
    /* `mat` is (vegetation, f0, blinn exponent), carried through exactly the
     * mixes the colour is — see terrain_surface in fly_render.c */ \
    "vec3 fly_terrain_surface(vec2 p, float h, float slope, float px, out vec3 mat){\n" \
    "  const vec3 m_grass = vec3(1.0, 0.020, 14.0);\n" \
    "  const vec3 m_dirt  = vec3(0.0, 0.030, 18.0);\n" \
    "  const vec3 m_rock  = vec3(0.0, 0.035, 26.0);\n" \
    "  const vec3 m_snow  = vec3(0.0, 0.055, 46.0);\n" \
    "  const vec3 m_sand  = vec3(0.0, 0.030, 18.0);\n" \
    "  vec3 m = m_grass;\n" \
    /* where the water stands here, taken once — twin of terrain_surface's own
       `wz`, and what the shore band, the wet band and the treeline are all
       measured against */ \
    "  float wz = fly_shore(p);\n" \
    "  uint s = uint(uSeed);\n" \
    /* every octave fades toward its own mean once it stops being resolvable;
     * point-sampling it past that is a bias, not a blur — terrain_surface */ \
    "  float wf = fly_detail_amount(47.0, px), wm = fly_detail_amount(12.0, px);\n" \
    "  float wd = fly_detail_amount(90.0, px);\n" \
    "  float mottle = 0.5 + 0.5*fly_noise2(s+31u, p.x/380.0, p.y/380.0);\n" \
    /* a faded octave is a skipped one: the fade reaches exactly zero */ \
    "  float fine = 0.5, micro = 0.5;\n" \
    "  if (wf > 0.0) fine  += 0.5*wf*fly_noise2(s+32u, p.x/47.0, p.y/47.0);\n" \
    "  if (wm > 0.0) micro += 0.5*wm*fly_noise2(s+36u, p.x/12.0, p.y/12.0);\n" \
    "  float region = fly_grass_region(s, p);\n" \
    "  vec3 lush    = mix(vec3(0.10,0.26,0.12), vec3(0.24,0.30,0.12), mottle);\n" \
    "  vec3 dry     = mix(vec3(0.30,0.26,0.10), vec3(0.38,0.31,0.13), mottle);\n" \
    "  vec3 heather = mix(vec3(0.14,0.17,0.12), vec3(0.22,0.16,0.20), mottle);\n" \
    "  vec3 grass = lush;\n" \
    "  grass = mix(grass, dry,     fly_smoothstep(0.12, 0.5, region));\n" \
    "  grass = mix(grass, heather, fly_smoothstep(-0.18, -0.55, region));\n" \
    "  grass *= (0.85 + 0.3*fine) * (0.9 + 0.2*micro);\n" \
    /* the grain: the variance the faded octaves lost, put back at a wavelength
       tied to the projection so it is resolvable at any range — see
       terrain_surface in fly_render.c for the amplitude bookkeeping */ \
    "  float lost = 0.0225*(1.0 - wf*wf) + 0.01*(1.0 - wm*wm);\n" \
    "  if (lost > 1e-5 && px > 1e-6) {\n" \
    "    float lam = min(14.0/px, 60.0);\n" \
    "    grass *= 1.0 + sqrt(lost)*fly_noise2(s + 37u, p.x/lam, p.y/lam)*0.30;\n" \
    "  }\n" \
    /* The worked ground, and it is taken here for the reason terrain_surface
       takes it here: the two terms below give way to it. Nothing else in this
       function has straight edges, which is the whole of why enclosure reads
       as inhabited country from any altitude. */ \
    "  vec3 ti = fly_tilth(s, p, h);\n" \
    "  float dirt = mix(0.0622, fly_smoothstep(0.55, 0.8,\n" \
"                   fly_noise2(s+35u, p.x/90.0, p.y/90.0)), wd);\n" \
    /* the worn-earth ribbons and their gritty shoulders: ridged noise, whose
       level sets are lines rather than islands, so what it lays down is a
       track network rather than another field of blobs — see terrain_surface
       in fly_render.c for the mean it fades toward and why */ \
    "  float wpath = fly_detail_amount(340.0, px);\n" \
    "  float path = mix(0.0562, fly_smoothstep(0.93, 1.0,\n" \
    "        1.0 - abs(fly_noise2(s+41u, p.x/340.0, p.y/340.0))), wpath);\n" \
    /* a track goes round a field, not through it */ \
    "  path *= 1.0 - ti.x*0.92;\n" \
    "  dirt *= 1.0 - ti.x*0.75;\n" \
    "  float bare = dirt*0.7 + path*(1.0 - dirt*0.7);\n" \
    "  vec3 soil = mix(vec3(0.30,0.24,0.16), vec3(0.37,0.34,0.29), path*0.55);\n" \
    "  float grit = fly_detail_amount(2.4, px);\n" \
    "  soil *= 1.0 + 0.26*grit*bare*fly_noise2(s+43u, p.x/2.4, p.y/2.4);\n" \
    "  grass = mix(grass, soil, bare);\n" \
    "  m = mix(m, m_dirt, bare);\n" \
    /* The crop and the hedge round it — twin of the enclosure block in
       terrain_surface. The parcel colour fades toward the parish mean on
       projected size the way every octave here does; what it never does is
       fade out, because a farmed country is still a farmed country at twenty
       kilometres. The hedge is a line, fades on its own much smaller size, and
       is green where the grass is lush and stone where it is not. */ \
    "  if (ti.x > 0.0) {\n" \
    "    float wc = fly_detail_amount(" FLY_GLSL_PARCEL_MIN ", px);\n" \
    "    float wh = fly_detail_amount(" FLY_GLSL_HEDGE_W "*2.0, px);\n" \
    "    float tint = 0.86 + 0.28*fract(ti.y*7.0);\n" \
    "    vec3 cc = ti.y < 0.28 ? vec3(0.23,0.16,0.10)\n" \
    "            : ti.y < 0.50 ? vec3(0.45,0.38,0.15)\n" \
    "            : ti.y < 0.74 ? vec3(0.14,0.28,0.09) : vec3(0.36,0.33,0.18);\n" \
    "    float cv = ti.y < 0.28 ? 0.0 : ti.y < 0.50 ? 0.55 : ti.y < 0.74 ? 1.0 : 0.35;\n" \
    "    vec3 crop = mix(" FLY_GLSL_CROP_MEAN ", cc*tint, wc);\n" \
    "    float veg = mix(" FLY_GLSL_CROP_MEAN_VEG ", cv, wc);\n" \
    "    float hedge = mix(" FLY_GLSL_HEDGE_MEAN ",\n" \
    "                      1.0 - fly_smoothstep(1.1, " FLY_GLSL_HEDGE_W ", ti.z), wh);\n" \
    "    grass = mix(grass, crop, ti.x*0.90);\n" \
    "    m = mix(m, vec3(veg, 0.024, 16.0), ti.x*0.90);\n" \
    "    vec3 hcol = mix(vec3(0.085,0.135,0.062), vec3(0.31,0.30,0.27),\n" \
    "                    fly_smoothstep(0.10, 0.48, region));\n" \
    "    float hm = ti.x*hedge*0.85;\n" \
    "    grass = mix(grass, hcol, hm);\n" \
    "    m = mix(m, vec3(0.75, 0.026, 20.0), hm);\n" \
    "  }\n" \
    /* Needle litter and canopy shade. A wood whose floor is the same green as
       the meadow beside it reads as trees standing on a lawn, and the floor is
       most of what you see of a forest from the air. The settlement clearing
       term is left out on both sides — a shader cannot walk the location list,
       and a town's ground is decked over anyway — so this is the mask, not the
       full field. */ \
    "  float fmask = fly_forest_mask(s, p);\n" \
    "  fmask *= fly_smoothstep(wz + 1.0, wz + 8.0, h);\n" \
    "  fmask *= 1.0 - fly_smoothstep(640.0, 990.0, h);\n" \
    /* and the wood gives way to the plough, on the ramp forest_open uses */ \
    "  fmask *= 1.0 - fly_smoothstep(0.10, 0.45, ti.x);\n" \
    "  grass = mix(grass, vec3(0.075,0.098,0.055), fmask*0.88);\n" \
    "  m = mix(m, m_dirt, fmask*0.35);\n" \
    "  vec3 rock = mix(vec3(0.26,0.22,0.19), vec3(0.36,0.33,0.30), fine);\n" \
    "  float rockness = fly_smoothstep(0.25, 0.55, slope);\n" \
    "  float strata = 0.5 + 0.5*sin(h*0.05 + fly_noise2(s+33u, p.x/850.0, p.y/850.0)*2.4);\n" \
    "  rock *= 0.82 + 0.18*strata;\n" \
    "  vec3 c = mix(grass, rock, rockness);\n" \
    "  m = mix(m, m_rock, rockness);\n" \
    "  float high = fly_smoothstep(900.0, 1600.0, h)*0.7;\n" \
    "  c = mix(c, rock, high);\n" \
    "  m = mix(m, m_rock, high);\n" \
    "  float snow = fly_smoothstep(1850.0, 2250.0, h) * (1.0 - fly_smoothstep(0.45, 0.75, slope));\n" \
    "  c = mix(c, vec3(0.86,0.89,0.94), snow);\n" \
    "  m = mix(m, m_snow, snow);\n" \
    "  if (h < wz + 1.6) {\n" \
    "    float ts = fly_smoothstep(wz + 0.2, wz + 1.6, h);\n" \
    "    c = mix(" FLY_GLSL_WATER_BED ", c, ts);\n" \
    "    m = mix(m_sand, m, ts);\n" \
    "  }\n" \
    "  float wet = fly_smoothstep(wz + 2.6, wz + 0.3, h);\n" \
    "  c *= 1.0 - 0.35*wet;\n" \
    "  m = mix(m, vec3(m.x, 0.10, 200.0), wet);\n" \
    /* rain on the ground: the film darkens the albedo and replaces a rough
       surface with a smooth dielectric one, and what does not run off stands
       in puddles on ground flat enough to hold it — twin of the rain block in
       terrain_surface, which carries the reasoning */ \
    "  if (uWet > 0.002 && h > " FLY_GLSL_WATER_Z " + 0.6) {\n" \
    "    float damp = uWet*(1.0 - fly_smoothstep(1850.0, 2250.0, h));\n" \
    "    c *= 1.0 - 0.34*damp*(1.0 - 0.55*m.x);\n" \
    "    m.y = mix(m.y, 0.021, damp*0.85);\n" \
    "    m.z = mix(m.z, mix(85.0, 20.0, m.x), damp*0.85);\n" \
    "    float pool = damp*(1.0 - fly_smoothstep(0.05, 0.17, slope));\n" \
    "    if (pool > 0.002) {\n" \
    "      float wpool = fly_detail_amount(11.0, px);\n" \
    "      pool *= mix(0.1345, fly_smoothstep(0.34, 0.70,\n" \
    "                  fly_noise2(s+44u, p.x/11.0, p.y/11.0)), wpool);\n" \
    "      c = mix(c, vec3(0.030,0.036,0.040), pool*0.88);\n" \
    "      m = mix(m, vec3(m.x*0.2, 0.021, 240.0), pool*0.9);\n" \
    "    }\n" \
    "  }\n" \
    /* and the crossover from the chart's albedo to the ball's, keyed on the
       camera's altitude — see chart_gives_way and terrain_surface */ \
    "  if (uBall > 0.002)\n" \
    "    c = mix(c, fly_planet_albedo(fly_ball_dir(p), h), uBall);\n" \
    "  mat = m;\n" \
    "  return c;\n" \
    "}\n" \
    /* tussock relief and blade-scale colour — terrain_detail in fly_render.c.
     * `px_per_m` is render-target pixels per world metre at this point; each
     * scale fades on how many pixels it covers, never on distance, and the
     * relief goes away entirely on a surface the material calls level. */ \
    "float fly_detail_relief(uint s, vec2 p, float a0, float a1){\n" \
    "  return fly_noise2(s+37u, p.x/7.4, p.y/7.4)*a0 + fly_noise2(s+39u, p.x/2.3, p.y/2.3)*a1;\n" \
    "}\n" \
    "vec3 fly_terrain_detail(vec2 p, vec3 mat, float px_per_m, inout vec3 n){\n" \
    "  const float HUMMOCK = 7.4, TUSSOCK = 2.3, BLADE = 0.55, E = 0.7;\n" \
    "  uint s = uint(uSeed);\n" \
    "  float veg = mat.x;\n" \
    "  float level = fly_smoothstep(90.0, 220.0, mat.z);\n" \
    "  float rough = (1.0 + 0.5*(1.0 - veg))*(1.0 - level);\n" \
    "  float a0 = fly_detail_amount(HUMMOCK, px_per_m)*0.62*rough;\n" \
    "  float a1 = fly_detail_amount(TUSSOCK, px_per_m)*0.26*rough;\n" \
    "  float fine = fly_detail_amount(BLADE, px_per_m)*veg;\n" \
    "  vec3 tint = vec3(1.0);\n" \
    "  if (a0 + a1 > 0.002) {\n" \
    "    float c0 = fly_detail_relief(s, p, a0, a1);\n" \
    "    float cx = fly_detail_relief(s, p + vec2(E, 0.0), a0, a1);\n" \
    "    float cy = fly_detail_relief(s, p + vec2(0.0, E), a0, a1);\n" \
    "    n = normalize(vec3(n.x - (cx-c0)/E, n.y - (cy-c0)/E, n.z));\n" \
    "    tint *= 1.0 + c0*0.20;\n" \
    "  }\n" \
    "  if (fine > 0.002) {\n" \
    "    float b = fly_noise2(s+38u, p.x/BLADE, p.y/BLADE)*fine;\n" \
    "    tint *= vec3(1.0 + b*0.26, 1.0 + b*0.17, 1.0 - b*0.05);\n" \
    "  }\n" \
    "  return tint;\n" \
    "}\n" \
    /* twin of cloud_fbm_lod: four octaves, blending toward the two-octave
       answer caught halfway through the same walk. See fly_render.c. */ \
    "float fly_cloud_fbm(uint s, float u, float v, float coarse){\n" \
    "  if (coarse <= 0.01) return fly_fbm2(s, u, v, 4);\n" \
    "  float n0 = fly_noise2(s, u, v);\n" \
    "  float n1 = fly_noise2(s + 0x9E37u, u*2.0, v*2.0);\n" \
    "  float n2 = fly_noise2(s + 0x9E37u*2u, u*4.0, v*4.0);\n" \
    "  float n3 = fly_noise2(s + 0x9E37u*3u, u*8.0, v*8.0);\n" \
    "  float a2 = 0.5*n0 + 0.25*n1;\n" \
    "  float a4 = a2 + 0.125*n2 + 0.0625*n3;\n" \
    "  return mix(a4/0.9375, a2/0.75, coarse);\n" \
    "}\n" \
    /* twin of cloud_cover_lod: four octaves near, two once a tap slides
       further per pixel than the fine ones, the deck's mean only in the last
       degree above level. See fly_render.c. */ \
    "float fly_cloud_lod(vec2 p, float coarse, float toMean){\n" \
    "  float t = uTime*0.004;\n" \
    "  float u = p.x/5200.0 + uWind.x*t*0.02 + t*0.4;\n" \
    "  float v = p.y/5200.0 + uWind.y*t*0.02;\n" \
    "  float lo = 0.62 - uStorm*0.34;\n" \
    "  float d = fly_cloud_fbm(uint(uSeed)+41u, u, v, coarse)*0.5 + 0.5;\n" \
    "  float c = fly_smoothstep(lo, lo+0.24, d);\n" \
    "  return toMean > 0.001 ? mix(c, uCloudMean, toMean) : c;\n" \
    "}\n" \
    "float fly_cloud_cover(vec2 p){ return fly_cloud_lod(p, 0.0, 0.0); }\n" \
    /* twin of cirrus_cover_lod: the ice, in a frame turned to the world's own
       prevailing bearing, four times longer along it than across */ \
    "float fly_cirrus_lod(vec2 p, float coarse, float toMean){\n" \
    "  float t = uTime*0.004;\n" \
    "  float al = dot(p, uCirrusDir)/" FLY_GLSL_CIRRUS_LEN " + t*0.13;\n" \
    "  float ac = (p.y*uCirrusDir.x - p.x*uCirrusDir.y)/" FLY_GLSL_CIRRUS_WID ";\n" \
    "  float d = fly_cloud_fbm(uint(uSeed)+63u, al, ac, coarse)*0.5 + 0.5;\n" \
    "  float c = fly_smoothstep(0.60, 0.86, d);\n" \
    "  return toMean > 0.001 ? mix(c, uCirrusMean, toMean) : c;\n" \
    "}\n" \
    /* twin of cloud_shadow: both layers, one plane crossing and one lookup
       each. The ice shades the ground the deck left clear rather than
       multiplying with it — see FLY_CIRRUS_SHADE in fly_render.c. */ \
    "float fly_cloud_shadow(vec3 p){\n" \
    "  if (uSun.z <= 0.02) return 1.0;\n" \
    "  float cc = 0.0;\n" \
    "  if (p.z < " FLY_GLSL_CLOUD_Z ")\n" \
    "    cc = fly_cloud_cover(p.xy + uSun.xy*((" FLY_GLSL_CLOUD_Z " - p.z)/uSun.z));\n" \
    "  float lost = cc*" FLY_GLSL_CLOUD_SHADE ";\n" \
    "  if (p.z < " FLY_GLSL_CIRRUS_Z ")\n" \
    "    lost += fly_cirrus_lod(p.xy + uSun.xy*((" FLY_GLSL_CIRRUS_Z " - p.z)/uSun.z),\n" \
    "                           0.0, 0.0)*" FLY_GLSL_CIRRUS_SHADE "*(1.0 - cc);\n" \
    "  return 1.0 - lost;\n" \
    "}\n" \
    /* --- the atmosphere: twin of fly__scatter in fly_render.c ---
       One single-scattering model behind the sky, the sun's colour and aerial
       perspective. Rayleigh is strongly wavelength-dependent and Mie is grey
       and forward-biased; both are exponential in altitude, so both integrate
       in closed form along a straight ray. FLY_SCAT_DENSITY thickens the whole
       atmosphere uniformly, which is the one liberty taken — this world is
       twenty kilometres across and real sea-level air barely touches a ridge
       three kilometres off. */ \
/* Twin of fly__planet_hit: where a ray meets the ball, and the sphere's own
   normal there, or a negative distance if it is above the horizon. */ \
    "float fly_planet_hit(vec3 ro, vec3 rd, out vec3 n){\n" \
    "  float r0 = " FLY_GLSL_PLANET_R " + ro.z;\n" \
    "  float b = r0*rd.z;\n" \
    "  float disc = " FLY_GLSL_PLANET_R "*" FLY_GLSL_PLANET_R "\n" \
    "             - r0*r0*(1.0 - rd.z*rd.z);\n" \
    "  n = vec3(0.0, 0.0, 1.0);\n" \
    "  if (disc <= 0.0) return -1.0;\n" \
    "  float t = -b - sqrt(disc);\n" \
    "  if (t <= 0.0) return -1.0;\n" \
    "  vec3 hp = ro + rd*t;\n" \
    "  n = normalize(vec3(hp.x - ro.x, hp.y - ro.y, hp.z - (ro.z - r0)));\n" \
    "  return t;\n" \
    "}\n" \
/* Twins of fly__erfcx, scat_chapman and scat_thick: the air along a ray,
   integrated on the ball rather than on a slope. exp(z*z)*erfc(z) to a relative
   1.1e-7 — the scaled form because both of its factors leave float range long
   before their product does — then the Chapman column out to space, then the
   segment as a difference of two of those. See fly_render.c for why the case
   split is not cosmetic and why the flat closed form could not be lengthened. */ \
    "float fly_erfcx(float z){\n" \
    "  float t = 1.0/(1.0 + 0.5*z);\n" \
    "  return t*exp(-1.26551223 + t*(1.00002368 + t*(0.37409196 + t*(0.09678418\n" \
    "       + t*(-0.18628806 + t*(0.27886807 + t*(-1.13520398 + t*(1.48851587\n" \
    "       + t*(-0.82215223 + t*0.17087277)))))))));\n" \
    "}\n" \
    "float fly_chapman(float H, float r, float c){\n" \
    /* no air below the surface: clamping the radius is what bounds the whole
       thing at the two grazing halves, sqrt(2*pi*H*R) */ \
    "  r = max(r, " FLY_GLSL_PLANET_R ");\n" \
    "  float X = r/H;\n" \
    "  float s = max(sqrt(clamp(1.0 - c*c, 0.0, 1.0)), 1e-5);\n" \
    "  float rho = exp(-(r - " FLY_GLSL_PLANET_R ")/H);\n" \
    "  return H*rho*sqrt(0.5*3.14159265*X)/s*fly_erfcx(c*sqrt(0.5*X)/s);\n" \
    "}\n" \
    "float fly_scat_thick(float H, float z0, float dz, float t){\n" \
    "  if (t <= 0.0) return 0.0;\n" \
    /* no ceiling on the altitude: one would tip a ray that clears the air into
       one whose perigee is buried, and the density under the surface overflows
       the column. See scat_thick. */ \
    "  float r0 = " FLY_GLSL_PLANET_R " + max(z0, 0.0);\n" \
    "  float c0 = clamp(dz, -1.0, 1.0);\n" \
    "  float r1 = sqrt(r0*r0 + 2.0*r0*c0*t + t*t);\n" \
    "  float c1 = (r0*c0 + t)/r1;\n" \
    "  float v;\n" \
    "  if (c0 >= 0.0) v = fly_chapman(H, r0, c0) - fly_chapman(H, r1, c1);\n" \
    "  else if (c1 <= 0.0) v = fly_chapman(H, r1, -c1) - fly_chapman(H, r0, -c0);\n" \
    "  else {\n" \
    "    float rp = r0*sqrt(clamp(1.0 - c0*c0, 0.0, 1.0));\n" \
    "    v = 2.0*fly_chapman(H, rp, 0.0) - fly_chapman(H, r0, -c0)\n" \
    "      - fly_chapman(H, r1, c1);\n" \
    "  }\n" \
    "  return max(v, 0.0);\n" \
    "}\n" \
    "void fly_scatter(vec3 ro, vec3 rd, float t, out vec3 transmit, out vec3 inscat){\n" \
    "  vec3 bR = vec3(5.802e-6, 13.558e-6, 33.100e-6);\n" \
    "  float haze = 1.0 + uStorm*7.0 + uDusk*1.2;\n" \
    "  float bM = 3.996e-6*haze;\n" \
    "  float bMe = bM*1.11;\n" \
    /* the compression is horizontal, so it is weighted by how horizontal the
       ray is: three for a level path, one for a vertical one — see fly__scatter */ \
    "  float spread = " FLY_GLSL_SCAT_SPREAD "\n" \
    "               + (1.0 - " FLY_GLSL_SCAT_SPREAD ")*clamp(abs(rd.z), 0.0, 1.0);\n" \
    "  vec3 pn; float pt = fly_planet_hit(ro, rd, pn);\n" \
    "  if (pt > 0.0 && t > pt) t = pt;\n" \
    "  float tr = fly_scat_thick(8000.0, ro.z, rd.z, t)*spread;\n" \
    "  float tm = fly_scat_thick(1200.0, ro.z, rd.z, t)*spread;\n" \
    "  vec3 tau = bR*tr + vec3(bMe*tm);\n" \
    "  transmit = exp(-tau);\n" \
    "  float mu = dot(rd, uSun);\n" \
    "  float pr = 0.0596831*(1.0 + mu*mu);\n" \
    "  float g2 = " FLY_GLSL_SCAT_G "*" FLY_GLSL_SCAT_G ";\n" \
    "  float dn = 1.0 + g2 - 2.0*" FLY_GLSL_SCAT_G "*mu;\n" \
    "  float pm = 0.0795775*(1.0 - g2)/(dn*sqrt(dn) + 1e-4);\n" \
    /* the midpoint's altitude is the sphere's, not the slope's — a ray half a
       degree below level never gets below its perigee, where the slope has it
       a kilometre underground — and it is taken along the first FLY_SCAT_FAR
       of the ray, since past that a longer ray adds no air and would only push
       the sample somewhere emptier. The old 0.035 elevation floor is gone with
       the flat model that needed it. See fly__scatter. */ \
    "  float zmt = min(t, 320000.0)*0.5;\n" \
    "  float rm = " FLY_GLSL_PLANET_R " + max(ro.z, 0.0);\n" \
    "  float zm = clamp(sqrt(rm*rm + 2.0*rm*rd.z*zmt + zmt*zmt)\n" \
    "                 - " FLY_GLSL_PLANET_R ", 0.0, 40000.0);\n" \
    /* how high the sun is where the air is: dot(sun, up) at the segment's
       lowest point, which is its perigee when it has one. One direction for the
       whole chart is right, one elevation is not — that is the terminator. See
       fly__scatter. */ \
    "  float tp = -rm*rd.z;\n" \
    "  float ts = clamp(tp, 0.0, t);\n" \
    "  vec3 sp = vec3(rd.x*ts, rd.y*ts, rm + rd.z*ts);\n" \
    "  float rl = length(sp);\n" \
    "  float sun_up = rl > 1.0 ? dot(uSun, sp)/rl : uSun.z;\n" \
    "  float sz = max(sun_up, -0.14);\n" \
    "  float ssp = " FLY_GLSL_SCAT_SPREAD " + (1.0 - " FLY_GLSL_SCAT_SPREAD ")*abs(sz);\n" \
    "  float sr = fly_scat_thick(8000.0, zm, sz, 1e6)*ssp;\n" \
    "  float sm = fly_scat_thick(1200.0, zm, sz, 1e6)*ssp;\n" \
    "  vec3 Ts = exp(-(bR*sr + vec3(bMe*sm)));\n" \
    /* per channel: weighting all three by the mean optical depth leaves a
       saturated path still blue, which puts the noon horizon bluer than the
       zenith — see fly__scatter */ \
    "  vec3 avg = mix(vec3(1.0), (1.0 - transmit)/max(tau, vec3(1e-5)),\n" \
    "                 step(vec3(1e-5), tau));\n" \
    "  float taua = (tau.x + tau.y + tau.z)*(1.0/3.0);\n" \
    "  float night = fly_smoothstep(-0.14, 0.05, sun_up);\n" \
    "  vec3 sc = bR*(tr*pr) + vec3(bM*tm*pm);\n" \
    /* shafts as shadowed fog: air scatters only the sunlight that reaches it,
       so the deck's gaps light the beam and its bodies do not. One tap answers
       that for a short segment and nothing at all for a long one — a sky ray's
       midpoint is 160 km out, where the tap slid most of a 5.2 km cloud cell
       between neighbouring pixels and hung hard grey slabs along the horizon.
       So it fades into the deck's mean transmission over the deck's own base
       wavelength: near field untouched, sky rays land on a constant. See
       fly__scatter. */ \
    "  float shaft = fly_cloud_shadow(ro + rd*(t*0.5));\n" \
    "  shaft = mix(shaft, 1.0 - uCloudMean*0.62, clamp(t*0.5/5200.0, 0.0, 1.0));\n" \
    /* multiple scattering, one isotropic term weighted by how much of the path
       has scattered at all — see fly__scatter */ \
    "  sc += (bR*tr + vec3(bM*tm))*(0.0795775*" FLY_GLSL_SCAT_MS "*(1.0 - exp(-taua)));\n" \
    "  inscat = sc*Ts*avg*(" FLY_GLSL_SCAT_SUN "*night*shaft);\n" \
    /* Airglow, attenuated by `avg` like everything else along the path — see
     * fly__scatter in fly_render.c for why an unattenuated one put a floor
     * under the planet's night side seen from orbit. The twins have to agree:
     * `gpu.parity` compares them sample for sample. */ \
    "  inscat += vec3(0.010,0.016,0.032)*avg*((1.0 - night)*(1.0 - exp(-taua*1.6)));\n" \
    /* the storm grey belongs to the air, weighted by how saturated the path
       is — otherwise a distant ridge converges to the ungreyed in-scatter and
       comes out brighter than the sky it sits against. See fly__scatter. */ \
    "  inscat = mix(inscat, vec3(0.22,0.23,0.26)*(uDay + 0.05),\n" \
    "               uStorm*" FLY_GLSL_SCAT_STORM "*(1.0 - exp(-taua)));\n" \
    "}\n" \
    "float fly_planet_cloud(vec3 u){\n" \
    "  float lat = abs(u.z);\n" \
    "  float itcz = lat/0.20, dry = (lat - 0.42)/0.16, track = (lat - 0.80)/0.22;\n" \
    "  float d = uTime*4.0e-6;\n" \
    "  float n = fly_fbm3(uint(uSeed) + 62u, u.x*4.0 + d, u.y*4.0, u.z*4.0, 4);\n" \
    "  float cov = 0.5 + 0.5*n + 0.18*exp(-itcz*itcz) - 0.24*exp(-dry*dry)\n" \
    "            + 0.14*exp(-track*track);\n" \
    "  return fly_smoothstep(0.52, 1.02, cov);\n" \
    "}\n" \
    /* The cloud deck as a medium. Twin of cloud_slab in fly_render.c, where
     * the argument for the per-tap LOD fade, the two footprints and the
     * Beer-Lambert alpha is written out. Returns how much of the path to
     * `tmax` the cloud covers and the
     * colour it covers it with, so the caller can mix whatever was behind it
     * toward the deck — a sky ray passes the far clamp, a surface ray passes
     * the surface, and the second is what makes cloud something a mountain can
     * be *inside* rather than a backdrop behind it. */ \
    "float fly_cloud_slab(vec3 ro, vec3 rd, float tmax, out vec3 outc){\n" \
    "  outc = vec3(0.0);\n" \
    "  if (tmax <= 0.0) return 0.0;\n" \
    "  float cb = " FLY_GLSL_CLOUD_Z " - " FLY_GLSL_CLOUD_BASE ";\n"              \
    "  float ctop = " FLY_GLSL_CLOUD_Z " + " FLY_GLSL_CLOUD_RISE ";\n"            \
    "  float depth = ctop - cb;\n"                                                \
    "  float cdz = abs(rd.z) < 1e-4 ? (rd.z < 0.0 ? -1e-4 : 1e-4) : rd.z;\n" \
    "  float t0 = (cb - ro.z)/cdz, t1 = (ctop - ro.z)/cdz;\n" \
    "  if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }\n" \
    "  t0 = max(t0, 0.0);\n" \
    "  t1 = min(t1, tmax);\n" \
    "  if (t1 <= t0) return 0.0;\n" \
    "  float fade = exp(-t0/38000.0);\n" \
    "  if (fade <= 0.002) return 0.0;\n" \
    "  vec3 bright = uCloudLit, dk = uCloudDark;\n" \
    "  float perm = 1.0/(max(abs(rd.z), 0.0015)*(uPxScale + 1e-6));\n" \
    /* what one tap stands for, in metres of ray — see cloud_slab, which
       carries why the alpha is Beer-Lambert over this and why the tap is
       averaged along it rather than point-sampled at its middle */ \
    "  float seg = (t1 - t0)*0.25;\n" \
    "  float acc = 0.0; vec3 ccol = vec3(0.0);\n" \
    "  for (int si = 0; si < 4; ++si) {\n" \
    "    if (acc >= 0.995) break;\n" \
    "    float t = mix(t0, t1, (float(si)+0.5)/4.0);\n" \
    "    vec3 p = ro + rd*t;\n" \
    /* two footprints: across the ray a tap covers what a pixel covers, along
       it the whole segment. The fine octaves are fetched at the pixel rate and
       averaged along the ray below; the collapse to the deck's mean, and the
       self-shadow lookup, take the segment. */ \
    "    float slp = t*perm;\n" \
    "    float sl = max(slp, seg*" FLY_GLSL_CLOUD_SEGW "), s2 = sl*sl;\n" \
    "    float sp2 = slp*slp;\n" \
    "    float coarse = sp2/(sp2 + " FLY_GLSL_CLOUD_FINE ");\n" \
    "    float coarse_s = s2/(s2 + " FLY_GLSL_CLOUD_FINE ");\n" \
    "    float wide = s2/(s2 + " FLY_GLSL_CLOUD_FLAT ");\n" \
    "    vec2 hs = rd.xy*(seg/3.0);\n" \
    "    float cov = (fly_cloud_lod(p.xy - hs, coarse, wide)\n" \
    "               + fly_cloud_lod(p.xy, coarse, wide)\n" \
    "               + fly_cloud_lod(p.xy + hs, coarse, wide))/3.0;\n"           \
    /* the column's own top, off the coverage the tap has already paid for,
       and the profile in units of that depth — see cloud_slab, which carries
       why this is per tap and not per ray */                                  \
    "    float hh = (p.z - cb)/(depth*(" FLY_GLSL_CLOUD_MIN                     \
    " + (1.0 - " FLY_GLSL_CLOUD_MIN ")*cov));\n"                               \
    "    float prof = fly_smoothstep(-0.10, 0.08, hh)*fly_smoothstep(1.0, 0.66, hh);\n" \
    "    float dens = cov*prof;\n"                                             \
    "    if (dens < 0.0005) continue;\n" \
    "    float cs = fly_cloud_lod(p.xy + uSun.xy*620.0, coarse_s, wide);\n" \
    /* and the two faces: the shading a sample carries is scaled by how near
       the layer's own crown it sits — see FLY_CLOUD_FACE */ \
    "    float crown = fly_smoothstep(" FLY_GLSL_CLOUD_FACE_LO ", 1.0,\n" \
    "                                 (p.z - cb)/depth);\n" \
    "    vec3 lit = mix(bright, dk, clamp((cs*0.85 + dens*0.25)\n" \
    "                                     *(1.0 - " FLY_GLSL_CLOUD_FACE "*crown),\n" \
    "                                     0.0, 1.0));\n" \
    "    float a = 1.0 - exp(-dens*seg/" FLY_GLSL_CLOUD_EXT ");\n" \
    "    ccol += lit*(1.0 - acc)*a;\n" \
    "    acc += (1.0 - acc)*a;\n" \
    "  }\n" \
    "  if (acc <= 0.003) return 0.0;\n" \
    "  outc = ccol/acc;\n" \
    "  return acc*fade;\n" \
    "}\n" \
    /* The ice sheet as one surface: twin of cirrus_sheet in fly_render.c,
     * where the argument for a plane rather than a slab is written out. One
     * intersection, one field lookup, Beer-Lambert over the slant path. */ \
    "float fly_cirrus_sheet(vec3 ro, vec3 rd, float tmax, out vec3 outc){\n" \
    "  outc = vec3(0.0);\n" \
    "  float dz = abs(rd.z);\n" \
    "  if (tmax <= 0.0 || dz < 1e-4) return 0.0;\n" \
    "  float t = (" FLY_GLSL_CIRRUS_Z " - ro.z)/rd.z;\n" \
    "  if (t <= 0.0 || t > tmax) return 0.0;\n" \
    "  float fade = exp(-t/38000.0);\n" \
    "  if (fade <= 0.002) return 0.0;\n" \
    "  float sl = t/(dz*(uPxScale + 1e-6)), s2 = sl*sl;\n" \
    "  float coarse = s2/(s2 + " FLY_GLSL_CIRRUS_FINE ");\n" \
    "  float wide = s2/(s2 + " FLY_GLSL_CIRRUS_FLAT ");\n" \
    "  float cov = fly_cirrus_lod(ro.xy + rd.xy*t, coarse, wide);\n" \
    "  if (cov <= 0.001) return 0.0;\n" \
    "  float slant = min(1.0/dz, " FLY_GLSL_CIRRUS_SLANT ");\n" \
    "  outc = uCirrusLit;\n" \
    "  return (1.0 - exp(-cov*" FLY_GLSL_CIRRUS_TAU "*slant))*fade;\n" \
    "}\n" \
    "vec3 fly_sky_at(vec3 ro, vec3 rd, float stars){\n" \
    "  vec3 vT, c, pn;\n" \
    "  float phit = fly_planet_hit(ro, rd, pn);\n" \
    "  fly_scatter(ro, rd, 3000000.0, vT, c);\n" \
    "  if (phit > 0.0) {\n" \
    "    vec3 hp = ro + rd*phit;\n" \
    "    vec3 u = fly_ball_dir(hp.xy);\n" \
    "    float gh = fly_ground(hp.xy);\n" \
    "    float far = 1.0 - exp(-phit/38000.0);\n" \
    "    vec3 alb = fly_planet_albedo(u, gh);\n" \
    "    if (far > 0.002)\n" \
    "      alb = mix(alb, vec3(0.62, 0.64, 0.67), fly_planet_cloud(u)*far);\n" \
    /* skylight belongs to the day side: the sky over a point is lit by the sun
       that point can see, and uSH is the sky over the camera — see sky_sample */ \
    "    float lam = dot(pn, uSun);\n" \
    "    vec3 lit = uSunLight*max(lam, 0.0)\n" \
    "             + fly_sky_irradiance(pn)*(0.5*fly_smoothstep(-0.10, 0.15, lam));\n" \
    "    c += alb*lit*vT;\n" \
    "  }\n" \
    "  float s = dot(rd, uSun);\n" \
    "  if (uSun.z > -0.08) {\n" \
    /* the disc is the sun seen through the same air the sky is made of, so it
       reddens on exactly the schedule the horizon does */ \
    "    vec3 sunc = vT*" FLY_GLSL_SCAT_SUN ";\n" \
    "    if (s > 0.9996) c += sunc*0.30;\n" \
    "    if (s > 0.0) c += sunc*(pow(s,180.0)*0.16 + pow(s,10.0)*0.02);\n" \
    "  }\n" \
    /* twin of the star field in sky_sample: both axes in radians, the star
       jittered inside its cell, four cells tested so none is clipped */ \
    "  if (stars > 0.0 && uSun.z < 0.06 && rd.z > 0.0) {\n" \
    "    float ni = fly_smoothstep(0.06, -0.12, uSun.z);\n" \
    "    float u = atan(rd.y, rd.x)*150.0;\n" \
    "    float v2 = asin(clamp(rd.z, -1.0, 1.0))*150.0;\n" \
    "    int iu = int(floor(u)), iv = int(floor(v2));\n" \
    "    for (int dv = 0; dv <= 1; ++dv)\n" \
    "    for (int du = 0; du <= 1; ++du) {\n" \
    "      uint hsh = fly_hash2(uint(uSeed)+7u, iu + du, iv + dv);\n" \
    "      if ((hsh & 31u) != 0u) continue;\n" \
    "      float jx = float((hsh >> 16) & 255u)/255.0;\n" \
    "      float jy = float((hsh >> 24) & 255u)/255.0;\n" \
    "      float dx = u - (float(iu + du) + jx);\n" \
    "      float dy = v2 - (float(iv + dv) + jy);\n" \
    "      float fall = clamp(1.0 - (dx*dx + dy*dy)*11.0, 0.0, 1.0);\n" \
    "      if (fall <= 0.0) continue;\n" \
    "      float mag = 0.35 + float((hsh >> 8) & 255u)/255.0;\n" \
    "      float warm = float((hsh >> 4) & 15u)/15.0;\n" \
    "      c += vec3(0.72 + 0.34*warm, 0.86 + 0.10*warm, 1.05 - 0.30*warm)*(fall*fall*mag*ni);\n" \
    "    }\n" \
    "  }\n" \
    /* both layers, with nothing in front of them: the whole of each out to the
       clamp is on the path, and the farther one — below seven kilometres, the
       ice — goes down first. Twin of sky_sample. */ \
    "  {\n" \
    "    vec3 ccol, icol;\n" \
    "    float ca = fly_cloud_slab(ro, rd, 400000.0, ccol);\n" \
    "    float ia = fly_cirrus_sheet(ro, rd, 400000.0, icol);\n" \
    "    if (ro.z <= " FLY_GLSL_CIRRUS_Z ") {\n" \
    "      if (ia > 0.0) c = mix(c, icol, ia);\n" \
    "      if (ca > 0.0) c = mix(c, ccol, ca);\n" \
    "    } else {\n" \
    "      if (ca > 0.0) c = mix(c, ccol, ca);\n" \
    "      if (ia > 0.0) c = mix(c, icol, ia);\n" \
    "    }\n" \
    "  }\n" \
    /* the storm grey is in fly_scatter now, not here — see sky_sample */ \
    "  return c;\n" \
    "}\n" \
    /* what a ray looking at the sky sees, and the sky as a light or fog source
     * without the star field — see sky_sample in fly_render.c */ \
    "vec3 fly_sky(vec3 ro, vec3 rd){ return fly_sky_at(ro, rd, 1.0); }\n" \
    "vec3 fly_sky_amb(vec3 ro, vec3 rd){ return fly_sky_at(ro, rd, 0.0); }\n" \
    /* Aerial perspective is the same integral, stopped at the surface: what
       survives of the colour, plus what the air in front of it adds. The fog
       this replaced faded toward a full sky lookup — cloud march and all —
       evaluated per vertex, and did it with one scalar for all three channels,
       so a far ridge went pale instead of blue. */ \
    "vec3 fly_apply_fog(vec3 col, vec3 ro, vec3 dirn, float dist){\n" \
    "  vec3 T, S, ccol;\n" \
    "  vec3 d = normalize(dirn);\n" \
    "  fly_scatter(ro, d, dist, T, S);\n" \
    /* The deck is part of what is in front of the surface, and used to not be:
       the slab was marched for sky rays and for nothing else, so with the
       camera inside the layer a mountain top *in* cloud came out crisp against
       a grey wash. Stopping the march at the surface is what makes it a medium
       in front of the ground rather than a backdrop behind it. Free where
       there is no cloud on the segment, which is most rays most of the time. */ \
    "  float ca = fly_cloud_slab(ro, d, dist, ccol);\n" \
    "  vec3 icol;\n" \
    "  float ia = fly_cirrus_sheet(ro, d, dist, icol);\n" \
    /* farther layer first, so the nearer one attenuates it — twin of fog_pair */ \
    "  bool iceFar = ro.z <= " FLY_GLSL_CIRRUS_Z ";\n" \
    "  if (iceFar && ia > 0.0) { T *= 1.0 - ia; S = S*(1.0 - ia) + icol*ia; }\n" \
    "  if (ca > 0.0) { T *= 1.0 - ca; S = S*(1.0 - ca) + ccol*ca; }\n" \
    "  if (!iceFar && ia > 0.0) { T *= 1.0 - ia; S = S*(1.0 - ia) + icol*ia; }\n" \
    "  return col*T + S;\n" \
    "}\n" \
    /* The water surface: exact port of water_surface in fly_render.c, and the
     * one water model in the program. The raster pass hands it a sky sample for
     * `sky`, the tracer hands it a traced reflection; everything else — waves,
     * Fresnel, glint, surf — is shared, so a coast is the same coast whichever
     * pipeline drew it. Returns the colour before fog. */ \
    "vec3 fly_water_surface(vec3 eye, vec2 wxy, float wz, float depth, vec3 sky, float cloud){\n" \
    "  uint s = uint(uSeed);\n" \
    "  float t = uTime;\n" \
    "  vec3 p = vec3(wxy, wz);\n" \
    "  float wspd = length(uWind.xy);\n" \
    "  float chopamp = clamp(wspd/16.0, 0.15, 1.0);\n" \
    "  vec3 wd = wspd > 0.5 ? normalize(vec3(uWind.xy, 0.0)) : vec3(1.0, 0.0, 0.0);\n" \
    "  vec3 view = normalize(eye - p);\n" \
    "  float px = uPxScale/max(distance(p, eye), 1e-3);\n" \
    "  float s1 = fly_detail_amount(42.0, px), s2 = fly_detail_amount(14.0, px);\n" \
    "  float s3 = fly_detail_amount(7.7, px), sf = fly_detail_amount(8.5, px);\n" \
    /* two near-field octaves: a 1.6 m ripple and a 0.5 m one, skipped outright
       once a pixel cannot hold them — see water_surface in fly_render.c for
       why nothing finer than six metres made close water look like glass */ \
    "  float s4 = fly_detail_amount(1.6, px), s5 = fly_detail_amount(0.55, px);\n" \
    "  float a4 = 0.0, a5 = 0.0;\n" \
    "  float c4 = 0.055*(0.4 + 0.6*chopamp), c5 = 0.040*(0.4 + 0.6*chopamp);\n" \
    "  float a1 = fly_noise2(s + 51u, wxy.x/46.0 + t*0.31, wxy.y/39.0 - t*0.22)*s1;\n" \
    "  float a2 = fly_noise2(s + 52u, wxy.x/13.0 - t*0.53, wxy.y/15.0 + t*0.44)*s2;\n" \
    "  float a3 = fly_noise2(s + 53u, (wxy.x*wd.x + wxy.y*wd.y)/6.5 - t*1.3,\n" \
    "                                 (wxy.x*-wd.y + wxy.y*wd.x)/9.0)*s3;\n" \
    "  float c3 = 0.06*chopamp, d3 = 0.04*chopamp;\n" \
    "  if (s4 > 0.0) a4 = fly_noise2(s + 55u, (wxy.x*wd.x + wxy.y*wd.y)/1.6 - t*2.4,\n" \
    "                                         (wxy.x*-wd.y + wxy.y*wd.x)/2.1 + t*0.5)*s4;\n" \
    "  if (s5 > 0.0) a5 = fly_noise2(s + 56u, wxy.x/0.55 + t*3.6, wxy.y/0.62 - t*2.9)*s5;\n" \
    "  float lost = ((0.10*0.10 + 0.07*0.07)*(1.0 - s1*s1) +\n" \
    "                (0.05*0.05 + 0.05*0.05)*(1.0 - s2*s2) +\n" \
    "                (c3*c3 + d3*d3)*(1.0 - s3*s3) +\n" \
    "                (c4*c4*2.0)*(1.0 - s4*s4) +\n" \
    "                (c5*c5*2.0)*(1.0 - s5*s5))*0.5*0.184;\n" \
    "  vec3 n = normalize(vec3(a1*0.10 + a2*0.05 + a3*c3 + a4*c4 + a5*c5,\n" \
    "                          a1*0.07 - a2*0.05 + a3*d3 - a4*c4 + a5*c5, 1.0));\n" \
    "  float fres = 0.04 + 0.96*pow(1.0 - clamp(dot(view, n), 0.0, 1.0), 5.0);\n" \
    /* the bed showing through the shallows; see water_surface in fly_render.c
     * for why its absence turned the whole shelf into a sheet of white sky */ \
    "  float clarity = exp(-clamp(depth, 0.0, 40.0)/1.1);\n" \
    "  vec3 bed = " FLY_GLSL_WATER_BED "*vec3(0.34, 0.62, 0.60)*(0.25 + 0.75*uDay*cloud);\n" \
    "  vec3 deep = mix(vec3(0.02,0.07,0.10), vec3(0.06,0.15,0.14),\n" \
    "                  clamp(1.0 - depth/25.0, 0.0, 1.0));\n" \
    "  vec3 col = mix(mix(deep*(0.2 + uDay), bed, clarity), sky, 0.22 + 0.68*fres);\n" \
    "  vec3 hv = normalize(view + uSun);\n" \
    "  float shine = mix(140.0, 40.0, chopamp);\n" \
    "  shine = shine/(1.0 + shine*lost*8.0);\n" \
    "  col += vec3(1.4,1.2,0.9)*(pow(clamp(dot(n, hv), 0.0, 1.0), shine)\n" \
    "                            *uDay*(1.0 - uStorm)*cloud);\n" \
    "  float edge = fly_smoothstep(0.9, 0.02, depth);\n" \
    "  float run = 0.35 + 0.65*mix(0.5, 0.5 + 0.5*\n" \
    "                fly_noise2(s + 54u, wxy.x/11.0 - t*0.7, wxy.y/10.0), sf);\n" \
    "  float foam = edge*edge*run*0.62;\n" \
    /* whitecaps break on the crests: steepness thresholded, fading toward its
       own mean with range — see water_surface in fly_render.c */ \
    "  { float tilt = length(n.xy);\n" \
    "    float crest = fly_smoothstep(0.055, 0.16, tilt*(0.6 + 0.4*chopamp));\n" \
    "    foam += chopamp*chopamp*0.55*mix(0.16, crest, max(s3, s4)); }\n" \
    "  return mix(col, vec3(0.90,0.93,0.95)*(0.25 + 0.75*uDay), clamp(foam, 0.0, 0.62));\n" \
    "}\n" \
    /* terrain shading without the indirect bounce — stands in for pt_shade's
     * recursive call, which GLSL cannot express */ \
    "vec3 fly_shade_direct(vec3 ro, vec3 rd){\n" \
    "  float t = fly_trace_terrain(ro, rd, 26000.0);\n" \
    /* the ambient sky, not the viewed one: this stands in for a bounce or a
     * reflection, and a single sample of the star field used as a light source
     * multiplies one star into the whole albedo of whatever it lands on — at
     * night that speckled every snow slope with what looked like sky showing
     * through the mountain */ \
    "  if (t < 0.0) return fly_sky_amb(ro, rd);\n" \
    "  vec3 p = ro + rd*t;\n" \
    "  vec3 gn = fly_ground_normal(p.xy, 14.0);\n" \
    "  vec3 n = gn, mat;\n" \
    "  vec3 alb = fly_terrain_surface(p.xy, p.z, 1.0 - gn.z, uPxScale/t, mat);\n" \
    "  alb *= fly_terrain_detail(p.xy, mat, uPxScale/t, n);\n" \
    "  vec3 col = alb*(0.04 + 0.05*uDay);\n" \
    "  float ndl = dot(gn, uSun);\n" \
    "  if (ndl > 0.0 && uSun.z > 0.0 && fly_trace_terrain(p + gn*3.0, uSun, 9000.0) < 0.0)\n" \
    "    col += fly_terrain_light(alb, mat, n, -rd, fly_cloud_shadow(p), 1.0)\n" \
    "         - fly_terrain_light(alb, mat, n, -rd, 0.0, 1.0);\n" \
    "  return fly_apply_fog(col, ro, rd, t);\n" \
    "}\n" \
    "vec3 fly_pt_shade(vec3 ro, vec3 rd, vec3 jitter_sun, vec3 bounce_dir){\n" \
    "  float t = fly_trace_terrain(ro, rd, 26000.0);\n" \
    /* the water plane is intersected in its own right, not only when the
     * terrain march happens to land under it — see pt_shade in fly_render.c */ \
    "  {\n" \
    "    float tw = rd.z < -1e-4 ? (" FLY_GLSL_WATER_Z " - ro.z)/rd.z : -1.0;\n" \
    /* a lake is the same intersection at its own level — twin of the lake loop
     * in pt_shade */ \
    "    for (int li = 0; li < uLakeCount; ++li) {\n" \
    "      float tl = rd.z < -1e-4 ? (uLake[li].w - ro.z)/rd.z : -1.0;\n" \
    "      if (tl <= 0.0 || tl >= 26000.0) continue;\n" \
    "      if (tw > 0.0 && tl > tw) continue;\n" \
    "      vec2 dl = (ro + rd*tl).xy - uLake[li].xy;\n" \
    "      if (dot(dl, dl) > uLake[li].z*uLake[li].z) continue;\n" \
    "      vec2 lp = (ro + rd*tl).xy;\n" \
    "      float lg = fly_ground(lp);\n" \
    "      if (lg >= fly_water_at(lp, lg)) continue;\n" \
    "      tw = tl;\n" \
    "    }\n" \
    "    if (tw > 0.0 && tw < 26000.0 && (t < 0.0 || tw < t)) {\n" \
    "      vec3 wp = ro + rd*tw;\n" \
    "      float a1 = fly_noise2(uint(uSeed)+51u, wp.x/46.0 + uTime*0.31, wp.y/39.0 - uTime*0.22);\n" \
    "      vec3 n = normalize(vec3(a1*0.08, a1*0.05, 1.0));\n" \
    "      vec3 refl = normalize(rd - n*2.0*dot(rd, n));\n" \
    "      vec3 rc = fly_shade_direct(wp + vec3(0.0,0.0,0.5), refl);\n" \
    "      float wdepth = wp.z - fly_ground(wp.xy);\n" \
    "      vec3 wcol = fly_water_surface(ro, wp.xy, wp.z, wdepth, rc, fly_cloud_shadow(wp));\n" \
    "      return fly_apply_fog(wcol, ro, rd, tw);\n" \
    "    }\n" \
    "  }\n" \
    /* and the rivers, bisected out of the segment already flown — twin of the
     * river block in pt_shade, and the same fourteen halvings */ \
    "  if (t > 0.0) {\n" \
    "    vec3 hp = ro + rd*t;\n" \
    "    if (fly_water(hp.xy) > fly_ground(hp.xy) + 0.02 && ro.z > fly_water(ro.xy)) {\n" \
    "      float lo = 0.0, hi = t;\n" \
    "      for (int it = 0; it < 14; ++it) {\n" \
    "        float mid = (lo + hi)*0.5;\n" \
    "        vec3 q = ro + rd*mid;\n" \
    "        if (q.z > fly_water(q.xy)) lo = mid; else hi = mid;\n" \
    "      }\n" \
    "      vec3 wp = ro + rd*hi;\n" \
    "      wp.z = fly_water(wp.xy);\n" \
    "      float a1 = fly_noise2(uint(uSeed)+51u, wp.x/46.0 + uTime*0.31, wp.y/39.0 - uTime*0.22);\n" \
    "      vec3 n = normalize(vec3(a1*0.08, a1*0.05, 1.0));\n" \
    "      vec3 refl = normalize(rd - n*2.0*dot(rd, n));\n" \
    "      vec3 rc = fly_shade_direct(wp + vec3(0.0,0.0,0.5), refl);\n" \
    "      vec3 wcol = fly_water_surface(ro, wp.xy, wp.z, wp.z - fly_ground(wp.xy), rc,\n" \
    "                                    fly_cloud_shadow(wp));\n" \
    "      return fly_apply_fog(wcol, ro, rd, hi);\n" \
    "    }\n" \
    "  }\n" \
    "  if (t < 0.0) return fly_sky(ro, rd);\n" \
    "  vec3 p = ro + rd*t;\n" \
    "  vec3 gn = fly_ground_normal(p.xy, 14.0);\n" \
    "  vec3 n = gn, mat;\n" \
    "  vec3 alb = fly_terrain_surface(p.xy, p.z, 1.0 - gn.z, uPxScale/t, mat);\n" \
    "  alb *= fly_terrain_detail(p.xy, mat, uPxScale/t, n);\n" \
    "  vec3 col = alb*(0.04 + 0.05*uDay);\n" \
    "  float ndl = dot(gn, jitter_sun);\n" \
    "  if (ndl > 0.0 && jitter_sun.z > 0.0 &&\n" \
    "      fly_trace_terrain(p + gn*3.0, jitter_sun, 9000.0) < 0.0)\n" \
    "    col += fly_terrain_light(alb, mat, n, -rd, fly_cloud_shadow(p), 1.0)\n" \
    "         - fly_terrain_light(alb, mat, n, -rd, 0.0, 1.0);\n" \
    "  vec3 bd = dot(bounce_dir, gn) < 0.0 ? -bounce_dir : bounce_dir;\n" \
    "  col += alb*fly_shade_direct(p + gn*3.0, bd)*0.6;\n" \
    "  return fly_apply_fog(col, ro, rd, t);\n" \
    "}\n"


/* Sun shadow map lookup: port of sm_pick / sm_bias / sm_cascade_vis /
 * shadow_sample from fly_render.c. The three cascades arrive as R32F textures
 * (nearest, clamped); off-map taps count as lit, exactly as on the CPU, so the
 * map's border never rings with false shadow.
 *
 * Expects:
 *   uniform vec3  uSmRight, uSmUp, uSmFwd;
 *   uniform vec3  uSmUmin, uSmVmin, uSmTexel;   x = near cascade, z = far
 *   uniform ivec3 uSmPcf;
 *   uniform vec3  uSmSize; uniform int uSmActive;
 *   uniform sampler2D uSmNear, uSmMid, uSmFar; */
/* The sun's disc and the penumbra it throws. Twins of FLY_SM_SUN_TAN and
 * FLY_SM_PEN_RMAX in fly_render.c, where the argument for both is written
 * out. */
#define FLY_GLSL_SUN_TAN "0.028"
#define FLY_GLSL_PEN_RMAX "5.0"
#define FLY_GLSL_PEN_TAPMAX 14

#define FLY_GLSL_SHADOW \
    /* `rad` is the filter half-width in texels: the bias covers how far the
     * receiver's own depth travels across the footprint, so a kernel spread
     * to soften a distant edge needs a proportionally larger one. Port of
     * sm_bias in fly_render.c. */ \
    "float fly_sm_bias(float texel, float ndotl, float pcf, float rad){\n" \
    "  float slope = clamp((1.0 - ndotl)/max(ndotl, 0.12), 0.0, 7.0);\n" \
    "  float span = (rad + 0.5)/(pcf + 0.5);\n" \
    "  return texel*(0.9 + slope*0.55)*span + 0.05;\n" \
    "}\n" \
    /* Normal offset: slide the sample off the surface along its own normal
     * before looking it up, instead of pushing its comparison depth further
     * back. Port of sm_offset in fly_render.c.
     *
     * A depth bias has to cover how much the receiver's own depth changes
     * across the filter footprint, and at a low sun that is large: one texel of
     * light-space travel is texel/tan(elevation) of depth, and the PCF box
     * spans two or more texels. Sized to cover it, the bias detaches contact
     * shadows by tens of metres at sunrise; sized not to, the terrain stripes
     * with its own acne — broad regular bands wherever the sun grazes it.
     * Moving perpendicular to the surface fixes the geometry rather than
     * papering over it: a shadow edge lying on the surface does not move, and
     * only occluders within the offset of the surface lose their contact.
     * Grazing light needs the most, head-on light none.
     *
     * `n` must already face the sun — a geometric normal whose sign follows the
     * winding would push the sample *into* the surface half the time. Callers
     * orient it once per primitive rather than once per fragment, which also
     * keeps this function free of the sun uniform. */ \
    /* Finest cascade at or after `from` covering this light-plane point, or -1.
     * Port of sm_pick. Returns the texel coordinates through `sxy` so the
     * caller does not divide twice. */ \
    "int fly_sm_pick(float pu, float pv, int from, out vec2 sxy){\n" \
    "  for (int c = 0; c < 3; ++c) {\n" \
    "    if (c < from) continue;\n" \
    "    float sz = uSmSize[c];\n" \
    "    float x = (pu - uSmUmin[c])/uSmTexel[c];\n" \
    "    float y = (pv - uSmVmin[c])/uSmTexel[c];\n" \
    "    if (x >= 1.0 && y >= 1.0 && x < sz-1.0 && y < sz-1.0) { sxy = vec2(x, y); return c; }\n" \
    "  }\n" \
    "  sxy = vec2(0.0);\n" \
    "  return -1;\n" \
    "}\n" \
    "vec3 fly_sm_offset(vec3 p, vec3 n, float ndotl){\n" \
    "  if (uSmActive == 0) return p;\n" \
    "  float pu = dot(p, uSmRight), pv = dot(p, uSmUp);\n" \
    "  vec2 sxy;\n" \
    "  int c = fly_sm_pick(pu, pv, 0, sxy);\n" \
    "  if (c < 0) return p;\n" \
    "  float graze = sqrt(clamp(1.0 - ndotl*ndotl, 0.0, 1.0));\n" \
    "  return p + n*(uSmTexel[c]*(float(uSmPcf[c]) + 1.6)*graze + 0.04);\n" \
    "}\n" \
    "float fly_sm_tap(int cascade, ivec2 c){\n" \
    "  if (cascade == 0) return texelFetch(uSmNear, c, 0).r;\n" \
    "  if (cascade == 1) return texelFetch(uSmMid, c, 0).r;\n" \
    "  return texelFetch(uSmFar, c, 0).r;\n" \
    "}\n" \
    /* bilinear-weighted PCF box; see sm_cascade_vis in fly_render.c for why the
     * nearest-tap version weaves a checkerboard across the terrain */ \
    /* Percentage-closer filter over a box of half-width `rad` texels, tapered.
     * Port of sm_cascade_vis in fly_render.c, where the argument for both the
     * overlap weights and the shoulder is written out: the radius is a real
     * number, the kernel fades a row in as it fades the next out rather than
     * stepping, and the widening tapers to nothing at the box's own edge so a
     * caster crossing it arrives at zero weight. At rad == pcf the shoulder is
     * zero wide and this is exactly the bilinear-weighted box the fixed-width
     * filter used, which the far cascade's four taps depend on. */ \
    "float fly_sm_vis(int cascade, float sx, float sy, float w, float bias, float rad,\n" \
    "                 inout float bsum, inout float bn){\n" \
    "  int sz = int(uSmSize[cascade]);\n" \
    "  float u = sx - 0.5, v = sy - 0.5;\n" \
    "  float ulo = u - rad, uhi = u + rad + 1.0;\n" \
    "  float vlo = v - rad, vhi = v + rad + 1.0;\n" \
    "  float hw = rad + 0.5;\n" \
    "  float sh = min(1.0, rad - float(uSmPcf[cascade]));\n" \
    "  int x0 = int(floor(ulo)), x1 = int(floor(uhi));\n" \
    "  int y0 = int(floor(vlo)), y1 = int(floor(vhi));\n" \
    "  float wxs[" FLY_GLSL__STR(FLY_GLSL_PEN_TAPMAX) "];\n" \
    "  float lit = 0.0, tot = 0.0;\n" \
    "  if (x1 - x0 >= " FLY_GLSL__STR(FLY_GLSL_PEN_TAPMAX) ") x1 = x0 + " FLY_GLSL__STR(FLY_GLSL_PEN_TAPMAX) " - 1;\n" \
    /* the column's weights do not depend on the row, so build them once */ \
    "  for (int i = x0; i <= x1; ++i) {\n" \
    "    float wx = min(float(i + 1), uhi) - max(float(i), ulo);\n" \
    "    if (sh > 0.0) wx *= clamp((hw - abs(float(i) - u))/sh, 0.0, 1.0);\n" \
    "    wxs[i - x0] = wx > 0.0 ? wx : 0.0;\n" \
    "  }\n" \
    "  for (int j = y0; j <= y1; ++j) {\n" \
    "    float wy = min(float(j + 1), vhi) - max(float(j), vlo);\n" \
    "    if (wy <= 0.0) continue;\n" \
    "    if (sh > 0.0) wy *= clamp((hw - abs(float(j) - v))/sh, 0.0, 1.0);\n" \
    "    if (wy <= 0.0) continue;\n" \
    "    for (int i = x0; i <= x1; ++i) {\n" \
    "      float wgt = wxs[i - x0]*wy;\n" \
    "      if (wgt <= 0.0) continue;\n" \
    "      tot += wgt;\n" \
    "      if (i < 0 || j < 0 || i >= sz || j >= sz) { lit += wgt; continue; }\n" \
    "      float d = fly_sm_tap(cascade, ivec2(i, j));\n" \
    "      if (d > 1e29 || w <= d + bias) { lit += wgt; continue; }\n" \
    /* it is in the way: the comparison this tap already made is the blocker
     * search, so record it rather than paying for it twice */ \
    "      bsum += d;\n" \
    "      bn += 1.0;\n" \
    "    }\n" \
    "  }\n" \
    "  return tot > 0.0 ? lit/tot : 1.0;\n" \
    "}\n" \
    /* Port of sm_ring: the blockers the base kernel cannot see. A search
     * inside the filter finds the caster over a receiver already in shadow and
     * misses the one standing just beyond it — the receiver in the outer half
     * of a wide penumbra, which is the half that decides whether an edge
     * softens both ways or only inward. Eight taps on a circle at the widest
     * radius the filter may reach, and nothing between. */ \
    "void fly_sm_ring(int cascade, float sx, float sy, float w, float bias,\n" \
    "                 inout float bsum, inout float bn){\n" \
    "  const vec2 k[8] = vec2[8](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0),\n" \
    "                            vec2(0.0, -1.0), vec2(0.7071, 0.7071), vec2(-0.7071, 0.7071),\n" \
    "                            vec2(0.7071, -0.7071), vec2(-0.7071, -0.7071));\n" \
    "  int sz = int(uSmSize[cascade]);\n" \
    "  for (int i = 0; i < 8; ++i) {\n" \
    "    int x = int(floor(sx + k[i].x*(" FLY_GLSL_PEN_RMAX " + 0.5)));\n" \
    "    int y = int(floor(sy + k[i].y*(" FLY_GLSL_PEN_RMAX " + 0.5)));\n" \
    "    if (x < 0 || y < 0 || x >= sz || y >= sz) continue;\n" \
    "    float d = fly_sm_tap(cascade, ivec2(x, y));\n" \
    "    if (d > 1e29 || w <= d + bias) continue;\n" \
    "    bsum += d;\n" \
    "    bn += 1.0;\n" \
    "  }\n" \
    "}\n" \
    /* Port of sm_penumbra: the gap between blocker and receiver is measured
     * along the light's own travel, so it is the distance the sun's disc
     * spreads over, and the penumbra is that gap times the disc's tangent. */ \
    "float fly_sm_penumbra(int cascade, float w, float bsum, float bn){\n" \
    "  float base = float(uSmPcf[cascade]);\n" \
    "  if (bn <= 0.0) return base;\n" \
    "  float gap = w - bsum/bn;\n" \
    "  if (gap <= 0.0) return base;\n" \
    "  return clamp(gap*" FLY_GLSL_SUN_TAN "/uSmTexel[cascade], base, " FLY_GLSL_PEN_RMAX ");\n" \
    "}\n" \
    /* Port of sm_cascade: filter at the base width, learn how far off the
     * caster was while doing it, and filter again at the width that gap
     * deserves. Only a receiver with something distant over it pays for the
     * second kernel. */ \
    "float fly_sm_cascade(int cascade, float sx, float sy, float w, float ndotl){\n" \
    "  float pcf = float(uSmPcf[cascade]);\n" \
    "  float base = fly_sm_bias(uSmTexel[cascade], ndotl, pcf, pcf);\n" \
    "  float bsum = 0.0, bn = 0.0;\n" \
    "  float vis = fly_sm_vis(cascade, sx, sy, w, base, pcf, bsum, bn);\n" \
    "  fly_sm_ring(cascade, sx, sy, w, base, bsum, bn);\n" \
    "  float rad = fly_sm_penumbra(cascade, w, bsum, bn);\n" \
    "  if (rad <= pcf + 1e-3) return vis;\n" \
    "  bsum = 0.0;\n" \
    "  bn = 0.0;\n" \
    "  return fly_sm_vis(cascade, sx, sy, w, fly_sm_bias(uSmTexel[cascade], ndotl, pcf, rad),\n" \
    "                    rad, bsum, bn);\n" \
    "}\n" \
    /* the coarser cascade is only tapped inside the border band where the two
     * cross-fade — that branch saves a whole PCF kernel over most of the frame */ \
    "float fly_shadow_sample(vec3 p, float ndotl){\n" \
    "  if (uSmActive == 0) return 1.0;\n" \
    "  float pu = dot(p, uSmRight), pv = dot(p, uSmUp), w = dot(p, uSmFwd);\n" \
    "  vec2 sxy;\n" \
    "  int c = fly_sm_pick(pu, pv, 0, sxy);\n" \
    "  if (c < 0) return 1.0;\n" \
    "  float sz = uSmSize[c];\n" \
    "  float vis = fly_sm_cascade(c, sxy.x, sxy.y, w, ndotl);\n" \
    "  float edge = min(min(sxy.x, sz-1.0-sxy.x), min(sxy.y, sz-1.0-sxy.y));\n" \
    "  float band = sz*0.10;\n" \
    "  if (edge >= band) return vis;\n" \
    "  vec2 fxy;\n" \
    "  int c2 = fly_sm_pick(pu, pv, c + 1, fxy);\n" \
    "  float vcoarse = c2 >= 0 ? fly_sm_cascade(c2, fxy.x, fxy.y, w, ndotl) : 1.0;\n" \
    "  return mix(vcoarse, vis, clamp(edge/band, 0.0, 1.0));\n" \
    "}\n" \
    /* Is this parcel of *air* in the sun? Port of shadow_beam in fly_render.c.
     * One tap, no filter kernel: there is no surface here to acne against and
     * nothing to soften a contact against, and the volumetric march that calls
     * it takes a dozen of these per ray. The cascade cross-fade stays, because
     * a beam that changed brightness at a cascade seam would be visible as a
     * line hanging in mid-air. */ \
    "float fly_shadow_beam(vec3 p){\n" \
    "  if (uSmActive == 0) return 1.0;\n" \
    "  float pu = dot(p, uSmRight), pv = dot(p, uSmUp), w = dot(p, uSmFwd);\n" \
    "  vec2 sxy;\n" \
    "  int c = fly_sm_pick(pu, pv, 0, sxy);\n" \
    "  if (c < 0) return 1.0;\n" \
    "  float d = fly_sm_tap(c, ivec2(sxy));\n" \
    "  float pc = float(uSmPcf[c]);\n" \
    "  float vis = (d > 1e29 || w <= d + fly_sm_bias(uSmTexel[c], 1.0, pc, pc)) ? 1.0 : 0.0;\n" \
    "  float sz = uSmSize[c];\n" \
    "  float edge = min(min(sxy.x, sz-1.0-sxy.x), min(sxy.y, sz-1.0-sxy.y));\n" \
    "  float band = sz*0.10;\n" \
    "  if (edge >= band) return vis;\n" \
    "  vec2 fxy;\n" \
    "  int c2 = fly_sm_pick(pu, pv, c + 1, fxy);\n" \
    "  float vcoarse = 1.0;\n" \
    "  if (c2 >= 0) {\n" \
    "    float d2 = fly_sm_tap(c2, ivec2(fxy));\n" \
    "    float pc2 = float(uSmPcf[c2]);\n" \
    "    vcoarse = (d2 > 1e29 || w <= d2 + fly_sm_bias(uSmTexel[c2], 1.0, pc2, pc2)) ? 1.0 : 0.0;\n" \
    "  }\n" \
    "  return mix(vcoarse, vis, clamp(edge/band, 0.0, 1.0));\n" \
    "}\n"

/* The wood as a medium, and the sun term that folds it in.
 *
 * Its own block rather than part of FLY_GLSL_SHADOW because it needs uniforms
 * that block does not — uSun, uSeed and the two canopy rows — and one of
 * FLY_GLSL_SHADOW's users is the shaft march, which wants the map on its own:
 * a parcel of air between the eye and the ground is not under the canopy the
 * ground is under. Include it after FLY_GLSL_WORLD, which is where fly_canopy
 * lives, and declare FLY_GLSL_CANOPY_UNIFORMS with it. */
#define FLY_GLSL_CANOPY_UNIFORMS \
    "uniform vec4 uCanopy;\n"  /* near cascade centre xy, crown-cast radius, FLY_CANOPY_H */ \
    "uniform vec3 uCanopyK;\n" /* FLY_CANOPY_K, FLY_CANOPY_TOP, FLY_CANOPY_OFFSET */

#define FLY_GLSL_CANOPY \
    /* Twins of canopy_medium and canopy_sun in fly_render.c, which carry the
       argument for both: past the near cascade's footprint the scatter's
       crowns are not cast at all, and what stands in for them is the wood as a
       medium — one number for how much sun a stand lets through, tapped where
       the ray crosses the crown layer rather than overhead, so an edge throws
       a fringe the way a tree does.

       uCanopy is (near cascade centre xy, the radius draw_foliage casts real
       crowns inside, FLY_CANOPY_H) and uCanopyK is (FLY_CANOPY_K,
       FLY_CANOPY_TOP, FLY_CANOPY_OFFSET). Every constant arrives as a uniform
       rather than as a literal here: this header is shader source and cannot
       include the ones that own them, and a second copy of a tuned number is a
       second thing to keep true. */                                              \
    "float fly_canopy_sun(vec3 p, float agl){\n"                              \
    "  if (uSun.z <= 0.05 || agl >= uCanopyK.y) return 1.0;\n"                \
    "  float k = uSmActive == 0 ? 1.0\n"                                      \
    "          : fly_smoothstep(uCanopy.z*0.75, uCanopy.z, distance(p.xy, uCanopy.xy));\n" \
    "  if (k <= 0.0) return 1.0;\n"                                           \
    "  if (agl > 0.0) k *= 1.0 - agl/uCanopyK.y;\n"                           \
    "  float t = min(uCanopy.w/uSun.z, uCanopyK.z);\n"                        \
    "  float d = fly_canopy(uint(uSeed), p.xy + uSun.xy*t, p.z, uCanopy.w)/uCanopy.w;\n" \
    "  return exp(-uCanopyK.x*d*k);\n"                                        \
    "}\n"                                                                     \
    /* What the terrain shades with: the map, and the wood standing over it.
       Twin of terrain_sun. */                                                \
    "float fly_sun_at(vec3 p, vec3 n, float ndotl, float agl){\n"             \
    "  return fly_shadow_sample(fly_sm_offset(p, n, ndotl), ndotl)\n"         \
    "       * fly_canopy_sun(p, agl);\n"                                      \
    "}\n"


/* Camera projection: the GPU twin of make_view/project in fly_render.c.
 *
 * fly_project returns clip space, so the hardware does the perspective divide
 * and — unlike the CPU rasterizer, which drops any triangle with a vertex
 * nearer than the near plane — clips properly against it. Depth is the usual
 * near/far mapping; the linear view depth the CPU z-buffer wants is written
 * separately into the target's alpha.
 *
 * Expects:
 *   uniform vec3  uCamPos, uCamRight, uCamUp, uCamFwd;
 *   uniform vec2  uRes;    framebuffer size in pixels
 *   uniform float uScale;  fly__view.sy
 *   uniform vec2  uClip;   near, far
 *   uniform int   uOrtho;  1 = parallel projection */
#define FLY_GLSL_PROJECT \
    "vec4 fly_project(vec3 p){\n" \
    "  vec3 d = p - uCamPos;\n" \
    "  float zv = dot(d, uCamFwd);\n" \
    "  float px = dot(d, uCamRight)*uScale/(uRes.x*0.5);\n" \
    "  float py = dot(d, uCamUp)   *uScale/(uRes.y*0.5);\n" \
    "  float n = uClip.x, f = uClip.y;\n" \
    "  if (uOrtho != 0) return vec4(px, py, (zv - n)/(f - n)*2.0 - 1.0, 1.0);\n" \
    "  return vec4(px, py, zv*(f + n)/(f - n) - 2.0*f*n/(f - n), zv);\n" \
    "}\n" \
    /* primary ray through a pixel, matching raster_sky's screen->direction */ \
    "vec3 fly_pixel_ray(vec2 frag){\n" \
    "  return normalize(uCamFwd + uCamRight*((frag.x - uRes.x*0.5)/uScale)\n" \
    "                           + uCamUp   *((frag.y - uRes.y*0.5)/uScale));\n" \
    "}\n"

/* Light-space projection used to *build* a shadow cascade: the mirror image of
 * the lookup above, and a port of sm_project from fly_render.c.
 *
 * The cascade is orthographic along the sun, so the stored quantity is just
 * dot(P, light-forward) in world units — which is what the lookup compares
 * against, and what the fragment shader writes out. Clip z only has to order
 * fragments, so it is that same distance mapped linearly onto the cascade's
 * depth interval; the hardware depth test then keeps the nearest-to-sun
 * fragment per texel, exactly as sm_raster_tri's `w < depth[idx]` does.
 *
 * Expects:
 *   uniform vec3  uCastRight, uCastUp, uCastFwd;
 *   uniform vec2  uCastOrigin;  umin, vmin
 *   uniform vec2  uCastRange;   dmin, dmax along the light
 *   uniform float uCastTexel, uCastSize; */
#define FLY_GLSL_CAST \
    "uniform vec3 uCastRight, uCastUp, uCastFwd;\n" \
    "uniform vec2 uCastOrigin, uCastRange;\n" \
    "uniform float uCastTexel, uCastSize;\n" \
    "vec4 fly_cast_project(vec3 p, out float d){\n" \
    "  float u = (dot(p, uCastRight) - uCastOrigin.x)/uCastTexel;\n" \
    "  float v = (dot(p, uCastUp)    - uCastOrigin.y)/uCastTexel;\n" \
    "  d = dot(p, uCastFwd);\n" \
    "  float z = (d - uCastRange.x)/(uCastRange.y - uCastRange.x);\n" \
    "  return vec4(u/uCastSize*2.0 - 1.0, v/uCastSize*2.0 - 1.0, z*2.0 - 1.0, 1.0);\n" \
    "}\n"

/* Uniform block shared by the environment programs (sky, terrain, water), so
 * the three declare exactly the same interface and one setter fills them. */
#define FLY_GLSL_ENV_UNIFORMS \
    "uniform int uSeed;\n" \
    "uniform int uLocCount;\n" \
    "uniform vec4 uLoc[28];\n" \
    "uniform vec3 uSun, uWind;\n" \
    "uniform float uDay, uDusk, uStorm, uTime;\n" \
    /* how wet the world is, 0 dry .. 1 running with it — see fly__env.wet */ \
    "uniform float uWet;\n" \
    "uniform float uCloudMean;\n" \
    "uniform vec3 uCloudLit;\n" \
    "uniform vec3 uCloudDark;\n" \
    /* the ice sheet seven kilometres up: its mean cover, the one colour it
       has, and the bearing its bands are drawn out along — see cirrus_sheet */ \
    "uniform float uCirrusMean;\n" \
    "uniform vec3 uCirrusLit;\n" \
    "uniform vec2 uCirrusDir;\n" \
    "uniform vec3 uSunLight;\n" \
    "uniform vec3 uSH[9];\n" \
    "uniform vec3 uGround;\n" \
    "uniform vec3 uCamPos, uCamFwd, uCamRight, uCamUp;\n" \
    "uniform vec2 uRes, uClip;\n" \
    "uniform float uScale;\n" \
    "uniform float uPxScale;\n" \
    "uniform float uBall;\n" \
    "uniform int uOrtho;\n"

/* Standard preamble for fly99 GLSL programs.
 *
 * `precision highp sampler2D` is not redundant: GLSL ES predeclares samplers as
 * *lowp*, and `precision highp float` does not cover them. Left at the default,
 * a texelFetch from the R32F shadow cascades came back quantised to about ten
 * mantissa bits — a several-metre error on a ~9 km light-space depth, which
 * banded the whole frame with shadow acne. */
#define FLY_GLSL_HEADER \
    "#version 310 es\n"                                                        \
    "precision highp float;\n"                                                 \
    "precision highp int;\n"                                                   \
    "precision highp sampler2D;\n"

#endif /* FLY_GLSL_H */
