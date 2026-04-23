/*
 * build.c — replaces configure + Makefile for this FFmpeg fork.
 *
 * Targets: macOS arm64 (default), Android arm64/arm, iOS arm64.
 * Usage: ./build [target]
 *   clang -O2 -o build build.c && ./build
 *
 * Produces: ffmpeg, ffprobe, ffplay (plus per-lib .a archives).
 *
 * Requires openssl headers+libs (for native SRT).
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "build_srcs.h"

/* ----- configuration ----- */

struct target {
    const char *name;
    const char *cc;
    const char *ar;
    const char *ranlib;         /* host ranlib on darwin, llvm-ranlib on NDK */
    const char *strip;
    const char *extra_cflags;   /* appended to COMMON_CFLAGS */
    const char *extra_ldflags;  /* appended to link commands */
    const char *frameworks;     /* Darwin linker frameworks */
    const char *sys_libs;       /* -lfoo for system libs */
    bool build_ffplay;          /* SDL-dependent */
    bool build_objc;            /* .m files (AVFoundation) */
    bool is_darwin;             /* macOS/iOS: compile VideoToolbox etc */
    bool is_macos;              /* macOS only: AudioToolbox etc */
    bool is_android;            /* Android: compile MediaCodec, android_camera */
};

/* Paths you may need to adjust for your machine. */
#define OPENSSL_MACOS    "/opt/homebrew/Cellar/openssl@3/3.6.2"
#define OPENSSL_ANDROID  "/Users/home/.local/android/arm64-v8a"
#define ANDROID_NDK      "/Users/home/Library/Android/sdk/ndk/29.0.14033849"
#define ANDROID_API      "21"

static const struct target targets[] = {
    {
        .name = "macos-arm64",
        .cc = "clang",
        .ar = "ar",
        .ranlib = "ranlib -D",
        .strip = "strip",
        .extra_cflags =
            " -I" OPENSSL_MACOS "/include"
            " -mdynamic-no-pic",
        .extra_ldflags =
            " -L" OPENSSL_MACOS "/lib"
            " -Wl,-dynamic,-search_paths_first"
            " -Wl,-no_warn_duplicate_libraries",
        .frameworks =
            " -framework Foundation -framework AudioToolbox -framework CoreAudio"
            " -framework AVFoundation -framework CoreVideo -framework CoreMedia"
            " -framework CoreGraphics -framework VideoToolbox"
            " -framework CoreFoundation -framework CoreServices",
        .sys_libs = " -lssl -lcrypto -lm -lz -lbz2 -liconv -lpthread",
        .build_ffplay = true,
        .build_objc = true,
        .is_darwin = true,
        .is_macos = true,
    },
    {
        /* Android arm64. Needs target-specific HAVE_* and ARCH_* values in
         * config.h (current config.h targets macOS). Generate those once via
         * upstream configure on the Android toolchain, then commit.
         */
        .name = "android-arm64",
        .cc = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android" ANDROID_API "-clang",
        .ar = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar",
        .ranlib = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ranlib",
        .strip = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip",
        .extra_cflags =
            " -I" OPENSSL_ANDROID "/include"
            " -fPIC -DANDROID",
        .extra_ldflags =
            " -L" OPENSSL_ANDROID "/lib",
        .frameworks = "",
        /* -lcamera2ndk + -lmediandk need Android API >= 24; omit by default. */
        .sys_libs = " -lssl -lcrypto -lm -lz -llog -landroid",
        .build_ffplay = false,
        .build_objc = false,
        .is_android = true,
    },
    {
        /* iOS arm64. Needs iPhoneOS SDK (via xcrun) and openssl built for iOS.
         * Openssl path must be set below; if missing, cross-compile openssl
         * for ios-arm64 first.
         */
        .name = "ios-arm64",
        .cc = "xcrun --sdk iphoneos clang",
        .ar = "xcrun --sdk iphoneos ar",
        .ranlib = "xcrun --sdk iphoneos ranlib -D",
        .strip = "xcrun --sdk iphoneos strip",
        .extra_cflags =
            " -arch arm64"
            " -miphoneos-version-min=13.0",
        .extra_ldflags = " -arch arm64",
        .frameworks =
            " -framework Foundation -framework AudioToolbox"
            " -framework AVFoundation -framework CoreVideo -framework CoreMedia"
            " -framework CoreGraphics -framework VideoToolbox"
            " -framework CoreFoundation",
        .sys_libs = " -lssl -lcrypto -lm -lz -lbz2 -liconv -lpthread",
        .build_ffplay = false,
        .build_objc = true,
        .is_darwin = true,
    },
    { .name = NULL }
};

