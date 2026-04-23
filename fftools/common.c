/* fftools common
 *
 * Merged from:
 *   - fftools/cmdutils.c
 *   - fftools/opt_common.c
 *   - fftools/sync_queue.c
 *   - fftools/thread_queue.c
 *   - fftools/textformat/avtextformat.c
 *   - fftools/textformat/tf_default.c
 *   - fftools/textformat/tf_compact.c
 *   - fftools/textformat/tf_flat.c
 *   - fftools/textformat/tf_ini.c
 *   - fftools/textformat/tf_json.c
 *   - fftools/textformat/tf_mermaid.c
 *   - fftools/textformat/tf_xml.c
 *   - fftools/textformat/tw_avio.c
 *   - fftools/textformat/tw_buffer.c
 *   - fftools/textformat/tw_stdout.c
 *   - fftools/graph/graphprint.c
 *   - fftools/resources/resman.c
 *   - fftools/resources/graph.html.c
 *   - fftools/resources/graph.css.c
 */


/* ========== fftools/cmdutils.c ========== */

/*
 * Various utilities for command line tools
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

/* Include only the enabled headers since some compilers (namely, Sun
   Studio) will not omit unused inline functions and create undefined
   references to libraries that are not being built. */

#include "config.h"
#include "compat/va_copy.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/display.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/libm.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"
#include "libavutil/eval.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "cmdutils.h"
#include "fopen_utf8.h"
#include "opt_common.h"
#ifdef _WIN32
#include <windows.h>
#include "compat/w32dlfcn.h"
#endif

AVDictionary *sws_dict;
AVDictionary *swr_opts;
AVDictionary *format_opts, *codec_opts;

int hide_banner = 0;

void uninit_opts(void)
{
    av_dict_free(&swr_opts);
    av_dict_free(&sws_dict);
    av_dict_free(&format_opts);
    av_dict_free(&codec_opts);
}

void log_callback_help(void *ptr, int level, const char *fmt, va_list vl)
{
    vfprintf(stdout, fmt, vl);
}

void init_dynload(void)
{
#if HAVE_SETDLLDIRECTORY && defined(_WIN32)
    /* Calling SetDllDirectory with the empty string (but not NULL) removes the
     * current working directory from the DLL search path as a security pre-caution. */
    SetDllDirectory("");
#endif
}

int parse_number(const char *context, const char *numstr, enum OptionType type,
                 double min, double max, double *dst)
{
    char *tail;
    const char *error;
    double d = av_strtod(numstr, &tail);
    if (*tail)
        error = "Expected number for %s but found: %s\n";
    else if (d < min || d > max)
        error = "The value for %s was %s which is not within %f - %f\n";
    else if (type == OPT_TYPE_INT64 && (int64_t)d != d)
        error = "Expected int64 for %s but found %s\n";
    else if (type == OPT_TYPE_INT && (int)d != d)
        error = "Expected int for %s but found %s\n";
    else {
        *dst = d;
        return 0;
    }

    av_log(NULL, AV_LOG_FATAL, error, context, numstr, min, max);
    return AVERROR(EINVAL);
}

void show_help_options(const OptionDef *options, const char *msg, int req_flags,
                       int rej_flags)
{
    const OptionDef *po;
    int first;

    first = 1;
    for (po = options; po->name; po++) {
        char buf[128];

        if (((po->flags & req_flags) != req_flags) ||
            (po->flags & rej_flags))
            continue;

        if (first) {
            printf("%s\n", msg);
            first = 0;
        }
        av_strlcpy(buf, po->name, sizeof(buf));

        if (po->flags & OPT_FLAG_PERSTREAM)
            av_strlcat(buf, "[:<stream_spec>]", sizeof(buf));
        else if (po->flags & OPT_FLAG_SPEC)
            av_strlcat(buf, "[:<spec>]", sizeof(buf));

        if (po->argname)
            av_strlcatf(buf, sizeof(buf), " <%s>", po->argname);

        printf("-%-17s  %s\n", buf, po->help);
    }
    printf("\n");
}

void show_help_children(const AVClass *class, int flags)
{
    void *iter = NULL;
    const AVClass *child;
    if (class->option) {
        av_opt_show2(&class, NULL, flags, 0);
        printf("\n");
    }

    while (child = av_opt_child_class_iterate(class, &iter))
        show_help_children(child, flags);
}

static const OptionDef *find_option(const OptionDef *po, const char *name)
{
    if (*name == '/')
        name++;

    while (po->name) {
        const char *end;
        if (av_strstart(name, po->name, &end) && (!*end || *end == ':'))
            break;
        po++;
    }
    return po;
}

/* _WIN32 means using the windows libc - cygwin doesn't define that
 * by default. HAVE_COMMANDLINETOARGVW is true on cygwin, while
 * it doesn't provide the actual command line via GetCommandLineW(). */
#if HAVE_COMMANDLINETOARGVW && defined(_WIN32)
#include <shellapi.h>
/* Will be leaked on exit */
static char** win32_argv_utf8 = NULL;
static int win32_argc = 0;

/**
 * Prepare command line arguments for executable.
 * For Windows - perform wide-char to UTF-8 conversion.
 * Input arguments should be main() function arguments.
 * @param argc_ptr Arguments number (including executable)
 * @param argv_ptr Arguments list.
 */
static void prepare_app_arguments(int *argc_ptr, char ***argv_ptr)
{
    char *argstr_flat;
    wchar_t **argv_w;
    int i, buffsize = 0, offset = 0;

    if (win32_argv_utf8) {
        *argc_ptr = win32_argc;
        *argv_ptr = win32_argv_utf8;
        return;
    }

    win32_argc = 0;
    argv_w = CommandLineToArgvW(GetCommandLineW(), &win32_argc);
    if (win32_argc <= 0 || !argv_w)
        return;

    /* determine the UTF-8 buffer size (including NULL-termination symbols) */
    for (i = 0; i < win32_argc; i++)
        buffsize += WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1,
                                        NULL, 0, NULL, NULL);

    win32_argv_utf8 = av_mallocz(sizeof(char *) * (win32_argc + 1) + buffsize);
    argstr_flat     = (char *)win32_argv_utf8 + sizeof(char *) * (win32_argc + 1);
    if (!win32_argv_utf8) {
        LocalFree(argv_w);
        return;
    }

    for (i = 0; i < win32_argc; i++) {
        win32_argv_utf8[i] = &argstr_flat[offset];
        offset += WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1,
                                      &argstr_flat[offset],
                                      buffsize - offset, NULL, NULL);
    }
    win32_argv_utf8[i] = NULL;
    LocalFree(argv_w);

    *argc_ptr = win32_argc;
    *argv_ptr = win32_argv_utf8;
}
#else
static inline void prepare_app_arguments(int *argc_ptr, char ***argv_ptr)
{
    /* nothing to do */
}
#endif /* HAVE_COMMANDLINETOARGVW */

static int opt_has_arg(const OptionDef *o)
{
    if (o->type == OPT_TYPE_BOOL)
        return 0;
    if (o->type == OPT_TYPE_FUNC)
        return !!(o->flags & OPT_FUNC_ARG);
    return 1;
}

static int write_option(void *optctx, const OptionDef *po, const char *opt,
                        const char *arg, const OptionDef *defs)
{
    /* new-style options contain an offset into optctx, old-style address of
     * a global var*/
    void *dst = po->flags & OPT_FLAG_OFFSET ?
                (uint8_t *)optctx + po->u.off : po->u.dst_ptr;
    char *arg_allocated = NULL;

    enum OptionType so_type = po->type;

    SpecifierOptList *sol = NULL;
    double num;
    int ret = 0;

    if (*opt == '/') {
        opt++;

        if (!opt_has_arg(po)) {
            av_log(NULL, AV_LOG_FATAL,
                   "Requested to load an argument from file for an option '%s'"
                   " which does not take an argument\n",
                   po->name);
            return AVERROR(EINVAL);
        }

        arg_allocated = read_file_to_string(arg);
        if (!arg_allocated) {
            av_log(NULL, AV_LOG_FATAL,
                   "Error reading the value for option '%s' from file: %s\n",
                   opt, arg);
            return AVERROR(EINVAL);
        }

        arg = arg_allocated;
    }

    if (po->flags & OPT_FLAG_SPEC) {
        const char *p = strchr(opt, ':');
        char *str;

        sol = dst;
        ret = GROW_ARRAY(sol->opt, sol->nb_opt);
        if (ret < 0)
            goto finish;

        str = av_strdup(p ? p + 1 : "");
        if (!str) {
            ret = AVERROR(ENOMEM);
            goto finish;
        }
        sol->opt[sol->nb_opt - 1].specifier = str;

        if (po->flags & OPT_FLAG_PERSTREAM) {
            ret = stream_specifier_parse(&sol->opt[sol->nb_opt - 1].stream_spec,
                                         str, 0, NULL);
            if (ret < 0)
                goto finish;
        }

        dst = &sol->opt[sol->nb_opt - 1].u;
    }

    if (po->type == OPT_TYPE_STRING) {
        char *str;
        if (arg_allocated) {
            str           = arg_allocated;
            arg_allocated = NULL;
        } else
            str = av_strdup(arg);
        av_freep(dst);

        if (!str) {
            ret = AVERROR(ENOMEM);
            goto finish;
        }

        *(char **)dst = str;
    } else if (po->type == OPT_TYPE_BOOL || po->type == OPT_TYPE_INT) {
        ret = parse_number(opt, arg, OPT_TYPE_INT64, INT_MIN, INT_MAX, &num);
        if (ret < 0)
            goto finish;

        *(int *)dst = num;
        so_type = OPT_TYPE_INT;
    } else if (po->type == OPT_TYPE_INT64) {
        ret = parse_number(opt, arg, OPT_TYPE_INT64, INT64_MIN, (double)INT64_MAX, &num);
        if (ret < 0)
            goto finish;

        *(int64_t *)dst = num;
    } else if (po->type == OPT_TYPE_TIME) {
        ret = av_parse_time(dst, arg, 1);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Invalid duration for option %s: %s\n",
                   opt, arg);
            goto finish;
        }
        so_type = OPT_TYPE_INT64;
    } else if (po->type == OPT_TYPE_FLOAT) {
        ret = parse_number(opt, arg, OPT_TYPE_FLOAT, -INFINITY, INFINITY, &num);
        if (ret < 0)
            goto finish;

        *(float *)dst = num;
    } else if (po->type == OPT_TYPE_DOUBLE) {
        ret = parse_number(opt, arg, OPT_TYPE_DOUBLE, -INFINITY, INFINITY, &num);
        if (ret < 0)
            goto finish;

        *(double *)dst = num;
    } else {
        av_assert0(po->type == OPT_TYPE_FUNC && po->u.func_arg);

        ret = po->u.func_arg(optctx, opt, arg);
        if (ret < 0) {
            if ((strcmp(opt, "init_hw_device") != 0) || (strcmp(arg, "list") != 0)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Failed to set value '%s' for option '%s': %s\n",
                       arg, opt, av_err2str(ret));
            }
            goto finish;
        }
    }
    if (po->flags & OPT_EXIT) {
        ret = AVERROR_EXIT;
        goto finish;
    }

    if (sol) {
        sol->type = so_type;
        sol->opt_canon = (po->flags & OPT_HAS_CANON) ?
                         find_option(defs, po->u1.name_canon) : po;
    }

finish:
    av_freep(&arg_allocated);
    return ret;
}

int parse_option(void *optctx, const char *opt, const char *arg,
                 const OptionDef *options)
{
    static const OptionDef opt_avoptions = {
        .name       = "AVOption passthrough",
        .type       = OPT_TYPE_FUNC,
        .flags      = OPT_FUNC_ARG,
        .u.func_arg = opt_default,
    };

    const OptionDef *po;
    int ret;

    po = find_option(options, opt);
    if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
        /* handle 'no' bool option */
        po = find_option(options, opt + 2);
        if ((po->name && po->type == OPT_TYPE_BOOL))
            arg = "0";
    } else if (po->type == OPT_TYPE_BOOL)
        arg = "1";

    if (!po->name)
        po = &opt_avoptions;
    if (!po->name) {
        av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'\n", opt);
        return AVERROR(EINVAL);
    }
    if (opt_has_arg(po) && !arg) {
        av_log(NULL, AV_LOG_ERROR, "Missing argument for option '%s'\n", opt);
        return AVERROR(EINVAL);
    }

    ret = write_option(optctx, po, opt, arg, options);
    if (ret < 0)
        return ret;

    return opt_has_arg(po);
}

int parse_options(void *optctx, int argc, char **argv, const OptionDef *options,
                  int (*parse_arg_function)(void *, const char*))
{
    const char *opt;
    int optindex, handleoptions = 1, ret;

    /* perform system-dependent conversions for arguments list */
    prepare_app_arguments(&argc, &argv);

    /* parse options */
    optindex = 1;
    while (optindex < argc) {
        opt = argv[optindex++];

        if (handleoptions && opt[0] == '-' && opt[1] != '\0') {
            if (opt[1] == '-' && opt[2] == '\0') {
                handleoptions = 0;
                continue;
            }
            opt++;

            if ((ret = parse_option(optctx, opt, argv[optindex], options)) < 0)
                return ret;
            optindex += ret;
        } else {
            if (parse_arg_function) {
                ret = parse_arg_function(optctx, opt);
                if (ret < 0)
                    return ret;
            }
        }
    }

    return 0;
}

int parse_optgroup(void *optctx, OptionGroup *g, const OptionDef *defs)
{
    int i, ret;

    av_log(NULL, AV_LOG_DEBUG, "Parsing a group of options: %s %s.\n",
           g->group_def->name, g->arg);

    for (i = 0; i < g->nb_opts; i++) {
        Option *o = &g->opts[i];

        if (g->group_def->flags &&
            !(g->group_def->flags & o->opt->flags)) {
            av_log(NULL, AV_LOG_ERROR, "Option %s (%s) cannot be applied to "
                   "%s %s -- you are trying to apply an input option to an "
                   "output file or vice versa. Move this option before the "
                   "file it belongs to.\n", o->key, o->opt->help,
                   g->group_def->name, g->arg);
            return AVERROR(EINVAL);
        }

        av_log(NULL, AV_LOG_DEBUG, "Applying option %s (%s) with argument %s.\n",
               o->key, o->opt->help, o->val);

        ret = write_option(optctx, o->opt, o->key, o->val, defs);
        if (ret < 0)
            return ret;
    }

    av_log(NULL, AV_LOG_DEBUG, "Successfully parsed a group of options.\n");

    return 0;
}

int locate_option(int argc, char **argv, const OptionDef *options,
                  const char *optname)
{
    const OptionDef *po;
    int i;

    for (i = 1; i < argc; i++) {
        const char *cur_opt = argv[i];

        if (!(cur_opt[0] == '-' && cur_opt[1]))
            continue;
        cur_opt++;

        po = find_option(options, cur_opt);
        if (!po->name && cur_opt[0] == 'n' && cur_opt[1] == 'o')
            po = find_option(options, cur_opt + 2);

        if ((!po->name && !strcmp(cur_opt, optname)) ||
             (po->name && !strcmp(optname, po->name)))
            return i;

        if (!po->name || opt_has_arg(po))
            i++;
    }
    return 0;
}

static void dump_argument(FILE *report_file, const char *a)
{
    const unsigned char *p;

    for (p = a; *p; p++)
        if (!((*p >= '+' && *p <= ':') || (*p >= '@' && *p <= 'Z') ||
              *p == '_' || (*p >= 'a' && *p <= 'z')))
            break;
    if (!*p) {
        fputs(a, report_file);
        return;
    }
    fputc('"', report_file);
    for (p = a; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '$' || *p == '`')
            fprintf(report_file, "\\%c", *p);
        else if (*p < ' ' || *p > '~')
            fprintf(report_file, "\\x%02x", *p);
        else
            fputc(*p, report_file);
    }
    fputc('"', report_file);
}

static void check_options(const OptionDef *po)
{
    while (po->name) {
        if (po->flags & OPT_PERFILE)
            av_assert0(po->flags & (OPT_INPUT | OPT_OUTPUT | OPT_DECODER));

        if (po->type == OPT_TYPE_FUNC)
            av_assert0(!(po->flags & (OPT_FLAG_OFFSET | OPT_FLAG_SPEC)));

        // OPT_FUNC_ARG can only be ser for OPT_TYPE_FUNC
        av_assert0((po->type == OPT_TYPE_FUNC) || !(po->flags & OPT_FUNC_ARG));

        po++;
    }
}

void parse_loglevel(int argc, char **argv, const OptionDef *options)
{
    int idx;
    char *env;

    check_options(options);

    idx = locate_option(argc, argv, options, "loglevel");
    if (!idx)
        idx = locate_option(argc, argv, options, "v");
    if (idx && argv[idx + 1])
        opt_loglevel(NULL, "loglevel", argv[idx + 1]);
    idx = locate_option(argc, argv, options, "report");
    env = getenv_utf8("FFREPORT");
    if (env || idx) {
        FILE *report_file = NULL;
        init_report(env, &report_file);
        if (report_file) {
            int i;
            fprintf(report_file, "Command line:\n");
            for (i = 0; i < argc; i++) {
                dump_argument(report_file, argv[i]);
                fputc(i < argc - 1 ? ' ' : '\n', report_file);
            }
            fflush(report_file);
        }
    }
    freeenv_utf8(env);
    idx = locate_option(argc, argv, options, "hide_banner");
    if (idx)
        hide_banner = 1;
}

static const AVOption *opt_find(void *obj, const char *name, const char *unit,
                            int opt_flags, int search_flags)
{
    const AVOption *o = av_opt_find(obj, name, unit, opt_flags, search_flags);
    if(o && !o->flags)
        return NULL;
    return o;
}

