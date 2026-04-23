/*
 * This file is part of FFmpeg.
 *
 * This table was generated from the long and short names of AVCodecs
 * please see the respective codec sources for authorship
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

#include <stdlib.h>
#include <string.h>

#include "libavutil/internal.h"
#include "libavutil/macros.h"

#include "codec_id.h"
#include "codec_desc.h"
#include "profiles.h"

#define MT(...) (const char *const[]){ __VA_ARGS__, NULL }

static const AVCodecDescriptor codec_descriptors[] = {
    /* video codecs */
    {
        .id        = AV_CODEC_ID_MPEG1VIDEO,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "mpeg1video",
        .long_name = NULL_IF_CONFIG_SMALL("MPEG-1 video"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER |
                     // FIXME this is strigly speaking not true, as MPEG-1 does
                     // not allow field coding, but our mpeg12 code (decoder and
                     // parser) can sometimes change codec id at runtime, so
                     // this is safer
                     AV_CODEC_PROP_FIELDS,
    },
    {
        .id        = AV_CODEC_ID_MPEG2VIDEO,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "mpeg2video",
        .long_name = NULL_IF_CONFIG_SMALL("MPEG-2 video"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER |
                     AV_CODEC_PROP_FIELDS,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_mpeg2_video_profiles),
    },
    {
        .id        = AV_CODEC_ID_H263,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "h263",
        .long_name = NULL_IF_CONFIG_SMALL("H.263 / H.263-1996, H.263+ / H.263-1998 / H.263 version 2"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER,
    },
    {
        .id        = AV_CODEC_ID_MJPEG,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "mjpeg",
        .long_name = NULL_IF_CONFIG_SMALL("Motion JPEG"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
        .mime_types= MT("image/jpeg"),
        .profiles  = NULL_IF_CONFIG_SMALL(ff_mjpeg_profiles),
    },
    {
        .id        = AV_CODEC_ID_MJPEGB,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "mjpegb",
        .long_name = NULL_IF_CONFIG_SMALL("Apple MJPEG-B"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_MPEG4,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "mpeg4",
        .long_name = NULL_IF_CONFIG_SMALL("MPEG-4 part 2"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_mpeg4_video_profiles),
    },
    {
        .id        = AV_CODEC_ID_RAWVIDEO,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "rawvideo",
        .long_name = NULL_IF_CONFIG_SMALL("raw video"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_H263P,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "h263p",
        .long_name = NULL_IF_CONFIG_SMALL("H.263+ / H.263-1998 / H.263 version 2"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER,
    },
    {
        .id        = AV_CODEC_ID_H263I,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "h263i",
        .long_name = NULL_IF_CONFIG_SMALL("Intel H.263"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_REORDER,
    },
    {
        .id        = AV_CODEC_ID_H264,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "h264",
        .long_name = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_LOSSLESS |
                     AV_CODEC_PROP_REORDER | AV_CODEC_PROP_FIELDS,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_h264_profiles),
    },
    {
        .id        = AV_CODEC_ID_PNG,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "png",
        .long_name = NULL_IF_CONFIG_SMALL("PNG (Portable Network Graphics) image"),
        .props     = AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/png"),
    },
    {
        .id        = AV_CODEC_ID_BMP,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "bmp",
        .long_name = NULL_IF_CONFIG_SMALL("BMP (Windows and OS/2 bitmap)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/x-ms-bmp"),
    },
    {
        .id        = AV_CODEC_ID_TIFF,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "tiff",
        .long_name = NULL_IF_CONFIG_SMALL("TIFF image"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/tiff"),
    },
    {
        .id        = AV_CODEC_ID_GIF,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "gif",
        .long_name = NULL_IF_CONFIG_SMALL("CompuServe GIF (Graphics Interchange Format)"),
        .props     = AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/gif"),
    },
    {
        .id        = AV_CODEC_ID_VP8,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "vp8",
        .long_name = NULL_IF_CONFIG_SMALL("On2 VP8"),
        .props     = AV_CODEC_PROP_LOSSY,
    },
#if FF_API_V408_CODECID
#endif
    {
        .id        = AV_CODEC_ID_VP9,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "vp9",
        .long_name = NULL_IF_CONFIG_SMALL("Google VP9"),
        .props     = AV_CODEC_PROP_LOSSY,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_vp9_profiles),
    },
    {
        .id        = AV_CODEC_ID_WEBP,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "webp",
        .long_name = NULL_IF_CONFIG_SMALL("WebP"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY |
                     AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/webp"),
    },
    {
        .id        = AV_CODEC_ID_HEVC,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "hevc",
        .long_name = NULL_IF_CONFIG_SMALL("H.265 / HEVC (High Efficiency Video Coding)"),
        .props     = AV_CODEC_PROP_LOSSY | AV_CODEC_PROP_LOSSLESS | AV_CODEC_PROP_REORDER,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_hevc_profiles),
    },
#if FF_API_V408_CODECID
#endif
    {
        .id        = AV_CODEC_ID_APNG,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "apng",
        .long_name = NULL_IF_CONFIG_SMALL("APNG (Animated Portable Network Graphics) image"),
        .props     = AV_CODEC_PROP_LOSSLESS,
        .mime_types= MT("image/png"),
    },

    /* various PCM "codecs" */
    {
        .id        = AV_CODEC_ID_PCM_S16LE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s16le",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 16-bit little-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_S16BE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s16be",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 16-bit big-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_S8,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s8",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 8-bit"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_U8,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_u8",
        .long_name = NULL_IF_CONFIG_SMALL("PCM unsigned 8-bit"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_MULAW,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_mulaw",
        .long_name = NULL_IF_CONFIG_SMALL("PCM mu-law / G.711 mu-law"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_PCM_ALAW,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_alaw",
        .long_name = NULL_IF_CONFIG_SMALL("PCM A-law / G.711 A-law"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_PCM_S32LE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s32le",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 32-bit little-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_S32BE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s32be",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 32-bit big-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_S24LE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s24le",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 24-bit little-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_S24BE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_s24be",
        .long_name = NULL_IF_CONFIG_SMALL("PCM signed 24-bit big-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_F32BE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_f32be",
        .long_name = NULL_IF_CONFIG_SMALL("PCM 32-bit floating point big-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_F32LE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_f32le",
        .long_name = NULL_IF_CONFIG_SMALL("PCM 32-bit floating point little-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_F64BE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_f64be",
        .long_name = NULL_IF_CONFIG_SMALL("PCM 64-bit floating point big-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_PCM_F64LE,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "pcm_f64le",
        .long_name = NULL_IF_CONFIG_SMALL("PCM 64-bit floating point little-endian"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },

    /* various ADPCM codecs */

    /* AMR */

    /* RealAudio codecs*/

    /* various DPCM codecs */

    /* audio codecs */
    {
        .id        = AV_CODEC_ID_MP2,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "mp2",
        .long_name = NULL_IF_CONFIG_SMALL("MP2 (MPEG audio layer 2)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_MP3,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "mp3",
        .long_name = NULL_IF_CONFIG_SMALL("MP3 (MPEG audio layer 3)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_AAC,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "aac",
        .long_name = NULL_IF_CONFIG_SMALL("AAC (Advanced Audio Coding)"),
        .props     = AV_CODEC_PROP_LOSSY,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_aac_profiles),
    },
    {
        .id        = AV_CODEC_ID_VORBIS,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "vorbis",
        .long_name = NULL_IF_CONFIG_SMALL("Vorbis"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_FLAC,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "flac",
        .long_name = NULL_IF_CONFIG_SMALL("FLAC (Free Lossless Audio Codec)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_MP3ADU,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "mp3adu",
        .long_name = NULL_IF_CONFIG_SMALL("ADU (Application Data Unit) MP3 (MPEG audio layer 3)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_MP3ON4,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "mp3on4",
        .long_name = NULL_IF_CONFIG_SMALL("MP3onMP4"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_ALAC,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "alac",
        .long_name = NULL_IF_CONFIG_SMALL("ALAC (Apple Lossless Audio Codec)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSLESS,
    },
    {
        .id        = AV_CODEC_ID_MP1,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "mp1",
        .long_name = NULL_IF_CONFIG_SMALL("MP1 (MPEG audio layer 1)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },
    {
        .id        = AV_CODEC_ID_AAC_LATM,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "aac_latm",
        .long_name = NULL_IF_CONFIG_SMALL("AAC LATM (Advanced Audio Coding LATM syntax)"),
        .props     = AV_CODEC_PROP_LOSSY,
        .profiles  = NULL_IF_CONFIG_SMALL(ff_aac_profiles),
    },
    {
        .id        = AV_CODEC_ID_OPUS,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "opus",
        .long_name = NULL_IF_CONFIG_SMALL("Opus (Opus Interactive Audio Codec)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },

    /* subtitle codecs */
    {
        .id        = AV_CODEC_ID_TEXT,
        .type      = AVMEDIA_TYPE_SUBTITLE,
        .name      = "text",
        .long_name = NULL_IF_CONFIG_SMALL("raw UTF-8 text"),
        .props     = AV_CODEC_PROP_TEXT_SUB,
    },
    {
        .id        = AV_CODEC_ID_SSA,
        .type      = AVMEDIA_TYPE_SUBTITLE,
        .name      = "ssa",
        .long_name = NULL_IF_CONFIG_SMALL("SSA (SubStation Alpha) subtitle"),
        .props     = AV_CODEC_PROP_TEXT_SUB,
    },
    {
        .id        = AV_CODEC_ID_SUBRIP,
        .type      = AVMEDIA_TYPE_SUBTITLE,
        .name      = "subrip",
        .long_name = NULL_IF_CONFIG_SMALL("SubRip subtitle"),
        .props     = AV_CODEC_PROP_TEXT_SUB,
    },
    {
        .id        = AV_CODEC_ID_WEBVTT,
        .type      = AVMEDIA_TYPE_SUBTITLE,
        .name      = "webvtt",
        .long_name = NULL_IF_CONFIG_SMALL("WebVTT subtitle"),
        .props     = AV_CODEC_PROP_TEXT_SUB,
    },
    {
        .id        = AV_CODEC_ID_ASS,
        .type      = AVMEDIA_TYPE_SUBTITLE,
        .name      = "ass",
        .long_name = NULL_IF_CONFIG_SMALL("ASS (Advanced SSA) subtitle"),
        .props     = AV_CODEC_PROP_TEXT_SUB,
    },

    /* other kind of codecs and pseudo-codecs */
    {
        .id        = AV_CODEC_ID_WRAPPED_AVFRAME,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "wrapped_avframe",
        .long_name = NULL_IF_CONFIG_SMALL("AVFrame to AVPacket passthrough"),
        .props     = AV_CODEC_PROP_LOSSLESS,
    },
};

static int descriptor_compare(const void *key, const void *member)
{
    enum AVCodecID id = *(const enum AVCodecID *) key;
    const AVCodecDescriptor *desc = member;

    return id - desc->id;
}

const AVCodecDescriptor *avcodec_descriptor_get(enum AVCodecID id)
{
    return bsearch(&id, codec_descriptors, FF_ARRAY_ELEMS(codec_descriptors),
                   sizeof(codec_descriptors[0]), descriptor_compare);
}

const AVCodecDescriptor *avcodec_descriptor_next(const AVCodecDescriptor *prev)
{
    if (!prev)
        return &codec_descriptors[0];
    if (prev - codec_descriptors < FF_ARRAY_ELEMS(codec_descriptors) - 1)
        return prev + 1;
    return NULL;
}

const AVCodecDescriptor *avcodec_descriptor_get_by_name(const char *name)
{
    const AVCodecDescriptor *desc = NULL;

    while ((desc = avcodec_descriptor_next(desc)))
        if (!strcmp(desc->name, name))
            return desc;
    return NULL;
}

enum AVMediaType avcodec_get_type(enum AVCodecID codec_id)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec_id);
    return desc ? desc->type : AVMEDIA_TYPE_UNKNOWN;
}
