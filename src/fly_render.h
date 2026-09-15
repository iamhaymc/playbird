/* fly_render: hybrid 3D renderer.
 *
 * Two pipelines share one render target:
 *  - raster: transformed triangles with a z-buffer (terrain mesh, craft,
 *    markers) — fast, always available.
 *  - path tracer: per-pixel rays marched against the terrain heightfield
 *    with sun shadowing and one diffuse bounce — slow, cinematic.
 * FLY_RENDER_MIX path-traces the environment and rasterizes dynamic objects
 * on top using the traced depth, so both techniques compose in one frame.
 * Supersampling renders at scale N and box-filters down (see fly_img).
 */
#ifndef FLY_RENDER_H
#define FLY_RENDER_H

#include "fly_game.h"
#include "fly_img.h"
#include "fly_math.h"

typedef enum {
    FLY_RENDER_RASTER, /* pure raster */
    FLY_RENDER_PT,     /* pure path tracing (environment; objects still raster) */
    FLY_RENDER_MIX     /* traced environment blended with raster environment */
} fly_render_mode;

/* --- graphics levels ---
 *
 * One ladder, four rungs, and every cost in the renderer hangs off it: the
 * terrain tessellation and the shadow cascades as before, and now also how
 * round a fuselage is built, how far the woodland and the grass reach, how
 * many lobes a plume is made of, and whether the air between the camera and
 * the world is marched for light shafts.
 *
 * The rungs are not a preference, they are two aims with a range in between.
 * LOW is a frame budget — 33 ms, thirty frames a second, at 480x270 on one
 * ordinary CPU core with no GPU of any kind — because the software path is the
 * fallback for machines with no GL driver and a fallback nobody can hold a
 * stopwatch to stops being one. ULTRA is the other end: everything the renderer
 * knows how to do, priced at whatever it costs, because the ceiling is what the
 * stills are shot at and a ceiling that flinches is not a ceiling.
 *
 * The budget is a target and not yet a fact, and `render.budget` prints the
 * distance rather than hiding it: on the shared virtual core this is developed
 * on, LOW renders the alpine vista — deliberately the most expensive view there
 * is — in about 230 ms, seven times over. Reordering the environment so the sky
 * is only evaluated where the ground does not cover it, and putting that sky on
 * a refined lattice at this rung, took it from 465 ms. What is left is the
 * aerial-perspective integral and the cloud-deck tap, both still per vertex
 * over the terrain rings; over the scene's *objects* they are now shared per
 * object, which is what `fog_anchor_begin` is for. What the test gates is what
 * is true on any machine — that LOW is a large multiple cheaper than ULTRA, and
 * that the number is on the record every run.
 *
 * The rungs are ordered and cumulative: rung n draws everything rung n-1 draws.
 * That is what lets a caller reason about them as a slider and what lets the
 * shipped level be picked by measurement rather than by taste. */
typedef enum {
    FLY_QUALITY_LOW = 0,    /* software-renderer budget: 30 fps on a CPU */
    FLY_QUALITY_MEDIUM = 1, /* the default: shadows, foliage, round models */
    FLY_QUALITY_HIGH = 2,   /* dense terrain, big cascades, light shafts */
    FLY_QUALITY_ULTRA = 3   /* as near photoreal as this renderer gets */
} fly_render_quality;

typedef struct {
    fly_render_mode mode;
    float pt_blend;   /* MIX: 0 = all raster, 1 = all traced */
    int pt_samples;   /* paths per pixel (>=1) */
    /* Traced ambient occlusion, cached in world space. `ao_target` is how many
     * rays a patch of ground converges to (0 keeps the old density scalar).
     * `ao_budget` caps the rays a single frame may add: 0 means fill every
     * visible patch before drawing, which is what makes a single frame
     * reproducible, and a positive budget refines over successive frames
     * instead and keeps whatever it has already learned when the camera moves. */
    int ao_target;
    long ao_budget;
    int ssaa;         /* supersample factor (1, 2, ...) */
    int post;         /* tonemap + bloom + vignette pass (default on) */
    int detail;       /* the graphics level: a fly_render_quality rung */
    int msaa;         /* GPU geometry multisampling: 1 = off, else 2/4/8 */
    int studio;       /* 1 = object only on a flat backdrop (no sky/terrain/sites) */
} fly_render_opts;

typedef struct {
    fly_img img;      /* 8-bit output, produced by the HDR resolve */
    fly_v3 *hdr;      /* linear radiance accumulation buffer */
    float *depth;     /* z-buffer, view depth in meters */
    int w, h;
} fly_render_target;