#define FLAGS ((o->type == AV_OPT_TYPE_FLAGS && (arg[0]=='-' || arg[0]=='+')) ? AV_DICT_APPEND : 0)
int opt_default(void *optctx, const char *opt, const char *arg)
{
    const AVOption *o;
    int consumed = 0;
    char opt_stripped[128];
    const char *p;
    const AVClass *cc = avcodec_get_class(), *fc = avformat_get_class();
#if CONFIG_SWSCALE
    const AVClass *sc = sws_get_class();
#endif
#if CONFIG_SWRESAMPLE
    const AVClass *swr_class = swr_get_class();
#endif

    if (!strcmp(opt, "debug") || !strcmp(opt, "fdebug"))
        av_log_set_level(AV_LOG_DEBUG);

    if (!(p = strchr(opt, ':')))
        p = opt + strlen(opt);
    av_strlcpy(opt_stripped, opt, FFMIN(sizeof(opt_stripped), p - opt + 1));

    if ((o = opt_find(&cc, opt_stripped, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ)) ||
        ((opt[0] == 'v' || opt[0] == 'a' || opt[0] == 's') &&
         (o = opt_find(&cc, opt + 1, NULL, 0, AV_OPT_SEARCH_FAKE_OBJ)))) {
        av_dict_set(&codec_opts, opt, arg, FLAGS);
        consumed = 1;
    }
    if ((o = opt_find(&fc, opt, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        av_dict_set(&format_opts, opt, arg, FLAGS);
        if (consumed)
            av_log(NULL, AV_LOG_VERBOSE, "Routing option %s to both codec and muxer layer\n", opt);
        consumed = 1;
    }
#if CONFIG_SWSCALE
    if (!consumed && (o = opt_find(&sc, opt, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        if (!strcmp(opt, "srcw") || !strcmp(opt, "srch") ||
            !strcmp(opt, "dstw") || !strcmp(opt, "dsth") ||
            !strcmp(opt, "src_format") || !strcmp(opt, "dst_format")) {
            av_log(NULL, AV_LOG_ERROR, "Directly using swscale dimensions/format options is not supported, please use the -s or -pix_fmt options\n");
            return AVERROR(EINVAL);
        }
        av_dict_set(&sws_dict, opt, arg, FLAGS);

        consumed = 1;
    }
#else
    if (!consumed && !strcmp(opt, "sws_flags")) {
        av_log(NULL, AV_LOG_WARNING, "Ignoring %s %s, due to disabled swscale\n", opt, arg);
        consumed = 1;
    }
#endif
#if CONFIG_SWRESAMPLE
    if (!consumed && (o=opt_find(&swr_class, opt, NULL, 0,
                                    AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        av_dict_set(&swr_opts, opt, arg, FLAGS);
        consumed = 1;
    }
#endif

    if (consumed)
        return 0;
    return AVERROR_OPTION_NOT_FOUND;
}

/*
 * Check whether given option is a group separator.
 *
 * @return index of the group definition that matched or -1 if none
 */
static int match_group_separator(const OptionGroupDef *groups, int nb_groups,
                                 const char *opt)
{
    int i;

    for (i = 0; i < nb_groups; i++) {
        const OptionGroupDef *p = &groups[i];
        if (p->sep && !strcmp(p->sep, opt))
            return i;
    }

    return -1;
}

/*
 * Finish parsing an option group.
 *
 * @param group_idx which group definition should this group belong to
 * @param arg argument of the group delimiting option
 */
static int finish_group(OptionParseContext *octx, int group_idx,
                        const char *arg)
{
    OptionGroupList *l = &octx->groups[group_idx];
    OptionGroup *g;
    int ret;

    ret = GROW_ARRAY(l->groups, l->nb_groups);
    if (ret < 0)
        return ret;

    g = &l->groups[l->nb_groups - 1];

    *g             = octx->cur_group;
    g->arg         = arg;
    g->group_def   = l->group_def;
    g->sws_dict    = sws_dict;
    g->swr_opts    = swr_opts;
    g->codec_opts  = codec_opts;
    g->format_opts = format_opts;

    codec_opts  = NULL;
    format_opts = NULL;
    sws_dict    = NULL;
    swr_opts    = NULL;

    memset(&octx->cur_group, 0, sizeof(octx->cur_group));

    return ret;
}

/*
 * Add an option instance to currently parsed group.
 */
static int add_opt(OptionParseContext *octx, const OptionDef *opt,
                   const char *key, const char *val)
{
    int global = !(opt->flags & OPT_PERFILE);
    OptionGroup *g = global ? &octx->global_opts : &octx->cur_group;
    int ret;

    ret = GROW_ARRAY(g->opts, g->nb_opts);
    if (ret < 0)
        return ret;

    g->opts[g->nb_opts - 1].opt = opt;
    g->opts[g->nb_opts - 1].key = key;
    g->opts[g->nb_opts - 1].val = val;

    return 0;
}

static int init_parse_context(OptionParseContext *octx,
                              const OptionGroupDef *groups, int nb_groups)
{
    static const OptionGroupDef global_group = { "global" };
    int i;

    memset(octx, 0, sizeof(*octx));

    octx->groups    = av_calloc(nb_groups, sizeof(*octx->groups));
    if (!octx->groups)
        return AVERROR(ENOMEM);
    octx->nb_groups = nb_groups;

    for (i = 0; i < octx->nb_groups; i++)
        octx->groups[i].group_def = &groups[i];

    octx->global_opts.group_def = &global_group;
    octx->global_opts.arg       = "";

    return 0;
}

void uninit_parse_context(OptionParseContext *octx)
{
    int i, j;

    for (i = 0; i < octx->nb_groups; i++) {
        OptionGroupList *l = &octx->groups[i];

        for (j = 0; j < l->nb_groups; j++) {
            av_freep(&l->groups[j].opts);
            av_dict_free(&l->groups[j].codec_opts);
            av_dict_free(&l->groups[j].format_opts);

            av_dict_free(&l->groups[j].sws_dict);
            av_dict_free(&l->groups[j].swr_opts);
        }
        av_freep(&l->groups);
    }
    av_freep(&octx->groups);

    av_freep(&octx->cur_group.opts);
    av_freep(&octx->global_opts.opts);

    uninit_opts();
}

int split_commandline(OptionParseContext *octx, int argc, char *argv[],
                      const OptionDef *options,
                      const OptionGroupDef *groups, int nb_groups)
{
    int ret;
    int optindex = 1;
    int dashdash = -2;

    /* perform system-dependent conversions for arguments list */
    prepare_app_arguments(&argc, &argv);

    ret = init_parse_context(octx, groups, nb_groups);
    if (ret < 0)
        return ret;

    av_log(NULL, AV_LOG_DEBUG, "Splitting the commandline.\n");

    while (optindex < argc) {
        const char *opt = argv[optindex++], *arg;
        const OptionDef *po;
        int group_idx;

        av_log(NULL, AV_LOG_DEBUG, "Reading option '%s' ...", opt);

        if (opt[0] == '-' && opt[1] == '-' && !opt[2]) {
            dashdash = optindex;
            continue;
        }
        /* unnamed group separators, e.g. output filename */
        if (opt[0] != '-' || !opt[1] || dashdash+1 == optindex) {
            ret = finish_group(octx, 0, opt);
            if (ret < 0)
                return ret;

            av_log(NULL, AV_LOG_DEBUG, " matched as %s.\n", groups[0].name);
            continue;
        }
        opt++;

#define GET_ARG(arg)                                                           \
do {                                                                           \
    arg = argv[optindex++];                                                    \
    if (!arg) {                                                                \
        av_log(NULL, AV_LOG_ERROR, "Missing argument for option '%s'.\n", opt);\
        return AVERROR(EINVAL);                                                \
    }                                                                          \
} while (0)

        /* named group separators, e.g. -i */
        group_idx = match_group_separator(groups, nb_groups, opt);
        if (group_idx >= 0) {
            GET_ARG(arg);
            ret = finish_group(octx, group_idx, arg);
            if (ret < 0)
                return ret;

            av_log(NULL, AV_LOG_DEBUG, " matched as %s with argument '%s'.\n",
                   groups[group_idx].name, arg);
            continue;
        }

        /* normal options */
        po = find_option(options, opt);
        if (po->name) {
            if (po->flags & OPT_EXIT) {
                /* optional argument, e.g. -h */
                arg = argv[optindex++];
            } else if (opt_has_arg(po)) {
                GET_ARG(arg);
            } else {
                arg = "1";
            }

            ret = add_opt(octx, po, opt, arg);
            if (ret < 0)
                return ret;

            av_log(NULL, AV_LOG_DEBUG, " matched as option '%s' (%s) with "
                   "argument '%s'.\n", po->name, po->help, arg);
            continue;
        }

        /* AVOptions */
        if (argv[optindex]) {
            ret = opt_default(NULL, opt, argv[optindex]);
            if (ret >= 0) {
                av_log(NULL, AV_LOG_DEBUG, " matched as AVOption '%s' with "
                       "argument '%s'.\n", opt, argv[optindex]);
                optindex++;
                continue;
            } else if (ret != AVERROR_OPTION_NOT_FOUND) {
                av_log(NULL, AV_LOG_ERROR, "Error parsing option '%s' "
                       "with argument '%s'.\n", opt, argv[optindex]);
                return ret;
            }
        }

        /* boolean -nofoo options */
        if (opt[0] == 'n' && opt[1] == 'o' &&
            (po = find_option(options, opt + 2)) &&
            po->name && po->type == OPT_TYPE_BOOL) {
            ret = add_opt(octx, po, opt, "0");
            if (ret < 0)
                return ret;

            av_log(NULL, AV_LOG_DEBUG, " matched as option '%s' (%s) with "
                   "argument 0.\n", po->name, po->help);
            continue;
        }

        av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'.\n", opt);
        return AVERROR_OPTION_NOT_FOUND;
    }

    if (octx->cur_group.nb_opts || codec_opts || format_opts)
        av_log(NULL, AV_LOG_WARNING, "Trailing option(s) found in the "
               "command: may be ignored.\n");

    av_log(NULL, AV_LOG_DEBUG, "Finished splitting the commandline.\n");

    return 0;
}

int read_yesno(void)
{
    int c = getchar();
    int yesno = (av_toupper(c) == 'Y');

    while (c != '\n' && c != EOF)
        c = getchar();

    return yesno;
}

FILE *get_preset_file(char *filename, size_t filename_size,
                      const char *preset_name, int is_path,
                      const char *codec_name)
{
    FILE *f = NULL;
    int i;
#if HAVE_GETMODULEHANDLE && defined(_WIN32)
    char *datadir = NULL;
#endif
    char *env_home = getenv_utf8("HOME");
    char *env_ffmpeg_datadir = getenv_utf8("FFMPEG_DATADIR");
    const char *base[3] = { env_ffmpeg_datadir,
                            env_home,   /* index=1(HOME) is special: search in a .ffmpeg subfolder */
                            FFMPEG_DATADIR, };

    if (is_path) {
        av_strlcpy(filename, preset_name, filename_size);
        f = fopen_utf8(filename, "r");
    } else {
#if HAVE_GETMODULEHANDLE && defined(_WIN32)
        wchar_t *datadir_w = get_module_filename(NULL);
        base[2] = NULL;

        if (wchartoutf8(datadir_w, &datadir))
            datadir = NULL;
        av_free(datadir_w);

        if (datadir)
        {
            char *ls;
            for (ls = datadir; *ls; ls++)
                if (*ls == '\\') *ls = '/';

            if (ls = strrchr(datadir, '/'))
            {
                ptrdiff_t datadir_len = ls - datadir;
                size_t desired_size = datadir_len + strlen("/ffpresets") + 1;
                char *new_datadir = av_realloc_array(
                    datadir, desired_size, sizeof *datadir);
                if (new_datadir) {
                    datadir = new_datadir;
                    strcpy(datadir + datadir_len, "/ffpresets");
                    base[2] = datadir;
                }
            }
        }
#endif
        for (i = 0; i < 3 && !f; i++) {
            if (!base[i])
                continue;
            snprintf(filename, filename_size, "%s%s/%s.ffpreset", base[i],
                     i != 1 ? "" : "/.ffmpeg", preset_name);
            f = fopen_utf8(filename, "r");
            if (!f && codec_name) {
                snprintf(filename, filename_size,
                         "%s%s/%s-%s.ffpreset",
                         base[i], i != 1 ? "" : "/.ffmpeg", codec_name,
                         preset_name);
                f = fopen_utf8(filename, "r");
            }
        }
    }

#if HAVE_GETMODULEHANDLE && defined(_WIN32)
    av_free(datadir);
#endif
    freeenv_utf8(env_ffmpeg_datadir);
    freeenv_utf8(env_home);
    return f;
}

int cmdutils_isalnum(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

void stream_specifier_uninit(StreamSpecifier *ss)
{
    av_freep(&ss->meta_key);
    av_freep(&ss->meta_val);
    av_freep(&ss->remainder);

    memset(ss, 0, sizeof(*ss));
}

int stream_specifier_parse(StreamSpecifier *ss, const char *spec,
                           int allow_remainder, void *logctx)
{
    char *endptr;
    int ret;

    memset(ss, 0, sizeof(*ss));

    ss->idx         = -1;
    ss->media_type  = AVMEDIA_TYPE_UNKNOWN;
    ss->stream_list = STREAM_LIST_ALL;

    av_log(logctx, AV_LOG_TRACE, "Parsing stream specifier: %s\n", spec);

    while (*spec) {
        if (*spec <= '9' && *spec >= '0') { /* opt:index */
            ss->idx = strtol(spec, &endptr, 0);

            av_assert0(endptr > spec);
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed index: %d; remainder: %s\n", ss->idx, spec);

            // this terminates the specifier
            break;
        } else if ((*spec == 'v' || *spec == 'a' || *spec == 's' ||
                    *spec == 'd' || *spec == 't' || *spec == 'V') &&
                   !cmdutils_isalnum(*(spec + 1))) { /* opt:[vasdtV] */
            if (ss->media_type != AVMEDIA_TYPE_UNKNOWN) {
                av_log(logctx, AV_LOG_ERROR, "Stream type specified multiple times\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            switch (*spec++) {
            case 'v': ss->media_type = AVMEDIA_TYPE_VIDEO;      break;
            case 'a': ss->media_type = AVMEDIA_TYPE_AUDIO;      break;
            case 's': ss->media_type = AVMEDIA_TYPE_SUBTITLE;   break;
            case 'd': ss->media_type = AVMEDIA_TYPE_DATA;       break;
            case 't': ss->media_type = AVMEDIA_TYPE_ATTACHMENT; break;
            case 'V': ss->media_type = AVMEDIA_TYPE_VIDEO;
                      ss->no_apic    = 1;                       break;
            default:  av_assert0(0);
            }

            av_log(logctx, AV_LOG_TRACE, "Parsed media type: %s; remainder: %s\n",
                   av_get_media_type_string(ss->media_type), spec);
        } else if (*spec == 'g' && *(spec + 1) == ':') {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            spec += 2;
            if (*spec == '#' || (*spec == 'i' && *(spec + 1) == ':')) {
                ss->stream_list = STREAM_LIST_GROUP_ID;

                spec += 1 + (*spec == 'i');
            } else
                ss->stream_list = STREAM_LIST_GROUP_IDX;

            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected stream group idx/ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE, "Parsed stream group %s: %"PRId64"; remainder: %s\n",
                   ss->stream_list == STREAM_LIST_GROUP_ID ? "ID" : "index", ss->list_id, spec);
        } else if (*spec == 'p' && *(spec + 1) == ':') {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            ss->stream_list = STREAM_LIST_PROGRAM;

            spec += 2;
            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected program ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed program ID: %"PRId64"; remainder: %s\n", ss->list_id, spec);
        } else if (!strncmp(spec, "disp:", 5)) {
            const AVClass *st_class = av_stream_get_class();
            const AVOption       *o = av_opt_find(&st_class, "disposition", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
            char *disp = NULL;
            size_t len;

            av_assert0(o);

            if (ss->disposition) {
                av_log(logctx, AV_LOG_ERROR, "Multiple disposition specifiers\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            spec += 5;

            for (len = 0; cmdutils_isalnum(spec[len]) ||
                          spec[len] == '_' || spec[len] == '+'; len++)
                continue;

            disp = av_strndup(spec, len);
            if (!disp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            ret = av_opt_eval_flags(&st_class, o, disp, &ss->disposition);
            av_freep(&disp);
            if (ret < 0) {
                av_log(logctx, AV_LOG_ERROR, "Invalid disposition specifier\n");
                goto fail;
            }

            spec += len;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed disposition: 0x%x; remainder: %s\n", ss->disposition, spec);
        } else if (*spec == '#' ||
                   (*spec == 'i' && *(spec + 1) == ':')) {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            ss->stream_list = STREAM_LIST_STREAM_ID;

            spec += 1 + (*spec == 'i');
            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected stream ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed stream ID: %"PRId64"; remainder: %s\n", ss->list_id, spec);

            // this terminates the specifier
            break;
        } else if (*spec == 'm' && *(spec + 1) == ':') {
            av_assert0(!ss->meta_key && !ss->meta_val);

            spec += 2;
            ss->meta_key = av_get_token(&spec, ":");
            if (!ss->meta_key) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            if (*spec == ':') {
                spec++;
                ss->meta_val = av_get_token(&spec, ":");
                if (!ss->meta_val) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            }

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed metadata: %s:%s; remainder: %s", ss->meta_key,
                   ss->meta_val ? ss->meta_val : "<any value>", spec);

            // this terminates the specifier
            break;
        } else if (*spec == 'u' && (*(spec + 1) == '\0' || *(spec + 1) == ':')) {
            ss->usable_only = 1;
            spec++;
            av_log(logctx, AV_LOG_ERROR, "Parsed 'usable only'\n");

            // this terminates the specifier
            break;
        } else
            break;

        if (*spec == ':')
            spec++;
    }

    if (*spec) {
        if (!allow_remainder) {
            av_log(logctx, AV_LOG_ERROR,
                   "Trailing garbage at the end of a stream specifier: %s\n",
                   spec);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        if (*spec == ':')
            spec++;

        ss->remainder = av_strdup(spec);
        if (!ss->remainder) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    return 0;

multiple_stream_lists:
    av_log(logctx, AV_LOG_ERROR,
           "Cannot combine multiple program/group designators in a "
           "single stream specifier");
    ret = AVERROR(EINVAL);

fail:
    stream_specifier_uninit(ss);
    return ret;
}

unsigned stream_specifier_match(const StreamSpecifier *ss,
                                const AVFormatContext *s, const AVStream *st,
                                void *logctx)
{
    const AVStreamGroup *g = NULL;
    const AVProgram *p = NULL;
    int start_stream = 0, nb_streams;
    int nb_matched = 0;

    switch (ss->stream_list) {
    case STREAM_LIST_STREAM_ID:
        // <n-th> stream with given ID makes no sense and should be impossible to request
        av_assert0(ss->idx < 0);
        // return early if we know for sure the stream does not match
        if (st->id != ss->list_id)
            return 0;
        start_stream = st->index;
        nb_streams   = st->index + 1;
        break;
    case STREAM_LIST_ALL:
        start_stream = ss->idx >= 0 ? 0 : st->index;
        nb_streams   = st->index + 1;
        break;
    case STREAM_LIST_PROGRAM:
        for (unsigned i = 0; i < s->nb_programs; i++) {
            if (s->programs[i]->id == ss->list_id) {
                p          = s->programs[i];
                break;
            }
        }
        if (!p) {
            av_log(logctx, AV_LOG_WARNING, "No program with ID %"PRId64" exists,"
                   " stream specifier can never match\n", ss->list_id);
            return 0;
        }
        nb_streams = p->nb_stream_indexes;
        break;
    case STREAM_LIST_GROUP_ID:
        for (unsigned i = 0; i < s->nb_stream_groups; i++) {
            if (ss->list_id == s->stream_groups[i]->id) {
                g = s->stream_groups[i];
                break;
            }
        }
        // fall-through
    case STREAM_LIST_GROUP_IDX:
        if (ss->stream_list == STREAM_LIST_GROUP_IDX &&
            ss->list_id >= 0 && ss->list_id < s->nb_stream_groups)
            g = s->stream_groups[ss->list_id];

        if (!g) {
            av_log(logctx, AV_LOG_WARNING, "No stream group with group %s %"
                   PRId64" exists, stream specifier can never match\n",
                   ss->stream_list == STREAM_LIST_GROUP_ID ? "ID" : "index",
                   ss->list_id);
            return 0;
        }
        nb_streams = g->nb_streams;
        break;
    default: av_assert0(0);
    }

    for (int i = start_stream; i < nb_streams; i++) {
        const AVStream *candidate = s->streams[g ? g->streams[i]->index :
                                               p ? p->stream_index[i]   : i];

        if (ss->media_type != AVMEDIA_TYPE_UNKNOWN &&
            (ss->media_type != candidate->codecpar->codec_type ||
             (ss->no_apic && (candidate->disposition & AV_DISPOSITION_ATTACHED_PIC))))
            continue;

        if (ss->meta_key) {
            const AVDictionaryEntry *tag = av_dict_get(candidate->metadata,
                                                       ss->meta_key, NULL, 0);

            if (!tag)
                continue;
            if (ss->meta_val && strcmp(tag->value, ss->meta_val))
                continue;
        }

        if (ss->usable_only) {
            const AVCodecParameters *par = candidate->codecpar;

            switch (par->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (!par->sample_rate || !par->ch_layout.nb_channels ||
                    par->format == AV_SAMPLE_FMT_NONE)
                    continue;
                break;
            case AVMEDIA_TYPE_VIDEO:
                if (!par->width || !par->height || par->format == AV_PIX_FMT_NONE)
                    continue;
                break;
            case AVMEDIA_TYPE_UNKNOWN:
                continue;
            }
        }

        if (ss->disposition &&
            (candidate->disposition & ss->disposition) != ss->disposition)
            continue;

        if (st == candidate)
            return ss->idx < 0 || ss->idx == nb_matched;

        nb_matched++;
    }

    return 0;
}

int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
    StreamSpecifier ss;
    int ret;

    ret = stream_specifier_parse(&ss, spec, 0, NULL);
    if (ret < 0)
        return ret;

    ret = stream_specifier_match(&ss, s, st, NULL);
    stream_specifier_uninit(&ss);
    return ret;
}

unsigned stream_group_specifier_match(const StreamSpecifier *ss,
                                      const AVFormatContext *s, const AVStreamGroup *stg,
                                      void *logctx)
{
    int start_stream_group = 0, nb_stream_groups;
    int nb_matched = 0;

    if (ss->idx >= 0)
        return 0;

    switch (ss->stream_list) {
    case STREAM_LIST_STREAM_ID:
    case STREAM_LIST_ALL:
    case STREAM_LIST_PROGRAM:
        return 0;
    case STREAM_LIST_GROUP_ID:
        // <n-th> stream with given ID makes no sense and should be impossible to request
        av_assert0(ss->idx < 0);
        // return early if we know for sure the stream does not match
        if (stg->id != ss->list_id)
            return 0;
        start_stream_group = stg->index;
        nb_stream_groups   = stg->index + 1;
        break;
    case STREAM_LIST_GROUP_IDX:
        start_stream_group = ss->list_id >= 0 ? 0 : stg->index;
        nb_stream_groups   = stg->index + 1;
        break;
    default: av_assert0(0);
    }

    for (int i = start_stream_group; i < nb_stream_groups; i++) {
        const AVStreamGroup *candidate = s->stream_groups[i];

        if (ss->meta_key) {
            const AVDictionaryEntry *tag = av_dict_get(candidate->metadata,
                                                       ss->meta_key, NULL, 0);

            if (!tag)
                continue;
            if (ss->meta_val && strcmp(tag->value, ss->meta_val))
                continue;
        }

        if (ss->usable_only) {
            switch (candidate->type) {
            case AV_STREAM_GROUP_PARAMS_TILE_GRID: {
                const AVStreamGroupTileGrid *tg = candidate->params.tile_grid;
                if (!tg->coded_width || !tg->coded_height || !tg->nb_tiles ||
                    !tg->width       || !tg->height       || !tg->nb_tiles)
                    continue;
                break;
            }
            default:
                continue;
            }
        }

        if (ss->disposition &&
            (candidate->disposition & ss->disposition) != ss->disposition)
            continue;

        if (stg == candidate)
            return ss->list_id < 0 || ss->list_id == nb_matched;

        nb_matched++;
    }

    return 0;
}

int filter_codec_opts(const AVDictionary *opts, enum AVCodecID codec_id,
                      AVFormatContext *s, AVStream *st, const AVCodec *codec,
                      AVDictionary **dst, AVDictionary **opts_used)
{
    AVDictionary    *ret = NULL;
    const AVDictionaryEntry *t = NULL;
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                                      : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;
    const AVClass    *cc = avcodec_get_class();

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix  = 'v';
        flags  |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix  = 'a';
        flags  |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix  = 's';
        flags  |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    }

    while (t = av_dict_iterate(opts, t)) {
        const AVClass *priv_class;
        char *p = strchr(t->key, ':');
        int used = 0;

        /* check stream specification in opt name */
        if (p) {
            int err = check_stream_specifier(s, st, p + 1);
            if (err < 0) {
                av_dict_free(&ret);
                return err;
            } else if (!err)
                continue;

            *p = 0;
        }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            ((priv_class = codec->priv_class) &&
             av_opt_find(&priv_class, t->key, NULL, flags,
                         AV_OPT_SEARCH_FAKE_OBJ))) {
            av_dict_set(&ret, t->key, t->value, 0);
            used = 1;
        } else if (t->key[0] == prefix &&
                 av_opt_find(&cc, t->key + 1, NULL, flags,
                             AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&ret, t->key + 1, t->value, 0);
            used = 1;
        }

        if (p)
            *p = ':';

        if (used && opts_used)
            av_dict_set(opts_used, t->key, "", 0);
    }

    *dst = ret;
    return 0;
}

int setup_find_stream_info_opts(AVFormatContext *s,
                                AVDictionary *local_codec_opts,
                                AVDictionary ***dst)
{
    int ret;
    AVDictionary **opts;

    *dst = NULL;

    if (!s->nb_streams)
        return 0;

    opts = av_calloc(s->nb_streams, sizeof(*opts));
    if (!opts)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_streams; i++) {
        ret = filter_codec_opts(local_codec_opts, s->streams[i]->codecpar->codec_id,
                                s, s->streams[i], NULL, &opts[i], NULL);
        if (ret < 0)
            goto fail;
    }
    *dst = opts;
    return 0;
fail:
    for (int i = 0; i < s->nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);
    return ret;
}

int grow_array(void **array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        return AVERROR(ERANGE);
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(*array, new_size, elem_size);
        if (!tmp)
            return AVERROR(ENOMEM);
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        *array = tmp;
        return 0;
    }
    return 0;
}

void *allocate_array_elem(void *ptr, size_t elem_size, int *nb_elems)
{
    void *new_elem;

    new_elem = av_mallocz(elem_size);
    if (!new_elem)
        return NULL;
    if (av_dynarray_add_nofree(ptr, nb_elems, new_elem) < 0)
        av_freep(&new_elem);

    return new_elem;
}

double get_rotation(const int32_t *displaymatrix)
{
    double theta = 0;
    if (displaymatrix)
        theta = -round(av_display_rotation_get(displaymatrix));

    theta -= 360*floor(theta/360 + 0.9/360);

    if (fabs(theta - 90*round(theta/90)) > 2)
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
               "If you want to help, upload a sample "
               "of this file to https://streams.videolan.org/upload/ "
               "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

    return theta;
}

/* read file contents into a string */
char *read_file_to_string(const char *filename)
{
    AVIOContext *pb      = NULL;
    int ret = avio_open(&pb, filename, AVIO_FLAG_READ);
    AVBPrint bprint;
    char *str;

    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error opening file %s.\n", filename);
        return NULL;
    }

    av_bprint_init(&bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
    ret = avio_read_to_bprint(pb, &bprint, SIZE_MAX);
    avio_closep(&pb);
    if (ret < 0) {
        av_bprint_finalize(&bprint, NULL);
        return NULL;
    }
    ret = av_bprint_finalize(&bprint, &str);
    if (ret < 0)
        return NULL;
    return str;
}

void remove_avoptions(AVDictionary **a, AVDictionary *b)
{
    const AVDictionaryEntry *t = NULL;

    while ((t = av_dict_iterate(b, t))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

int check_avoptions(AVDictionary *m)
{
    const AVDictionaryEntry *t = av_dict_iterate(m, NULL);
    if (t) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }

    return 0;
}

void dump_dictionary(void *ctx, const AVDictionary *m,
                     const char *name, const char *indent,
                     int log_level)
{
    const AVDictionaryEntry *tag = NULL;

    if (!m)
        return;

    av_log(ctx, log_level, "%s%s:\n", indent, name);
    while ((tag = av_dict_iterate(m, tag))) {
        const char *p = tag->value;
        av_log(ctx, log_level, "%s  %-16s: ", indent, tag->key);
        while (*p) {
            size_t len = strcspn(p, "\x8\xa\xb\xc\xd");
            av_log(ctx, log_level, "%.*s", (int)(FFMIN(255, len)), p);
            p += len;
            if (*p == 0xd) av_log(ctx, log_level, " ");
            if (*p == 0xa) av_log(ctx, log_level, "\n%s  %-16s: ", indent, "");
            if (*p) p++;
        }
        av_log(ctx, log_level, "\n");
    }
}


/* ========== fftools/opt_common.c ========== */

/*
 * Option handlers shared between the tools.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stdio.h>

#include "cmdutils.h"
#include "fopen_utf8.h"
#include "opt_common.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/cpu.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/ffversion.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/version.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavcodec/codec.h"
#include "libavcodec/codec_desc.h"
#include "libavcodec/version.h"

#include "libavformat/avformat.h"
#include "libavformat/version.h"

#include "libavdevice/avdevice.h"
#include "libavdevice/version.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/version.h"

#include "libswscale/swscale.h"
#include "libswscale/version.h"

#include "libswresample/swresample.h"
#include "libswresample/version.h"


enum show_muxdemuxers {
    SHOW_DEFAULT,
    SHOW_DEMUXERS,
    SHOW_MUXERS,
};

static FILE *report_file;
static int report_file_level = AV_LOG_DEBUG;

int show_license(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_NONFREE
    printf(
    "This version of %s has nonfree parts compiled in.\n"
    "Therefore it is not legally redistributable.\n",
    program_name );
#elif CONFIG_GPLV3
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation; either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with %s.  If not, see <http://www.gnu.org/licenses/>.\n",
    program_name, program_name, program_name );
#elif CONFIG_GPL
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation; either version 2 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with %s; if not, write to the Free Software\n"
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA\n",
    program_name, program_name, program_name );
#elif CONFIG_LGPLV3
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU Lesser General Public License as published by\n"
    "the Free Software Foundation; either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU Lesser General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Lesser General Public License\n"
    "along with %s.  If not, see <http://www.gnu.org/licenses/>.\n",
    program_name, program_name, program_name );
#else
    printf(
    "%s is free software; you can redistribute it and/or\n"
    "modify it under the terms of the GNU Lesser General Public\n"
    "License as published by the Free Software Foundation; either\n"
    "version 2.1 of the License, or (at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n"
    "Lesser General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Lesser General Public\n"
    "License along with %s; if not, write to the Free Software\n"
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA\n",
    program_name, program_name, program_name );
#endif

    return 0;
}

static int warned_cfg = 0;

#define INDENT        1
#define SHOW_VERSION  2
#define SHOW_CONFIG   4
#define SHOW_COPYRIGHT 8

#define PRINT_LIB_INFO(libname, LIBNAME, flags, level)                  \
    if (CONFIG_##LIBNAME) {                                             \
        const char *indent = flags & INDENT? "  " : "";                 \
        if (flags & SHOW_VERSION) {                                     \
            unsigned int version = libname##_version();                 \
            av_log(NULL, level,                                         \
                   "%slib%-11s %2d.%3d.%3d / %2d.%3d.%3d\n",            \
                   indent, #libname,                                    \
                   LIB##LIBNAME##_VERSION_MAJOR,                        \
                   LIB##LIBNAME##_VERSION_MINOR,                        \
                   LIB##LIBNAME##_VERSION_MICRO,                        \
                   AV_VERSION_MAJOR(version), AV_VERSION_MINOR(version),\
                   AV_VERSION_MICRO(version));                          \
        }                                                               \
        if (flags & SHOW_CONFIG) {                                      \
            const char *cfg = libname##_configuration();                \
            if (strcmp(FFMPEG_CONFIGURATION, cfg)) {                    \
                if (!warned_cfg) {                                      \
                    av_log(NULL, level,                                 \
                            "%sWARNING: library configuration mismatch\n", \
                            indent);                                    \
                    warned_cfg = 1;                                     \
                }                                                       \
                av_log(NULL, level, "%s%-11s configuration: %s\n",      \
                        indent, #libname, cfg);                         \
            }                                                           \
        }                                                               \
    }                                                                   \

static void print_all_libs_info(int flags, int level)
{
    PRINT_LIB_INFO(avutil,     AVUTIL,     flags, level);
    PRINT_LIB_INFO(avcodec,    AVCODEC,    flags, level);
    PRINT_LIB_INFO(avformat,   AVFORMAT,   flags, level);
    PRINT_LIB_INFO(avdevice,   AVDEVICE,   flags, level);
    PRINT_LIB_INFO(avfilter,   AVFILTER,   flags, level);
    PRINT_LIB_INFO(swscale,    SWSCALE,    flags, level);
    PRINT_LIB_INFO(swresample, SWRESAMPLE, flags, level);
}

static void print_program_info(int flags, int level)
{
    const char *indent = flags & INDENT? "  " : "";

    av_log(NULL, level, "%s version " FFMPEG_VERSION, program_name);
    if (flags & SHOW_COPYRIGHT)
        av_log(NULL, level, " Copyright (c) %d-%d the FFmpeg developers",
               program_birth_year, CONFIG_THIS_YEAR);
    av_log(NULL, level, "\n");
    av_log(NULL, level, "%sbuilt with %s\n", indent, CC_IDENT);

    av_log(NULL, level, "%sconfiguration: " FFMPEG_CONFIGURATION "\n", indent);
}

static void print_buildconf(int flags, int level)
{
    const char *indent = flags & INDENT ? "  " : "";
    char str[] = { FFMPEG_CONFIGURATION };
    char *conflist, *remove_tilde, *splitconf;

    // Change all the ' --' strings to '~--' so that
    // they can be identified as tokens.
    while ((conflist = strstr(str, " --")) != NULL) {
        conflist[0] = '~';
    }

    // Compensate for the weirdness this would cause
    // when passing 'pkg-config --static'.
    while ((remove_tilde = strstr(str, "pkg-config~")) != NULL) {
        remove_tilde[sizeof("pkg-config~") - 2] = ' ';
    }

    splitconf = strtok(str, "~");
    av_log(NULL, level, "\n%sconfiguration:\n", indent);
    while (splitconf != NULL) {
        av_log(NULL, level, "%s%s%s\n", indent, indent, splitconf);
        splitconf = strtok(NULL, "~");
    }
}

void show_banner(int argc, char **argv, const OptionDef *options)
{
    int idx = locate_option(argc, argv, options, "version");
    if (hide_banner || idx)
        return;

    print_program_info (INDENT|SHOW_COPYRIGHT, AV_LOG_INFO);
    print_all_libs_info(INDENT|SHOW_CONFIG,  AV_LOG_INFO);
    print_all_libs_info(INDENT|SHOW_VERSION, AV_LOG_INFO);
}

int show_version(void *optctx, const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    print_program_info (SHOW_COPYRIGHT, AV_LOG_INFO);
    print_all_libs_info(SHOW_VERSION, AV_LOG_INFO);

    return 0;
}

int show_buildconf(void *optctx, const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    print_buildconf      (INDENT|0, AV_LOG_INFO);

    return 0;
}

#define PRINT_CODEC_SUPPORTED(codec, config, type, name, elem, fmt, ...)        \
    do {                                                                        \
        int num = 0;                                                            \
        const type *elem = NULL;                                                \
        avcodec_get_supported_config(NULL, codec, config, 0,                    \
                                     (const void **) &elem, &num);              \
        if (elem) {                                                             \
            printf("    Supported " name ":");                                  \
            for (int i = 0; i < num; i++) {                                     \
                printf(" " fmt, __VA_ARGS__);                                   \
                elem++;                                                         \
            }                                                                   \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static const char *get_channel_layout_desc(const AVChannelLayout *layout, AVBPrint *bp)
{
    int ret;
    av_bprint_clear(bp);
    ret = av_channel_layout_describe_bprint(layout, bp);
    if (!av_bprint_is_complete(bp) || ret < 0)
        return "unknown/invalid";
    return bp->str;
}

static void print_codec(const AVCodec *c)
{
    int encoder = av_codec_is_encoder(c);
    AVBPrint desc;

    printf("%s %s [%s]:\n", encoder ? "Encoder" : "Decoder", c->name,
           c->long_name ? c->long_name : "");

    printf("    General capabilities: ");
    if (c->capabilities & AV_CODEC_CAP_DRAW_HORIZ_BAND)
        printf("horizband ");
    if (c->capabilities & AV_CODEC_CAP_DR1)
        printf("dr1 ");
    if (c->capabilities & AV_CODEC_CAP_DELAY)
        printf("delay ");
    if (c->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)
        printf("small ");
    if (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL)
        printf("exp ");
    if (c->capabilities & AV_CODEC_CAP_CHANNEL_CONF)
        printf("chconf ");
    if (c->capabilities & AV_CODEC_CAP_PARAM_CHANGE)
        printf("paramchange ");
    if (c->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        printf("variable ");
    if (c->capabilities & (AV_CODEC_CAP_FRAME_THREADS |
                           AV_CODEC_CAP_SLICE_THREADS |
                           AV_CODEC_CAP_OTHER_THREADS))
        printf("threads ");
    if (c->capabilities & AV_CODEC_CAP_AVOID_PROBING)
        printf("avoidprobe ");
    if (c->capabilities & AV_CODEC_CAP_HARDWARE)
        printf("hardware ");
    if (c->capabilities & AV_CODEC_CAP_HYBRID)
        printf("hybrid ");
    if (!c->capabilities)
        printf("none");
    printf("\n");

    if (c->type == AVMEDIA_TYPE_VIDEO ||
        c->type == AVMEDIA_TYPE_AUDIO) {
        printf("    Threading capabilities: ");
        switch (c->capabilities & (AV_CODEC_CAP_FRAME_THREADS |
                                   AV_CODEC_CAP_SLICE_THREADS |
                                   AV_CODEC_CAP_OTHER_THREADS)) {
        case AV_CODEC_CAP_FRAME_THREADS |
             AV_CODEC_CAP_SLICE_THREADS: printf("frame and slice"); break;
        case AV_CODEC_CAP_FRAME_THREADS: printf("frame");           break;
        case AV_CODEC_CAP_SLICE_THREADS: printf("slice");           break;
        case AV_CODEC_CAP_OTHER_THREADS: printf("other");           break;
        default:                         printf("none");            break;
        }
        printf("\n");
    }

    if (avcodec_get_hw_config(c, 0)) {
        printf("    Supported hardware devices: ");
        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
            const char *name;
            if (!config)
                break;
            name = av_hwdevice_get_type_name(config->device_type);
            if (name)
                printf("%s ", name);
        }
        printf("\n");
    }

    PRINT_CODEC_SUPPORTED(c, AV_CODEC_CONFIG_FRAME_RATE, AVRational, "framerates",
                          fps, "%d/%d", fps->num, fps->den);
    PRINT_CODEC_SUPPORTED(c, AV_CODEC_CONFIG_PIX_FORMAT, enum AVPixelFormat,
                          "pixel formats", fmt,  "%s", av_get_pix_fmt_name(*fmt));
    PRINT_CODEC_SUPPORTED(c, AV_CODEC_CONFIG_SAMPLE_RATE, int, "sample rates",
                          rate, "%d", *rate);
    PRINT_CODEC_SUPPORTED(c, AV_CODEC_CONFIG_SAMPLE_FORMAT, enum AVSampleFormat,
                          "sample formats", fmt, "%s", av_get_sample_fmt_name(*fmt));

    av_bprint_init(&desc, 0, AV_BPRINT_SIZE_AUTOMATIC);
    PRINT_CODEC_SUPPORTED(c, AV_CODEC_CONFIG_CHANNEL_LAYOUT, AVChannelLayout,
                          "channel layouts", layout, "%s",
                          get_channel_layout_desc(layout, &desc));
    av_bprint_finalize(&desc, NULL);

    if (c->priv_class) {
        show_help_children(c->priv_class,
                           AV_OPT_FLAG_ENCODING_PARAM |
                           AV_OPT_FLAG_DECODING_PARAM);
    }
}

static const AVCodec *next_codec_for_id(enum AVCodecID id, void **iter,
                                        int encoder)
{
    const AVCodec *c;
    while ((c = av_codec_iterate(iter))) {
        if (c->id == id &&
            (encoder ? av_codec_is_encoder(c) : av_codec_is_decoder(c)))
            return c;
    }
    return NULL;
}

static void show_help_codec(const char *name, int encoder)
{
    const AVCodecDescriptor *desc;
    const AVCodec *codec;

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No codec name specified.\n");
        return;
    }

    codec = encoder ? avcodec_find_encoder_by_name(name) :
                      avcodec_find_decoder_by_name(name);

    if (codec)
        print_codec(codec);
    else if ((desc = avcodec_descriptor_get_by_name(name))) {
        void *iter = NULL;
        int printed = 0;

        while ((codec = next_codec_for_id(desc->id, &iter, encoder))) {
            printed = 1;
            print_codec(codec);
        }

        if (!printed) {
            av_log(NULL, AV_LOG_ERROR, "Codec '%s' is known to FFmpeg, "
                   "but no %s for it are available. FFmpeg might need to be "
                   "recompiled with additional external libraries.\n",
                   name, encoder ? "encoders" : "decoders");
        }
    } else {
        av_log(NULL, AV_LOG_ERROR, "Codec '%s' is not recognized by FFmpeg.\n",
               name);
    }
}

static void show_help_demuxer(const char *name)
{
    const AVInputFormat *fmt = av_find_input_format(name);

    if (!fmt) {
        av_log(NULL, AV_LOG_ERROR, "Unknown format '%s'.\n", name);
        return;
    }

    printf("Demuxer %s [%s]:\n", fmt->name, fmt->long_name);

    if (fmt->extensions)
        printf("    Common extensions: %s.\n", fmt->extensions);

    if (fmt->priv_class)
        show_help_children(fmt->priv_class, AV_OPT_FLAG_DECODING_PARAM);
}

static void show_help_protocol(const char *name)
{
    const AVClass *proto_class;

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No protocol name specified.\n");
        return;
    }

    proto_class = avio_protocol_get_class(name);
    if (!proto_class) {
        av_log(NULL, AV_LOG_ERROR, "Unknown protocol '%s'.\n", name);
        return;
    }

    show_help_children(proto_class, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM);
}

static void show_help_muxer(const char *name)
{
    const AVCodecDescriptor *desc;
    const AVOutputFormat *fmt = av_guess_format(name, NULL, NULL);

    if (!fmt) {
        av_log(NULL, AV_LOG_ERROR, "Unknown format '%s'.\n", name);
        return;
    }

    printf("Muxer %s [%s]:\n", fmt->name, fmt->long_name);

    if (fmt->extensions)
        printf("    Common extensions: %s.\n", fmt->extensions);
    if (fmt->mime_type)
        printf("    Mime type: %s.\n", fmt->mime_type);
    if (fmt->video_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->video_codec))) {
        printf("    Default video codec: %s.\n", desc->name);
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->audio_codec))) {
        printf("    Default audio codec: %s.\n", desc->name);
    }
    if (fmt->subtitle_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->subtitle_codec))) {
        printf("    Default subtitle codec: %s.\n", desc->name);
    }

    if (fmt->priv_class)
        show_help_children(fmt->priv_class, AV_OPT_FLAG_ENCODING_PARAM);
}

#if CONFIG_AVFILTER
static void show_help_filter(const char *name)
{
#if CONFIG_AVFILTER
    const AVFilter *f = avfilter_get_by_name(name);
    int i, count;

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No filter name specified.\n");
        return;
    } else if (!f) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter '%s'.\n", name);
        return;
    }

    printf("Filter %s\n", f->name);
    if (f->description)
        printf("  %s\n", f->description);

    if (f->flags & AVFILTER_FLAG_SLICE_THREADS)
        printf("    slice threading supported\n");

    printf("    Inputs:\n");
    count = avfilter_filter_pad_count(f, 0);
    for (i = 0; i < count; i++) {
        printf("       #%d: %s (%s)\n", i, avfilter_pad_get_name(f->inputs, i),
               av_get_media_type_string(avfilter_pad_get_type(f->inputs, i)));
    }
    if (f->flags & AVFILTER_FLAG_DYNAMIC_INPUTS)
        printf("        dynamic (depending on the options)\n");
    else if (!count)
        printf("        none (source filter)\n");

    printf("    Outputs:\n");
    count = avfilter_filter_pad_count(f, 1);
    for (i = 0; i < count; i++) {
        printf("       #%d: %s (%s)\n", i, avfilter_pad_get_name(f->outputs, i),
               av_get_media_type_string(avfilter_pad_get_type(f->outputs, i)));
    }
    if (f->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS)
        printf("        dynamic (depending on the options)\n");
    else if (!count)
        printf("        none (sink filter)\n");

    if (f->priv_class)
        show_help_children(f->priv_class, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM |
                                          AV_OPT_FLAG_AUDIO_PARAM);
    if (f->flags & AVFILTER_FLAG_SUPPORT_TIMELINE)
        printf("This filter has support for timeline through the 'enable' option.\n");
#else
    av_log(NULL, AV_LOG_ERROR, "Build without libavfilter; "
           "can not to satisfy request\n");
#endif
}
#endif

static void show_help_bsf(const char *name)
{
    const AVBitStreamFilter *bsf = av_bsf_get_by_name(name);

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No bitstream filter name specified.\n");
        return;
    } else if (!bsf) {
        av_log(NULL, AV_LOG_ERROR, "Unknown bit stream filter '%s'.\n", name);
        return;
    }

    printf("Bit stream filter %s\n", bsf->name);
    if (bsf->codec_ids) {
        const enum AVCodecID *id = bsf->codec_ids;
        printf("    Supported codecs:");
        while (*id != AV_CODEC_ID_NONE) {
            printf(" %s", avcodec_descriptor_get(*id)->name);
            id++;
        }
        printf("\n");
    }
    if (bsf->priv_class)
        show_help_children(bsf->priv_class, AV_OPT_FLAG_BSF_PARAM);
}

int show_help(void *optctx, const char *opt, const char *arg)
{
    char *topic, *par;
    av_log_set_callback(log_callback_help);

    topic = av_strdup(arg ? arg : "");
    if (!topic)
        return AVERROR(ENOMEM);
    par = strchr(topic, '=');
    if (par)
        *par++ = 0;

    if (!*topic) {
        show_help_default(topic, par);
    } else if (!strcmp(topic, "decoder")) {
        show_help_codec(par, 0);
    } else if (!strcmp(topic, "encoder")) {
        show_help_codec(par, 1);
    } else if (!strcmp(topic, "demuxer")) {
        show_help_demuxer(par);
    } else if (!strcmp(topic, "muxer")) {
        show_help_muxer(par);
    } else if (!strcmp(topic, "protocol")) {
        show_help_protocol(par);
#if CONFIG_AVFILTER
    } else if (!strcmp(topic, "filter")) {
        show_help_filter(par);
#endif
    } else if (!strcmp(topic, "bsf")) {
        show_help_bsf(par);
    } else {
        show_help_default(topic, par);
    }

    av_freep(&topic);
    return 0;
}

static void print_codecs_for_id(enum AVCodecID id, int encoder)
{
    void *iter = NULL;
    const AVCodec *codec;

    printf(" (%s:", encoder ? "encoders" : "decoders");

    while ((codec = next_codec_for_id(id, &iter, encoder)))
        printf(" %s", codec->name);

    printf(")");
}

static int compare_codec_desc(const void *a, const void *b)
{
    const AVCodecDescriptor * const *da = a;
    const AVCodecDescriptor * const *db = b;

    return (*da)->type != (*db)->type ? FFDIFFSIGN((*da)->type, (*db)->type) :
           strcmp((*da)->name, (*db)->name);
}

static int get_codecs_sorted(const AVCodecDescriptor ***rcodecs)
{
    const AVCodecDescriptor *desc = NULL;
    const AVCodecDescriptor **codecs;
    unsigned nb_codecs = 0, i = 0;

    while ((desc = avcodec_descriptor_next(desc)))
        nb_codecs++;
    if (!(codecs = av_calloc(nb_codecs, sizeof(*codecs))))
        return AVERROR(ENOMEM);
    desc = NULL;
    while ((desc = avcodec_descriptor_next(desc)))
        codecs[i++] = desc;
    av_assert0(i == nb_codecs);
    qsort(codecs, nb_codecs, sizeof(*codecs), compare_codec_desc);
    *rcodecs = codecs;
    return nb_codecs;
}

static char get_media_type_char(enum AVMediaType type)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:    return 'V';
        case AVMEDIA_TYPE_AUDIO:    return 'A';
        case AVMEDIA_TYPE_DATA:     return 'D';
        case AVMEDIA_TYPE_SUBTITLE: return 'S';
        case AVMEDIA_TYPE_ATTACHMENT:return 'T';
        default:                    return '?';
    }
}

int show_codecs(void *optctx, const char *opt, const char *arg)
{
    const AVCodecDescriptor **codecs;
    unsigned i;
    int nb_codecs = get_codecs_sorted(&codecs);

    if (nb_codecs < 0)
        return nb_codecs;

    printf("Codecs:\n"
           " D..... = Decoding supported\n"
           " .E.... = Encoding supported\n"
           " ..V... = Video codec\n"
           " ..A... = Audio codec\n"
           " ..S... = Subtitle codec\n"
           " ..D... = Data codec\n"
           " ..T... = Attachment codec\n"
           " ...I.. = Intra frame-only codec\n"
           " ....L. = Lossy compression\n"
           " .....S = Lossless compression\n"
           " -------\n");
    for (i = 0; i < nb_codecs; i++) {
        const AVCodecDescriptor *desc = codecs[i];
        const AVCodec *codec;
        void *iter = NULL;

        if (strstr(desc->name, "_deprecated"))
            continue;

        printf(" %c%c%c%c%c%c",
               avcodec_find_decoder(desc->id) ? 'D' : '.',
               avcodec_find_encoder(desc->id) ? 'E' : '.',
               get_media_type_char(desc->type),
               (desc->props & AV_CODEC_PROP_INTRA_ONLY) ? 'I' : '.',
               (desc->props & AV_CODEC_PROP_LOSSY)      ? 'L' : '.',
               (desc->props & AV_CODEC_PROP_LOSSLESS)   ? 'S' : '.');

        printf(" %-20s %s", desc->name, desc->long_name ? desc->long_name : "");

        /* print decoders/encoders when there's more than one or their
         * names are different from codec name */
        while ((codec = next_codec_for_id(desc->id, &iter, 0))) {
            if (strcmp(codec->name, desc->name)) {
                print_codecs_for_id(desc->id, 0);
                break;
            }
        }
        iter = NULL;
        while ((codec = next_codec_for_id(desc->id, &iter, 1))) {
            if (strcmp(codec->name, desc->name)) {
                print_codecs_for_id(desc->id, 1);
                break;
            }
        }

        printf("\n");
    }
    av_free(codecs);
    return 0;
}

static int print_codecs(int encoder)
{
    const AVCodecDescriptor **codecs;
    int i, nb_codecs = get_codecs_sorted(&codecs);

    if (nb_codecs < 0)
        return nb_codecs;

    printf("%s:\n"
           " V..... = Video\n"
           " A..... = Audio\n"
           " S..... = Subtitle\n"
           " .F.... = Frame-level multithreading\n"
           " ..S... = Slice-level multithreading\n"
           " ...X.. = Codec is experimental\n"
           " ....B. = Supports draw_horiz_band\n"
           " .....D = Supports direct rendering method 1\n"
           " ------\n",
           encoder ? "Encoders" : "Decoders");
    for (i = 0; i < nb_codecs; i++) {
        const AVCodecDescriptor *desc = codecs[i];
        const AVCodec *codec;
        void *iter = NULL;

        while ((codec = next_codec_for_id(desc->id, &iter, encoder))) {
            printf(" %c%c%c%c%c%c",
                   get_media_type_char(desc->type),
                   (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)   ? 'F' : '.',
                   (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)   ? 'S' : '.',
                   (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL)    ? 'X' : '.',
                   (codec->capabilities & AV_CODEC_CAP_DRAW_HORIZ_BAND) ? 'B' : '.',
                   (codec->capabilities & AV_CODEC_CAP_DR1)             ? 'D' : '.');

            printf(" %-20s %s", codec->name, codec->long_name ? codec->long_name : "");
            if (strcmp(codec->name, desc->name))
                printf(" (codec %s)", desc->name);

            printf("\n");
        }
    }
    av_free(codecs);
    return 0;
}

int show_decoders(void *optctx, const char *opt, const char *arg)
{
    return print_codecs(0);
}

int show_encoders(void *optctx, const char *opt, const char *arg)
{
    return print_codecs(1);
}

int show_bsfs(void *optctx, const char *opt, const char *arg)
{
    const AVBitStreamFilter *bsf = NULL;
    void *opaque = NULL;

    printf("Bitstream filters:\n");
    while ((bsf = av_bsf_iterate(&opaque)))
        printf("%s\n", bsf->name);
    printf("\n");
    return 0;
}

int show_filters(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_AVFILTER
    const AVFilter *filter = NULL;
    char descr[64], *descr_cur;
    void *opaque = NULL;
    int i, j;
    const AVFilterPad *pad;

    printf("Filters:\n"
           "  T.. = Timeline support\n"
           "  .S. = Slice threading\n"
           "  A = Audio input/output\n"
           "  V = Video input/output\n"
           "  N = Dynamic number and/or type of input/output\n"
           "  | = Source or sink filter\n"
           "  ------\n");
    while ((filter = av_filter_iterate(&opaque))) {
        descr_cur = descr;
        for (i = 0; i < 2; i++) {
            unsigned nb_pads;
            if (i) {
                *(descr_cur++) = '-';
                *(descr_cur++) = '>';
            }
            pad = i ? filter->outputs : filter->inputs;
            nb_pads = avfilter_filter_pad_count(filter, i);
            for (j = 0; j < nb_pads; j++) {
                if (descr_cur >= descr + sizeof(descr) - 4)
                    break;
                *(descr_cur++) = get_media_type_char(avfilter_pad_get_type(pad, j));
            }
            if (!j)
                *(descr_cur++) = ((!i && (filter->flags & AVFILTER_FLAG_DYNAMIC_INPUTS)) ||
                                  ( i && (filter->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS))) ? 'N' : '|';
        }
        *descr_cur = 0;
        printf(" %c%c %-17s %-10s %s\n",
               filter->flags & AVFILTER_FLAG_SUPPORT_TIMELINE ? 'T' : '.',
               filter->flags & AVFILTER_FLAG_SLICE_THREADS    ? 'S' : '.',
               filter->name, descr, filter->description);
    }
#else
    printf("No filters available: libavfilter disabled\n");
#endif
    return 0;
}

static int is_device(const AVClass *avclass)
{
    if (!avclass)
        return 0;
    return AV_IS_INPUT_DEVICE(avclass->category) || AV_IS_OUTPUT_DEVICE(avclass->category);
}

static int show_formats_devices(void *optctx, const char *opt, const char *arg, int device_only, int muxdemuxers)
{
    void *ifmt_opaque = NULL;
    const AVInputFormat *ifmt  = NULL;
    void *ofmt_opaque = NULL;
    const AVOutputFormat *ofmt = NULL;
    const char *last_name;
    int is_dev;
    const char *is_device_placeholder = device_only ? "" : ".";

    printf("%s:\n"
           " D.%s = Demuxing supported\n"
           " .E%s = Muxing supported\n"
           "%s"
           " ---\n",
           device_only ? "Devices" : "Formats",
           is_device_placeholder, is_device_placeholder,
           device_only ? "": " ..d = Is a device\n");

    last_name = "000";
    for (;;) {
        int decode = 0;
        int encode = 0;
        int device = 0;
        const char *name      = NULL;
        const char *long_name = NULL;

        if (muxdemuxers !=SHOW_DEMUXERS) {
            ofmt_opaque = NULL;
            while ((ofmt = av_muxer_iterate(&ofmt_opaque))) {
                is_dev = is_device(ofmt->priv_class);
                if (!is_dev && device_only)
                    continue;
                if ((!name || strcmp(ofmt->name, name) < 0) &&
                    strcmp(ofmt->name, last_name) > 0) {
                    name      = ofmt->name;
                    long_name = ofmt->long_name;
                    encode    = 1;
                    device    = is_dev;
                }
            }
        }
        if (muxdemuxers != SHOW_MUXERS) {
            ifmt_opaque = NULL;
            while ((ifmt = av_demuxer_iterate(&ifmt_opaque))) {
                is_dev = is_device(ifmt->priv_class);
                if (!is_dev && device_only)
                    continue;
                if ((!name || strcmp(ifmt->name, name) < 0) &&
                    strcmp(ifmt->name, last_name) > 0) {
                    name      = ifmt->name;
                    long_name = ifmt->long_name;
                    encode    = 0;
                    device    = is_dev;
                }
                if (name && strcmp(ifmt->name, name) == 0) {
                    decode = 1;
                    device = is_dev;
                }
            }
        }
        if (!name)
            break;
        last_name = name;

        printf(" %c%c%s %-15s %s\n",
               decode ? 'D' : ' ',
               encode ? 'E' : ' ',
               device_only ? "" : (device ? "d" : " "),
               name,
            long_name ? long_name : " ");
    }
    return 0;
}

int show_formats(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_DEFAULT);
}

int show_muxers(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_MUXERS);
}

int show_demuxers(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_DEMUXERS);
}

int show_devices(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 1, SHOW_DEFAULT);
}

int show_protocols(void *optctx, const char *opt, const char *arg)
{
    void *opaque = NULL;
    const char *name;

    printf("Supported file protocols:\n"
           "Input:\n");
    while ((name = avio_enum_protocols(&opaque, 0)))
        printf("  %s\n", name);
    printf("Output:\n");
    while ((name = avio_enum_protocols(&opaque, 1)))
        printf("  %s\n", name);
    return 0;
}

int show_colors(void *optctx, const char *opt, const char *arg)
{
    const char *name;
    const uint8_t *rgb;
    int i;

    printf("%-32s #RRGGBB\n", "name");

    for (i = 0; name = av_get_known_color_name(i, &rgb); i++)
        printf("%-32s #%02x%02x%02x\n", name, rgb[0], rgb[1], rgb[2]);

    return 0;
}

int show_pix_fmts(void *optctx, const char *opt, const char *arg)
{
    const AVPixFmtDescriptor *pix_desc = NULL;

    printf("Pixel formats:\n"
           "I.... = Supported Input  format for conversion\n"
           ".O... = Supported Output format for conversion\n"
           "..H.. = Hardware accelerated format\n"
           "...P. = Paletted format\n"
           "....B = Bitstream format\n"
           "FLAGS NAME            NB_COMPONENTS BITS_PER_PIXEL BIT_DEPTHS\n"
           "-----\n");

#if !CONFIG_SWSCALE
#   define sws_isSupportedInput(x)  0
#   define sws_isSupportedOutput(x) 0
#endif

    while ((pix_desc = av_pix_fmt_desc_next(pix_desc))) {
        av_unused enum AVPixelFormat pix_fmt = av_pix_fmt_desc_get_id(pix_desc);
        printf("%c%c%c%c%c %-16s       %d            %3d      %d",
               sws_isSupportedInput (pix_fmt)              ? 'I' : '.',
               sws_isSupportedOutput(pix_fmt)              ? 'O' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_HWACCEL   ? 'H' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_PAL       ? 'P' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_BITSTREAM ? 'B' : '.',
               pix_desc->name,
               pix_desc->nb_components,
               av_get_bits_per_pixel(pix_desc),
               pix_desc->comp[0].depth);

        for (unsigned i = 1; i < pix_desc->nb_components; i++)
            printf("-%d", pix_desc->comp[i].depth);
        printf("\n");
    }
    return 0;
}

int show_layouts(void *optctx, const char *opt, const char *arg)
{
    const AVChannelLayout *ch_layout;
    void *iter = NULL;
    char buf[128], buf2[128];
    int i = 0;

    printf("Individual channels:\n"
           "NAME           DESCRIPTION\n");
    for (i = 0; i < 63; i++) {
        av_channel_name(buf, sizeof(buf), i);
        if (strstr(buf, "USR"))
            continue;
        av_channel_description(buf2, sizeof(buf2), i);
        printf("%-14s %s\n", buf, buf2);
    }
    printf("\nStandard channel layouts:\n"
           "NAME           DECOMPOSITION\n");
    while (ch_layout = av_channel_layout_standard(&iter)) {
            av_channel_layout_describe(ch_layout, buf, sizeof(buf));
            printf("%-14s ", buf);
            for (i = 0; i < 63; i++) {
                int idx = av_channel_layout_index_from_channel(ch_layout, i);
                if (idx >= 0) {
                    av_channel_name(buf2, sizeof(buf2), i);
                    printf("%s%s", idx ? "+" : "", buf2);
                }
            }
            printf("\n");
    }
    return 0;
}

int show_sample_fmts(void *optctx, const char *opt, const char *arg)
{
    int i;
    char fmt_str[128];
    for (i = -1; i < AV_SAMPLE_FMT_NB; i++)
        printf("%s\n", av_get_sample_fmt_string(fmt_str, sizeof(fmt_str), i));
    return 0;
}

int show_dispositions(void *optctx, const char *opt, const char *arg)
{
    for (int i = 0; i < 32; i++) {
        const char *str = av_disposition_to_string(1U << i);
        if (str)
            printf("%s\n", str);
    }
    return 0;
}

int opt_cpuflags(void *optctx, const char *opt, const char *arg)
{
    int ret;
    unsigned flags = av_get_cpu_flags();

    if ((ret = av_parse_cpu_caps(&flags, arg)) < 0)
        return ret;

    av_force_cpu_flags(flags);
    return 0;
}

int opt_cpucount(void *optctx, const char *opt, const char *arg)
{
    int ret;
    int count;

    static const AVOption opts[] = {
        {"count", NULL, 0, AV_OPT_TYPE_INT, { .i64 = -1}, -1, INT_MAX},
        {NULL},
    };
    static const AVClass class = {
        .class_name = "cpucount",
        .item_name  = av_default_item_name,
        .option     = opts,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *pclass = &class;

    ret = av_opt_eval_int(&pclass, opts, arg, &count);

    if (!ret) {
        av_cpu_force_count(count);
    }

    return ret;
}

static void expand_filename_template(AVBPrint *bp, const char *template,
                                     struct tm *tm)
{
    int c;

    while ((c = *(template++))) {
        if (c == '%') {
            if (!(c = *(template++)))
                break;
            switch (c) {
            case 'p':
                av_bprintf(bp, "%s", program_name);
                break;
            case 't':
                av_bprintf(bp, "%04d%02d%02d-%02d%02d%02d",
                           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                           tm->tm_hour, tm->tm_min, tm->tm_sec);
                break;
            case '%':
                av_bprint_chars(bp, c, 1);
                break;
            }
        } else {
            av_bprint_chars(bp, c, 1);
        }
    }
}

static void log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    va_copy(vl2, vl);
    av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);
    if (report_file_level >= level) {
        fputs(line, report_file);
        fflush(report_file);
    }
}

int init_report(const char *env, FILE **file)
{
    char *filename_template = NULL;
    char *key, *val;
    int ret, count = 0;
    int prog_loglevel, envlevel = 0;
    time_t now;
    struct tm *tm;
    AVBPrint filename;

    if (report_file) /* already opened */
        return 0;
    time(&now);
    tm = localtime(&now);

    while (env && *env) {
        if ((ret = av_opt_get_key_value(&env, "=", ":", 0, &key, &val)) < 0) {
            if (count)
                av_log(NULL, AV_LOG_ERROR,
                       "Failed to parse FFREPORT environment variable: %s\n",
                       av_err2str(ret));
            break;
        }
        if (*env)
            env++;
        count++;
        if (!strcmp(key, "file")) {
            av_free(filename_template);
            filename_template = val;
            val = NULL;
        } else if (!strcmp(key, "level")) {
            char *tail;
            report_file_level = strtol(val, &tail, 10);
            if (*tail) {
                av_log(NULL, AV_LOG_FATAL, "Invalid report file level\n");
                av_free(key);
                av_free(val);
                av_free(filename_template);
                return AVERROR(EINVAL);
            }
            envlevel = 1;
        } else {
            av_log(NULL, AV_LOG_ERROR, "Unknown key '%s' in FFREPORT\n", key);
        }
        av_free(val);
        av_free(key);
    }

    av_bprint_init(&filename, 0, AV_BPRINT_SIZE_AUTOMATIC);
    expand_filename_template(&filename,
                             av_x_if_null(filename_template, "%p-%t.log"), tm);
    av_free(filename_template);
    if (!av_bprint_is_complete(&filename)) {
        av_log(NULL, AV_LOG_ERROR, "Out of memory building report file name\n");
        return AVERROR(ENOMEM);
    }

    prog_loglevel = av_log_get_level();
    if (!envlevel)
        report_file_level = FFMAX(report_file_level, prog_loglevel);

    report_file = fopen_utf8(filename.str, "w");
    if (!report_file) {
        int ret = AVERROR(errno);
        av_log(NULL, AV_LOG_ERROR, "Failed to open report \"%s\": %s\n",
               filename.str, strerror(errno));
        return ret;
    }
    av_log_set_callback(log_callback_report);
    av_log(NULL, AV_LOG_INFO,
           "%s started on %04d-%02d-%02d at %02d:%02d:%02d\n"
           "Report written to \"%s\"\n"
           "Log level: %d\n",
           program_name,
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec,
           filename.str, report_file_level);
    av_bprint_finalize(&filename, NULL);

    if (file)
        *file = report_file;

    return 0;
}

int opt_report(void *optctx, const char *opt, const char *arg)
{
    return init_report(NULL, NULL);
}

int opt_max_alloc(void *optctx, const char *opt, const char *arg)
{
    char *tail;
    size_t max;

    max = strtol(arg, &tail, 10);
    if (*tail) {
        av_log(NULL, AV_LOG_FATAL, "Invalid max_alloc \"%s\".\n", arg);
        return AVERROR(EINVAL);
    }
    av_max_alloc(max);
    return 0;
}

int opt_loglevel(void *optctx, const char *opt, const char *arg)
{
    const struct { const char *name; int level; } log_levels[] = {
        { "quiet"  , AV_LOG_QUIET   },
        { "panic"  , AV_LOG_PANIC   },
        { "fatal"  , AV_LOG_FATAL   },
        { "error"  , AV_LOG_ERROR   },
        { "warning", AV_LOG_WARNING },
        { "info"   , AV_LOG_INFO    },
        { "verbose", AV_LOG_VERBOSE },
        { "debug"  , AV_LOG_DEBUG   },
        { "trace"  , AV_LOG_TRACE   },
    };
    const char *token;
    char *tail;
    int flags = av_log_get_flags();
    int level = av_log_get_level();
    int cmd, i = 0;

    av_assert0(arg);
    while (*arg) {
        token = arg;
        if (*token == '+' || *token == '-') {
            cmd = *token++;
        } else {
            cmd = 0;
        }
        if (!i && !cmd) {
            flags = 0;  /* missing relative prefix, build absolute value */
        }
        if (av_strstart(token, "repeat", &arg)) {
            if (cmd == '-') {
                flags |= AV_LOG_SKIP_REPEATED;
            } else {
                flags &= ~AV_LOG_SKIP_REPEATED;
            }
        } else if (av_strstart(token, "level", &arg)) {
            if (cmd == '-') {
                flags &= ~AV_LOG_PRINT_LEVEL;
            } else {
                flags |= AV_LOG_PRINT_LEVEL;
            }
        } else if (av_strstart(token, "time", &arg)) {
            if (cmd == '-') {
                flags &= ~AV_LOG_PRINT_TIME;
            } else {
                flags |= AV_LOG_PRINT_TIME;
            }
        } else if (av_strstart(token, "datetime", &arg)) {
            if (cmd == '-') {
                flags &= ~AV_LOG_PRINT_DATETIME;
            } else {
                flags |= AV_LOG_PRINT_DATETIME;
            }
        } else {
            break;
        }
        i++;
    }
    if (!*arg) {
        goto end;
    } else if (*arg == '+') {
        arg++;
    } else if (!i) {
        flags = av_log_get_flags();  /* level value without prefix, reset flags */
    }

    for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
        if (!strcmp(log_levels[i].name, arg)) {
            level = log_levels[i].level;
            goto end;
        }
    }

    level = strtol(arg, &tail, 10);
    if (*tail) {
        av_log(NULL, AV_LOG_FATAL, "Invalid loglevel \"%s\". "
               "Possible levels are numbers or:\n", arg);
        for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++)
            av_log(NULL, AV_LOG_FATAL, "\"%s\"\n", log_levels[i].name);
        av_log(NULL, AV_LOG_FATAL, "Possible flags are:\n");
        av_log(NULL, AV_LOG_FATAL, "\"repeat\"\n");
        av_log(NULL, AV_LOG_FATAL, "\"level\"\n");
        av_log(NULL, AV_LOG_FATAL, "\"time\"\n");
        av_log(NULL, AV_LOG_FATAL, "\"datetime\"\n");
        return AVERROR(EINVAL);
    }

