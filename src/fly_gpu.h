/* fly_gpu: optional OpenGL(ES)/GLSL acceleration service.
 *
 * A thin, self-contained GPU service: it owns a headless (surfaceless EGL)
 * GL context, compiles fullscreen fragment programs, and reads rendered
 * frames back into a fly_img. It deliberately knows nothing about the game
 * or the scene — fly_render owns the shader source and the uniforms, this
 * module only runs them — so the layering stays one-directional.
 *
 * The whole module is optional and never fatal:
 *   - built without FLY_GPU it compiles to stubs that report "unavailable";
 *   - built with FLY_GPU it still reports "unavailable" at runtime if no EGL
 *     device, context or program can be created.
 * Callers must treat an unavailable GPU as "use the CPU renderer", so a
 * machine with no GL driver behaves exactly as it did before. */
#ifndef FLY_GPU_H
#define FLY_GPU_H

#include "fly_img.h"

/* opaque compiled fullscreen program */
typedef struct fly_gpu_prog fly_gpu_prog;

/* Bring the context up (idempotent). Returns 0 when a usable context exists,
 * negative when the GPU path is unavailable and the caller must fall back. */
int fly_gpu_init(void);
void fly_gpu_shutdown(void);
/* 1 once fly_gpu_init has succeeded, else 0 */
int fly_gpu_available(void);
/* GL_RENDERER string of the live context, or NULL when unavailable */
const char *fly_gpu_renderer(void);

/* Compile (and cache) a fullscreen fragment program. `frag_src` must be a
 * stable string — its pointer is the cache key, so pass a string literal.
 * Returns NULL if the GPU is unavailable or the shader failed to build;
 * on failure fly_gpu_error() describes why. */
fly_gpu_prog *fly_gpu_program(const char *frag_src);
/* last shader/context error message ("" when none) */
const char *fly_gpu_error(void);

/* uniform setters; unknown names are ignored so shaders can drop inputs */
void fly_gpu_set1i(fly_gpu_prog *p, const char *name, int v);
void fly_gpu_set1f(fly_gpu_prog *p, const char *name, float v);
void fly_gpu_set2f(fly_gpu_prog *p, const char *name, float x, float y);
void fly_gpu_set2i(fly_gpu_prog *p, const char *name, int x, int y);
void fly_gpu_set3f(fly_gpu_prog *p, const char *name, float x, float y, float z);
void fly_gpu_set4f(fly_gpu_prog *p, const char *name, float x, float y, float z, float w);
void fly_gpu_set3i(fly_gpu_prog *p, const char *name, int x, int y, int z);
/* upload an array of vec4s (e.g. per-location pad data) */
void fly_gpu_set3fv(fly_gpu_prog *p, const char *name, const float *v, int count);
void fly_gpu_set4fv(fly_gpu_prog *p, const char *name, const float *v, int count);

/* Render `p` over out->w x out->h and read the result back into `out`.
 * Returns 0 on success, negative on failure (caller should fall back). */
int fly_gpu_draw(fly_gpu_prog *p, fly_img *out);

/* Render `p` at w x h into a float (RGBA16F) target and read all four channels
 * back into `rgba` (4*w*h floats, row 0 = top). Unlike fly_gpu_draw this keeps
 * values outside 0..1, so a shader can return HDR radiance and pack extra data
 * (e.g. view depth) in alpha instead of being clamped to 8 bits per channel.
 * Note the target is RGBA16F, so readback carries half-float precision: fine
 * near 0..1, but spacing widens to ~2 around 3000 and ~16 around 26000. Values
 * needing more than that must be scaled or split across channels. */
int fly_gpu_draw_hdr(fly_gpu_prog *p, float *rgba, int w, int h);

/* Upload a single-channel float image and bind it to `name` on texture unit
 * `unit`. Nearest filtering, clamped edges — this is for data a shader must
 * read exactly (the sun shadow-map cascades), not for artwork.
 * Returns 0 on success, negative on failure. */
int fly_gpu_set_texture(fly_gpu_prog *p, const char *name, int unit,
                        const float *data, int w, int h);

/* ---------------- mesh pipeline ----------------
 *
 * Real geometry rendering: vertex/index buffers drawn with hardware depth
 * testing into the float target. This is what the GPU rasterizer needs —
 * raymarching a fullscreen quad was measured ~40x slower than rasterizing the
 * same scene's triangles, because rasterization is cheap by construction.
 *
 * Vertices are interleaved floats; the program declares its own attribute
 * layout, and `floats_per_vert` is the stride. Attribute i is bound to
 * location i with `attr_sizes[i]` components. */
typedef struct fly_gpu_mesh fly_gpu_mesh;

