/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_encoder.c - the encoder kind: ril.video_encode for H.264,
 * ril.image_encode for JPEG.  Pictures in, packets out.
 *
 * An H.264 encoder given anything but I420 at the picture's own size takes
 * ril.isp in front of it, tunnelled inside the VPU: the client sends RGB, and
 * neither the pictures between the two nor their memory reach the host.
 *
 * H.264 sets headers with the first frame, whole frames per buffer where
 * they fit, the bit rate, rate control, key frame interval, profile and
 * level, and an immutable input.  JPEG takes the quality and writes no EXIF.
 */
#include <videocore_private.h>
#include <session_internal.h>

#define H264_BITRATE_DEFAULT 10000000u /* bits per second */
#define H264_KEY_INTERVAL_DEFAULT 60   /* frames */
#define JPEG_QUALITY_DEFAULT 80
#define TUNNEL_BUFFERS 2               /* pictures between the ISP and the encoder: one being written, one being read */
#define TUNNEL_ROWS 16u                /* the encoder's pictures are whole 16 rows in their buffer */

/**
 * h264_profile_mmal() - The firmware's H.264 profile for a VCP_H264_*.
 * @profile: VCP_H264_*
 *
 * Return: MMAL_VIDEO_PROFILE_H264_*, High unless told otherwise.
 */
static ULONG h264_profile_mmal(ULONG profile)
{
	switch (profile)
	{
	case VCP_H264_Baseline:
		return MMAL_VIDEO_PROFILE_H264_BASELINE;
	case VCP_H264_ConstrainedBaseline:
		return MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE;
	case VCP_H264_Main:
		return MMAL_VIDEO_PROFILE_H264_MAIN;
	default:
		return MMAL_VIDEO_PROFILE_H264_HIGH;
	}
}

/**
 * h264_level_mmal() - The firmware's H.264 level for a level times ten.
 * @level: the level times ten, as the tags give it (41 = 4.1); 0 for the default
 *
 * Return: MMAL_VIDEO_LEVEL_H264_*, 4.0 unless told otherwise, 0 for a level
 * that does not exist.
 */
static ULONG h264_level_mmal(ULONG level)
{
	static const struct
	{
		UBYTE tenths;
		ULONG mmal;
	} levels[] = {
	    {0, MMAL_VIDEO_LEVEL_H264_4},   {10, MMAL_VIDEO_LEVEL_H264_1},  {11, MMAL_VIDEO_LEVEL_H264_11}, {12, MMAL_VIDEO_LEVEL_H264_12},
	    {13, MMAL_VIDEO_LEVEL_H264_13}, {20, MMAL_VIDEO_LEVEL_H264_2},  {21, MMAL_VIDEO_LEVEL_H264_21}, {22, MMAL_VIDEO_LEVEL_H264_22},
	    {30, MMAL_VIDEO_LEVEL_H264_3},  {31, MMAL_VIDEO_LEVEL_H264_31}, {32, MMAL_VIDEO_LEVEL_H264_32}, {40, MMAL_VIDEO_LEVEL_H264_4},
	    {41, MMAL_VIDEO_LEVEL_H264_41}, {42, MMAL_VIDEO_LEVEL_H264_42}, {50, MMAL_VIDEO_LEVEL_H264_5},  {51, MMAL_VIDEO_LEVEL_H264_51},
	};
	for (u32 i = 0; i < ARRAY_SIZE(levels); i++)
		if (levels[i].tenths == level)
			return levels[i].mmal;
	return 0;
}

/**
 * h264_bitrate() - The H.264 bit rate the client asked for.
 * @params: the session's parameters
 *
 * Return: VCA_Bitrate, H264_BITRATE_DEFAULT when not given.
 */
static ULONG h264_bitrate(const struct VCSessionParams *params)
{
	return params->Bitrate != 0 ? params->Bitrate : H264_BITRATE_DEFAULT;
}

/**
 * jpeg_settings() - A JPEG encoder's settings: the quality, no EXIF.
 * @session: the session, the output format committed
 *
 * Return: 0; both are best effort.
 */