typedef struct {
    fly_v3 pos;
    fly_v3 fwd, up;
    float fov;        /* vertical, radians (perspective only) */
    float ortho_h;    /* >0 = orthographic; world-space height of the view */
} fly_cam;

int fly_rt_init(fly_render_target *rt, int w, int h);
void fly_rt_free(fly_render_target *rt);
void fly_rt_clear(fly_render_target *rt, uint32_t color);

fly_render_opts fly_render_opts_default(void);
/* A whole options set for one rung of the ladder: the graphics level chooses
 * the sampling (ssaa, msaa, traced occlusion) as well as the detail, because
 * the two are one decision — a frame budget spent on supersampling is a frame
 * budget not spent on geometry, and picking them separately is how a "low"
 * preset ends up supersampled. */
fly_render_opts fly_render_opts_quality(fly_render_quality q);
/* "low" | "medium" | "high" | "ultra", for CLI and HUD text. */
const char *fly_render_quality_name(fly_render_quality q);
/* chase camera behind the player craft */
fly_cam fly_cam_chase(const fly_game *g);
fly_cam fly_cam_player(const fly_game *g);
/* orbit camera around a point (used for edit/preview shots) */
fly_cam fly_cam_orbit(fly_v3 center, float yaw, float pitch, float dist);
/* axis-aligned orthographic elevation: parallel projection, `height` world
 * units tall. Picks a stable up vector so a straight top-down view works. */
fly_cam fly_cam_ortho(fly_v3 center, float yaw, float pitch, float dist, float height);

/* raster primitives */
void fly_raster_tri(fly_render_target *rt, const fly_cam *cam,
                    fly_v3 a, fly_v3 b, fly_v3 c, uint32_t color);

/* full frame: sky, terrain, locations, craft, weather cues */
void fly_render_frame(fly_render_target *rt, const fly_game *g, const fly_cam *cam,
                      const fly_render_opts *opts);

/* render into img at opts->ssaa scale then resolve (allocates internally) */
int fly_render_frame_aa(fly_img *out, const fly_game *g, const fly_cam *cam,
                        const fly_render_opts *opts);

/* --- where the frame went ---
 *
 * Wall-clock milliseconds the last frame spent in each phase. A frame that
 * misses its budget has to be able to say which part of it did: the renderer
 * is a CPU pipeline, a GPU pipeline and a hand-over between them, and the
 * three fail in different ways. Guessing which one is slow from one total is
 * how the object build came to cost more than every GL pass put together
 * without anybody noticing.
 *
 * The phases partition the frame, so they sum to it (within the cost of
 * reading a clock nine times, which is nanoseconds). GL is asynchronous, so a
 * phase that only queues work bills the queueing and the driver bills the wait
 * to whichever phase next asks for a result — FLY_PHASE_FINISH and
 * FLY_PHASE_RESOLVE, both of which end in a synchronising call. That is a
 * property of the pipeline rather than of the measurement: the CPU phases are
 * the ones a faster GPU cannot help, which is exactly the split a reader
 * wants. */
typedef enum {
    FLY_PHASE_SETUP = 0, /* view, environment, occlusion window, depth clear */
    FLY_PHASE_SHADOW,    /* sun shadow cascades */
    FLY_PHASE_TERRAIN,   /* terrain and water rings: grid build, uniforms, draw */
    FLY_PHASE_OBJECTS,   /* CPU build + Gouraud shade of settlements/craft/scatter */
    FLY_PHASE_UPLOAD,    /* handing that geometry to GL */
    FLY_PHASE_FINISH,    /* shafts, emissive splats, translucent pass */
    FLY_PHASE_READBACK,  /* GL frame -> main memory (0 on a resident frame) */
    FLY_PHASE_RESOLVE,   /* exposure, bloom, tonemap, supersample downsample */
    FLY_PHASE_OVERLAY,   /* rain and lightning over the finished picture */
    FLY_PHASE_COUNT
} fly_render_phase;

/* The last frame's breakdown: `ms` receives FLY_PHASE_COUNT milliseconds.
 * `verts` (may be NULL) receives the object vertices that frame built. */
void fly_render_timings(double *ms, long *verts);
/* "setup", "shadow", ... for CLI and HUD text. */
const char *fly_render_phase_name(int phase);

/* Which pipeline drew the last frame's sky/terrain/water: "gpu" or "cpu".
 * The GPU environment pass is always attempted first; "cpu" means no GL
 * backend was available (or a GL call failed) and the reference rasterizer
 * ran instead. */
