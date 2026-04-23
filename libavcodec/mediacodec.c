/*
 * Android MediaCodec public API functions
 *
 * Copyright (c) 2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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

#include "libavutil/error.h"

#include "mediacodec.h"


#include <stdlib.h>

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return NULL;
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    return AVERROR(ENOSYS);
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer, int64_t time)
{
    return AVERROR(ENOSYS);
}