static LONG jpeg_settings(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	ULONG quality = params->Quality != 0 ? clamp_val(params->Quality, 1, 100) : JPEG_QUALITY_DEFAULT;
	if (mmal_port_param_set_u32(session->base, session->encoder_out, MMAL_PARAMETER_JPEG_Q_FACTOR, quality) != 0)
		Kprintf("[videocore] JPEG quality not taken\n");
	(void)mmal_port_param_set_u32(session->base, session->encoder->control, MMAL_PARAMETER_EXIF_DISABLE, 1);
	return 0;
}

/**
 * h264_settings() - An H.264 encoder's settings.
 * @session: the session, the output format committed
 *
 * Headers with the first frame and the SPS timing, whole frames per buffer,
 * the bit rate, rate control, key frame interval, profile and level, an
 * immutable input.  Only the profile and level must be taken; the rest are
 * best effort.
 *
 * Return: 0, or the profile's error.
 */
static LONG h264_settings(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	const struct VCSessionParams *params = &session->params;
	struct mmal_port *output = session->encoder_out;
	struct mmal_port *control = session->encoder->control;
	(void)mmal_port_param_set_u32(base, output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, 1);
	(void)mmal_port_param_set_u32(base, control, MMAL_PARAMETER_VIDEO_ENCODE_HEADERS_WITH_FRAME, 1);
	(void)mmal_port_param_set_u32(base, control, MMAL_PARAMETER_MINIMISE_FRAGMENTATION, 1);
	if (mmal_port_param_set_u32(base, output, MMAL_PARAMETER_VIDEO_BIT_RATE, h264_bitrate(params)) != 0)
		Kprintf("[videocore] bit rate not taken (an old firmware?)\n");
	if (params->RateControl != 0)
		(void)mmal_port_param_set_u32(base, output, MMAL_PARAMETER_RATECONTROL,
		                              params->RateControl == VCRC_CBR ? MMAL_VIDEO_RATECONTROL_CONSTANT : MMAL_VIDEO_RATECONTROL_VARIABLE);
	(void)mmal_port_param_set_u32(base, output, MMAL_PARAMETER_INTRAPERIOD,
	                              params->KeyInterval != 0 ? params->KeyInterval : H264_KEY_INTERVAL_DEFAULT);
	const u32 profile[2] = {h264_profile_mmal(params->Profile), h264_level_mmal(params->Level)};
	LONG error = mmal_port_param_set(base, output, MMAL_PARAMETER_PROFILE, profile, 2);
	if (error != 0)
	{
		Kprintf("[videocore] set H.264 profile %ld level %ld: %ld\n", params->Profile, params->Level, error);
		return error;
	}
	(void)mmal_port_param_set_u32(base, session->encoder_in, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, 1);
	return 0;
}

/**
 * encoder_wants_isp() - Whether an encoder's pictures go through the ISP.
 * @params: the session's parameters
 *
 * Return: TRUE for H.264 with an input other than I420, or an encoded size of its own.
 */
static BOOL encoder_wants_isp(const struct VCSessionParams *params)
{
	return params->Codec == VCC_H264 &&
	       ((params->InputFormat != 0 && params->InputFormat != VCF_I420) || params->OutputWidth != 0 || params->OutputHeight != 0);
}

/**
 * encoder_tunnel_size() - Settle the encoded picture of a session through the ISP.
 * @session: a tunnelled encoder session, its input resolved
 *
 * The tunnel's pictures are I420 and no one but the ISP writes them, so a
 * place in them, a pitch or another format would leave the rest undefined.
 *
 * Return: 0, or VCERR_BADPARAM for what cannot be done.
 */
static LONG encoder_tunnel_size(struct vc_session *session)
{
	struct VCSessionParams *params = &session->params;
	if (params->HaveDestRect || params->KeepAspect || params->OutputPitch != 0 || param_output_format(params) != VCF_I420)
		return VCERR_BADPARAM;
	if (params->OutputWidth == 0)
		params->OutputWidth = params->Width & ~1u;
	if (params->OutputHeight == 0)
		params->OutputHeight = params->Height & ~1u;
	return ((params->OutputWidth | params->OutputHeight) & 1u) == 0 ? 0 : VCERR_BADPARAM;
}