const char *fly_render_env_backend(void);
/* Vertices the last frame drew as *instances* rather than building — the wood's
 * crowns, and nothing else so far. Zero on the software rasterizer and on any
 * frame the instance path did not take, which is what makes it the difference
 * between the two pipelines' object builds rather than a second way of counting
 * the same thing. See the instance lattice in fly_render.c. */
long fly_render_instanced_verts(void);
/* Test hook: pin the environment to the CPU reference pipeline (1) or restore
 * the normal GPU-first behaviour (0). Returns the previous setting. Exists so a
 * test can render the same frame both ways and compare; nothing else should
 * call it — the renderer's policy is GPU whenever one exists. */
int fly_render_env_force_cpu(int on);
/* Test hook: bring the frame down to main memory as soon as the geometry pass
 * has drawn it, so everything after — the volumetric correction, the emissive
 * splats, the translucent pass, auto-exposure — runs in C off the buffer
 * rather than as GL passes over a frame that never left the GPU. The two are
 * meant to describe the same frame, and this is what lets a test render it
 * both ways with the geometry held constant. Returns the previous setting;
 * nothing but a test should call it, because the readback it forces is the
 * single largest cost the renderer has. */
int fly_render_force_readback(int on);
/* Test hook: stop sharing one scattering integral and one cloud-deck tap across
 * a whole object, so every triangle pays for its own. Off (0) is what ships and
 * what the renderer means; on (1) is the reference the sharing is measured
 * against. Returns the previous setting. See render.fog, which renders the same
 * wood both ways and holds how far apart the two frames may be. */
int fly_render_fog_per_tri(int on);

/* Cascades in the sun shadow map, finest first. Public because the probes below
 * hand back one value per cascade and callers have to size their arrays. */
#define FLY_SHADOW_CASCADES 3

/* --- shadow-map test hook ---
 * Builds this frame's sun shadow map for `g`/`cam` and reports its parameters
 * plus a sun-visibility sample at each of `n` world points. Exists so the GLSL
 * port of the lookup can be checked against the C original; not part of the
 * normal render path. Returns 0 on success, negative if no map was built (e.g.
 * the sun is below the horizon).
 *
 * `depth` may be NULL, or an array of FLY_SHADOW_CASCADES pointers of which any
 * may be NULL; each non-NULL one receives that cascade's size*size texels.
 * `size`, `pcf`, `umin`, `vmin` and `texel` each receive FLY_SHADOW_CASCADES
 * values — the far cascade is built smaller than the others.
 *
 * `nrm` may be NULL, in which case the points are sampled exactly as given —
 * that is what the GLSL-vs-C parity check wants. Pass surface normals instead
 * and the probe applies the renderer's normal offset first, so `vis` is what
 * the frame would actually shade with. */
int fly_render_shadow_probe(const fly_game *g, const fly_cam *cam, int detail,
                            const fly_v3 *pts, const float *ndotl, const fly_v3 *nrm,
                            float *vis, int n,
                            float *const *depth, int *size, int *pcf,
                            fly_v3 *right, fly_v3 *up, fly_v3 *fwd,
                            float *umin, float *vmin, float *texel);

/* --- terrain sun test hook ---
 * Sun visibility at ground points the way the terrain shader takes it: the
 * cast shadows the map holds, times what the canopy standing over the point
 * lets through. `fly_render_shadow_probe` above answers only the map, which is
 * what a comparison against a march of the heightfield wants; this answers the
 * whole sun term, which is what a comparison between the two ways a wood is
 * shaded wants — real crowns inside the near cascade, the medium beyond it.
 * `nrm` is the surface normal at each point. Returns 0, or negative when the
 * sun is down and there is no map. */
int fly_render_terrain_sun_probe(const fly_game *g, const fly_cam *cam, int detail,
                                 const fly_v3 *pts, const fly_v3 *nrm, float *vis, int n);

/* --- cascade-build test hook ---
 * Builds one cascade of this frame's shadow map both ways and hands back the
 * texels: `gpu_c` from the GL builder, `cpu_c` from the reference rasterizer,
 * each size*size floats (allocate for the largest cascade the detail level can
 * ask for). The two are meant to be equal within float rounding — a divergence
 * means the GL build has drifted from the C one, which is invisible in a frame
 * until a shadow quietly goes missing. Returns 0 on success, negative when no
 * GL backend built a map (nothing to compare) or the sun is down. */
