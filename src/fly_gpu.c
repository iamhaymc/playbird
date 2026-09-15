#include "fly_gpu.h"

#include <stdio.h>
#include <string.h>

/* ============================ disabled build ============================ */
#ifndef FLY_GPU

int fly_gpu_init(void) { return -1; }
void fly_gpu_shutdown(void) {}
int fly_gpu_available(void) { return 0; }
const char *fly_gpu_renderer(void) { return NULL; }
fly_gpu_prog *fly_gpu_program(const char *frag_src) { (void)frag_src; return NULL; }
const char *fly_gpu_error(void) { return "built without FLY_GPU"; }
void fly_gpu_set1i(fly_gpu_prog *p, const char *n, int v) { (void)p; (void)n; (void)v; }
void fly_gpu_set1f(fly_gpu_prog *p, const char *n, float v) { (void)p; (void)n; (void)v; }
void fly_gpu_set2f(fly_gpu_prog *p, const char *n, float x, float y) {
    (void)p; (void)n; (void)x; (void)y;
}
void fly_gpu_set3i(fly_gpu_prog *p, const char *n, int x, int y, int z) {
    (void)p; (void)n; (void)x; (void)y; (void)z;
}
void fly_gpu_set2i(fly_gpu_prog *p, const char *n, int x, int y) {
    (void)p; (void)n; (void)x; (void)y;
}
void fly_gpu_set3f(fly_gpu_prog *p, const char *n, float x, float y, float z) {
    (void)p; (void)n; (void)x; (void)y; (void)z;
}
void fly_gpu_set4f(fly_gpu_prog *p, const char *n, float x, float y, float z, float w) {
    (void)p; (void)n; (void)x; (void)y; (void)z; (void)w;
}
void fly_gpu_set3fv(fly_gpu_prog *p, const char *n, const float *v, int c) {
    (void)p; (void)n; (void)v; (void)c;
}
void fly_gpu_set4fv(fly_gpu_prog *p, const char *n, const float *v, int c) {
    (void)p; (void)n; (void)v; (void)c;
}
int fly_gpu_draw(fly_gpu_prog *p, fly_img *out) { (void)p; (void)out; return -1; }
int fly_gpu_draw_hdr(fly_gpu_prog *p, float *rgba, int w, int h) {
    (void)p; (void)rgba; (void)w; (void)h; return -1;
}
int fly_gpu_set_texture(fly_gpu_prog *p, const char *n, int u, const float *d, int w, int h) {
    (void)p; (void)n; (void)u; (void)d; (void)w; (void)h; return -1;
}
fly_gpu_mesh *fly_gpu_mesh_create(void) { return NULL; }
void fly_gpu_mesh_free(fly_gpu_mesh *m) { (void)m; }
int fly_gpu_mesh_upload(fly_gpu_mesh *m, const float *v, int nv, int fpv,
                        const int *as, int na, const uint32_t *ix, int ni) {
    (void)m; (void)v; (void)nv; (void)fpv; (void)as; (void)na; (void)ix; (void)ni; return -1;
}
int fly_gpu_mesh_instances(fly_gpu_mesh *m, const float *d, int ni, int fpi,
                           const int *as, int na, int loc) {
    (void)m; (void)d; (void)ni; (void)fpi; (void)as; (void)na; (void)loc; return -1;
}
fly_gpu_prog *fly_gpu_program_mesh(const char *vs, const char *fs) {
    (void)vs; (void)fs; return NULL;
}
int fly_gpu_pass_begin(int w, int h, const float *c, int s) {
    (void)w; (void)h; (void)c; (void)s; return -1;
}
int fly_gpu_pass_samples(void) { return 1; }
int fly_gpu_mesh_draw(fly_gpu_prog *p, fly_gpu_mesh *m) { (void)p; (void)m; return -1; }
int fly_gpu_mesh_draw_instanced(fly_gpu_prog *p, fly_gpu_mesh *m, int n) {
    (void)p; (void)m; (void)n; return -1;
}
int fly_gpu_pass_end(float *rgba, int w, int h, int flip) {
    (void)rgba; (void)w; (void)h; (void)flip; return -1;
}
int fly_gpu_frame_end(void) { return -1; }
int fly_gpu_frame_swap(void) { return -1; }
int fly_gpu_frame_bind(fly_gpu_prog *p, const char *n, int u) {
    (void)p; (void)n; (void)u; return -1;
}
int fly_gpu_frame_overlay(int blend) { (void)blend; return -1; }
int fly_gpu_frame_draw(fly_gpu_prog *p) { (void)p; return -1; }
int fly_gpu_frame_read(float *rgba, int w, int h) {
    (void)rgba; (void)w; (void)h; return -1;
}
void fly_gpu_traffic(double *up, double *down, int reset) {
    (void)reset;
    if (up) *up = 0.0;
    if (down) *down = 0.0;
}
fly_gpu_img *fly_gpu_img_create(void) { return NULL; }
void fly_gpu_img_free(fly_gpu_img *im) { (void)im; }
int fly_gpu_img_set(fly_gpu_img *im, int w, int h, const float *px, int nc) {
    (void)im; (void)w; (void)h; (void)px; (void)nc; return -1;
}
int fly_gpu_img_bind(fly_gpu_prog *p, const char *n, int u, const fly_gpu_img *im) {
    (void)p; (void)n; (void)u; (void)im; return -1;
}
int fly_gpu_img_draw(fly_gpu_prog *p, fly_gpu_img *im) { (void)p; (void)im; return -1; }
int fly_gpu_img_read(const fly_gpu_img *im, float *px) { (void)im; (void)px; return -1; }
int fly_gpu_pass_begin_img(fly_gpu_img *im, int w, int h, int nc, const float *c) {
    (void)im; (void)w; (void)h; (void)nc; (void)c; return -1;
}
int fly_gpu_pass_end_img(void) { return -1; }

#else /* ======================== enabled build ======================== */

#include <stdlib.h>

/* ---------------- platform GL ----------------
 *
 * Everything below this block is written once, against GL ES 3.1 semantics.
 * The platform only decides where a headless context and the entry points to
 * drive it come from.
 *
 * POSIX has EGL, so the system's EGL/GLES headers declare both.
 *
 * Windows has no EGL. The context is a WGL one on a hidden window, and every
 * entry point past OpenGL 1.1 is loaded through wglGetProcAddress into the
 * table below — which this file declares itself rather than including a GL
 * SDK. Both halves of that are deliberate:
 *   - nothing has to be installed: opengl32.dll ships with Windows, while the
 *     toolchains setup.ps1 installs carry no GL headers to rely on;
 *   - every name here has internal linkage, so the windowed build links.
 *     TIGR's Windows backend defines *global* gl* function pointers of its
 *     own and a GLES import library exports the same names as import stubs,
 *     so the two collide at link time — the reason this platform used to be
 *     forced onto the CPU renderer. A file-scope static collides with nothing.
 * The shaders stay ES 3.1 on both: desktop drivers compile that dialect
 * through ARB_ES3_1_compatibility, and fly_gpu_init proves it before it
 * reports a GPU (see the probe there). */
