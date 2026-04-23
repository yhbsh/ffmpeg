/*
 * General DV demuxer
 * Copyright (c) 2003 Roman Shaposhnik
 *
 * Many thanks to Dan Dennedy <dan@dennedy.org> for providing wealth
 * of DV technical info.
 *
 * Raw DV format
 * Copyright (c) 2002 Fabrice Bellard
 *
 * 50 Mbps (DVCPRO50) and 100 Mbps (DVCPRO HD) support
 * Copyright (c) 2006 Daniel Maas <dmaas@maasdigital.com>
 * Funded by BBC Research & Development
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

#include "config_components.h"

#include <time.h>
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "libavcodec/dv_profile.h"
#include "libavcodec/dv.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/timecode.h"
#include "dv.h"
#include "libavutil/avassert.h"

DVDemuxContext *avpriv_dv_init_demux(AVFormatContext *s)
{
    return NULL;
}

int avpriv_dv_get_packet(DVDemuxContext *c, AVPacket *pkt)
{
    return AVERROR(ENOSYS);
}

int avpriv_dv_produce_packet(DVDemuxContext *c, AVPacket *pkt,
                             uint8_t *buf, int buf_size, int64_t pos)
{
    return AVERROR(ENOSYS);
}
