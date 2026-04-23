/*
 * ISO Media codec tags
 * Copyright (c) 2001 Fabrice Bellard
 * Copyright (c) 2002 Francois Revol <revol@free.fr>
 * Copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@free.fr>
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

#include "libavcodec/codec_id.h"
#include "avformat.h"
#include "internal.h"
#include "isom.h"

const AVCodecTag ff_codec_movvideo_tags[] = {
/*  { AV_CODEC_ID_, MKTAG('I', 'V', '5', '0') }, *//* Indeo 5.0 */

    { AV_CODEC_ID_RAWVIDEO, MKTAG('r', 'a', 'w', ' ') }, /* uncompressed RGB */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('y', 'u', 'v', '2') }, /* uncompressed YUV422 */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('2', 'v', 'u', 'y') }, /* uncompressed 8-bit 4:2:2 */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('y', 'u', 'v', 's') }, /* same as 2VUY but byte-swapped */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('v', '3', '0', '8') }, /* uncompressed  8-bit 4:4:4 */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('v', '4', '0', '8') }, /* uncompressed  8-bit 4:4:4:4 */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('v', '4', '1', '0') }, /* uncompressed 10-bit 4:4:4 */

    { AV_CODEC_ID_RAWVIDEO, MKTAG('L', '5', '5', '5') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('L', '5', '6', '5') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('B', '5', '6', '5') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('2', '4', 'B', 'G') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('B', 'G', 'R', 'A') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('R', 'G', 'B', 'A') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('A', 'B', 'G', 'R') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', '1', '6', 'g') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', '4', '8', 'r') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', '6', '4', 'a') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', 'x', 'b', 'g') }, /* BOXX */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', 'x', 'r', 'g') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('b', 'x', 'y', 'v') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('N', 'O', '1', '6') },
    { AV_CODEC_ID_RAWVIDEO, MKTAG('D', 'V', 'O', 'O') }, /* Digital Voodoo SD 8 Bit */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('R', '4', '2', '0') }, /* Radius DV YUV PAL */
    { AV_CODEC_ID_RAWVIDEO, MKTAG('R', '4', '1', '1') }, /* Radius DV YUV NTSC */