/* Source files to skip on platforms where they don't apply. */
static bool skip_source(const char *src, const struct target *t) {
    /* VideoToolbox / AVFoundation are macOS/iOS (Darwin) only */
    if (!t->is_darwin) {
        if (strstr(src, "videotoolbox") ||
            strstr(src, "hwcontext_videotoolbox") ||
            strstr(src, "avfoundation") ||
            strstr(src, "audiotoolbox") ||
            strstr(src, "dispatch_semaphore"))
            return true;
    }
    /* audiotoolbox outdev uses AudioDeviceID which only exists on macOS */
    if (!t->is_macos && strstr(src, "libavdevice/audiotoolbox.m"))
        return true;
    /* MediaCodec + android_camera + hwcontext_mediacodec only on Android */
    if (!t->is_android) {
        if (strstr(src, "mediacodec") ||
            strstr(src, "android_camera") ||
            strstr(src, "ohcodec"))
            return true;
    }
    return false;
}

static const char *COMMON_CFLAGS =
    " -I. -I./"
    " -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"
    " -I./compat/dispatch_semaphore -I./compat/stdbit"
    " -DPIC -DZLIB_CONST"
    " -std=c17 -Werror=partial-availability"
    " -fno-common -fomit-frame-pointer -fPIC -pthread"
    " -g -Wall -Wdisabled-optimization -Wpointer-arith -Wredundant-decls"
    " -Wwrite-strings -Wtype-limits -Wundef -Wempty-body"
    " -Wmissing-prototypes -Wstrict-prototypes -Wunterminated-string-initialization"
    " -Wno-parentheses -Wno-switch -Wno-format-zero-length -Wno-pointer-sign"
    " -Wno-unused-const-variable -Wno-bool-operation -Wno-char-subscripts"
    " -Wno-implicit-const-int-float-conversion -Wno-microsoft-enum-forward-reference"
    " -O3 -fno-math-errno -fno-signed-zeros -mstack-alignment=16"
    " -Qunused-arguments"
    " -Werror=implicit-function-declaration -Werror=missing-prototypes"
    " -Werror=return-type";

static const char *SDL_CFLAGS = " -I/Users/home/.local/include/SDL2 -D_THREAD_SAFE";

static const char *SDL_LIBS =
    " -L/Users/home/.local/lib -lSDL2"
    " -Wl,-framework,Cocoa -Wl,-framework,IOKit -Wl,-framework,ForceFeedback"
    " -Wl,-framework,Carbon"
    " -Wl,-weak_framework,GameController -Wl,-weak_framework,Metal"
    " -Wl,-weak_framework,QuartzCore -Wl,-weak_framework,CoreHaptics";

static const char *FFTOOLS_SHARED_OBJS[] = {
    "fftools/common.o",
    NULL,
};

static const char *FFMPEG_OBJS[]  = { "fftools/ffmpeg.o",  NULL };
static const char *FFPROBE_OBJS[] = { "fftools/ffprobe.o", NULL };
static const char *FFPLAY_OBJS[]  = { "fftools/ffplay.o",  NULL };

static const char *LIB_ORDER[] = {
    "libavdevice", "libavfilter", "libavformat", "libavcodec",
    "libswresample", "libswscale", "libavutil",
    NULL,
};