#ifdef _WIN32
#include <stddef.h>
#include <windows.h>
/* windef.h defines `far`, `near`, `FAR` and `NEAR` as empty macros, and the
 * amalgamation compiles this file together with fly_render.c, which uses all
 * four as identifiers. Undone here for the same reason it is undone there. */
#undef far
#undef near
#undef FAR
#undef NEAR

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

/* the enumerants this module names, at their fixed values from the registry */
#define GL_FALSE 0
#define GL_NO_ERROR 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRIANGLES 0x0004
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RENDERER 0x1F01
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA8 0x8058
#define GL_SAMPLES 0x80A9
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_RG 0x8227
#define GL_R32F 0x822E
#define GL_RG32F 0x8230
#define GL_TEXTURE0 0x84C0
#define GL_RGBA32F 0x8814
#define GL_RGB32F 0x8815
#define GL_RGBA16F 0x881A
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_MAX_SAMPLES 0x8D57

/* wglCreateContextAttribsARB's attribute names (WGL_ARB_create_context) */
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

/* Every GL entry point this module calls, in one list. The declarations, the
 * loader and the "which one is missing" message all come out of it, so a call
 * added downstream is a build error here rather than a null call at runtime. */
#define FLY_GL_PROCS(X)                                                                        \
    X(void, glActiveTexture, (GLenum texture))                                                 \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                   \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                      \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                            \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                          \
    X(void, glBindTexture, (GLenum target, GLuint texture))                                    \
    X(void, glBindVertexArray, (GLuint array))                                                 \
    X(void, glBlendFuncSeparate, (GLenum srgb, GLenum drgb, GLenum sa, GLenum da))             \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0,         \
                                GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,              \
                                GLenum filter))                                                \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))    \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size,                 \
                              const void *data))                                               \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                       \
    X(void, glClear, (GLbitfield mask))                                                        \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                        \
    X(void, glClearDepthf, (GLfloat d))                                                        \
    X(void, glCompileShader, (GLuint shader))                                                  \
    X(GLuint, glCreateProgram, (void))                                                         \
    X(GLuint, glCreateShader, (GLenum type))                                                   \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers))                               \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))                     \
    X(void, glDeleteProgram, (GLuint program))                                                 \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers))                   \
    X(void, glDeleteShader, (GLuint shader))                                                   \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures))                             \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays))                           \
    X(void, glDepthFunc, (GLenum func))                                                        \
    X(void, glDisable, (GLenum cap))                                                           \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                           \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices))    \
    X(void, glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type,                 \
                                      const void *indices, GLsizei ninstances))                \
    X(void, glEnable, (GLenum cap))                                                            \
    X(void, glEnableVertexAttribArray, (GLuint index))                                         \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget,     \
                                        GLuint renderbuffer))                                  \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget,       \
                                     GLuint texture, GLint level))                             \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers))                                        \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers))                              \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers))                            \
    X(void, glGenTextures, (GLsizei n, GLuint *textures))                                      \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays))                                    \
    X(GLenum, glGetError, (void))                                                              \
    X(void, glGetIntegerv, (GLenum pname, GLint *data))                                        \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufsz, GLsizei *len, GLchar *log))   \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params))                     \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufsz, GLsizei *len, GLchar *log))     \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))                       \
    X(const GLubyte *, glGetString, (GLenum name))                                             \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))                       \
    X(void, glLinkProgram, (GLuint program))                                                   \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                        \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,              \
                           GLenum type, void *pixels))                                         \
    X(void, glRenderbufferStorage, (GLenum target, GLenum fmt, GLsizei w, GLsizei h))          \
    X(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum fmt,     \
                                               GLsizei w, GLsizei h))                          \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string,        \
                             const GLint *length))                                             \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                       \
    X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w,   \
                              GLsizei h, GLenum format, GLenum type, const void *pixels))      \
    X(void, glUniform1f, (GLint loc, GLfloat v0))                                              \
    X(void, glUniform1i, (GLint loc, GLint v0))                                                \
    X(void, glUniform2f, (GLint loc, GLfloat v0, GLfloat v1))                                  \
    X(void, glUniform2i, (GLint loc, GLint v0, GLint v1))                                      \
    X(void, glUniform3f, (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2))                      \
    X(void, glUniform3fv, (GLint loc, GLsizei count, const GLfloat *value))                    \
    X(void, glUniform3i, (GLint loc, GLint v0, GLint v1, GLint v2))                            \
    X(void, glUniform4f, (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))          \
    X(void, glUniform4fv, (GLint loc, GLsizei count, const GLfloat *value))                    \
    X(void, glUseProgram, (GLuint program))                                                    \
    X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                             \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type,                     \
                                    GLboolean normalized, GLsizei stride, const void *ptr))    \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))