end:
    av_log_set_flags(flags);
    av_log_set_level(level);
    return 0;
}

#if CONFIG_AVDEVICE
static void print_device_list(const AVDeviceInfoList *device_list)
{
    // print devices
    for (int i = 0; i < device_list->nb_devices; i++) {
        const AVDeviceInfo *device = device_list->devices[i];
        printf("%c %s [%s] (", device_list->default_device == i ? '*' : ' ',
            device->device_name, device->device_description);
        if (device->nb_media_types > 0) {
            for (int j = 0; j < device->nb_media_types; ++j) {
                const char* media_type = av_get_media_type_string(device->media_types[j]);
                if (j > 0)
                    printf(", ");
                printf("%s", media_type ? media_type : "unknown");
            }
        } else {
            printf("none");
        }
        printf(")\n");
    }
}

static int print_device_sources(const AVInputFormat *fmt, AVDictionary *opts)
{
    int ret;
    AVDeviceInfoList *device_list = NULL;

    if (!fmt || !fmt->priv_class  || !AV_IS_INPUT_DEVICE(fmt->priv_class->category))
        return AVERROR(EINVAL);

    printf("Auto-detected sources for %s:\n", fmt->name);
    if ((ret = avdevice_list_input_sources(fmt, NULL, opts, &device_list)) < 0) {
        printf("Cannot list sources: %s\n", av_err2str(ret));
        goto fail;
    }

    print_device_list(device_list);

  fail:
    avdevice_free_list_devices(&device_list);
    return ret;
}

static int print_device_sinks(const AVOutputFormat *fmt, AVDictionary *opts)
{
    int ret;
    AVDeviceInfoList *device_list = NULL;

    if (!fmt || !fmt->priv_class  || !AV_IS_OUTPUT_DEVICE(fmt->priv_class->category))
        return AVERROR(EINVAL);

    printf("Auto-detected sinks for %s:\n", fmt->name);
    if ((ret = avdevice_list_output_sinks(fmt, NULL, opts, &device_list)) < 0) {
        printf("Cannot list sinks: %s\n", av_err2str(ret));
        goto fail;
    }

    print_device_list(device_list);

  fail:
    avdevice_free_list_devices(&device_list);
    return ret;
}

static int show_sinks_sources_parse_arg(const char *arg, char **dev, AVDictionary **opts)
{
    int ret;
    if (arg) {
        char *opts_str = NULL;
        av_assert0(dev && opts);
        *dev = av_strdup(arg);
        if (!*dev)
            return AVERROR(ENOMEM);
        if ((opts_str = strchr(*dev, ','))) {
            *(opts_str++) = '\0';
            if (opts_str[0] && ((ret = av_dict_parse_string(opts, opts_str, "=", ":", 0)) < 0)) {
                av_freep(dev);
                return ret;
            }
        }
    } else
        printf("\nDevice name is not provided.\n"
                "You can pass devicename[,opt1=val1[,opt2=val2...]] as an argument.\n\n");
    return 0;
}

int show_sources(void *optctx, const char *opt, const char *arg)
{
    const AVInputFormat *fmt = NULL;
    char *dev = NULL;
    AVDictionary *opts = NULL;
    int ret = 0;
    int error_level = av_log_get_level();

    av_log_set_level(AV_LOG_WARNING);

    if ((ret = show_sinks_sources_parse_arg(arg, &dev, &opts)) < 0)
        goto fail;

    do {
        fmt = av_input_audio_device_next(fmt);
        if (fmt) {
            if (!strcmp(fmt->name, "lavfi"))
                continue; //it's pointless to probe lavfi
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sources(fmt, opts);
        }
    } while (fmt);
    do {
        fmt = av_input_video_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sources(fmt, opts);
        }
    } while (fmt);
  fail:
    av_dict_free(&opts);
    av_free(dev);
    av_log_set_level(error_level);
    return ret;
}

int show_sinks(void *optctx, const char *opt, const char *arg)
{
    const AVOutputFormat *fmt = NULL;
    char *dev = NULL;
    AVDictionary *opts = NULL;
    int ret = 0;
    int error_level = av_log_get_level();

    av_log_set_level(AV_LOG_WARNING);

    if ((ret = show_sinks_sources_parse_arg(arg, &dev, &opts)) < 0)
        goto fail;

    do {
        fmt = av_output_audio_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sinks(fmt, opts);
        }
    } while (fmt);
    do {
        fmt = av_output_video_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sinks(fmt, opts);
        }
    } while (fmt);
  fail:
    av_dict_free(&opts);
    av_free(dev);
    av_log_set_level(error_level);
    return ret;
}
#endif /* CONFIG_AVDEVICE */


/* ========== fftools/sync_queue.c ========== */

/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/container_fifo.h"
#include "libavutil/channel_layout.h"
#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libavutil/timestamp.h"

#include "sync_queue.h"

/*
 * How this works:
 * --------------
 * time:   0    1    2    3    4    5    6    7    8    9    10   11   12   13
 *         -------------------------------------------------------------------
 *         |    |    |    |    |    |    |    |    |    |    |    |    |    |
 *         |    ┌───┐┌────────┐┌───┐┌─────────────┐
 * stream 0|    │d=1││  d=2   ││d=1││    d=3      │
 *         |    └───┘└────────┘└───┘└─────────────┘
 *         ┌───┐               ┌───────────────────────┐
 * stream 1│d=1│               │         d=5           │
 *         └───┘               └───────────────────────┘
 *         |    ┌───┐┌───┐┌───┐┌───┐
 * stream 2|    │d=1││d=1││d=1││d=1│ <- stream 2 is the head stream of the queue
 *         |    └───┘└───┘└───┘└───┘
 *                  ^              ^
 *          [stream 2 tail] [stream 2 head]
 *
 * We have N streams (N=3 in the diagram), each stream is a FIFO. The *tail* of
 * each FIFO is the frame with smallest end time, the *head* is the frame with
 * the largest end time. Frames submitted to the queue with sq_send() are placed
 * after the head, frames returned to the caller with sq_receive() are taken
 * from the tail.
 *
 * The head stream of the whole queue (SyncQueue.head_stream) is the limiting
 * stream with the *smallest* head timestamp, i.e. the stream whose source lags
 * furthest behind all other streams. It determines which frames can be output
 * from the queue.
 *
 * In the diagram, the head stream is 2, because it head time is t=5, while
 * streams 0 and 1 end at t=8 and t=9 respectively. All frames that _end_ at
 * or before t=5 can be output, i.e. the first 3 frames from stream 0, first
 * frame from stream 1, and all 4 frames from stream 2.
 */

#define SQPTR(sq, frame) ((sq->type == SYNC_QUEUE_FRAMES) ? \
                          (void*)frame.f : (void*)frame.p)

typedef struct SyncQueueStream {
    AVContainerFifo *fifo;
    AVRational       tb;

    /* number of audio samples in fifo */
    uint64_t         samples_queued;
    /* stream head: largest timestamp seen */
    int64_t          head_ts;
    int              limiting;
    /* no more frames will be sent for this stream */
    int              finished;

    uint64_t         frames_sent;
    uint64_t         samples_sent;
    uint64_t         frames_max;
    int              frame_samples;
} SyncQueueStream;

struct SyncQueue {
    enum SyncQueueType type;

    void *logctx;

    /* no more frames will be sent for any stream */
    int finished;
    /* sync head: the stream with the _smallest_ head timestamp
     * this stream determines which frames can be output */
    int head_stream;
    /* the finished stream with the smallest finish timestamp or -1 */
    int head_finished_stream;

    // maximum buffering duration in microseconds
    int64_t buf_size_us;

    SyncQueueStream *streams;
    unsigned int  nb_streams;

    int have_limiting;

    uintptr_t align_mask;
};

/**
 * Compute the end timestamp of a frame. If nb_samples is provided, consider
 * the frame to have this number of audio samples, otherwise use frame duration.
 */
static int64_t frame_end(const SyncQueue *sq, SyncQueueFrame frame, int nb_samples)
{
    if (nb_samples) {
        int64_t d = av_rescale_q(nb_samples, (AVRational){ 1, frame.f->sample_rate},
                                 frame.f->time_base);
        return frame.f->pts + d;
    }

    return (sq->type == SYNC_QUEUE_PACKETS) ?
           frame.p->pts + frame.p->duration :
           frame.f->pts + frame.f->duration;
}

static int frame_samples(const SyncQueue *sq, SyncQueueFrame frame)
{
    return (sq->type == SYNC_QUEUE_PACKETS) ? 0 : frame.f->nb_samples;
}

static int frame_null(const SyncQueue *sq, SyncQueueFrame frame)
{
    return (sq->type == SYNC_QUEUE_PACKETS) ? (frame.p == NULL) : (frame.f == NULL);
}

static void tb_update(const SyncQueue *sq, SyncQueueStream *st,
                      const SyncQueueFrame frame)
{
    AVRational tb = (sq->type == SYNC_QUEUE_PACKETS) ?
                    frame.p->time_base : frame.f->time_base;

    av_assert0(tb.num > 0 && tb.den > 0);

    if (tb.num == st->tb.num && tb.den == st->tb.den)
        return;

    // timebase should not change after the first frame
    av_assert0(!av_container_fifo_can_read(st->fifo));

    if (st->head_ts != AV_NOPTS_VALUE)
        st->head_ts = av_rescale_q(st->head_ts, st->tb, tb);

    st->tb = tb;
}

static void finish_stream(SyncQueue *sq, unsigned int stream_idx)
{
    SyncQueueStream *st = &sq->streams[stream_idx];

    if (!st->finished)
        av_log(sq->logctx, AV_LOG_DEBUG,
               "sq: finish %u; head ts %s\n", stream_idx,
               av_ts2timestr(st->head_ts, &st->tb));

    st->finished = 1;

    if (st->limiting && st->head_ts != AV_NOPTS_VALUE) {
        /* check if this stream is the new finished head */
        if (sq->head_finished_stream < 0 ||
            av_compare_ts(st->head_ts, st->tb,
                          sq->streams[sq->head_finished_stream].head_ts,
                          sq->streams[sq->head_finished_stream].tb) < 0) {
            sq->head_finished_stream = stream_idx;
        }

        /* mark as finished all streams that should no longer receive new frames,
         * due to them being ahead of some finished stream */
        st = &sq->streams[sq->head_finished_stream];
        for (unsigned int i = 0; i < sq->nb_streams; i++) {
            SyncQueueStream *st1 = &sq->streams[i];
            if (st != st1 && st1->head_ts != AV_NOPTS_VALUE &&
                av_compare_ts(st->head_ts, st->tb, st1->head_ts, st1->tb) <= 0) {
                if (!st1->finished)
                    av_log(sq->logctx, AV_LOG_DEBUG,
                           "sq: finish secondary %u; head ts %s\n", i,
                           av_ts2timestr(st1->head_ts, &st1->tb));

                st1->finished = 1;
            }
        }
    }

    /* mark the whole queue as finished if all streams are finished */
    for (unsigned int i = 0; i < sq->nb_streams; i++) {
        if (!sq->streams[i].finished)
            return;
    }
    sq->finished = 1;

    av_log(sq->logctx, AV_LOG_DEBUG, "sq: finish queue\n");
}

static void queue_head_update(SyncQueue *sq)
{
    av_assert0(sq->have_limiting);

    if (sq->head_stream < 0) {
        unsigned first_limiting = UINT_MAX;

        /* wait for one timestamp in each stream before determining
         * the queue head */
        for (unsigned int i = 0; i < sq->nb_streams; i++) {
            SyncQueueStream *st = &sq->streams[i];
            if (!st->limiting)
                continue;
            if (st->head_ts == AV_NOPTS_VALUE)
                return;
            if (first_limiting == UINT_MAX)
                first_limiting = i;
        }

        // placeholder value, correct one will be found below
        av_assert0(first_limiting < UINT_MAX);
        sq->head_stream = first_limiting;
    }

    for (unsigned int i = 0; i < sq->nb_streams; i++) {
        SyncQueueStream *st_head  = &sq->streams[sq->head_stream];
        SyncQueueStream *st_other = &sq->streams[i];
        if (st_other->limiting && st_other->head_ts != AV_NOPTS_VALUE &&
            av_compare_ts(st_other->head_ts, st_other->tb,
                          st_head->head_ts,  st_head->tb) < 0)
            sq->head_stream = i;
    }
}

/* update this stream's head timestamp */
static void stream_update_ts(SyncQueue *sq, unsigned int stream_idx, int64_t ts)
{
    SyncQueueStream *st = &sq->streams[stream_idx];

    if (ts == AV_NOPTS_VALUE ||
        (st->head_ts != AV_NOPTS_VALUE && st->head_ts >= ts))
        return;

    st->head_ts = ts;

    /* if this stream is now ahead of some finished stream, then
     * this stream is also finished */
    if (sq->head_finished_stream >= 0 &&
        av_compare_ts(sq->streams[sq->head_finished_stream].head_ts,
                      sq->streams[sq->head_finished_stream].tb,
                      ts, st->tb) <= 0)
        finish_stream(sq, stream_idx);

    /* update the overall head timestamp if it could have changed */
    if (st->limiting &&
        (sq->head_stream < 0 || sq->head_stream == stream_idx))
        queue_head_update(sq);
}

/* If the queue for the given stream (or all streams when stream_idx=-1)
 * is overflowing, trigger a fake heartbeat on lagging streams.
 *
 * @return 1 if heartbeat triggered, 0 otherwise
 */
static int overflow_heartbeat(SyncQueue *sq, int stream_idx)
{
    SyncQueueStream *st;
    SyncQueueFrame frame;
    int64_t tail_ts = AV_NOPTS_VALUE;

    /* if no stream specified, pick the one that is most ahead */
    if (stream_idx < 0) {
        int64_t ts = AV_NOPTS_VALUE;

        for (int i = 0; i < sq->nb_streams; i++) {
            st = &sq->streams[i];
            if (st->head_ts != AV_NOPTS_VALUE &&
                (ts == AV_NOPTS_VALUE ||
                 av_compare_ts(ts, sq->streams[stream_idx].tb,
                               st->head_ts, st->tb) < 0)) {
                ts = st->head_ts;
                stream_idx = i;
            }
        }
        /* no stream has a timestamp yet -> nothing to do */
        if (stream_idx < 0)
            return 0;
    }

    st = &sq->streams[stream_idx];

    /* get the chosen stream's tail timestamp */
    for (size_t i = 0; tail_ts == AV_NOPTS_VALUE &&
                       av_container_fifo_peek(st->fifo, (void**)&frame, i) >= 0; i++)
        tail_ts = frame_end(sq, frame, 0);

    /* overflow triggers when the tail is over specified duration behind the head */
    if (tail_ts == AV_NOPTS_VALUE || tail_ts >= st->head_ts ||
        av_rescale_q(st->head_ts - tail_ts, st->tb, AV_TIME_BASE_Q) < sq->buf_size_us)
        return 0;

    /* signal a fake timestamp for all streams that prevent tail_ts from being output */
    tail_ts++;
    for (unsigned int i = 0; i < sq->nb_streams; i++) {
        const SyncQueueStream *st1 = &sq->streams[i];
        int64_t ts;

        if (st == st1 || st1->finished ||
            (st1->head_ts != AV_NOPTS_VALUE &&
             av_compare_ts(tail_ts, st->tb, st1->head_ts, st1->tb) <= 0))
            continue;

        ts = av_rescale_q(tail_ts, st->tb, st1->tb);
        if (st1->head_ts != AV_NOPTS_VALUE)
            ts = FFMAX(st1->head_ts + 1, ts);

        av_log(sq->logctx, AV_LOG_DEBUG, "sq: %u overflow heardbeat %s -> %s\n",
               i, av_ts2timestr(st1->head_ts, &st1->tb), av_ts2timestr(ts, &st1->tb));

        stream_update_ts(sq, i, ts);
    }

    return 1;
}

int sq_send(SyncQueue *sq, unsigned int stream_idx, SyncQueueFrame frame)
{
    SyncQueueStream *st;
    int64_t ts;
    int ret, nb_samples;

    av_assert0(stream_idx < sq->nb_streams);
    st = &sq->streams[stream_idx];

    if (frame_null(sq, frame)) {
        av_log(sq->logctx, AV_LOG_DEBUG, "sq: %u EOF\n", stream_idx);
        finish_stream(sq, stream_idx);
        return 0;
    }
    if (st->finished)
        return AVERROR_EOF;

    tb_update(sq, st, frame);

    nb_samples = frame_samples(sq, frame);
    // make sure frame duration is consistent with sample count
    if (nb_samples) {
        av_assert0(frame.f->sample_rate > 0);
        frame.f->duration = av_rescale_q(nb_samples, (AVRational){ 1, frame.f->sample_rate },
                                         frame.f->time_base);
    }

    ts = frame_end(sq, frame, 0);

    av_log(sq->logctx, AV_LOG_DEBUG, "sq: send %u ts %s\n", stream_idx,
           av_ts2timestr(ts, &st->tb));

    ret = av_container_fifo_write(st->fifo, SQPTR(sq, frame), 0);
    if (ret < 0)
        return ret;

    stream_update_ts(sq, stream_idx, ts);

    st->samples_queued += nb_samples;
    st->samples_sent   += nb_samples;

    if (st->frame_samples)
        st->frames_sent = st->samples_sent / st->frame_samples;
    else
        st->frames_sent++;

    if (st->frames_sent >= st->frames_max) {
        av_log(sq->logctx, AV_LOG_DEBUG, "sq: %u frames_max %"PRIu64" reached\n",
               stream_idx, st->frames_max);

        finish_stream(sq, stream_idx);
    }

    return 0;
}