/* ----- utilities ----- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "build: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static time_t mtime(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return st.st_mtime;
}

static bool needs_rebuild(const char *obj, const char *src) {
    time_t ot = mtime(obj);
    if (ot == 0) return true;
    return mtime(src) > ot;
}

static void mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void obj_path(char *out, size_t n, const char *src) {
    snprintf(out, n, "%s", src);
    char *dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".o");
}

static void ensure_dir_for(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (slash) {
        *slash = 0;
        mkdir_p(buf);
    }
}

/* ----- command runner ----- */

static bool verbose = false;     /* -v: print every command */
static bool quiet = false;       /* -q: suppress CC/AS/AR/LD tag lines */
static bool dry_run = false;     /* -n: print but don't execute */
static bool keep_going = false;  /* -k: don't stop on first failure */

static int run(const char *cmd) {
    if (verbose || dry_run) fprintf(stderr, "$ %s\n", cmd);
    if (dry_run) return 0;
    int rc = system(cmd);
    if (rc == -1) die("system: %s", strerror(errno));
    if (WIFSIGNALED(rc)) die("killed by signal %d", WTERMSIG(rc));
    return WEXITSTATUS(rc);
}

/* Thread-safe alternative to system() for parallel workers:
 * fork + execvp + waitpid on the specific child.
 */
static int run_cmd_mt(const char *cmd) {
    if (dry_run) return 0;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* ----- build graph ----- */

struct job {
    const char *src;
    char out[1024];
    char cmd[8192];
};

static int ncpu(void) {
    int n = 1;
    size_t sz = sizeof n;
#ifdef __APPLE__
    sysctlbyname("hw.ncpu", &n, &sz, NULL, 0);
#else
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    if (v > 0) n = (int)v;
#endif
    return n < 1 ? 1 : n;
}

static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;
static struct job *jobs;
static size_t njobs;
static size_t next_job;
static int job_fail;
static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&job_mu);
        if ((job_fail && !keep_going) || next_job >= njobs) {
            pthread_mutex_unlock(&job_mu);
            return NULL;
        }
        size_t i = next_job++;
        pthread_mutex_unlock(&job_mu);

        if (!quiet) {
            pthread_mutex_lock(&out_mu);
            const char *tag = strstr(jobs[i].src, ".m") ? "OBJCC" :
                              (strstr(jobs[i].src, ".S") ? "AS   " : "CC   ");
            fprintf(stderr, "%s %s\n", tag, jobs[i].src);
            pthread_mutex_unlock(&out_mu);
        }

        int rc = (verbose || dry_run)
            ? (fprintf(stderr, "$ %s\n", jobs[i].cmd), (dry_run ? 0 : run_cmd_mt(jobs[i].cmd)))
            : run_cmd_mt(jobs[i].cmd);
        if (rc != 0) {
            pthread_mutex_lock(&job_mu);
            job_fail = 1;
            pthread_mutex_unlock(&job_mu);
            pthread_mutex_lock(&out_mu);
            fprintf(stderr, "FAILED: %s\n", jobs[i].src);
            fprintf(stderr, "CMD: %s\n", jobs[i].cmd);
            pthread_mutex_unlock(&out_mu);
            if (!keep_going) return NULL;
        }
    }
}

static void run_jobs_parallel(int nthreads) {
    pthread_t ts[64];
    if (nthreads > 64) nthreads = 64;
    if (!quiet) fprintf(stderr, "build: spawning %d workers\n", nthreads);
    for (int i = 0; i < nthreads; i++) {
        int rc = pthread_create(&ts[i], NULL, worker, NULL);
        if (rc != 0) die("pthread_create: %s", strerror(rc));
    }
    for (int i = 0; i < nthreads; i++) pthread_join(ts[i], NULL);
    if (job_fail) die("compile failed");
}

/* ----- compile steps ----- */

