/*
 * Simple Media Protocol (SMP) demuxer
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

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "avio.h"
#include "demux.h"
#include "internal.h"

#define SMP_FMT_VERSION 1

#define SMP_MSG_HEADER 0x01
#define SMP_MSG_PACKET 0x02
#define SMP_MSG_KEY    0x03

#define SMP_MAX_MESSAGE (64 * 1024 * 1024)

static int smp_probe(const AVProbeData *p)
{
    /* First message of any session is HEADER (type 0x01) of fmt version 1.
     * Length prefix is reasonable (well under 64KB for plausible header). */
    if (p->buf_size < 6)
        return 0;
    if (p->buf[0] != 0 || p->buf[1] != 0)
        return 0;
    if (p->buf[4] != SMP_MSG_HEADER || p->buf[5] != SMP_FMT_VERSION)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int smp_read_message(AVFormatContext *s, uint8_t **out, int *out_len)
{
    AVIOContext *pb = s->pb;
    uint32_t len;
    uint8_t *buf;
    int ret;

    len = avio_rb32(pb);
    if (avio_feof(pb))
        return AVERROR_EOF;
    if (len == 0 || len > SMP_MAX_MESSAGE) {
        av_log(s, AV_LOG_ERROR, "smp: bad message length %u\n", len);
        return AVERROR_INVALIDDATA;
    }

    buf = av_malloc(len);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = avio_read(pb, buf, len);
    if (ret != (int)len) {
        av_free(buf);
        return ret < 0 ? ret : AVERROR_EOF;
    }

    *out = buf;
    *out_len = (int)len;
    return 0;
}

static int smp_read_header(AVFormatContext *s)
{
    uint8_t *msg = NULL;
    int msg_len = 0;
    int ret, i, off;
    uint8_t version, num_streams;

    if ((ret = smp_read_message(s, &msg, &msg_len)) < 0)
        return ret;

    if (msg_len < 3 || msg[0] != SMP_MSG_HEADER) {
        av_log(s, AV_LOG_ERROR, "smp: first message is not a header\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    version     = msg[1];
    num_streams = msg[2];
    off         = 3;

    if (version != SMP_FMT_VERSION) {
        av_log(s, AV_LOG_ERROR, "smp: unsupported version %d\n", version);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    for (i = 0; i < num_streams; i++) {
        AVStream *st;
        AVCodecParameters *par;
        uint32_t codec_id, tb_num, tb_den, width, height, sample_rate, extradata_size;
        uint8_t  codec_type, channels;

        if (off + 30 > msg_len) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        codec_id    = AV_RB32(msg + off); off += 4;
        codec_type  = msg[off];           off += 1;
        tb_num      = AV_RB32(msg + off); off += 4;
        tb_den      = AV_RB32(msg + off); off += 4;
        width       = AV_RB32(msg + off); off += 4;
        height      = AV_RB32(msg + off); off += 4;
        sample_rate = AV_RB32(msg + off); off += 4;
        channels    = msg[off];           off += 1;
        extradata_size = AV_RB32(msg + off); off += 4;

        if (extradata_size > (uint32_t)(msg_len - off)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        st = avformat_new_stream(s, NULL);
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        par = st->codecpar;
        par->codec_id    = (enum AVCodecID)codec_id;
        par->codec_type  = (enum AVMediaType)codec_type;
        par->width       = (int)width;
        par->height      = (int)height;
        par->sample_rate = (int)sample_rate;
        if (channels)
            av_channel_layout_default(&par->ch_layout, channels);

        if (tb_den)
            avpriv_set_pts_info(st, 64, (int)tb_num, (int)tb_den);

        if (extradata_size) {
            ret = ff_alloc_extradata(par, (int)extradata_size);
            if (ret < 0)
                goto end;
            memcpy(par->extradata, msg + off, extradata_size);
            off += extradata_size;
        }
    }

    /* SMP header carries full codec params — skip expensive probing and
     * disable avio-level buffering for lowest time-to-first-frame. */
    s->max_analyze_duration = 0;
    s->probesize            = 32;
    s->flags               |= AVFMT_FLAG_NOBUFFER;

    ret = 0;
end:
    av_free(msg);
    return ret;
}

static int smp_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    uint8_t *msg = NULL;
    int msg_len = 0;
    int ret;
    uint8_t type, stream_idx;
    int64_t pts, dts, duration;
    uint32_t data_size;
    int off;

    for (;;) {
        if ((ret = smp_read_message(s, &msg, &msg_len)) < 0)
            return ret;

        if (msg_len < 1) {
            av_free(msg);
            return AVERROR_INVALIDDATA;
        }

        type = msg[0];
        if (type == SMP_MSG_PACKET || type == SMP_MSG_KEY)
            break;

        /* Skip unknown / out-of-band messages (e.g. a re-sent header). */
        av_free(msg);
        msg = NULL;
    }

    if (msg_len < 30) {
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }

    off = 1;
    stream_idx = msg[off];                       off += 1;
    pts        = (int64_t)AV_RB64(msg + off);    off += 8;
    dts        = (int64_t)AV_RB64(msg + off);    off += 8;
    duration   = (int64_t)AV_RB64(msg + off);    off += 8;
    data_size  = AV_RB32(msg + off);             off += 4;

    if (data_size > (uint32_t)(msg_len - off)) {
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }
    if (stream_idx >= s->nb_streams) {
        av_free(msg);
        return AVERROR_INVALIDDATA;
    }

    if ((ret = av_new_packet(pkt, (int)data_size)) < 0) {
        av_free(msg);
        return ret;
    }

    memcpy(pkt->data, msg + off, data_size);
    pkt->stream_index = stream_idx;
    pkt->pts          = pts;
    pkt->dts          = dts;
    pkt->duration     = duration;
    if (type == SMP_MSG_KEY)
        pkt->flags |= AV_PKT_FLAG_KEY;

    av_free(msg);
    return 0;
}

const FFInputFormat ff_smp_demuxer = {
    .p.name      = "smp",
    .p.long_name = NULL_IF_CONFIG_SMALL("Simple Media Protocol"),
    .p.flags     = AVFMT_TS_DISCONT,
    .read_probe  = smp_probe,
    .read_header = smp_read_header,
    .read_packet = smp_read_packet,
};
