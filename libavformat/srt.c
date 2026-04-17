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

/**
 * @file
 * Secure Reliable Transport (SRT) protocol — native srt.h implementation.
 */

#include <fcntl.h>
#include <unistd.h>

#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"

#define SRT_IMPLEMENTATION
#include "srt.h"

/* Default SRT live payload size (8 × 188-byte MPEG-TS packets). */
#define FF_SRT_DEFAULT_PAYLOAD  1316
#define FF_SRT_MAX_PAYLOAD      1456

enum FFSRTMode {
    FF_SRT_MODE_CALLER     = 0,
    FF_SRT_MODE_LISTENER   = 1,
    FF_SRT_MODE_RENDEZVOUS = 2,
};

typedef struct FFSRTContext {
    const AVClass *class;

    int             udp_fd;
    srt_context    *ctx;
    srt_socket     *sock;   /* caller / rendezvous / listening socket */
    srt_socket     *peer;   /* accepted child, once CONNECTED (listener mode) */

    struct sockaddr_storage remote;
    socklen_t               remote_len;
    struct sockaddr_storage local;
    socklen_t               local_len;

    /* ---- AVOptions ---- */
    int64_t rw_timeout;
    int64_t listen_timeout;
    int     recv_buffer_size;
    int     send_buffer_size;
    int     payload_size;
    int     mode;           /* FFSRTMode */
    int64_t latency;        /* µs, applied to both snd + rcv if specific ones unset */
    int64_t rcvlatency;
    int64_t peerlatency;
    char   *passphrase;
    int     pbkeylen;
    char   *streamid;
    int     tlpktdrop;
    int     nakreport;
    int64_t maxbw;
    int64_t connect_timeout;
} FFSRTContext;

#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
#define OFFSET(x) offsetof(FFSRTContext, x)