/**
 * encoder_tunnel_formats() - The formats from the client's pictures to the encoder's input.
 * @session: a tunnelled encoder session, both components created
 *
 * The ISP's output decides, and the encoder's input takes its format as the
 * VPU read it back, which is libmmal's rule.  The picture is whole 64 pixels
 * wide and whole 16 rows high in its buffer, the encoded part visible.  RGB
 * comes out of the ISP as full-range BT.601 whatever it is told, so that is
 * what the encoder writes into the stream.
 *
 * Return: 0, VCERR_UNSUPPORTED if the ISP does not read the format, or the error.
 */
static LONG encoder_tunnel_formats(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	const struct VCSessionParams *params = &session->params;
	struct mmal_port *out = session->isp_out;
	struct mmal_port *in = session->encoder_in;

	LONG error = input_format_commit(session, session->isp_in);
	if (error != 0)
		return error;

	struct VCRect whole = rect_whole(params->OutputWidth, params->OutputHeight);
	video_format_set(&out->format, MMAL_ENCODING_I420, ALIGN_UP(params->OutputWidth, isp_row_pixels(VCF_I420)),
	                 ALIGN_UP(params->OutputHeight, TUNNEL_ROWS), &whole, &params->FrameRate);
	error = mmal_port_commit(base, out);
	if (error != 0)
		return error;

	in->format = out->format;
	in->format.es.video.color_space = color_space_mmal(format_is_planar(params->InputFormat) ? params->ColorSpace : VCCS_JPEG);
	error = mmal_port_commit(base, in);
	if (error != 0)
		return error;
	if (in->format.es.video.width != out->format.es.video.width)
	{
		Kprintf("[videocore] the encoder read the tunnel's rows as %ld, the ISP's are %ld\n", in->format.es.video.width, out->format.es.video.width);
		return VCERR_BADPARAM;
	}

	out->buffer_num = in->buffer_num = umax(umax(out->buffer_num_min, in->buffer_num_min), TUNNEL_BUFFERS);
	out->buffer_size = in->buffer_size = umax(out->buffer_size_min, in->buffer_size_min);
	error = mmal_port_commit(base, out);
	return error != 0 ? error : mmal_port_commit(base, in);
}

/**
 * encoder_formats() - The encoder's input and output formats.
 * @session: the session, its input resolved and the encoder created
 * @jpeg:    JPEG, else H.264
 *
 * The output takes the input's format as the VPU read it back: the picture's
 * size, its visible part, which is what gets encoded, and the frame rate,
 * which the SPS timing needs.  Only the encoding and the bit rate change.
 *
 * Return: 0, VCERR_UNSUPPORTED for an input format the encoder does not
 * take, or the error.
 */
static LONG encoder_formats(struct vc_session *session, BOOL jpeg)
{
	const struct VCSessionParams *params = &session->params;
	LONG error = session->tunnelled ? encoder_tunnel_formats(session) : input_format_commit(session, session->encoder_in);
	if (error != 0)
		return error;
	struct mmal_format *format = &session->encoder_out->format;
	*format = session->encoder_in->format;
	format->encoding = jpeg ? MMAL_ENCODING_JPEG : MMAL_ENCODING_H264;
	format->encoding_variant = 0;
	format->bitrate = jpeg ? 0 : h264_bitrate(params);
	return mmal_port_commit(session->base, session->encoder_out);
}

/**
 * encoder_params() - Check an encoder's parameters and fill in the defaults.
 * @session: an encoder session
 * @jpeg:    JPEG, else H.264
 *
 * The input pictures' layout is resolved; H.264 runs at 30 frames per second
 * unless told.
 *
 * Return: 0, VCERR_UNSUPPORTED for another codec or an input format the
 * encoder does not read (codec_reads()), VCERR_BADPARAM for a size, pitch or
 * level that does not exist.
 */
