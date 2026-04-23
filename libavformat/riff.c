/*
 * RIFF common functions and data
 * Copyright (c) 2000 Fabrice Bellard
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

#include <stddef.h>
#include "config.h"
#include "config_components.h"
#include "libavutil/macros.h"
#include "avformat.h"
#include "internal.h"
#include "metadata.h"
#include "riff.h"

/* Note: When encoding, the first matching tag is used, so order is
 * important if multiple tags are possible for a given codec.
 * Note also that this list is used for more than just riff, other
 * files use it as well.
 */
const AVCodecTag ff_codec_bmp_tags[] = {
    { AV_CODEC_ID_H264,         MKTAG('H', '2', '6', '4') },
    { AV_CODEC_ID_H264,         MKTAG('h', '2', '6', '4') },
    { AV_CODEC_ID_H264,         MKTAG('X', '2', '6', '4') },
    { AV_CODEC_ID_H264,         MKTAG('x', '2', '6', '4') },
    { AV_CODEC_ID_H264,         MKTAG('a', 'v', 'c', '1') },
    { AV_CODEC_ID_H264,         MKTAG('D', 'A', 'V', 'C') },
    { AV_CODEC_ID_H264,         MKTAG('S', 'M', 'V', '2') },
    { AV_CODEC_ID_H264,         MKTAG('V', 'S', 'S', 'H') },
    { AV_CODEC_ID_H264,         MKTAG('Q', '2', '6', '4') }, /* QNAP surveillance system */
    { AV_CODEC_ID_H264,         MKTAG('V', '2', '6', '4') }, /* CCTV recordings */
    { AV_CODEC_ID_H264,         MKTAG('G', 'A', 'V', 'C') }, /* GeoVision camera */
    { AV_CODEC_ID_H264,         MKTAG('U', 'M', 'S', 'V') },
    { AV_CODEC_ID_H264,         MKTAG('t', 's', 'h', 'd') },
    { AV_CODEC_ID_H264,         MKTAG('I', 'N', 'M', 'C') },
    { AV_CODEC_ID_H263,         MKTAG('H', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('X', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('T', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('L', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('V', 'X', '1', 'K') },
    { AV_CODEC_ID_H263,         MKTAG('Z', 'y', 'G', 'o') },
    { AV_CODEC_ID_H263,         MKTAG('M', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('l', 's', 'v', 'm') },
    { AV_CODEC_ID_H263P,        MKTAG('H', '2', '6', '3') },
    { AV_CODEC_ID_H263I,        MKTAG('I', '2', '6', '3') }, /* Intel H.263 */
    { AV_CODEC_ID_H263,         MKTAG('U', '2', '6', '3') },
    { AV_CODEC_ID_H263,         MKTAG('V', 'S', 'M', '4') }, /* needs -vf il=l=i:c=i */
    { AV_CODEC_ID_MPEG4,        MKTAG('F', 'M', 'P', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'I', 'V', 'X') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'X', '5', '0') },
    { AV_CODEC_ID_MPEG4,        MKTAG('X', 'V', 'I', 'D') },
    { AV_CODEC_ID_MPEG4,        MKTAG('M', 'P', '4', 'S') },
    { AV_CODEC_ID_MPEG4,        MKTAG('M', '4', 'S', '2') },
    /* some broken AVIs use this */
    { AV_CODEC_ID_MPEG4,        MKTAG( 4 ,  0 ,  0 ,  0 ) },
    /* some broken AVIs use this */
    { AV_CODEC_ID_MPEG4,        MKTAG('Z', 'M', 'P', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'I', 'V', '1') },
    { AV_CODEC_ID_MPEG4,        MKTAG('B', 'L', 'Z', '0') },
    { AV_CODEC_ID_MPEG4,        MKTAG('m', 'p', '4', 'v') },
    { AV_CODEC_ID_MPEG4,        MKTAG('U', 'M', 'P', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('W', 'V', '1', 'F') },
    { AV_CODEC_ID_MPEG4,        MKTAG('S', 'E', 'D', 'G') },
    { AV_CODEC_ID_MPEG4,        MKTAG('R', 'M', 'P', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('3', 'I', 'V', '2') },
    /* WaWv MPEG-4 Video Codec */
    { AV_CODEC_ID_MPEG4,        MKTAG('W', 'A', 'W', 'V') },
    { AV_CODEC_ID_MPEG4,        MKTAG('F', 'F', 'D', 'S') },
    { AV_CODEC_ID_MPEG4,        MKTAG('F', 'V', 'F', 'W') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'C', 'O', 'D') },
    { AV_CODEC_ID_MPEG4,        MKTAG('M', 'V', 'X', 'M') },
    { AV_CODEC_ID_MPEG4,        MKTAG('P', 'M', '4', 'V') },
    { AV_CODEC_ID_MPEG4,        MKTAG('S', 'M', 'P', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'X', 'G', 'M') },
    { AV_CODEC_ID_MPEG4,        MKTAG('V', 'I', 'D', 'M') },
    { AV_CODEC_ID_MPEG4,        MKTAG('M', '4', 'T', '3') },
    { AV_CODEC_ID_MPEG4,        MKTAG('G', 'E', 'O', 'X') },
    /* flipped video */
    { AV_CODEC_ID_MPEG4,        MKTAG('G', '2', '6', '4') },
    /* flipped video */
    { AV_CODEC_ID_MPEG4,        MKTAG('H', 'D', 'X', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'M', '4', 'V') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'M', 'K', '2') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'Y', 'M', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'I', 'G', 'I') },
    /* Ephv MPEG-4 */
    { AV_CODEC_ID_MPEG4,        MKTAG('E', 'P', 'H', 'V') },
    { AV_CODEC_ID_MPEG4,        MKTAG('E', 'M', '4', 'A') },
    /* Divio MPEG-4 */
    { AV_CODEC_ID_MPEG4,        MKTAG('M', '4', 'C', 'C') },
    { AV_CODEC_ID_MPEG4,        MKTAG('S', 'N', '4', '0') },
    { AV_CODEC_ID_MPEG4,        MKTAG('V', 'S', 'P', 'X') },
    { AV_CODEC_ID_MPEG4,        MKTAG('U', 'L', 'D', 'X') },
    { AV_CODEC_ID_MPEG4,        MKTAG('G', 'E', 'O', 'V') },
    /* Samsung SHR-6040 */
    { AV_CODEC_ID_MPEG4,        MKTAG('S', 'I', 'P', 'P') },
    { AV_CODEC_ID_MPEG4,        MKTAG('S', 'M', '4', 'V') },
    { AV_CODEC_ID_MPEG4,        MKTAG('X', 'V', 'I', 'X') },
    { AV_CODEC_ID_MPEG4,        MKTAG('D', 'r', 'e', 'X') },
    { AV_CODEC_ID_MPEG4,        MKTAG('Q', 'M', 'P', '4') }, /* QNAP Systems */
    { AV_CODEC_ID_MPEG4,        MKTAG('P', 'L', 'V', '1') }, /* Pelco DVR MPEG-4 */
    { AV_CODEC_ID_MPEG4,        MKTAG('G', 'L', 'V', '4') },
    { AV_CODEC_ID_MPEG4,        MKTAG('G', 'M', 'P', '4') }, /* GeoVision camera */
    { AV_CODEC_ID_MPEG4,        MKTAG('M', 'N', 'M', '4') }, /* March Networks DVR */
    { AV_CODEC_ID_MPEG4,        MKTAG('G', 'T', 'M', '4') }, /* Telefactor */
    /* Canopus DV */
    /* Canopus DV */
    /* Canopus DV */
    { AV_CODEC_ID_MPEG1VIDEO,   MKTAG('m', 'p', 'g', '1') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('m', 'p', 'g', '2') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', 'P', 'E', 'G') },
    { AV_CODEC_ID_MPEG1VIDEO,   MKTAG('P', 'I', 'M', '1') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('P', 'I', 'M', '2') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('V', 'C', 'R', '2') },
    { AV_CODEC_ID_MPEG1VIDEO,   MKTAG( 1 ,  0 ,  0 ,  16) },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG( 2 ,  0 ,  0 ,  16) },
    { AV_CODEC_ID_MPEG4,        MKTAG( 4 ,  0 ,  0 ,  16) },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('D', 'V', 'R', ' ') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', 'M', 'E', 'S') },
    /* Lead MPEG-2 in AVI */
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('L', 'M', 'P', '2') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('s', 'l', 'i', 'f') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('E', 'M', '2', 'V') },
    /* Matrox MPEG-2 intra-only */
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', '7', '0', '1') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', '7', '0', '2') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', '7', '0', '3') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', '7', '0', '4') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('M', '7', '0', '5') },
    { AV_CODEC_ID_MPEG2VIDEO,   MKTAG('m', 'p', 'g', 'v') },
    { AV_CODEC_ID_MPEG1VIDEO,   MKTAG('B', 'W', '1', '0') },
    { AV_CODEC_ID_MPEG1VIDEO,   MKTAG('X', 'M', 'P', 'G') }, /* Xing MPEG intra only */
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'J', 'P', 'G') },
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'S', 'C', '2') }, /* Multiscope II */
    { AV_CODEC_ID_MJPEG,        MKTAG('L', 'J', 'P', 'G') },
    { AV_CODEC_ID_MJPEG,        MKTAG('d', 'm', 'b', '1') },
    { AV_CODEC_ID_MJPEG,        MKTAG('m', 'j', 'p', 'a') },
    { AV_CODEC_ID_MJPEG,        MKTAG('J', 'R', '2', '4') }, /* Quadrox Mjpeg */
    /* Pegasus lossless JPEG */
    { AV_CODEC_ID_MJPEG,        MKTAG('J', 'P', 'G', 'L') },
    /* JPEG-LS custom FOURCC for AVI - encoder */
    /* JPEG-LS custom FOURCC for AVI - decoder */
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'J', 'L', 'S') },
    { AV_CODEC_ID_MJPEG,        MKTAG('j', 'p', 'e', 'g') },
    { AV_CODEC_ID_MJPEG,        MKTAG('I', 'J', 'P', 'G') },
    { AV_CODEC_ID_MJPEG,        MKTAG('A', 'C', 'D', 'V') },
    { AV_CODEC_ID_MJPEG,        MKTAG('Q', 'I', 'V', 'G') },
    /* SL M-JPEG */
    { AV_CODEC_ID_MJPEG,        MKTAG('S', 'L', 'M', 'J') },
    /* Creative Webcam JPEG */
    { AV_CODEC_ID_MJPEG,        MKTAG('C', 'J', 'P', 'G') },
    /* Intel JPEG Library Video Codec */
    { AV_CODEC_ID_MJPEG,        MKTAG('I', 'J', 'L', 'V') },
    /* Midvid JPEG Video Codec */
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'V', 'J', 'P') },
    { AV_CODEC_ID_MJPEG,        MKTAG('A', 'V', 'I', '1') },
    { AV_CODEC_ID_MJPEG,        MKTAG('A', 'V', 'I', '2') },
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'T', 'S', 'J') },
    /* Paradigm Matrix M-JPEG Codec */
    { AV_CODEC_ID_MJPEG,        MKTAG('Z', 'J', 'P', 'G') },
    { AV_CODEC_ID_MJPEG,        MKTAG('M', 'M', 'J', 'P') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG( 0 ,  0 ,  0 ,  0 ) },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG( 3 ,  0 ,  0 ,  0 ) },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '2', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'U', 'Y', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '2', '1', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '2', '1', '6') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '1', '6') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '2', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('V', '4', '2', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '1', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'U', 'N', 'V') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('U', 'Y', 'N', 'V') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('U', 'Y', 'N', 'Y') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('u', 'y', 'v', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('2', 'V', 'u', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('2', 'v', 'u', 'y') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('y', 'u', 'v', 's') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('y', 'u', 'v', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('P', '4', '2', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', '1', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', '1', '6') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', '2', '4') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('U', 'Y', 'V', 'Y') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('V', 'Y', 'U', 'Y') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', 'Y', 'U', 'V') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('A', 'Y', 'U', 'V') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '8', '0', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '8', ' ', ' ') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('H', 'D', 'Y', 'C') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', 'U', '9') },
    /* SoftLab-NSK VideoTizer */
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('V', 'D', 'T', 'Z') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '1', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('N', 'V', '1', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('N', 'V', '2', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '1', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', '4', '2', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'U', 'V', '9') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', 'U', '9') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('a', 'u', 'v', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'V', 'Y', 'U') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'U', 'Y', 'V') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '1', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '1', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '2', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '4', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '4', '4') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('J', '4', '2', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('J', '4', '2', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('J', '4', '4', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('J', '4', '4', '4') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('Y', 'U', 'V', 'A') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '0', 'A') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '2', 'A') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'G', 'B', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'V', '1', '5') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'V', '1', '6') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'V', '2', '4') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'V', '3', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('R', 'G', 'B', 'A') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('A', 'V', '3', '2') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('G', 'R', 'E', 'Y') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', '9', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', '9', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', '9', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', '9', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '9', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '9', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'A', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'A', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', 'A', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', 'A', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'A', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'A', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'F', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'F', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'C', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'C', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', 'C', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '2', 'C', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'C', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', 'C', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'F', 'L') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '0', 'F', 'B') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('v', '3', '0', '8') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('v', '4', '0', '8') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('v', '4', '1', '0') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('y', '4', '0', '8') },
#if FF_API_V408_CODECID
#endif
    { AV_CODEC_ID_VP8,          MKTAG('V', 'P', '8', '0') },
    { AV_CODEC_ID_VP9,          MKTAG('V', 'P', '9', '0') },
    { AV_CODEC_ID_PNG,          MKTAG('M', 'P', 'N', 'G') },
    { AV_CODEC_ID_PNG,          MKTAG('P', 'N', 'G', '1') },
    { AV_CODEC_ID_PNG,          MKTAG('p', 'n', 'g', ' ') }, /* ImageJ */
    /* Ut Video version 13.0.1 BT.709 codecs */
    { AV_CODEC_ID_NONE,         0 }
};