#if FF_API_V408_CODECID
#endif

    { AV_CODEC_ID_MJPEG,  MKTAG('j', 'p', 'e', 'g') }, /* PhotoJPEG */
    { AV_CODEC_ID_MJPEG,  MKTAG('m', 'j', 'p', 'a') }, /* Motion-JPEG (format A) */
    { AV_CODEC_ID_MJPEG,  MKTAG('A', 'V', 'D', 'J') }, /* MJPEG with alpha-channel (AVID JFIF meridien compressed) */
    { AV_CODEC_ID_MJPEG,  MKTAG('A', 'V', 'R', 'n') }, /* MJPEG with alpha-channel (AVID ABVB/Truevision NuVista) */
    { AV_CODEC_ID_MJPEG,  MKTAG('d', 'm', 'b', '1') }, /* Motion JPEG OpenDML */
    { AV_CODEC_ID_MJPEGB, MKTAG('m', 'j', 'p', 'b') }, /* Motion-JPEG (format B) */


    { AV_CODEC_ID_MPEG4, MKTAG('m', 'p', '4', 'v') },
    { AV_CODEC_ID_MPEG4, MKTAG('D', 'I', 'V', 'X') }, /* OpenDiVX *//* sample files at http://heroinewarrior.com/xmovie.php3 use this tag */
    { AV_CODEC_ID_MPEG4, MKTAG('X', 'V', 'I', 'D') },
    { AV_CODEC_ID_MPEG4, MKTAG('3', 'I', 'V', '2') }, /* experimental: 3IVX files before ivx D4 4.5.1 */

    { AV_CODEC_ID_H263, MKTAG('h', '2', '6', '3') }, /* H.263 */
    { AV_CODEC_ID_H263, MKTAG('s', '2', '6', '3') }, /* H.263 ?? works */



    { AV_CODEC_ID_RAWVIDEO, MKTAG('W', 'R', 'A', 'W') },


    { AV_CODEC_ID_HEVC, MKTAG('h', 'e', 'v', '1') }, /* HEVC/H.265 which indicates parameter sets may be in ES */
    { AV_CODEC_ID_HEVC, MKTAG('h', 'v', 'c', '1') }, /* HEVC/H.265 which indicates parameter sets shall not be in ES */
    { AV_CODEC_ID_HEVC, MKTAG('d', 'v', 'h', 'e') }, /* HEVC-based Dolby Vision derived from hev1 */
                                                     /* dvh1 is handled within mov.c */

    { AV_CODEC_ID_H264, MKTAG('a', 'v', 'c', '1') }, /* AVC-1/H.264 */
    { AV_CODEC_ID_H264, MKTAG('a', 'v', 'c', '2') },
    { AV_CODEC_ID_H264, MKTAG('a', 'v', 'c', '3') },
    { AV_CODEC_ID_H264, MKTAG('a', 'v', 'c', '4') },
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', 'p') }, /* AVC-Intra  50M 720p24/30/60 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', 'q') }, /* AVC-Intra  50M 720p25/50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', '2') }, /* AVC-Intra  50M 1080p25/50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', '3') }, /* AVC-Intra  50M 1080p24/30/60 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', '5') }, /* AVC-Intra  50M 1080i50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '5', '6') }, /* AVC-Intra  50M 1080i60 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', 'p') }, /* AVC-Intra 100M 720p24/30/60 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', 'q') }, /* AVC-Intra 100M 720p25/50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', '2') }, /* AVC-Intra 100M 1080p25/50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', '3') }, /* AVC-Intra 100M 1080p24/30/60 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', '5') }, /* AVC-Intra 100M 1080i50 */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', '1', '6') }, /* AVC-Intra 100M 1080i60 */
    { AV_CODEC_ID_H264, MKTAG('A', 'V', 'i', 'n') }, /* AVC-Intra with implicit SPS/PPS */
    { AV_CODEC_ID_H264, MKTAG('a', 'i', 'v', 'x') }, /* XAVC 10-bit 4:2:2 */
    { AV_CODEC_ID_H264, MKTAG('r', 'v', '6', '4') }, /* X-Com Radvision */
    { AV_CODEC_ID_H264, MKTAG('x', 'a', 'l', 'g') }, /* XAVC-L HD422 produced by FCP */
    { AV_CODEC_ID_H264, MKTAG('a', 'v', 'l', 'g') }, /* Panasonic P2 AVC-LongG */
    { AV_CODEC_ID_H264, MKTAG('d', 'v', 'a', '1') }, /* AVC-based Dolby Vision derived from avc1 */
    { AV_CODEC_ID_H264, MKTAG('d', 'v', 'a', 'v') }, /* AVC-based Dolby Vision derived from avc3 */


    { AV_CODEC_ID_VP8,  MKTAG('v', 'p', '0', '8') }, /* VP8 */
    { AV_CODEC_ID_VP9,  MKTAG('v', 'p', '0', '9') }, /* VP9 */

    { AV_CODEC_ID_MPEG1VIDEO, MKTAG('m', '1', 'v', ' ') },
    { AV_CODEC_ID_MPEG1VIDEO, MKTAG('m', '1', 'v', '1') }, /* Apple MPEG-1 Camcorder */
    { AV_CODEC_ID_MPEG1VIDEO, MKTAG('m', 'p', 'e', 'g') }, /* MPEG */
    { AV_CODEC_ID_MPEG1VIDEO, MKTAG('m', 'p', '1', 'v') }, /* CoreMedia CMVideoCodecType */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', '2', 'v', '1') }, /* Apple MPEG-2 Camcorder */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '1') }, /* MPEG-2 HDV 720p30 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '2') }, /* MPEG-2 HDV 1080i60 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '3') }, /* MPEG-2 HDV 1080i50 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '4') }, /* MPEG-2 HDV 720p24 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '5') }, /* MPEG-2 HDV 720p25 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '6') }, /* MPEG-2 HDV 1080p24 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '7') }, /* MPEG-2 HDV 1080p25 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '8') }, /* MPEG-2 HDV 1080p30 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', '9') }, /* MPEG-2 HDV 720p60 JVC */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('h', 'd', 'v', 'a') }, /* MPEG-2 HDV 720p50 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '5', 'n') }, /* MPEG-2 IMX NTSC 525/60 50mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '5', 'p') }, /* MPEG-2 IMX PAL 625/50 50mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '4', 'n') }, /* MPEG-2 IMX NTSC 525/60 40mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '4', 'p') }, /* MPEG-2 IMX PAL 625/50 40mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '3', 'n') }, /* MPEG-2 IMX NTSC 525/60 30mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'x', '3', 'p') }, /* MPEG-2 IMX PAL 625/50 30mb/s produced by FCP */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', '1') }, /* XDCAM HD422 720p30 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', '4') }, /* XDCAM HD422 720p24 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', '5') }, /* XDCAM HD422 720p25 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', '9') }, /* XDCAM HD422 720p60 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'a') }, /* XDCAM HD422 720p50 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'b') }, /* XDCAM HD422 1080i60 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'c') }, /* XDCAM HD422 1080i50 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'd') }, /* XDCAM HD422 1080p24 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'e') }, /* XDCAM HD422 1080p25 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', '5', 'f') }, /* XDCAM HD422 1080p30 CBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '1') }, /* XDCAM EX 720p30 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '2') }, /* XDCAM HD 1080i60 */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '3') }, /* XDCAM HD 1080i50 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '4') }, /* XDCAM EX 720p24 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '5') }, /* XDCAM EX 720p25 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '6') }, /* XDCAM HD 1080p24 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '7') }, /* XDCAM HD 1080p25 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '8') }, /* XDCAM HD 1080p30 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', '9') }, /* XDCAM EX 720p60 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'a') }, /* XDCAM EX 720p50 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'b') }, /* XDCAM EX 1080i60 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'c') }, /* XDCAM EX 1080i50 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'd') }, /* XDCAM EX 1080p24 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'e') }, /* XDCAM EX 1080p25 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'v', 'f') }, /* XDCAM EX 1080p30 VBR */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'h', 'd') }, /* XDCAM HD 540p */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('x', 'd', 'h', '2') }, /* XDCAM HD422 540p */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('A', 'V', 'm', 'p') }, /* AVID IMX PAL */
    { AV_CODEC_ID_MPEG2VIDEO, MKTAG('m', 'p', '2', 'v') }, /* FCP5 */


    { AV_CODEC_ID_TIFF,  MKTAG('t', 'i', 'f', 'f') }, /* TIFF embedded in MOV */
    { AV_CODEC_ID_GIF,   MKTAG('g', 'i', 'f', ' ') }, /* embedded gif files as frames (usually one "click to play movie" frame) */
    { AV_CODEC_ID_PNG,   MKTAG('p', 'n', 'g', ' ') },
    { AV_CODEC_ID_PNG,   MKTAG('M', 'N', 'G', ' ') },


    { AV_CODEC_ID_H263,      MKTAG('H', '2', '6', '3') },
    { AV_CODEC_ID_RAWVIDEO,  MKTAG('A', 'V', '1', 'x') }, /* AVID 1:1x */
    { AV_CODEC_ID_RAWVIDEO,  MKTAG('A', 'V', 'u', 'p') },











    { AV_CODEC_ID_RAWVIDEO, MKTAG('B', 'G', 'G', 'R') }, /* ASC Bayer BGGR */






    { AV_CODEC_ID_NONE, 0 },
};