static LONG encoder_params(struct vc_session *session, BOOL jpeg)
{
	struct VCSessionParams *params = &session->params;
	if (!jpeg && params->Codec != VCC_H264)
		return VCERR_UNSUPPORTED;
	session->tunnelled = encoder_wants_isp(params);
	LONG error = input_resolve(session, session->tunnelled);
	if (error == 0 && !codec_reads(VCK_Encoder, params->Codec, params->InputFormat))
		error = VCERR_UNSUPPORTED;
	if (error != 0 || jpeg)
		return error; /* the rest is H.264's */
	if (session->tunnelled)
	{
		error = encoder_tunnel_size(session);
		if (error != 0)
			return error;
	}
	if (params->FrameRate.Den == 0)
	{
		params->FrameRate.Num = 30;
		params->FrameRate.Den = 1;
	}
	return h264_level_mmal(params->Level) != 0 ? 0 : VCERR_BADPARAM;
}

/**
 * encoder_create() - Create the encoder component and take its ports.
 * @session: the session, its parameters checked
 * @jpeg:    JPEG, else H.264
 *
 * An H.264 encoder is told, before its formats, not to send the headers in a
 * buffer of their own at the enable: they come with the first frame.
 *
 * Return: 0 or the error.
 */
static LONG encoder_create(struct vc_session *session, BOOL jpeg)
{
	LONG error = component_open(session, jpeg ? "ril.image_encode" : "ril.video_encode", &session->encoder, &session->encoder_in,
	                            &session->encoder_out);
	if (error != 0)
		return error;
	session->input_port = session->encoder_in;
	session->output_port = session->encoder_out;
	if (session->tunnelled)
	{
		error = isp_open(session);
		if (error != 0)
			return error;
		session->input_port = session->isp_in;
	}
	if (!jpeg)
		(void)mmal_port_param_set_u32(session->base, session->encoder->control, MMAL_PARAMETER_VIDEO_ENCODE_HEADER_ON_OPEN, 0);
	return 0;
}

/**
 * encoder_packets() - The backend's own output packets.
 * @session: the session, the output format committed
 * @jpeg:    JPEG, else H.264
 *
 * VCA_OutputCount and VCA_OutputSize, else what the VPU recommends; a JPEG
 * packet holds at least a byte a pixel, a whole picture's worth.
 *
 * Return: 0 or VCERR_NOMEM.
 */
static LONG encoder_packets(struct vc_session *session, BOOL jpeg)
{
	const struct VCSessionParams *params = &session->params;
	const struct mmal_port *output = session->encoder_out;
	u32 count = output_set_count(session, clamp_val(umax(output->buffer_num_recommended, output->buffer_num_min), 2, 8));
	u32 size = umax(output->buffer_size_recommended, output->buffer_size_min);
	if (jpeg)
		size = umax(size, params->Width * params->Height);
	if (params->OutputSize != 0)
		size = params->OutputSize;
	Kprintf("[videocore] encoder: %ldx%ld %s, output buffers min %ld x %ld, recommended %ld x %ld, %ld x %ld used\n", params->Width,
	        params->Height, jpeg ? "to JPEG" : "to H.264", output->buffer_num_min, output->buffer_size_min,
	        output->buffer_num_recommended, output->buffer_size_recommended, count, size);
	return set_fill(session, &session->output_set, FALSE, count, size);
}

/**
 * encoder_open() - Open an encoder for H.264 or JPEG.
 * @session: an encoder session
 *
 * The parameters, the component, its formats and settings, then both sets:
 * the backend's own input pictures and output packets until the client
 * imports.
 *
 * Return: 0 or the error.
 */