#define FLY_GL_DECLARE(ret, name, params)      \
    typedef ret(APIENTRY *fly__pfn_##name) params; \
    static fly__pfn_##name name;
FLY_GL_PROCS(FLY_GL_DECLARE)
#undef FLY_GL_DECLARE

#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#endif

#define FLY_GPU_PROG_MAX 32

struct fly_gpu_prog {
    /* Both source pointers are the cache key. The fragment source alone is not
     * enough: two mesh programs may legitimately share a fragment shader and
     * differ only in their vertex stage (the shadow cascade writes the same
     * depth from a heightfield grid and from an occluder buffer), and keying on
     * it alone silently handed the second one the first one's program. */
    const char *key_vert, *key_frag;
    GLuint prog;
};

static struct {
    int tried;         /* init attempted? */
    int ready;         /* context live? */
#ifndef _WIN32
    EGLDisplay dpy;
    EGLContext ctx;
#endif
    GLuint vao;
    /* render target, grown on demand */
    GLuint fbo, tex;
    int tw, th;
    uint32_t *readback;
    size_t readback_px;
    struct fly_gpu_prog progs[FLY_GPU_PROG_MAX];
    int nprogs;
    char err[512];
} G;

/* Float render targets, grown on demand and kept apart from the RGBA8 one.
 * Two of them, so a pass that rewrites the frame writes one and samples the
 * other: `G_front` is the one being written, `G_read` the one published for
 * sampling. They are the same target until the first swap. */
static struct { GLuint fbo, tex; int w, h; } G_hdr[2];
static int G_front, G_read;

/* data textures bound to shaders (shadow cascades); one per unit */
#define FLY_GPU_TEX_MAX 4
static struct { GLuint tex; int w, h; } G_tex[FLY_GPU_TEX_MAX];

/* depth attachment for the float target, created alongside it on demand */
static GLuint G_depth_rb;
static int G_depth_w, G_depth_h;

/* depth attachment shared by every offscreen-image pass (the shadow cascades
 * are all one size, so one renderbuffer serves them) plus the image a pass is
 * currently rendering into */
static GLuint G_img_depth;
static int G_img_depth_w, G_img_depth_h;
static fly_gpu_img *G_img_pass;

/* multisample pass target, resolved into G_hdr at pass end */
static struct { GLuint fbo, color, depth; int w, h, want, samples; } G_ms;
static int G_pass_samples = 1; /* samples the last pass used (reporting) */
static int G_pass_ms = 0;      /* is a multisample buffer awaiting resolve? */

/* bytes over the host/GL boundary; see fly_gpu_traffic */
static double G_up, G_down;

void fly_gpu_traffic(double *up, double *down, int reset) {
    if (up) *up = G_up;
    if (down) *down = G_down;
    if (reset) G_up = G_down = 0.0;
}

static int gpu_fail(const char *msg) {
    snprintf(G.err, sizeof G.err, "%s", msg);
    return -1;
}

/* ---------------- context ----------------
 *
 * One init and one shutdown; the platform half is gpu_context_open /
 * gpu_context_close, and everything past the context is shared. */

#ifdef _WIN32

/* The hidden window the WGL context lives on. A GL context on Windows needs a
 * device context, and a device context needs a window — it is never shown, is
 * one pixel across, and is never drawn into: every frame goes to a
 * framebuffer object, exactly as it does on the surfaceless EGL path. */
#define FLY_GL_WNDCLASS "fly99_gl"

static struct {
    HMODULE lib; /* opengl32.dll, for the entry points wglGetProcAddress refuses */
    ATOM cls;    /* the window class, when this process registered it */
    HWND wnd;
    HDC dc;
    HGLRC rc;
} W;

static void gpu_context_close(void);

static void *wgl_proc(const char *name) {
    /* wglGetProcAddress serves everything past OpenGL 1.1 and answers the rest
     * with one of several falsy sentinels; those entry points are exported by
     * opengl32.dll itself. Casting between a code pointer and void * is the
     * shape both of these APIs are used in on Win32. */
    PROC p = wglGetProcAddress(name);
    if (p == NULL || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1)
        p = W.lib ? GetProcAddress(W.lib, name) : NULL;
    return (void *)p;
}

static int gpu_context_open(void) {
    PIXELFORMATDESCRIPTOR pfd;
    WNDCLASSA wc;
    HINSTANCE inst = GetModuleHandleA(NULL);
    HGLRC boot;
    const char *missing = NULL;
    int fmt;

    W.lib = LoadLibraryA("opengl32.dll");
    if (!W.lib) return gpu_fail("no opengl32.dll");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = FLY_GL_WNDCLASS;
    W.cls = RegisterClassA(&wc);
    if (!W.cls && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        gpu_context_close();
        return gpu_fail("RegisterClass failed");
    }
    W.wnd = CreateWindowExA(0, FLY_GL_WNDCLASS, "fly99", WS_OVERLAPPED, 0, 0, 1, 1,
                            NULL, NULL, inst, NULL);
    if (!W.wnd) {
        gpu_context_close();
        return gpu_fail("CreateWindow failed");
    }
    W.dc = GetDC(W.wnd);
    if (!W.dc) {
        gpu_context_close();
        return gpu_fail("GetDC failed");
    }

    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    /* Double-buffered although nothing is ever presented: an accelerated
     * pixel format is what this is really asking for, and a single-buffered
     * request is the one some drivers answer with the GDI software format —
     * which is OpenGL 1.1, and would lose the GPU on a machine that has one. */
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    fmt = ChoosePixelFormat(W.dc, &pfd);
    if (!fmt || !SetPixelFormat(W.dc, fmt, &pfd)) {
        gpu_context_close();
        return gpu_fail("no OpenGL pixel format");
    }

    /* A context has to exist before wglGetProcAddress answers anything, so the
     * modern one is created through an entry point only the old one can find. */
    boot = wglCreateContext(W.dc);
    if (!boot || !wglMakeCurrent(W.dc, boot)) {
        if (boot) wglDeleteContext(boot);
        gpu_context_close();
        return gpu_fail("wglCreateContext failed");
    }
    {
        typedef HGLRC(WINAPI * wgl_create_attribs)(HDC, HGLRC, const int *);
        wgl_create_attribs create =
            (wgl_create_attribs)wgl_proc("wglCreateContextAttribsARB");
        /* 4.5 has ES 3.1 shader compatibility in core; 4.3 is the floor this
         * module's own feature set sits on, and reaches the same dialect
         * through ARB_ES3_1_compatibility. Ask for the newest first. */
        static const int VERSION[][2] = { { 4, 5 }, { 4, 3 } };
        size_t i;
        for (i = 0; create && i < sizeof VERSION / sizeof *VERSION; ++i) {
            int attr[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 0,
                           WGL_CONTEXT_MINOR_VERSION_ARB, 0,
                           WGL_CONTEXT_PROFILE_MASK_ARB,
                           WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                           0 };
            HGLRC rc;
            attr[1] = VERSION[i][0];
            attr[3] = VERSION[i][1];
            rc = create(W.dc, NULL, attr);
            if (!rc) continue;
            if (wglMakeCurrent(W.dc, rc)) {
                W.rc = rc;
                break;
            }
            wglDeleteContext(rc);
        }
    }
    if (W.rc) {
        wglDeleteContext(boot);
    } else {
        /* No wglCreateContextAttribsARB, or no core profile: keep the context
         * that is already current. On a real driver that is the compatibility
         * profile of its newest version, which serves; on the GDI software
         * renderer it is OpenGL 1.1, and the load below turns it down. */
        W.rc = boot;
    }

#define FLY_GL_LOAD(ret, name, params)                    \
    name = (fly__pfn_##name)wgl_proc(#name);              \
    if (!name) missing = #name;
    FLY_GL_PROCS(FLY_GL_LOAD)
#undef FLY_GL_LOAD
    if (missing) {
        char msg[160];
        snprintf(msg, sizeof msg, "driver has no %s (OpenGL 4.3+ needed)", missing);
        gpu_context_close();
        return gpu_fail(msg);
    }
    return 0;
}

static void gpu_context_bind(void) {
    if (wglGetCurrentContext() != W.rc) wglMakeCurrent(W.dc, W.rc);
}

static void gpu_context_close(void) {
    if (W.rc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(W.rc);
    }
    if (W.dc) ReleaseDC(W.wnd, W.dc);
    if (W.wnd) DestroyWindow(W.wnd);
    if (W.cls) UnregisterClassA(FLY_GL_WNDCLASS, GetModuleHandleA(NULL));
    if (W.lib) FreeLibrary(W.lib);
    memset(&W, 0, sizeof W);
#define FLY_GL_CLEAR(ret, name, params) name = NULL;
    FLY_GL_PROCS(FLY_GL_CLEAR)
#undef FLY_GL_CLEAR
}

#else /* ---- POSIX: surfaceless EGL ---- */

static int gpu_context_open(void) {
    /* Prefer Mesa's surfaceless platform so this works with no X11/Wayland
     * display at all (headless CI, containers); fall back to the default
     * display when the extension is absent. */
    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return gpu_fail("no EGL display");
    if (!eglInitialize(dpy, NULL, NULL)) return gpu_fail("eglInitialize failed");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        eglTerminate(dpy);
        return gpu_fail("eglBindAPI(ES) failed");
    }

    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                          EGL_NONE };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        eglTerminate(dpy);
        return gpu_fail("no ES3 EGL config");
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        eglTerminate(dpy);
        return gpu_fail("eglCreateContext(ES 3.1) failed");
    }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        return gpu_fail("eglMakeCurrent(surfaceless) failed");
    }
    G.dpy = dpy;
    G.ctx = ctx;
    return 0;
}