const AVCodecTag ff_codec_bmp_tags_unofficial[] = {
    { AV_CODEC_ID_HEVC,         MKTAG('H', 'E', 'V', 'C') },
    { AV_CODEC_ID_HEVC,         MKTAG('H', '2', '6', '5') },
    { AV_CODEC_ID_NONE,         0 }
};

const AVCodecTag ff_codec_wav_tags[] = {
    { AV_CODEC_ID_PCM_S16LE,       0x0001 },
    /* must come after s16le in this list */
    { AV_CODEC_ID_PCM_U8,          0x0001 },
    { AV_CODEC_ID_PCM_S24LE,       0x0001 },
    { AV_CODEC_ID_PCM_S32LE,       0x0001 },
    { AV_CODEC_ID_PCM_F32LE,       0x0003 },
    /* must come after f32le in this list */
    { AV_CODEC_ID_PCM_F64LE,       0x0003 },
    { AV_CODEC_ID_PCM_ALAW,        0x0006 },
    { AV_CODEC_ID_PCM_MULAW,       0x0007 },
    /* must come after adpcm_ima_wav in this list */
    { AV_CODEC_ID_MP2,             0x0050 },
    { AV_CODEC_ID_MP3,             0x0055 },
    /* rogue format number */
    /* rogue format number */
    { AV_CODEC_ID_AAC,             0x00ff },
    /* ADTS AAC */
    { AV_CODEC_ID_AAC,             0x1600 },
    { AV_CODEC_ID_AAC_LATM,        0x1602 },
    { AV_CODEC_ID_AAC,             0x1610 },
    /* There is no Microsoft Format Tag for E-AC3, the GUID has to be used */
    { AV_CODEC_ID_PCM_MULAW,       0x6c75 },
    { AV_CODEC_ID_AAC,             0x706d },
    { AV_CODEC_ID_AAC,             0x4143 },
    { AV_CODEC_ID_AAC,             0xA106 },
    { AV_CODEC_ID_FLAC,            0xF1AC },
    /* DFPWM does not have an assigned format tag; it uses a GUID in WAVEFORMATEX instead */
    /* HACK/FIXME: Does Vorbis in WAV/AVI have an (in)official ID? */
    { AV_CODEC_ID_VORBIS,          ('V' << 8) + 'o' },
    { AV_CODEC_ID_NONE,      0 },
};


#if CONFIG_WAV_DEMUXER || CONFIG_WAV_MUXER || CONFIG_W64_DEMUXER || CONFIG_W64_MUXER
const AVCodecTag *const ff_wav_codec_tags_list[] = { ff_codec_wav_tags, NULL };
#endif

const AVMetadataConv ff_riff_info_conv[] = {
    { "IART", "artist"     },
    { "ICMT", "comment"    },
    { "ICOP", "copyright"  },
    { "ICRD", "date"       },
    { "IGNR", "genre"      },
    { "ILNG", "language"   },
    { "INAM", "title"      },
    { "IPRD", "album"      },
    { "IPRT", "track"      },
    { "ITRK", "track"      },
    { "ISFT", "encoder"    },
    { "ISMP", "timecode"   },
    { "ITCH", "encoded_by" },
    { 0 },
};

const struct AVCodecTag *avformat_get_riff_video_tags(void)
{
    return ff_codec_bmp_tags;
}

const struct AVCodecTag *avformat_get_riff_audio_tags(void)
{
    return ff_codec_wav_tags;
}

const AVCodecGuid ff_codec_wav_guids[] = {
    { AV_CODEC_ID_MP2,      { 0x2B, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA } },
    { AV_CODEC_ID_NONE }
};