static LONG encoder_open(struct vc_session *session)
{
	BOOL jpeg = session->params.Codec == VCC_JPEG;
	LONG error = encoder_params(session, jpeg);
	if (error == 0)
		error = encoder_create(session, jpeg);
	if (error == 0)
		error = encoder_formats(session, jpeg);
	if (error == 0)
		error = jpeg ? jpeg_settings(session) : h264_settings(session);
	if (error == 0)
		error = input_set_fill(session);
	if (error == 0)
		error = encoder_packets(session, jpeg);
	return error;
}

/**
 * encoder_tunnel_run() - Connect the ISP to the encoder and run everything but the ISP's input.
 * @session: a tunnelled encoder session, both sets attached
 *
 * The VPU enables both ends of a tunnel at the first enable, here the
 * encoder's input, and then the encoder's output, input before output as for
 * any component.  Nothing moves before the ISP's input gets a picture.
 *
 * Return: 0 or the first error.
 */
static LONG encoder_tunnel_run(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	LONG error = mmal_port_connect(base, session->isp_out, session->encoder_in);
	if (error == 0)
		error = mmal_component_enable(base, session->isp);
	if (error == 0)
		error = component_run(session, session->encoder, session->encoder_in, session->encoder_out);
	if (error == 0)
		error = mmal_port_enable(base, session->isp_out); /* a no-op: its state was read back */
	return error;
}

/**
 * encoder_start() - The first send: both sets attached, the component and its ports running, the packets out.
 * @session: an encoder session, its sets final
 *
 * Return: 0 or the error.
 */
static LONG encoder_start(struct vc_session *session)
{
	LONG error = set_attach_checked(session, &session->input_set, session->input_port, "input");
	if (error == 0)
		error = set_attach_checked(session, &session->output_set, session->encoder_out, "output");
	if (error == 0)
		error = session->tunnelled ? encoder_tunnel_run(session)
		                           : component_run(session, session->encoder, session->encoder_in, session->encoder_out);
	if (error != 0)
		return error;
	session->output_state = OUTPUT_RUNNING;
	Kprintf("[videocore] encoder running%s: %ld input pictures, %ld packets of %ld bytes\n", session->tunnelled ? " behind the ISP" : "",
	        session->input_set.count, session->output_set.count, session->output_set.size);
	error = set_send_output(session, &session->output_set);
	if (error == 0 && session->tunnelled)
		error = mmal_port_enable(session->base, session->isp_in);
	return error;
}

/**
 * encoder_output_item() - An encoded packet for the client.
 * @session: the session
 * @object:  the packet object, back from the VPU
 * @item:    the poll's item
 */
static void encoder_output_item(struct vc_session *session, struct vc_object *object, struct VCItem *item)
{
	(void)session;
	output_item_packet(object, item);
}

/**
 * encoder_set_attrs() - An H.264 encoder's running controls: the bit rate, a key frame on request.
 * @session: an encoder session
 * @changes: the controls the client set
 *
 * Return: 0, VCERR_UNSUPPORTED for a JPEG encoder or any other change, or the error.
 */
static LONG encoder_set_attrs(struct vc_session *session, const struct VCSessionParams *changes)
{
	if (!(changes->Changed & (VCSPF_Bitrate | VCSPF_RequestKeyFrame)) || session->params.Codec != VCC_H264)
		return VCERR_UNSUPPORTED;
	LONG error = 0;
	if ((changes->Changed & VCSPF_Bitrate) && changes->Bitrate != 0)
	{
		error = mmal_port_param_set_u32(session->base, session->encoder_out, MMAL_PARAMETER_VIDEO_BIT_RATE, changes->Bitrate);
		if (error == 0)
			session->params.Bitrate = changes->Bitrate;
	}
	if (error == 0 && (changes->Changed & VCSPF_RequestKeyFrame) && changes->RequestKeyFrame)
		error = mmal_port_param_set_u32(session->base, session->encoder_out, MMAL_PARAMETER_VIDEO_REQUEST_I_FRAME, 1);
	return error;
}

const struct session_ops encoder_ops = {
    .open = encoder_open,
    .start = encoder_start,
    .send_frame = input_frame_send,
    .output_item = encoder_output_item,
    .set_attrs = encoder_set_attrs,
};