static void gpu_context_bind(void) {
    if (eglGetCurrentContext() != G.ctx)
        eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, G.ctx);
}

static void gpu_context_close(void) {
    eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(G.dpy, G.ctx);
    eglTerminate(G.dpy);
    G.dpy = EGL_NO_DISPLAY;
    G.ctx = EGL_NO_CONTEXT;
}

#endif

/* "the service is usable right now", which is a different question from
 * fly_gpu_available's "the service came up". A windowed build shares its
 * thread with TIGR, whose backends take the thread's GL context for the
 * length of a window update and hand it back released — wglMakeCurrent(NULL,
 * NULL) on Windows, glXMakeCurrent(NULL, 0, 0) on X11 — so the frame after it
 * has to take the context back. Every entry point that touches GL asks here,
 * and it is a pointer compare when the context is already ours. */
static int gpu_live(void) {
    if (!G.ready) return 0;
    gpu_context_bind();
    return 1;
}

static GLuint compile_stage(GLenum type, const char *src, const char *tag);

/* The smallest shader in the dialect every program in this build is written
 * in. A GLES context speaks it natively; a desktop driver reaches it through
 * ARB_ES3_1_compatibility, and one that cannot would otherwise report a live
 * GPU and then fail every program for the rest of the run. */
static const char *FLY_GPU_ES_PROBE =
    "#version 310 es\n"
    "precision highp float;\n"
    "out vec4 o;\n"
    "void main(){ o = vec4(1.0); }\n";

int fly_gpu_init(void) {
    if (G.tried) return gpu_live() ? 0 : -1;
    G.tried = 1;
    G.err[0] = 0;

    if (gpu_context_open() != 0) return -1;

    /* core profiles need a bound VAO even for attribute-less draws */
    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);

    {
        GLuint probe = compile_stage(GL_FRAGMENT_SHADER, FLY_GPU_ES_PROBE, "ES 3.1");
        if (!probe) {
            glDeleteVertexArrays(1, &G.vao);
            G.vao = 0;
            gpu_context_close();
            return -1; /* compile_stage left the compiler's own message */
        }
        glDeleteShader(probe);
    }

    G.ready = 1;
    return 0;
}

void fly_gpu_shutdown(void) {
    int i;
    if (!gpu_live()) {
        G.tried = 0;
        return;
    }
    for (i = 0; i < G.nprogs; ++i)
        if (G.progs[i].prog) glDeleteProgram(G.progs[i].prog);
    G.nprogs = 0;
    { int ti; for (ti = 0; ti < FLY_GPU_TEX_MAX; ++ti) {
        if (G_tex[ti].tex) glDeleteTextures(1, &G_tex[ti].tex);
        G_tex[ti].tex = 0; G_tex[ti].w = G_tex[ti].h = 0; } }
    if (G_depth_rb) glDeleteRenderbuffers(1, &G_depth_rb);
    G_depth_rb = 0; G_depth_w = G_depth_h = 0;
    if (G_img_depth) glDeleteRenderbuffers(1, &G_img_depth);
    G_img_depth = 0; G_img_depth_w = G_img_depth_h = 0; G_img_pass = NULL;
    if (G_ms.fbo) glDeleteFramebuffers(1, &G_ms.fbo);
    if (G_ms.color) glDeleteRenderbuffers(1, &G_ms.color);
    if (G_ms.depth) glDeleteRenderbuffers(1, &G_ms.depth);
    G_ms.fbo = G_ms.color = G_ms.depth = 0;
    G_ms.w = G_ms.h = G_ms.want = G_ms.samples = 0;
    { int hi; for (hi = 0; hi < 2; ++hi) {
        if (G_hdr[hi].fbo) glDeleteFramebuffers(1, &G_hdr[hi].fbo);
        if (G_hdr[hi].tex) glDeleteTextures(1, &G_hdr[hi].tex);
        G_hdr[hi].fbo = G_hdr[hi].tex = 0;
        G_hdr[hi].w = G_hdr[hi].h = 0; } }
    G_front = G_read = 0;
    if (G.fbo) glDeleteFramebuffers(1, &G.fbo);
    if (G.tex) glDeleteTextures(1, &G.tex);
    if (G.vao) glDeleteVertexArrays(1, &G.vao);
    G.fbo = G.tex = G.vao = 0;
    G.tw = G.th = 0;
    free(G.readback);
    G.readback = NULL;
    G.readback_px = 0;
    gpu_context_close();
    G.ready = 0;
    G.tried = 0;
}

int fly_gpu_available(void) { return G.ready; }

const char *fly_gpu_renderer(void) {
    return gpu_live() ? (const char *)glGetString(GL_RENDERER) : NULL;
}

const char *fly_gpu_error(void) { return G.err; }

/* ---------------- programs ---------------- */

/* every fullscreen pass shares this attribute-less covering triangle */
static const char *FLY_GPU_VS =
    "#version 310 es\n"
    "void main(){\n"
    "  vec2 p[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
    "  gl_Position = vec4(p[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static GLuint compile_stage(GLenum type, const char *src, const char *tag) {
    GLuint s = glCreateShader(type);
    if (!s) {
        gpu_fail("glCreateShader failed");
        return 0;
    }
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[400];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        snprintf(G.err, sizeof G.err, "%s shader: %s", tag, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

fly_gpu_prog *fly_gpu_program(const char *frag_src) {
    int i;
    if (!frag_src) return NULL;
    if (!gpu_live() && fly_gpu_init() != 0) return NULL;
    for (i = 0; i < G.nprogs; ++i)
        if (!G.progs[i].key_vert && G.progs[i].key_frag == frag_src) return &G.progs[i];
    if (G.nprogs >= FLY_GPU_PROG_MAX) {
        gpu_fail("program cache full");
        return NULL;
    }

    GLuint vs = compile_stage(GL_VERTEX_SHADER, FLY_GPU_VS, "vertex");
    if (!vs) return NULL;
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag_src, "fragment");
    if (!fs) {
        glDeleteShader(vs);
        return NULL;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        gpu_fail("glCreateProgram failed");
        return NULL;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    /* the stages live on inside the linked program */
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[400];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        snprintf(G.err, sizeof G.err, "link: %s", log);
        glDeleteProgram(prog);
        return NULL;
    }
    G.progs[G.nprogs].key_vert = NULL;
    G.progs[G.nprogs].key_frag = frag_src;
    G.progs[G.nprogs].prog = prog;
    return &G.progs[G.nprogs++];
}

/* ---------------- uniforms ---------------- */

/* NB: the parameter is deliberately not named `prog` — the preprocessor is
 * token-based and would also rewrite the `->prog` member access below. */
#define FLY_GPU_UNIFORM(P_, name, call)                    \
    do {                                                   \
        if (!(P_) || !gpu_live()) return;                     \
        glUseProgram((P_)->prog);                          \
        GLint loc = glGetUniformLocation((P_)->prog, name); \
        if (loc >= 0) { call; }                            \
    } while (0)