static void emit_job(const char *src, const char *lib, const struct target *t,
                     const char *base_cflags) {
    char out[1024];
    obj_path(out, sizeof out, src);
    if (!needs_rebuild(out, src)) return;
    ensure_dir_for(out);

    char lib_def[96] = "";
    if (lib) snprintf(lib_def, sizeof lib_def, " -DHAVE_AV_CONFIG_H -DBUILDING_%s", lib + 3);

    /* Grow job array. */
    jobs = realloc(jobs, (njobs + 1) * sizeof *jobs);
    if (!jobs) die("oom");
    struct job *j = &jobs[njobs++];
    j->src = src;
    snprintf(j->out, sizeof j->out, "%s", out);

    bool is_m = strstr(src, ".m") != NULL;
    bool is_asm = (strstr(src, ".S") || strstr(src, ".asm"));

    /* For sources in subdirs (e.g. libavcodec/bsf/foo.c), add -I./libavcodec/
     * so files can include sibling-dir headers like "bsf.h" (at libavcodec/bsf.h).
     */
    char subdir_inc[256] = "";
    const char *slash1 = strchr(src, '/');
    if (slash1) {
        const char *slash2 = strchr(slash1 + 1, '/');
        if (slash2) {
            int n = (int)(slash1 - src);
            snprintf(subdir_inc, sizeof subdir_inc, " -I./%.*s/", n, src);
        }
    }

    if (is_asm) {
        snprintf(j->cmd, sizeof j->cmd,
            "%s -I. -I./ -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"
            " -I./compat/dispatch_semaphore -DPIC -I./compat/stdbit -DZLIB_CONST%s"
            " %s -fPIC -g -Qunused-arguments -c -o %s %s",
            t->cc, subdir_inc, t->extra_cflags ? t->extra_cflags : "", out, src);
    } else {
        snprintf(j->cmd, sizeof j->cmd,
            "%s%s%s%s%s%s -c -o %s %s",
            t->cc, base_cflags, t->extra_cflags ? t->extra_cflags : "",
            subdir_inc, lib_def, is_m ? " -ObjC" : "",
            out, src);
    }
}

static void queue_lib(const char *lib,
                     const char **c_srcs, const char **m_srcs, const char **s_srcs,
                     const struct target *t, const char *cflags) {
    for (int i = 0; c_srcs && c_srcs[i]; i++) {
        if (skip_source(c_srcs[i], t)) continue;
        emit_job(c_srcs[i], lib, t, cflags);
    }
    if (t->build_objc)
        for (int i = 0; m_srcs && m_srcs[i]; i++) {
            if (skip_source(m_srcs[i], t)) continue;
            emit_job(m_srcs[i], lib, t, cflags);
        }
    for (int i = 0; s_srcs && s_srcs[i]; i++) {
        if (skip_source(s_srcs[i], t)) continue;
        emit_job(s_srcs[i], lib, t, cflags);
    }
}

