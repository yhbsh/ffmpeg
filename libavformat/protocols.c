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

#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "url.h"

extern const URLProtocol ff_android_content_protocol;
extern const URLProtocol ff_crypto_protocol;
extern const URLProtocol ff_data_protocol;
extern const URLProtocol ff_fd_protocol;
extern const URLProtocol ff_ffrtmphttp_protocol;
extern const URLProtocol ff_file_protocol;
extern const URLProtocol ff_http_protocol;
extern const URLProtocol ff_httpproxy_protocol;
extern const URLProtocol ff_https_protocol;
extern const URLProtocol ff_pipe_protocol;
extern const URLProtocol ff_rtp_protocol;
extern const URLProtocol ff_smp_protocol;
extern const URLProtocol ff_tcp_protocol;
extern const URLProtocol ff_tls_protocol;
extern const URLProtocol ff_dtls_protocol;
extern const URLProtocol ff_udp_protocol;
extern const URLProtocol ff_udplite_protocol;
extern const URLProtocol ff_unix_protocol;
extern const URLProtocol ff_srt_protocol;

#include "libavformat/protocol_list.c"

const AVClass *ff_urlcontext_child_class_iterate(void **iter)
{
    const AVClass *ret = NULL;
    uintptr_t i;

    for (i = (uintptr_t)*iter; url_protocols[i]; i++) {
        ret = url_protocols[i]->priv_data_class;
        if (ret)
            break;
    }

    *iter = (void*)(uintptr_t)(url_protocols[i] ? i + 1 : i);
    return ret;
}

const char *avio_enum_protocols(void **opaque, int output)
{
    uintptr_t i;

    for (i = (uintptr_t)*opaque; url_protocols[i]; i++) {
        const URLProtocol *p = url_protocols[i];
        if ((output && p->url_write) || (!output && p->url_read)) {
            *opaque = (void*)(uintptr_t)(i + 1);
            return p->name;
        }
    }
    *opaque = NULL;
    return NULL;
}

const AVClass *avio_protocol_get_class(const char *name)
{
    int i = 0;
    for (i = 0; url_protocols[i]; i++) {
        if (!strcmp(url_protocols[i]->name, name))
            return url_protocols[i]->priv_data_class;
    }
    return NULL;
}

const URLProtocol **ffurl_get_protocols(const char *whitelist,
                                        const char *blacklist)
{
    const URLProtocol **ret;
    int i, ret_idx = 0;

    ret = av_calloc(FF_ARRAY_ELEMS(url_protocols), sizeof(*ret));
    if (!ret)
        return NULL;

    for (i = 0; url_protocols[i]; i++) {
        const URLProtocol *up = url_protocols[i];

        if (whitelist && *whitelist && !av_match_name(up->name, whitelist))
            continue;
        if (blacklist && *blacklist && av_match_name(up->name, blacklist))
            continue;

        ret[ret_idx++] = up;
    }

    return ret;
}