int fly_render_shadow_cascade_probe(const fly_game *g, const fly_cam *cam, int detail,
                                    float *gpu_c, float *cpu_c, int *size, int cascade);

/* --- terrain material test hook ---
 *
 * What the ground at `p` is made of and how it answers one lighting geometry.
 * `mat` comes back as (vegetation, normal-incidence reflectance, Blinn
 * exponent) and `radiance` as the shaded colour for surface normal `nrm`, unit
 * view direction `vdir` (surface toward eye) and sun visibility `shadow`.
 *
 * Exposed because the vegetation response is a claim about a *function* —
 * grass is brighter down-sun than up-sun at the same N.L, because a canopy
 * hides its own shadows when you look along the light — and a rendered frame
 * cannot isolate it. Turning the camera around to put the sun behind you also
 * changes which slopes you are looking at, and plain Lambert already makes the
 * faces turned toward you brighter when the light is over your shoulder; that
 * confound measured 27% against the 12% the canopy terms are worth. Evaluating
 * the function at a fixed normal with only the view direction moving is the
 * only way to measure the thing itself.
 *
 * `px_per_m` is output pixels per world metre at the point: it selects how
 * much of the albedo's octave detail is resolvable there, so pass something
 * huge for "all of it". Returns 0, or negative if there is no game. */
int fly_render_material_probe(const fly_game *g, fly_v3 p, fly_v3 nrm, fly_v3 vdir,
                              float shadow, float px_per_m,
                              fly_v3 *mat, fly_v3 *albedo, fly_v3 *radiance);

/* --- microfacet BRDF test hook ---
 *
 * The specular lobe on its own, with no scene around it: `lobe` is the
 * distribution times the masking term times the cosine — what the sunbeam is
 * multiplied by — and `env` is the split-sum environment response, the fraction
 * of the sky's radiance in the mirror direction that comes back.
 *
 * Public because the claims worth testing about a BRDF are claims about a
 * *function*, and not one of them can be read out of a tonemapped frame.
 * Whether it conserves energy is an integral over the hemisphere; whether it is
 * reciprocal is an equality between two evaluations with the light and the eye
 * swapped; whether roughness does what roughness means is a comparison of two
 * lobes at their peaks. A rendered image can show that a highlight is there. It
 * cannot show that the surface reflects less light than falls on it, which is
 * exactly the property the lobe this replaced did not have.
 *
 * `gloss` is the material's Blinn exponent, which is how the material tables
 * and `fly_render_material_probe` describe a surface; the conversion to a GGX
 * roughness happens inside. `f0` is the normal-incidence reflectance. The
 * directions are given as cosines against the surface normal and the half
 * vector, which is all the lobe depends on. Either output may be NULL. */
int fly_render_brdf_probe(float gloss, float f0, float ndl, float ndv,
                          float ndh, float vdh, float *lobe, float *env);

/* --- volumetric shaft statistics ---
 *
 * What the last frame's volumetric pass did, in three numbers: the mean and the
 * most negative per-sample correction to the radiance buffer, and how many
 * samples it touched at all. Zero touched means the pass did not run — the
 * graphics level did not ask for it, there was no shadow map, or the sun was
 * down.
 *
 * Public because the invariant that matters here is a sign. The pass models air
 * that never received sunlight, so it may only ever *remove* light; a version
 * that adds a bright streak toward the sun is a lens effect wearing a
 * volumetric's name, and it would look convincing in every screenshot while
 * putting energy into the frame that nothing emitted. A test can hold the sign.
 * Any pointer may be NULL.
 *
 * The GPU pass marches its lattice in GL memory and reports nothing unless
 * fly_render_shaft_probe has been turned on, because answering means reading
 * that lattice back and a frame built not to cross the bus should not cross it
 * for a statistic. The software pass accumulates the same numbers as it
 * marches, for nothing, and always reports them. */
void fly_render_shaft_stats(double *mean, double *worst, long *touched);
/* Collect fly_render_shaft_stats from the GPU pass too. Returns the previous
 * setting; off by default. */
int fly_render_shaft_probe(int on);

/* --- atmosphere test hook ---
 *
 * Sky radiance along `rd` from `ro`, and the pair the aerial-perspective model
 * produces over `dist`: `transmit` is what survives of a surface at that range
 * and `inscat` is what the air in front of it adds. Public because sky colour,
 * sun colour and aerial perspective all come out of one scattering integral
 * now, and the way to check that is to ask it, not to read a tonemapped frame.
 * Takes the time of day from `g`. */
