#!/usr/bin/env python3
"""Build CLI for fly99.

Usage:
  ./make.py            build library objects + fly99 CLI
  ./make.py --test     also build and run the e2e test suite (src/test.c)
  ./make.py --test A B  run only aspects/groups A B (e.g. water views, unit, cover)
  ./make.py --exec ... build then run the CLI (extra args forwarded)
  ./make.py --window   enable the realtime window backend (needs X11/GL dev libs)
  ./make.py --no-threads  run the CPU path tracer on one core (same frame, slower)
  ./make.py --roll     amalgamate the library into build/<rid>/fly.h (single-file lib)
  ./make.py --release  optimized build
  ./make.py --shots    refresh docs/shots + docs/coverage.md (run --test first)

The e2e suite doubles as a visual-coverage harness. Run `build/<rid>/test --list`
to see every runnable aspect, `build/<rid>/test --units` for correctness only,
or e.g. `build/<rid>/test --hq water` to refresh a single hero shot at high
quality.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
EXT = ROOT / "ext"
STB = EXT / "stb"
TIGR = EXT / "tigr"


# Derive a runtime-id matching the existing build/<rid>/ convention.
def _runtime_id():
    os_name = platform.system().lower()
    if os_name == "windows":
        rid_os = "win"
    elif os_name == "darwin":
        rid_os = "osx"
    elif os_name == "linux":
        rid_os = "linux"
    else:
        rid_os = os_name

    arch = platform.machine().lower()
    rid_arch = {
        "x86_64": "x64", "amd64": "x64",
        "aarch64": "arm64", "arm64": "arm64",
        "armv7l": "arm", "armhf": "arm",
        "i386": "x86", "i686": "x86",
    }.get(arch, arch)

    return f"{rid_os}-{rid_arch}"


RID = _runtime_id()
BUILD = ROOT / "build" / RID
# The project is fly99; its directory is `apps/fly`, the way every project here
# is a short directory with a 99 name. The executable was named after the
# directory, so everything that talks about it — this file's own docstring, the
# README's command lines, the CLI's usage text, the default save name — said
# `fly99` and the build produced `fly`. Naming it explicitly is what the sibling
# projects do (ray builds `rayray`) and it puts the docs and the artifact back on
# the same word.
APP = "fly99"

def _find_compiler():
    """Locate a C compiler, preferring a GCC-family one (the codebase is C99
    and uses GNU-style flags). On Windows the common toolchains (MSYS2/MinGW,
    LLVM) are often not on PATH, so probe well-known install locations too.
    Returns the compiler path (or bare name) and the directory to prepend to
    PATH so the compiler can spawn its own subprocesses (gcc needs its bin dir
    on PATH to find cc1)."""
    candidates = []
    if os.name == "nt":
        roots = [
            os.environ.get("MSYS2_ROOT"),
            os.environ.get("MINGW_ROOT"),
            r"C:\msys64",
            r"C:\Program Files\LLVM",
            r"C:\Program Files\Git",
        ]
        for root in roots:
            if not root:
                continue
            for sub in ("mingw64", "ucrt64", "clang64", "usr", "mingw32"):
                for name in ("gcc.exe", "clang.exe", "cc.exe"):
                    p = os.path.join(root, sub, "bin", name)
                    if os.path.isfile(p):
                        candidates.append((p, os.path.join(root, sub, "bin")))
    # bare names on PATH (works on Linux and any Windows shell with the
    # toolchain already exported)
    for name in ("cc", "clang", "gcc"):
        w = shutil.which(name)
        if w:
            candidates.append((w, os.path.dirname(w)))
    if not candidates:
        return "cc", None
    # prefer gcc over clang over cc; keep first-found order otherwise
    def rank(c):
        base = os.path.basename(c[0]).lower()
        return 0 if base.startswith("gcc") else 1 if base.startswith("clang") else 2
    return min(candidates, key=rank)


CC, _CC_BIN = _find_compiler()
if _CC_BIN and os.name == "nt":
    os.environ["PATH"] = _CC_BIN + os.pathsep + os.environ.get("PATH", "")
CFLAGS = ["-std=c99", "-Wall", "-Wextra", "-I", str(SRC)]
if os.name == "nt":
    CFLAGS += ["-D_CRT_SECURE_NO_WARNINGS"]
LDFLAGS = [] if os.name == "nt" else ["-lm"]

# library modules in dependency order (also the amalgamation order)
MODULES = [
    "fly_rng", "fly_img", "fly_font", "fly_yaml", "fly_sim", "fly_ord", "fly_faction",
    "fly_world", "fly_river", "fly_walk", "fly_line", "fly_rail", "fly_road", "fly_sea", "fly_convoy",
    "fly_pilot",
    "fly_game", "fly_gpu",
    "fly_render", "fly_hud", "fly_ui", "fly_app",
]
# Header-only modules: no .c of their own, but fly_render.c's shader strings are
# built from fly_glsl.h's macros, so the amalgamation is not compilable without
# it. verify_roll() below keeps that honest rather than trusting this list.
HEADER_ONLY = ["fly_math", "fly_glsl"]

# Vendored stb_truetype powers antialiased TTF text in every standard build.
if not (STB / "stb_truetype.h").is_file():
    raise SystemExit("missing vendored source: ext/stb/stb_truetype.h")
CFLAGS += ["-DFLY_HAS_STB", "-I", str(STB)]


def lib_sources():
    return [SRC / f"{m}.c" for m in MODULES if (SRC / f"{m}.c").exists()]


# GPU acceleration (fly_gpu) is enabled when a headless EGL + GLES 3.1 toolchain
# is actually present, and silently skipped otherwise, so the build stays
# dependency-free on machines without GL. Probe by compiling+linking for real
# rather than sniffing header paths, so a broken/partial install is caught here.
GPU_LDFLAGS = ["-lEGL", "-lGLESv2"]
_GPU_PROBE = """
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
int main(void){ eglGetProcAddress("eglGetPlatformDisplayEXT"); glCreateProgram(); return 0; }
"""


def gpu_supported():
    cached = getattr(gpu_supported, "_cache", None)
    if cached is not None:
        return cached
    ok = False
    try:
        BUILD.mkdir(parents=True, exist_ok=True)
        probe_c = BUILD / "_gpu_probe.c"
        probe_c.write_text(_GPU_PROBE)
        probe_exe = BUILD / ("_gpu_probe.exe" if os.name == "nt" else "_gpu_probe")
        ok = subprocess.run(
            [CC, "-std=c99", str(probe_c), "-o", str(probe_exe), *GPU_LDFLAGS],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
        probe_c.unlink(missing_ok=True)
        probe_exe.unlink(missing_ok=True)
    except OSError:
        ok = False
    gpu_supported._cache = ok
    return ok


# Worker threads for the path tracer. Probed the same way the GPU is, and for
# the same reason: the renderer runs on one core without them, so a toolchain
# that has no threads is a slower build rather than a broken one. Win32 threads
# need nothing declared; pthreads need the POSIX feature macro, because strict
# -std=c99 hides sysconf.
THREAD_CFLAGS = [] if os.name == "nt" else ["-D_POSIX_C_SOURCE=200809L"]
THREAD_LDFLAGS = [] if os.name == "nt" else ["-pthread"]
_THREAD_PROBE = """
#ifdef _WIN32
#include <process.h>
#include <windows.h>
static unsigned __stdcall body(void *p){ (void)p; return 0; }
int main(void){
    HANDLE h = (HANDLE)_beginthreadex(0, 0, body, 0, 0, 0);
    SYSTEM_INFO si; GetSystemInfo(&si);
    WaitForSingleObject(h, INFINITE); CloseHandle(h); return 0;
}
#else
#include <pthread.h>
#include <unistd.h>
static void *body(void *p){ return p; }
int main(void){
    pthread_t t;
    if (pthread_create(&t, 0, body, 0) != 0) return 1;
    pthread_join(t, 0);
    return (int)sysconf(_SC_NPROCESSORS_ONLN) > 0 ? 0 : 1;
}
#endif
"""


def threads_supported():
    cached = getattr(threads_supported, "_cache", None)
    if cached is not None:
        return cached
    ok = False
    try:
        BUILD.mkdir(parents=True, exist_ok=True)
        probe_c = BUILD / "_thread_probe.c"
        probe_c.write_text(_THREAD_PROBE)
        probe_exe = BUILD / ("_thread_probe.exe" if os.name == "nt" else "_thread_probe")
        ok = subprocess.run(
            [CC, "-std=c99", *THREAD_CFLAGS, str(probe_c), "-o", str(probe_exe), *THREAD_LDFLAGS],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
        probe_c.unlink(missing_ok=True)
        probe_exe.unlink(missing_ok=True)
    except OSError:
        ok = False
    threads_supported._cache = ok
    return ok


def objects(sources, extra_cflags):
    """Compile each source to its own object under build/<rid>/obj, in parallel,
    skipping any object already newer than its inputs.

    The CLI and the test suite are two links over the same library modules, and
    compiling that whole list once per executable — in one serial command each —
    was most of a cold build: fly_render.c alone is 6.3 s and it was paid twice.
    Per-source objects are shared between the two links, so the library is
    compiled once, and the critical path becomes the slowest single translation
    unit rather than the sum of all of them. Editing one .c recompiles one .c."""
    objdir = BUILD / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    cflags = [*CFLAGS, *extra_cflags]

    # A flag change invalidates every object, and there is no way to tell from
    # the object itself, so the flags are kept beside them.
    stamp = objdir / "cflags"
    key = " ".join(cflags)
    if not stamp.exists() or stamp.read_text() != key:
        for stale in objdir.glob("*.o"):
            stale.unlink()
        stamp.write_text(key)

    # Any header edit rebuilds everything. There are two dozen of them, they are
    # edited alongside the .c that owns them, and real dependency tracking would
    # mean depfiles for a saving of a second or two on the uncommon case.
    newest_h = max((h.stat().st_mtime for h in SRC.glob("*.h")), default=0.0)

    outs, todo = [], []
    for s in sources:
        o = objdir / (s.stem + ".o")
        outs.append(o)
        if not o.exists() or o.stat().st_mtime <= max(s.stat().st_mtime, newest_h):
            todo.append((s, o))

    if todo:
        def one(job):
            s, o = job
            cmd = [CC, *cflags, "-c", str(s), "-o", str(o)]
            return s, cmd, subprocess.run(cmd).returncode
        print("cc %d object%s: %s" % (len(todo), "" if len(todo) == 1 else "s",
                                      " ".join(s.name for s, _ in todo)))
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for s, cmd, rc in pool.map(one, todo):
                if rc != 0:
                    raise SystemExit("failed to compile %s:\n  %s" % (s.name, " ".join(cmd)))
    return outs


def link(objs, out, extra_ldflags=()):
    BUILD.mkdir(parents=True, exist_ok=True)
    cmd = [CC, *[str(o) for o in objs], "-o", str(out), *LDFLAGS, *extra_ldflags]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    return out


def run(exe, args=()):
    print(str(exe.relative_to(ROOT)), *args)
    return subprocess.run([str(exe), *args]).returncode


def roll():
    """Amalgamate all modules into build/<rid>/fly.h as a single-file library."""
    parts_hdr, parts_src = [], []
    for name in HEADER_ONLY + MODULES:
        h = SRC / f"{name}.h"
        if h.exists():
            parts_hdr.append(strip_include_guard(h.read_text(), f"{name.upper()}_H"))
        c = SRC / f"{name}.c"
        if c.exists():
            parts_src.append(strip_local_includes(c.read_text()))
    header = BUILD / "fly.h"
    header.write_text(
        "/* fly.h - fly99 single-file library (generated by make.py --roll) */\n"
        "#ifndef FLY_H\n#define FLY_H\n\n"
        + "\n\n".join(parts_hdr)
        + "\n\n#ifdef FLY_IMPLEMENTATION\n\n"
        + "\n\n".join(parts_src)
        + "\n\n#endif /* FLY_IMPLEMENTATION */\n#endif /* FLY_H */\n"
    )
    print(f"rolled {header.relative_to(ROOT)}")
    verify_roll(header)


def verify_roll(header):
    """Compile the amalgamation. Nothing else does, so a module missing from
    the roll list produces a single-file library that does not build and no
    test notices — which is exactly what happened when fly_glsl.h landed."""
    BUILD.mkdir(parents=True, exist_ok=True)
    probe = BUILD / "_roll_probe.c"
    probe.write_text('#define FLY_IMPLEMENTATION\n#include "fly.h"\n'
                     "int main(void){ return 0; }\n")
    out = BUILD / ("_roll_probe.exe" if os.name == "nt" else "_roll_probe")
    cmd = [CC, "-std=c99", "-Wall", "-Wextra", "-I", str(BUILD),
           "-DFLY_HAS_STB", "-I", str(STB)]
    if gpu_supported():
        cmd += ["-DFLY_GPU=1"]
    if threads_supported():
        cmd += ["-DFLY_THREADS=1", *THREAD_CFLAGS]
    cmd += [str(probe), "-o", str(out), *LDFLAGS]
    if gpu_supported():
        cmd += GPU_LDFLAGS
    if threads_supported():
        cmd += THREAD_LDFLAGS
    rc = subprocess.run(cmd).returncode
    probe.unlink(missing_ok=True)
    out.unlink(missing_ok=True)
    if rc != 0:
        raise SystemExit("build/%s/fly.h does not compile - a module is missing from the roll" % RID)
    print("verified build/%s/fly.h compiles standalone" % RID)


def strip_include_guard(text, guard):
    lines = text.splitlines()
    out, pending = [], False
    for line in lines:
        s = line.strip()
        if not pending and s == f"#ifndef {guard}":
            pending = True
            continue
        if pending and s == f"#define {guard}":
            pending = False
            continue
        out.append(line)
    while out and out[-1].strip() == "":
        out.pop()
    if out and out[-1].strip().startswith("#endif"):
        out.pop()
    # drop local includes; the amalgamation is ordered already
    out = [l for l in out if not l.strip().startswith('#include "fly_')]
    return "\n".join(out).strip()


def strip_local_includes(text):
    return "\n".join(
        l for l in text.splitlines() if not l.strip().startswith('#include "fly_')
    ).strip()


def png_recompress(src, dst):
    """Rewrite a PNG with real deflate compression (the in-game writer emits
    stored blocks to stay dependency-free; committed docs should be small)."""
    import struct
    import zlib

    data = src.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", src
    pos, idat, chunks = 8, b"", []
    while pos < len(data):
        ln = int.from_bytes(data[pos:pos + 4], "big")
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if tag == b"IDAT":
            idat += payload
        else:
            chunks.append((tag, payload))
        pos += 12 + ln
    comp = zlib.compress(zlib.decompress(idat), 9)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    body = b""
    for tag, payload in chunks:
        if tag == b"IEND":
            body += chunk(b"IDAT", comp)
        body += chunk(tag, payload)
    dst.write_bytes(data[:8] + body)


def _copy_shot(src_dir, dst_dir, name):
    src = src_dir / f"{name}.png"
    if not src.exists():
        return False
    dst = dst_dir / f"{name}.png"
    png_recompress(src, dst)
    print(f"{dst.relative_to(ROOT)}  {dst.stat().st_size // 1024} KiB")
    return True


def coverage_docs(src_dir, dst_dir):
    """Publish exactly the stills the manifest names, and nothing else.

    The manifest is the whole truth about docs/shots: a still the document no
    longer prints is deleted here rather than left behind, so the directory
    cannot fill up with the leftovers of renamed or retired shots.

    Which is why it has to have been the whole gallery that wrote it. The suite
    rewrites the manifest after any run that rendered anything, and a partial
    one names only what it happened to shoot: `--test --units` leaves eleven
    stills in it, and publishing that would delete the hundred pictures
    docs/coverage.md is made of, quietly and in one command. The suite says
    which kind of run it was in coverage.state, and anything short of a full,
    full-quality one is refused here rather than half-applied.
    """
    manifest = src_dir / "manifest.tsv"
    if not manifest.exists():
        return
    state_file = src_dir / "coverage.state"
    state = state_file.read_text().strip() if state_file.exists() else "partial"
    if state != "full":
        raise SystemExit(
            "%s was written by a partial or --fast run, which names only some of\n"
            "the gallery; publishing it would delete the rest of docs/shots.\n"
            "Refresh the whole gallery first:  ./make.py --test && ./make.py --shots"
            % manifest.relative_to(ROOT)
        )
    names = []
    for line in manifest.read_text().splitlines()[1:]:
        cols = line.split("\t")
        if cols and cols[0]:
            names.append(cols[0])
    for name in names:
        _copy_shot(src_dir, dst_dir, name)
    keep = {f"{name}.png" for name in names}
    stale = sorted(p for p in dst_dir.glob("*.png") if p.name not in keep)
    for path in stale:
        path.unlink()
        print(f"{path.relative_to(ROOT)}  removed (not in the manifest)")
    gallery = src_dir / "coverage.md"
    if gallery.exists():
        out = ROOT / "docs" / "coverage.md"
        out.write_text(gallery.read_text())
        print(f"{out.relative_to(ROOT)}  ({len(names)} coverage stills)")


def shots():
    src_dir = BUILD / "shots"
    dst_dir = ROOT / "docs" / "shots"
    dst_dir.mkdir(parents=True, exist_ok=True)
    coverage_docs(src_dir, dst_dir)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--roll", action="store_true", help="amalgamate library into build/<rid>/fly.h")
    parser.add_argument("--test", action="store_true", help="compile and run src/test.c")
    parser.add_argument("--exec", action="store_true", help="run the compiled app")
    parser.add_argument("--window", action="store_true", help="build realtime window backend (tigr, needs X11+GL)")
    parser.add_argument("--release", action="store_true", help="optimized build")
    parser.add_argument("--shots", action="store_true", help="refresh docs/shots from build/<rid>/shots")
    parser.add_argument("--no-gpu", action="store_true",
                        help="force the CPU renderer even when EGL/GLES is available")
    parser.add_argument("--no-threads", action="store_true",
                        help="run the CPU path tracer on one core (it renders the same frame)")
    parser.add_argument("args", nargs="*", help="arguments forwarded to --exec")
    args, unknown = parser.parse_known_args()
    if unknown:
        args.args = [*unknown, *args.args]

    cflags, ldflags, extra_src = [], [], []
    cflags += ["-O2"] if args.release else ["-g", "-O2"]
    # Point test binaries at the RID output dir for their scratch files.
    cflags += [f"-DFLY_BUILD_DIR=\"build/{RID}\""]
    # The windowed backend on Windows cannot share a link with the headless
    # GPU path: TIGR's GL backend defines its own gl* function-pointer globals
    # (loaded through wglGetProcAddress) and libGLESv2.dll.a exports the same
    # names as import stubs, so the two collide at link time. The windowed
    # loop blits the CPU-rendered frame into TIGR's buffer anyway, so the
    # headless EGL path is pointless there; force the CPU renderer (the
    # documented reference and fallback). Linux/macOS TIGR calls real GL
    # functions directly, so those builds keep the GPU path.
    gpu_off = args.no_gpu or (args.window and os.name == "nt")
    if not gpu_off and gpu_supported():
        cflags += ["-DFLY_GPU=1"]
        ldflags += GPU_LDFLAGS
        print("gpu: enabled (headless EGL + GLES 3.1)")
    else:
        reason = ("--no-gpu" if args.no_gpu else
                  "windowed build on Windows (TIGR owns the GL symbols)"
                  if args.window and os.name == "nt" else
                  "no EGL/GLES toolchain")
        print("gpu: disabled (%s) - using the CPU renderer" % reason)
    if not args.no_threads and threads_supported():
        cflags += ["-DFLY_THREADS=1", *THREAD_CFLAGS]
        ldflags += THREAD_LDFLAGS
        print("threads: enabled (the CPU path tracer runs its rows in parallel)")
    else:
        print("threads: disabled (%s) - the CPU path tracer runs on one core" %
              ("--no-threads" if args.no_threads else "no thread toolchain"))
    if args.window:
        if not (TIGR / "tigr.c").is_file() or not (TIGR / "tigr.h").is_file():
            raise SystemExit("missing vendored TIGR sources under ext/tigr")
        # _DEFAULT_SOURCE: tigr.c needs strdup, which strict -std=c99 hides
        cflags += ["-DFLY_WINDOW=1", "-D_DEFAULT_SOURCE", "-I", str(TIGR)]
        extra_src += [SRC / "fly_win.c", TIGR / "tigr.c"]
        if sys.platform == "darwin":
            ldflags += ["-framework", "OpenGL", "-framework", "Cocoa"]
        elif sys.platform.startswith("linux"):
            ldflags += ["-lGLU", "-lGL", "-lX11"]
        else:
            ldflags += ["-lopengl32", "-lgdi32"]

    exe_suffix = ".exe" if os.name == "nt" else ""
    lib_objs = objects(lib_sources(), cflags)
    app = link([*objects([SRC / "main.c", *extra_src], cflags), *lib_objs],
               BUILD / f"{APP}{exe_suffix}", ldflags)

    if args.roll:
        roll()

    if args.shots:
        shots()

    tests_passed = True
    if args.test:
        test_exe = link([*objects([SRC / "test.c"], cflags), *lib_objs],
                        BUILD / f"test{exe_suffix}", ldflags)
        # forward any trailing tokens as aspect/group filters (unless --exec owns
        # them), e.g. ./make.py --test water views  ·  ./make.py --test --units
        test_args = args.args if not args.exec else ()
        tests_passed = run(test_exe, test_args) == 0
        if not tests_passed:
            print("tests failed", file=sys.stderr)

    if args.exec:
        if args.test and not tests_passed:
            print("skipping exec: tests failed", file=sys.stderr)
            sys.exit(1)
        sys.exit(run(app, args.args))

    if args.test and not tests_passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
