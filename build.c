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
    const char *strip;
    const char *extra_cflags;   /* appended to COMMON_CFLAGS */
    const char *extra_ldflags;  /* appended to link commands */
    const char *frameworks;     /* Darwin linker frameworks */
    const char *sys_libs;       /* -lfoo for system libs */
    bool build_ffplay;          /* SDL-dependent */
    bool build_objc;            /* .m files (AVFoundation) */
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
    },
    {
        /* Android arm64. Needs target-specific HAVE_* and ARCH_* values in
         * config.h (current config.h targets macOS). Generate those once via
         * upstream configure on the Android toolchain, then commit.
         */
        .name = "android-arm64",
        .cc = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android" ANDROID_API "-clang",
        .ar = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar",
        .strip = ANDROID_NDK "/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip",
        .extra_cflags =
            " -I" OPENSSL_ANDROID "/include"
            " -fPIC -DANDROID",
        .extra_ldflags =
            " -L" OPENSSL_ANDROID "/lib",
        .frameworks = "",
        .sys_libs =
            " -lssl -lcrypto -lm -lz -llog -landroid"
            " -lcamera2ndk -lmediandk",
        .build_ffplay = false,
        .build_objc = false,
    },
    {
        /* iOS arm64. Needs iPhoneOS SDK (via xcrun) and openssl built for iOS.
         * Openssl path must be set below; if missing, cross-compile openssl
         * for ios-arm64 first.
         */
        .name = "ios-arm64",
        .cc = "xcrun --sdk iphoneos clang",
        .ar = "xcrun --sdk iphoneos ar",
        .strip = "xcrun --sdk iphoneos strip",
        .extra_cflags =
            " -arch arm64"
            " -miphoneos-version-min=13.0"
            " -fembed-bitcode",
        .extra_ldflags = " -arch arm64",
        .frameworks =
            " -framework Foundation -framework AudioToolbox"
            " -framework AVFoundation -framework CoreVideo -framework CoreMedia"
            " -framework CoreGraphics -framework VideoToolbox"
            " -framework CoreFoundation",
        .sys_libs = " -lssl -lcrypto -lm -lz -lbz2 -liconv -lpthread",
        .build_ffplay = false,
        .build_objc = true,
    },
    { .name = NULL }
};

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
    "fftools/cmdutils.o",
    "fftools/opt_common.o",
    NULL,
};

static const char *FFMPEG_OBJS[] = {
    "fftools/ffmpeg.o",
    "fftools/ffmpeg_dec.o",
    "fftools/ffmpeg_demux.o",
    "fftools/ffmpeg_enc.o",
    "fftools/ffmpeg_filter.o",
    "fftools/ffmpeg_hw.o",
    "fftools/ffmpeg_mux.o",
    "fftools/ffmpeg_mux_init.o",
    "fftools/ffmpeg_opt.o",
    "fftools/ffmpeg_sched.o",
    "fftools/graph/graphprint.o",
    "fftools/sync_queue.o",
    "fftools/thread_queue.o",
    "fftools/textformat/avtextformat.o",
    "fftools/textformat/tf_compact.o",
    "fftools/textformat/tf_default.o",
    "fftools/textformat/tf_flat.o",
    "fftools/textformat/tf_ini.o",
    "fftools/textformat/tf_json.o",
    "fftools/textformat/tf_mermaid.o",
    "fftools/textformat/tf_xml.o",
    "fftools/textformat/tw_avio.o",
    "fftools/textformat/tw_buffer.o",
    "fftools/textformat/tw_stdout.o",
    "fftools/resources/resman.o",
    "fftools/resources/graph.html.o",
    "fftools/resources/graph.css.o",
    NULL,
};

static const char *FFPROBE_OBJS[] = {
    "fftools/ffprobe.o",
    "fftools/textformat/avtextformat.o",
    "fftools/textformat/tf_compact.o",
    "fftools/textformat/tf_default.o",
    "fftools/textformat/tf_flat.o",
    "fftools/textformat/tf_ini.o",
    "fftools/textformat/tf_json.o",
    "fftools/textformat/tf_mermaid.o",
    "fftools/textformat/tf_xml.o",
    "fftools/textformat/tw_avio.o",
    "fftools/textformat/tw_buffer.o",
    "fftools/textformat/tw_stdout.o",
    NULL,
};

static const char *FFPLAY_OBJS[] = {
    "fftools/ffplay.o",
    "fftools/ffplay_renderer.o",
    NULL,
};

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

static bool verbose = false;

static int run(const char *cmd) {
    if (verbose) fprintf(stderr, "$ %s\n", cmd);
    int rc = system(cmd);
    if (rc == -1) die("system: %s", strerror(errno));
    if (WIFSIGNALED(rc)) die("killed by signal %d", WTERMSIG(rc));
    return WEXITSTATUS(rc);
}

/* Thread-safe alternative to system() for parallel workers:
 * fork + execvp + waitpid on the specific child.
 */
static int run_cmd_mt(const char *cmd) {
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
        if (job_fail || next_job >= njobs) {
            pthread_mutex_unlock(&job_mu);
            return NULL;
        }
        size_t i = next_job++;
        pthread_mutex_unlock(&job_mu);

        pthread_mutex_lock(&out_mu);
        const char *tag = strstr(jobs[i].src, ".m") ? "OBJCC" :
                          (strstr(jobs[i].src, ".S") ? "AS   " : "CC   ");
        fprintf(stderr, "%s %s\n", tag, jobs[i].src);
        pthread_mutex_unlock(&out_mu);

        int rc = run_cmd_mt(jobs[i].cmd);
        if (rc != 0) {
            pthread_mutex_lock(&job_mu);
            job_fail = 1;
            pthread_mutex_unlock(&job_mu);
            fprintf(stderr, "FAILED: %s\n", jobs[i].src);
            fprintf(stderr, "CMD: %s\n", jobs[i].cmd);
            return NULL;
        }
    }
}