int fly_render_atmos_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                           fly_v3 *sky, fly_v3 *transmit, fly_v3 *inscat, fly_v3 *sun);

/* --- cloud deck test hook ---
 *
 * How much of the ray from `ro` along `rd` out to `dist` the deck covers, and
 * the colour it covers it with — the march itself, not a pixel it ended up in.
 * A short ray is a probe of one place in the volume, which is how a test asks
 * where a cloud stops: the deck's tops follow the coverage field, so "how high
 * does the cloud go here" has a different answer in every column and cannot be
 * read off a rendered frame without the sky, the haze and the tonemap in the
 * way. Takes the time of day and the weather from `g`. */
int fly_render_cloud_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                           float *cover, fly_v3 *color);

/* --- ice sheet test hook ---
 *
 * The same question about the second layer: how much of the ray the cirrus at
 * seven kilometres covers, and the colour it covers it with. Separate from the
 * deck's hook rather than folded into it because the two layers are different
 * models — a march through a slab and one plane intersection — and a test that
 * could not tell them apart could not say which one it had measured. Takes the
 * time of day and the weather from `g`. */
int fly_render_cirrus_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, float dist,
                            float *cover, fly_v3 *color);

/* --- what the two layers do to the sunbeam ---
 *
 * The sun `n` points still get with both decks between them and it, and — so a
 * test can say which layer it is looking at — the cover each layer alone puts
 * on the beam at each point. Public because the composition is a claim rather
 * than an arithmetic identity: the layers do not multiply, the ice shades the
 * ground the deck left clear, and the deck's own share already carries the ice
 * (see the march). None of that is visible in a rendered frame, where a cloud
 * shadow and a hillside in shade are the same pixel. Any of the three outputs
 * may be NULL. Takes the time of day and the weather from `g`. */
int fly_render_cloud_sun_probe(const fly_game *g, const fly_v3 *pts, float *sun,
                               float *deck, float *ice, int n);

/* --- propeller disc geometry ---
 *
 * Where the disc sits on an airframe: `hub` is the hub as an offset from the
 * craft's origin in body-frame metres — forward, right, up — and `radius` is
 * the disc's radius in world metres, sized to the airframe and clipped for
 * ground clearance.
 * Public because `render.cine` measures the disc off a rendered frame and has
 * to know where on screen it is; it had those numbers copied out, the copy
 * went stale when the radius stopped being a flat 1.9 m, and the gate spent
 * that time sampling clear sky past the tip. Either output may be NULL. */
int fly_render_prop_disc(const fly_airframe *af, fly_v3 *hub, float *radius);

/* --- sky test hook, with parts of it left out ---
 *
 * The sky along `rd` from `ro`, the same integral `fly_render_atmos_probe`
 * returns, minus whichever of two things the caller asks to be left out.
 *
 * Public because a claim about *air* cannot be measured through a cloud. The
 * Rayleigh ratios in `render.atmosphere` are statements about the atmosphere,
 * and asking the ordinary probe for them returns whatever cloud happened to be
 * in that direction on that seed at that hour — the noon zenith reads 2.03
 * with the sky clear over that spot, and the changelog records it reading 1.56
 * when a second layer was first tried over it. Neither number is about
 * Rayleigh scattering. `NOSUN` is the same switch the
 * irradiance projection uses, for the same reason: the disc is carried as a
 * directional light, so anything summing the sky as a source has to leave it
 * out or count it twice.
 *
 * None of this is what lights the world. The renderer's own sky is the sky
 * that is there, cloud included; these are questions a test asks. */
#define FLY_RENDER_SKY_NOSUN 1u    /* leave the disc and its halo out */
#define FLY_RENDER_SKY_NOCLOUD 2u  /* leave both cloud layers out */
/* Put the star field in. Off by default because everything that asks this for
 * a colour wants the sky's average in a direction and a star is a point sample
 * of a hash — see sky_sample. On, it is the only way to ask when a star is
 * visible: the field is gated on the sky's own radiance rather than on the
 * clock, so the question "is the sky bright enough to hide this one" has no
 * answer outside the sky itself, and counting specks in a rendered frame
 * cannot separate a star from a lit hilltop. */
#define FLY_RENDER_SKY_STARS 4u
int fly_render_sky_probe(const fly_game *g, fly_v3 ro, fly_v3 rd, unsigned opts,
                         fly_v3 *sky);