static const AVOption ff_srt_options[] = {
    { "timeout",          "I/O timeout (µs); -1 = blocking",                    OFFSET(rw_timeout),        AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "listen_timeout",   "Accept/connect timeout (µs); -1 = forever",          OFFSET(listen_timeout),    AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "connect_timeout",  "Caller connect timeout (ms); -1 = default",          OFFSET(connect_timeout),   AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "recv_buffer_size", "UDP recv buffer (bytes)",                            OFFSET(recv_buffer_size),  AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,   .flags = D|E },
    { "send_buffer_size", "UDP send buffer (bytes)",                            OFFSET(send_buffer_size),  AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,   .flags = D|E },
    { "pkt_size",         "SRT payload size (alias: payload_size)",             OFFSET(payload_size),      AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, FF_SRT_MAX_PAYLOAD, .flags = D|E },
    { "payload_size",     "SRT payload size",                                   OFFSET(payload_size),      AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, FF_SRT_MAX_PAYLOAD, .flags = D|E },
    { "mode",             "Connection mode",                                    OFFSET(mode),              AV_OPT_TYPE_INT,    { .i64 = FF_SRT_MODE_CALLER }, FF_SRT_MODE_CALLER, FF_SRT_MODE_RENDEZVOUS, .flags = D|E, .unit = "mode" },
    {   "caller",         NULL,                                                 0,                         AV_OPT_TYPE_CONST,  { .i64 = FF_SRT_MODE_CALLER },     INT_MIN, INT_MAX, .flags = D|E, .unit = "mode" },
    {   "listener",       NULL,                                                 0,                         AV_OPT_TYPE_CONST,  { .i64 = FF_SRT_MODE_LISTENER },   INT_MIN, INT_MAX, .flags = D|E, .unit = "mode" },
    {   "rendezvous",     NULL,                                                 0,                         AV_OPT_TYPE_CONST,  { .i64 = FF_SRT_MODE_RENDEZVOUS }, INT_MIN, INT_MAX, .flags = D|E, .unit = "mode" },
    { "latency",          "TSBPD latency (µs) applied to both directions",      OFFSET(latency),           AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "rcvlatency",       "Receiver-side TSBPD latency (µs)",                   OFFSET(rcvlatency),        AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "peerlatency",      "Peer-side TSBPD latency (µs)",                       OFFSET(peerlatency),       AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "passphrase",       "Crypto passphrase (10..79 chars)",                   OFFSET(passphrase),        AV_OPT_TYPE_STRING, { .str = NULL },              .flags = D|E },
    { "pbkeylen",         "Crypto key length {16,24,32}",                       OFFSET(pbkeylen),          AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 32,        .flags = D|E },
    { "streamid",         "Stream ID extension string",                         OFFSET(streamid),          AV_OPT_TYPE_STRING, { .str = NULL },              .flags = D|E },
    { "tlpktdrop",        "Too-late packet drop",                               OFFSET(tlpktdrop),         AV_OPT_TYPE_BOOL,   { .i64 = -1 }, -1, 1,         .flags = D|E },
    { "nakreport",        "Periodic NAK reports",                               OFFSET(nakreport),         AV_OPT_TYPE_BOOL,   { .i64 = -1 }, -1, 1,         .flags = D|E },
    { "maxbw",            "Maximum bandwidth cap (bytes/sec)",                  OFFSET(maxbw),             AV_OPT_TYPE_INT64,  { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { NULL }
};

#undef D
#undef E
#undef OFFSET

/* ------------------------------------------------------------------ */

static uint64_t ff_srt_now_us(void) {
    return (uint64_t)av_gettime_relative();
}

/* Drain incoming UDP into the srt_context and drain the context's tx ring
 * back to the wire. Called before every read/write step so the state
 * machine keeps up with arrivals and emissions. */
static void ff_srt_pump(FFSRTContext *s, uint64_t now_us) {
    uint8_t buf[1600];
    /* recv → feed */
    for (;;) {
        struct sockaddr_storage from; socklen_t flen = sizeof from;
        ssize_t r = recvfrom(s->udp_fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&from, &flen);
        if (r < 0) break;
        srt_context_feed_udp(s->ctx, buf, (size_t)r,
                             (struct sockaddr *)&from, flen, now_us);
    }
    /* pull → sendto */
    for (;;) {
        struct sockaddr_storage to; socklen_t tlen = sizeof to;
        int n = srt_context_pull_udp(s->ctx, buf, sizeof buf,
                                     (struct sockaddr *)&to, &tlen, now_us);
        if (n == SRT_ERR_AGAIN) break;
        if (n > 0) {
            sendto(s->udp_fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&to, tlen);
        }
    }
    srt_tick(s->sock, now_us);
    if (s->peer) srt_tick(s->peer, now_us);
}

/* Return the "data socket" for read/write once HS is done.
 *   caller / rendezvous: s->sock becomes CONNECTED.
 *   listener:            s->peer (the accepted child) becomes CONNECTED. */
static srt_socket *ff_srt_data_socket(FFSRTContext *s) {
    return s->peer ? s->peer : s->sock;
}

static void ff_srt_apply_options(FFSRTContext *s, srt_sockopts *opt) {
    memset(opt, 0, sizeof *opt);

    opt->caller     = (s->mode == FF_SRT_MODE_CALLER);
    opt->rendezvous = (s->mode == FF_SRT_MODE_RENDEZVOUS);
    opt->tsbpd      = 1;

    /* FFmpeg uses microseconds, our API uses milliseconds. */
    int64_t base_us = (s->latency >= 0)     ? s->latency     : 120000;
    int64_t rcv_us  = (s->rcvlatency >= 0)  ? s->rcvlatency  : base_us;
    int64_t snd_us  = (s->peerlatency >= 0) ? s->peerlatency : base_us;
    opt->rcv_tsbpd_ms = (uint16_t)(rcv_us / 1000);
    opt->snd_tsbpd_ms = (uint16_t)(snd_us / 1000);

    opt->tlpktdrop    = (s->tlpktdrop != 0);
    opt->periodic_nak = (s->nakreport != 0);
    opt->mtu          = s->payload_size > 0
                          ? (uint32_t)(s->payload_size + 16)  /* +SRT header */
                          : SRT_DEFAULT_MTU;
    opt->flow_window  = SRT_DEFAULT_FLOW_WINDOW;
    opt->max_bw_bps   = (s->maxbw > 0) ? (uint64_t)s->maxbw : 0;

    if (s->passphrase && s->passphrase[0]) {
        opt->passphrase = s->passphrase;
        switch (s->pbkeylen) {
            case 24: opt->encryption = SRT_ENC_AES_192; break;
            case 32: opt->encryption = SRT_ENC_AES_256; break;
            default: opt->encryption = SRT_ENC_AES_128; break;
        }
        opt->cipher = SRT_CIPHER_AES_CTR;
    }
    if (s->streamid && s->streamid[0]) opt->stream_id = s->streamid;
}

/* ------------------------------------------------------------------ */

static int ff_srt_url_parse(URLContext *h, const char *uri,
                            char *host, size_t hlen, int *port)
{
    char proto[16], path[1024];
    av_url_split(proto, sizeof proto, NULL, 0, host, (int)hlen,
                 port, path, sizeof path, uri);
    if (strcmp(proto, "srt") != 0) return AVERROR(EINVAL);
    if (*port <= 0 || *port >= 65536) {
        av_log(h, AV_LOG_ERROR, "srt: missing or invalid port in uri '%s'\n", uri);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int ff_srt_open(URLContext *h, const char *uri, int flags)
{
    FFSRTContext *s = h->priv_data;
    char host[1024] = {0};
    int  port = 0;
    int  ret;
    const char *q;

    /* Parse query options before reading the address. */
    q = strchr(uri, '?');
    if (q) {
        int qr = ff_parse_opts_from_query_string(s, q, 0);
        if (qr < 0) return qr;
    }

    if ((ret = ff_srt_url_parse(h, uri, host, sizeof host, &port)) < 0)
        return ret;

    /* Resolve remote address. */
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (s->mode == FF_SRT_MODE_LISTENER) hints.ai_flags |= AI_PASSIVE;
    char pstr[16]; snprintf(pstr, sizeof pstr, "%d", port);
    if ((ret = getaddrinfo(host[0] ? host : NULL, pstr, &hints, &ai)) != 0) {
        av_log(h, AV_LOG_ERROR, "srt: getaddrinfo '%s': %s\n",
               host, gai_strerror(ret));
        return AVERROR(EIO);
    }
    memcpy(&s->remote, ai->ai_addr, ai->ai_addrlen);
    s->remote_len = ai->ai_addrlen;
    freeaddrinfo(ai);

    /* Create UDP socket. */
    s->udp_fd = socket(s->remote.ss_family, SOCK_DGRAM, 0);
    if (s->udp_fd < 0) return AVERROR(errno);

    int flg = fcntl(s->udp_fd, F_GETFL, 0);
    fcntl(s->udp_fd, F_SETFL, flg | O_NONBLOCK);
    int reuse = 1;
    setsockopt(s->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (s->recv_buffer_size > 0)
        setsockopt(s->udp_fd, SOL_SOCKET, SO_RCVBUF,
                   &s->recv_buffer_size, sizeof s->recv_buffer_size);
    if (s->send_buffer_size > 0)
        setsockopt(s->udp_fd, SOL_SOCKET, SO_SNDBUF,
                   &s->send_buffer_size, sizeof s->send_buffer_size);

    /* For caller mode we don't need to bind — the OS picks an ephemeral
     * port. For listener / rendezvous we bind to the URL-supplied addr. */
    if (s->mode != FF_SRT_MODE_CALLER) {
        if (bind(s->udp_fd, (struct sockaddr *)&s->remote, s->remote_len) < 0) {
            ret = AVERROR(errno);
            goto fail;
        }
    }

    /* Set up srt.h context + socket. */
    s->ctx = srt_create(NULL);
    if (!s->ctx) { ret = AVERROR(ENOMEM); goto fail; }
    srt_sockopts opt;
    ff_srt_apply_options(s, &opt);
    s->sock = srt_socket_new(s->ctx, &opt);
    if (!s->sock) { ret = AVERROR(ENOMEM); goto fail; }

    switch (s->mode) {
    case FF_SRT_MODE_CALLER:
        ret = srt_connect(s->sock, (struct sockaddr *)&s->remote, s->remote_len);
        break;
    case FF_SRT_MODE_LISTENER:
        ret = srt_listen(s->sock, (struct sockaddr *)&s->remote, s->remote_len);
        break;
    case FF_SRT_MODE_RENDEZVOUS: {
        /* For rendezvous we need a local address — ephemeral from the bind
         * above. Read it back via getsockname. */
        socklen_t llen = sizeof s->local;
        getsockname(s->udp_fd, (struct sockaddr *)&s->local, &llen);
        s->local_len = llen;
        ret = srt_rendezvous(s->sock, (struct sockaddr *)&s->local,
                             (struct sockaddr *)&s->remote, s->remote_len);
        break;
    }
    default:
        ret = AVERROR(EINVAL);
    }
    if (ret != SRT_OK) { ret = AVERROR(EIO); goto fail; }

    /* Drive HS to CONNECTED (or timeout). */
    uint64_t deadline = ff_srt_now_us()
                      + ((s->listen_timeout > 0) ? (uint64_t)s->listen_timeout
                                                 : 10ULL * 1000 * 1000);
    while (ff_srt_now_us() < deadline) {
        ff_srt_pump(s, ff_srt_now_us());
        if (s->mode == FF_SRT_MODE_LISTENER) {
            if (!s->peer && srt_accept(s->sock, &s->peer) == SRT_OK)
                ; /* peer now CONNECTED */
            if (s->peer && srt_get_state(s->peer) == SRT_STATE_CONNECTED)
                break;
        } else {
            if (srt_get_state(s->sock) == SRT_STATE_CONNECTED) break;
            if (srt_get_state(s->sock) == SRT_STATE_BROKEN) {
                ret = AVERROR(ECONNREFUSED); goto fail;
            }
        }
        if (ff_check_interrupt(&h->interrupt_callback)) {
            ret = AVERROR_EXIT; goto fail;
        }
        av_usleep(2000);
    }

    {
        srt_socket *ds = ff_srt_data_socket(s);
        if (!ds || srt_get_state(ds) != SRT_STATE_CONNECTED) {
            av_log(h, AV_LOG_ERROR, "srt: handshake timed out\n");
            ret = AVERROR(ETIMEDOUT); goto fail;
        }
    }

    h->is_streamed    = 1;
    h->max_packet_size = (s->payload_size > 0) ? s->payload_size
                                               : FF_SRT_DEFAULT_PAYLOAD;
    if (s->rw_timeout >= 0) h->rw_timeout = s->rw_timeout;
    return 0;

fail:
    if (s->sock) { srt_close(s->sock); s->sock = NULL; }
    if (s->ctx)  { srt_destroy(s->ctx); s->ctx = NULL; }
    if (s->udp_fd >= 0) { close(s->udp_fd); s->udp_fd = -1; }
    return ret;
}

/* ------------------------------------------------------------------ */

static int ff_srt_read(URLContext *h, uint8_t *buf, int size)
{
    FFSRTContext *s = h->priv_data;
    srt_socket *ds = ff_srt_data_socket(s);
    if (!ds) return AVERROR(EIO);

    uint64_t deadline = (h->rw_timeout > 0)
                         ? ff_srt_now_us() + (uint64_t)h->rw_timeout
                         : UINT64_MAX;

    for (;;) {
        ff_srt_pump(s, ff_srt_now_us());

        srt_msg m = {0};
        int r = srt_recv(ds, buf, (size_t)size, &m);
        if (r > 0) return r;
        if (srt_get_state(ds) == SRT_STATE_CLOSED ||
            srt_get_state(ds) == SRT_STATE_BROKEN) return AVERROR_EOF;

        if (h->flags & AVIO_FLAG_NONBLOCK) return AVERROR(EAGAIN);
        if (ff_check_interrupt(&h->interrupt_callback)) return AVERROR_EXIT;
        if (ff_srt_now_us() >= deadline) return AVERROR(ETIMEDOUT);
        av_usleep(1000);
    }
}

static int ff_srt_write(URLContext *h, const uint8_t *buf, int size)
{
    FFSRTContext *s = h->priv_data;
    srt_socket *ds = ff_srt_data_socket(s);
    if (!ds) return AVERROR(EIO);

    uint64_t deadline = (h->rw_timeout > 0)
                         ? ff_srt_now_us() + (uint64_t)h->rw_timeout
                         : UINT64_MAX;

    for (;;) {
        ff_srt_pump(s, ff_srt_now_us());

        int r = srt_send(ds, buf, (size_t)size, NULL);
        if (r > 0) { ff_srt_pump(s, ff_srt_now_us()); return r; }
        if (r != SRT_ERR_AGAIN) {
            if (srt_get_state(ds) == SRT_STATE_CLOSED ||
                srt_get_state(ds) == SRT_STATE_BROKEN) return AVERROR_EOF;
            return AVERROR(EIO);
        }
        if (h->flags & AVIO_FLAG_NONBLOCK) return AVERROR(EAGAIN);
        if (ff_check_interrupt(&h->interrupt_callback)) return AVERROR_EXIT;
        if (ff_srt_now_us() >= deadline) return AVERROR(ETIMEDOUT);
        av_usleep(1000);
    }
}

static int ff_srt_close(URLContext *h)
{
    FFSRTContext *s = h->priv_data;
    if (s->peer) { srt_shutdown(s->peer); ff_srt_pump(s, ff_srt_now_us()); srt_close(s->peer); }
    if (s->sock) { srt_shutdown(s->sock); ff_srt_pump(s, ff_srt_now_us()); srt_close(s->sock); }
    if (s->ctx)  { srt_destroy(s->ctx); }
    if (s->udp_fd >= 0) close(s->udp_fd);
    return 0;
}

static int ff_srt_get_file_handle(URLContext *h)
{
    FFSRTContext *s = h->priv_data;
    return s->udp_fd;
}

/* ------------------------------------------------------------------ */

static const AVClass ff_srt_class = {
    .class_name = "srt",
    .item_name  = av_default_item_name,
    .option     = ff_srt_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_srt_protocol = {
    .name                = "srt",
    .url_open            = ff_srt_open,
    .url_read            = ff_srt_read,
    .url_write           = ff_srt_write,
    .url_close           = ff_srt_close,
    .url_get_file_handle = ff_srt_get_file_handle,
    .priv_data_size      = sizeof(FFSRTContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &ff_srt_class,
};
