/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * image_info.c - what a still picture's header says.
 *
 * Plain byte reading, no allocation: the size, colour model, bits per sample,
 * transparency, coding, interlacing and frame count (struct image_facts).  A
 * header that goes on past the bytes given fills in what it can and says so
 * (IMAGE_INFO_SHORT); a GIF's frames are counted as far as the bytes go, its
 * facts complete once its first image's descriptor was read.
 *
 * The signature is checked once (image_info()); each format's reader starts
 * behind it.  Fields are put together a byte at a time: they lie at any
 * offset (a word read could be unaligned), and GIF, BMP and TGA store theirs
 * little-endian.
 */
#include <memory.h>

#include <image_info.h>
#include <jpeg.h>

#define FOURCC(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (ULONG)(d))

static const UBYTE png_signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

static ULONG get_be32(const UBYTE *d)
{
	return (get_be16(d) << 16) | get_be16(d + 2);
}

static ULONG get_le16(const UBYTE *d)
{
	return (ULONG)d[0] | ((ULONG)d[1] << 8);
}

static ULONG get_le32(const UBYTE *d)
{
	return get_le16(d) | (get_le16(d + 2) << 16);
}

/**
 * holds() - Whether bytes hold a picture's rows after an offset.
 * @len:       the bytes
 * @offset:    where the rows start
 * @row_bytes: the bytes of one row
 * @rows:      how many
 *
 * Return: TRUE if all of them are within @len (no product overflows).
 */
static BOOL holds(ULONG len, ULONG offset, ULONG row_bytes, ULONG rows)
{
	return offset <= len && (rows == 0 || row_bytes <= (len - offset) / rows);
}

/**
 * image_codec() - The still picture codec a picture's signature names.
 * @d:   the bytes
 * @len: their count
 *
 * The whole signature is read, not the header behind it.
 *
 * Return: VCC_JPEG, VCC_PNG, VCC_GIF, VCC_BMP or VCC_PPM; VCC_None for none (a TGA has no signature).
 */
ULONG image_codec(const UBYTE *d, ULONG len)
{
	if (len >= JPEG_MARKER_BYTES && d[0] == JPEG_MARKER_PREFIX && d[1] == JPEG_SOI)
		return VCC_JPEG;
	if (len >= 8 && memcmp(d, png_signature, 8) == 0)
		return VCC_PNG;
	if (len >= 6 && memcmp(d, "GIF8", 4) == 0 && (d[4] == '7' || d[4] == '9') && d[5] == 'a')
		return VCC_GIF;
	if (len >= 2 && d[0] == 'B' && d[1] == 'M')
		return VCC_BMP;
	if (len >= 2 && d[0] == 'P' && (d[1] == '5' || d[1] == '6'))
		return VCC_PPM;
	return VCC_None;
}

/**
 * jpeg_frame() - A JPEG's frame header (SOFn): size, colour model, precision, process.
 * @sof:  the segment from its marker on, at least JPEG_SOF_BYTES
 * @info: the facts
 *
 * Return: 0, or VCERR_STREAM for a component count no picture has.
 */
static LONG jpeg_frame(const UBYTE *sof, struct image_facts *info)
{
	static const UBYTE models[5] = {0, IMAGE_MODEL_GREY, 0, IMAGE_MODEL_RGB, IMAGE_MODEL_CMYK}; /* by component count */
	ULONG components = sof[JPEG_SOF_COMPONENTS];
	if (components >= sizeof(models) || models[components] == 0)
		return VCERR_STREAM;
	info->model = models[components];
	info->bits = sof[JPEG_SOF_PRECISION];
	info->height = get_be16(sof + JPEG_SOF_HEIGHT);
	info->width = get_be16(sof + JPEG_SOF_WIDTH);
	/* the rest of SOF4 to SOF15 are hierarchical or arithmetic-coded */
	UBYTE marker = sof[1];
	info->coding = marker == JPEG_SOF0 || marker == JPEG_SOF1 ? IMAGE_CODING_SEQUENTIAL
	               : marker == JPEG_SOF2                      ? IMAGE_CODING_PROGRESSIVE
	               : marker == JPEG_SOF3                      ? IMAGE_CODING_LOSSLESS
	                                                          : IMAGE_CODING_OTHER;
	return 0;
}