fly_gpu_mesh *fly_gpu_mesh_create(void);
void fly_gpu_mesh_free(fly_gpu_mesh *m);
/* (re)upload geometry; safe to call every frame — the buffers are reused when
 * the size is unchanged. Returns 0 on success. */
int fly_gpu_mesh_upload(fly_gpu_mesh *m, const float *verts, int nverts,
                        int floats_per_vert, const int *attr_sizes, int nattrs,
                        const uint32_t *indices, int nindices);

/* Per-instance attributes, for drawing one uploaded mesh many times.
 *
 * The same interleaved-floats contract as the vertex buffer, one record per
 * instance, bound to locations `first_loc`, `first_loc + 1`, ... with a divisor
 * of one. `first_loc` is the caller's to state because the program declares the
 * layout; it has to be past the attributes fly_gpu_mesh_upload bound. The
 * template is uploaded once and only the small per-instance record moves after
 * that, which is the whole point. Returns 0 on success. */
int fly_gpu_mesh_instances(fly_gpu_mesh *m, const float *data, int ninstances,
                           int floats_per_instance, const int *attr_sizes,
                           int nattrs, int first_loc);

/* Compile a vertex+fragment program. Cached on `frag_src`'s pointer like
 * fly_gpu_program, which it complements (that one supplies its own fullscreen
 * vertex shader; this one takes yours). */
fly_gpu_prog *fly_gpu_program_mesh(const char *vert_src, const char *frag_src);

/* Depth-tested pass into the float target: begin, draw meshes, end (which
 * reads 4*w*h floats back into `rgba`). Colour clears to `clear_rgba` and
 * depth to 1. `flip` reorders the readback into fly_img's top-down rows; pass
 * 0 to keep GL's bottom-up order when the consumer can index it directly,
 * which saves a second pass over the whole buffer. Returns 0 on success.
 *
 * `samples` > 1 asks for multisampling: coverage and depth are stored per
 * sample while the fragment shader still runs once per pixel, so geometry
 * edges are antialiased for a fraction of what supersampling costs. It is
 * clamped to the implementation's GL_MAX_SAMPLES and silently falls back to 1
 * if a multisample target cannot be made. Note that the resolve averages all
 * four channels, so data packed in alpha — view depth here — is blended across
 * silhouettes rather than picked from one sample. */
int fly_gpu_pass_begin(int w, int h, const float *clear_rgba, int samples);
/* how many samples the last pass actually used (1 when not multisampling) */
int fly_gpu_pass_samples(void);
int fly_gpu_mesh_draw(fly_gpu_prog *p, fly_gpu_mesh *m);
/* the same draw, `ninstances` times, against the instance buffer above */
int fly_gpu_mesh_draw_instanced(fly_gpu_prog *p, fly_gpu_mesh *m, int ninstances);
int fly_gpu_pass_end(float *rgba, int w, int h, int flip);

/* ---------------- the resident frame ----------------
 *
 * The same pass, ended without a readback: the frame stays in GL memory and
 * every later stage runs as another GL pass over it. That is the whole reason
 * this exists. A frame that comes back to the CPU to be corrected, composited
 * and tonemapped crosses the bus three times — down as float radiance, up
 * again for the post chain, down once more as pixels — and at 1080p with 2x
 * supersampling that is a quarter of a gigabyte a frame, which is more than
 * PCIe delivers at sixty of them whatever the GPU is. Left resident, the only
 * thing that crosses is the finished 8-bit image.
 *
 * There are two float targets and one rule between them: a pass writes the
 * *front* one and samples the *readable* one, and fly_gpu_frame_swap publishes
 * what was just written so the next pass can read it. A stage that rewrites
 * the frame therefore never reads the texture it is writing, which is the one
 * thing GL does not define.
 *
 *   fly_gpu_pass_begin(...);  ... mesh draws ...;  fly_gpu_frame_end();
 *   fly_gpu_frame_swap();                       // publish, write the other
 *   fly_gpu_frame_bind(p, "uFrame", 3);         // sample what was published
 *   fly_gpu_frame_draw(p);                      // fullscreen into the front
 *   fly_gpu_frame_overlay(FLY_GPU_BLEND_ADD);   // then mesh draws over it
 *   fly_gpu_frame_swap();                       // publish for the post chain
 */
enum { FLY_GPU_BLEND_OFF = 0, FLY_GPU_BLEND_OVER, FLY_GPU_BLEND_ADD };

/* End a mesh pass with the result left in GL memory (resolving multisampling
 * if the pass used it) and publish it for sampling. The twin of
 * fly_gpu_pass_end, minus the readback. */
