/*
 * Simple Media Protocol (SMP) - transport
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
 * SMP transport. After a tiny handshake the connection is a raw byte
 * pipe in both directions; all message framing is done by the smp
 * muxer/demuxer pair on top.
 *
 *   Client -> Server handshake (big-endian):
 *     4 bytes  magic   = "SMP0"
 *     1 byte   version = 2
 *     1 byte   mode    (0 = PULL / play, 1 = PUSH / publish)
 *     2 bytes  path length N
 *     N bytes  path (e.g. "/live/stream1")
 *     16 bytes session id (all-zero = anonymous; non-zero lets a publisher
 *              reconnect to the same path and pick up subscribers without
 *              losing the cached header / GOP)
 *
 *   Server -> Client response:
 *     4 bytes  magic   = "SMP0"
 *     1 byte   version = 2
 *     1 byte   status  (0 = OK, anything else = error)
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "url.h"

#define SMP_MAGIC     "SMP0"
#define SMP_VERSION   2
#define SMP_MODE_PULL 0
#define SMP_MODE_PUSH 1
#define SMP_SESSION_ID_LEN 16

typedef struct SMPContext {
    const AVClass *class;
    URLContext   *tcp_hd;
    int           rw_timeout;
    int           is_push;
    char         *resource;
    uint8_t      *session_id;
    int           session_id_len;
} SMPContext;

#define OFFSET(x) offsetof(SMPContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption smp_options[] = {
    { "timeout",    "Set timeout (in microseconds) of socket I/O operations", OFFSET(rw_timeout), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, .flags = D|E },
    { "session_id", "16-byte session id (32 hex chars) for publisher reconnect", OFFSET(session_id), AV_OPT_TYPE_BINARY, .flags = D|E },
    { NULL }
};

static const AVClass smp_class = {
    .class_name = "smp",
    .item_name  = av_default_item_name,
    .option     = smp_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int smp_close(URLContext *h)
{
    SMPContext *s = h->priv_data;
    ffurl_closep(&s->tcp_hd);
    av_freep(&s->resource);
    return 0;
}

static int smp_open(URLContext *h, const char *uri, int flags, AVDictionary **options)
{
    SMPContext *s = h->priv_data;
    char proto[16], hostname[256], path[1024], buf[1280];
    uint8_t hdr[8];
    uint8_t resp[6];
    size_t path_len;
    int port, ret;

    av_url_split(proto, sizeof(proto), NULL, 0,
                 hostname, sizeof(hostname), &port,
                 path, sizeof(path), uri);

    if (strcmp(proto, "smp"))
        return AVERROR(EINVAL);
    if (port <= 0)
        port = 7777;
    if (port >= 65536) {
        av_log(h, AV_LOG_ERROR, "Invalid port in uri\n");
        return AVERROR(EINVAL);
    }

    s->is_push  = !!(flags & AVIO_FLAG_WRITE);
    s->resource = av_strdup(path[0] ? path : "/");
    if (!s->resource)
        return AVERROR(ENOMEM);

    path_len = strlen(s->resource);
    if (path_len > UINT16_MAX) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ff_url_join(buf, sizeof(buf), "tcp", NULL, hostname, port, NULL);

    if (s->rw_timeout >= 0)
        av_dict_set_int(options, "timeout", s->rw_timeout, 0);
    av_dict_set(options, "tcp_nodelay", "1", 0);

    ret = ffurl_open_whitelist(&s->tcp_hd, buf, AVIO_FLAG_READ_WRITE,
                               &h->interrupt_callback, options,
                               h->protocol_whitelist, h->protocol_blacklist, h);
    if (ret < 0)
        goto fail;

    memcpy(hdr, SMP_MAGIC, 4);
    hdr[4] = SMP_VERSION;
    hdr[5] = s->is_push ? SMP_MODE_PUSH : SMP_MODE_PULL;
    AV_WB16(hdr + 6, (uint16_t)path_len);

    if ((ret = ffurl_write(s->tcp_hd, hdr, sizeof(hdr))) < 0)
        goto fail;
    if ((ret = ffurl_write(s->tcp_hd, (const uint8_t *)s->resource, path_len)) < 0)
        goto fail;

    {
        uint8_t sid[SMP_SESSION_ID_LEN] = { 0 };
        if (s->session_id && s->session_id_len > 0) {
            int n = FFMIN(s->session_id_len, SMP_SESSION_ID_LEN);
            memcpy(sid, s->session_id, n);
        }
        if ((ret = ffurl_write(s->tcp_hd, sid, SMP_SESSION_ID_LEN)) < 0)
            goto fail;
    }

    if ((ret = ffurl_read_complete(s->tcp_hd, resp, sizeof(resp))) < 0)
        goto fail;
    if (memcmp(resp, SMP_MAGIC, 4)) {
        av_log(h, AV_LOG_ERROR, "SMP bad magic in server reply\n");
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    if (resp[4] != SMP_VERSION) {
        av_log(h, AV_LOG_ERROR, "SMP server version %d unsupported\n", resp[4]);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    if (resp[5] != 0) {
        av_log(h, AV_LOG_ERROR, "SMP server rejected request (status %d)\n", resp[5]);
        ret = AVERROR(EACCES);
        goto fail;
    }

    h->is_streamed = 1;
    return 0;

fail:
    smp_close(h);
    return ret;
}

static int smp_read(URLContext *h, uint8_t *buf, int size)
{
    SMPContext *s = h->priv_data;
    return ffurl_read(s->tcp_hd, buf, size);
}

static int smp_write(URLContext *h, const uint8_t *buf, int size)
{
    SMPContext *s = h->priv_data;
    return ffurl_write(s->tcp_hd, buf, size);
}

static int smp_get_file_handle(URLContext *h)
{
    SMPContext *s = h->priv_data;
    return ffurl_get_file_handle(s->tcp_hd);
}

const URLProtocol ff_smp_protocol = {
    .name                = "smp",
    .url_open2           = smp_open,
    .url_read            = smp_read,
    .url_write           = smp_write,
    .url_close           = smp_close,
    .url_get_file_handle = smp_get_file_handle,
    .priv_data_size      = sizeof(SMPContext),
    .priv_data_class     = &smp_class,
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