void fly_gpu_set1i(fly_gpu_prog *p, const char *name, int v) {
    FLY_GPU_UNIFORM(p, name, glUniform1i(loc, v));
}
void fly_gpu_set1f(fly_gpu_prog *p, const char *name, float v) {
    FLY_GPU_UNIFORM(p, name, glUniform1f(loc, v));
}
void fly_gpu_set3i(fly_gpu_prog *p, const char *name, int x, int y, int z) {
    FLY_GPU_UNIFORM(p, name, glUniform3i(loc, x, y, z));
}
void fly_gpu_set2f(fly_gpu_prog *p, const char *name, float x, float y) {
    FLY_GPU_UNIFORM(p, name, glUniform2f(loc, x, y));
}
void fly_gpu_set2i(fly_gpu_prog *p, const char *name, int x, int y) {
    FLY_GPU_UNIFORM(p, name, glUniform2i(loc, x, y));
}
void fly_gpu_set3f(fly_gpu_prog *p, const char *name, float x, float y, float z) {
    FLY_GPU_UNIFORM(p, name, glUniform3f(loc, x, y, z));
}
void fly_gpu_set4f(fly_gpu_prog *p, const char *name, float x, float y, float z, float w) {
    FLY_GPU_UNIFORM(p, name, glUniform4f(loc, x, y, z, w));
}
void fly_gpu_set3fv(fly_gpu_prog *p, const char *name, const float *v, int count) {
    if (!v || count <= 0) return;
    FLY_GPU_UNIFORM(p, name, glUniform3fv(loc, count, v));
}

void fly_gpu_set4fv(fly_gpu_prog *p, const char *name, const float *v, int count) {
    if (!v || count <= 0) return;
    FLY_GPU_UNIFORM(p, name, glUniform4fv(loc, count, v));
}

static int ensure_hdr(int i, int w, int h) {
    if (G_hdr[i].fbo && G_hdr[i].w == w && G_hdr[i].h == h) return 0;
    if (G_hdr[i].fbo) glDeleteFramebuffers(1, &G_hdr[i].fbo);
    if (G_hdr[i].tex) glDeleteTextures(1, &G_hdr[i].tex);
    G_hdr[i].fbo = G_hdr[i].tex = 0;
    /* Bind render-target textures on a unit past the data-texture range:
     * glBindTexture affects whichever unit is active, so creating a target
     * while unit 0 was selected silently unbound the caller's data texture. */
    glActiveTexture(GL_TEXTURE0 + FLY_GPU_TEX_MAX);
    glGenTextures(1, &G_hdr[i].tex);
    glBindTexture(GL_TEXTURE_2D, G_hdr[i].tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, w, h);
    /* nearest and clamped: these carry radiance and view depth, and every
     * pass over them indexes texels rather than filtering between them */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &G_hdr[i].fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[i].fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G_hdr[i].tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &G_hdr[i].fbo);
        glDeleteTextures(1, &G_hdr[i].tex);
        G_hdr[i].fbo = G_hdr[i].tex = 0;
        return gpu_fail("float framebuffer incomplete");
    }
    G_hdr[i].w = w;
    G_hdr[i].h = h;
    return 0;
}

/* the target a pass renders into, which is also where a frame starts */
static int ensure_hdr_target(int w, int h) {
    G_front = G_read = 0;
    return ensure_hdr(0, w, h);
}

int fly_gpu_draw_hdr(fly_gpu_prog *p, float *rgba, int w, int h) {
    if (!p || !rgba || w <= 0 || h <= 0) return gpu_fail("bad hdr draw args");
    if (!gpu_live()) return gpu_fail("no GL context");
    if (ensure_hdr_target(w, h) != 0) return -1;
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[G_front].fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(p->prog);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, rgba);
    G_down += (double)w * (double)h * 16.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("float glReadPixels failed");
    /* GL origin is bottom-left; flip into fly_img's top-down row order */
    {
        int y, x;
        for (y = 0; y < h / 2; ++y) {
            float *a = rgba + (size_t)y * (size_t)w * 4;
            float *b = rgba + (size_t)(h - 1 - y) * (size_t)w * 4;
            for (x = 0; x < w * 4; ++x) { float tmp = a[x]; a[x] = b[x]; b[x] = tmp; }
        }
    }
    return 0;
}

int fly_gpu_set_texture(fly_gpu_prog *p, const char *name, int unit,
                        const float *data, int w, int h) {
    if (!p || !name || !data || w <= 0 || h <= 0) return gpu_fail("bad texture args");
    if (unit < 0 || unit >= FLY_GPU_TEX_MAX) return gpu_fail("texture unit out of range");
    if (!gpu_live()) return gpu_fail("no GL context");
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    /* Select the unit *first*: glBindTexture always acts on the active unit, so
     * creating this texture while another unit was selected would leave that
     * unit pointing here — which silently aimed both shadow cascades at the
     * same map when two were uploaded back to back. */
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    if (!G_tex[unit].tex || G_tex[unit].w != w || G_tex[unit].h != h) {
        if (G_tex[unit].tex) glDeleteTextures(1, &G_tex[unit].tex);
        glGenTextures(1, &G_tex[unit].tex);
        glBindTexture(GL_TEXTURE_2D, G_tex[unit].tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, w, h);
        /* exact reads: no filtering, no wrap */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        G_tex[unit].w = w;
        G_tex[unit].h = h;
    }
    glBindTexture(GL_TEXTURE_2D, G_tex[unit].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_FLOAT, data);
    G_up += (double)w * (double)h * 4.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("texture upload failed");
    glUseProgram(p->prog);
    {
        GLint loc = glGetUniformLocation(p->prog, name);
        if (loc >= 0) glUniform1i(loc, unit);
    }
    return 0;
}

/* ---------------- mesh pipeline ---------------- */

struct fly_gpu_mesh {
    GLuint vao, vbo, ibo, inst;
    int nindices;
    int vbytes, ibytes, ibytes_inst;   /* current buffer capacities */
};

fly_gpu_mesh *fly_gpu_mesh_create(void) {
    fly_gpu_mesh *m;
    if (!gpu_live() && fly_gpu_init() != 0) return NULL;
    m = (fly_gpu_mesh *)calloc(1, sizeof *m);
    if (!m) { gpu_fail("mesh alloc failed"); return NULL; }
    glGenVertexArrays(1, &m->vao);
    glGenBuffers(1, &m->vbo);
    glGenBuffers(1, &m->ibo);
    glGenBuffers(1, &m->inst);
    return m;
}

void fly_gpu_mesh_free(fly_gpu_mesh *m) {
    if (!m) return;
    if (gpu_live()) {
        if (m->vao) glDeleteVertexArrays(1, &m->vao);
        if (m->vbo) glDeleteBuffers(1, &m->vbo);
        if (m->ibo) glDeleteBuffers(1, &m->ibo);
        if (m->inst) glDeleteBuffers(1, &m->inst);
    }
    free(m);
}