/* --- object shading test hook ---
 *
 * What one triangle of an aircraft, a settlement or a ground item reflects for
 * a given surface normal: `lit` with the sun unoccluded and `shaded` with it
 * fully occluded, which is the pair the shadow map cross-fades between per
 * pixel. Public because the invariant that matters here cannot be read back
 * out of a tonemapped frame — a term that removes light poisons the frame's
 * auto-exposure rather than showing up as a dark patch, and whether it poisons
 * a given frame at all depends on where the sampling grid happens to land.
 * Either output may be NULL. Takes the time of day and the weather from `g`. */
int fly_render_surface_probe(const fly_game *g, fly_v3 n, fly_v3 albedo, float ao,
                             fly_v3 *lit, fly_v3 *shaded);

/* --- the air along a ray ---
 *
 * Sea-level-equivalent metres of an atmosphere of scale height `H` crossed
 * between `z0` and `z0 + dz*dist`, on the ball: the raw column the scattering
 * integral is built out of, before any coefficient, compression or phase
 * function touches it. `dz` is the ray's vertical component and the chart is a
 * tangent plane, so that plus the altitude is the whole geometry.
 *
 * Exposed because the column is the part with an exactly right answer — a
 * grazing ray at sea level crosses sqrt(2*pi*H*R) of air, twenty-two times the
 * vertical column — and a test can hold it to that and to its own numerical
 * integration, which a tonemapped frame cannot. */
float fly_render_air_probe(float H, float z0, float dz, float dist);

/* --- the planet's own geometry ---
 *
 * Where a ray meets the ball and which way the surface faces there, or a
 * negative distance if the ray is above the horizon. Exposed so "the world
 * ends where the curve says it does" can be a measurement against
 * sqrt(2Rh + h^2) rather than a look at a screenshot. */
float fly_render_planet_probe(fly_v3 ro, fly_v3 rd, fly_v3 *normal);

/* --- ambient lighting test hooks ---
 *
 * `irr` is what the sky alone puts on a surface facing `n`, per unit albedo,
 * seen from `ro`; `ground` is what the ground under `ro` sends back up, form
 * factor already folded in. Public because the ambient half of the lighting is
 * now two integrals — the sky's own scattering model above and the terrain's
 * own albedo below — and the way to check they agree with what is drawn is to
 * ask them rather than to read a tonemapped frame. Either may be NULL. */
int fly_render_irradiance_probe(const fly_game *g, fly_v3 ro, fly_v3 n,
                                fly_v3 *irr, fly_v3 *ground);
/* The same quantity by brute force: the cosine-weighted hemisphere integral of
 * the sky over four thousand directions, with no harmonic fit in between. Slow
 * and exact, so a test can hold the fit to it. */
int fly_render_irradiance_exact(const fly_game *g, fly_v3 ro, fly_v3 n, fly_v3 *irr);

/* --- surface relief test hook ---
 *
 * What `terrain_detail` does to the shading normal and the albedo at a point,
 * for a material the caller names: `n` goes in as the geometric normal and
 * comes back perturbed by the hummock and tussock relief, `tint` is the
 * multiplier the blade-scale octave applies to the colour. Either may be NULL.
 *
 * Exposed because the relief is a bump and not geometry, so nothing that
 * traces or rasterizes the world can be asked whether it is there — and the
 * claim worth holding is a comparison between two materials at the same point
 * rather than an appearance. Standing water is level whatever is drowned under
 * it, and a puddle with hummocks embossed on it is the one thing that gives a
 * wet meadow away instantly. `px_per_m` selects how much of the relief is
 * resolvable, exactly as it does for the albedo's octaves; pass something huge
 * for all of it. Returns 0, or negative if there is no game. */
int fly_render_detail_probe(const fly_game *g, float x, float y, fly_v3 mat,
                            float px_per_m, fly_v3 *n, fly_v3 *tint);

/* --- cinematography test hooks ---
 * The per-biome grade this frame would apply (per-channel gain and a
 * saturation multiplier), and the regional grass field it reads. */
int fly_render_grade_probe(const fly_game *g, const fly_cam *cam, fly_v3 *grade, float *sat);
float fly_render_grass_region_probe(const fly_game *g, float x, float y);

/* --- what is actually on the ground ---
 *
 * The height the terrain is *drawn* at, which is not `fly_world_ground`: the
 * mesh is bilinear between vertices up to a cell apart, so over a concave patch
 * the surface that gets rasterised stands above the height function by the
 * better part of a metre. Anything laid on the ground has to clear that, and
 * everything here does — which is a claim about geometry rather than about
 * pixels, so it is a claim worth testing rather than eyeballing.
 *
 * And the deck, at `t` of the way along span `span` of road `road`: the
 * surveyed profile, lifted where the drawn terrain would otherwise come up
 * through the carriageway. `centre` receives the crown and `across` a unit
 * vector to the left of travel, so a caller can find the deck edges the same
 * way the drawing does — they are a camber below the crown, half a
 * carriageway out. Either may be NULL; returns 0 when there is no such span. */