static void run_jobs_parallel(int nthreads) {
    pthread_t ts[64];
    if (nthreads > 64) nthreads = 64;
    fprintf(stderr, "build: spawning %d workers\n", nthreads);
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
    for (int i = 0; c_srcs && c_srcs[i]; i++)
        emit_job(c_srcs[i], lib, t, cflags);
    if (t->build_objc)
        for (int i = 0; m_srcs && m_srcs[i]; i++)
            emit_job(m_srcs[i], lib, t, cflags);
    for (int i = 0; s_srcs && s_srcs[i]; i++)
        emit_job(s_srcs[i], lib, t, cflags);
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
            char o[1024]; obj_path(o, sizeof o, L ## _c_srcs[i]); \
            n += snprintf(cmd+n, sizeof cmd - n, " %s", o); \
        } \
    }

#define ADD_LIB_M(L) \
    if (strcmp(lib, #L) == 0 && t->build_objc) { \
        for (int i = 0; L ## _m_srcs[i]; i++) { \
            char o[1024]; obj_path(o, sizeof o, L ## _m_srcs[i]); \
            n += snprintf(cmd+n, sizeof cmd - n, " %s", o); \
        } \
    }

#define ADD_LIB_S(L) \
    if (strcmp(lib, #L) == 0) { \
        for (int i = 0; L ## _S_srcs[i]; i++) { \
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

    unlink(arfile);
    fprintf(stderr, "AR    %s\n", arfile);
    if (run(cmd) != 0) die("ar failed");

    char ranlib[512];
    snprintf(ranlib, sizeof ranlib, "ranlib -D %s", arfile);
    run(ranlib);
}

/* ----- linking ----- */

static void link_binary(const struct target *t, const char *outname,
                        const char **objs, bool with_sdl) {
    char cmd[64 * 1024];
    int n = snprintf(cmd, sizeof cmd,
        "%s -Llibavcodec -Llibavdevice -Llibavfilter -Llibavformat"
        " -Llibavutil -Llibswscale -Llibswresample%s -o %s_g",
        t->cc, t->extra_ldflags ? t->extra_ldflags : "", outname);

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

    fprintf(stderr, "LD    %s_g\n", outname);
    if (run(cmd) != 0) die("link failed: %s", outname);

    /* Strip */
    char strip_cmd[512];
    snprintf(strip_cmd, sizeof strip_cmd, "%s -x -o %s %s_g",
             t->strip, outname, outname);
    fprintf(stderr, "STRIP %s\n", outname);
    if (run(strip_cmd) != 0) die("strip failed");
}

/* ----- resource generation (html/css → .c as byte array) ----- */

static void bin2c(const char *input, const char *output, const char *varname) {
    if (file_exists(output) && mtime(output) > mtime(input)) return;
    FILE *in = fopen(input, "rb");
    if (!in) die("open %s: %s", input, strerror(errno));
    FILE *out = fopen(output, "wb");
    if (!out) die("open %s: %s", output, strerror(errno));
    fprintf(stderr, "BIN2C %s\n", output);
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
    mkdir_p("fftools/resources");
    bin2c("fftools/resources/graph.html", "fftools/resources/graph.html.c", "graph_html");
    bin2c("fftools/resources/graph.css",  "fftools/resources/graph.css.c",  "graph_css");
}

/* ----- orchestration ----- */

static void usage(void) {
    fprintf(stderr, "usage: build [-v] [-j N] [target|clean]\n");
    fprintf(stderr, "targets:\n");
    for (int i = 0; targets[i].name; i++)
        fprintf(stderr, "  %s\n", targets[i].name);
    fprintf(stderr, "  clean   remove all build artifacts\n");
    exit(1);
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
    /* Binaries */
    const char *bins[] = {
        "ffmpeg","ffmpeg_g","ffprobe","ffprobe_g","ffplay","ffplay_g",
        "fftools/resources/graph.html.c","fftools/resources/graph.css.c",
        NULL
    };
    for (int i = 0; bins[i]; i++)
        if (unlink(bins[i]) == 0) total++;
    fprintf(stderr, "clean: removed %d files\n", total);
}

int main(int argc, char **argv) {
    int jobs_n = ncpu();
    const char *tname = "macos-arm64";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = true;
        else if (strncmp(argv[i], "-j", 2) == 0) {
            const char *v = argv[i] + 2;
            if (*v == 0 && i + 1 < argc) v = argv[++i];
            jobs_n = atoi(v);
            if (jobs_n < 1) jobs_n = 1;
        }
        else if (strcmp(argv[i], "-h") == 0) usage();
        else if (strcmp(argv[i], "clean") == 0) { do_clean(); return 0; }
        else tname = argv[i];
    }

    const struct target *t = NULL;
    for (int i = 0; targets[i].name; i++)
        if (strcmp(targets[i].name, tname) == 0) { t = &targets[i]; break; }
    if (!t) die("unknown target: %s", tname);

    fprintf(stderr, "build: target=%s jobs=%d\n", t->name, jobs_n);

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

    fprintf(stderr, "build: %zu compile jobs\n", njobs);

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

    fprintf(stderr, "build: done.\n");
    return 0;
}