int fly_gpu_mesh_upload(fly_gpu_mesh *m, const float *verts, int nverts,
                        int floats_per_vert, const int *attr_sizes, int nattrs,
                        const uint32_t *indices, int nindices) {
    int i, offset = 0, vb, ib;
    if (!m || !verts || !indices || !attr_sizes) return gpu_fail("bad mesh args");
    if (nverts <= 0 || nindices <= 0 || floats_per_vert <= 0 || nattrs <= 0)
        return gpu_fail("empty mesh");
    if (!gpu_live()) return gpu_fail("no GL context");
    vb = nverts * floats_per_vert * (int)sizeof(float);
    ib = nindices * (int)sizeof(uint32_t);

    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    /* orphan-and-refill when the size changes, sub-update when it does not:
     * terrain rings re-upload every frame at a stable vertex count */
    if (vb != m->vbytes) {
        glBufferData(GL_ARRAY_BUFFER, vb, verts, GL_DYNAMIC_DRAW);
        m->vbytes = vb;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vb, verts);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    if (ib != m->ibytes) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ib, indices, GL_DYNAMIC_DRAW);
        m->ibytes = ib;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ib, indices);
    }
    for (i = 0; i < nattrs; ++i) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, attr_sizes[i], GL_FLOAT, GL_FALSE,
                              floats_per_vert * (GLsizei)sizeof(float),
                              (const void *)(size_t)(offset * (int)sizeof(float)));
        offset += attr_sizes[i];
    }
    if (offset != floats_per_vert) return gpu_fail("attribute sizes do not fill the stride");
    m->nindices = nindices;
    G_up += (double)vb + (double)ib;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("mesh upload failed");
    return 0;
}

int fly_gpu_mesh_instances(fly_gpu_mesh *m, const float *data, int ninstances,
                           int floats_per_instance, const int *attr_sizes,
                           int nattrs, int first_loc) {
    int i, offset = 0, bytes;
    if (!m || !data || !attr_sizes) return gpu_fail("bad instance args");
    if (ninstances <= 0 || floats_per_instance <= 0 || nattrs <= 0)
        return gpu_fail("empty instance buffer");
    if (!gpu_live()) return gpu_fail("no GL context");
    bytes = ninstances * floats_per_instance * (int)sizeof(float);

    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, m->inst);
    /* orphan-and-refill when the size changes, sub-update when it does not —
     * the same rule the vertex buffer uses, and it matters more here: a scatter
     * re-sends its whole instance list every frame at a count that wanders. */
    if (bytes != m->ibytes_inst) {
        glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_DYNAMIC_DRAW);
        m->ibytes_inst = bytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, data);
    }
    for (i = 0; i < nattrs; ++i) {
        GLuint loc = (GLuint)(first_loc + i);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, attr_sizes[i], GL_FLOAT, GL_FALSE,
                              floats_per_instance * (GLsizei)sizeof(float),
                              (const void *)(size_t)(offset * (int)sizeof(float)));
        glVertexAttribDivisor(loc, 1);
        offset += attr_sizes[i];
    }
    if (offset != floats_per_instance)
        return gpu_fail("instance attribute sizes do not fill the stride");
    G_up += (double)bytes;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("instance upload failed");
    return 0;
}

fly_gpu_prog *fly_gpu_program_mesh(const char *vert_src, const char *frag_src) {
    int i;
    GLuint vs, fs, prog;
    GLint linked = 0;
    if (!vert_src || !frag_src) return NULL;
    if (!gpu_live() && fly_gpu_init() != 0) return NULL;
    for (i = 0; i < G.nprogs; ++i)
        if (G.progs[i].key_vert == vert_src && G.progs[i].key_frag == frag_src)
            return &G.progs[i];
    if (G.nprogs >= FLY_GPU_PROG_MAX) { gpu_fail("program cache full"); return NULL; }
    vs = compile_stage(GL_VERTEX_SHADER, vert_src, "vertex");
    if (!vs) return NULL;
    fs = compile_stage(GL_FRAGMENT_SHADER, frag_src, "fragment");
    if (!fs) { glDeleteShader(vs); return NULL; }
    prog = glCreateProgram();
    if (!prog) { glDeleteShader(vs); glDeleteShader(fs); gpu_fail("glCreateProgram failed"); return NULL; }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[400];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        snprintf(G.err, sizeof G.err, "link: %s", log);
        glDeleteProgram(prog);
        return NULL;
    }
    G.progs[G.nprogs].key_vert = vert_src;
    G.progs[G.nprogs].key_frag = frag_src;
    G.progs[G.nprogs].prog = prog;
    return &G.progs[G.nprogs++];
}

/* Build (or reuse) the multisample colour+depth pair. Returns the sample count
 * actually in force: 1 means "no multisample target", and the caller falls back
 * to the plain single-sample path rather than failing.
 *
 * The returned count is GL_SAMPLES on the finished framebuffer, not the number
 * that was asked for. glRenderbufferStorageMultisample is free to allocate
 * *more* samples than requested, and llvmpipe does exactly that — a 2-sample
 * request comes back 4-sample, producing a frame byte-identical to msaa 4 at
 * msaa 4's price. Reporting the request would make that invisible. */
static int ensure_ms_target(int w, int h, int samples) {
    GLint maxs = 1, got = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxs);
    if (samples > maxs) samples = maxs;
    if (samples < 2) return 1;
    if (G_ms.fbo && G_ms.w == w && G_ms.h == h && G_ms.want == samples) return G_ms.samples;
    if (G_ms.fbo) glDeleteFramebuffers(1, &G_ms.fbo);
    if (G_ms.color) glDeleteRenderbuffers(1, &G_ms.color);
    if (G_ms.depth) glDeleteRenderbuffers(1, &G_ms.depth);
    G_ms.fbo = G_ms.color = G_ms.depth = 0;
    glGenRenderbuffers(1, &G_ms.color);
    glBindRenderbuffer(GL_RENDERBUFFER, G_ms.color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA16F, w, h);
    glGenRenderbuffers(1, &G_ms.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, G_ms.depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &G_ms.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G_ms.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, G_ms.color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G_ms.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        glDeleteFramebuffers(1, &G_ms.fbo);
        glDeleteRenderbuffers(1, &G_ms.color);
        glDeleteRenderbuffers(1, &G_ms.depth);
        G_ms.fbo = G_ms.color = G_ms.depth = 0;
        return 1; /* not fatal: render single-sampled */
    }
    glGetIntegerv(GL_SAMPLES, &got);
    G_ms.w = w;
    G_ms.h = h;
    G_ms.want = samples;
    G_ms.samples = got > 1 ? got : samples;
    return G_ms.samples;
}

int fly_gpu_pass_samples(void) { return G_pass_samples; }

int fly_gpu_pass_begin(int w, int h, const float *clear_rgba, int samples) {
    if (!gpu_live() && fly_gpu_init() != 0) return -1;
    if (ensure_hdr_target(w, h) != 0) return -1;
    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    G_pass_samples = samples > 1 ? ensure_ms_target(w, h, samples) : 1;
    G_pass_ms = G_pass_samples > 1;
    glDisable(GL_BLEND);
    if (G_pass_ms) {
        glBindFramebuffer(GL_FRAMEBUFFER, G_ms.fbo);
        glViewport(0, 0, w, h);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_CULL_FACE);
        if (clear_rgba) glClearColor(clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]);
        else glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (glGetError() != GL_NO_ERROR) return gpu_fail("multisample pass begin failed");
        return 0;
    }
    if (!G_depth_rb || G_depth_w != w || G_depth_h != h) {
        if (G_depth_rb) glDeleteRenderbuffers(1, &G_depth_rb);
        glGenRenderbuffers(1, &G_depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, G_depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        G_depth_w = w;
        G_depth_h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[G_front].fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G_depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return gpu_fail("depth framebuffer incomplete");
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    /* LEQUAL, not LESS: the environment pass draws its sky as a quad pinned to
     * the far plane, which must survive the depth clear it is equal to. */
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); /* fly99 meshes are not consistently wound */
    if (clear_rgba) glClearColor(clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]);
    else glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("pass begin failed");
    return 0;
}