static void offset_audio(AVFrame *f, int nb_samples)
{
    const int planar = av_sample_fmt_is_planar(f->format);
    const int planes = planar ? f->ch_layout.nb_channels : 1;
    const int    bps = av_get_bytes_per_sample(f->format);
    const int offset = nb_samples * bps * (planar ? 1 : f->ch_layout.nb_channels);

    av_assert0(bps > 0);
    av_assert0(nb_samples < f->nb_samples);

    for (int i = 0; i < planes; i++) {
        f->extended_data[i] += offset;
        if (i < FF_ARRAY_ELEMS(f->data))
            f->data[i] = f->extended_data[i];
    }
    f->linesize[0] -= offset;
    f->nb_samples  -= nb_samples;
    f->duration     = av_rescale_q(f->nb_samples, (AVRational){ 1, f->sample_rate },
                                   f->time_base);
    f->pts         += av_rescale_q(nb_samples,    (AVRational){ 1, f->sample_rate },
                                   f->time_base);
}

static int frame_is_aligned(const SyncQueue *sq, const AVFrame *frame)
{
    // only checks linesize[0], so only works for audio
    av_assert0(frame->nb_samples > 0);
    av_assert0(sq->align_mask);

    // only check data[0], because we always offset all data pointers
    // by the same offset, so if one is aligned, all are
    if (!((uintptr_t)frame->data[0] & sq->align_mask) &&
        !(frame->linesize[0]        & sq->align_mask) &&
        frame->linesize[0] > sq->align_mask)
        return 1;

    return 0;
}

static int receive_samples(SyncQueue *sq, SyncQueueStream *st,
                           AVFrame *dst, int nb_samples)
{
    SyncQueueFrame src;
    int ret;

    av_assert0(st->samples_queued >= nb_samples);

    ret = av_container_fifo_peek(st->fifo, (void**)&src, 0);
    av_assert0(ret >= 0);

    // peeked frame has enough samples and its data is aligned
    // -> we can just make a reference and limit its sample count
    if (src.f->nb_samples > nb_samples && frame_is_aligned(sq, src.f)) {
        ret = av_frame_ref(dst, src.f);
        if (ret < 0)
            return ret;

        dst->nb_samples = nb_samples;
        offset_audio(src.f, nb_samples);
        st->samples_queued -= nb_samples;

        goto finish;
    }

    // otherwise allocate a new frame and copy the data
    ret = av_channel_layout_copy(&dst->ch_layout, &src.f->ch_layout);
    if (ret < 0)
        return ret;

    dst->format     = src.f->format;
    dst->nb_samples = nb_samples;

    ret = av_frame_get_buffer(dst, 0);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(dst, src.f);
    if (ret < 0)
        goto fail;

    dst->nb_samples = 0;
    while (dst->nb_samples < nb_samples) {
        int to_copy;

        ret = av_container_fifo_peek(st->fifo, (void**)&src, 0);
        av_assert0(ret >= 0);

        to_copy = FFMIN(nb_samples - dst->nb_samples, src.f->nb_samples);

        av_samples_copy(dst->extended_data, src.f->extended_data, dst->nb_samples,
                        0, to_copy, dst->ch_layout.nb_channels, dst->format);

        if (to_copy < src.f->nb_samples)
            offset_audio(src.f, to_copy);
        else
            av_container_fifo_drain(st->fifo, 1);

        st->samples_queued -= to_copy;

        dst->nb_samples += to_copy;
    }

finish:
    dst->duration   = av_rescale_q(nb_samples, (AVRational){ 1, dst->sample_rate },
                                   dst->time_base);

    return 0;

fail:
    av_frame_unref(dst);
    return ret;
}

static int receive_for_stream(SyncQueue *sq, unsigned int stream_idx,
                              SyncQueueFrame frame)
{
    const SyncQueueStream *st_head = sq->head_stream >= 0 ?
                                     &sq->streams[sq->head_stream] : NULL;
    SyncQueueStream *st;

    av_assert0(stream_idx < sq->nb_streams);
    st = &sq->streams[stream_idx];

    if (av_container_fifo_can_read(st->fifo) &&
        (st->frame_samples <= st->samples_queued || st->finished)) {
        int nb_samples = st->frame_samples;
        SyncQueueFrame peek;
        int64_t ts;
        int cmp = 1;

        if (st->finished)
            nb_samples = FFMIN(nb_samples, st->samples_queued);

        av_container_fifo_peek(st->fifo, (void**)&peek, 0);
        ts = frame_end(sq, peek, nb_samples);

        /* check if this stream's tail timestamp does not overtake
         * the overall queue head */
        if (ts != AV_NOPTS_VALUE && st_head)
            cmp = av_compare_ts(ts, st->tb, st_head->head_ts, st_head->tb);

        /* We can release frames that do not end after the queue head.
         * Frames with no timestamps are just passed through with no conditions.
         * Frames are also passed through when there are no limiting streams.
         */
        if (cmp <= 0 || ts == AV_NOPTS_VALUE || !sq->have_limiting) {
            if (nb_samples &&
                (nb_samples != peek.f->nb_samples || !frame_is_aligned(sq, peek.f))) {
                int ret = receive_samples(sq, st, frame.f, nb_samples);
                if (ret < 0)
                    return ret;
            } else {
                int ret = av_container_fifo_read(st->fifo, SQPTR(sq, frame), 0);
                av_assert0(ret >= 0);

                av_assert0(st->samples_queued >= frame_samples(sq, frame));
                st->samples_queued -= frame_samples(sq, frame);
            }

            av_log(sq->logctx, AV_LOG_DEBUG,
                   "sq: receive %u ts %s queue head %d ts %s\n", stream_idx,
                   av_ts2timestr(frame_end(sq, frame, 0), &st->tb),
                   sq->head_stream,
                   st_head ? av_ts2timestr(st_head->head_ts, &st_head->tb) : "N/A");

            return 0;
        }
    }

    return (sq->finished || (st->finished && !av_container_fifo_can_read(st->fifo))) ?
            AVERROR_EOF : AVERROR(EAGAIN);
}

static int receive_internal(SyncQueue *sq, int stream_idx, SyncQueueFrame frame)
{
    int nb_eof = 0;
    int ret;

    /* read a frame for a specific stream */
    if (stream_idx >= 0) {
        ret = receive_for_stream(sq, stream_idx, frame);
        return (ret < 0) ? ret : stream_idx;
    }

    /* read a frame for any stream with available output */
    for (unsigned int i = 0; i < sq->nb_streams; i++) {
        ret = receive_for_stream(sq, i, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            nb_eof += (ret == AVERROR_EOF);
            continue;
        }
        return (ret < 0) ? ret : i;
    }

    return (nb_eof == sq->nb_streams) ? AVERROR_EOF : AVERROR(EAGAIN);
}

int sq_receive(SyncQueue *sq, int stream_idx, SyncQueueFrame frame)
{
    int ret = receive_internal(sq, stream_idx, frame);

    /* try again if the queue overflowed and triggered a fake heartbeat
     * for lagging streams */
    if (ret == AVERROR(EAGAIN) && overflow_heartbeat(sq, stream_idx))
        ret = receive_internal(sq, stream_idx, frame);

    return ret;
}

int sq_add_stream(SyncQueue *sq, int limiting)
{
    SyncQueueStream *tmp, *st;

    tmp = av_realloc_array(sq->streams, sq->nb_streams + 1, sizeof(*sq->streams));
    if (!tmp)
        return AVERROR(ENOMEM);
    sq->streams = tmp;

    st = &sq->streams[sq->nb_streams];
    memset(st, 0, sizeof(*st));

    st->fifo = (sq->type == SYNC_QUEUE_FRAMES) ?
               av_container_fifo_alloc_avframe(0) : av_container_fifo_alloc_avpacket(0);
    if (!st->fifo)
        return AVERROR(ENOMEM);

    /* we set a valid default, so that a pathological stream that never
     * receives even a real timebase (and no frames) won't stall all other
     * streams forever; cf. overflow_heartbeat() */
    st->tb      = (AVRational){ 1, 1 };
    st->head_ts = AV_NOPTS_VALUE;
    st->frames_max = UINT64_MAX;
    st->limiting   = limiting;

    sq->have_limiting |= limiting;

    return sq->nb_streams++;
}

void sq_limit_frames(SyncQueue *sq, unsigned int stream_idx, uint64_t frames)
{
    SyncQueueStream *st;

    av_assert0(stream_idx < sq->nb_streams);
    st = &sq->streams[stream_idx];

    st->frames_max = frames;
    if (st->frames_sent >= st->frames_max)
        finish_stream(sq, stream_idx);
}

void sq_frame_samples(SyncQueue *sq, unsigned int stream_idx,
                      int frame_samples)
{
    SyncQueueStream *st;

    av_assert0(sq->type == SYNC_QUEUE_FRAMES);
    av_assert0(stream_idx < sq->nb_streams);
    st = &sq->streams[stream_idx];

    st->frame_samples = frame_samples;

    sq->align_mask = av_cpu_max_align() - 1;
}

SyncQueue *sq_alloc(enum SyncQueueType type, int64_t buf_size_us, void *logctx)
{
    SyncQueue *sq = av_mallocz(sizeof(*sq));

    if (!sq)
        return NULL;

    sq->type                 = type;
    sq->buf_size_us          = buf_size_us;
    sq->logctx               = logctx;

    sq->head_stream          = -1;
    sq->head_finished_stream = -1;

    return sq;
}

void sq_free(SyncQueue **psq)
{
    SyncQueue *sq = *psq;

    if (!sq)
        return;

    for (unsigned int i = 0; i < sq->nb_streams; i++)
        av_container_fifo_free(&sq->streams[i].fifo);

    av_freep(&sq->streams);

    av_freep(psq);
}


/* ========== fftools/thread_queue.c ========== */

/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/container_fifo.h"
#include "libavutil/error.h"
#include "libavutil/fifo.h"
#include "libavutil/frame.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#include "libavcodec/packet.h"

#include "thread_queue.h"

enum {
    FINISHED_SEND = (1 << 0),
    FINISHED_RECV = (1 << 1),
};

struct ThreadQueue {
    int             choked;
    int              *finished;
    unsigned int    nb_streams;

    enum ThreadQueueType type;

    AVContainerFifo *fifo;
    AVFifo          *fifo_stream_index;

    pthread_mutex_t lock;
    pthread_cond_t  cond;
};

void tq_free(ThreadQueue **ptq)
{
    ThreadQueue *tq = *ptq;

    if (!tq)
        return;

    av_container_fifo_free(&tq->fifo);
    av_fifo_freep2(&tq->fifo_stream_index);

    av_freep(&tq->finished);

    pthread_cond_destroy(&tq->cond);
    pthread_mutex_destroy(&tq->lock);

    av_freep(ptq);
}

ThreadQueue *tq_alloc(unsigned int nb_streams, size_t queue_size,
                      enum ThreadQueueType type)
{
    ThreadQueue *tq;
    int ret;

    tq = av_mallocz(sizeof(*tq));
    if (!tq)
        return NULL;

    ret = pthread_cond_init(&tq->cond, NULL);
    if (ret) {
        av_freep(&tq);
        return NULL;
    }

    ret = pthread_mutex_init(&tq->lock, NULL);
    if (ret) {
        pthread_cond_destroy(&tq->cond);
        av_freep(&tq);
        return NULL;
    }

    tq->finished = av_calloc(nb_streams, sizeof(*tq->finished));
    if (!tq->finished)
        goto fail;
    tq->nb_streams = nb_streams;

    tq->type = type;

    tq->fifo = (type == THREAD_QUEUE_FRAMES) ?
               av_container_fifo_alloc_avframe(0) : av_container_fifo_alloc_avpacket(0);
    if (!tq->fifo)
        goto fail;

    tq->fifo_stream_index = av_fifo_alloc2(queue_size, sizeof(unsigned), 0);
    if (!tq->fifo_stream_index)
        goto fail;

    return tq;
fail:
    tq_free(&tq);
    return NULL;
}

int tq_send(ThreadQueue *tq, unsigned int stream_idx, void *data)
{
    int *finished;
    int ret;

    av_assert0(stream_idx < tq->nb_streams);
    finished = &tq->finished[stream_idx];

    pthread_mutex_lock(&tq->lock);

    if (*finished & FINISHED_SEND) {
        ret = AVERROR(EINVAL);
        goto finish;
    }

    while (!(*finished & FINISHED_RECV) && !av_fifo_can_write(tq->fifo_stream_index))
        pthread_cond_wait(&tq->cond, &tq->lock);

    if (*finished & FINISHED_RECV) {
        ret = AVERROR_EOF;
        *finished |= FINISHED_SEND;
    } else {
        ret = av_fifo_write(tq->fifo_stream_index, &stream_idx, 1);
        if (ret < 0)
            goto finish;

        ret = av_container_fifo_write(tq->fifo, data, 0);
        if (ret < 0)
            goto finish;

        pthread_cond_broadcast(&tq->cond);
    }

finish:
    pthread_mutex_unlock(&tq->lock);

    return ret;
}

static int receive_locked(ThreadQueue *tq, int *stream_idx,
                          void *data)
{
    unsigned int nb_finished = 0;

    if (tq->choked)
        return AVERROR(EAGAIN);

    while (av_container_fifo_read(tq->fifo, data, 0) >= 0) {
        unsigned idx;
        int ret;

        ret = av_fifo_read(tq->fifo_stream_index, &idx, 1);
        av_assert0(ret >= 0);
        if (tq->finished[idx] & FINISHED_RECV) {
            (tq->type == THREAD_QUEUE_FRAMES) ?
            av_frame_unref(data) : av_packet_unref(data);
            continue;
        }

        *stream_idx = idx;
        return 0;
    }

    for (unsigned int i = 0; i < tq->nb_streams; i++) {
        if (!tq->finished[i])
            continue;

        /* return EOF to the consumer at most once for each stream */
        if (!(tq->finished[i] & FINISHED_RECV)) {
            tq->finished[i] |= FINISHED_RECV;
            *stream_idx   = i;
            return AVERROR_EOF;
        }

        nb_finished++;
    }

    return nb_finished == tq->nb_streams ? AVERROR_EOF : AVERROR(EAGAIN);
}

int tq_receive(ThreadQueue *tq, int *stream_idx, void *data)
{
    int ret;

    *stream_idx = -1;

    pthread_mutex_lock(&tq->lock);

    while (1) {
        size_t can_read = av_container_fifo_can_read(tq->fifo);

        ret = receive_locked(tq, stream_idx, data);

        // signal other threads if the fifo state changed
        if (can_read != av_container_fifo_can_read(tq->fifo))
            pthread_cond_broadcast(&tq->cond);

        if (ret == AVERROR(EAGAIN)) {
            pthread_cond_wait(&tq->cond, &tq->lock);
            continue;
        }

        break;
    }

    pthread_mutex_unlock(&tq->lock);

    return ret;
}

void tq_send_finish(ThreadQueue *tq, unsigned int stream_idx)
{
    av_assert0(stream_idx < tq->nb_streams);

    pthread_mutex_lock(&tq->lock);

    /* mark the stream as send-finished;
     * next time the consumer thread tries to read this stream it will get
     * an EOF and recv-finished flag will be set */
    tq->finished[stream_idx] |= FINISHED_SEND;
    tq->choked = 0;
    pthread_cond_broadcast(&tq->cond);

    pthread_mutex_unlock(&tq->lock);
}

void tq_receive_finish(ThreadQueue *tq, unsigned int stream_idx)
{
    av_assert0(stream_idx < tq->nb_streams);

    pthread_mutex_lock(&tq->lock);

    /* mark the stream as recv-finished;
     * next time the producer thread tries to send for this stream, it will
     * get an EOF and send-finished flag will be set */
    tq->finished[stream_idx] |= FINISHED_RECV;
    pthread_cond_broadcast(&tq->cond);

    pthread_mutex_unlock(&tq->lock);
}

void tq_choke(ThreadQueue *tq, int choked)
{
    pthread_mutex_lock(&tq->lock);

    int prev_choked = tq->choked;
    tq->choked = choked;
    if (choked != prev_choked)
        pthread_cond_broadcast(&tq->cond);

    pthread_mutex_unlock(&tq->lock);
}


/* ========== fftools/textformat/avtextformat.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/mem.h"
#include "libavutil/avassert.h"
#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/hash.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/macros.h"
#include "libavutil/opt.h"
#include "avtextformat.h"

#define SECTION_ID_NONE (-1)

#define SHOW_OPTIONAL_FIELDS_AUTO      (-1)
#define SHOW_OPTIONAL_FIELDS_NEVER       0
#define SHOW_OPTIONAL_FIELDS_ALWAYS      1

static const struct {
    double bin_val;
    double dec_val;
    char bin_str[4];
    char dec_str[4];
} si_prefixes[] = {
    { 1.0, 1.0, "", "" },
    { 1.024e3, 1e3, "Ki", "K" },
    { 1.048576e6, 1e6, "Mi", "M" },
    { 1.073741824e9, 1e9, "Gi", "G" },
    { 1.099511627776e12, 1e12, "Ti", "T" },
    { 1.125899906842624e15, 1e15, "Pi", "P" },
};

static const char *textcontext_get_formatter_name(void *p)
{
    AVTextFormatContext *tctx = p;
    return tctx->formatter->name;
}

#define OFFSET(x) offsetof(AVTextFormatContext, x)

static const AVOption textcontext_options[] = {
    { "string_validation", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, { .i64 = AV_TEXTFORMAT_STRING_VALIDATION_REPLACE }, 0, AV_TEXTFORMAT_STRING_VALIDATION_NB - 1, .unit = "sv" },
    { "sv", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, { .i64 = AV_TEXTFORMAT_STRING_VALIDATION_REPLACE }, 0, AV_TEXTFORMAT_STRING_VALIDATION_NB - 1, .unit = "sv" },
        { "ignore",  NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = AV_TEXTFORMAT_STRING_VALIDATION_IGNORE },  .unit = "sv" },
        { "replace", NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = AV_TEXTFORMAT_STRING_VALIDATION_REPLACE }, .unit = "sv" },
        { "fail",    NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = AV_TEXTFORMAT_STRING_VALIDATION_FAIL },    .unit = "sv" },
    { "string_validation_replacement", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, { .str = "" } },
    { "svr", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, { .str = "\xEF\xBF\xBD" } },
    { NULL }
};

static void *textcontext_child_next(void *obj, void *prev)
{
    AVTextFormatContext *ctx = obj;
    if (!prev && ctx->formatter && ctx->formatter->priv_class && ctx->priv)
        return ctx->priv;
    return NULL;
}

static const AVClass textcontext_class = {
    .class_name = "AVTextContext",
    .item_name  = textcontext_get_formatter_name,
    .option     = textcontext_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .child_next = textcontext_child_next,
};

static void bprint_bytes(AVBPrint *bp, const uint8_t *ubuf, size_t ubuf_size)
{
    av_bprintf(bp, "0X");
    for (unsigned i = 0; i < ubuf_size; i++)
        av_bprintf(bp, "%02X", ubuf[i]);
}

int avtext_context_close(AVTextFormatContext **ptctx)
{
    AVTextFormatContext *tctx = *ptctx;
    int ret = 0;

    if (!tctx)
        return AVERROR(EINVAL);

    av_hash_freep(&tctx->hash);

    if (tctx->formatter) {
        if (tctx->formatter->uninit)
            ret = tctx->formatter->uninit(tctx);
        if (tctx->formatter->priv_class)
            av_opt_free(tctx->priv);
    }
    for (int i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_finalize(&tctx->section_pbuf[i], NULL);
    av_freep(&tctx->priv);
    av_opt_free(tctx);
    av_freep(ptctx);
    return ret;
}


int avtext_context_open(AVTextFormatContext **ptctx, const AVTextFormatter *formatter, AVTextWriterContext *writer_context, const char *args,
                        const AVTextFormatSection *sections, int nb_sections, AVTextFormatOptions options, char *show_data_hash)
{
    AVTextFormatContext *tctx;
    int ret = 0;

    av_assert0(ptctx && formatter);

    if (!(tctx = av_mallocz(sizeof(AVTextFormatContext)))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (int i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_init(&tctx->section_pbuf[i], 1, AV_BPRINT_SIZE_UNLIMITED);

    tctx->class = &textcontext_class;
    av_opt_set_defaults(tctx);

    if (!(tctx->priv = av_mallocz(formatter->priv_size))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    tctx->opts = options;

    if (nb_sections > SECTION_MAX_NB_SECTIONS) {
        av_log(tctx, AV_LOG_ERROR, "The number of section definitions (%d) is larger than the maximum allowed (%d)\n", nb_sections, SECTION_MAX_NB_SECTIONS);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    tctx->formatter = formatter;
    tctx->level = -1;
    tctx->sections = sections;
    tctx->nb_sections = nb_sections;
    tctx->writer = writer_context;

    if (formatter->priv_class) {
        void *priv_ctx = tctx->priv;
        *(const AVClass **)priv_ctx = formatter->priv_class;
        av_opt_set_defaults(priv_ctx);
    }

    /* convert options to dictionary */
    if (args) {
        AVDictionary *opts = NULL;
        const AVDictionaryEntry *opt = NULL;

        if ((ret = av_dict_parse_string(&opts, args, "=", ":", 0)) < 0) {
            av_log(tctx, AV_LOG_ERROR, "Failed to parse option string '%s' provided to textformat context\n", args);
            av_dict_free(&opts);
            goto fail;
        }

        while ((opt = av_dict_iterate(opts, opt))) {
            if ((ret = av_opt_set(tctx, opt->key, opt->value, AV_OPT_SEARCH_CHILDREN)) < 0) {
                av_log(tctx, AV_LOG_ERROR, "Failed to set option '%s' with value '%s' provided to textformat context\n",
                       opt->key, opt->value);
                av_dict_free(&opts);
                goto fail;
            }
        }

        av_dict_free(&opts);
    }

    if (show_data_hash) {
        if ((ret = av_hash_alloc(&tctx->hash, show_data_hash)) < 0) {
            if (ret == AVERROR(EINVAL)) {
                const char *n;
                av_log(NULL, AV_LOG_ERROR, "Unknown hash algorithm '%s'\nKnown algorithms:", show_data_hash);
                for (unsigned i = 0; (n = av_hash_names(i)); i++)
                    av_log(NULL, AV_LOG_ERROR, " %s", n);
                av_log(NULL, AV_LOG_ERROR, "\n");
            }
            goto fail;
        }
    }

    /* validate replace string */
    {
        const uint8_t *p = (uint8_t *)tctx->string_validation_replacement;
        const uint8_t *endp = p + strlen((const char *)p);
        while (*p) {
            const uint8_t *p0 = p;
            int32_t code;
            ret = av_utf8_decode(&code, &p, endp, tctx->string_validation_utf8_flags);
            if (ret < 0) {
                AVBPrint bp;
                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                bprint_bytes(&bp, p0, p - p0);
                av_log(tctx, AV_LOG_ERROR,
                       "Invalid UTF8 sequence %s found in string validation replace '%s'\n",
                       bp.str, tctx->string_validation_replacement);
                goto fail;
            }
        }
    }

    if (tctx->formatter->init)
        ret = tctx->formatter->init(tctx);
    if (ret < 0)
        goto fail;

    *ptctx = tctx;

    return 0;

fail:
    avtext_context_close(&tctx);
    return ret;
}

/* Temporary definitions during refactoring */
static const char unit_second_str[]         = "s";
static const char unit_hertz_str[]          = "Hz";
static const char unit_byte_str[]           = "byte";
static const char unit_bit_per_second_str[] = "bit/s";


void avtext_print_section_header(AVTextFormatContext *tctx, const void *data, int section_id)
{
    if (section_id < 0 || section_id >= tctx->nb_sections) {
        av_log(tctx, AV_LOG_ERROR, "Invalid section_id for section_header: %d\n", section_id);
        return;
    }

    tctx->level++;
    av_assert0(tctx->level < SECTION_MAX_NB_LEVELS);

    tctx->nb_item[tctx->level] = 0;
    memset(tctx->nb_item_type[tctx->level], 0, sizeof(tctx->nb_item_type[tctx->level]));
    tctx->section[tctx->level] = &tctx->sections[section_id];

    if (tctx->formatter->print_section_header)
        tctx->formatter->print_section_header(tctx, data);
}

void avtext_print_section_footer(AVTextFormatContext *tctx)
{
    if (tctx->level < 0 || tctx->level >= SECTION_MAX_NB_LEVELS) {
        av_log(tctx, AV_LOG_ERROR, "Invalid level for section_footer: %d\n", tctx->level);
        return;
    }

    int section_id = tctx->section[tctx->level]->id;
    int parent_section_id = tctx->level ?
        tctx->section[tctx->level - 1]->id : SECTION_ID_NONE;

    if (parent_section_id != SECTION_ID_NONE) {
        tctx->nb_item[tctx->level - 1]++;
        tctx->nb_item_type[tctx->level - 1][section_id]++;
    }

    if (tctx->formatter->print_section_footer)
        tctx->formatter->print_section_footer(tctx);
    tctx->level--;
}

void avtext_print_integer(AVTextFormatContext *tctx, const char *key, int64_t val, int flags)
{
    av_assert0(tctx);

    if (tctx->opts.show_optional_fields == SHOW_OPTIONAL_FIELDS_NEVER)
        return;

    if (tctx->opts.show_optional_fields == SHOW_OPTIONAL_FIELDS_AUTO
        && (flags & AV_TEXTFORMAT_PRINT_STRING_OPTIONAL)
        && !(tctx->formatter->flags & AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS))
        return;

    av_assert0(key && tctx->level >= 0 && tctx->level < SECTION_MAX_NB_LEVELS);

    if (!tctx->opts.is_key_selected || tctx->opts.is_key_selected(tctx, key)) {
        tctx->formatter->print_integer(tctx, key, val);
        tctx->nb_item[tctx->level]++;
    }
}

static inline int validate_string(AVTextFormatContext *tctx, char **dstp, const char *src)
{
    const uint8_t *p, *endp, *srcp = (const uint8_t *)src;
    AVBPrint dstbuf;
    AVBPrint invalid_seq;
    int invalid_chars_nb = 0, ret = 0;

    *dstp = NULL;
    av_bprint_init(&dstbuf, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprint_init(&invalid_seq, 0, AV_BPRINT_SIZE_UNLIMITED);

    endp = srcp + strlen(src);
    for (p = srcp; *p;) {
        int32_t code;
        int invalid = 0;
        const uint8_t *p0 = p;

        if (av_utf8_decode(&code, &p, endp, tctx->string_validation_utf8_flags) < 0) {

            av_bprint_clear(&invalid_seq);

            bprint_bytes(&invalid_seq, p0, p - p0);

            av_log(tctx, AV_LOG_DEBUG, "Invalid UTF-8 sequence '%s' found in string '%s'\n", invalid_seq.str, src);
            invalid = 1;
        }

        if (invalid) {
            invalid_chars_nb++;

            switch (tctx->string_validation) {
            case AV_TEXTFORMAT_STRING_VALIDATION_FAIL:
                av_log(tctx, AV_LOG_ERROR, "Invalid UTF-8 sequence found in string '%s'\n", src);
                ret = AVERROR_INVALIDDATA;
                goto end;

            case AV_TEXTFORMAT_STRING_VALIDATION_REPLACE:
                av_bprintf(&dstbuf, "%s", tctx->string_validation_replacement);
                break;
            }
        }

        if (!invalid || tctx->string_validation == AV_TEXTFORMAT_STRING_VALIDATION_IGNORE)
            av_bprint_append_data(&dstbuf, p0, p-p0);
    }

    if (invalid_chars_nb && tctx->string_validation == AV_TEXTFORMAT_STRING_VALIDATION_REPLACE)
        av_log(tctx, AV_LOG_WARNING,
               "%d invalid UTF-8 sequence(s) found in string '%s', replaced with '%s'\n",
               invalid_chars_nb, src, tctx->string_validation_replacement);

end:
    av_bprint_finalize(&dstbuf, dstp);
    av_bprint_finalize(&invalid_seq, NULL);
    return ret;
}

struct unit_value {
    union {
        double  d;
        int64_t i;
    } val;

    const char *unit;
};

static char *value_string(const AVTextFormatContext *tctx, char *buf, int buf_size, struct unit_value uv)
{
    double vald;
    int64_t vali = 0;
    int show_float = 0;

    if (uv.unit == unit_second_str) {
        vald = uv.val.d;
        show_float = 1;
    } else {
        vald = (double)uv.val.i;
        vali = uv.val.i;
    }

    if (uv.unit == unit_second_str && tctx->opts.use_value_sexagesimal_format) {
        double secs;
        int hours, mins;
        secs  = vald;
        mins  = (int)secs / 60;
        secs  = secs - mins * 60;
        hours = mins / 60;
        mins %= 60;
        snprintf(buf, buf_size, "%d:%02d:%09.6f", hours, mins, secs);
    } else {
        const char *prefix_string = "";

        if (tctx->opts.use_value_prefix && vald > 1) {
            int64_t index;

            if (uv.unit == unit_byte_str && tctx->opts.use_byte_value_binary_prefix) {
                index = (int64_t)(log2(vald) / 10);
                index = av_clip64(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].bin_val;
                prefix_string = si_prefixes[index].bin_str;
            } else {
                index = (int64_t)(log10(vald) / 3);
                index = av_clip64(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].dec_val;
                prefix_string = si_prefixes[index].dec_str;
            }
            vali = (int64_t)vald;
        }

        if (show_float || (tctx->opts.use_value_prefix && vald != (int64_t)vald))
            snprintf(buf, buf_size, "%f", vald);
        else
            snprintf(buf, buf_size, "%"PRId64, vali);

        av_strlcatf(buf, buf_size, "%s%s%s", *prefix_string || tctx->opts.show_value_unit ? " " : "",
                    prefix_string, tctx->opts.show_value_unit ? uv.unit : "");
    }

    return buf;
}


void avtext_print_unit_integer(AVTextFormatContext *tctx, const char *key, int64_t val, const char *unit)
{
    char val_str[128];
    struct unit_value uv;
    uv.val.i = val;
    uv.unit = unit;
    avtext_print_string(tctx, key, value_string(tctx, val_str, sizeof(val_str), uv), 0);
}


int avtext_print_string(AVTextFormatContext *tctx, const char *key, const char *val, int flags)
{
    const AVTextFormatSection *section;
    int ret = 0;

    av_assert0(key && val && tctx->level >= 0 && tctx->level < SECTION_MAX_NB_LEVELS);

    section = tctx->section[tctx->level];

    if (tctx->opts.show_optional_fields == SHOW_OPTIONAL_FIELDS_NEVER)
        return 0;

    if (tctx->opts.show_optional_fields == SHOW_OPTIONAL_FIELDS_AUTO
        && (flags & AV_TEXTFORMAT_PRINT_STRING_OPTIONAL)
        && !(tctx->formatter->flags & AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS))
        return 0;

    if (!tctx->opts.is_key_selected || tctx->opts.is_key_selected(tctx, key)) {
        if (flags & AV_TEXTFORMAT_PRINT_STRING_VALIDATE) {
            char *key1 = NULL, *val1 = NULL;
            ret = validate_string(tctx, &key1, key);
            if (ret < 0) goto end;
            ret = validate_string(tctx, &val1, val);
            if (ret < 0) goto end;
            tctx->formatter->print_string(tctx, key1, val1);
        end:
            if (ret < 0)
                av_log(tctx, AV_LOG_ERROR,
                       "Invalid key=value string combination %s=%s in section %s\n",
                       key, val, section->unique_name);
            av_free(key1);
            av_free(val1);
        } else {
            tctx->formatter->print_string(tctx, key, val);
        }

        tctx->nb_item[tctx->level]++;
    }

    return ret;
}

void avtext_print_rational(AVTextFormatContext *tctx, const char *key, AVRational q, char sep)
{
    char buf[44];
    snprintf(buf, sizeof(buf), "%d%c%d", q.num, sep, q.den);
    avtext_print_string(tctx, key, buf, 0);
}

void avtext_print_time(AVTextFormatContext *tctx, const char *key,
                       int64_t ts, const AVRational *time_base, int is_duration)
{
    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        avtext_print_string(tctx, key, "N/A", AV_TEXTFORMAT_PRINT_STRING_OPTIONAL);
    } else {
        char buf[128];
        double d = av_q2d(*time_base) * ts;
        struct unit_value uv;
        uv.val.d = d;
        uv.unit = unit_second_str;
        value_string(tctx, buf, sizeof(buf), uv);
        avtext_print_string(tctx, key, buf, 0);
    }
}

void avtext_print_ts(AVTextFormatContext *tctx, const char *key, int64_t ts, int is_duration)
{
    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0))
        avtext_print_string(tctx, key, "N/A", AV_TEXTFORMAT_PRINT_STRING_OPTIONAL);
    else
        avtext_print_integer(tctx, key, ts, 0);
}

static void print_data_xxd(AVBPrint *bp, const uint8_t *data, int size)
{
    unsigned offset = 0;
    int i;

    av_bprintf(bp, "\n");
    while (size) {
        av_bprintf(bp, "%08x: ", offset);
        int l = FFMIN(size, 16);
        for (i = 0; i < l; i++) {
            av_bprintf(bp, "%02x", data[i]);
            if (i & 1)
                av_bprintf(bp, " ");
        }
        av_bprint_chars(bp, ' ', 41 - 2 * i - i / 2);
        for (i = 0; i < l; i++)
            av_bprint_chars(bp, data[i] - 32U < 95 ? data[i] : '.', 1);
        av_bprintf(bp, "\n");
        offset += l;
        data   += l;
        size   -= l;
    }
}

static void print_data_base64(AVBPrint *bp, const uint8_t *data, int size)
{
    char buf[AV_BASE64_SIZE(60)];

    av_bprintf(bp, "\n");
    while (size) {
        int l = FFMIN(size, 60);
        av_base64_encode(buf, sizeof(buf), data, l);
        av_bprintf(bp, "%s\n", buf);
        data   += l;
        size   -= l;
    }
}
void avtext_print_data(AVTextFormatContext *tctx, const char *key,
                       const uint8_t *data, int size)
{
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    switch (tctx->opts.data_dump_format) {
    case AV_TEXTFORMAT_DATADUMP_XXD:
        print_data_xxd(&bp, data, size);
        break;
    case AV_TEXTFORMAT_DATADUMP_BASE64:
        print_data_base64(&bp, data, size);
        break;
    default:
        av_unreachable("Invalid data dump type");
    }
    avtext_print_string(tctx, key, bp.str, 0);
    av_bprint_finalize(&bp, NULL);
}

void avtext_print_data_hash(AVTextFormatContext *tctx, const char *key,
                            const uint8_t *data, int size)
{
    char buf[AV_HASH_MAX_SIZE * 2 + 64] = { 0 };
    int len;

    if (!tctx->hash)
        return;

    av_hash_init(tctx->hash);
    av_hash_update(tctx->hash, data, size);
    len = snprintf(buf, sizeof(buf), "%s:", av_hash_get_name(tctx->hash));
    av_hash_final_hex(tctx->hash, (uint8_t *)&buf[len], (int)sizeof(buf) - len);
    avtext_print_string(tctx, key, buf, 0);
}