/**
 * jpeg_ended() - Whether a JPEG's end of image (EOI) comes after its first scan's header.
 * @d:    the bytes
 * @len:  their count
 * @from: the end of the scan's header
 *
 * Looked for from the end back: any data after the picture is skipped, and
 * coded data never holds FF D9 (an FF there is followed by 00 or a restart
 * marker).
 *
 * Return: TRUE if found.
 */
static BOOL jpeg_ended(const UBYTE *d, ULONG len, ULONG from)
{
	for (ULONG end = len; end >= from + JPEG_MARKER_BYTES; end--)
		if (d[end - 2] == JPEG_MARKER_PREFIX && d[end - 1] == JPEG_EOI)
			return TRUE;
	return FALSE;
}

/**
 * jpeg_info() - A JPEG's frame header, the markers walked from SOI on to the first scan's header (SOS).
 * @d:    the bytes, behind their signature
 * @len:  their count
 * @info: the facts, cleared
 *
 * Return: 0, IMAGE_INFO_SHORT before the frame header, or VCERR_STREAM.
 */
static LONG jpeg_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	BOOL have_frame = FALSE; /* the frame header was read: the facts are complete */
	ULONG pos = JPEG_MARKER_BYTES; /* behind SOI */
	ULONG run_from = 0, run_to = 0; /* the metadata segments just walked */
	for (;;)
	{
		if (pos + JPEG_MARKER_BYTES + JPEG_LENGTH_BYTES > len)
			return have_frame ? 0 : IMAGE_INFO_SHORT;
		if (d[pos] != JPEG_MARKER_PREFIX)
			return VCERR_STREAM;
		UBYTE marker = d[pos + 1];
		if (marker == JPEG_MARKER_PREFIX)
		{
			pos++; /* a fill byte */
			continue;
		}
		if (jpeg_standalone(marker))
		{
			pos += JPEG_MARKER_BYTES;
			continue;
		}
		if (marker == JPEG_EOI)
			return VCERR_STREAM; /* the end before any scan */
		ULONG segment = get_be16(d + pos + JPEG_MARKER_BYTES), end = pos + JPEG_MARKER_BYTES + segment;
		if (segment < JPEG_LENGTH_BYTES)
			return VCERR_STREAM;
		if (marker == JPEG_SOS) /* the first scan: the picture's data follows its header */
		{
			if (!have_frame)
				return VCERR_STREAM;
			if (end <= len)
			{
				info->header = end;
				info->whole = jpeg_ended(d, len, end);
			}
			return 0;
		}
		if (jpeg_frame_marker(marker) && !have_frame)
		{
			if (pos + JPEG_SOF_BYTES > len)
				return IMAGE_INFO_SHORT;
			LONG error = jpeg_frame(d + pos, info);
			if (error != 0)
				return error;
			have_frame = TRUE;
		}
		if (jpeg_metadata_marker(marker))
		{
			run_from = pos == run_to ? run_from : pos;
			run_to = end;
			if (run_to - run_from > info->skip_to - info->skip_from)
			{
				info->skip_from = run_from;
				info->skip_to = run_to;
			}
		}
		pos = end;
	}
}

/**
 * png_palette_transparency() - What a palette PNG's tRNS chunk makes of its transparency.
 * @levels: the chunk's data: an alpha level per palette entry
 * @count:  the chunk's length
 *
 * Return: VCTR_Mask if every level is 0 or 255, else VCTR_Alpha.
 */
static ULONG png_palette_transparency(const UBYTE *levels, ULONG count)
{
	for (ULONG i = 0; i < count; i++)
		if (levels[i] != 0 && levels[i] != 255)
			return VCTR_Alpha;
	return VCTR_Mask;
}

/**
 * png_ended() - Whether a PNG's IEND chunk comes after its image data starts.
 * @d:    the bytes
 * @len:  their count
 * @from: where the image data starts
 *
 * Looked for from the end back: any data after the picture is skipped.
 *
 * Return: TRUE if found.
 */
static BOOL png_ended(const UBYTE *d, ULONG len, ULONG from)
{
	static const UBYTE iend[8] = {0, 0, 0, 0, 'I', 'E', 'N', 'D'}; /* its length (none) and its type */
	for (ULONG i = len; i >= from + 8; i--)
		if (memcmp(d + i - 8, iend, 8) == 0)
			return TRUE;
	return FALSE;
}

/**
 * png_info() - A PNG's IHDR, and the transparency a tRNS chunk before the image data adds.
 * @d:    the bytes, behind their signature
 * @len:  their count
 * @info: the facts, cleared
 *
 * Return: 0, IMAGE_INFO_SHORT before the image data, or VCERR_STREAM.
 */