static void archive_lib(const struct target *t, const char *lib) {
    char arfile[256];
    snprintf(arfile, sizeof arfile, "%s/%s.a", lib, lib);

    /* Collect all .o files for this lib */
    char cmd[64 * 1024];
    int n = snprintf(cmd, sizeof cmd, "%s rc %s", t->ar, arfile);

    const char ***groups[] = {NULL, NULL, NULL};
    if (strcmp(lib, "libavcodec") == 0) {
        groups[0] = (const char ***)&(const char**[]){(const char**)libavcodec_c_srcs};
        groups[1] = (const char ***)&(const char**[]){(const char**)libavcodec_S_srcs};
    }
    /* This approach is ugly — use switch. */

#define ADD_LIB(L) \
    if (strcmp(lib, #L) == 0) { \
        for (int i = 0; L ## _c_srcs[i]; i++) { \
            if (skip_source(L ## _c_srcs[i], t)) continue; \
            char o[1024]; obj_path(o, sizeof o, L ## _c_srcs[i]); \
            n += snprintf(cmd+n, sizeof cmd - n, " %s", o); \
        } \
    }

#define ADD_LIB_M(L) \
    if (strcmp(lib, #L) == 0 && t->build_objc) { \
        for (int i = 0; L ## _m_srcs[i]; i++) { \
            if (skip_source(L ## _m_srcs[i], t)) continue; \
            char o[1024]; obj_path(o, sizeof o, L ## _m_srcs[i]); \
            n += snprintf(cmd+n, sizeof cmd - n, " %s", o); \
        } \
    }

#define ADD_LIB_S(L) \
    if (strcmp(lib, #L) == 0) { \
        for (int i = 0; L ## _S_srcs[i]; i++) { \
            if (skip_source(L ## _S_srcs[i], t)) continue; \
            char o[1024]; obj_path(o, sizeof o, L ## _S_srcs[i]); \
            n += snprintf(cmd+n, sizeof cmd - n, " %s", o); \
        } \
    }

    (void)groups;
    ADD_LIB(libavcodec)  ADD_LIB_S(libavcodec)
    ADD_LIB(libavdevice) ADD_LIB_M(libavdevice)
    ADD_LIB(libavfilter)
    ADD_LIB(libavformat)
    ADD_LIB(libavutil)   ADD_LIB_S(libavutil)
    ADD_LIB(libswresample) ADD_LIB_S(libswresample)
    ADD_LIB(libswscale)  ADD_LIB_S(libswscale)

    if (!dry_run) unlink(arfile);
    if (!quiet) fprintf(stderr, "AR    %s\n", arfile);
    if (run(cmd) != 0) die("ar failed");

    char ranlib_cmd[512];
    snprintf(ranlib_cmd, sizeof ranlib_cmd, "%s %s", t->ranlib, arfile);
    run(ranlib_cmd);
}

/* ----- linking ----- */

static void link_binary(const struct target *t, const char *outname,
                        const char **objs, bool with_sdl) {
    /* On Android, compile stdio_shim.c (fixes stderr/stdin from older
     * openssl builds). */
    if (t->is_android) {
        if (!file_exists("compat/android/stdio_shim.o")) {
            char cc[1024];
            snprintf(cc, sizeof cc,
                "%s -fPIC -O2 -c -o compat/android/stdio_shim.o "
                "compat/android/stdio_shim.c", t->cc);
            if (run(cc) != 0) die("stdio_shim build failed");
        }
    }

    char cmd[64 * 1024];
    int n = snprintf(cmd, sizeof cmd,
        "%s -Llibavcodec -Llibavdevice -Llibavfilter -Llibavformat"
        " -Llibavutil -Llibswscale -Llibswresample%s -o %s_g",
        t->cc, t->extra_ldflags ? t->extra_ldflags : "", outname);
    if (t->is_android)
        n += snprintf(cmd+n, sizeof cmd - n, " compat/android/stdio_shim.o");

    /* Shared fftools helpers always linked */
    for (int i = 0; FFTOOLS_SHARED_OBJS[i]; i++)
        n += snprintf(cmd+n, sizeof cmd - n, " %s", FFTOOLS_SHARED_OBJS[i]);

    /* Tool-specific objs */
    for (int i = 0; objs[i]; i++)
        n += snprintf(cmd+n, sizeof cmd - n, " %s", objs[i]);

    /* Libs in dependency order */
    n += snprintf(cmd+n, sizeof cmd - n,
        " -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil");

    /* System libs + frameworks */
    n += snprintf(cmd+n, sizeof cmd - n, "%s%s",
                  t->frameworks ? t->frameworks : "",
                  t->sys_libs ? t->sys_libs : "");

    if (with_sdl)
        n += snprintf(cmd+n, sizeof cmd - n, "%s", SDL_LIBS);

    if (!quiet) fprintf(stderr, "LD    %s_g\n", outname);
    if (run(cmd) != 0) die("link failed: %s", outname);

    /* Strip. macOS strip -x removes local symbols which breaks runtime
     * resolution for Objective-C frameworks — use plain strip on Darwin. */
    char strip_cmd[512];
    snprintf(strip_cmd, sizeof strip_cmd, "%s -o %s %s_g",
             t->strip, outname, outname);
    if (!quiet) fprintf(stderr, "STRIP %s\n", outname);
    if (run(strip_cmd) != 0) die("strip failed");
}

/* ----- resource generation (html/css → .c as byte array) ----- */

static void bin2c(const char *input, const char *output, const char *varname) {
    if (file_exists(output) && mtime(output) > mtime(input)) return;
    if (dry_run) {
        fprintf(stderr, "BIN2C %s (dry-run, skipped)\n", output);
        return;
    }
    FILE *in = fopen(input, "rb");
    if (!in) die("open %s: %s", input, strerror(errno));
    FILE *out = fopen(output, "wb");
    if (!out) die("open %s: %s", output, strerror(errno));
    if (!quiet) fprintf(stderr, "BIN2C %s\n", output);
    fprintf(out, "const unsigned char ff_%s_data[] = { ", varname);
    unsigned char byte;
    unsigned len = 0;
    while (fread(&byte, 1, 1, in) == 1) {
        fprintf(out, "0x%02x, ", byte);
        len++;
    }
    fprintf(out, "0x00 };\n");
    fprintf(out, "const unsigned int ff_%s_len = %u;\n", varname, len);
    fclose(out);
    fclose(in);
}

static void gen_resources(void) {
    /* The byte-array forms of graph.html / graph.css are pre-baked into
     * fftools/common.c. No generation step required. */
}

/* ----- orchestration ----- */

/* Copy a per-target header into its canonical location. */
static void install_header(const char *dst, const char *src_template, const char *tname) {
    char src[256];
    snprintf(src, sizeof src, src_template, tname);
    if (!file_exists(src))
        die("missing %s (hand-maintained per-target header)", src);

    /* Skip if identical */
    FILE *a = fopen(src, "rb");
    FILE *b = fopen(dst, "rb");
    bool same = false;
    if (a && b) {
        same = true;
        int ca, cb;
        while ((ca = fgetc(a)) == (cb = fgetc(b)) && ca != EOF) {}
        if (ca != EOF || cb != EOF) same = false;
    }
    if (a) fclose(a);
    if (b) fclose(b);
    if (same) return;

    char cmd[512];
    snprintf(cmd, sizeof cmd, "cp %s %s", src, dst);
    if (!quiet) fprintf(stderr, "GEN   %s (from %s)\n", dst, src);
    if (run(cmd) != 0) die("install %s failed", dst);
}

static void install_config_for(const struct target *t) {
    install_header("config.h",             "config-%s.h",             t->name);
    install_header("config_components.h",  "config_components-%s.h",  t->name);
    install_header("libavdevice/indev_list.c",
                   "libavdevice/indev_list-%s.c",  t->name);
    install_header("libavdevice/outdev_list.c",
                   "libavdevice/outdev_list-%s.c", t->name);
    install_header("libavcodec/codec_list.c",
                   "libavcodec/codec_list-%s.c",  t->name);
}

static void print_help(FILE *out) {
    fprintf(out,
        "usage: build [options] [target|command]\n"
        "\n"
        "A single-file C build driver for this FFmpeg fork. Compiles each\n"
        "source via fork+execvp, archives per-library, links the tools.\n"
        "\n"
        "Commands:\n"
        "  <target>          Build for the named target (default: macos-arm64)\n"
        "  clean             Remove all .o / .d / .a files and built binaries\n"
        "  help              Show this help\n"
        "  list-targets      Print known targets\n"
        "\n"
        "Options:\n"
        "  -t, --target T    Select build target (same as giving positional arg)\n"
        "  -j, --jobs N      Parallel workers (default: host cpu count)\n"
        "  -v, --verbose     Print every spawned command\n"
        "  -q, --quiet       Suppress per-file CC/AS/AR/LD tag lines\n"
        "  -n, --dry-run     Print commands without executing anything\n"
        "  -k, --keep-going  Keep compiling other sources after a failure\n"
        "  -l, --list-targets\n"
        "                    Equivalent to the list-targets command\n"
        "  -h, --help        Show this help\n"
        "\n"
        "Targets:\n");
    for (int i = 0; targets[i].name; i++) {
        const struct target *t = &targets[i];
        fprintf(out, "  %-16s %s%s%s\n", t->name,
                t->is_darwin  ? "Darwin "   : "",
                t->is_macos   ? "(macOS)"   : (t->is_darwin ? "(iOS)" : ""),
                t->is_android ? "Android"   : "");
    }
    fprintf(out,
        "\n"
        "Examples:\n"
        "  build                   # build default target in parallel\n"
        "  build -j4 -v            # 4 workers, verbose\n"
        "  build android-arm64     # cross-compile for Android\n"
        "  build -n ios-arm64      # preview iOS build without running\n"
        "  build clean             # remove all artifacts\n"
        "\n");
}

static void list_targets(void) {
    for (int i = 0; targets[i].name; i++)
        printf("%s\n", targets[i].name);
}

static void usage_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "build: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fprintf(stderr, "try 'build -h' for help\n");
    exit(2);
}

static int rm_walk(const char *root, const char *const *exts) {
    int count = 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += rm_walk(path, exts);
        } else {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            for (int i = 0; exts[i]; i++) {
                if (strcmp(dot, exts[i]) == 0) {
                    unlink(path);
                    count++;
                    break;
                }
            }
        }
    }
    closedir(d);
    return count;
}

static void do_clean(void) {
    static const char *exts[] = {".o", ".a", ".d", NULL};
    const char *dirs[] = {
        "libavcodec","libavformat","libavfilter","libavdevice",
        "libavutil","libswscale","libswresample","fftools", NULL
    };
    int total = 0;
    for (int i = 0; dirs[i]; i++) total += rm_walk(dirs[i], exts);
    /* Binaries and generated resource sources */
    const char *bins[] = {
        "ffmpeg","ffmpeg_g","ffprobe","ffprobe_g","ffplay","ffplay_g",
        "fftools/resources/graph.html.c","fftools/resources/graph.css.c",
        NULL
    };
    for (int i = 0; bins[i]; i++)
        if (unlink(bins[i]) == 0) total++;
    if (!quiet) fprintf(stderr, "clean: removed %d files\n", total);
}

/* Parse a value for --flag=X or --flag X / -fX / -f X. Consumes argv[i]
 * (+ argv[i+1] if needed) and returns the value string. Sets *idx to the
 * last index consumed. */
static const char *take_value(int *idx, int argc, char **argv, const char *name) {
    char *s = argv[*idx];
    size_t nlen = strlen(name);
    if (s[0] == '-' && s[1] == '-') {
        char *eq = strchr(s, '=');
        if (eq) return eq + 1;
    } else if (s[0] == '-' && strlen(s) > nlen) {
        /* -jN attached form */
        return s + nlen;
    }
    if (*idx + 1 >= argc)
        usage_err("option '%s' requires a value", name);
    *idx += 1;
    return argv[*idx];
}

static bool arg_is(const char *a, const char *short_name, const char *long_name) {
    if (short_name && strcmp(a, short_name) == 0) return true;
    if (long_name) {
        size_t n = strlen(long_name);
        if (strncmp(a, long_name, n) == 0 &&
            (a[n] == 0 || a[n] == '='))
            return true;
    }
    return false;
}

int main(int argc, char **argv) {
    int jobs_n = ncpu();
    const char *tname = NULL;
    const char *command = NULL;  /* "build" (default), "clean", "list-targets" */

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (arg_is(a, "-h", "--help") || strcmp(a, "help") == 0) {
            print_help(stdout);
            return 0;
        }
        if (arg_is(a, "-l", "--list-targets") || strcmp(a, "list-targets") == 0) {
            list_targets();
            return 0;
        }
        if (arg_is(a, "-v", "--verbose"))   { verbose = true; continue; }
        if (arg_is(a, "-q", "--quiet"))     { quiet = true; continue; }
        if (arg_is(a, "-n", "--dry-run"))   { dry_run = true; continue; }
        if (arg_is(a, "-k", "--keep-going")){ keep_going = true; continue; }
        if (arg_is(a, "-j", "--jobs") || strncmp(a, "-j", 2) == 0) {
            const char *v = take_value(&i, argc, argv,
                strncmp(a, "--", 2) == 0 ? "--jobs" : "-j");
            jobs_n = atoi(v);
            if (jobs_n < 1) usage_err("-j/--jobs expects a positive integer");
            continue;
        }
        if (arg_is(a, "-t", "--target")) {
            tname = take_value(&i, argc, argv,
                strncmp(a, "--", 2) == 0 ? "--target" : "-t");
            continue;
        }
        if (strcmp(a, "clean") == 0) { command = "clean"; continue; }
        if (strcmp(a, "build") == 0) { command = "build"; continue; }
        if (a[0] == '-') usage_err("unknown option: %s", a);
        /* Positional: target name */
        if (tname && strcmp(tname, a) != 0)
            usage_err("target specified twice: %s and %s", tname, a);
        tname = a;
    }

    if (command && strcmp(command, "clean") == 0) {
        do_clean();
        return 0;
    }

    if (!tname) tname = "macos-arm64";

    const struct target *t = NULL;
    for (int i = 0; targets[i].name; i++)
        if (strcmp(targets[i].name, tname) == 0) { t = &targets[i]; break; }
    if (!t) usage_err("unknown target: %s (try --list-targets)", tname);

    if (quiet && verbose) usage_err("--quiet and --verbose are mutually exclusive");

    if (!quiet)
        fprintf(stderr, "build: target=%s jobs=%d%s\n",
                t->name, jobs_n, dry_run ? " (dry-run)" : "");

    /* 0. Install target-specific config.h */
    install_config_for(t);

    /* 1. Generate resources (html/css → C arrays) */
    gen_resources();

    /* 2. Build combined CFLAGS */
    char cflags[4096];
    snprintf(cflags, sizeof cflags, "%s", COMMON_CFLAGS);

    /* 3. Queue all compile jobs */
    queue_lib("libavutil",   libavutil_c_srcs,   NULL,                libavutil_S_srcs, t, cflags);
    queue_lib("libswresample", libswresample_c_srcs, NULL,             libswresample_S_srcs, t, cflags);
    queue_lib("libswscale",  libswscale_c_srcs,  NULL,                libswscale_S_srcs, t, cflags);
    queue_lib("libavcodec",  libavcodec_c_srcs,  NULL,                libavcodec_S_srcs, t, cflags);
    queue_lib("libavformat", libavformat_c_srcs, NULL,                NULL, t, cflags);
    queue_lib("libavfilter", libavfilter_c_srcs, NULL,                NULL, t, cflags);
    queue_lib("libavdevice", libavdevice_c_srcs, libavdevice_m_srcs,  NULL, t, cflags);

    /* fftools — ffplay sources need SDL includes */
    char cflags_sdl[4500];
    snprintf(cflags_sdl, sizeof cflags_sdl, "%s%s", cflags, SDL_CFLAGS);
    for (int i = 0; fftools_c_srcs[i]; i++) {
        const char *s = fftools_c_srcs[i];
        bool is_ffplay = strstr(s, "ffplay") != NULL;
        if (!t->build_ffplay && is_ffplay) continue;
        emit_job(s, NULL, t, is_ffplay ? cflags_sdl : cflags);
    }

    if (!quiet) fprintf(stderr, "build: %zu compile jobs\n", njobs);

    /* 4. Run compiles in parallel */
    run_jobs_parallel(jobs_n);

    /* 5. Archive per lib */
    for (int i = 0; LIB_ORDER[i]; i++)
        archive_lib(t, LIB_ORDER[i]);

    /* 6. Link binaries */
    link_binary(t, "ffmpeg",  FFMPEG_OBJS,  false);
    link_binary(t, "ffprobe", FFPROBE_OBJS, false);
    if (t->build_ffplay)
        link_binary(t, "ffplay",  FFPLAY_OBJS,  true);

    if (!quiet) fprintf(stderr, "build: done.\n");
    return 0;
}
