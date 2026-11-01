/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * h264_sps.c - what the backend reads from an H.264 sequence header (SPS)
 * itself (h264_sps.h has the why).
 *
 * An SPS is a NAL unit of an Annex-B byte stream, after a 00 00 01 start
 * code.  So that no start code appears inside a unit, the encoder puts an 03
 * after any 00 00 followed by a byte up to 03 (emulation prevention, as
 * JPEG's FF 00): the reader takes those 03 out before reading the fields,
 * which are packed bit by bit, most as Exp-Golomb numbers.  The fields are
 * walked up to the VUI's pixel aspect (H.264 7.3.2.1.1, E.1.1).
 */
#include <h264_sps.h>

#define H264_SPS_MAX 256u /* an SPS's bytes read, up to its VUI's aspect (scaling lists make it long) */

/* An SPS's bits, its emulation-prevention bytes taken out */
struct sps_bits
{
	UBYTE data[H264_SPS_MAX];
	u32 count; /* bits in data */
	u32 pos;   /* the next bit; past count once the SPS was cut short */
};

/* The pixel aspects of aspect_ratio_idc 1 to 16 (H.264 Table E-1); 255 gives its own */
static const UBYTE h264_aspects[16][2] = {{1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33},  {24, 11}, {20, 11}, {32, 11},
                                          {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3},   {3, 2},   {2, 1}};

/* The profiles whose SPS carries chroma_format_idc and the fields after it */
static const UBYTE h264_chroma_profiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};

/**
 * sps_read() - The next bits of an SPS, as a number; zeros past its end.
 * @b: the bits
 * @n: how many, at most 32
 *
 * Return: the value.
 */