static LONG png_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	static const UBYTE models[7] = {IMAGE_MODEL_GREY, 0, IMAGE_MODEL_RGB, IMAGE_MODEL_PALETTE, /* by colour type */
	                                IMAGE_MODEL_GREY, 0, IMAGE_MODEL_RGB};
	if (len < 33)
		return IMAGE_INFO_SHORT;
	if (get_be32(d + 12) != FOURCC('I', 'H', 'D', 'R'))
		return VCERR_STREAM;
	ULONG type = d[25];
	if (type > 6 || models[type] == 0)
		return VCERR_STREAM;
	info->width = get_be32(d + 16);
	info->height = get_be32(d + 20);
	info->model = models[type];
	info->bits = d[24];
	info->transparency = type == 4 || type == 6 ? VCTR_Alpha : VCTR_None; /* grey or RGB with alpha */
	info->coding = IMAGE_CODING_DEFLATE;
	info->interlaced = d[28] != 0;

	BOOL complete = FALSE; /* a tRNS chunk was read: nothing later changes the facts */
	for (ULONG pos = 33;;) /* the chunks after IHDR: length, type, data, CRC */
	{
		if (pos + 8 > len)
			return complete ? 0 : IMAGE_INFO_SHORT;
		ULONG chunk = get_be32(d + pos), type_id = get_be32(d + pos + 4);
		if (type_id == FOURCC('I', 'D', 'A', 'T'))
		{
			info->header = pos + 8; /* the picture's data starts in this chunk */
			info->whole = png_ended(d, len, pos + 8);
			return 0;
		}
		if (type_id == FOURCC('I', 'E', 'N', 'D'))
			return 0;
		if (chunk > len - (pos + 8))
			return complete ? 0 : IMAGE_INFO_SHORT;
		if (type_id == FOURCC('t', 'R', 'N', 'S'))
		{
			/* a palette's: a level per entry; else one transparent colour */
			info->transparency = type == 3 ? png_palette_transparency(d + pos + 8, chunk) : VCTR_Mask;
			complete = TRUE;
		}
		pos += 12 + chunk;
	}
}

/**
 * gif_skip_blocks() - Past a GIF's data sub-blocks.
 * @d:   the bytes
 * @len: their count
 * @pos: the first sub-block's length byte
 *
 * Return: the position after the terminating empty block, past @len if the bytes end first.
 */
static ULONG gif_skip_blocks(const UBYTE *d, ULONG len, ULONG pos)
{
	while (pos < len && d[pos] != 0)
		pos += 1u + d[pos];
	return pos < len ? pos + 1 : len + 1;
}

/**
 * gif_ended() - What a GIF's header reading says when the bytes end.
 * @info: the facts so far
 *
 * Return: 0 once the first image's descriptor was read (the rest only adds to
 * the count), else IMAGE_INFO_SHORT.
 */
static LONG gif_ended(const struct image_facts *info)
{
	return info->frames != 0 ? 0 : IMAGE_INFO_SHORT;
}

/**
 * gif_info() - A GIF's screen, and its images counted as far as the bytes go.
 * @d:    the bytes, behind their signature
 * @len:  their count
 * @info: the facts, cleared
 *
 * The first image's interlacing, colour table and transparent colour (a
 * graphic control extension before it) are reported: a decoder takes that
 * image.
 *
 * Return: 0, IMAGE_INFO_SHORT when the bytes end before the first image's
 * descriptor, or VCERR_STREAM.
 */
