/*
 * Simple Media Protocol (SMP) muxer
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

/*
 * Wire format (all integers big-endian). Each message is:
 *
 *   4 bytes  message length L (excludes this length field)
 *   1 byte   type
 *   L-1 bytes  payload
 *
 * Types:
 *
 *   0x01 HEADER (sent once, first message of a session)
 *     1 byte  version = 1
 *     1 byte  num_streams
 *     For each stream:
 *       4 bytes  codec_id           (AVCodecID)
 *       1 byte   codec_type         (AVMediaType)
 *       4 bytes  time_base.num
 *       4 bytes  time_base.den
 *       4 bytes  width              (0 if not video)
 *       4 bytes  height             (0 if not video)
 *       4 bytes  sample_rate        (0 if not audio)
 *       1 byte   channels           (0 if not audio)
 *       4 bytes  extradata_size
 *       M bytes  extradata
 *
 *   0x02 PACKET     (regular packet)
 *   0x03 KEY        (keyframe packet)
 *     1 byte   stream_index
 *     8 bytes  pts
 *     8 bytes  dts
 *     8 bytes  duration
 *     4 bytes  data_size
 *     N bytes  data
 *
 * The relay server only inspects the type byte to maintain header + the
 * current GOP per stream so new subscribers join at the next keyframe
 * boundary instead of buffering arbitrary container bytes.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio.h"
#include "mux.h"

#define SMP_FMT_VERSION 1

#define SMP_MSG_HEADER 0x01
#define SMP_MSG_PACKET 0x02
#define SMP_MSG_KEY    0x03

typedef struct SMPMuxContext {
    const AVClass *class;
    int primary_stream;
} SMPMuxContext;

static int smp_send_message(AVFormatContext *s, AVIOContext *dyn)
{
    uint8_t *buf = NULL;
    int len;

    len = avio_close_dyn_buf(dyn, &buf);
    if (len < 0) {
        av_free(buf);
        return len;
    }

    avio_wb32(s->pb, (uint32_t)len);
    avio_write(s->pb, buf, len);
    avio_flush(s->pb);

    av_free(buf);
    return 0;
}

static int smp_write_header(AVFormatContext *s)
{
    SMPMuxContext *ctx = s->priv_data;
    AVIOContext *dyn = NULL;
    int i, ret;

    if (s->nb_streams > 255) {
        av_log(s, AV_LOG_ERROR, "smp: too many streams (max 255)\n");
        return AVERROR(EINVAL);
    }

    /* Pick the primary stream whose keyframes mark GOP boundaries: first
     * video, else first audio, else stream 0. The relay only needs to track
     * one buffer per session and a new subscriber catches up at the next
     * primary keyframe. */
    ctx->primary_stream = 0;
    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->primary_stream = i;
            goto have_primary;
        }
    }
    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ctx->primary_stream = i;
            break;
        }
    }
have_primary:

    if ((ret = avio_open_dyn_buf(&dyn)) < 0)
        return ret;

    avio_w8(dyn, SMP_MSG_HEADER);
    avio_w8(dyn, SMP_FMT_VERSION);
    avio_w8(dyn, (uint8_t)s->nb_streams);

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        AVCodecParameters *par = st->codecpar;

        avio_wb32(dyn, (uint32_t)par->codec_id);
        avio_w8 (dyn, (uint8_t)par->codec_type);
        avio_wb32(dyn, (uint32_t)st->time_base.num);
        avio_wb32(dyn, (uint32_t)st->time_base.den);
        avio_wb32(dyn, (uint32_t)par->width);
        avio_wb32(dyn, (uint32_t)par->height);
        avio_wb32(dyn, (uint32_t)par->sample_rate);
        avio_w8 (dyn, (uint8_t)par->ch_layout.nb_channels);
        avio_wb32(dyn, (uint32_t)par->extradata_size);
        if (par->extradata_size > 0)
            avio_write(dyn, par->extradata, par->extradata_size);
    }

    return smp_send_message(s, dyn);
}

static int smp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SMPMuxContext *ctx = s->priv_data;
    AVIOContext *dyn = NULL;
    int is_gop_key;
    int ret;

    if (pkt->stream_index < 0 || pkt->stream_index > 255)
        return AVERROR(EINVAL);

    if ((ret = avio_open_dyn_buf(&dyn)) < 0)
        return ret;

    is_gop_key = (pkt->stream_index == ctx->primary_stream) &&
                 (pkt->flags & AV_PKT_FLAG_KEY);
    avio_w8 (dyn, is_gop_key ? SMP_MSG_KEY : SMP_MSG_PACKET);
    avio_w8 (dyn, (uint8_t)pkt->stream_index);
    avio_wb64(dyn, (uint64_t)pkt->pts);
    avio_wb64(dyn, (uint64_t)pkt->dts);
    avio_wb64(dyn, (uint64_t)pkt->duration);
    avio_wb32(dyn, (uint32_t)pkt->size);
    if (pkt->size > 0)
        avio_write(dyn, pkt->data, pkt->size);

    return smp_send_message(s, dyn);
}

const FFOutputFormat ff_smp_muxer = {
    .p.name         = "smp",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Simple Media Protocol"),
    .p.audio_codec  = AV_CODEC_ID_AAC,
    .p.video_codec  = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(SMPMuxContext),
    .write_header   = smp_write_header,
    .write_packet   = smp_write_packet,
    .p.flags        = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                      AVFMT_TS_NONSTRICT | AVFMT_TS_NEGATIVE,
};