const AVCodecTag ff_codec_movaudio_tags[] = {
    { AV_CODEC_ID_AAC,             MKTAG('m', 'p', '4', 'a') },
    { AV_CODEC_ID_ALAC,            MKTAG('a', 'l', 'a', 'c') },
    { AV_CODEC_ID_MP1,             MKTAG('.', 'm', 'p', '1') },
    { AV_CODEC_ID_MP2,             MKTAG('.', 'm', 'p', '2') },
    { AV_CODEC_ID_MP3,             MKTAG('.', 'm', 'p', '3') },
    { AV_CODEC_ID_MP3,             MKTAG('m', 'p', '3', ' ') }, /* vlc */
    { AV_CODEC_ID_MP3,             0x6D730055                },
    { AV_CODEC_ID_PCM_ALAW,        MKTAG('a', 'l', 'a', 'w') },
    { AV_CODEC_ID_PCM_F32BE,       MKTAG('f', 'l', '3', '2') },
    { AV_CODEC_ID_PCM_F32LE,       MKTAG('f', 'l', '3', '2') },
    { AV_CODEC_ID_PCM_F64BE,       MKTAG('f', 'l', '6', '4') },
    { AV_CODEC_ID_PCM_F64LE,       MKTAG('f', 'l', '6', '4') },
    { AV_CODEC_ID_PCM_MULAW,       MKTAG('u', 'l', 'a', 'w') },
    { AV_CODEC_ID_PCM_S16BE,       MKTAG('t', 'w', 'o', 's') },
    { AV_CODEC_ID_PCM_S16LE,       MKTAG('s', 'o', 'w', 't') },
    { AV_CODEC_ID_PCM_S16BE,       MKTAG('l', 'p', 'c', 'm') },
    { AV_CODEC_ID_PCM_S16LE,       MKTAG('l', 'p', 'c', 'm') },
    { AV_CODEC_ID_PCM_S24BE,       MKTAG('i', 'n', '2', '4') },
    { AV_CODEC_ID_PCM_S24LE,       MKTAG('i', 'n', '2', '4') },
    { AV_CODEC_ID_PCM_S24LE,       MKTAG('4', '2', 'n', 'i') },
    { AV_CODEC_ID_PCM_S32BE,       MKTAG('i', 'n', '3', '2') },
    { AV_CODEC_ID_PCM_S32LE,       MKTAG('i', 'n', '3', '2') },
    { AV_CODEC_ID_PCM_S32LE,       MKTAG('2', '3', 'n', 'i') },
    { AV_CODEC_ID_PCM_S8,          MKTAG('s', 'o', 'w', 't') },
    { AV_CODEC_ID_PCM_U8,          MKTAG('r', 'a', 'w', ' ') },
    { AV_CODEC_ID_PCM_U8,          MKTAG('N', 'O', 'N', 'E') },
    { AV_CODEC_ID_FLAC,            MKTAG('f', 'L', 'a', 'C') },
    { AV_CODEC_ID_OPUS,            MKTAG('O', 'p', 'u', 's') }, /* mp4ra.org */
    { AV_CODEC_ID_NONE, 0 },
};

const struct AVCodecTag *avformat_get_mov_video_tags(void)
{
    return ff_codec_movvideo_tags;
}

const struct AVCodecTag *avformat_get_mov_audio_tags(void)
{
    return ff_codec_movaudio_tags;
}