static const char *writercontext_get_writer_name(void *p)
{
    AVTextWriterContext *wctx = p;
    return wctx->writer->name;
}

static void *writercontext_child_next(void *obj, void *prev)
{
    AVTextFormatContext *ctx = obj;
    if (!prev && ctx->formatter && ctx->formatter->priv_class && ctx->priv)
        return ctx->priv;
    return NULL;
}

static const AVClass textwriter_class = {
    .class_name = "AVTextWriterContext",
    .item_name  = writercontext_get_writer_name,
    .version    = LIBAVUTIL_VERSION_INT,
    .child_next = writercontext_child_next,
};


int avtextwriter_context_close(AVTextWriterContext **pwctx)
{
    AVTextWriterContext *wctx = *pwctx;
    int ret = 0;

    if (!wctx)
        return AVERROR(EINVAL);

    if (wctx->writer) {
        if (wctx->writer->uninit)
            ret = wctx->writer->uninit(wctx);
        if (wctx->writer->priv_class)
            av_opt_free(wctx->priv);
    }
    av_freep(&wctx->priv);
    av_freep(pwctx);
    return ret;
}


int avtextwriter_context_open(AVTextWriterContext **pwctx, const AVTextWriter *writer)
{
    AVTextWriterContext *wctx;
    int ret = 0;

    if (!pwctx || !writer)
        return AVERROR(EINVAL);

    if (!((wctx = av_mallocz(sizeof(AVTextWriterContext))))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (writer->priv_size && !((wctx->priv = av_mallocz(writer->priv_size)))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (writer->priv_class) {
        void *priv_ctx = wctx->priv;
        *(const AVClass **)priv_ctx = writer->priv_class;
        av_opt_set_defaults(priv_ctx);
    }

    wctx->class = &textwriter_class;
    wctx->writer = writer;

    av_opt_set_defaults(wctx);


    if (wctx->writer->init)
        ret = wctx->writer->init(wctx);
    if (ret < 0)
        goto fail;

    *pwctx = wctx;

    return 0;

fail:
    avtextwriter_context_close(&wctx);
    return ret;
}

static const AVTextFormatter *const registered_formatters[] =
{
    &avtextformatter_default,
    &avtextformatter_compact,
    &avtextformatter_csv,
    &avtextformatter_flat,
    &avtextformatter_ini,
    &avtextformatter_json,
    &avtextformatter_xml,
    &avtextformatter_mermaid,
    &avtextformatter_mermaidhtml,
    NULL
};

const AVTextFormatter *avtext_get_formatter_by_name(const char *name)
{
    for (int i = 0; registered_formatters[i]; i++) {
        const char *end;
        if (av_strstart(name, registered_formatters[i]->name, &end) &&
            (*end == '\0' || *end == '='))
            return registered_formatters[i];
    }

    return NULL;
}


/* ========== fftools/textformat/tf_default.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"

/* --- inlined tf_internal.h --- */
#include "avtextformat.h"

#define DEFINE_FORMATTER_CLASS(name)                \
static const AVClass name##_class = {               \
    .class_name = #name,                            \
    .item_name  = av_default_item_name,             \
    .option     = name##_options                    \
}


/**
 * Safely validate and access a section at a given level
 */
static inline const AVTextFormatSection *tf_get_section(AVTextFormatContext *tfc, int level)
{
    if (!tfc || level < 0 || level >= SECTION_MAX_NB_LEVELS || !tfc->section[level]) {
        if (tfc)
            av_log(tfc, AV_LOG_ERROR, "Invalid section access at level %d\n", level);
        return NULL;
    }
    return tfc->section[level];
}

/**
 * Safely access the parent section
 */
static inline const AVTextFormatSection *tf_get_parent_section(AVTextFormatContext *tfc, int level)
{
    if (level <= 0)
        return NULL;

    return tf_get_section(tfc, level - 1);
}

static inline void writer_w8(AVTextFormatContext *wctx, int b)
{
    wctx->writer->writer->writer_w8(wctx->writer, b);
}

static inline void writer_put_str(AVTextFormatContext *wctx, const char *str)
{
    wctx->writer->writer->writer_put_str(wctx->writer, str);
}

static inline void writer_printf(AVTextFormatContext *wctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    wctx->writer->writer->writer_vprintf(wctx->writer, fmt, args);
    va_end(args);
}
/* --- end tf_internal.h --- */


/* Default output */

typedef struct DefaultContext {
    const AVClass *class;
    int nokey;
    int noprint_wrappers;
    int nested_section[SECTION_MAX_NB_LEVELS];
} DefaultContext;

#undef OFFSET
#define OFFSET(x) offsetof(DefaultContext, x)

static const AVOption default_options[] = {
    { "noprint_wrappers", "do not print headers and footers", OFFSET(noprint_wrappers), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { "nw",               "do not print headers and footers", OFFSET(noprint_wrappers), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { "nokey",            "force no key printing",            OFFSET(nokey),            AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { "nk",               "force no key printing",            OFFSET(nokey),            AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(default);

/* lame uppercasing routine, assumes the string is lower case ASCII */
static inline char *upcase_string(char *dst, size_t dst_size, const char *src)
{
    unsigned i;

    for (i = 0; src[i] && i < dst_size - 1; i++)
        dst[i] = (char)av_toupper(src[i]);
    dst[i] = 0;
    return dst;
}

static void default_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    DefaultContext *def = wctx->priv;
    char buf[32];
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);

    if (!section)
        return;

    av_bprint_clear(&wctx->section_pbuf[wctx->level]);
    if (parent_section &&
        !(parent_section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY))) {
        def->nested_section[wctx->level] = 1;
        av_bprintf(&wctx->section_pbuf[wctx->level], "%s%s:",
                   wctx->section_pbuf[wctx->level - 1].str,
                   upcase_string(buf, sizeof(buf),
                                 av_x_if_null(section->element_name, section->name)));
    }

    if (def->noprint_wrappers || def->nested_section[wctx->level])
        return;

    if (!(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)))
        writer_printf(wctx, "[%s]\n", upcase_string(buf, sizeof(buf), section->name));
}

static void default_print_section_footer(AVTextFormatContext *wctx)
{
    DefaultContext *def = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);

    char buf[32];

    if (!section)
        return;

    if (def->noprint_wrappers || def->nested_section[wctx->level])
        return;

    if (!(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)))
        writer_printf(wctx, "[/%s]\n", upcase_string(buf, sizeof(buf), section->name));
}

static void default_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    DefaultContext *def = wctx->priv;

    if (!def->nokey)
        writer_printf(wctx, "%s%s=", wctx->section_pbuf[wctx->level].str, key);
    writer_printf(wctx, "%s\n", value);
}

static void default_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    DefaultContext *def = wctx->priv;

    if (!def->nokey)
        writer_printf(wctx, "%s%s=", wctx->section_pbuf[wctx->level].str, key);
    writer_printf(wctx, "%"PRId64"\n", value);
}

const AVTextFormatter avtextformatter_default = {
    .name                  = "default",
    .priv_size             = sizeof(DefaultContext),
    .print_section_header  = default_print_section_header,
    .print_section_footer  = default_print_section_footer,
    .print_integer         = default_print_int,
    .print_string          = default_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS,
    .priv_class            = &default_class,
};


/* ========== fftools/textformat/tf_compact.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"



/* Compact output */

/**
 * Apply C-language-like string escaping.
 */
static const char *c_escape_str(AVBPrint *dst, const char *src, const char sep, void *log_ctx)
{
    const char *p;

    for (p = src; *p; p++) {
        switch (*p) {
        case '\b': av_bprintf(dst, "%s", "\\b"); break;
        case '\f': av_bprintf(dst, "%s", "\\f"); break;
        case '\n': av_bprintf(dst, "%s", "\\n"); break;
        case '\r': av_bprintf(dst, "%s", "\\r"); break;
        case '\\': av_bprintf(dst, "%s", "\\\\"); break;
        default:
            if (*p == sep)
                av_bprint_chars(dst, '\\', 1);
            av_bprint_chars(dst, *p, 1);
        }
    }
    return dst->str;
}

/**
 * Quote fields containing special characters, check RFC4180.
 */
static const char *csv_escape_str(AVBPrint *dst, const char *src, const char sep, void *log_ctx)
{
    char meta_chars[] = { sep, '"', '\n', '\r', '\0' };

    int needs_quoting = !!src[strcspn(src, meta_chars)];

    if (needs_quoting)
        av_bprint_chars(dst, '"', 1);

    for (; *src; src++) {
        if (*src == '"')
            av_bprint_chars(dst, '"', 1);
        av_bprint_chars(dst, *src, 1);
    }
    if (needs_quoting)
        av_bprint_chars(dst, '"', 1);
    return dst->str;
}

static const char *none_escape_str(AVBPrint *dst, const char *src, const char sep, void *log_ctx)
{
    return src;
}

typedef struct CompactContext {
    const AVClass *class;
    char *item_sep_str;
    char item_sep;
    int nokey;
    int print_section;
    char *escape_mode_str;
    const char * (*escape_str)(AVBPrint *dst, const char *src, const char sep, void *log_ctx);
    int nested_section[SECTION_MAX_NB_LEVELS];
    int has_nested_elems[SECTION_MAX_NB_LEVELS];
    int terminate_line[SECTION_MAX_NB_LEVELS];
} CompactContext;

#undef OFFSET
#define OFFSET(x) offsetof(CompactContext, x)

static const AVOption compact_options[] = {
    { "item_sep", "set item separator",      OFFSET(item_sep_str),    AV_OPT_TYPE_STRING, { .str = "|" },  0, 0 },
    { "s",        "set item separator",      OFFSET(item_sep_str),    AV_OPT_TYPE_STRING, { .str = "|" },  0, 0 },
    { "nokey",    "force no key printing",   OFFSET(nokey),           AV_OPT_TYPE_BOOL,   { .i64 = 0   },  0, 1 },
    { "nk",       "force no key printing",   OFFSET(nokey),           AV_OPT_TYPE_BOOL,   { .i64 = 0   },  0, 1 },
    { "escape",   "set escape mode",         OFFSET(escape_mode_str), AV_OPT_TYPE_STRING, { .str = "c" },  0, 0 },
    { "e",        "set escape mode",         OFFSET(escape_mode_str), AV_OPT_TYPE_STRING, { .str = "c" },  0, 0 },
    { "print_section", "print section name", OFFSET(print_section),   AV_OPT_TYPE_BOOL,   { .i64 = 1   },  0, 1 },
    { "p",             "print section name", OFFSET(print_section),   AV_OPT_TYPE_BOOL,   { .i64 = 1   },  0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(compact);

static av_cold int compact_init(AVTextFormatContext *wctx)
{
    CompactContext *compact = wctx->priv;

    if (strlen(compact->item_sep_str) != 1) {
        av_log(wctx, AV_LOG_ERROR, "Item separator '%s' specified, but must contain a single character\n",
               compact->item_sep_str);
        return AVERROR(EINVAL);
    }
    compact->item_sep = compact->item_sep_str[0];

    if        (!strcmp(compact->escape_mode_str, "none")) {
        compact->escape_str = none_escape_str;
    } else if (!strcmp(compact->escape_mode_str, "c"   )) {
        compact->escape_str = c_escape_str;
    } else if (!strcmp(compact->escape_mode_str, "csv" )) {
        compact->escape_str = csv_escape_str;
    } else {
        av_log(wctx, AV_LOG_ERROR, "Unknown escape mode '%s'\n", compact->escape_mode_str);
        return AVERROR(EINVAL);
    }

    return 0;
}

static void compact_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    CompactContext *compact = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);

    if (!section)
        return;

    compact->terminate_line[wctx->level] = 1;
    compact->has_nested_elems[wctx->level] = 0;

    av_bprint_clear(&wctx->section_pbuf[wctx->level]);
    if (parent_section &&
        (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_TYPE ||
            (!(section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) &&
                !(parent_section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY))))) {

        /* define a prefix for elements not contained in an array or
           in a wrapper, or for array elements with a type */
        const char *element_name = (char *)av_x_if_null(section->element_name, section->name);
        AVBPrint *section_pbuf = &wctx->section_pbuf[wctx->level];

        compact->nested_section[wctx->level] = 1;
        compact->has_nested_elems[wctx->level - 1] = 1;

        av_bprintf(section_pbuf, "%s%s",
                   wctx->section_pbuf[wctx->level - 1].str, element_name);

        if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_TYPE) {
            // add /TYPE to prefix
            av_bprint_chars(section_pbuf, '/', 1);

            // normalize section type, replace special characters and lower case
            for (const char *p = section->get_type(data); *p; p++) {
                char c =
                    (*p >= '0' && *p <= '9') ||
                    (*p >= 'a' && *p <= 'z') ||
                    (*p >= 'A' && *p <= 'Z') ? av_tolower(*p) : '_';
                av_bprint_chars(section_pbuf, c, 1);
            }
        }
        av_bprint_chars(section_pbuf, ':', 1);

        wctx->nb_item[wctx->level] = wctx->nb_item[wctx->level - 1];
    } else {
        if (parent_section && !(parent_section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)) &&
            wctx->level && wctx->nb_item[wctx->level - 1])
            writer_w8(wctx, compact->item_sep);
        if (compact->print_section &&
            !(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)))
            writer_printf(wctx, "%s%c", section->name, compact->item_sep);
    }
}

static void compact_print_section_footer(AVTextFormatContext *wctx)
{
    CompactContext *compact = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);

    if (!section)
        return;

    if (!compact->nested_section[wctx->level] &&
        compact->terminate_line[wctx->level] &&
        !(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER | AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)))
        writer_w8(wctx, '\n');
}

static void compact_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    CompactContext *compact = wctx->priv;
    AVBPrint buf;

    if (wctx->nb_item[wctx->level])
        writer_w8(wctx, compact->item_sep);

    if (!compact->nokey)
        writer_printf(wctx, "%s%s=", wctx->section_pbuf[wctx->level].str, key);

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_put_str(wctx, compact->escape_str(&buf, value, compact->item_sep, wctx));
    av_bprint_finalize(&buf, NULL);
}

static void compact_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    CompactContext *compact = wctx->priv;

    if (wctx->nb_item[wctx->level])
        writer_w8(wctx, compact->item_sep);

    if (!compact->nokey)
        writer_printf(wctx, "%s%s=", wctx->section_pbuf[wctx->level].str, key);

    writer_printf(wctx, "%"PRId64, value);
}

const AVTextFormatter avtextformatter_compact = {
    .name                 = "compact",
    .priv_size            = sizeof(CompactContext),
    .init                 = compact_init,
    .print_section_header = compact_print_section_header,
    .print_section_footer = compact_print_section_footer,
    .print_integer        = compact_print_int,
    .print_string         = compact_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS,
    .priv_class           = &compact_class,
};

/* CSV output */

#undef OFFSET
#define OFFSET(x) offsetof(CompactContext, x)

static const AVOption csv_options[] = {
    { "item_sep", "set item separator",      OFFSET(item_sep_str),    AV_OPT_TYPE_STRING, { .str = ","   }, 0, 0 },
    { "s",        "set item separator",      OFFSET(item_sep_str),    AV_OPT_TYPE_STRING, { .str = ","   }, 0, 0 },
    { "nokey",    "force no key printing",   OFFSET(nokey),           AV_OPT_TYPE_BOOL,   { .i64 = 1     }, 0, 1 },
    { "nk",       "force no key printing",   OFFSET(nokey),           AV_OPT_TYPE_BOOL,   { .i64 = 1     }, 0, 1 },
    { "escape",   "set escape mode",         OFFSET(escape_mode_str), AV_OPT_TYPE_STRING, { .str = "csv" }, 0, 0 },
    { "e",        "set escape mode",         OFFSET(escape_mode_str), AV_OPT_TYPE_STRING, { .str = "csv" }, 0, 0 },
    { "print_section", "print section name", OFFSET(print_section),   AV_OPT_TYPE_BOOL,   { .i64 = 1     }, 0, 1 },
    { "p",             "print section name", OFFSET(print_section),   AV_OPT_TYPE_BOOL,   { .i64 = 1     }, 0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(csv);

const AVTextFormatter avtextformatter_csv = {
    .name                 = "csv",
    .priv_size            = sizeof(CompactContext),
    .init                 = compact_init,
    .print_section_header = compact_print_section_header,
    .print_section_footer = compact_print_section_footer,
    .print_integer        = compact_print_int,
    .print_string         = compact_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS,
    .priv_class           = &csv_class,
};


/* ========== fftools/textformat/tf_flat.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"


/* Flat output */

typedef struct FlatContext {
    const AVClass *class;
    const char *sep_str;
    char sep;
    int hierarchical;
} FlatContext;

#undef OFFSET
#define OFFSET(x) offsetof(FlatContext, x)

static const AVOption flat_options[] = {
    { "sep_char",     "set separator",                                               OFFSET(sep_str),      AV_OPT_TYPE_STRING, { .str = "." }, 0, 0 },
    { "s",            "set separator",                                               OFFSET(sep_str),      AV_OPT_TYPE_STRING, { .str = "." }, 0, 0 },
    { "hierarchical", "specify if the section specification should be hierarchical", OFFSET(hierarchical), AV_OPT_TYPE_BOOL,   { .i64 = 1   }, 0, 1 },
    { "h",            "specify if the section specification should be hierarchical", OFFSET(hierarchical), AV_OPT_TYPE_BOOL,   { .i64 = 1   }, 0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(flat);

static av_cold int flat_init(AVTextFormatContext *wctx)
{
    FlatContext *flat = wctx->priv;

    if (strlen(flat->sep_str) != 1) {
        av_log(wctx, AV_LOG_ERROR, "Item separator '%s' specified, but must contain a single character\n",
               flat->sep_str);
        return AVERROR(EINVAL);
    }
    flat->sep = flat->sep_str[0];

    return 0;
}

static const char *flat_escape_key_str(AVBPrint *dst, const char *src, const char sep)
{
    const char *p;

    for (p = src; *p; p++) {
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z')))
            av_bprint_chars(dst, '_', 1);
        else
            av_bprint_chars(dst, *p, 1);
    }
    return dst->str;
}

static const char *flat_escape_value_str(AVBPrint *dst, const char *src)
{
    const char *p;

    for (p = src; *p; p++) {
        switch (*p) {
        case '\n': av_bprintf(dst, "%s", "\\n");  break;
        case '\r': av_bprintf(dst, "%s", "\\r");  break;
        case '\\': av_bprintf(dst, "%s", "\\\\"); break;
        case '"':  av_bprintf(dst, "%s", "\\\""); break;
        case '`':  av_bprintf(dst, "%s", "\\`");  break;
        case '$':  av_bprintf(dst, "%s", "\\$");  break;
        default:   av_bprint_chars(dst, *p, 1);   break;
        }
    }
    return dst->str;
}

static void flat_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    FlatContext *flat = wctx->priv;
    AVBPrint *buf = &wctx->section_pbuf[wctx->level];
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);

    if (!section)
        return;

    /* build section header */
    av_bprint_clear(buf);
    if (!parent_section)
        return;

    av_bprintf(buf, "%s", wctx->section_pbuf[wctx->level - 1].str);

    if (flat->hierarchical ||
        !(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER))) {
        av_bprintf(buf, "%s%s", wctx->section[wctx->level]->name, flat->sep_str);

        if (parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
            int n = parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE
                ? wctx->nb_item_type[wctx->level - 1][section->id]
                : wctx->nb_item[wctx->level - 1];

            av_bprintf(buf, "%d%s", n, flat->sep_str);
        }
    }
}

static void flat_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    writer_printf(wctx, "%s%s=%"PRId64"\n", wctx->section_pbuf[wctx->level].str, key, value);
}

static void flat_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    FlatContext *flat = wctx->priv;
    AVBPrint buf;

    writer_put_str(wctx, wctx->section_pbuf[wctx->level].str);
    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "%s=", flat_escape_key_str(&buf, key, flat->sep));
    av_bprint_clear(&buf);
    writer_printf(wctx, "\"%s\"\n", flat_escape_value_str(&buf, value));
    av_bprint_finalize(&buf, NULL);
}

const AVTextFormatter avtextformatter_flat = {
    .name                  = "flat",
    .priv_size             = sizeof(FlatContext),
    .init                  = flat_init,
    .print_section_header  = flat_print_section_header,
    .print_integer         = flat_print_int,
    .print_string          = flat_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS | AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT,
    .priv_class            = &flat_class,
};


/* ========== fftools/textformat/tf_ini.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"

#include "libavutil/bprint.h"
#include "libavutil/opt.h"


/* Default output */

typedef struct IniStrayDefault_unused {
    const AVClass *class;
    int nokey;
    int noprint_wrappers;
    int nested_section[SECTION_MAX_NB_LEVELS];
} IniStrayDefault_unused;

/* INI format output */

typedef struct INIContext {
    const AVClass *class;
    int hierarchical;
} INIContext;

#undef OFFSET
#define OFFSET(x) offsetof(INIContext, x)

static const AVOption ini_options[] = {
    { "hierarchical", "specify if the section specification should be hierarchical", OFFSET(hierarchical), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1 },
    { "h",            "specify if the section specification should be hierarchical", OFFSET(hierarchical), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(ini);

static char *ini_escape_str(AVBPrint *dst, const char *src)
{
    int i = 0;
    char c;

    while ((c = src[i++])) {
        switch (c) {
        case '\b': av_bprintf(dst, "%s", "\\b"); break;
        case '\f': av_bprintf(dst, "%s", "\\f"); break;
        case '\n': av_bprintf(dst, "%s", "\\n"); break;
        case '\r': av_bprintf(dst, "%s", "\\r"); break;
        case '\t': av_bprintf(dst, "%s", "\\t"); break;
        case '\\':
        case '#':
        case '=':
        case ':':
            av_bprint_chars(dst, '\\', 1);
            /* fallthrough */
        default:
            if ((unsigned char)c < 32)
                av_bprintf(dst, "\\x00%02x", (unsigned char)c);
            else
                av_bprint_chars(dst, c, 1);
            break;
        }
    }
    return dst->str;
}

static void ini_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    INIContext *ini = wctx->priv;
    AVBPrint *buf = &wctx->section_pbuf[wctx->level];
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);

    if (!section)
        return;

    av_bprint_clear(buf);
    if (!parent_section) {
        writer_put_str(wctx, "# ffprobe output\n\n");
        return;
    }

    if (wctx->nb_item[wctx->level - 1])
        writer_w8(wctx, '\n');

    av_bprintf(buf, "%s", wctx->section_pbuf[wctx->level - 1].str);
    if (ini->hierarchical ||
        !(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER))) {
        av_bprintf(buf, "%s%s", buf->str[0] ? "." : "", wctx->section[wctx->level]->name);

        if (parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
            unsigned n = parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE
                ? wctx->nb_item_type[wctx->level - 1][section->id]
                : wctx->nb_item[wctx->level - 1];
            av_bprintf(buf, ".%u", n);
        }
    }

    if (!(section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER)))
        writer_printf(wctx, "[%s]\n", buf->str);
}

static void ini_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    AVBPrint buf;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "%s=", ini_escape_str(&buf, key));
    av_bprint_clear(&buf);
    writer_printf(wctx, "%s\n", ini_escape_str(&buf, value));
    av_bprint_finalize(&buf, NULL);
}

static void ini_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    writer_printf(wctx, "%s=%"PRId64"\n", key, value);
}

const AVTextFormatter avtextformatter_ini = {
    .name                  = "ini",
    .priv_size             = sizeof(INIContext),
    .print_section_header  = ini_print_section_header,
    .print_integer         = ini_print_int,
    .print_string          = ini_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS | AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT,
    .priv_class            = &ini_class,
};


/* ========== fftools/textformat/tf_json.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"


/* JSON output */

typedef struct JSONContext {
    const AVClass *class;
    int indent_level;
    int compact;
    const char *item_sep, *item_start_end;
} JSONContext;

#undef OFFSET
#define OFFSET(x) offsetof(JSONContext, x)

static const AVOption json_options[] = {
    { "compact", "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { "c",       "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { NULL }
};

DEFINE_FORMATTER_CLASS(json);

static av_cold int json_init(AVTextFormatContext *wctx)
{
    JSONContext *json = wctx->priv;

    json->item_sep       = json->compact ? ", " : ",\n";
    json->item_start_end = json->compact ? " "  : "\n";

    return 0;
}

static const char *json_escape_str(AVBPrint *dst, const char *src, void *log_ctx)
{
    static const char json_escape[] = { '"', '\\', '\b', '\f', '\n', '\r', '\t', 0 };
    static const char json_subst[]  = { '"', '\\',  'b',  'f',  'n',  'r',  't', 0 };
    const char *p;

    if (!src) {
        av_log(log_ctx, AV_LOG_WARNING, "Cannot escape NULL string, returning NULL\n");
        return NULL;
    }

    for (p = src; *p; p++) {
        char *s = strchr(json_escape, *p);
        if (s) {
            av_bprint_chars(dst, '\\', 1);
            av_bprint_chars(dst, json_subst[s - json_escape], 1);
        } else if ((unsigned char)*p < 32) {
            av_bprintf(dst, "\\u00%02x", (unsigned char)*p);
        } else {
            av_bprint_chars(dst, *p, 1);
        }
    }
    return dst->str;
}

#define JSON_INDENT() writer_printf(wctx, "%*c", json->indent_level * 4, ' ')

static void json_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);
    JSONContext *json = wctx->priv;
    AVBPrint buf;

    if (!section)
        return;

    if (wctx->level && wctx->nb_item[wctx->level - 1])
        writer_put_str(wctx, ",\n");

    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER) {
        writer_put_str(wctx, "{\n");
        json->indent_level++;
    } else {
        av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
        json_escape_str(&buf, section->name, wctx);
        JSON_INDENT();

        json->indent_level++;
        if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
            writer_printf(wctx, "\"%s\": [\n", buf.str);
        } else if (parent_section && !(parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)) {
            writer_printf(wctx, "\"%s\": {%s", buf.str, json->item_start_end);
        } else {
            writer_printf(wctx, "{%s", json->item_start_end);

            /* this is required so the parser can distinguish between packets and frames */
            if (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE) {
                if (!json->compact)
                    JSON_INDENT();
                writer_printf(wctx, "\"type\": \"%s\"", section->name);
                wctx->nb_item[wctx->level]++;
            }
        }
        av_bprint_finalize(&buf, NULL);
    }
}

static void json_print_section_footer(AVTextFormatContext *wctx)
{
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    JSONContext *json = wctx->priv;

    if (!section)
        return;

    if (wctx->level == 0) {
        json->indent_level--;
        writer_put_str(wctx, "\n}\n");
    } else if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
        writer_w8(wctx, '\n');
        json->indent_level--;
        JSON_INDENT();
        writer_w8(wctx, ']');
    } else {
        writer_put_str(wctx, json->item_start_end);
        json->indent_level--;
        if (!json->compact)
            JSON_INDENT();
        writer_w8(wctx, '}');
    }
}

static inline void json_print_item_str(AVTextFormatContext *wctx,
                                       const char *key, const char *value)
{
    AVBPrint buf;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "\"%s\":", json_escape_str(&buf, key,   wctx));
    av_bprint_clear(&buf);
    writer_printf(wctx, " \"%s\"", json_escape_str(&buf, value, wctx));
    av_bprint_finalize(&buf, NULL);
}

static void json_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);
    JSONContext *json = wctx->priv;

    if (!section)
        return;

    if (wctx->nb_item[wctx->level] || (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE))
        writer_put_str(wctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();
    json_print_item_str(wctx, key, value);
}

static void json_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);
    JSONContext *json = wctx->priv;
    AVBPrint buf;

    if (!section)
        return;

    if (wctx->nb_item[wctx->level] || (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE))
        writer_put_str(wctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "\"%s\": %"PRId64, json_escape_str(&buf, key, wctx), value);
    av_bprint_finalize(&buf, NULL);
}

const AVTextFormatter avtextformatter_json = {
    .name                 = "json",
    .priv_size            = sizeof(JSONContext),
    .init                 = json_init,
    .print_section_header = json_print_section_header,
    .print_section_footer = json_print_section_footer,
    .print_integer        = json_print_int,
    .print_string         = json_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT,
    .priv_class           = &json_class,
};


/* ========== fftools/textformat/tf_mermaid.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avtextformat.h"


/* --- inlined tf_mermaid.h --- */
typedef enum {
    AV_DIAGRAMTYPE_GRAPH,
    AV_DIAGRAMTYPE_ENTITYRELATIONSHIP,
} AVDiagramType;

typedef struct AVDiagramConfig {
    AVDiagramType diagram_type;
    const char *diagram_css;
    const char *html_template;
} AVDiagramConfig;


void av_diagram_init(AVTextFormatContext *tfc, AVDiagramConfig *diagram_config);

void av_mermaid_set_html_template(AVTextFormatContext *tfc, const char *html_template);
/* --- end tf_mermaid.h --- */

#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"


static const char *init_directive = ""
    "%%{init: {"
        "\"theme\": \"base\","
        "\"curve\": \"monotoneX\","
        "\"rankSpacing\": 10,"
        "\"nodeSpacing\": 10,"
        "\"themeCSS\": \"__###__\","
        "\"fontFamily\": \"Roboto,Segoe UI,sans-serif\","
        "\"themeVariables\": { "
            "\"clusterBkg\": \"white\", "
            "\"primaryBorderColor\": \"gray\", "
            "\"lineColor\": \"gray\", "
            "\"secondaryTextColor\": \"gray\", "
            "\"tertiaryBorderColor\": \"gray\", "
            "\"primaryTextColor\": \"#666\", "
            "\"secondaryTextColor\": \"red\" "
        "},"
        "\"flowchart\": { "
            "\"subGraphTitleMargin\": { \"top\": -15, \"bottom\": 20 }, "
            "\"diagramPadding\": 20, "
            "\"curve\": \"monotoneX\" "
        "}"
    " }}%%\n\n";

static const char* init_directive_er = ""
    "%%{init: {"
        "\"theme\": \"base\","
        "\"layout\": \"elk\","
        "\"curve\": \"monotoneX\","
        "\"rankSpacing\": 65,"
        "\"nodeSpacing\": 60,"
        "\"themeCSS\": \"__###__\","
        "\"fontFamily\": \"Roboto,Segoe UI,sans-serif\","
        "\"themeVariables\": { "
            "\"clusterBkg\": \"white\", "
            "\"primaryBorderColor\": \"gray\", "
            "\"lineColor\": \"gray\", "
            "\"secondaryTextColor\": \"gray\", "
            "\"tertiaryBorderColor\": \"gray\", "
            "\"primaryTextColor\": \"#666\", "
            "\"secondaryTextColor\": \"red\" "
        "},"
        "\"er\": { "
            "\"diagramPadding\": 12, "
            "\"entityPadding\": 4, "
            "\"minEntityWidth\": 150, "
            "\"minEntityHeight\": 20, "
            "\"curve\": \"monotoneX\" "
        "}"
    " }}%%\n\n";

static const char *theme_css_er = ""

    // Variables
            ".root { "
                "--ff-colvideo: #6eaa7b; "
                "--ff-colaudio: #477fb3; "
                "--ff-colsubtitle: #ad76ab; "
                "--ff-coltext: #666; "
            "} "
            " g.nodes g.node.default rect.basic.label-container, "
            " g.nodes g.node.default path { "
            "     rx: 1; "
            "     ry: 1; "
            "     stroke-width: 1px !important; "
            "     stroke: #e9e9e9 !important; "
            "     fill: url(#ff-filtergradient) !important; "
            "     filter: drop-shadow(0px 0px 5.5px rgba(0, 0, 0, 0.05)); "
            "     fill: white !important; "
            " } "
            "  "
            " .relationshipLine { "
            "     stroke: gray; "
            "     stroke-width: 1; "
            "     fill: none; "
            "     filter: drop-shadow(0px 0px 3px rgba(0, 0, 0, 0.2)); "
            " } "
            "  "
            " g.node.default g.label.name  foreignObject > div > span > p, "
            " g.nodes g.node.default g.label:not(.attribute-name, .attribute-keys, .attribute-type, .attribute-comment) foreignObject > div > span > p { "
            "     font-size: 0.95rem; "
            "     font-weight: 500; "
            "     text-transform: uppercase; "
            "     min-width: 5.5rem; "
            "     margin-bottom: 0.5rem; "
            "      "
            " } "
            "  "
            " .edgePaths path { "
            "     marker-end: none; "
            "     marker-start: none; "
            "  "
            "} ";


/* Mermaid Graph output */

typedef struct MermaidContext {
    const AVClass *class;
    AVDiagramConfig *diagram_config;
    int subgraph_count;
    int within_tag;
    int indent_level;
    int create_html;

    // Options
    int enable_link_colors; // Requires Mermaid 11.5

    struct section_data {
        const char *section_id;
        const char *section_type;
        const char *src_id;
        const char *dest_id;
        AVTextFormatLinkType link_type;
        int current_is_textblock;
        int current_is_stadium;
        int subgraph_start_incomplete;
    }  section_data[SECTION_MAX_NB_LEVELS];

    unsigned nb_link_captions[SECTION_MAX_NB_LEVELS]; ///< generic print buffer dedicated to each section,
    AVBPrint link_buf; ///< print buffer for writing diagram links
    AVDictionary *link_dict;
} MermaidContext;

#undef OFFSET
#define OFFSET(x) offsetof(MermaidContext, x)

static const AVOption mermaid_options[] = {
    { "link_coloring",    "enable colored links (requires Mermaid >= 11.5)",  OFFSET(enable_link_colors), AV_OPT_TYPE_BOOL,   { .i64 = 1 },  0, 1 },
    ////{"diagram_css",      "CSS for the diagram",                              OFFSET(diagram_css),        AV_OPT_TYPE_STRING, {.i64=0},  0, 1 },
    ////{"html_template",    "Template HTML",                                    OFFSET(html_template),      AV_OPT_TYPE_STRING, {.i64=0},  0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(mermaid);

void av_diagram_init(AVTextFormatContext *tfc, AVDiagramConfig *diagram_config)
{
    MermaidContext *mmc = tfc->priv;
    mmc->diagram_config = diagram_config;
}

static av_cold int has_link_pair(const AVTextFormatContext *tfc, const char *src, const char *dest)
{
    MermaidContext *mmc = tfc->priv;
    AVBPrint buf;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&buf, "%s--%s", src, dest);

    if (mmc->link_dict && av_dict_get(mmc->link_dict, buf.str, NULL, 0))
        return 1;

    av_dict_set(&mmc->link_dict, buf.str, buf.str, 0);

    return 0;
}