int fly_gpu_mesh_draw(fly_gpu_prog *p, fly_gpu_mesh *m) {
    if (!p || !m || m->nindices <= 0) return gpu_fail("bad mesh draw");
    if (!gpu_live()) return gpu_fail("no GL context");
    glUseProgram(p->prog);
    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->nindices, GL_UNSIGNED_INT, 0);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("mesh draw failed");
    return 0;
}

int fly_gpu_mesh_draw_instanced(fly_gpu_prog *p, fly_gpu_mesh *m, int ninstances) {
    if (!p || !m || m->nindices <= 0) return gpu_fail("bad mesh draw");
    if (ninstances <= 0) return 0;   /* nothing to draw is not a failure */
    if (!gpu_live()) return gpu_fail("no GL context");
    glUseProgram(p->prog);
    glBindVertexArray(m->vao);
    glDrawElementsInstanced(GL_TRIANGLES, m->nindices, GL_UNSIGNED_INT, 0, ninstances);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("instanced draw failed");
    return 0;
}

/* Land the pass in the float target and leave it bound: resolve multisampling
 * if there was any, drop the depth test and detach the depth buffer so a later
 * fullscreen pass is not tested against geometry that is no longer there. */
static int pass_settle(void) {
    if (!gpu_live()) return gpu_fail("no GL context");
    if (G_pass_ms) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, G_ms.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G_hdr[G_front].fbo);
        glBlitFramebuffer(0, 0, G_ms.w, G_ms.h, 0, 0, G_ms.w, G_ms.h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (glGetError() != GL_NO_ERROR) return gpu_fail("multisample resolve failed");
        G_pass_ms = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[G_front].fbo);
    glDisable(GL_DEPTH_TEST);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glBindVertexArray(G.vao);
    return 0;
}

int fly_gpu_pass_end(float *rgba, int w, int h, int flip) {
    int y, x;
    if (!rgba) return gpu_fail("bad pass end");
    if (pass_settle() != 0) return -1;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, rgba);
    G_down += (double)w * (double)h * 16.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("pass readback failed");
    if (flip)
        for (y = 0; y < h / 2; ++y) {
            float *a = rgba + (size_t)y * (size_t)w * 4;
            float *b = rgba + (size_t)(h - 1 - y) * (size_t)w * 4;
            for (x = 0; x < w * 4; ++x) { float tmp = a[x]; a[x] = b[x]; b[x] = tmp; }
        }
    return 0;
}

/* ---------------- the resident frame ---------------- */

int fly_gpu_frame_end(void) {
    if (pass_settle() != 0) return -1;
    G_read = G_front; /* what the pass drew is what a later pass may sample */
    return 0;
}

int fly_gpu_frame_swap(void) {
    if (!gpu_live()) return gpu_fail("no GL context");
    if (!G_hdr[G_front].fbo) return gpu_fail("no frame to publish");
    G_read = G_front;
    G_front ^= 1;
    return ensure_hdr(G_front, G_hdr[G_read].w, G_hdr[G_read].h);
}

int fly_gpu_frame_bind(fly_gpu_prog *p, const char *name, int unit) {
    GLint loc;
    if (!p || !name) return gpu_fail("bad frame bind");
    if (unit < 0 || unit >= FLY_GPU_TEX_MAX) return gpu_fail("texture unit out of range");
    if (!gpu_live() || !G_hdr[G_read].tex) return gpu_fail("no frame to bind");
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, G_hdr[G_read].tex);
    glUseProgram(p->prog);
    loc = glGetUniformLocation(p->prog, name);
    if (loc >= 0) glUniform1i(loc, unit);
    return 0;
}

int fly_gpu_frame_overlay(int blend) {
    if (!gpu_live()) return gpu_fail("no GL context");
    if (!G_hdr[G_front].fbo) return gpu_fail("no frame target");
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[G_front].fbo);
    glViewport(0, 0, G_hdr[G_front].w, G_hdr[G_front].h);
    glDisable(GL_DEPTH_TEST);
    /* Alpha is never blended, in either mode. The frame's alpha is its view
     * depth in metres, not coverage, and an ordinary over-blend would mix a
     * fragment's coverage into it — leaving the z-buffer reading a quarter of
     * a metre wherever a propeller disc crossed it. Colour composites; depth
     * is kept. */
    if (blend == FLY_GPU_BLEND_OVER) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else if (blend == FLY_GPU_BLEND_ADD) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }
    if (glGetError() != GL_NO_ERROR) return gpu_fail("frame overlay failed");
    return 0;
}

int fly_gpu_frame_draw(fly_gpu_prog *p) {
    if (!p) return gpu_fail("bad frame draw");
    if (fly_gpu_frame_overlay(FLY_GPU_BLEND_OFF) != 0) return -1;
    glUseProgram(p->prog);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("frame draw failed");
    return 0;
}

int fly_gpu_frame_read(float *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return gpu_fail("bad frame read");
    if (!gpu_live() || !G_hdr[G_read].fbo) return gpu_fail("no frame to read");
    if (G_hdr[G_read].w != w || G_hdr[G_read].h != h) return gpu_fail("frame size mismatch");
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    glBindFramebuffer(GL_FRAMEBUFFER, G_hdr[G_read].fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, rgba);
    G_down += (double)w * (double)h * 16.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("frame readback failed");
    return 0;
}

/* ---------------- draw + readback ---------------- */

/* (re)allocate the offscreen target when the requested size changes */
static int ensure_target(int w, int h) {
    if (G.fbo && G.tw == w && G.th == h) return 0;
    if (G.fbo) glDeleteFramebuffers(1, &G.fbo);
    if (G.tex) glDeleteTextures(1, &G.tex);
    G.fbo = G.tex = 0;
    /* Bind render-target textures on a unit past the data-texture range:
     * glBindTexture affects whichever unit is active, so creating a target
     * while unit 0 was selected silently unbound the caller's data texture. */
    glActiveTexture(GL_TEXTURE0 + FLY_GPU_TEX_MAX);
    glGenTextures(1, &G.tex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, &G.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &G.fbo);
        glDeleteTextures(1, &G.tex);
        G.fbo = G.tex = 0;
        return gpu_fail("framebuffer incomplete");
    }
    G.tw = w;
    G.th = h;
    return 0;
}