float fly_render_ground_drawn(const fly_world *w, float x, float y);
int fly_render_road_deck_probe(const fly_game *g, int road, int span, float t,
                               fly_v3 *centre, fly_v3 *across);

/* Where the k-th lamp on a line stands, and how far along the line that is.
 *
 * `line` is a road index, or -1 for the guideway. Returns 0 once k is past the
 * end of it, so a caller walks k upward until it stops.
 *
 * The claim it exists to let a test hold is that the light on a way is *evenly
 * spread along it*: a fixed pitch of carriageway or of guideway, alternating
 * verges, from one terminus to the other. It answers the placement and not what
 * a frame does with it — the drawing thins the chain with distance and leaves
 * out the columns that would stand inside a crossing's structure, which the
 * structure lights itself. Nothing about the spacing is visible in a frame — a
 * night shot of a road four kilometres off is a line of dots either way — and
 * a lamp that crawled along the verge with the camera, or a chain that stopped
 * at the range the columns stop being drawn at, would look perfectly fine in
 * every still and be wrong in motion. This is the drawing's own answer, out of
 * the same function the columns are placed by, so the two cannot drift. */
int fly_render_lamp_probe(const fly_game *g, int line, int k,
                          fly_v3 *pos, float *chain);
/* And where the k-th mast of the transmission line beside road `road` stands.
 *
 * The same shape as the lamp probe and for the same reason: the run is placed
 * by chainage from one function, and a test that re-derived the pitch would be
 * checking its own arithmetic rather than the line. Returns 0 once k is past
 * the end of the road, so a caller walks k upward until it stops.
 *
 * `stands` receives 0 where the placement lands on ground a mast may not be
 * built on — in water, inside a settlement's own clearing, inside the
 * structure carrying the guideway over the carriageway, or inside any road's
 * right of way — which at a junction is another carriageway and at a terminus
 * is the apron a road arrives through. Those are holes in the
 * chain rather than towers shoved sideways, and the span either side of one
 * simply gets longer, so the placement is still on an even pitch end to end
 * and that is what a test can hold it to. Any of the three may be NULL. */
int fly_render_pylon_probe(const fly_game *g, int road, int k, fly_v3 *pos,
                           float *chain, int *stands);
/* And the plain height as the *scatter* reads it, which goes through a memo of
 * its own for the same reason: a tree, a boulder and a tuft of grass are placed
 * deterministically per world cell, so a cell in range asks for exactly the
 * coordinates it asked for last frame. Exposed so a test can hold the thing
 * that makes a memo safe — that it is the function it stands in for, bit for
 * bit — rather than inferring it from a frame that looks unchanged. */
float fly_render_ground_scatter(const fly_world *w, float x, float y);

/* --- settlement layout ---
 *
 * Half-width of the rail's right of way, the clearing kept at a terminus, and
 * the furthest a surveyed plot may be nudged to find dry ground before it stops
 * being that plot. */
#define FLY_RAIL_ROW 34.0f
#define FLY_RAIL_YARD 95.0f
#define FLY_SITE_WALK 40.0f
/* How far apart the line's piers stand.
 *
 * Not the survey's business: `fly_rail_lay` samples the *plan* every 120 m,
 * which is the resolution a curve needs and has nothing to do with how far a
 * girder will span. A bent every thirty metres is what carries the deck, and
 * drawing one only at each surveyed station left a hundred and twenty metres
 * of thirteen-metre-high pipe standing on nothing between them. */
#define FLY_RAIL_BENT 30.0f
/* How far past its terminus a road ramps its deck down to grade.
 *
 * Shared because two things have to agree about it: `road_ramp` draws the
 * ramp, and the settlement's gate starts its run-in at the toe of it. Start
 * the run-in at the terminus instead and the two surfaces cross somewhere in
 * the middle of the ramp, which is a z-fighting band across the one piece of
 * ground every arrival by road is looking at. */
#define FLY_ROAD_RAMP 13.0f
/* how far below the lowest point of its own footprint a piece is seated, so
 * that a finite set of boundary samples cannot leave a corner in the air */
