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

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/pixdesc.h"

#include "avcodec.h"
#include "dv_profile.h"
#include "dv_profile_internal.h"


const AVDVProfile* ff_dv_frame_profile(AVCodecContext* codec, const AVDVProfile *sys,
                                       const uint8_t *frame, unsigned buf_size)
{

    return NULL;
}

const AVDVProfile *av_dv_frame_profile(const AVDVProfile *sys,
                                       const uint8_t *frame, unsigned buf_size)
{
    return ff_dv_frame_profile(NULL, sys, frame, buf_size);
}

const AVDVProfile *av_dv_codec_profile(int width, int height,
                                       enum AVPixelFormat pix_fmt)
{

    return NULL;
}

const AVDVProfile *av_dv_codec_profile2(int width, int height,
                                       enum AVPixelFormat pix_fmt,
                                       AVRational frame_rate)
{
    const AVDVProfile *p = NULL;

    return p;
}