static u32 sps_read(struct sps_bits *b, u32 n)
{
	u32 value = 0;
	for (u32 i = 0; i < n; i++, b->pos++)
		value = (value << 1) | (b->pos < b->count ? (u32)(b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u : 0u);
	return value;
}

/**
 * sps_ue() - An unsigned Exp-Golomb number.
 * @b: the bits
 *
 * Return: the value, 0 for one past 32 bits (a broken SPS).
 */
static u32 sps_ue(struct sps_bits *b)
{
	u32 zeros = 0;
	while (zeros < 32 && sps_read(b, 1) == 0)
		zeros++;
	return zeros < 32 ? (1u << zeros) - 1u + sps_read(b, zeros) : 0;
}

/**
 * sps_se() - A signed Exp-Golomb number.
 * @b: the bits
 *
 * Return: the value.
 */
static s32 sps_se(struct sps_bits *b)
{
	u32 k = sps_ue(b);
	return (k & 1u) ? (s32)((k + 1u) / 2u) : -(s32)(k / 2u);
}

/**
 * sps_skip_scaling_list() - Read past a scaling list.
 * @b:    the bits
 * @size: its entries, 16 or 64
 */
static void sps_skip_scaling_list(struct sps_bits *b, u32 size)
{
	s32 last = 8, next = 8;
	for (u32 j = 0; j < size && next != 0; j++)
	{
		next = (last + sps_se(b) + 256) % 256;
		last = next != 0 ? next : last;
	}
}

/**
 * h264_sps_read() - What the backend needs from the first SPS in a piece of an Annex-B stream.
 * @data:   the bytes
 * @length: their count
 * @sps:    filled: the profile, 0 if no SPS starts in them; its reference frames and macroblocks;
 *          the pixel aspect its VUI gives, 0/0 for none
 *
 * The SPS is read up to its VUI's aspect (H.264 7.3.2.1.1, E.1.1).  One cut
 * short gives no aspect.
 */
void h264_sps_read(const UBYTE *data, u32 length, struct h264_sps *sps)
{
	sps->profile = 0;
	sps->ref_frames = sps->macroblocks = 0;
	sps->par.Num = sps->par.Den = 0;
	u32 i = 0;
	while (i + 4 < length && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 7))
		i++;
	if (i + 4 >= length)
		return;
	sps->profile = data[i + 4];

	/* its bytes up to the next start code (00 00 0x, x < 3), without the 03 that follows any 00 00 */
	struct sps_bits b;
	u32 n = 0, zeros = 0;
	for (i += 4; i < length && n < sizeof(b.data); i++)
	{
		if (zeros >= 2 && data[i] <= 3)
		{
			if (data[i] != 3)
				break;
			zeros = 0;
			continue;
		}
		zeros = data[i] == 0 ? zeros + 1 : 0;
		b.data[n++] = data[i];
	}
	b.count = n * 8;
	b.pos = 24; /* profile_idc, the constraint flags, level_idc */
	sps_ue(&b); /* seq_parameter_set_id */
	for (u32 k = 0; k < sizeof(h264_chroma_profiles); k++)
	{
		if (sps->profile != h264_chroma_profiles[k])
			continue;
		u32 chroma = sps_ue(&b);
		if (chroma == 3)
			sps_read(&b, 1); /* separate_colour_plane_flag */
		sps_ue(&b);         /* bit_depth_luma_minus8 */
		sps_ue(&b);         /* bit_depth_chroma_minus8 */
		sps_read(&b, 1);    /* qpprime_y_zero_transform_bypass_flag */
		if (sps_read(&b, 1)) /* seq_scaling_matrix_present_flag */
			for (u32 list = 0; list < (chroma != 3 ? 8u : 12u); list++)
				if (sps_read(&b, 1))
					sps_skip_scaling_list(&b, list < 6 ? 16u : 64u);
		break;
	}
	sps_ue(&b); /* log2_max_frame_num_minus4 */
	u32 poc_type = sps_ue(&b);
	if (poc_type == 0)
		sps_ue(&b); /* log2_max_pic_order_cnt_lsb_minus4 */
	else if (poc_type == 1)
	{
		sps_read(&b, 1); /* delta_pic_order_always_zero_flag */
		sps_se(&b);      /* offset_for_non_ref_pic */
		sps_se(&b);      /* offset_for_top_to_bottom_field */
		u32 cycle = sps_ue(&b);
		for (u32 k = 0; k < cycle && b.pos <= b.count; k++)
			sps_se(&b); /* offset_for_ref_frame */
	}
	u32 ref_frames = sps_ue(&b); /* max_num_ref_frames */
	sps_read(&b, 1);             /* gaps_in_frame_num_value_allowed_flag */
	u32 width = sps_ue(&b) + 1u; /* pic_width_in_mbs_minus1 */
	u32 rows = sps_ue(&b) + 1u;  /* pic_height_in_map_units_minus1 */
	BOOL frames_only = sps_read(&b, 1) != 0; /* frame_mbs_only_flag */
	if (!frames_only)
		sps_read(&b, 1); /* mb_adaptive_frame_field_flag */
	if (b.pos <= b.count)
	{
		sps->ref_frames = ref_frames;
		sps->macroblocks = width * rows * (frames_only ? 1u : 2u);
	}
	sps_read(&b, 1);      /* direct_8x8_inference_flag */
	if (sps_read(&b, 1))  /* frame_cropping_flag: four offsets */
		for (u32 k = 0; k < 4; k++)
			sps_ue(&b);
	if (!sps_read(&b, 1) || !sps_read(&b, 1)) /* vui_parameters_present_flag, aspect_ratio_info_present_flag */
		return;
	u32 idc = sps_read(&b, 8), num = 0, den = 0;
	if (idc >= 1 && idc <= 16)
	{
		num = h264_aspects[idc - 1][0];
		den = h264_aspects[idc - 1][1];
	}
	else if (idc == 255) /* Extended_SAR */
	{
		num = sps_read(&b, 16);
		den = sps_read(&b, 16);
	}
	if (b.pos <= b.count && num != 0 && den != 0)
	{
		sps->par.Num = (LONG)num;
		sps->par.Den = (LONG)den;
	}
}
