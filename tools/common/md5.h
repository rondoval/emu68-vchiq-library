/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * md5.h - MD5 (RFC 1321) for the decode test: one digest per frame,
 * compared with ffmpeg's framemd5 output.  Plain C, byte order handled
 * explicitly, so the same file compiles on the host for its own check.
 */
#ifndef TOOLS_MD5_H
#define TOOLS_MD5_H

#ifdef __amigaos__
#include <exec/types.h>
typedef ULONG md5_u32;
#else
#include <stdint.h>
typedef uint32_t md5_u32;
#endif

struct md5
{
	md5_u32 state[4];
	md5_u32 len_lo; /* bytes so far */
	md5_u32 len_hi;
	unsigned char block[64];
	md5_u32 used;
};

static const md5_u32 md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static const unsigned char md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static inline md5_u32 md5_rotl(md5_u32 x, unsigned n)
{
	return (x << n) | (x >> (32u - n));
}

static void md5_block(struct md5 *m, const unsigned char *p)
{
	md5_u32 w[16];
	for (unsigned i = 0; i < 16; i++)
		w[i] = (md5_u32)p[i * 4] | ((md5_u32)p[i * 4 + 1] << 8) | ((md5_u32)p[i * 4 + 2] << 16) |
		       ((md5_u32)p[i * 4 + 3] << 24);

	md5_u32 a = m->state[0], b = m->state[1], c = m->state[2], d = m->state[3];
	for (unsigned i = 0; i < 64; i++)
	{
		md5_u32 f;
		unsigned g;
		if (i < 16)
		{
			f = (b & c) | (~b & d);
			g = i;
		}
		else if (i < 32)
		{
			f = (d & b) | (~d & c);
			g = (5u * i + 1u) & 15u;
		}
		else if (i < 48)
		{
			f = b ^ c ^ d;
			g = (3u * i + 5u) & 15u;
		}
		else
		{
			f = c ^ (b | ~d);
			g = (7u * i) & 15u;
		}
		md5_u32 t = d;
		d = c;
		c = b;
		b = b + md5_rotl(a + f + md5_k[i] + w[g], md5_r[i]);
		a = t;
	}
	m->state[0] += a;
	m->state[1] += b;
	m->state[2] += c;
	m->state[3] += d;
}

static void md5_init(struct md5 *m)
{
	m->state[0] = 0x67452301u;
	m->state[1] = 0xefcdab89u;
	m->state[2] = 0x98badcfeu;
	m->state[3] = 0x10325476u;
	m->len_lo = 0;
	m->len_hi = 0;
	m->used = 0;
}

static void md5_update(struct md5 *m, const unsigned char *data, md5_u32 n)
{
	md5_u32 lo = m->len_lo + n;
	if (lo < m->len_lo)
		m->len_hi++;
	m->len_lo = lo;

	if (m->used != 0)
	{
		md5_u32 take = 64u - m->used;
		if (take > n)
			take = n;
		for (md5_u32 i = 0; i < take; i++)
			m->block[m->used + i] = data[i];
		m->used += take;
		data += take;
		n -= take;
		if (m->used < 64u)
			return;
		md5_block(m, m->block);
		m->used = 0;
	}
	while (n >= 64u)
	{
		md5_block(m, data);
		data += 64;
		n -= 64u;
	}
	for (md5_u32 i = 0; i < n; i++)
		m->block[i] = data[i];
	m->used = n;
}

static void md5_final(struct md5 *m, unsigned char digest[16])
{
	md5_u32 bits_lo = m->len_lo << 3;
	md5_u32 bits_hi = (m->len_hi << 3) | (m->len_lo >> 29);
	unsigned char pad[72];
	md5_u32 padlen = (m->used < 56u) ? 56u - m->used : 120u - m->used;

	pad[0] = 0x80;
	for (md5_u32 i = 1; i < padlen; i++)
		pad[i] = 0;
	for (unsigned i = 0; i < 4; i++)
	{
		pad[padlen + i] = (unsigned char)(bits_lo >> (8u * i));
		pad[padlen + 4 + i] = (unsigned char)(bits_hi >> (8u * i));
	}
	md5_update(m, pad, padlen + 8u);

	for (unsigned i = 0; i < 4; i++)
		for (unsigned k = 0; k < 4; k++)
			digest[i * 4 + k] = (unsigned char)(m->state[i] >> (8u * k));
}

#endif /* TOOLS_MD5_H */