static av_cold int mermaid_init(AVTextFormatContext *tfc)
{
    MermaidContext *mmc = tfc->priv;

    av_bprint_init(&mmc->link_buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    ////mmc->enable_link_colors = 1; // Requires Mermaid 11.5
    return 0;
}

static av_cold int mermaid_init_html(AVTextFormatContext *tfc)
{
    MermaidContext *mmc = tfc->priv;

    int ret = mermaid_init(tfc);

    if (ret < 0)
        return ret;

    mmc->create_html = 1;

    return 0;
}

static av_cold int mermaid_uninit(AVTextFormatContext *tfc)
{
    MermaidContext *mmc = tfc->priv;

    av_bprint_finalize(&mmc->link_buf, NULL);
    av_dict_free(&mmc->link_dict);

    for (unsigned i = 0; i < SECTION_MAX_NB_LEVELS; i++) {
        av_freep(&mmc->section_data[i].dest_id);
        av_freep(&mmc->section_data[i].section_id);
        av_freep(&mmc->section_data[i].src_id);
        av_freep(&mmc->section_data[i].section_type);
    }

    return 0;
}

static void set_str(const char **dst, const char *src)
{
    if (*dst)
        av_freep(dst);

    if (src)
        *dst = av_strdup(src);
}

static void mermaid_subgraph_complete_start(MermaidContext *mmc, AVTextFormatContext *tfc, int level) {
    struct section_data parent_sec_data = mmc->section_data[level];
    AVBPrint *parent_buf = &tfc->section_pbuf[level];

    if (parent_sec_data.subgraph_start_incomplete) {
        if (parent_buf->len > 0)
            writer_printf(tfc, "%s", parent_buf->str);

        writer_put_str(tfc, "</div>\"]\n");

        mmc->section_data[level].subgraph_start_incomplete = 0;
    }
}

#define MM_INDENT() writer_printf(tfc, "%*c", mmc->indent_level * 2, ' ')

static void mermaid_print_section_header(AVTextFormatContext *tfc, const void *data)
{
    const AVTextFormatSection *section = tf_get_section(tfc, tfc->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(tfc, tfc->level);

    if (!section)
        return;
    AVBPrint *buf = &tfc->section_pbuf[tfc->level];
    MermaidContext *mmc = tfc->priv;
    const AVTextFormatSectionContext *sec_ctx = data;

    if (tfc->level == 0) {
        char *directive;
        AVBPrint css_buf;
        const char *diag_directive = mmc->diagram_config->diagram_type == AV_DIAGRAMTYPE_ENTITYRELATIONSHIP ? init_directive_er : init_directive;
        char *single_line_css = av_strireplace(mmc->diagram_config->diagram_css, "\n", " ");
        (void)theme_css_er;
        ////char *single_line_css = av_strireplace(theme_css_er, "\n", " ");
        av_bprint_init(&css_buf, 0, AV_BPRINT_SIZE_UNLIMITED);
        av_bprint_escape(&css_buf, single_line_css, "'\\", AV_ESCAPE_MODE_BACKSLASH, AV_ESCAPE_FLAG_STRICT);
        av_freep(&single_line_css);

        directive = av_strireplace(diag_directive, "__###__", css_buf.str);
        if (mmc->create_html) {
            uint64_t length;
            char *token_pos = av_stristr(mmc->diagram_config->html_template, "__###__");
            if (!token_pos) {
                av_log(tfc, AV_LOG_ERROR, "Unable to locate the required token (__###__) in the html template.");
                return;
            }

            length = token_pos - mmc->diagram_config->html_template;
            for (uint64_t i = 0; i < length; i++)
                writer_w8(tfc, mmc->diagram_config->html_template[i]);
        }

        writer_put_str(tfc, directive);
        switch (mmc->diagram_config->diagram_type) {
        case AV_DIAGRAMTYPE_GRAPH:
            writer_put_str(tfc, "flowchart LR\n");
        ////writer_put_str(tfc, "  gradient_def@{ shape: text, label: \"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"><defs><linearGradient id=\"ff-filtergradient\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\"><stop offset=\"0%\" style=\"stop-color:hsla(0, 0%, 30%, 0.02);\"/><stop offset=\"50%\" style=\"stop-color:hsla(0, 0%, 30%, 0);\"/><stop offset=\"100%\" style=\"stop-color:hsla(0, 0%, 30%, 0.05);\"/></linearGradient></defs></svg>\" }\n");
            writer_put_str(tfc, "  gradient_def@{ shape: text, label: \"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"><defs><linearGradient id=\"ff-filtergradient\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\"><stop offset=\"0%\" style=\"stop-color:hsl(0, 0%, 98.6%);     \"/><stop offset=\"50%\" style=\"stop-color:hsl(0, 0%, 100%);   \"/><stop offset=\"100%\" style=\"stop-color:hsl(0, 0%, 96.5%);     \"/></linearGradient><radialGradient id=\"ff-radgradient\" cx=\"50%\" cy=\"50%\" r=\"100%\" fx=\"45%\" fy=\"40%\"><stop offset=\"25%\" stop-color=\"hsl(0, 0%, 100%)\" /><stop offset=\"100%\" stop-color=\"hsl(0, 0%, 96%)\" /></radialGradient></defs></svg>\" }\n");
            break;
        case AV_DIAGRAMTYPE_ENTITYRELATIONSHIP:
            writer_put_str(tfc, "erDiagram\n");
            break;
        }

        av_bprint_finalize(&css_buf, NULL);
        av_freep(&directive);
        return;
    }

    if (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH) {
        mermaid_subgraph_complete_start(mmc, tfc, tfc->level - 1);
    }

    av_freep(&mmc->section_data[tfc->level].section_id);
    av_freep(&mmc->section_data[tfc->level].section_type);
    av_freep(&mmc->section_data[tfc->level].src_id);
    av_freep(&mmc->section_data[tfc->level].dest_id);
    mmc->section_data[tfc->level].current_is_textblock = 0;
    mmc->section_data[tfc->level].current_is_stadium = 0;
    mmc->section_data[tfc->level].subgraph_start_incomplete = 0;
    mmc->section_data[tfc->level].link_type = AV_TEXTFORMAT_LINKTYPE_SRCDEST;

    // NOTE: av_strdup() allocations aren't checked
    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH) {

        av_bprint_clear(buf);
        writer_put_str(tfc, "\n");

        mmc->indent_level++;

        if (sec_ctx->context_id) {
            MM_INDENT();
            writer_printf(tfc, "subgraph %s[\"<div class=\"ff-%s\">", sec_ctx->context_id, section->name);
        } else {
            av_log(tfc, AV_LOG_ERROR, "Unable to write subgraph start. Missing id field. Section: %s", section->name);
        }

        mmc->section_data[tfc->level].subgraph_start_incomplete = 1;
        set_str(&mmc->section_data[tfc->level].section_id, sec_ctx->context_id);
    }

    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE) {

        av_bprint_clear(buf);
        writer_put_str(tfc, "\n");

        mmc->indent_level++;

        if (sec_ctx->context_id) {

            set_str(&mmc->section_data[tfc->level].section_id, sec_ctx->context_id);

            switch (mmc->diagram_config->diagram_type) {
            case AV_DIAGRAMTYPE_GRAPH:
                if (sec_ctx->context_flags & 1) {

                    MM_INDENT();
                    writer_printf(tfc, "%s@{ shape: text, label: \"", sec_ctx->context_id);
                    mmc->section_data[tfc->level].current_is_textblock = 1;
                } else if (sec_ctx->context_flags & 2) {

                    MM_INDENT();
                    writer_printf(tfc, "%s([\"", sec_ctx->context_id);
                    mmc->section_data[tfc->level].current_is_stadium = 1;
                } else {
                    MM_INDENT();
                    writer_printf(tfc, "%s(\"", sec_ctx->context_id);
                }

                break;
            case AV_DIAGRAMTYPE_ENTITYRELATIONSHIP:
                MM_INDENT();
                writer_printf(tfc, "%s {\n", sec_ctx->context_id);
                break;
            }

        } else {
            av_log(tfc, AV_LOG_ERROR, "Unable to write shape start. Missing id field. Section: %s", section->name);
        }

        set_str(&mmc->section_data[tfc->level].section_id, sec_ctx->context_id);
    }


    if (section->flags & AV_TEXTFORMAT_SECTION_PRINT_TAGS) {

        if (sec_ctx && sec_ctx->context_type)
            writer_printf(tfc, "<div class=\"ff-%s %s\">", section->name, sec_ctx->context_type);
        else
            writer_printf(tfc, "<div class=\"ff-%s\">", section->name);
    }


    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS) {

        av_bprint_clear(buf);
        mmc->nb_link_captions[tfc->level] = 0;

        if (sec_ctx && sec_ctx->context_type)
            set_str(&mmc->section_data[tfc->level].section_type, sec_ctx->context_type);

        ////if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_TYPE) {
        ////    AVBPrint buf;
        ////    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
        ////    av_bprint_escape(&buf, section->get_type(data), NULL,
        ////                     AV_ESCAPE_MODE_XML, AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
        ////    writer_printf(tfc, " type=\"%s\"", buf.str);
    }
}

static void mermaid_print_section_footer(AVTextFormatContext *tfc)
{
    MermaidContext *mmc = tfc->priv;
    const AVTextFormatSection *section = tf_get_section(tfc, tfc->level);

    if (!section)
        return;
    AVBPrint *buf = &tfc->section_pbuf[tfc->level];
    struct section_data sec_data = mmc->section_data[tfc->level];

    if (section->flags & AV_TEXTFORMAT_SECTION_PRINT_TAGS)
        writer_put_str(tfc, "</div>");

    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE) {

        switch (mmc->diagram_config->diagram_type) {
        case AV_DIAGRAMTYPE_GRAPH:

            if (sec_data.current_is_textblock) {
                writer_printf(tfc, "\"}\n", section->name);

                if (sec_data.section_id) {
                    MM_INDENT();
                    writer_put_str(tfc, "class ");
                    writer_put_str(tfc, sec_data.section_id);
                    writer_put_str(tfc, " ff-");
                    writer_put_str(tfc, section->name);
                    writer_put_str(tfc, "\n");
                }
            } else if (sec_data.current_is_stadium) {
                writer_printf(tfc, "\"]):::ff-%s\n", section->name);
            } else {
                writer_printf(tfc, "\"):::ff-%s\n", section->name);
            }

            break;
        case AV_DIAGRAMTYPE_ENTITYRELATIONSHIP:
            MM_INDENT();
            writer_put_str(tfc, "}\n\n");
            break;
        }

        mmc->indent_level--;

    } else if ((section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH)) {

        mermaid_subgraph_complete_start(mmc, tfc, tfc->level);

        MM_INDENT();
        writer_put_str(tfc, "end\n");

        if (sec_data.section_id) {
            MM_INDENT();
            writer_put_str(tfc, "class ");
            writer_put_str(tfc, sec_data.section_id);
            writer_put_str(tfc, " ff-");
            writer_put_str(tfc, section->name);
            writer_put_str(tfc, "\n");
        }

        mmc->indent_level--;
    }

    if ((section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS))
        if (sec_data.src_id && sec_data.dest_id
            && !has_link_pair(tfc, sec_data.src_id, sec_data.dest_id))
            switch (mmc->diagram_config->diagram_type) {
            case AV_DIAGRAMTYPE_GRAPH:

                if (sec_data.section_type && mmc->enable_link_colors)
                    av_bprintf(&mmc->link_buf, "\n  %s %s-%s-%s@==", sec_data.src_id, sec_data.section_type, sec_data.src_id, sec_data.dest_id);
                else
                    av_bprintf(&mmc->link_buf, "\n  %s ==", sec_data.src_id);

                if (buf->len > 0) {
                    av_bprintf(&mmc->link_buf, " \"%s", buf->str);

                    for (unsigned i = 0; i < mmc->nb_link_captions[tfc->level]; i++)
                        av_bprintf(&mmc->link_buf, "<br>&nbsp;");

                    av_bprintf(&mmc->link_buf, "\" ==");
                }

                av_bprintf(&mmc->link_buf, "> %s", sec_data.dest_id);

                break;
            case AV_DIAGRAMTYPE_ENTITYRELATIONSHIP:


                av_bprintf(&mmc->link_buf, "\n  %s", sec_data.src_id);

                switch (sec_data.link_type) {
                case AV_TEXTFORMAT_LINKTYPE_ONETOMANY:
                    av_bprintf(&mmc->link_buf, "%s", " ||--o{ ");
                    break;
                case AV_TEXTFORMAT_LINKTYPE_MANYTOONE:
                    av_bprintf(&mmc->link_buf, "%s", " }o--|| ");
                    break;
                case AV_TEXTFORMAT_LINKTYPE_ONETOONE:
                    av_bprintf(&mmc->link_buf, "%s", " ||--|| ");
                    break;
                case AV_TEXTFORMAT_LINKTYPE_MANYTOMANY:
                    av_bprintf(&mmc->link_buf, "%s", " }o--o{ ");
                    break;
                default:
                    av_bprintf(&mmc->link_buf, "%s", " ||--|| ");
                    break;
                }

                av_bprintf(&mmc->link_buf, "%s : \"\"", sec_data.dest_id);

                break;
            }

    if (tfc->level == 0) {

        writer_put_str(tfc, "\n");
        if (mmc->create_html) {
            char *token_pos = av_stristr(mmc->diagram_config->html_template, "__###__");
            if (!token_pos) {
                av_log(tfc, AV_LOG_ERROR, "Unable to locate the required token (__###__) in the html template.");
                return;
            }
            token_pos += strlen("__###__");
            writer_put_str(tfc, token_pos);
        }
    }

    if (tfc->level == 1) {

        if (mmc->link_buf.len > 0) {
            writer_put_str(tfc, mmc->link_buf.str);
            av_bprint_clear(&mmc->link_buf);
        }

        writer_put_str(tfc, "\n");
    }
}

static void mermaid_print_value(AVTextFormatContext *tfc, const char *key,
                                const char *str, int64_t num, const int is_int)
{
    MermaidContext *mmc = tfc->priv;
    const AVTextFormatSection *section = tf_get_section(tfc, tfc->level);

    if (!section)
        return;

    AVBPrint *buf = &tfc->section_pbuf[tfc->level];
    struct section_data sec_data = mmc->section_data[tfc->level];
    int exit = 0;

    if (section->id_key && !strcmp(section->id_key, key)) {
        set_str(&mmc->section_data[tfc->level].section_id, str);
        exit = 1;
    }

    if (section->dest_id_key && !strcmp(section->dest_id_key, key)) {
        set_str(&mmc->section_data[tfc->level].dest_id, str);
        exit = 1;
    }

    if (section->src_id_key && !strcmp(section->src_id_key, key)) {
        set_str(&mmc->section_data[tfc->level].src_id, str);
        exit = 1;
    }

    if (section->linktype_key && !strcmp(section->linktype_key, key)) {
        mmc->section_data[tfc->level].link_type = (AVTextFormatLinkType)num;
        exit = 1;
    }

    if (exit)
        return;

    if ((section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS))
        || (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH && sec_data.subgraph_start_incomplete)) {
        switch (mmc->diagram_config->diagram_type) {
        case AV_DIAGRAMTYPE_GRAPH:

            if (is_int) {
                writer_printf(tfc, "<span class=\"%s\">%s: %"PRId64"</span>", key, key, num);
            } else {
                const char *tmp = av_strireplace(str, "\"", "'");
                writer_printf(tfc, "<span class=\"%s\">%s</span>", key, tmp);
                av_freep(&tmp);
            }

            break;
        case AV_DIAGRAMTYPE_ENTITYRELATIONSHIP:

            if (!is_int && str)
            {
                const char *col_type;

                if (key[0] == '_')
                    return;

                if (sec_data.section_id && !strcmp(str, sec_data.section_id))
                    col_type = "PK";
                else if (sec_data.dest_id && !strcmp(str, sec_data.dest_id))
                    col_type = "FK";
                else if (sec_data.src_id && !strcmp(str, sec_data.src_id))
                    col_type = "FK";
                else
                    col_type = "";

                MM_INDENT();

                writer_printf(tfc, "    %s %s %s\n", key, str, col_type);
            }
            break;
        }

    } else if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS) {
        if (buf->len > 0)
            av_bprintf(buf, "%s", "<br>");

        av_bprintf(buf, "");
        if (is_int)
            av_bprintf(buf, "<span>%s: %"PRId64"</span>", key, num);
        else
            av_bprintf(buf, "<span>%s</span>", str);

        mmc->nb_link_captions[tfc->level]++;
    }
}

static inline void mermaid_print_str(AVTextFormatContext *tfc, const char *key, const char *value)
{
    mermaid_print_value(tfc, key, value, 0, 0);
}

static void mermaid_print_int(AVTextFormatContext *tfc, const char *key, int64_t value)
{
    mermaid_print_value(tfc, key, NULL, value, 1);
}

const AVTextFormatter avtextformatter_mermaid = {
    .name                 = "mermaid",
    .priv_size            = sizeof(MermaidContext),
    .init                 = mermaid_init,
    .uninit               = mermaid_uninit,
    .print_section_header = mermaid_print_section_header,
    .print_section_footer = mermaid_print_section_footer,
    .print_integer        = mermaid_print_int,
    .print_string         = mermaid_print_str,
    .flags = AV_TEXTFORMAT_FLAG_IS_DIAGRAM_FORMATTER,
    .priv_class           = &mermaid_class,
};


const AVTextFormatter avtextformatter_mermaidhtml = {
    .name                 = "mermaidhtml",
    .priv_size            = sizeof(MermaidContext),
    .init                 = mermaid_init_html,
    .uninit               = mermaid_uninit,
    .print_section_header = mermaid_print_section_header,
    .print_section_footer = mermaid_print_section_footer,
    .print_integer        = mermaid_print_int,
    .print_string         = mermaid_print_str,
    .flags = AV_TEXTFORMAT_FLAG_IS_DIAGRAM_FORMATTER,
    .priv_class           = &mermaid_class,
};


/* ========== fftools/textformat/tf_xml.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <string.h>

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"


/* XML output */

typedef struct XMLContext {
    const AVClass *class;
    int within_tag;
    int indent_level;
    int fully_qualified;
    int xsd_strict;
} XMLContext;

#undef OFFSET
#define OFFSET(x) offsetof(XMLContext, x)

static const AVOption xml_options[] = {
    { "fully_qualified", "specify if the output should be fully qualified", OFFSET(fully_qualified), AV_OPT_TYPE_BOOL, { .i64 = 0 },  0, 1 },
    { "q",               "specify if the output should be fully qualified", OFFSET(fully_qualified), AV_OPT_TYPE_BOOL, { .i64 = 0 },  0, 1 },
    { "xsd_strict",      "ensure that the output is XSD compliant",         OFFSET(xsd_strict),      AV_OPT_TYPE_BOOL, { .i64 = 0 },  0, 1 },
    { "x",               "ensure that the output is XSD compliant",         OFFSET(xsd_strict),      AV_OPT_TYPE_BOOL, { .i64 = 0 },  0, 1 },
    { NULL },
};

DEFINE_FORMATTER_CLASS(xml);

static av_cold int xml_init(AVTextFormatContext *wctx)
{
    XMLContext *xml = wctx->priv;

    if (xml->xsd_strict) {
        xml->fully_qualified = 1;
#define CHECK_COMPLIANCE(opt, opt_name)                                 \
        if (opt) {                                                      \
            av_log(wctx, AV_LOG_ERROR,                                  \
                   "XSD-compliant output selected but option '%s' was selected, XML output may be non-compliant.\n" \
                   "You need to disable such option with '-no%s'\n", opt_name, opt_name); \
            return AVERROR(EINVAL);                                     \
        }
        ////CHECK_COMPLIANCE(show_private_data, "private");
        CHECK_COMPLIANCE(wctx->opts.show_value_unit,   "unit");
        CHECK_COMPLIANCE(wctx->opts.use_value_prefix,  "prefix");
    }

    return 0;
}

#define XML_INDENT() writer_printf(wctx, "%*c", xml->indent_level * 4, ' ')

static void xml_print_section_header(AVTextFormatContext *wctx, const void *data)
{
    XMLContext *xml = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(wctx, wctx->level);

    if (!section)
        return;

    if (wctx->level == 0) {
        const char *qual = " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "xmlns:ffprobe=\"http://www.ffmpeg.org/schema/ffprobe\" "
            "xsi:schemaLocation=\"http://www.ffmpeg.org/schema/ffprobe ffprobe.xsd\"";

        writer_put_str(wctx, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        writer_printf(wctx, "<%sffprobe%s>\n",
                      xml->fully_qualified ? "ffprobe:" : "",
                      xml->fully_qualified ? qual : "");
        return;
    }

    if (xml->within_tag) {
        xml->within_tag = 0;
        writer_put_str(wctx, ">\n");
    }

    if (parent_section && (parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER) &&
        wctx->level && wctx->nb_item[wctx->level - 1])
        writer_w8(wctx, '\n');
    xml->indent_level++;

    if (section->flags & (AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_HAS_VARIABLE_FIELDS)) {
        XML_INDENT();
        writer_printf(wctx, "<%s", section->name);

        if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_TYPE) {
            AVBPrint buf;
            av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
            av_bprint_escape(&buf, section->get_type(data), NULL,
                             AV_ESCAPE_MODE_XML, AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
            writer_printf(wctx, " type=\"%s\"", buf.str);
        }
        writer_printf(wctx, ">\n", section->name);
    } else {
        XML_INDENT();
        writer_printf(wctx, "<%s ", section->name);
        xml->within_tag = 1;
    }
}

static void xml_print_section_footer(AVTextFormatContext *wctx)
{
    XMLContext *xml = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);

    if (!section)
        return;

    if (wctx->level == 0) {
        writer_printf(wctx, "</%sffprobe>\n", xml->fully_qualified ? "ffprobe:" : "");
    } else if (xml->within_tag) {
        xml->within_tag = 0;
        writer_put_str(wctx, "/>\n");
        xml->indent_level--;
    } else {
        XML_INDENT();
        writer_printf(wctx, "</%s>\n", section->name);
        xml->indent_level--;
    }
}

static void xml_print_value(AVTextFormatContext *wctx, const char *key,
                            const char *str, int64_t num, const int is_int)
{
    AVBPrint buf;
    XMLContext *xml = wctx->priv;
    const AVTextFormatSection *section = tf_get_section(wctx, wctx->level);

    if (!section)
        return;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);

    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_HAS_VARIABLE_FIELDS) {
        xml->indent_level++;
        XML_INDENT();
        av_bprint_escape(&buf, key, NULL,
                         AV_ESCAPE_MODE_XML, AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
        writer_printf(wctx, "<%s key=\"%s\"",
                      section->element_name, buf.str);
        av_bprint_clear(&buf);

        if (is_int) {
            writer_printf(wctx, " value=\"%"PRId64"\"/>\n", num);
        } else {
            av_bprint_escape(&buf, str, NULL,
                             AV_ESCAPE_MODE_XML, AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
            writer_printf(wctx, " value=\"%s\"/>\n", buf.str);
        }
        xml->indent_level--;
    } else {
        if (wctx->nb_item[wctx->level])
            writer_w8(wctx, ' ');

        if (is_int) {
            writer_printf(wctx, "%s=\"%"PRId64"\"", key, num);
        } else {
            av_bprint_escape(&buf, str, NULL,
                             AV_ESCAPE_MODE_XML, AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
            writer_printf(wctx, "%s=\"%s\"", key, buf.str);
        }
    }

    av_bprint_finalize(&buf, NULL);
}

static inline void xml_print_str(AVTextFormatContext *wctx, const char *key, const char *value)
{
    xml_print_value(wctx, key, value, 0, 0);
}

static void xml_print_int(AVTextFormatContext *wctx, const char *key, int64_t value)
{
    xml_print_value(wctx, key, NULL, value, 1);
}

const AVTextFormatter avtextformatter_xml = {
    .name                 = "xml",
    .priv_size            = sizeof(XMLContext),
    .init                 = xml_init,
    .print_section_header = xml_print_section_header,
    .print_section_footer = xml_print_section_footer,
    .print_integer        = xml_print_int,
    .print_string         = xml_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT,
    .priv_class           = &xml_class,
};


/* ========== fftools/textformat/tw_avio.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>
#include <string.h>

#include "avtextwriters.h"
#include "libavutil/avassert.h"

#include "libavutil/error.h"

/* AVIO Writer */

# define WRITER_NAME "aviowriter"

typedef struct IOWriterContext {
    const AVClass *class;
    AVIOContext *avio_context;
    int close_on_uninit;
} IOWriterContext;

static av_cold int iowriter_uninit(AVTextWriterContext *wctx)
{
    IOWriterContext *ctx = wctx->priv;
    int ret = 0;

    if (ctx->close_on_uninit)
        ret = avio_closep(&ctx->avio_context);
    return ret;
}

static void io_w8(AVTextWriterContext *wctx, int b)
{
    IOWriterContext *ctx = wctx->priv;
    avio_w8(ctx->avio_context, b);
}

static void io_put_str(AVTextWriterContext *wctx, const char *str)
{
    IOWriterContext *ctx = wctx->priv;
    avio_write(ctx->avio_context, (const unsigned char *)str, (int)strlen(str));
}

static void io_vprintf(AVTextWriterContext *wctx, const char *fmt, va_list vl)
{
    IOWriterContext *ctx = wctx->priv;

    avio_vprintf(ctx->avio_context, fmt, vl);
}


const AVTextWriter avtextwriter_avio = {
    .name                 = WRITER_NAME,
    .priv_size            = sizeof(IOWriterContext),
    .uninit               = iowriter_uninit,
    .writer_put_str       = io_put_str,
    .writer_vprintf       = io_vprintf,
    .writer_w8            = io_w8
};

int avtextwriter_create_file(AVTextWriterContext **pwctx, const char *output_filename)
{
    IOWriterContext *ctx;
    int ret;

    if (!output_filename || !output_filename[0]) {
        av_log(NULL, AV_LOG_ERROR, "The output_filename cannot be NULL or empty\n");
        return AVERROR(EINVAL);
    }

    ret = avtextwriter_context_open(pwctx, &avtextwriter_avio);
    if (ret < 0)
        return ret;

    ctx = (*pwctx)->priv;

    if ((ret = avio_open(&ctx->avio_context, output_filename, AVIO_FLAG_WRITE)) < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to open output '%s' with error: %s\n", output_filename, av_err2str(ret));
        avtextwriter_context_close(pwctx);
        return ret;
    }

    ctx->close_on_uninit = 1;

    return ret;
}


int avtextwriter_create_avio(AVTextWriterContext **pwctx, AVIOContext *avio_ctx, int close_on_uninit)
{
    IOWriterContext *ctx;
    int ret;

    av_assert0(avio_ctx);

    ret = avtextwriter_context_open(pwctx, &avtextwriter_avio);
    if (ret < 0)
        return ret;

    ctx = (*pwctx)->priv;
    ctx->avio_context = avio_ctx;
    ctx->close_on_uninit = close_on_uninit;

    return ret;
}


/* ========== fftools/textformat/tw_buffer.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdarg.h>

#include "avtextwriters.h"
#include "libavutil/opt.h"
#include "libavutil/bprint.h"

/* Buffer Writer */

# undef WRITER_NAME
# define WRITER_NAME "bufferwriter"

typedef struct BufferWriterContext {
    const AVClass *class;
    AVBPrint *buffer;
} BufferWriterContext;

static const char *bufferwriter_get_name(void *ctx)
{
    return WRITER_NAME;
}

static const AVClass bufferwriter_class = {
    .class_name = WRITER_NAME,
    .item_name = bufferwriter_get_name,
};

static void buffer_w8(AVTextWriterContext *wctx, int b)
{
    BufferWriterContext *ctx = wctx->priv;
    av_bprintf(ctx->buffer, "%c", b);
}

static void buffer_put_str(AVTextWriterContext *wctx, const char *str)
{
    BufferWriterContext *ctx = wctx->priv;
    av_bprintf(ctx->buffer, "%s", str);
}

static void buffer_vprintf(AVTextWriterContext *wctx, const char *fmt, va_list vl)
{
    BufferWriterContext *ctx = wctx->priv;

    av_vbprintf(ctx->buffer, fmt, vl);
}


const AVTextWriter avtextwriter_buffer = {
    .name                 = WRITER_NAME,
    .priv_size            = sizeof(BufferWriterContext),
    .priv_class           = &bufferwriter_class,
    .writer_put_str       = buffer_put_str,
    .writer_vprintf       = buffer_vprintf,
    .writer_w8            = buffer_w8
};

int avtextwriter_create_buffer(AVTextWriterContext **pwctx, AVBPrint *buffer)
{
    BufferWriterContext *ctx;
    int ret;

    ret = avtextwriter_context_open(pwctx, &avtextwriter_buffer);
    if (ret < 0)
        return ret;

    ctx = (*pwctx)->priv;
    ctx->buffer = buffer;

    return ret;
}


/* ========== fftools/textformat/tw_stdout.c ========== */

/*
 * Copyright (c) The FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "avtextwriters.h"
#include "libavutil/opt.h"

/* STDOUT Writer */

# undef WRITER_NAME
# define WRITER_NAME "stdoutwriter"

typedef struct StdOutWriterContext {
    const AVClass *class;
} StdOutWriterContext;

static const char *stdoutwriter_get_name(void *ctx)
{
    return WRITER_NAME;
}

static const AVClass stdoutwriter_class = {
    .class_name = WRITER_NAME,
    .item_name = stdoutwriter_get_name,
};

static inline void stdout_w8(AVTextWriterContext *wctx, int b)
{
    printf("%c", b);
}

static inline void stdout_put_str(AVTextWriterContext *wctx, const char *str)
{
    printf("%s", str);
}

static inline void stdout_vprintf(AVTextWriterContext *wctx, const char *fmt, va_list vl)
{
    vprintf(fmt, vl);
}


static const AVTextWriter avtextwriter_stdout = {
    .name                 = WRITER_NAME,
    .priv_size            = sizeof(StdOutWriterContext),
    .priv_class           = &stdoutwriter_class,
    .writer_put_str       = stdout_put_str,
    .writer_vprintf       = stdout_vprintf,
    .writer_w8            = stdout_w8
};

int avtextwriter_create_stdout(AVTextWriterContext **pwctx)
{
    int ret;

    ret = avtextwriter_context_open(pwctx, &avtextwriter_stdout);

    return ret;
}


/* ========== fftools/graph/graphprint.c ========== */

/*
 * Copyright (c) 2018-2025 - softworkz
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * output writers for filtergraph details
 */

#include <string.h>
#include <stdatomic.h>


/* --- inlined graphprint.h --- */
#include "fftools/ffmpeg.h"

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, InputFile **ifiles, int nb_ifiles, OutputFile **ofiles, int nb_ofiles);

int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph);
/* --- end graphprint.h --- */


#include "fftools/ffmpeg.h"
#include "fftools/ffmpeg_mux.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/common.h"
#include "libavfilter/avfilter.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "avtextformat.h"


/* --- inlined resman.h --- */
#include <stdint.h>

#include "config.h"
#include "fftools/ffmpeg.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
#include "avtextformat.h"

typedef enum {
    FF_RESOURCE_GRAPH_CSS,
    FF_RESOURCE_GRAPH_HTML,
} FFResourceId;

typedef struct FFResourceDefinition {
    FFResourceId resource_id;
    const char *name;

    const unsigned char *data;
    const unsigned *data_len;

} FFResourceDefinition;

void ff_resman_uninit(void);

char *ff_resman_get_string(FFResourceId resource_id);
/* --- end resman.h --- */


typedef enum {
    SECTION_ID_ROOT,
    SECTION_ID_FILTERGRAPHS,
    SECTION_ID_FILTERGRAPH,
    SECTION_ID_GRAPH_INPUTS,
    SECTION_ID_GRAPH_INPUT,
    SECTION_ID_GRAPH_OUTPUTS,
    SECTION_ID_GRAPH_OUTPUT,
    SECTION_ID_FILTERS,
    SECTION_ID_FILTER,
    SECTION_ID_FILTER_INPUTS,
    SECTION_ID_FILTER_INPUT,
    SECTION_ID_FILTER_OUTPUTS,
    SECTION_ID_FILTER_OUTPUT,
    SECTION_ID_HWFRAMESCONTEXT,
    SECTION_ID_INPUTFILES,
    SECTION_ID_INPUTFILE,
    SECTION_ID_INPUTSTREAMS,
    SECTION_ID_INPUTSTREAM,
    SECTION_ID_OUTPUTFILES,
    SECTION_ID_OUTPUTFILE,
    SECTION_ID_OUTPUTSTREAMS,
    SECTION_ID_OUTPUTSTREAM,
    SECTION_ID_STREAMLINKS,
    SECTION_ID_STREAMLINK,
    SECTION_ID_DECODERS,
    SECTION_ID_DECODER,
    SECTION_ID_ENCODERS,
    SECTION_ID_ENCODER,
} SectionID;