int fly_gpu_frame_end(void);
/* Publish the target just written and start writing the other one. */
int fly_gpu_frame_swap(void);
/* Bind the published frame for sampling as `name` on `unit`. */
int fly_gpu_frame_bind(fly_gpu_prog *p, const char *name, int unit);
/* Point draws at the front target with `blend` compositing and no depth test,
 * for geometry laid over the frame already there (emissive splats, the
 * translucent pass). Draw with fly_gpu_mesh_draw; the frame's own view depth
 * is in its alpha, so an overlay that must be occluded tests against that
 * rather than against a depth buffer that multisampling would not have left
 * in a readable form. */
int fly_gpu_frame_overlay(int blend);
/* Run a fullscreen program over the whole front target (implies overlay OFF). */
int fly_gpu_frame_draw(fly_gpu_prog *p);
/* Read the published frame into `rgba` (4*w*h floats, GL's bottom-up row
 * order). The escape hatch for a caller that wants the linear buffer rather
 * than the picture — the renderer uses it only when it was asked to fill a
 * render target's HDR and depth planes. */
int fly_gpu_frame_read(float *rgba, int w, int h);

/* Bytes handed to GL and bytes taken back since this was last reset.
 *
 * The renderer's shape depends on this number staying small, and the number is
 * the only thing that says whether it did. A frame that comes back to the CPU
 * to be corrected, composited and tonemapped crosses the bus three times: at
 * 1080p with 2x supersampling that is 133 MB down as float radiance, 100 MB up
 * again for the post chain and 33 MB down as pixels — a quarter of a gigabyte
 * a frame, or 16 GB/s at sixty of them, which is more than PCIe 3.0 x16
 * delivers whatever the GPU is. Counted here, a test can hold "the picture is
 * the only thing that comes back" as an invariant instead of a hope, and the
 * next readback anyone adds shows up as a number rather than as a frame rate.
 *
 * Both pointers may be NULL; `reset` zeroes the counters after reading. */
void fly_gpu_traffic(double *up, double *down, int reset);

/* ---------------- offscreen float images ----------------
 *
 * A float image that a program can render into and a later program can sample.
 * That is what a multi-pass post chain needs — bright pass, separable blur,
 * composite — without a CPU round trip between the passes.
 *
 * Channel count picks the format: 1 = R32F, 3 = RGB32F, 4 = RGBA32F. Only 4
 * can be rendered into (the others are not colour-renderable), so a source
 * image uploaded from the CPU may use any of them but a pass target must be 4.
 * Sampling is always nearest and clamped: these carry data, not artwork. */
typedef struct fly_gpu_img fly_gpu_img;

fly_gpu_img *fly_gpu_img_create(void);
void fly_gpu_img_free(fly_gpu_img *im);
/* (Re)size to w x h with `nc` channels (1..4, stored as the matching 32-bit
 * float format), filling from `px` (w*h*nc floats, row 0 first) or leaving the
 * contents undefined when `px` is NULL. Returns 0 on success. */
int fly_gpu_img_set(fly_gpu_img *im, int w, int h, const float *px, int nc);
/* bind for sampling as `name` on texture `unit` (0..3) */
int fly_gpu_img_bind(fly_gpu_prog *p, const char *name, int unit, const fly_gpu_img *im);
/* Read the image back into `px` (w*h*nc floats, GL's bottom-up row order —
 * the same layout fly_gpu_img_set uploads). The renderer never needs this;
 * it exists so a test can check what a GPU-resident buffer actually holds
 * against the CPU reference that produced the same thing. */
int fly_gpu_img_read(const fly_gpu_img *im, float *px);
/* run a fullscreen program over `im`, leaving the result there (no readback) */
int fly_gpu_img_draw(fly_gpu_prog *p, fly_gpu_img *im);

/* Depth-tested mesh pass into an offscreen float image, left resident for a
 * later pass to sample — the same begin/draw/end shape as fly_gpu_pass_*, but
 * with no readback, which is the whole point: data a shader produces for
 * another shader (the sun shadow cascades) never needs to touch the CPU.
 * `im` is (re)sized to w x h with `nc` channels; the format must be
 * colour-renderable, and while R32F (nc = 1) is on every implementation that
 * advertises float rendering, a driver that refuses it reports the framebuffer
 * as incomplete and the caller can retry with nc = 4. Depth clears to 1 and
 * the test is GL_LESS, so the surviving texel is the nearest fragment's.
 * Draw with fly_gpu_mesh_draw between the two. */
int fly_gpu_pass_begin_img(fly_gpu_img *im, int w, int h, int nc, const float *clear_rgba);
int fly_gpu_pass_end_img(void);

#endif /* FLY_GPU_H */
