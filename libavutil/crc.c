/*
 * copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
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

#include "thread.h"
#include "avassert.h"
#include "bswap.h"
#include "crc.h"
#include "error.h"
#if ARCH_AARCH64
#include "libavutil/aarch64/crc.h"
#elif ARCH_X86
#include "libavutil/x86/crc.h"
#endif

#define CRC_TABLE_SIZE 1024
static AVCRC av_crc_table[AV_CRC_MAX][CRC_TABLE_SIZE];

#define DECLARE_CRC_INIT_TABLE_ONCE(id, le, bits, poly)                                       \
static AVOnce id ## _once_control = AV_ONCE_INIT;                                             \
static void id ## _init_table_once(void)                                                      \
{                                                                                             \
    av_assert0(av_crc_init(av_crc_table[id], le, bits, poly, sizeof(av_crc_table[id])) >= 0); \
}

#define CRC_INIT_TABLE_ONCE(id) ff_thread_once(&id ## _once_control, id ## _init_table_once)

DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_8_ATM,      0,  8,       0x07)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_8_EBU,      0,  8,       0x1D)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_16_ANSI,    0, 16,     0x8005)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_16_CCITT,   0, 16,     0x1021)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_24_IEEE,    0, 24,   0x864CFB)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_32_IEEE,    0, 32, 0x04C11DB7)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_32_IEEE_LE, 1, 32, 0xEDB88320)
DECLARE_CRC_INIT_TABLE_ONCE(AV_CRC_16_ANSI_LE, 1, 16,     0xA001)

int av_crc_init(AVCRC *ctx, int le, int bits, uint32_t poly, int ctx_size)
{
    unsigned i, j;
    uint32_t c;

    if (bits < 8 || bits > 32 || poly >= (1LL << bits))
        return AVERROR(EINVAL);
    if (ctx_size != sizeof(AVCRC) * 257 && ctx_size != sizeof(AVCRC) * 1024)
        return AVERROR(EINVAL);

#if ARCH_X86
    int done = ff_crc_init_x86(ctx, le, bits, poly, ctx_size);
    if (done)
        return 0;
#endif

    for (i = 0; i < 256; i++) {
        if (le) {
            for (c = i, j = 0; j < 8; j++)
                c = (c >> 1) ^ (poly & (-(c & 1)));
            ctx[i] = c;
        } else {
            for (c = i << 24, j = 0; j < 8; j++)
                c = (c << 1) ^ ((poly << (32 - bits)) & (((int32_t) c) >> 31));
            ctx[i] = av_bswap32(c);
        }
    }
    ctx[256] = 1;
#if !CONFIG_SMALL
    if (ctx_size >= sizeof(AVCRC) * 1024)
        for (i = 0; i < 256; i++)
            for (j = 0; j < 3; j++)
                ctx[256 * (j + 1) + i] =
                    (ctx[256 * j + i] >> 8) ^ ctx[ctx[256 * j + i] & 0xFF];
#endif

    return 0;
}

const AVCRC *av_crc_get_table(AVCRCId crc_id)
{
    if ((unsigned)crc_id >= AV_CRC_MAX)
        return NULL;
// Check for arch-specific extensions first to avoid initializing
// ordinary CRC tables unnecessarily.
#if ARCH_AARCH64
    const AVCRC *table = ff_crc_get_table_aarch64(crc_id);
    if (table)
        return table;
#elif ARCH_X86
    const AVCRC *table = ff_crc_get_table_x86(crc_id);
    if (table)
        return table;
#endif

#if !CONFIG_HARDCODED_TABLES
    switch (crc_id) {
    case AV_CRC_8_ATM:      CRC_INIT_TABLE_ONCE(AV_CRC_8_ATM); break;
    case AV_CRC_8_EBU:      CRC_INIT_TABLE_ONCE(AV_CRC_8_EBU); break;
    case AV_CRC_16_ANSI:    CRC_INIT_TABLE_ONCE(AV_CRC_16_ANSI); break;
    case AV_CRC_16_CCITT:   CRC_INIT_TABLE_ONCE(AV_CRC_16_CCITT); break;
    case AV_CRC_24_IEEE:    CRC_INIT_TABLE_ONCE(AV_CRC_24_IEEE); break;
    case AV_CRC_32_IEEE:    CRC_INIT_TABLE_ONCE(AV_CRC_32_IEEE); break;
    case AV_CRC_32_IEEE_LE: CRC_INIT_TABLE_ONCE(AV_CRC_32_IEEE_LE); break;
    case AV_CRC_16_ANSI_LE: CRC_INIT_TABLE_ONCE(AV_CRC_16_ANSI_LE); break;
    default: av_unreachable("already checked");
    }
#endif
    return av_crc_table[crc_id];
}

uint32_t av_crc(const AVCRC *ctx, uint32_t crc,
                const uint8_t *buffer, size_t length)
{
    if (ctx[0]) {
#if ARCH_AARCH64
        return ff_crc_aarch64(ctx, crc, buffer, length);
#elif ARCH_X86
        return ff_crc_x86(ctx, crc, buffer, length);
#endif
    }
    av_assert2(ctx[0] == 0);

    const uint8_t *end = buffer + length;

#if !CONFIG_SMALL
    if (!ctx[256]) {
        while (((intptr_t) buffer & 3) && buffer < end)
            crc = ctx[((uint8_t) crc) ^ *buffer++] ^ (crc >> 8);

        while (buffer < end - 3) {
            crc ^= av_le2ne32(*(const uint32_t *) buffer); buffer += 4;
            crc = ctx[3 * 256 + ( crc        & 0xFF)] ^
                  ctx[2 * 256 + ((crc >> 8 ) & 0xFF)] ^
                  ctx[1 * 256 + ((crc >> 16) & 0xFF)] ^
                  ctx[0 * 256 + ((crc >> 24)       )];
        }
    }
#endif
    while (buffer < end)
        crc = ctx[((uint8_t) crc) ^ *buffer++] ^ (crc >> 8);

    return crc;
}