static LONG gif_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	if (len < 13)
		return IMAGE_INFO_SHORT;
	info->width = get_le16(d + 6);
	info->height = get_le16(d + 8);
	info->model = IMAGE_MODEL_PALETTE;
	info->bits = 8;
	info->coding = IMAGE_CODING_LZW;
	info->frames = 0;
	ULONG pos = 13;
	if (d[10] & 0x80) /* a global colour table: 2^bits entries */
	{
		info->bits = (d[10] & 7u) + 1u;
		pos += 3u << info->bits;
	}
	for (;;)
	{
		if (pos >= len)
			return gif_ended(info);
		if (d[pos] == 0x3b) /* the trailer */
			return info->frames != 0 ? 0 : VCERR_STREAM;
		if (d[pos] == 0x21) /* an extension: a graphic control one names a transparent colour */
		{
			if (pos + 4 > len)
				return gif_ended(info);
			if (d[pos + 1] == 0xf9 && (d[pos + 3] & 1u) && info->frames == 0)
				info->transparency = VCTR_Mask;
			pos = gif_skip_blocks(d, len, pos + 2);
		}
		else if (d[pos] == 0x2c) /* an image */
		{
			if (pos + 10 > len)
				return gif_ended(info);
			ULONG packed = d[pos + 9], data = pos + 10;
			if (info->frames++ == 0)
			{
				info->interlaced = (packed & 0x40) != 0;
				if (packed & 0x80)
					info->bits = (packed & 7u) + 1u; /* its own colour table */
			}
			if (packed & 0x80) /* a local colour table */
				data += 3u << ((packed & 7u) + 1u);
			if (info->frames == 1 && data <= len)
				info->header = data; /* the first image's data starts here */
			pos = gif_skip_blocks(d, len, data + 1); /* past the LZW code size */
			if (info->frames == 1)
				info->whole = pos <= len; /* the decoder takes this image only */
		}
		else
			return info->frames != 0 ? 0 : VCERR_STREAM; /* garbage after the images */
	}
}

/**
 * bmp_info() - A Windows BMP's info header.
 * @d:    the bytes, behind their signature
 * @len:  their count
 * @info: the facts, cleared
 *
 * Return: 0, IMAGE_INFO_SHORT, or VCERR_STREAM (also for an OS/2 1.x header, which this does not read).
 */
static LONG bmp_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	if (len < 18)
		return IMAGE_INFO_SHORT;
	if (get_le32(d + 14) < 40)
		return VCERR_STREAM; /* the info header's size: 12 is OS/2 1.x */
	if (len < 34)
		return IMAGE_INFO_SHORT;
	LONG height = (LONG)get_le32(d + 22); /* negative: the rows top first */
	ULONG bpp = get_le16(d + 28), compression = get_le32(d + 30);
	info->width = get_le32(d + 18);
	info->height = (ULONG)(height < 0 ? -height : height);
	info->model = bpp != 0 && bpp <= 8u ? IMAGE_MODEL_PALETTE : IMAGE_MODEL_RGB;
	info->bits = bpp <= 8u ? bpp : bpp == 16u ? 5u : 8u; /* 0: a JPEG or PNG inside says */
	info->transparency = bpp == 32u ? VCTR_Alpha : VCTR_None;      /* the fourth byte, as a decoder reads it */
	/* 0 none, 1 and 2 RLE8 and RLE4; 3 and 6 bit fields, 4 and 5 an embedded JPEG or PNG */
	info->coding = compression == 0 ? IMAGE_CODING_NONE : compression <= 2u ? IMAGE_CODING_RLE : IMAGE_CODING_OTHER;
	info->header = 34; /* as far as read: the palette after it is at most 1 KiB */
	if (info->coding == IMAGE_CODING_NONE && bpp <= 32u && info->width <= (1u << 26)) /* rows of whole 32-bit words */
		info->whole = holds(len, get_le32(d + 10), (info->width * bpp + 31u) / 32u * 4u, info->height);
	return 0;
}

/**
 * tga_info() - A Truevision TGA's header (no signature: the caller names the codec).
 * @d:    the bytes
 * @len:  their count
 * @info: the facts, cleared
 *
 * Return: 0, IMAGE_INFO_SHORT, or VCERR_STREAM for a header no TGA has.
 */
static LONG tga_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	if (len < 18)
		return IMAGE_INFO_SHORT;
	ULONG type = d[2], depth = d[16], attribute = d[17] & 0x0fu; /* attribute: alpha bits per pixel */
	BOOL known_type = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
	BOOL known_depth = depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
	if (!known_type || !known_depth || d[1] > 1)
		return VCERR_STREAM;
	info->width = get_le16(d + 12);
	info->height = get_le16(d + 14);
	if (type == 1 || type == 9)
	{
		info->model = IMAGE_MODEL_PALETTE;
		info->bits = depth; /* the index */
	}
	else if (type == 3 || type == 11)
	{
		info->model = IMAGE_MODEL_GREY;
		info->bits = 8; /* 16 bits a pixel: grey and alpha */
	}
	else
	{
		info->model = IMAGE_MODEL_RGB;
		info->bits = depth <= 16u ? 5u : 8u;
	}
	info->transparency = depth == 32u || attribute >= 2u ? VCTR_Alpha : attribute == 1u ? VCTR_Mask : VCTR_None;
	info->coding = type >= 9u ? IMAGE_CODING_RLE : IMAGE_CODING_NONE;
	info->header = 18; /* as far as read: the ID and colour map after it are small but for a palette's */
	if (info->coding == IMAGE_CODING_NONE) /* the pixels follow the ID and the colour map */
	{
		ULONG map = d[1] != 0 ? get_le16(d + 5) * ((d[7] + 7u) / 8u) : 0;
		info->whole = holds(len, 18u + d[0] + map, info->width * ((depth + 7u) / 8u), info->height);
	}
	return 0;
}