static const AVTextFormatSection sections[] = {
    [SECTION_ID_ROOT]            = { SECTION_ID_ROOT, "root", AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER, { SECTION_ID_FILTERGRAPHS, SECTION_ID_INPUTFILES, SECTION_ID_OUTPUTFILES, SECTION_ID_DECODERS, SECTION_ID_ENCODERS, SECTION_ID_STREAMLINKS, -1 } },

    [SECTION_ID_FILTERGRAPHS]    = { SECTION_ID_FILTERGRAPHS, "graphs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTERGRAPH, -1 } },
    [SECTION_ID_FILTERGRAPH]     = { SECTION_ID_FILTERGRAPH, "graph", AV_TEXTFORMAT_SECTION_FLAG_HAS_VARIABLE_FIELDS, { SECTION_ID_GRAPH_INPUTS, SECTION_ID_GRAPH_OUTPUTS, SECTION_ID_FILTERS, -1 }, .element_name = "graph_info" },

    [SECTION_ID_GRAPH_INPUTS]    = { SECTION_ID_GRAPH_INPUTS, "graph_inputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_GRAPH_INPUT, -1 }, .id_key = "id" },
    [SECTION_ID_GRAPH_INPUT]     = { SECTION_ID_GRAPH_INPUT, "graph_input", 0, { -1 }, .id_key = "filter_id" },

    [SECTION_ID_GRAPH_OUTPUTS]   = { SECTION_ID_GRAPH_OUTPUTS, "graph_outputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_GRAPH_OUTPUT, -1 }, .id_key = "id" },
    [SECTION_ID_GRAPH_OUTPUT]    = { SECTION_ID_GRAPH_OUTPUT, "graph_output", 0, { -1 }, .id_key = "filter_id" },

    [SECTION_ID_FILTERS]         = { SECTION_ID_FILTERS, "filters", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_FILTER, -1 }, .id_key = "graph_id" },
    [SECTION_ID_FILTER]          = { SECTION_ID_FILTER, "filter", AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS, { SECTION_ID_FILTER_INPUTS, SECTION_ID_FILTER_OUTPUTS, -1 }, .id_key = "filter_id" },

    [SECTION_ID_FILTER_INPUTS]   = { SECTION_ID_FILTER_INPUTS, "filter_inputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTER_INPUT, -1 } },
    [SECTION_ID_FILTER_INPUT]    = { SECTION_ID_FILTER_INPUT, "filter_input", AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS, { SECTION_ID_HWFRAMESCONTEXT, -1 }, .id_key = "filter_id", .src_id_key = "source_filter_id", .dest_id_key = "filter_id" },

    [SECTION_ID_FILTER_OUTPUTS]  = { SECTION_ID_FILTER_OUTPUTS, "filter_outputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTER_OUTPUT, -1 } },
    [SECTION_ID_FILTER_OUTPUT]   = { SECTION_ID_FILTER_OUTPUT, "filter_output", AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS, { SECTION_ID_HWFRAMESCONTEXT, -1 }, .id_key = "filter_id", .src_id_key = "filter_id", .dest_id_key = "dest_filter_id" },

    [SECTION_ID_HWFRAMESCONTEXT] = { SECTION_ID_HWFRAMESCONTEXT, "hw_frames_context",  0, { -1 }, },

    [SECTION_ID_INPUTFILES]      = { SECTION_ID_INPUTFILES, "inputfiles", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_INPUTFILE, -1 }, .id_key = "id" },
    [SECTION_ID_INPUTFILE]       = { SECTION_ID_INPUTFILE, "inputfile", AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_INPUTSTREAMS, -1 }, .id_key = "id" },

    [SECTION_ID_INPUTSTREAMS]    = { SECTION_ID_INPUTSTREAMS, "inputstreams", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_INPUTSTREAM, -1 }, .id_key = "id" },
    [SECTION_ID_INPUTSTREAM]     = { SECTION_ID_INPUTSTREAM, "inputstream", AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS, { -1 }, .id_key = "id" },

    [SECTION_ID_OUTPUTFILES]     = { SECTION_ID_OUTPUTFILES, "outputfiles", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_OUTPUTFILE, -1 }, .id_key = "id" },
    [SECTION_ID_OUTPUTFILE]      = { SECTION_ID_OUTPUTFILE, "outputfile", AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_OUTPUTSTREAMS, -1 }, .id_key = "id" },

    [SECTION_ID_OUTPUTSTREAMS]   = { SECTION_ID_OUTPUTSTREAMS, "outputstreams", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_OUTPUTSTREAM, -1 }, .id_key = "id" },
    [SECTION_ID_OUTPUTSTREAM]    = { SECTION_ID_OUTPUTSTREAM, "outputstream", AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS, { -1 }, .id_key = "id", },

    [SECTION_ID_STREAMLINKS]     = { SECTION_ID_STREAMLINKS, "streamlinks", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_STREAMLINK, -1 } },
    [SECTION_ID_STREAMLINK]      = { SECTION_ID_STREAMLINK, "streamlink", AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS, { -1 }, .src_id_key = "source_stream_id", .dest_id_key = "dest_stream_id" },

    [SECTION_ID_DECODERS]        = { SECTION_ID_DECODERS, "decoders", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_DECODER, -1 } },
    [SECTION_ID_DECODER]         = { SECTION_ID_DECODER, "decoder", AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS | AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS, { -1 }, .id_key = "id", .src_id_key = "source_id", .dest_id_key = "id" },

    [SECTION_ID_ENCODERS]        = { SECTION_ID_ENCODERS, "encoders", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY | AV_TEXTFORMAT_SECTION_FLAG_IS_SUBGRAPH, { SECTION_ID_ENCODER, -1 } },
    [SECTION_ID_ENCODER]         = { SECTION_ID_ENCODER, "encoder", AV_TEXTFORMAT_SECTION_FLAG_IS_SHAPE | AV_TEXTFORMAT_SECTION_PRINT_TAGS | AV_TEXTFORMAT_SECTION_FLAG_HAS_LINKS, { -1 }, .id_key = "id", .src_id_key = "id", .dest_id_key = "dest_id" },
};

typedef struct GraphPrintContext {
    AVTextFormatContext *tfc;
    AVTextWriterContext *wctx;
    AVDiagramConfig diagram_config;

    int id_prefix_num;
    int is_diagram;
    int opt_flags;
    int skip_buffer_filters;
    AVBPrint pbuf;

} GraphPrintContext;

/* Text Format API Shortcuts */
#define print_id(k, v)          print_sanizied_id(gpc, k, v, 0)
#define print_id_noprefix(k, v) print_sanizied_id(gpc, k, v, 1)
#define print_int(k, v)         avtext_print_integer(tfc, k, v, 0)
#define print_int_opt(k, v)     avtext_print_integer(tfc, k, v, gpc->opt_flags)
#define print_q(k, v, s)        avtext_print_rational(tfc, k, v, s)
#define print_str(k, v)         avtext_print_string(tfc, k, v, 0)
#define print_str_opt(k, v)     avtext_print_string(tfc, k, v, gpc->opt_flags)
#define print_val(k, v, u)      avtext_print_unit_integer(tfc, k, v, u)

#define print_fmt(k, f, ...) do {              \
    av_bprint_clear(&gpc->pbuf);                    \
    av_bprintf(&gpc->pbuf, f, __VA_ARGS__);         \
    avtext_print_string(tfc, k, gpc->pbuf.str, 0);    \
} while (0)

#define print_fmt_opt(k, f, ...) do {              \
    av_bprint_clear(&gpc->pbuf);                    \
    av_bprintf(&gpc->pbuf, f, __VA_ARGS__);         \
    avtext_print_string(tfc, k, gpc->pbuf.str, gpc->opt_flags);    \
} while (0)


static atomic_int prefix_num = 0;

static inline char *graph_upcase_string(char *dst, size_t dst_size, const char *src)
{
    unsigned i;
    for (i = 0; src[i] && i < dst_size - 1; i++)
        dst[i]      = (char)av_toupper(src[i]);
    dst[i] = 0;
    return dst;
}

static char *get_extension(const char *url)
{
    const char *dot = NULL;
    const char *sep = NULL;
    const char *end;

    if (!url)
        return NULL;

    /* Stop at the first query ('?') or fragment ('#') delimiter so they
     * are not considered part of the path. */
    end = strpbrk(url, "?#");
    if (!end)
        end = url + strlen(url);

    /* Scan the path component only. */
    for (const char *p = url; p < end; p++) {
        if (*p == '.')
            dot = p;
        else if (*p == '/' || *p == '\\')
            sep = p;
    }

    /* Validate that we have a proper extension. */
    if (dot && dot != url && (!sep || dot > sep + 1) && (dot + 1) < end) {
        /* Use FFmpeg helper to duplicate the substring. */
        return av_strndup(dot + 1, end - (dot + 1));
    }

    return NULL;
}

static void print_hwdevicecontext(const GraphPrintContext *gpc, const AVHWDeviceContext *hw_device_context)
{
    AVTextFormatContext *tfc = gpc->tfc;

    if (!hw_device_context)
        return;

    print_int_opt("has_hw_device_context", 1);
    print_str_opt("hw_device_type", av_hwdevice_get_type_name(hw_device_context->type));
}

static void print_hwframescontext(const GraphPrintContext *gpc, const AVHWFramesContext *hw_frames_context)
{
    AVTextFormatContext *tfc = gpc->tfc;
    const AVPixFmtDescriptor *pix_desc_hw;
    const AVPixFmtDescriptor *pix_desc_sw;

    if (!hw_frames_context || !hw_frames_context->device_ctx)
        return;

    avtext_print_section_header(tfc, NULL, SECTION_ID_HWFRAMESCONTEXT);

    print_int_opt("has_hw_frames_context", 1);
    print_str("hw_device_type", av_hwdevice_get_type_name(hw_frames_context->device_ctx->type));

    pix_desc_hw = av_pix_fmt_desc_get(hw_frames_context->format);
    if (pix_desc_hw) {
        print_str("hw_pixel_format", pix_desc_hw->name);
        if (pix_desc_hw->alias)
            print_str_opt("hw_pixel_format_alias", pix_desc_hw->alias);
    }

    pix_desc_sw = av_pix_fmt_desc_get(hw_frames_context->sw_format);
    if (pix_desc_sw) {
        print_str("sw_pixel_format", pix_desc_sw->name);
        if (pix_desc_sw->alias)
            print_str_opt("sw_pixel_format_alias", pix_desc_sw->alias);
    }

    print_int_opt("width", hw_frames_context->width);
    print_int_opt("height", hw_frames_context->height);
    print_int_opt("initial_pool_size", hw_frames_context->initial_pool_size);

    avtext_print_section_footer(tfc); // SECTION_ID_HWFRAMESCONTEXT
}

static void print_link(GraphPrintContext *gpc, AVFilterLink *link)
{
    AVTextFormatContext *tfc = gpc->tfc;
    AVBufferRef *hw_frames_ctx;
    char layout_string[64];

    if (!link)
        return;

    hw_frames_ctx = avfilter_link_get_hw_frames_ctx(link);

    print_str_opt("media_type", av_get_media_type_string(link->type));

    switch (link->type) {
    case AVMEDIA_TYPE_VIDEO:

        if (hw_frames_ctx && hw_frames_ctx->data) {
            AVHWFramesContext *      hwfctx      = (AVHWFramesContext *)hw_frames_ctx->data;
            const AVPixFmtDescriptor *pix_desc_hw = av_pix_fmt_desc_get(hwfctx->format);
            const AVPixFmtDescriptor *pix_desc_sw = av_pix_fmt_desc_get(hwfctx->sw_format);
            if (pix_desc_hw && pix_desc_sw)
                print_fmt("format", "%s | %s", pix_desc_hw->name, pix_desc_sw->name);
        } else {
            print_str("format", av_x_if_null(av_get_pix_fmt_name(link->format), "?"));
        }

        if (link->w && link->h) {
            if (tfc->opts.show_value_unit) {
                print_fmt("size", "%dx%d", link->w, link->h);
            } else {
                print_int("width", link->w);
                print_int("height", link->h);
            }
        }

        print_q("sar", link->sample_aspect_ratio, ':');

        if (link->color_range != AVCOL_RANGE_UNSPECIFIED)
            print_str_opt("color_range", av_color_range_name(link->color_range));

        if (link->colorspace != AVCOL_SPC_UNSPECIFIED)
            print_str("color_space", av_color_space_name(link->colorspace));
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        ////print_str("format", av_x_if_null(av_get_subtitle_fmt_name(link->format), "?"));

        if (link->w && link->h) {
            if (tfc->opts.show_value_unit) {
                print_fmt("size", "%dx%d", link->w, link->h);
            } else {
                print_int("width", link->w);
                print_int("height", link->h);
            }
        }

        break;

    case AVMEDIA_TYPE_AUDIO:
        av_channel_layout_describe(&link->ch_layout, layout_string, sizeof(layout_string));
        print_str("channel_layout", layout_string);
        print_val("channels", link->ch_layout.nb_channels, "ch");
        if (tfc->opts.show_value_unit)
            print_fmt("sample_rate", "%d.1 kHz", link->sample_rate / 1000);
        else
            print_val("sample_rate", link->sample_rate, "Hz");

        break;
    }

    print_fmt_opt("sample_rate", "%d/%d", link->time_base.num, link->time_base.den);

    if (hw_frames_ctx && hw_frames_ctx->data)
        print_hwframescontext(gpc, (AVHWFramesContext *)hw_frames_ctx->data);
    av_buffer_unref(&hw_frames_ctx);
}

static char sanitize_char(const char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return c;
    return '_';
}

static void print_sanizied_id(const GraphPrintContext *gpc, const char *key, const char *id_str, int skip_prefix)
{
    AVTextFormatContext *tfc = gpc->tfc;
    AVBPrint buf;

    if (!key || !id_str)
        return;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (!skip_prefix)
        av_bprintf(&buf, "G%d_", gpc->id_prefix_num);

    // sanizize section id
    for (const char *p = id_str; *p; p++)
        av_bprint_chars(&buf, sanitize_char(*p), 1);

    print_str(key, buf.str);

    av_bprint_finalize(&buf, NULL);
}

static void print_section_header_id(const GraphPrintContext *gpc, int section_id, const char *id_str, int skip_prefix)
{
    AVTextFormatContext *tfc = gpc->tfc;
    AVTextFormatSectionContext sec_ctx = { 0 };
    AVBPrint buf;

    if (!id_str)
        return;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (!skip_prefix)
        av_bprintf(&buf, "G%d_", gpc->id_prefix_num);

    // sanizize section id
    for (const char *p = id_str; *p; p++)
        av_bprint_chars(&buf, sanitize_char(*p), 1);

    sec_ctx.context_id = buf.str;

    avtext_print_section_header(tfc, &sec_ctx, section_id);

    av_bprint_finalize(&buf, NULL);
}

static const char *get_filterpad_name(const AVFilterPad *pad)
{
    return pad ? avfilter_pad_get_name(pad, 0) : "pad";
}

static void print_filter(GraphPrintContext *gpc, const AVFilterContext *filter, AVDictionary *input_map, AVDictionary *output_map)
{
    AVTextFormatContext *tfc = gpc->tfc;
    AVTextFormatSectionContext sec_ctx = { 0 };

    print_section_header_id(gpc, SECTION_ID_FILTER, filter->name, 0);

    ////print_id("filter_id", filter->name);

    if (filter->filter) {
        print_str("filter_name", filter->filter->name);
        print_str_opt("description", filter->filter->description);
        print_int_opt("nb_inputs", filter->nb_inputs);
        print_int_opt("nb_outputs", filter->nb_outputs);
    }

    if (filter->hw_device_ctx) {
        AVHWDeviceContext *device_context = (AVHWDeviceContext *)filter->hw_device_ctx->data;
        print_hwdevicecontext(gpc, device_context);
        if (filter->extra_hw_frames > 0)
            print_int("extra_hw_frames", filter->extra_hw_frames);
    }

    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTER_INPUTS);

    for (unsigned i = 0; i < filter->nb_inputs; i++) {
        AVDictionaryEntry *dic_entry;
        AVFilterLink *link = filter->inputs[i];

        sec_ctx.context_type = av_get_media_type_string(link->type);
        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_FILTER_INPUT);
        sec_ctx.context_type = NULL;

        print_int_opt("input_index", i);
        print_str_opt("pad_name", get_filterpad_name(link->dstpad));;

        dic_entry = av_dict_get(input_map, link->src->name, NULL, 0);
        if (dic_entry) {
            char buf[256];
            (void)snprintf(buf, sizeof(buf), "in_%s", dic_entry->value);
            print_id_noprefix("source_filter_id", buf);
        } else {
            print_id("source_filter_id", link->src->name);
        }

        print_str_opt("source_pad_name", get_filterpad_name(link->srcpad));
        print_id("filter_id", filter->name);

        print_link(gpc, link);

        avtext_print_section_footer(tfc); // SECTION_ID_FILTER_INPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_FILTER_INPUTS

    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTER_OUTPUTS);

    for (unsigned i = 0; i < filter->nb_outputs; i++) {
        AVDictionaryEntry *dic_entry;
        AVFilterLink *link = filter->outputs[i];
        char buf[256];

        sec_ctx.context_type = av_get_media_type_string(link->type);
        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_FILTER_OUTPUT);
        sec_ctx.context_type = NULL;

        dic_entry = av_dict_get(output_map, link->dst->name, NULL, 0);
        if (dic_entry) {
            (void)snprintf(buf, sizeof(buf), "out_%s", dic_entry->value);
            print_id_noprefix("dest_filter_id", buf);
        } else {
            print_id("dest_filter_id", link->dst->name);
        }

        print_int_opt("output_index", i);
        print_str_opt("pad_name", get_filterpad_name(link->srcpad));
        ////print_id("dest_filter_id", link->dst->name);
        print_str_opt("dest_pad_name", get_filterpad_name(link->dstpad));
        print_id("filter_id", filter->name);

        print_link(gpc, link);

        avtext_print_section_footer(tfc); // SECTION_ID_FILTER_OUTPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_FILTER_OUTPUTS

    avtext_print_section_footer(tfc); // SECTION_ID_FILTER
}

static void print_filtergraph_single(GraphPrintContext *gpc, FilterGraph *fg, AVFilterGraph *graph)
{
    AVTextFormatContext *tfc = gpc->tfc;
    AVDictionary *input_map = NULL;
    AVDictionary *output_map = NULL;

    print_int("graph_index", fg->index);
    print_fmt("name", "Graph %d.%d", gpc->id_prefix_num, fg->index);
    print_fmt("id", "Graph_%d_%d", gpc->id_prefix_num, fg->index);
    print_str("description", fg->graph_desc);

    print_section_header_id(gpc, SECTION_ID_GRAPH_INPUTS, "Input_File", 0);

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilter *ifilter = fg->inputs[i];
        enum AVMediaType media_type = ifilter->type;

        avtext_print_section_header(tfc, NULL, SECTION_ID_GRAPH_INPUT);

        print_int("input_index", ifilter->index);

        if (ifilter->linklabel)
            print_str("link_label", (const char*)ifilter->linklabel);

        if (ifilter->filter) {
            print_id("filter_id", ifilter->filter->name);
            print_str("filter_name", ifilter->filter->filter->name);
        }

        if (ifilter->linklabel && ifilter->filter)
            av_dict_set(&input_map, ifilter->filter->name, (const char *)ifilter->linklabel, 0);
        else if (ifilter->input_name && ifilter->filter)
            av_dict_set(&input_map, ifilter->filter->name, (const char *)ifilter->input_name, 0);

        print_str("media_type", av_get_media_type_string(media_type));

        avtext_print_section_footer(tfc); // SECTION_ID_GRAPH_INPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_GRAPH_INPUTS

    print_section_header_id(gpc, SECTION_ID_GRAPH_OUTPUTS, "Output_File", 0);

    for (int i = 0; i < fg->nb_outputs; i++) {
        OutputFilter *ofilter = fg->outputs[i];

        avtext_print_section_header(tfc, NULL, SECTION_ID_GRAPH_OUTPUT);

        print_int("output_index", ofilter->index);

        print_str("name", ofilter->output_name);

        if (fg->outputs[i]->linklabel)
            print_str("link_label", (const char*)fg->outputs[i]->linklabel);

        if (ofilter->filter) {
            print_id("filter_id", ofilter->filter->name);
            print_str("filter_name", ofilter->filter->filter->name);
        }

        if (ofilter->output_name && ofilter->filter)
            av_dict_set(&output_map, ofilter->filter->name, ofilter->output_name, 0);


        print_str("media_type", av_get_media_type_string(ofilter->type));

        avtext_print_section_footer(tfc); // SECTION_ID_GRAPH_OUTPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_GRAPH_OUTPUTS

    if (graph) {
        AVTextFormatSectionContext sec_ctx = { 0 };

        sec_ctx.context_id = av_asprintf("Graph_%d_%d", gpc->id_prefix_num, fg->index);

        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_FILTERS);

        if (gpc->is_diagram) {
            print_fmt("name", "Graph %d.%d", gpc->id_prefix_num, fg->index);
            print_str("description", fg->graph_desc);
            print_str("id", sec_ctx.context_id);
        }

        av_freep(&sec_ctx.context_id);

        for (unsigned i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *filter = graph->filters[i];

            if (gpc->skip_buffer_filters) {
                if (av_dict_get(input_map, filter->name, NULL, 0))
                    continue;
                if (av_dict_get(output_map, filter->name, NULL, 0))
                    continue;
            }

            sec_ctx.context_id = filter->name;

            print_filter(gpc, filter, input_map, output_map);
        }

        avtext_print_section_footer(tfc); // SECTION_ID_FILTERS
    }

    // Clean up dictionaries
    av_dict_free(&input_map);
    av_dict_free(&output_map);
}

static int print_streams(GraphPrintContext *gpc, InputFile **ifiles, int nb_ifiles, OutputFile **ofiles, int nb_ofiles)
{
    AVTextFormatContext       *tfc = gpc->tfc;
    AVBPrint                   buf;
    AVTextFormatSectionContext sec_ctx = { 0 };

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    print_section_header_id(gpc, SECTION_ID_INPUTFILES, "Inputs", 0);

    for (int n = nb_ifiles - 1; n >= 0; n--) {
        InputFile *ifi = ifiles[n];
        AVFormatContext *fc = ifi->ctx;

        sec_ctx.context_id = av_asprintf("Input_%d", n);
        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_INPUTFILE);
        av_freep(&sec_ctx.context_id);

        print_fmt("index", "%d", ifi->index);

        if (fc) {
            print_str("demuxer_name", fc->iformat->name);
            if (fc->url) {
                char *extension = get_extension(fc->url);
                if (extension) {
                    print_str("file_extension", extension);
                    av_freep(&extension);
                }
                print_str("url", fc->url);
            }
        }

        sec_ctx.context_id = av_asprintf("InputStreams_%d", n);

        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_INPUTSTREAMS);

        av_freep(&sec_ctx.context_id);

        for (int i = 0; i < ifi->nb_streams; i++) {
            InputStream *ist = ifi->streams[i];
            const AVCodecDescriptor *codec_desc;

            if (!ist || !ist->par)
                continue;

            codec_desc = avcodec_descriptor_get(ist->par->codec_id);

            sec_ctx.context_id = av_asprintf("r_in_%d_%d", n, i);

            sec_ctx.context_type = av_get_media_type_string(ist->par->codec_type);

            avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_INPUTSTREAM);
            av_freep(&sec_ctx.context_id);
            sec_ctx.context_type = NULL;

            av_bprint_clear(&buf);

            print_fmt("id", "r_in_%d_%d", n, i);

            if (codec_desc && codec_desc->name) {
                ////av_bprintf(&buf, "%s", graph_upcase_string(char_buf, sizeof(char_buf), codec_desc->long_name));
                av_bprintf(&buf, "%s", codec_desc->long_name);
            } else if (ist->dec) {
                char char_buf[256];
                av_bprintf(&buf, "%s", graph_upcase_string(char_buf, sizeof(char_buf), ist->dec->name));
            } else if (ist->par->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
                av_bprintf(&buf, "%s", "Attachment");
            } else if (ist->par->codec_type == AVMEDIA_TYPE_DATA) {
                av_bprintf(&buf, "%s", "Data");
            }

            print_fmt("name", "%s", buf.str);
            print_fmt("index", "%d", ist->index);

            if (ist->dec)
                print_str_opt("media_type", av_get_media_type_string(ist->par->codec_type));

            avtext_print_section_footer(tfc); // SECTION_ID_INPUTSTREAM
        }

        avtext_print_section_footer(tfc); // SECTION_ID_INPUTSTREAMS
        avtext_print_section_footer(tfc); // SECTION_ID_INPUTFILE
    }

    avtext_print_section_footer(tfc); // SECTION_ID_INPUTFILES


    print_section_header_id(gpc, SECTION_ID_DECODERS, "Decoders", 0);

    for (int n = 0; n < nb_ifiles; n++) {
        InputFile *ifi = ifiles[n];

        for (int i = 0; i < ifi->nb_streams; i++) {
            InputStream *ist = ifi->streams[i];

            if (!ist->decoder)
                continue;

            sec_ctx.context_id = av_asprintf("in_%d_%d", n, i);
            sec_ctx.context_type = av_get_media_type_string(ist->par->codec_type);
            sec_ctx.context_flags = 2;

            avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_DECODER);
            av_freep(&sec_ctx.context_id);
            sec_ctx.context_type = NULL;
            sec_ctx.context_flags = 0;

            av_bprint_clear(&buf);

            print_fmt("source_id", "r_in_%d_%d", n, i);
            print_fmt("id", "in_%d_%d", n, i);

            ////av_bprintf(&buf, "%s", graph_upcase_string(char_buf, sizeof(char_buf), ist->dec->name));
            print_fmt("name", "%s", ist->dec->name);

            print_str_opt("media_type", av_get_media_type_string(ist->par->codec_type));

            avtext_print_section_footer(tfc); // SECTION_ID_DECODER
        }
    }

    avtext_print_section_footer(tfc); // SECTION_ID_DECODERS


    print_section_header_id(gpc, SECTION_ID_ENCODERS, "Encoders", 0);

    for (int n = 0; n < nb_ofiles; n++) {
        OutputFile *of = ofiles[n];

        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            ////const AVCodecDescriptor *codec_desc;

            if (!ost || !ost->st || !ost->st->codecpar || !ost->enc)
                continue;

            ////codec_desc = avcodec_descriptor_get(ost->st->codecpar->codec_id);

            sec_ctx.context_id = av_asprintf("out__%d_%d", n, i);
            sec_ctx.context_type = av_get_media_type_string(ost->type);
            sec_ctx.context_flags = 2;

            avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_ENCODER);
            av_freep(&sec_ctx.context_id);
            sec_ctx.context_type = NULL;
            sec_ctx.context_flags = 0;

            av_bprint_clear(&buf);

            print_fmt("id", "out__%d_%d", n, i);
            print_fmt("dest_id", "r_out__%d_%d", n, i);

            print_fmt("name", "%s", ost->enc->enc_ctx->av_class->item_name(ost->enc->enc_ctx));

            print_str_opt("media_type", av_get_media_type_string(ost->type));

            avtext_print_section_footer(tfc); // SECTION_ID_ENCODER
        }
    }

    avtext_print_section_footer(tfc); // SECTION_ID_ENCODERS


    print_section_header_id(gpc, SECTION_ID_OUTPUTFILES, "Outputs", 0);

    for (int n = nb_ofiles - 1; n >= 0; n--) {
        OutputFile *of = ofiles[n];
        Muxer *muxer = (Muxer *)of;

        if (!muxer->fc)
            continue;

        sec_ctx.context_id = av_asprintf("Output_%d", n);

        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_OUTPUTFILE);

        av_freep(&sec_ctx.context_id);

        ////print_str_opt("index", av_get_media_type_string(of->index));
        print_fmt("index", "%d", of->index);
        ////print_str("url", of->url);
        print_str("muxer_name", muxer->fc->oformat->name);
        if (of->url) {
            char *extension = get_extension(of->url);
            if (extension) {
                print_str("file_extension", extension);
                av_freep(&extension);
            }
            print_str("url", of->url);
        }

        sec_ctx.context_id = av_asprintf("OutputStreams_%d", n);

        avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_OUTPUTSTREAMS);

        av_freep(&sec_ctx.context_id);

        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            const AVCodecDescriptor *codec_desc = avcodec_descriptor_get(ost->st->codecpar->codec_id);

            sec_ctx.context_id = av_asprintf("r_out__%d_%d", n, i);
            sec_ctx.context_type = av_get_media_type_string(ost->type);
            avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_OUTPUTSTREAM);
            av_freep(&sec_ctx.context_id);
            sec_ctx.context_type = NULL;

            av_bprint_clear(&buf);

            print_fmt("id", "r_out__%d_%d", n, i);

            if (codec_desc && codec_desc->name) {
                av_bprintf(&buf, "%s", codec_desc->long_name);
            } else {
                av_bprintf(&buf, "%s", "unknown");
            }

            print_fmt("name", "%s", buf.str);
            print_fmt("index", "%d", ost->index);

            print_str_opt("media_type", av_get_media_type_string(ost->type));

            avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTSTREAM
        }

        avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTSTREAMS
        avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTFILE
    }

    avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTFILES


    avtext_print_section_header(tfc, NULL, SECTION_ID_STREAMLINKS);

    for (int n = 0; n < nb_ofiles; n++) {
        OutputFile *of = ofiles[n];

        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];

            if (ost->ist && !ost->filter) {
                sec_ctx.context_type = av_get_media_type_string(ost->type);
                avtext_print_section_header(tfc, &sec_ctx, SECTION_ID_STREAMLINK);
                sec_ctx.context_type = NULL;

                if (ost->enc) {
                    print_fmt("dest_stream_id", "out__%d_%d", n, i);
                    print_fmt("source_stream_id", "in_%d_%d", ost->ist->file->index, ost->ist->index);
                    print_str("operation", "Transcode");
                } else {
                    print_fmt("dest_stream_id", "r_out__%d_%d", n, i);
                    print_fmt("source_stream_id", "r_in_%d_%d", ost->ist->file->index, ost->ist->index);
                    print_str("operation", "Stream Copy");
                }

                print_str_opt("media_type", av_get_media_type_string(ost->type));

                avtext_print_section_footer(tfc); // SECTION_ID_STREAMLINK
            }
        }
    }

    avtext_print_section_footer(tfc); // SECTION_ID_STREAMLINKS

    av_bprint_finalize(&buf, NULL);
    return 0;
}


static void uninit_graphprint(GraphPrintContext *gpc)
{
    if (gpc->tfc)
        avtext_context_close(&gpc->tfc);

    if (gpc->wctx)
        avtextwriter_context_close(&gpc->wctx);

    // Finalize the print buffer if it was initialized
    av_bprint_finalize(&gpc->pbuf, NULL);

    av_freep(&gpc);
}

static int init_graphprint(GraphPrintContext **pgpc, AVBPrint *target_buf)
{
    const AVTextFormatter *text_formatter;
    AVTextFormatContext *tfc = NULL;
    AVTextWriterContext *wctx = NULL;
    GraphPrintContext *gpc = NULL;
    int ret;

    *pgpc = NULL;

    av_bprint_init(target_buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    const char *w_name = print_graphs_format ? print_graphs_format : "json";

    text_formatter = avtext_get_formatter_by_name(w_name);
    if (!text_formatter) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph output format with name '%s'\n", w_name);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = avtextwriter_create_buffer(&wctx, target_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "avtextwriter_create_buffer failed. Error code %d\n", ret);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    AVTextFormatOptions tf_options = { .show_optional_fields = -1 };
    const char *w_args = print_graphs_format ? strchr(print_graphs_format, '=') : NULL;
    if (w_args)
        ++w_args; // consume '='
    ret = avtext_context_open(&tfc, text_formatter, wctx, w_args, sections, FF_ARRAY_ELEMS(sections), tf_options, NULL);
    if (ret < 0) {
        goto fail;
    }

    gpc = av_mallocz(sizeof(GraphPrintContext));
    if (!gpc) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    gpc->wctx = wctx;
    gpc->tfc = tfc;
    av_bprint_init(&gpc->pbuf, 0, AV_BPRINT_SIZE_UNLIMITED);

    gpc->id_prefix_num = atomic_fetch_add(&prefix_num, 1);
    gpc->is_diagram = !!(tfc->formatter->flags & AV_TEXTFORMAT_FLAG_IS_DIAGRAM_FORMATTER);
    if (gpc->is_diagram) {
        tfc->opts.show_value_unit = 1;
        tfc->opts.show_optional_fields = -1;
        gpc->opt_flags = AV_TEXTFORMAT_PRINT_STRING_OPTIONAL;
        gpc->skip_buffer_filters = 1;
        ////} else {
        ////    gpc->opt_flags = AV_TEXTFORMAT_PRINT_STRING_OPTIONAL;
    }

    if (!strcmp(text_formatter->name, "mermaid") || !strcmp(text_formatter->name, "mermaidhtml")) {
        gpc->diagram_config.diagram_css = ff_resman_get_string(FF_RESOURCE_GRAPH_CSS);

        if (!strcmp(text_formatter->name, "mermaidhtml"))
            gpc->diagram_config.html_template = ff_resman_get_string(FF_RESOURCE_GRAPH_HTML);

        av_diagram_init(tfc, &gpc->diagram_config);
    }

    *pgpc = gpc;

    return 0;

fail:
    if (tfc)
        avtext_context_close(&tfc);
    if (wctx && !tfc) // Only free wctx if tfc didn't take ownership of it
        avtextwriter_context_close(&wctx);
    av_freep(&gpc);

    return ret;
}


int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph)
{
    av_assert2(fg);

    GraphPrintContext *gpc = NULL;
    AVTextFormatContext *tfc;
    AVBPrint *target_buf = &fg->graph_print_buf;
    int ret;

    if (target_buf->len)
        av_bprint_finalize(target_buf, NULL);

    ret = init_graphprint(&gpc, target_buf);
    if (ret)
        return ret;

    tfc = gpc->tfc;

    // Due to the threading model each graph needs to print itself into a buffer
    // from its own thread. The actual printing happens short before cleanup in ffmpeg.c
    // where all graphs are assembled together. To make this work, we need to put the
    // formatting context into the same state like it would be when printing all at once,
    // so here we print the section headers and clear the buffer to get into the right state.
    avtext_print_section_header(tfc, NULL, SECTION_ID_ROOT);
    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERGRAPHS);
    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERGRAPH);

    av_bprint_clear(target_buf);

    print_filtergraph_single(gpc, fg, graph);

    if (gpc->is_diagram) {
        avtext_print_section_footer(tfc); // SECTION_ID_FILTERGRAPH
        avtext_print_section_footer(tfc); // SECTION_ID_FILTERGRAPHS
    }

    uninit_graphprint(gpc);

    return 0;
}

static int print_filtergraphs_priv(FilterGraph **graphs, int nb_graphs, InputFile **ifiles, int nb_ifiles, OutputFile **ofiles, int nb_ofiles)
{
    GraphPrintContext *gpc = NULL;
    AVTextFormatContext *tfc;
    AVBPrint target_buf;
    int ret;

    ret = init_graphprint(&gpc, &target_buf);
    if (ret)
        goto cleanup;

    tfc = gpc->tfc;

    avtext_print_section_header(tfc, NULL, SECTION_ID_ROOT);
    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERGRAPHS);

    for (int i = 0; i < nb_graphs; i++) {
        AVBPrint *graph_buf = &graphs[i]->graph_print_buf;

        if (graph_buf->len > 0) {
            avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERGRAPH);
            av_bprint_append_data(&target_buf, graph_buf->str, graph_buf->len);
            av_bprint_finalize(graph_buf, NULL);
            avtext_print_section_footer(tfc); // SECTION_ID_FILTERGRAPH
        }
    }

    for (int n = 0; n < nb_ofiles; n++) {
        OutputFile *of = ofiles[n];

        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];

            if (ost->fg_simple) {
                AVBPrint *graph_buf = &ost->fg_simple->graph_print_buf;

                if (graph_buf->len > 0) {
                    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERGRAPH);
                    av_bprint_append_data(&target_buf, graph_buf->str, graph_buf->len);
                    av_bprint_finalize(graph_buf, NULL);
                    avtext_print_section_footer(tfc); // SECTION_ID_FILTERGRAPH
                }
            }
        }
    }

    avtext_print_section_footer(tfc); // SECTION_ID_FILTERGRAPHS

    print_streams(gpc, ifiles, nb_ifiles, ofiles, nb_ofiles);

    avtext_print_section_footer(tfc); // SECTION_ID_ROOT

    if (print_graphs_file) {
        AVIOContext *avio = NULL;

        if (!strcmp(print_graphs_file, "-")) {
            printf("%s", target_buf.str);
        } else {
            ret = avio_open2(&avio, print_graphs_file, AVIO_FLAG_WRITE, NULL, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open graph output file, \"%s\": %s\n", print_graphs_file, av_err2str(ret));
                goto cleanup;
            }

            avio_write(avio, (const unsigned char *)target_buf.str, FFMIN(target_buf.len, target_buf.size - 1));

            if ((ret = avio_closep(&avio)) < 0)
                av_log(NULL, AV_LOG_ERROR, "Error closing graph output file, loss of information possible: %s\n", av_err2str(ret));
        }
    }

    if (print_graphs)
        av_log(NULL, AV_LOG_INFO, "%s    %c", target_buf.str, '\n');

