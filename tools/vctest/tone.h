/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tone.h - the test tone: a triangle at -12 dBFS, one sample at a time.
 */
#ifndef VCTEST_TONE_H
#define VCTEST_TONE_H

#include <exec/types.h>

#define TONE_HZ 1000
#define TONE_PEAK 8192 /* -12 dBFS */

/**
 * tone_sample() - One 16-bit sample of the tone, the phase carried on.
 * @phase: the phase, 16.16 of one period
 * @rate:  samples per second
 * @hz:    the tone's frequency
 *
 * Return: the sample.
 */
static inline WORD tone_sample(ULONG *phase, ULONG rate, ULONG hz)
{
	LONG p = (LONG)(*phase & 0xffffu);
	LONG v = p < 0x8000 ? p - 0x4000 : 0xc000 - p; /* a triangle, -0x4000 to 0x4000 */
	*phase += (hz << 16) / rate;
	return (WORD)(v * TONE_PEAK / 0x4000);
}

#endif /* VCTEST_TONE_H */