/**
 * ppm_number() - The next number of a PPM's header, past white space and comments.
 * @d:     the bytes
 * @len:   their count
 * @pos:   where to read; moved past the number
 * @value: where the number goes
 *
 * Return: 0, IMAGE_INFO_SHORT when the bytes end before the number does (it
 * may go on), or VCERR_STREAM for something else where it should be.
 */
static LONG ppm_number(const UBYTE *d, ULONG len, ULONG *pos, ULONG *value)
{
	ULONG p = *pos;
	for (;;)
	{
		if (p >= len)
			return IMAGE_INFO_SHORT;
		if (d[p] == '#')
			while (p < len && d[p] != '\n')
				p++;
		else if (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n')
			p++;
		else
			break;
	}
	if (d[p] < '0' || d[p] > '9')
		return VCERR_STREAM;
	ULONG n = 0;
	while (p < len && d[p] >= '0' && d[p] <= '9' && n < 100000u)
		n = n * 10u + (ULONG)(d[p++] - '0');
	if (p >= len)
		return IMAGE_INFO_SHORT;
	*pos = p;
	*value = n;
	return 0;
}

/**
 * ppm_info() - A binary PPM's or PGM's header: width, height, maximum value.
 * @d:    the bytes, behind their signature
 * @len:  their count
 * @info: the facts, cleared
 *
 * Return: 0, IMAGE_INFO_SHORT, or VCERR_STREAM.
 */
static LONG ppm_info(const UBYTE *d, ULONG len, struct image_facts *info)
{
	ULONG field[3], pos = 2;
	for (ULONG i = 0; i < 3; i++)
	{
		LONG rc = ppm_number(d, len, &pos, &field[i]);
		if (rc != 0)
			return rc;
	}
	if (field[0] == 0 || field[1] == 0 || field[2] == 0 || field[2] > 65535u)
		return VCERR_STREAM;
	info->width = field[0];
	info->height = field[1];
	info->model = d[1] == '6' ? IMAGE_MODEL_RGB : IMAGE_MODEL_GREY;
	info->bits = field[2] > 255u ? 16u : 8u;
	info->coding = IMAGE_CODING_NONE;
	info->header = pos + 1; /* past the white space after the maximum value */
	ULONG sample_bytes = field[2] > 255u ? 2u : 1u, channels = d[1] == '6' ? 3u : 1u;
	info->whole = holds(len, info->header, info->width * channels * sample_bytes, info->height);
	return 0;
}

/**
 * image_info() - What a still picture's header says.
 * @data:   the picture's bytes
 * @length: their count
 * @codec:  VCC_* to read them as, VCC_None to tell from the signature
 * @info:   filled whole
 *
 * Return: 0, IMAGE_INFO_SHORT (the facts so far filled in), or VCERR_STREAM
 * for bytes that are no picture this knows, or not the one @codec names.
 */
LONG image_info(const UBYTE *data, ULONG length, ULONG codec, struct image_facts *info)
{
	memset(info, 0, sizeof(*info));
	ULONG signature = image_codec(data, length);
	info->codec = codec != VCC_None ? codec : signature;
	info->frames = 1;
	if (info->codec != signature && info->codec != VCC_TGA)
		return VCERR_STREAM; /* no signature, or another codec's (a TGA has none) */
	switch (info->codec)
	{
	case VCC_JPEG:
		return jpeg_info(data, length, info);
	case VCC_PNG:
		return png_info(data, length, info);
	case VCC_GIF:
		return gif_info(data, length, info);
	case VCC_BMP:
		return bmp_info(data, length, info);
	case VCC_TGA:
		return tga_info(data, length, info);
	case VCC_PPM:
		return ppm_info(data, length, info);
	default:
		return VCERR_STREAM;
	}
}