#define FLY_SITE_BURY 0.25f

/* Where a piece of a settlement actually stands, or 0 if it should not be built
 * at all. `base` is the site centre; `p` carries the surveyed plot in and the
 * footing out. Every settlement kind's architecture goes through this.
 *
 * Public because it is the layout rule, not a drawing detail, and the rule is
 * what wants a test: the failure it prevents is a tower standing in the sea or
 * squatting in the middle of the rail's arrival yard, neither of which the
 * renderer can notice and both of which draw perfectly happily. */
/* `radius` circumscribes the piece's footprint; it is seated on the lowest
 * ground under that circle so no corner is left hanging. Pass 0 for a piece
 * with no footprint to speak of. */
int fly_site_footing(const fly_game *g, fly_v3 base, float radius, fly_v3 *p);

/* --- a farm in the country ------------------------------------------------
 *
 * The lattice the worked country's steadings are placed on, and one cell's
 * answer. See fly_steading_at, which is where the placement rules live and the
 * only place they are written down: the pass that draws farms and the aspect
 * that measures where they may stand read the same function.
 *
 * `yaw` is square to the fields the farm works, `hash` is what its buildings
 * are drawn from, and `big` is the hamlet version. */
#define FLY_STEADING_CELL 88.0f
/* How far a steading keeps off a carriageway, and — plus a little, because a
 * guideway is on piers and casts further than it stands — off the rail. Wide
 * enough for the yard and the verge, narrow enough that a farm still reads as
 * belonging to the road it is on. */
#define FLY_STEADING_VERGE 40.0f
/* Half the yard, in its own frame: the fence, and therefore the footprint
 * every footing rule is asked of. FLY_STEADING_VERGE clears the carriageway by
 * the diagonal of this plus FLY_ROAD_CLEAR, so the corner of a yard is off the
 * road and not only its gate. */
#define FLY_STEADING_YARD 16.0f
/* How far a farm will reach for the road: past this the lane in would want a
 * survey of its own, which is a road, and the network already knows how to lay
 * one. */
#define FLY_STEADING_LANE 240.0f

typedef struct {
    fly_wpos pos;
    float ground;   /* the height it stands on */
    float yaw;      /* square to its own fields */
    uint32_t hash;  /* what its buildings are drawn from */
    int big;        /* a hamlet's range of buildings rather than one farm */
} fly_steading;

int fly_steading_at(const fly_game *g, int ix, int iy, fly_steading *out);
/* Where a site's banner mast stands, in world space, or 0 if that site has no
 * dry ground to put one on. Public for the same reason the footing rule is:
 * the offset and the site's own axis are layout, and a caller that wants to
 * point a camera at the flag should not have to guess where the renderer put
 * it — the gallery did, and framed the buildings instead. */
int fly_site_banner(const fly_game *g, int loc, fly_v3 *out);
/* The bearing a settlement is laid out on: where its infrastructure arrives
 * from, which is the rail's approach if it has one and its road's otherwise.
 * Public for the same reason the banner is — a caller that wants to look at a
 * settlement has to be able to ask which way round it was built, or it frames
 * the back of it. */
float fly_site_axis(const fly_game *g, int loc);
/* The sunbeam at `ro`, measured across the beam: what a surface held square to
 * the sun receives, before its own cosine. Zero once the sun is down. */
int fly_render_sun_beam(const fly_game *g, fly_v3 ro, fly_v3 *beam);
/* Throw away everything the occlusion cache has learned. Only tests need this:
   the cache is world-space and meant to outlive any one camera. */
void fly_render_ao_reset(void);
/* Rays the last frame actually traced, which is how the budget is measured. */
long fly_render_ao_rays(void);
/* Traced sky visibility over one patch of ground, filled to `target` samples.
   `agl` is the height above that ground the answer is wanted at: 0 reads what
   the floor sees, FLY_AO_HI (a canopy's height) what clears the wood, and
   between the two it interpolates — which is the difference between shading a
   crown and shading the ground it grew out of. */
int fly_render_ao_probe(const fly_game *g, fly_v3 p, float agl, int target, float *vis);

/* How high a settlement's built geometry stands at (x, y) — absolute, not above
 * ground — or -1e30 where nothing is built there. This is the occluder the
 * traced term marches, captured by drawing each site as a caster, so a test
 * asking "is this the foot of a building" gets the renderer's own answer
 * rather than a second opinion about where the layout puts things. */
float fly_render_built_top(const fly_game *g, float x, float y);

#endif /* FLY_RENDER_H */
