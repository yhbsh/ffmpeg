/*
 * Copyright (c) 2007 Nicolas George <nicolas.george@normalesup.org>
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2012 Paul B Mahol
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
 * Misc test sources.
 *
 * testsrc is based on the test pattern generator demuxer by Nicolas George:
 * http://lists.ffmpeg.org/pipermail/ffmpeg-devel/2007-October/037845.html
 *
 * rgbtestsrc is ported from MPlayer libmpcodecs/vf_rgbtest.c by
 * Michael Niedermayer.
 *
 * allyuv, smptebars and smptehdbars are by Paul B Mahol.
 */

#include "config_components.h"

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/ffmath.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/xga_font_data.h"
#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

typedef struct TestSourceContext {
    const AVClass *class;
    int w, h;
    int pw, ph;
    unsigned int nb_frame;
    AVRational time_base, frame_rate;
    int64_t pts;
    int64_t duration;           ///< duration expressed in microseconds
    AVRational sar;             ///< sample aspect ratio
    int draw_once;              ///< draw only the first frame, always put out the same picture
    int draw_once_reset;        ///< draw only the first frame or in case of reset
    AVFrame *picref;            ///< cached reference containing the painted picture

    void (* fill_picture_fn)(AVFilterContext *ctx, AVFrame *frame);

    /* only used by testsrc */
    int nb_decimals;

    /* only used by testsrc2 */
    int alpha;

    /* only used by yuvtest */
    uint8_t ayuv_map[4];

    /* only used by colorspectrum */
    int type;

    /* only used by color */
    FFDrawContext draw;
    FFDrawColor color;
    uint8_t color_rgba[4];

    /* only used by rgbtest */
    uint8_t rgba_map[4];
    int complement;
    int depth;

    /* only used by haldclut */
    int level;

    /* only used by zoneplate */
    int k0, kx, ky, kt;
    int kxt, kyt, kxy;
    int kx2, ky2, kt2;
    int xo, yo, to, kU, kV;
    int lut_precision;
    uint8_t *lut;
    int (*fill_slice_fn)(AVFilterContext *ctx, void *arg, int job, int nb_jobs);
} TestSourceContext;

#define OFFSET(x) offsetof(TestSourceContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define FLAGSR AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

#define SIZE_OPTIONS \
    { "size",     "set video size",     OFFSET(w),        AV_OPT_TYPE_IMAGE_SIZE, {.str = "320x240"}, 0, 0, FLAGS },\
    { "s",        "set video size",     OFFSET(w),        AV_OPT_TYPE_IMAGE_SIZE, {.str = "320x240"}, 0, 0, FLAGS },\

#define COMMON_OPTIONS_NOSIZE \
    { "rate",     "set video rate",     OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, FLAGS },\
    { "r",        "set video rate",     OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, FLAGS },\
    { "duration", "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },\
    { "d",        "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },\
    { "sar",      "set video sample aspect ratio", OFFSET(sar), AV_OPT_TYPE_RATIONAL, {.dbl= 1},  0, INT_MAX, FLAGS },

#define COMMON_OPTIONS SIZE_OPTIONS COMMON_OPTIONS_NOSIZE

#define NOSIZE_OPTIONS_OFFSET 2
/* Filters using COMMON_OPTIONS_NOSIZE also use the following options
 * via &options[NOSIZE_OPTIONS_OFFSET]. So don't break it. */
static const AVOption options[] = {
    COMMON_OPTIONS
    { NULL }
};

static av_cold int init(AVFilterContext *ctx)
{
    TestSourceContext *test = ctx->priv;

    test->time_base = av_inv_q(test->frame_rate);
    test->nb_frame = 0;
    test->pts = 0;

    av_log(ctx, AV_LOG_VERBOSE, "size:%dx%d rate:%d/%d duration:%f sar:%d/%d\n",
           test->w, test->h, test->frame_rate.num, test->frame_rate.den,
           test->duration < 0 ? -1 : (double)test->duration/1000000,
           test->sar.num, test->sar.den);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TestSourceContext *test = ctx->priv;

    av_frame_free(&test->picref);
    av_freep(&test->lut);
}

static int config_props(AVFilterLink *outlink)
{
    TestSourceContext *test = outlink->src->priv;
    FilterLink *l = ff_filter_link(outlink);

    outlink->w = test->w;
    outlink->h = test->h;
    outlink->sample_aspect_ratio = test->sar;
    l->frame_rate = test->frame_rate;
    outlink->time_base  = test->time_base;

    return 0;
}

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_props,
    },
};

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    TestSourceContext *test = ctx->priv;
    AVFrame *frame;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;
    if (test->duration >= 0 &&
        av_rescale_q(test->pts, test->time_base, AV_TIME_BASE_Q) >= test->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, test->pts);
        return 0;
    }

    if (test->draw_once) {
        if (test->draw_once_reset) {
            av_frame_free(&test->picref);
            test->draw_once_reset = 0;
        }
        if (!test->picref) {
            test->picref =
                ff_get_video_buffer(outlink, test->w, test->h);
            if (!test->picref)
                return AVERROR(ENOMEM);
            test->fill_picture_fn(outlink->src, test->picref);
        }
        frame = av_frame_clone(test->picref);
    } else
        frame = ff_get_video_buffer(outlink, test->w, test->h);

    if (!frame)
        return AVERROR(ENOMEM);
    frame->pts                 = test->pts;
    frame->duration            = 1;
    frame->flags              |= AV_FRAME_FLAG_KEY;
    frame->flags              &= ~AV_FRAME_FLAG_INTERLACED;
    frame->pict_type           = AV_PICTURE_TYPE_I;
    frame->sample_aspect_ratio = test->sar;
    if (!test->draw_once)
        test->fill_picture_fn(outlink->src, frame);

    test->pts++;
    test->nb_frame++;

    return ff_filter_frame(outlink, frame);
}



AVFILTER_DEFINE_CLASS_EXT(nullsrc_yuvtestsrc, "nullsrc/yuvtestsrc", options);

#if CONFIG_NULLSRC_FILTER

static void nullsrc_fill_picture(AVFilterContext *ctx, AVFrame *picref) { }

static av_cold int nullsrc_init(AVFilterContext *ctx)
{
    TestSourceContext *test = ctx->priv;

    test->fill_picture_fn = nullsrc_fill_picture;
    return init(ctx);
}

const FFFilter ff_vsrc_nullsrc = {
    .p.name      = "nullsrc",
    .p.description= NULL_IF_CONFIG_SMALL("Null video source, return unprocessed video frames."),
    .p.priv_class= &nullsrc_yuvtestsrc_class,
    .init        = nullsrc_init,
    .uninit      = uninit,
    .activate    = activate,
    .priv_size   = sizeof(TestSourceContext),
    FILTER_OUTPUTS(outputs),
};

#endif /* CONFIG_NULLSRC_FILTER */


av_unused static void set_color(TestSourceContext *s, FFDrawColor *color, uint32_t argb)
{
    uint8_t rgba[4] = { (argb >> 16) & 0xFF,
                        (argb >>  8) & 0xFF,
                        (argb >>  0) & 0xFF,
                        (argb >> 24) & 0xFF, };
    ff_draw_color(&s->draw, color, rgba);
}





AVFILTER_DEFINE_CLASS_EXT(allyuv_allrgb, "allyuv/allrgb",
                          &options[NOSIZE_OPTIONS_OFFSET]);