int fly_gpu_draw(fly_gpu_prog *p, fly_img *out) {
    if (!p || !out || !out->px || out->w <= 0 || out->h <= 0) return gpu_fail("bad draw args");
    if (!gpu_live()) return gpu_fail("no GL context");
    int w = out->w, h = out->h;
    if (ensure_target(w, h) != 0) return -1;

    size_t npx = (size_t)w * (size_t)h;
    if (G.readback_px < npx) {
        uint32_t *buf = (uint32_t *)realloc(G.readback, npx * sizeof(uint32_t));
        if (!buf) return gpu_fail("readback alloc failed");
        G.readback = buf;
        G.readback_px = npx;
    }

    /* Drop any error left behind by earlier calls (e.g. a uniform type
     * mismatch) so the check after readback reports only this draw's fault. */
    while (glGetError() != GL_NO_ERROR) { /* drain */ }

    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(p->prog);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, G.readback);
    G_down += (double)w * (double)h * 4.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("glReadPixels failed");

    /* GL's origin is bottom-left; fly_img rows run top-down */
    int y;
    for (y = 0; y < h; ++y)
        memcpy(&out->px[(size_t)y * (size_t)w],
               &G.readback[(size_t)(h - 1 - y) * (size_t)w],
               (size_t)w * sizeof(uint32_t));
    return 0;
}

/* ---------------- offscreen float images ---------------- */

struct fly_gpu_img {
    GLuint tex, fbo;
    int w, h, nc;
};

fly_gpu_img *fly_gpu_img_create(void) {
    fly_gpu_img *im;
    if (!gpu_live() && fly_gpu_init() != 0) return NULL;
    im = (fly_gpu_img *)calloc(1, sizeof *im);
    if (!im) { gpu_fail("image alloc failed"); return NULL; }
    return im;
}

void fly_gpu_img_free(fly_gpu_img *im) {
    if (!im) return;
    if (gpu_live()) {
        if (im->fbo) glDeleteFramebuffers(1, &im->fbo);
        if (im->tex) glDeleteTextures(1, &im->tex);
    }
    free(im);
}

int fly_gpu_img_set(fly_gpu_img *im, int w, int h, const float *px, int nc) {
    GLenum internal, format;
    if (!im || w <= 0 || h <= 0) return gpu_fail("bad image size");
    if (nc < 1 || nc > 4) return gpu_fail("bad image channel count");
    if (!gpu_live()) return gpu_fail("no GL context");
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    internal = nc == 1 ? GL_R32F : (nc == 2 ? GL_RG32F : (nc == 3 ? GL_RGB32F : GL_RGBA32F));
    format = nc == 1 ? GL_RED : (nc == 2 ? GL_RG : (nc == 3 ? GL_RGB : GL_RGBA));
    /* the scratch unit: binding here must not disturb a caller's data texture */
    glActiveTexture(GL_TEXTURE0 + FLY_GPU_TEX_MAX);
    if (!im->tex || im->w != w || im->h != h || im->nc != nc) {
        if (im->fbo) { glDeleteFramebuffers(1, &im->fbo); im->fbo = 0; }
        if (im->tex) glDeleteTextures(1, &im->tex);
        glGenTextures(1, &im->tex);
        glBindTexture(GL_TEXTURE_2D, im->tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        im->w = w;
        im->h = h;
        im->nc = nc;
    } else {
        glBindTexture(GL_TEXTURE_2D, im->tex);
    }
    if (px) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_FLOAT, px);
        G_up += (double)w * (double)h * (double)nc * 4.0;
    }
    if (glGetError() != GL_NO_ERROR) return gpu_fail("image upload failed");
    return 0;
}

int fly_gpu_img_bind(fly_gpu_prog *p, const char *name, int unit, const fly_gpu_img *im) {
    GLint loc;
    if (!p || !name || !im || !im->tex) return gpu_fail("bad image bind");
    if (unit < 0 || unit >= FLY_GPU_TEX_MAX) return gpu_fail("texture unit out of range");
    if (!gpu_live()) return gpu_fail("no GL context");
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, im->tex);
    glUseProgram(p->prog);
    loc = glGetUniformLocation(p->prog, name);
    if (loc >= 0) glUniform1i(loc, unit);
    return 0;
}

/* bind (creating on first use) the framebuffer that draws into `im` */
static int img_bind_fbo(fly_gpu_img *im) {
    if (!im->fbo) {
        glGenFramebuffers(1, &im->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, im->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &im->fbo);
            im->fbo = 0;
            return gpu_fail("image framebuffer incomplete");
        }
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, im->fbo);
    }
    return 0;
}

int fly_gpu_img_draw(fly_gpu_prog *p, fly_gpu_img *im) {
    if (!p || !im || !im->tex) return gpu_fail("bad image draw");
    if (im->nc != 4) return gpu_fail("only 4-channel images are renderable");
    if (!gpu_live()) return gpu_fail("no GL context");
    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    if (img_bind_fbo(im) != 0) return -1;
    glViewport(0, 0, im->w, im->h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(p->prog);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("image draw failed");
    return 0;
}

int fly_gpu_pass_begin_img(fly_gpu_img *im, int w, int h, int nc, const float *clear_rgba) {
    if (!im || w <= 0 || h <= 0) return gpu_fail("bad image pass size");
    if (!gpu_live() && fly_gpu_init() != 0) return -1;
    if (fly_gpu_img_set(im, w, h, NULL, nc) != 0) return -1;
    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    if (!G_img_depth || G_img_depth_w != w || G_img_depth_h != h) {
        if (G_img_depth) glDeleteRenderbuffers(1, &G_img_depth);
        glGenRenderbuffers(1, &G_img_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, G_img_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        G_img_depth_w = w;
        G_img_depth_h = h;
    }
    if (img_bind_fbo(im) != 0) return -1;
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G_img_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        return gpu_fail("image pass framebuffer incomplete");
    }
    G_img_pass = im;
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    /* LESS, so the texel that survives is the nearest fragment's — the same
     * "keep the minimum" rule the CPU shadow rasterizer applies by hand */
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    if (clear_rgba) glClearColor(clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]);
    else glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("image pass begin failed");
    return 0;
}

int fly_gpu_img_read(const fly_gpu_img *im, float *px) {
    if (!im || !px || !im->tex) return gpu_fail("bad image read");
    if (!gpu_live()) return gpu_fail("no GL context");
    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    if (img_bind_fbo((fly_gpu_img *)im) != 0) return -1;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, im->w, im->h,
                 im->nc == 1 ? GL_RED : (im->nc == 2 ? GL_RG
                                       : (im->nc == 3 ? GL_RGB : GL_RGBA)), GL_FLOAT, px);
    G_down += (double)im->w * (double)im->h * (double)im->nc * 4.0;
    if (glGetError() != GL_NO_ERROR) return gpu_fail("image readback failed");
    return 0;
}

int fly_gpu_pass_end_img(void) {
    if (!gpu_live()) return gpu_fail("no GL context");
    if (!G_img_pass) return gpu_fail("no image pass in flight");
    glDisable(GL_DEPTH_TEST);
    /* detach, or the next fullscreen draw into this image would be depth-tested
     * against whatever this pass left behind */
    glBindFramebuffer(GL_FRAMEBUFFER, G_img_pass->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    G_img_pass = NULL;
    glBindVertexArray(G.vao);
    if (glGetError() != GL_NO_ERROR) return gpu_fail("image pass end failed");
    return 0;
}

#endif /* FLY_GPU */
