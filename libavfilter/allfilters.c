/*
 * filter registration
 * Copyright (c) 2008 Vitor Sessak
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

#include "avfilter.h"
#include "filters.h"

extern const FFFilter ff_af_acrossfade;
extern const FFFilter ff_af_afade;
extern const FFFilter ff_af_aformat;
extern const FFFilter ff_af_amix;
extern const FFFilter ff_af_anull;
extern const FFFilter ff_af_arealtime;
extern const FFFilter ff_af_aresample;
extern const FFFilter ff_af_asetpts;
extern const FFFilter ff_af_asplit;
extern const FFFilter ff_af_atrim;
extern const FFFilter ff_af_volume;

extern const FFFilter ff_asrc_anullsrc;
extern const FFFilter ff_asrc_sine;

extern const FFFilter ff_asink_anullsink;

extern const FFFilter ff_vf_colorbalance;
extern const FFFilter ff_vf_copy;
extern const FFFilter ff_vf_crop;
extern const FFFilter ff_vf_drawbox;
extern const FFFilter ff_vf_drawgrid;
extern const FFFilter ff_vf_fade;
extern const FFFilter ff_vf_format;
extern const FFFilter ff_vf_fps;
extern const FFFilter ff_vf_framerate;
extern const FFFilter ff_vf_hflip;
extern const FFFilter ff_vf_hue;
extern const FFFilter ff_vf_mix;
extern const FFFilter ff_vf_noformat;
extern const FFFilter ff_vf_null;
extern const FFFilter ff_vf_overlay;
extern const FFFilter ff_vf_pad;
extern const FFFilter ff_vf_realtime;
extern const FFFilter ff_vf_rotate;
extern const FFFilter ff_vf_scale;
extern const FFFilter ff_vf_scale2ref;
extern const FFFilter ff_vf_setpts;
extern const FFFilter ff_vf_split;
extern const FFFilter ff_vf_tmix;
extern const FFFilter ff_vf_transpose;
extern const FFFilter ff_vf_trim;
extern const FFFilter ff_vf_vflip;

extern const FFFilter ff_vsrc_allrgb;
extern const FFFilter ff_vsrc_allyuv;
extern const FFFilter ff_vsrc_color;
extern const FFFilter ff_vsrc_colorchart;
extern const FFFilter ff_vsrc_colorspectrum;
extern const FFFilter ff_vsrc_haldclutsrc;
extern const FFFilter ff_vsrc_nullsrc;
extern const FFFilter ff_vsrc_pal75bars;
extern const FFFilter ff_vsrc_pal100bars;
extern const FFFilter ff_vsrc_rgbtestsrc;
extern const FFFilter ff_vsrc_smptebars;
extern const FFFilter ff_vsrc_smptehdbars;
extern const FFFilter ff_vsrc_testsrc;
extern const FFFilter ff_vsrc_testsrc2;
extern const FFFilter ff_vsrc_yuvtestsrc;
extern const FFFilter ff_vsrc_zoneplate;

extern const FFFilter ff_vsink_nullsink;

/* multimedia filters */
extern const FFFilter ff_avf_concat;

/* multimedia sources */
extern const FFFilter ff_avsrc_amovie;
extern const FFFilter ff_avsrc_movie;

/* those filters are part of public or internal API,
 * they are formatted to not be found by the grep
 * as they are manually added again (due to their 'names'
 * being the same while having different 'types'). */
extern  const FFFilter ff_asrc_abuffer;
extern  const FFFilter ff_vsrc_buffer;
extern  const FFFilter ff_asink_abuffer;
extern  const FFFilter ff_vsink_buffer;

#include "libavfilter/filter_list.c"


const AVFilter *av_filter_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const FFFilter *f = filter_list[i];

    if (f) {
        *opaque = (void*)(i + 1);
        return &f->p;
    }

    return NULL;
}

const AVFilter *avfilter_get_by_name(const char *name)
{
    const AVFilter *f = NULL;
    void *opaque = 0;

    if (!name)
        return NULL;

    while ((f = av_filter_iterate(&opaque)))
        if (!strcmp(f->name, name))
            return f;

    return NULL;
}