cleanup:
    // Properly clean up resources
    if (gpc)
        uninit_graphprint(gpc);

    // Ensure the target buffer is properly finalized
    av_bprint_finalize(&target_buf, NULL);

    return ret;
}

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, InputFile **ifiles, int nb_ifiles, OutputFile **ofiles, int nb_ofiles)
{
    int ret = print_filtergraphs_priv(graphs, nb_graphs, ifiles, nb_ifiles, ofiles, nb_ofiles);
    ff_resman_uninit();
    return ret;
}


/* ========== fftools/resources/resman.c ========== */

/*
 * Copyright (c) 2025 - softworkz
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * output writers for filtergraph details
 */

#include "config.h"

#include <string.h>

#if CONFIG_RESOURCE_COMPRESSION
#include <zlib.h>
#endif


#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/common.h"

extern const unsigned char ff_graph_html_data[];
extern const unsigned int ff_graph_html_len;

extern const unsigned char ff_graph_css_data[];
extern const unsigned ff_graph_css_len;

static const FFResourceDefinition resource_definitions[] = {
    [FF_RESOURCE_GRAPH_CSS]   = { FF_RESOURCE_GRAPH_CSS,   "graph.css",   &ff_graph_css_data[0],   &ff_graph_css_len   },
    [FF_RESOURCE_GRAPH_HTML]  = { FF_RESOURCE_GRAPH_HTML,  "graph.html",  &ff_graph_html_data[0],  &ff_graph_html_len  },
};


static const AVClass resman_class = {
    .class_name = "ResourceManager",
};

typedef struct ResourceManagerContext {
    const AVClass *class;
    AVDictionary *resource_dic;
} ResourceManagerContext;

static AVMutex mutex = AV_MUTEX_INITIALIZER;

static ResourceManagerContext resman_ctx = { .class = &resman_class };


#if CONFIG_RESOURCE_COMPRESSION

static int decompress_gzip(ResourceManagerContext *ctx, uint8_t *in, unsigned in_len, char **out, size_t *out_len)
{
    z_stream strm;
    unsigned chunk = 65534;
    int ret;
    uint8_t *buf;

    *out = NULL;
    memset(&strm, 0, sizeof(strm));

    // Allocate output buffer with extra byte for null termination
    buf = (uint8_t *)av_mallocz(chunk + 1);
    if (!buf) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate decompression buffer\n");
        return AVERROR(ENOMEM);
    }

    // 15 + 16 tells zlib to detect GZIP or zlib automatically
    ret = inflateInit2(&strm, 15 + 16);
    if (ret != Z_OK) {
        av_log(ctx, AV_LOG_ERROR, "Error during zlib initialization: %s\n", strm.msg);
        av_free(buf);
        return AVERROR(ENOSYS);
    }

    strm.avail_in  = in_len;
    strm.next_in   = in;
    strm.avail_out = chunk;
    strm.next_out  = buf;

    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
        av_log(ctx, AV_LOG_ERROR, "Inflate failed: %d, %s\n", ret, strm.msg);
        inflateEnd(&strm);
        av_free(buf);
        return (ret == Z_STREAM_END) ? Z_OK : ((ret == Z_OK) ? Z_BUF_ERROR : ret);
    }

    if (strm.avail_out == 0) {
        // TODO: Error or loop decoding?
        av_log(ctx, AV_LOG_WARNING, "Decompression buffer may be too small\n");
    }

    *out_len = chunk - strm.avail_out;
    buf[*out_len] = 0; // Ensure null termination

    inflateEnd(&strm);
    *out = (char *)buf;
    return Z_OK;
}
#endif

void ff_resman_uninit(void)
{
    ff_mutex_lock(&mutex);

    av_dict_free(&resman_ctx.resource_dic);

    ff_mutex_unlock(&mutex);
}


char *ff_resman_get_string(FFResourceId resource_id)
{
    ResourceManagerContext *ctx = &resman_ctx;
    FFResourceDefinition resource_definition = { 0 };
    AVDictionaryEntry *dic_entry;
    char *res = NULL;

    for (unsigned i = 0; i < FF_ARRAY_ELEMS(resource_definitions); ++i) {
        FFResourceDefinition def = resource_definitions[i];
        if (def.resource_id == resource_id) {
            resource_definition = def;
            break;
        }
    }

    av_assert1(resource_definition.name);

    ff_mutex_lock(&mutex);

    dic_entry = av_dict_get(ctx->resource_dic, resource_definition.name, NULL, 0);

    if (!dic_entry) {
        int dict_ret;

#if CONFIG_RESOURCE_COMPRESSION

        char *out = NULL;
        size_t out_len;

        int ret = decompress_gzip(ctx, (uint8_t *)resource_definition.data, *resource_definition.data_len, &out, &out_len);

        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "Unable to decompress the resource with ID %d\n", resource_id);
            goto end;
        }

        dict_ret = av_dict_set(&ctx->resource_dic, resource_definition.name, out, 0);
        if (dict_ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to store decompressed resource in dictionary: %d\n", dict_ret);
            av_freep(&out);
            goto end;
        }

        av_freep(&out);
#else

        dict_ret = av_dict_set(&ctx->resource_dic, resource_definition.name, (const char *)resource_definition.data, 0);
        if (dict_ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to store resource in dictionary: %d\n", dict_ret);
            goto end;
        }

#endif
        dic_entry = av_dict_get(ctx->resource_dic, resource_definition.name, NULL, 0);

        if (!dic_entry) {
            av_log(ctx, AV_LOG_ERROR, "Failed to retrieve resource from dictionary after storing it\n");
            goto end;
        }
    }

    res = dic_entry->value;

end:
    ff_mutex_unlock(&mutex);
    return res;
}


/* ========== fftools/resources/graph.html.c ========== */

const unsigned char ff_graph_html_data[] = { 0x3c, 0x21, 0x44, 0x4f, 0x43, 0x54, 0x59, 0x50, 0x45, 0x20, 0x68, 0x74, 0x6d, 0x6c, 0x3e, 0x0a, 0x3c, 0x68, 0x74, 0x6d, 0x6c, 0x20, 0x6c, 0x61, 0x6e, 0x67, 0x3d, 0x22, 0x65, 0x6e, 0x22, 0x3e, 0x0a, 0x3c, 0x68, 0x65, 0x61, 0x64, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x3c, 0x6d, 0x65, 0x74, 0x61, 0x20, 0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x22, 0x55, 0x54, 0x46, 0x2d, 0x38, 0x22, 0x2f, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x3c, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x3e, 0x46, 0x46, 0x6d, 0x70, 0x65, 0x67, 0x20, 0x47, 0x72, 0x61, 0x70, 0x68, 0x3c, 0x2f, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x3e, 0x0a, 0x3c, 0x2f, 0x68, 0x65, 0x61, 0x64, 0x3e, 0x0a, 0x3c, 0x62, 0x6f, 0x64, 0x79, 0x3e, 0x0a, 0x3c, 0x73, 0x74, 0x79, 0x6c, 0x65, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x68, 0x74, 0x6d, 0x6c, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x36, 0x36, 0x36, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x66, 0x61, 0x6d, 0x69, 0x6c, 0x79, 0x3a, 0x20, 0x52, 0x6f, 0x62, 0x6f, 0x74, 0x6f, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x30, 0x30, 0x25, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x64, 0x79, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x66, 0x39, 0x66, 0x39, 0x66, 0x39, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x78, 0x2d, 0x73, 0x69, 0x7a, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x2d, 0x62, 0x6f, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x2d, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x63, 0x6f, 0x6c, 0x75, 0x6d, 0x6e, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x30, 0x30, 0x25, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x31, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x20, 0x31, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x20, 0x33, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x20, 0x31, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x76, 0x23, 0x62, 0x61, 0x6e, 0x6e, 0x65, 0x72, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x2d, 0x69, 0x74, 0x65, 0x6d, 0x73, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x2d, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x72, 0x6f, 0x77, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x62, 0x6f, 0x74, 0x74, 0x6f, 0x6d, 0x3a, 0x20, 0x31, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x36, 0x76, 0x77, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x76, 0x23, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x61, 0x73, 0x70, 0x65, 0x63, 0x74, 0x2d, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x3a, 0x20, 0x31, 0x2f, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x69, 0x6d, 0x61, 0x67, 0x65, 0x3a, 0x20, 0x75, 0x72, 0x6c, 0x28, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f, 0x74, 0x72, 0x61, 0x63, 0x2e, 0x66, 0x66, 0x6d, 0x70, 0x65, 0x67, 0x2e, 0x6f, 0x72, 0x67, 0x2f, 0x66, 0x66, 0x6d, 0x70, 0x65, 0x67, 0x2d, 0x6c, 0x6f, 0x67, 0x6f, 0x2e, 0x70, 0x6e, 0x67, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x63, 0x6f, 0x76, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x2e, 0x36, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x68, 0x31, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x72, 0x65, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x2d, 0x69, 0x74, 0x65, 0x6d, 0x73, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x78, 0x2d, 0x73, 0x68, 0x61, 0x64, 0x6f, 0x77, 0x3a, 0x20, 0x32, 0x70, 0x78, 0x20, 0x32, 0x70, 0x78, 0x20, 0x32, 0x35, 0x70, 0x78, 0x20, 0x30, 0x70, 0x78, 0x20, 0x23, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3a, 0x20, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6a, 0x75, 0x73, 0x74, 0x69, 0x66, 0x79, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x6c, 0x61, 0x79, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x72, 0x65, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x73, 0x76, 0x67, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x61, 0x75, 0x74, 0x6f, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x75, 0x6e, 0x73, 0x65, 0x74, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x61, 0x75, 0x74, 0x6f, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x72, 0x65, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x73, 0x76, 0x67, 0x20, 0x2a, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x75, 0x73, 0x65, 0x72, 0x2d, 0x73, 0x65, 0x6c, 0x65, 0x63, 0x74, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x3c, 0x2f, 0x73, 0x74, 0x79, 0x6c, 0x65, 0x3e, 0x0a, 0x3c, 0x64, 0x69, 0x76, 0x20, 0x69, 0x64, 0x3d, 0x22, 0x62, 0x61, 0x6e, 0x6e, 0x65, 0x72, 0x22, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x3c, 0x64, 0x69, 0x76, 0x20, 0x69, 0x64, 0x3d, 0x22, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72, 0x22, 0x3e, 0x3c, 0x2f, 0x64, 0x69, 0x76, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x3c, 0x68, 0x31, 0x3e, 0x46, 0x46, 0x6d, 0x70, 0x65, 0x67, 0x20, 0x45, 0x78, 0x65, 0x63, 0x75, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x47, 0x72, 0x61, 0x70, 0x68, 0x3c, 0x2f, 0x68, 0x31, 0x3e, 0x0a, 0x3c, 0x2f, 0x64, 0x69, 0x76, 0x3e, 0x0a, 0x3c, 0x70, 0x72, 0x65, 0x20, 0x63, 0x6c, 0x61, 0x73, 0x73, 0x3d, 0x22, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x22, 0x3e, 0x0a, 0x5f, 0x5f, 0x23, 0x23, 0x23, 0x5f, 0x5f, 0x0a, 0x3c, 0x2f, 0x70, 0x72, 0x65, 0x3e, 0x0a, 0x3c, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x20, 0x74, 0x79, 0x70, 0x65, 0x3d, 0x22, 0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x22, 0x3e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x20, 0x76, 0x61, 0x6e, 0x69, 0x6c, 0x6c, 0x61, 0x4a, 0x73, 0x57, 0x68, 0x65, 0x65, 0x6c, 0x5a, 0x6f, 0x6f, 0x6d, 0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x27, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f, 0x63, 0x64, 0x6e, 0x2e, 0x6a, 0x73, 0x64, 0x65, 0x6c, 0x69, 0x76, 0x72, 0x2e, 0x6e, 0x65, 0x74, 0x2f, 0x6e, 0x70, 0x6d, 0x2f, 0x76, 0x61, 0x6e, 0x69, 0x6c, 0x6c, 0x61, 0x2d, 0x6a, 0x73, 0x2d, 0x77, 0x68, 0x65, 0x65, 0x6c, 0x2d, 0x7a, 0x6f, 0x6f, 0x6d, 0x40, 0x39, 0x2e, 0x30, 0x2e, 0x34, 0x2f, 0x2b, 0x65, 0x73, 0x6d, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x20, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x27, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f, 0x63, 0x64, 0x6e, 0x2e, 0x6a, 0x73, 0x64, 0x65, 0x6c, 0x69, 0x76, 0x72, 0x2e, 0x6e, 0x65, 0x74, 0x2f, 0x6e, 0x70, 0x6d, 0x2f, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x40, 0x31, 0x31, 0x2f, 0x64, 0x69, 0x73, 0x74, 0x2f, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x2e, 0x65, 0x73, 0x6d, 0x2e, 0x6d, 0x69, 0x6e, 0x2e, 0x6d, 0x6a, 0x73, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x75, 0x6e, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x69, 0x6e, 0x69, 0x74, 0x56, 0x69, 0x65, 0x77, 0x65, 0x72, 0x28, 0x29, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x76, 0x61, 0x72, 0x20, 0x65, 0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x3d, 0x20, 0x64, 0x6f, 0x63, 0x75, 0x6d, 0x65, 0x6e, 0x74, 0x2e, 0x71, 0x75, 0x65, 0x72, 0x79, 0x53, 0x65, 0x6c, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x28, 0x27, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x73, 0x76, 0x67, 0x27, 0x29, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x76, 0x61, 0x6e, 0x69, 0x6c, 0x6c, 0x61, 0x4a, 0x73, 0x57, 0x68, 0x65, 0x65, 0x6c, 0x5a, 0x6f, 0x6f, 0x6d, 0x2e, 0x63, 0x72, 0x65, 0x61, 0x74, 0x65, 0x28, 0x27, 0x70, 0x72, 0x65, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x20, 0x73, 0x76, 0x67, 0x27, 0x2c, 0x20, 0x7b, 0x20, 0x74, 0x79, 0x70, 0x65, 0x3a, 0x20, 0x27, 0x68, 0x74, 0x6d, 0x6c, 0x27, 0x2c, 0x20, 0x73, 0x6d, 0x6f, 0x6f, 0x74, 0x68, 0x54, 0x69, 0x6d, 0x65, 0x44, 0x72, 0x61, 0x67, 0x3a, 0x20, 0x30, 0x2c, 0x20, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x65, 0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x74, 0x2e, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x57, 0x69, 0x64, 0x74, 0x68, 0x2c, 0x20, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x65, 0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x74, 0x2e, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x48, 0x65, 0x69, 0x67, 0x68, 0x74, 0x2c, 0x20, 0x6d, 0x61, 0x78, 0x53, 0x63, 0x61, 0x6c, 0x65, 0x3a, 0x20, 0x33, 0x20, 0x7d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x2e, 0x69, 0x6e, 0x69, 0x74, 0x69, 0x61, 0x6c, 0x69, 0x7a, 0x65, 0x28, 0x7b, 0x20, 0x73, 0x74, 0x61, 0x72, 0x74, 0x4f, 0x6e, 0x4c, 0x6f, 0x61, 0x64, 0x3a, 0x20, 0x66, 0x61, 0x6c, 0x73, 0x65, 0x20, 0x7d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x6f, 0x63, 0x75, 0x6d, 0x65, 0x6e, 0x74, 0x2e, 0x66, 0x6f, 0x6e, 0x74, 0x73, 0x2e, 0x72, 0x65, 0x61, 0x64, 0x79, 0x2e, 0x74, 0x68, 0x65, 0x6e, 0x28, 0x28, 0x29, 0x20, 0x3d, 0x3e, 0x20, 0x7b, 0x20, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x2e, 0x72, 0x75, 0x6e, 0x28, 0x7b, 0x20, 0x71, 0x75, 0x65, 0x72, 0x79, 0x53, 0x65, 0x6c, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x3a, 0x20, 0x27, 0x2e, 0x6d, 0x65, 0x72, 0x6d, 0x61, 0x69, 0x64, 0x27, 0x2c, 0x20, 0x70, 0x6f, 0x73, 0x74, 0x52, 0x65, 0x6e, 0x64, 0x65, 0x72, 0x43, 0x61, 0x6c, 0x6c, 0x62, 0x61, 0x63, 0x6b, 0x3a, 0x20, 0x69, 0x6e, 0x69, 0x74, 0x56, 0x69, 0x65, 0x77, 0x65, 0x72, 0x20, 0x7d, 0x29, 0x3b, 0x20, 0x7d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x3c, 0x2f, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x3e, 0x0a, 0x3c, 0x2f, 0x62, 0x6f, 0x64, 0x79, 0x3e, 0x0a, 0x3c, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3e, 0x0a, 0x00 };
const unsigned int ff_graph_html_len = 2153;


/* ========== fftools/resources/graph.css.c ========== */

const unsigned char ff_graph_css_data[] = { 0x2f, 0x2a, 0x20, 0x56, 0x61, 0x72, 0x69, 0x61, 0x62, 0x6c, 0x65, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x72, 0x6f, 0x6f, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x3a, 0x20, 0x23, 0x36, 0x65, 0x61, 0x61, 0x37, 0x62, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x3a, 0x20, 0x23, 0x34, 0x37, 0x37, 0x66, 0x62, 0x33, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x3a, 0x20, 0x23, 0x61, 0x64, 0x37, 0x36, 0x61, 0x62, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x3a, 0x20, 0x23, 0x36, 0x36, 0x36, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x43, 0x6f, 0x6d, 0x6d, 0x6f, 0x6e, 0x20, 0x26, 0x20, 0x4d, 0x69, 0x73, 0x63, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x64, 0x65, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6e, 0x74, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x73, 0x70, 0x61, 0x6e, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x23, 0x64, 0x66, 0x64, 0x66, 0x64, 0x66, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x32, 0x2e, 0x33, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x64, 0x72, 0x6f, 0x70, 0x2d, 0x73, 0x68, 0x61, 0x64, 0x6f, 0x77, 0x28, 0x31, 0x70, 0x78, 0x20, 0x32, 0x70, 0x78, 0x20, 0x32, 0x70, 0x78, 0x20, 0x72, 0x67, 0x62, 0x61, 0x28, 0x31, 0x38, 0x35, 0x2c, 0x31, 0x38, 0x35, 0x2c, 0x31, 0x38, 0x35, 0x2c, 0x30, 0x2e, 0x32, 0x29, 0x29, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x78, 0x3a, 0x20, 0x38, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x79, 0x3a, 0x20, 0x38, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x75, 0x6e, 0x73, 0x65, 0x74, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x33, 0x70, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x30, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x49, 0x6e, 0x70, 0x75, 0x74, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x20, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x76, 0x69, 0x73, 0x69, 0x62, 0x6c, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x3a, 0x6e, 0x6f, 0x74, 0x28, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x20, 0x64, 0x69, 0x76, 0x29, 0x2c, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2d, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x3a, 0x6e, 0x6f, 0x74, 0x28, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x20, 0x64, 0x69, 0x76, 0x29, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x69, 0x6e, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x34, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x30, 0x30, 0x25, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x74, 0x6f, 0x70, 0x3a, 0x20, 0x30, 0x2e, 0x31, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x2e, 0x33, 0x35, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x2d, 0x62, 0x6f, 0x74, 0x74, 0x6f, 0x6d, 0x3a, 0x20, 0x31, 0x2e, 0x39, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x2d, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x72, 0x6f, 0x77, 0x2d, 0x72, 0x65, 0x76, 0x65, 0x72, 0x73, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x32, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x2d, 0x72, 0x61, 0x64, 0x69, 0x75, 0x73, 0x3a, 0x20, 0x30, 0x2e, 0x34, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x30, 0x2e, 0x31, 0x38, 0x65, 0x6d, 0x20, 0x73, 0x6f, 0x6c, 0x69, 0x64, 0x20, 0x23, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x64, 0x62, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x36, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x2e, 0x33, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x70, 0x61, 0x63, 0x69, 0x74, 0x79, 0x3a, 0x20, 0x30, 0x2e, 0x38, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x49, 0x6e, 0x20, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x4f, 0x75, 0x74, 0x20, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x64, 0x65, 0x6d, 0x75, 0x78, 0x65, 0x72, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x6d, 0x75, 0x78, 0x65, 0x72, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3a, 0x20, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x39, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x2d, 0x73, 0x70, 0x61, 0x63, 0x65, 0x3a, 0x20, 0x6e, 0x6f, 0x77, 0x72, 0x61, 0x70, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x68, 0x69, 0x64, 0x64, 0x65, 0x6e, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x65, 0x6c, 0x6c, 0x69, 0x70, 0x73, 0x69, 0x73, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x38, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x20, 0x30, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x20, 0x30, 0x20, 0x30, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x66, 0x69, 0x6c, 0x65, 0x5f, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x66, 0x69, 0x6c, 0x65, 0x5f, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x38, 0x38, 0x38, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x2d, 0x72, 0x61, 0x64, 0x69, 0x75, 0x73, 0x3a, 0x20, 0x30, 0x2e, 0x34, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x36, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x2e, 0x34, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x70, 0x61, 0x63, 0x69, 0x74, 0x79, 0x3a, 0x20, 0x30, 0x2e, 0x38, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x75, 0x72, 0x6c, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x2e, 0x75, 0x72, 0x6c, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x34, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x61, 0x62, 0x73, 0x6f, 0x6c, 0x75, 0x74, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x74, 0x74, 0x6f, 0x6d, 0x3a, 0x20, 0x30, 0x2e, 0x37, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x34, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x2d, 0x73, 0x70, 0x61, 0x63, 0x65, 0x3a, 0x20, 0x6e, 0x6f, 0x77, 0x72, 0x61, 0x70, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x68, 0x69, 0x64, 0x64, 0x65, 0x6e, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x65, 0x6c, 0x6c, 0x69, 0x70, 0x73, 0x69, 0x73, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x2e, 0x33, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x72, 0x74, 0x6c, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x39, 0x39, 0x39, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x31, 0x2e, 0x38, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x75, 0x72, 0x6c, 0x28, 0x23, 0x66, 0x66, 0x2d, 0x72, 0x61, 0x64, 0x67, 0x72, 0x61, 0x64, 0x69, 0x65, 0x6e, 0x74, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x49, 0x6e, 0x70, 0x75, 0x74, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x20, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x70, 0x61, 0x64, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x30, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x30, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x23, 0x65, 0x35, 0x65, 0x35, 0x65, 0x35, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x32, 0x2e, 0x37, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x30, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x78, 0x3a, 0x20, 0x33, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x79, 0x3a, 0x20, 0x33, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x30, 0x2e, 0x32, 0x25, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x76, 0x69, 0x73, 0x69, 0x62, 0x6c, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x3a, 0x6e, 0x6f, 0x74, 0x28, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x20, 0x64, 0x69, 0x76, 0x29, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x3a, 0x6e, 0x6f, 0x74, 0x28, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x64, 0x69, 0x76, 0x20, 0x64, 0x69, 0x76, 0x29, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x2e, 0x35, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x30, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x69, 0x6e, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x30, 0x30, 0x25, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x2d, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x72, 0x6f, 0x77, 0x2d, 0x72, 0x65, 0x76, 0x65, 0x72, 0x73, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6e, 0x61, 0x6d, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x6e, 0x61, 0x6d, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3a, 0x20, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x2d, 0x73, 0x70, 0x61, 0x63, 0x65, 0x3a, 0x20, 0x6e, 0x6f, 0x77, 0x72, 0x61, 0x70, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x68, 0x69, 0x64, 0x64, 0x65, 0x6e, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x65, 0x6c, 0x6c, 0x69, 0x70, 0x73, 0x69, 0x73, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x6c, 0x65, 0x66, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x62, 0x6f, 0x74, 0x74, 0x6f, 0x6d, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x31, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x20, 0x31, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x2d, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x38, 0x38, 0x38, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x63, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x2d, 0x72, 0x61, 0x64, 0x69, 0x75, 0x73, 0x3a, 0x20, 0x30, 0x2e, 0x33, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x36, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x33, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x70, 0x61, 0x63, 0x69, 0x74, 0x79, 0x3a, 0x20, 0x30, 0x2e, 0x38, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x20, 0x2e, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x36, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x76, 0x61, 0x72, 0x69, 0x61, 0x6e, 0x74, 0x2d, 0x65, 0x6d, 0x6f, 0x6a, 0x69, 0x3a, 0x20, 0x74, 0x65, 0x78, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6c, 0x65, 0x78, 0x3a, 0x20, 0x30, 0x20, 0x30, 0x20, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x38, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x6c, 0x65, 0x66, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x36, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x5c, 0x32, 0x33, 0x39, 0x41, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x32, 0x2e, 0x32, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x35, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x62, 0x6f, 0x6c, 0x64, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x5c, 0x31, 0x46, 0x33, 0x39, 0x44, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x37, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x39, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x5c, 0x31, 0x41, 0x43, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x32, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x73, 0x63, 0x61, 0x6c, 0x65, 0x58, 0x28, 0x31, 0x2e, 0x35, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x74, 0x6f, 0x70, 0x3a, 0x20, 0x30, 0x2e, 0x30, 0x35, 0x30, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x61, 0x74, 0x74, 0x61, 0x63, 0x68, 0x6d, 0x65, 0x6e, 0x74, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x61, 0x74, 0x74, 0x61, 0x63, 0x68, 0x6d, 0x65, 0x6e, 0x74, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x5c, 0x31, 0x46, 0x34, 0x43, 0x45, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x33, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x35, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x66, 0x66, 0x2d, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x64, 0x61, 0x74, 0x61, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x2c, 0x20, 0x2e, 0x66, 0x66, 0x2d, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x2e, 0x64, 0x61, 0x74, 0x61, 0x3a, 0x3a, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x3a, 0x20, 0x27, 0x5c, 0x32, 0x37, 0x45, 0x38, 0x5c, 0x32, 0x32, 0x31, 0x39, 0x5c, 0x32, 0x32, 0x31, 0x39, 0x5c, 0x32, 0x32, 0x31, 0x39, 0x5c, 0x32, 0x37, 0x45, 0x39, 0x27, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x69, 0x6e, 0x65, 0x2d, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x31, 0x2e, 0x31, 0x37, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6c, 0x65, 0x74, 0x74, 0x65, 0x72, 0x2d, 0x73, 0x70, 0x61, 0x63, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x2d, 0x30, 0x2e, 0x33, 0x70, 0x78, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x46, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x47, 0x72, 0x61, 0x70, 0x68, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x2d, 0x64, 0x61, 0x73, 0x68, 0x61, 0x72, 0x72, 0x61, 0x79, 0x3a, 0x20, 0x36, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x2e, 0x33, 0x70, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x23, 0x64, 0x31, 0x64, 0x31, 0x64, 0x31, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x2e, 0x69, 0x64, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x3a, 0x20, 0x6e, 0x6f, 0x6e, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x2e, 0x6e, 0x61, 0x6d, 0x65, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x72, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x30, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x39, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x73, 0x20, 0x2e, 0x64, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x34, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x37, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x76, 0x65, 0x72, 0x74, 0x69, 0x63, 0x61, 0x6c, 0x2d, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x3a, 0x20, 0x6d, 0x69, 0x64, 0x64, 0x6c, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x23, 0x37, 0x37, 0x37, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x66, 0x61, 0x6d, 0x69, 0x6c, 0x79, 0x3a, 0x20, 0x43, 0x61, 0x73, 0x63, 0x61, 0x64, 0x69, 0x61, 0x20, 0x43, 0x6f, 0x64, 0x65, 0x2c, 0x20, 0x4c, 0x75, 0x63, 0x69, 0x64, 0x61, 0x20, 0x43, 0x6f, 0x6e, 0x73, 0x6f, 0x6c, 0x65, 0x2c, 0x20, 0x6d, 0x6f, 0x6e, 0x6f, 0x73, 0x70, 0x61, 0x63, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x46, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x53, 0x68, 0x61, 0x70, 0x65, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x78, 0x3a, 0x20, 0x31, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x72, 0x79, 0x3a, 0x20, 0x31, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x70, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x23, 0x64, 0x33, 0x64, 0x33, 0x64, 0x33, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x75, 0x72, 0x6c, 0x28, 0x23, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x67, 0x72, 0x61, 0x64, 0x69, 0x65, 0x6e, 0x74, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x64, 0x72, 0x6f, 0x70, 0x2d, 0x73, 0x68, 0x61, 0x64, 0x6f, 0x77, 0x28, 0x31, 0x70, 0x78, 0x20, 0x31, 0x70, 0x78, 0x20, 0x32, 0x70, 0x78, 0x20, 0x72, 0x67, 0x62, 0x61, 0x28, 0x30, 0x2c, 0x20, 0x30, 0x2c, 0x20, 0x30, 0x2c, 0x20, 0x30, 0x2e, 0x31, 0x29, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x30, 0x2e, 0x34, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x66, 0x6c, 0x6f, 0x77, 0x3a, 0x20, 0x76, 0x69, 0x73, 0x69, 0x62, 0x6c, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x31, 0x2e, 0x30, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x75, 0x70, 0x70, 0x65, 0x72, 0x63, 0x61, 0x73, 0x65, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x69, 0x6e, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x35, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x2d, 0x62, 0x6f, 0x74, 0x74, 0x6f, 0x6d, 0x3a, 0x20, 0x30, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x20, 0x73, 0x70, 0x61, 0x6e, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x69, 0x6e, 0x68, 0x65, 0x72, 0x69, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x44, 0x65, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x73, 0x20, 0x26, 0x20, 0x45, 0x6e, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x64, 0x65, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x20, 0x72, 0x65, 0x63, 0x74, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x2e, 0x66, 0x66, 0x2d, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x20, 0x72, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x70, 0x78, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x23, 0x64, 0x33, 0x64, 0x33, 0x64, 0x33, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x75, 0x72, 0x6c, 0x28, 0x23, 0x66, 0x66, 0x2d, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x67, 0x72, 0x61, 0x64, 0x69, 0x65, 0x6e, 0x74, 0x29, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x3a, 0x20, 0x64, 0x72, 0x6f, 0x70, 0x2d, 0x73, 0x68, 0x61, 0x64, 0x6f, 0x77, 0x28, 0x31, 0x70, 0x78, 0x20, 0x31, 0x70, 0x78, 0x20, 0x32, 0x70, 0x78, 0x20, 0x72, 0x67, 0x62, 0x61, 0x28, 0x30, 0x2c, 0x20, 0x30, 0x2c, 0x20, 0x30, 0x2c, 0x20, 0x30, 0x2e, 0x31, 0x29, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x64, 0x65, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x2c, 0x20, 0x2e, 0x6e, 0x6f, 0x64, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x64, 0x69, 0x76, 0x2e, 0x66, 0x66, 0x2d, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x38, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x69, 0x6e, 0x2d, 0x77, 0x69, 0x64, 0x74, 0x68, 0x3a, 0x20, 0x33, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2f, 0x2a, 0x20, 0x4c, 0x69, 0x6e, 0x6b, 0x73, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x41, 0x72, 0x72, 0x6f, 0x77, 0x73, 0x20, 0x2a, 0x2f, 0x0a, 0x70, 0x61, 0x74, 0x68, 0x2e, 0x66, 0x6c, 0x6f, 0x77, 0x63, 0x68, 0x61, 0x72, 0x74, 0x2d, 0x6c, 0x69, 0x6e, 0x6b, 0x5b, 0x69, 0x64, 0x7c, 0x3d, 0x27, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x27, 0x5d, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x76, 0x69, 0x64, 0x65, 0x6f, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x70, 0x61, 0x74, 0x68, 0x2e, 0x66, 0x6c, 0x6f, 0x77, 0x63, 0x68, 0x61, 0x72, 0x74, 0x2d, 0x6c, 0x69, 0x6e, 0x6b, 0x5b, 0x69, 0x64, 0x7c, 0x3d, 0x27, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x27, 0x5d, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x70, 0x61, 0x74, 0x68, 0x2e, 0x66, 0x6c, 0x6f, 0x77, 0x63, 0x68, 0x61, 0x72, 0x74, 0x2d, 0x6c, 0x69, 0x6e, 0x6b, 0x5b, 0x69, 0x64, 0x7c, 0x3d, 0x27, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x27, 0x5d, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x73, 0x75, 0x62, 0x74, 0x69, 0x74, 0x6c, 0x65, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x6d, 0x61, 0x72, 0x6b, 0x65, 0x72, 0x2e, 0x6d, 0x61, 0x72, 0x6b, 0x65, 0x72, 0x20, 0x70, 0x61, 0x74, 0x68, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x69, 0x6c, 0x6c, 0x3a, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x78, 0x74, 0x2d, 0x73, 0x74, 0x72, 0x6f, 0x6b, 0x65, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x65, 0x64, 0x67, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x6c, 0x61, 0x74, 0x65, 0x59, 0x28, 0x2d, 0x31, 0x72, 0x65, 0x6d, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x65, 0x64, 0x67, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x70, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x77, 0x68, 0x69, 0x74, 0x65, 0x2d, 0x73, 0x70, 0x61, 0x63, 0x65, 0x3a, 0x20, 0x6e, 0x6f, 0x77, 0x72, 0x61, 0x70, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x6d, 0x61, 0x72, 0x67, 0x69, 0x6e, 0x3a, 0x20, 0x31, 0x72, 0x65, 0x6d, 0x20, 0x30, 0x2e, 0x35, 0x72, 0x65, 0x6d, 0x20, 0x21, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x6e, 0x74, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3a, 0x20, 0x35, 0x30, 0x30, 0x3b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x63, 0x6f, 0x6c, 0x6f, 0x72, 0x3a, 0x20, 0x76, 0x61, 0x72, 0x28, 0x2d, 0x2d, 0x66, 0x66, 0x2d, 0x63, 0x6f, 0x6c, 0x74, 0x65, 0x78, 0x74, 0x29, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x65, 0x64, 0x67, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x2c, 0x20, 0x2e, 0x6c, 0x61, 0x62, 0x65, 0x6c, 0x42, 0x6b, 0x67, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x62, 0x61, 0x63, 0x6b, 0x67, 0x72, 0x6f, 0x75, 0x6e, 0x64, 0x3a, 0x20, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6e, 0x74, 0x3b, 0x0a, 0x7d, 0x0a, 0x0a, 0x2e, 0x65, 0x64, 0x67, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x73, 0x20, 0x2e, 0x65, 0x64, 0x67, 0x65, 0x4c, 0x61, 0x62, 0x65, 0x6c, 0x20, 0x2a, 0x20, 0x7b, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x66, 0x6f, 0x6e, 0x74, 0x2d, 0x73, 0x69, 0x7a, 0x65, 0x3a, 0x20, 0x30, 0x2e, 0x38, 0x72, 0x65, 0x6d, 0x3b, 0x0a, 0x7d, 0x0a, 0x00 };
const unsigned int ff_graph_css_len = 7752;


/* shared globals for graphprint (referenced from graphprint.c) */
int print_graphs = 0;
char *print_graphs_file = NULL;
char *print_graphs_format = NULL;
