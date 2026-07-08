#include "ffmpeg-video-codec.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <sstream>

namespace comp_delay {

namespace {

constexpr AVRational kNsTimeBase = {1, 1000000000};

AVCodecContext *asCodecContext(void *context)
{
	return static_cast<AVCodecContext *>(context);
}

AVFrame *asFrame(void *frame)
{
	return static_cast<AVFrame *>(frame);
}

AVPacket *asPacket(void *packet)
{
	return static_cast<AVPacket *>(packet);
}

SwsContext *asScaleContext(void *context)
{
	return static_cast<SwsContext *>(context);
}

std::string ffmpegError(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}

uint64_t rescaleToNs(int64_t value, AVRational sourceTimeBase)
{
	if (value == AV_NOPTS_VALUE)
		return 0;
	return static_cast<uint64_t>(std::max<int64_t>(0, av_rescale_q(value, sourceTimeBase, kNsTimeBase)));
}

int64_t nsToPts(uint64_t timestampNs, AVRational destinationTimeBase)
{
	return av_rescale_q(static_cast<int64_t>(timestampNs), kNsTimeBase, destinationTimeBase);
}

uint64_t frameDurationNs(uint32_t fpsNumerator, uint32_t fpsDenominator)
{
	if (fpsNumerator == 0)
		return 16666667ULL;
	return (1000000000ULL * static_cast<uint64_t>(std::max<uint32_t>(fpsDenominator, 1))) /
	       static_cast<uint64_t>(fpsNumerator);
}

int gopSize(uint32_t fpsNumerator, uint32_t fpsDenominator, uint32_t keyframeSeconds)
{
	if (fpsNumerator == 0)
		return static_cast<int>(std::max<uint32_t>(keyframeSeconds, 1) * 60U);
	const uint32_t fps = std::max<uint32_t>(1, fpsNumerator / std::max<uint32_t>(fpsDenominator, 1));
	return static_cast<int>(std::max<uint32_t>(keyframeSeconds, 1) * fps);
}

AVPixelFormat selectPixelFormat(const AVCodec *codec)
{
	static constexpr AVPixelFormat preferred[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P};
	if (!codec)
		return AV_PIX_FMT_YUV420P;

	const void *configs = nullptr;
	int configCount = 0;
	if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &configCount) < 0 ||
	    !configs || configCount <= 0)
		return AV_PIX_FMT_YUV420P;

	const auto *formats = static_cast<const AVPixelFormat *>(configs);
	for (const AVPixelFormat format : preferred) {
		for (int i = 0; i < configCount; ++i) {
			if (formats[i] == format)
				return format;
		}
	}

	return formats[0];
}

std::string encoderDisplayName(const AVCodec *codec)
{
	if (!codec)
		return {};

	std::ostringstream out;
	out << codec->name;
	if (codec->long_name && *codec->long_name)
		out << " - " << codec->long_name;
	return out.str();
}

bool hasSampleFormat(const AVCodec *codec, AVSampleFormat desired);

std::vector<FfmpegEncoderInfo> listEncoders(AVMediaType mediaType, AVCodecID codecId)
{
	std::vector<FfmpegEncoderInfo> encoders;
	void *iterator = nullptr;
	const AVCodec *codec = nullptr;
	while ((codec = av_codec_iterate(&iterator)) != nullptr) {
		if (!av_codec_is_encoder(codec) || codec->type != mediaType || codec->id != codecId)
			continue;
		encoders.push_back({codec->name, encoderDisplayName(codec)});
	}

	std::sort(encoders.begin(), encoders.end(), [](const FfmpegEncoderInfo &left, const FfmpegEncoderInfo &right) {
		return left.name < right.name;
	});
	return encoders;
}

bool canOpenH264Encoder(const AVCodec *codec)
{
	if (!codec || !av_codec_is_encoder(codec) || codec->type != AVMEDIA_TYPE_VIDEO || codec->id != AV_CODEC_ID_H264)
		return false;

	AVCodecContext *context = avcodec_alloc_context3(codec);
	if (!context)
		return false;

	context->codec_type = AVMEDIA_TYPE_VIDEO;
	context->codec_id = AV_CODEC_ID_H264;
	context->width = 128;
	context->height = 128;
	context->time_base = kNsTimeBase;
	context->framerate = {30, 1};
	context->pix_fmt = selectPixelFormat(codec);
	context->bit_rate = 1000000;
	context->gop_size = 30;
	context->max_b_frames = 0;

	av_opt_set(context->priv_data, "preset", "veryfast", 0);
	av_opt_set(context->priv_data, "tune", "zerolatency", 0);
	av_opt_set(context->priv_data, "repeat-headers", "1", 0);
	av_opt_set(context->priv_data, "sc_threshold", "0", 0);

	const int result = avcodec_open2(context, codec, nullptr);
	avcodec_free_context(&context);
	return result >= 0;
}

bool canOpenAacEncoder(const AVCodec *codec)
{
	if (!codec || !av_codec_is_encoder(codec) || codec->type != AVMEDIA_TYPE_AUDIO || codec->id != AV_CODEC_ID_AAC ||
	    !hasSampleFormat(codec, AV_SAMPLE_FMT_FLTP))
		return false;

	AVCodecContext *context = avcodec_alloc_context3(codec);
	if (!context)
		return false;

	context->codec_type = AVMEDIA_TYPE_AUDIO;
	context->codec_id = AV_CODEC_ID_AAC;
	context->sample_rate = 48000;
	context->sample_fmt = AV_SAMPLE_FMT_FLTP;
	context->bit_rate = 160000;
	context->time_base = {1, context->sample_rate};
	av_channel_layout_default(&context->ch_layout, 2);

	const int result = avcodec_open2(context, codec, nullptr);
	avcodec_free_context(&context);
	return result >= 0;
}

bool hasEncoder(const std::string &name, AVMediaType mediaType, AVCodecID codecId)
{
	if (name.empty())
		return false;
	const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
	return codec && av_codec_is_encoder(codec) && codec->type == mediaType && codec->id == codecId;
}

bool hasSampleFormat(const AVCodec *codec, AVSampleFormat desired)
{
	if (!codec)
		return false;

	const void *configs = nullptr;
	int configCount = 0;
	if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &configCount) < 0 ||
	    !configs || configCount <= 0)
		return true;

	const auto *formats = static_cast<const AVSampleFormat *>(configs);
	for (int i = 0; i < configCount; ++i) {
		if (formats[i] == desired)
			return true;
	}

	return false;
}

AVSampleFormat selectSampleFormat(const AVCodec *codec)
{
	static constexpr AVSampleFormat preferred[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT};
	if (!codec)
		return AV_SAMPLE_FMT_FLTP;

	const void *configs = nullptr;
	int configCount = 0;
	if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &configCount) < 0 ||
	    !configs || configCount <= 0)
		return AV_SAMPLE_FMT_FLTP;

	const auto *formats = static_cast<const AVSampleFormat *>(configs);
	for (const AVSampleFormat format : preferred) {
		for (int i = 0; i < configCount; ++i) {
			if (formats[i] == format)
				return format;
		}
	}

	return formats[0];
}

size_t channelCount(enum speaker_layout speakers)
{
	return get_audio_channels(speakers == SPEAKERS_UNKNOWN ? SPEAKERS_STEREO : speakers);
}

enum speaker_layout speakersForChannels(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_STEREO;
	}
}

uint64_t audioDurationNs(uint32_t frames, uint32_t sampleRate)
{
	if (sampleRate == 0)
		return 0;
	return (1000000000ULL * static_cast<uint64_t>(frames)) / static_cast<uint64_t>(sampleRate);
}

} // namespace

std::vector<FfmpegEncoderInfo> listFfmpegVideoEncoders()
{
	std::vector<FfmpegEncoderInfo> encoders;
	for (const auto &encoder : listEncoders(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264)) {
		if (canOpenH264Encoder(avcodec_find_encoder_by_name(encoder.name.c_str())))
			encoders.push_back(encoder);
	}
	return encoders;
}

std::vector<FfmpegEncoderInfo> listFfmpegAudioEncoders()
{
	std::vector<FfmpegEncoderInfo> encoders;
	for (const auto &encoder : listEncoders(AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC)) {
		const AVCodec *codec = avcodec_find_encoder_by_name(encoder.name.c_str());
		if (canOpenAacEncoder(codec))
			encoders.push_back(encoder);
	}
	return encoders;
}

bool isFfmpegVideoEncoderAvailable(const std::string &name)
{
	if (!hasEncoder(name, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264))
		return false;

	return canOpenH264Encoder(avcodec_find_encoder_by_name(name.c_str()));
}

bool isFfmpegAudioEncoderAvailable(const std::string &name)
{
	if (!hasEncoder(name, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC))
		return false;

	return canOpenAacEncoder(avcodec_find_encoder_by_name(name.c_str()));
}

FfmpegVideoEncoder::FfmpegVideoEncoder()
{
	status_ = "Idle";
}

FfmpegVideoEncoder::~FfmpegVideoEncoder()
{
	close();
}

bool FfmpegVideoEncoder::open(uint32_t width, uint32_t height, uint32_t fpsNumerator, uint32_t fpsDenominator,
			      const DelaySettings &settings)
{
	close();

	if (width == 0 || height == 0) {
		setError("Invalid video size");
		return false;
	}

	const std::string encoderName =
		settings.videoEncoderName.empty() ? std::string(kDefaultVideoEncoderName) : settings.videoEncoderName;
	const AVCodec *codec = avcodec_find_encoder_by_name(encoderName.c_str());
	if (!codec || !av_codec_is_encoder(codec) || codec->type != AVMEDIA_TYPE_VIDEO || codec->id != AV_CODEC_ID_H264) {
		setError("Selected FFmpeg video encoder is unavailable or not H.264: " + encoderName);
		return false;
	}

	auto *context = avcodec_alloc_context3(codec);
	auto *frame = av_frame_alloc();
	auto *packet = av_packet_alloc();
	if (!context || !frame || !packet) {
		if (context)
			avcodec_free_context(&context);
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		setError("Could not allocate FFmpeg video encoder buffers");
		return false;
	}

	context->codec_type = AVMEDIA_TYPE_VIDEO;
	context->codec_id = AV_CODEC_ID_H264;
	context->width = static_cast<int>(width);
	context->height = static_cast<int>(height);
	context->time_base = kNsTimeBase;
	context->framerate = {static_cast<int>(fpsNumerator ? fpsNumerator : 60), static_cast<int>(fpsDenominator ? fpsDenominator : 1)};
	context->pix_fmt = selectPixelFormat(codec);
	context->bit_rate = static_cast<int64_t>(settings.videoBitrateKbps) * 1000;
	context->gop_size = gopSize(fpsNumerator, fpsDenominator, settings.keyframeIntervalSeconds);
	context->max_b_frames = 0;

	av_opt_set(context->priv_data, "preset", "veryfast", 0);
	av_opt_set(context->priv_data, "tune", "zerolatency", 0);
	av_opt_set(context->priv_data, "repeat-headers", "1", 0);
	av_opt_set(context->priv_data, "sc_threshold", "0", 0);

	int result = avcodec_open2(context, codec, nullptr);
	if (result < 0) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Could not open FFmpeg H.264 encoder: " + ffmpegError(result));
		return false;
	}

	frame->format = context->pix_fmt;
	frame->width = context->width;
	frame->height = context->height;
	result = av_frame_get_buffer(frame, 32);
	if (result < 0) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Could not allocate FFmpeg video frame: " + ffmpegError(result));
		return false;
	}

	codecContext_ = context;
	frame_ = frame;
	packet_ = packet;
	width_ = width;
	height_ = height;
	frameDurationNs_ = frameDurationNs(fpsNumerator, fpsDenominator);
	active_ = true;
	status_ = "FFmpeg video encoder active: " + encoderName;
	return true;
}

void FfmpegVideoEncoder::close()
{
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	auto *packet = asPacket(packet_);
	auto *scale = asScaleContext(scaleContext_);

	if (scale)
		sws_freeContext(scale);
	if (packet)
		av_packet_free(&packet);
	if (frame)
		av_frame_free(&frame);
	if (context)
		avcodec_free_context(&context);

	codecContext_ = nullptr;
	frame_ = nullptr;
	packet_ = nullptr;
	scaleContext_ = nullptr;
	width_ = 0;
	height_ = 0;
	frameDurationNs_ = 0;
	active_ = false;
	status_ = "Stopped";
}

bool FfmpegVideoEncoder::active() const
{
	return active_;
}

const std::string &FfmpegVideoEncoder::status() const
{
	return status_;
}

std::vector<EncodedPacket> FfmpegVideoEncoder::encode(const RawVideoFrame &rawFrame)
{
	if (!active_ || !codecContext_ || !frame_ || !packet_)
		return {};
	if (rawFrame.data.empty() || rawFrame.width != width_ || rawFrame.height != height_)
		return {};

	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);

	int result = av_frame_make_writable(frame);
	if (result < 0) {
		setError("FFmpeg video frame is not writable: " + ffmpegError(result));
		return {};
	}

	auto *scale = sws_getCachedContext(asScaleContext(scaleContext_), static_cast<int>(width_), static_cast<int>(height_),
					   AV_PIX_FMT_RGBA, static_cast<int>(width_), static_cast<int>(height_),
					   context->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!scale) {
		setError("Could not create FFmpeg RGBA to YUV scaler");
		return {};
	}
	scaleContext_ = scale;

	const uint8_t *sourceData[4] = {rawFrame.data.data(), nullptr, nullptr, nullptr};
	const int sourceLineSize[4] = {static_cast<int>(rawFrame.lineSize), 0, 0, 0};
	sws_scale(scale, sourceData, sourceLineSize, 0, static_cast<int>(height_), frame->data, frame->linesize);
	frame->pts = nsToPts(rawFrame.timestampNs, context->time_base);

	result = avcodec_send_frame(context, frame);
	if (result < 0) {
		setError("Could not encode H.264 video frame: " + ffmpegError(result));
		return {};
	}

	return receivePackets();
}

std::vector<EncodedPacket> FfmpegVideoEncoder::flush()
{
	if (!active_ || !codecContext_)
		return {};

	int result = avcodec_send_frame(asCodecContext(codecContext_), nullptr);
	if (result < 0 && result != AVERROR_EOF) {
		setError("Could not flush H.264 encoder: " + ffmpegError(result));
		return {};
	}

	return receivePackets();
}

std::vector<EncodedPacket> FfmpegVideoEncoder::receivePackets()
{
	std::vector<EncodedPacket> packets;
	auto *context = asCodecContext(codecContext_);
	auto *packet = asPacket(packet_);
	if (!context || !packet)
		return packets;

	for (;;) {
		const int result = avcodec_receive_packet(context, packet);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0) {
			setError("Could not receive H.264 packet: " + ffmpegError(result));
			break;
		}

		EncodedPacket encoded;
		encoded.kind = EncodedPacketKind::Video;
		const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		encoded.timestampNs = rescaleToNs(timestamp, context->time_base);
		encoded.durationNs = packet->duration > 0 ? rescaleToNs(packet->duration, context->time_base) : frameDurationNs_;
		encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
		encoded.data.assign(packet->data, packet->data + packet->size);
		packets.push_back(std::move(encoded));
		av_packet_unref(packet);
	}

	return packets;
}

void FfmpegVideoEncoder::setError(const std::string &message)
{
	status_ = message;
	active_ = false;
}

FfmpegVideoDecoder::FfmpegVideoDecoder()
{
	status_ = "Idle";
}

FfmpegVideoDecoder::~FfmpegVideoDecoder()
{
	close();
}

bool FfmpegVideoDecoder::open()
{
	close();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		setError("FFmpeg H.264 decoder is unavailable");
		return false;
	}

	auto *context = avcodec_alloc_context3(codec);
	auto *frame = av_frame_alloc();
	auto *packet = av_packet_alloc();
	if (!context || !frame || !packet) {
		if (context)
			avcodec_free_context(&context);
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		setError("Could not allocate FFmpeg video decoder buffers");
		return false;
	}

	context->time_base = kNsTimeBase;
	const int result = avcodec_open2(context, codec, nullptr);
	if (result < 0) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Could not open FFmpeg H.264 decoder: " + ffmpegError(result));
		return false;
	}

	codecContext_ = context;
	frame_ = frame;
	packet_ = packet;
	active_ = true;
	status_ = "FFmpeg H.264 video decoder active";
	return true;
}

void FfmpegVideoDecoder::close()
{
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	auto *packet = asPacket(packet_);
	auto *scale = asScaleContext(scaleContext_);

	if (scale)
		sws_freeContext(scale);
	if (packet)
		av_packet_free(&packet);
	if (frame)
		av_frame_free(&frame);
	if (context)
		avcodec_free_context(&context);

	codecContext_ = nullptr;
	frame_ = nullptr;
	packet_ = nullptr;
	scaleContext_ = nullptr;
	active_ = false;
	status_ = "Stopped";
}

void FfmpegVideoDecoder::reset()
{
	if (codecContext_)
		avcodec_flush_buffers(asCodecContext(codecContext_));
	auto *scale = asScaleContext(scaleContext_);
	if (scale)
		sws_freeContext(scale);
	scaleContext_ = nullptr;
}

bool FfmpegVideoDecoder::active() const
{
	return active_;
}

const std::string &FfmpegVideoDecoder::status() const
{
	return status_;
}

std::vector<RawVideoFrame> FfmpegVideoDecoder::decode(const EncodedPacket &encodedPacket)
{
	if (!active_ || !codecContext_ || !packet_ || encodedPacket.kind != EncodedPacketKind::Video ||
	    encodedPacket.data.empty())
		return {};

	auto *context = asCodecContext(codecContext_);
	auto *packet = asPacket(packet_);
	av_packet_unref(packet);

	int result = av_new_packet(packet, static_cast<int>(encodedPacket.data.size()));
	if (result < 0) {
		setError("Could not allocate H.264 decode packet: " + ffmpegError(result));
		return {};
	}
	std::memcpy(packet->data, encodedPacket.data.data(), encodedPacket.data.size());
	packet->pts = nsToPts(encodedPacket.timestampNs, context->time_base);
	packet->dts = packet->pts;
	packet->duration = nsToPts(encodedPacket.durationNs, context->time_base);

	result = avcodec_send_packet(context, packet);
	av_packet_unref(packet);
	if (result < 0) {
		setError("Could not send H.264 packet to decoder: " + ffmpegError(result));
		return {};
	}

	return receiveFrames();
}

std::vector<RawVideoFrame> FfmpegVideoDecoder::flush()
{
	if (!active_ || !codecContext_)
		return {};

	int result = avcodec_send_packet(asCodecContext(codecContext_), nullptr);
	if (result < 0 && result != AVERROR_EOF) {
		setError("Could not flush H.264 decoder: " + ffmpegError(result));
		return {};
	}

	return receiveFrames();
}

std::vector<RawVideoFrame> FfmpegVideoDecoder::receiveFrames()
{
	std::vector<RawVideoFrame> frames;
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	if (!context || !frame)
		return frames;

	for (;;) {
		const int result = avcodec_receive_frame(context, frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0) {
			setError("Could not receive H.264 frame: " + ffmpegError(result));
			break;
		}

		auto *scale = sws_getCachedContext(asScaleContext(scaleContext_), frame->width, frame->height,
						   static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
						   AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!scale) {
			setError("Could not create FFmpeg video decoder scaler");
			av_frame_unref(frame);
			break;
		}
		scaleContext_ = scale;

		RawVideoFrame decoded;
		decoded.width = static_cast<uint32_t>(frame->width);
		decoded.height = static_cast<uint32_t>(frame->height);
		decoded.lineSize = decoded.width * 4;
		decoded.format = VIDEO_FORMAT_RGBA;
		decoded.data.resize(static_cast<size_t>(decoded.lineSize) * decoded.height);

		uint8_t *destinationData[4] = {decoded.data.data(), nullptr, nullptr, nullptr};
		const int destinationLineSize[4] = {static_cast<int>(decoded.lineSize), 0, 0, 0};
		sws_scale(scale, frame->data, frame->linesize, 0, frame->height, destinationData, destinationLineSize);

		const int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
											 : frame->pts;
		decoded.timestampNs = rescaleToNs(timestamp, context->time_base);
		frames.push_back(std::move(decoded));
		av_frame_unref(frame);
	}

	return frames;
}

void FfmpegVideoDecoder::setError(const std::string &message)
{
	status_ = message;
	active_ = false;
}

FfmpegAudioEncoder::FfmpegAudioEncoder()
{
	status_ = "Idle";
}

FfmpegAudioEncoder::~FfmpegAudioEncoder()
{
	close();
}

bool FfmpegAudioEncoder::open(uint32_t sampleRate, enum speaker_layout speakers, const DelaySettings &settings)
{
	close();

	const std::string encoderName =
		settings.audioEncoderName.empty() ? std::string(kDefaultAudioEncoderName) : settings.audioEncoderName;
	const AVCodec *codec = avcodec_find_encoder_by_name(encoderName.c_str());
	if (!codec || !av_codec_is_encoder(codec) || codec->type != AVMEDIA_TYPE_AUDIO || codec->id != AV_CODEC_ID_AAC) {
		setError("Selected FFmpeg audio encoder is unavailable or not AAC: " + encoderName);
		return false;
	}

	auto *context = avcodec_alloc_context3(codec);
	auto *frame = av_frame_alloc();
	auto *packet = av_packet_alloc();
	if (!context || !frame || !packet) {
		if (context)
			avcodec_free_context(&context);
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		setError("Could not allocate FFmpeg audio encoder buffers");
		return false;
	}

	const size_t channels = std::max<size_t>(1, channelCount(speakers));
	context->codec_type = AVMEDIA_TYPE_AUDIO;
	context->codec_id = AV_CODEC_ID_AAC;
	context->sample_rate = static_cast<int>(sampleRate ? sampleRate : 48000);
	context->sample_fmt = selectSampleFormat(codec);
	context->bit_rate = static_cast<int64_t>(settings.audioBitrateKbps) * 1000;
	context->time_base = {1, context->sample_rate};
	av_channel_layout_default(&context->ch_layout, static_cast<int>(channels));

	if (context->sample_fmt != AV_SAMPLE_FMT_FLTP) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Selected AAC encoder does not support planar float audio: " + encoderName);
		return false;
	}

	int result = avcodec_open2(context, codec, nullptr);
	if (result < 0) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Could not open FFmpeg AAC encoder: " + ffmpegError(result));
		return false;
	}

	if (context->extradata && context->extradata_size > 0) {
		codecConfig_.assign(context->extradata, context->extradata + context->extradata_size);
	}

	codecContext_ = context;
	frame_ = frame;
	packet_ = packet;
	sampleRate_ = static_cast<uint32_t>(context->sample_rate);
	speakers_ = speakers == SPEAKERS_UNKNOWN ? SPEAKERS_STEREO : speakers;
	encoderFrameSize_ = static_cast<uint32_t>(std::max(context->frame_size, 1));
	fallbackFrameDurationNs_ = audioDurationNs(encoderFrameSize_, sampleRate_);
	pendingPlanes_.assign(channels, {});
	pendingFrames_ = 0;
	havePendingTimestamp_ = false;
	pendingTimestampNs_ = 0;
	active_ = true;
	status_ = "FFmpeg audio encoder active: " + encoderName;
	return true;
}

void FfmpegAudioEncoder::close()
{
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	auto *packet = asPacket(packet_);

	if (packet)
		av_packet_free(&packet);
	if (frame)
		av_frame_free(&frame);
	if (context)
		avcodec_free_context(&context);

	codecContext_ = nullptr;
	frame_ = nullptr;
	packet_ = nullptr;
	sampleRate_ = 48000;
	speakers_ = SPEAKERS_STEREO;
	encoderFrameSize_ = 0;
	pendingFrames_ = 0;
	havePendingTimestamp_ = false;
	pendingTimestampNs_ = 0;
	fallbackFrameDurationNs_ = 0;
	active_ = false;
	codecConfig_.clear();
	pendingPlanes_.clear();
	status_ = "Stopped";
}

bool FfmpegAudioEncoder::active() const
{
	return active_;
}

const std::string &FfmpegAudioEncoder::status() const
{
	return status_;
}

uint32_t FfmpegAudioEncoder::frameSize() const
{
	const auto *context = asCodecContext(codecContext_);
	return context && context->frame_size > 0 ? static_cast<uint32_t>(context->frame_size) : 0;
}

std::vector<EncodedPacket> FfmpegAudioEncoder::encode(const RawAudioFrame &rawFrame)
{
	if (!active_ || !codecContext_ || !frame_ || !packet_ || rawFrame.planes.empty())
		return {};

	auto *context = asCodecContext(codecContext_);
	const size_t channels = static_cast<size_t>(context->ch_layout.nb_channels);
	if (rawFrame.format != AUDIO_FORMAT_FLOAT_PLANAR || rawFrame.samplesPerSec != sampleRate_ ||
	    rawFrame.planes.size() < channels || rawFrame.frames == 0)
		return {};

	const size_t bytesPerPlane = static_cast<size_t>(rawFrame.frames) * sizeof(float);
	for (size_t i = 0; i < channels; ++i) {
		if (rawFrame.planes[i].size() < bytesPerPlane)
			return {};
	}

	if (pendingPlanes_.size() != channels)
		pendingPlanes_.assign(channels, {});
	if (!havePendingTimestamp_) {
		pendingTimestampNs_ = rawFrame.timestampNs;
		havePendingTimestamp_ = true;
	}

	for (size_t i = 0; i < channels; ++i) {
		auto &pending = pendingPlanes_[i];
		pending.insert(pending.end(), rawFrame.planes[i].begin(), rawFrame.planes[i].begin() + bytesPerPlane);
	}
	pendingFrames_ += rawFrame.frames;

	return drainPendingFrames(false);
}

std::vector<EncodedPacket> FfmpegAudioEncoder::drainPendingFrames(bool flushRemaining)
{
	std::vector<EncodedPacket> packets;
	const uint32_t frameSize = encoderFrameSize_ ? encoderFrameSize_ : 1;
	while (pendingFrames_ >= frameSize) {
		auto encoded = encodePendingFrame(frameSize, false);
		packets.insert(packets.end(), encoded.begin(), encoded.end());
		if (!active_)
			return packets;
	}

	if (flushRemaining && pendingFrames_ > 0) {
		auto encoded = encodePendingFrame(frameSize, true);
		packets.insert(packets.end(), encoded.begin(), encoded.end());
	}

	return packets;
}

std::vector<EncodedPacket> FfmpegAudioEncoder::encodePendingFrame(uint32_t frames, bool padSilence)
{
	if (!havePendingTimestamp_ || frames == 0 || pendingPlanes_.empty())
		return {};

	RawAudioFrame frame;
	frame.frames = frames;
	frame.timestampNs = pendingTimestampNs_;
	frame.samplesPerSec = sampleRate_;
	frame.speakers = speakers_;
	frame.format = AUDIO_FORMAT_FLOAT_PLANAR;
	frame.planes.resize(pendingPlanes_.size());

	const size_t bytesPerPlane = static_cast<size_t>(frames) * sizeof(float);
	for (size_t i = 0; i < pendingPlanes_.size(); ++i) {
		auto &pending = pendingPlanes_[i];
		const size_t copyBytes = std::min(bytesPerPlane, pending.size());
		if (!padSilence && copyBytes < bytesPerPlane)
			return {};

		frame.planes[i].assign(bytesPerPlane, 0);
		if (copyBytes > 0)
			std::memcpy(frame.planes[i].data(), pending.data(), copyBytes);

		const size_t eraseBytes = std::min(bytesPerPlane, pending.size());
		pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(eraseBytes));
	}

	const uint32_t consumedFrames = std::min(frames, pendingFrames_);
	pendingFrames_ -= consumedFrames;
	pendingTimestampNs_ += audioDurationNs(frames, sampleRate_);
	if (pendingFrames_ == 0) {
		havePendingTimestamp_ = false;
		pendingTimestampNs_ = 0;
	}

	return encodeFrame(frame);
}

std::vector<EncodedPacket> FfmpegAudioEncoder::encodeFrame(const RawAudioFrame &rawFrame)
{
	if (!active_ || !codecContext_ || !frame_ || !packet_ || rawFrame.planes.empty())
		return {};

	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	const size_t channels = static_cast<size_t>(context->ch_layout.nb_channels);
	if (rawFrame.format != AUDIO_FORMAT_FLOAT_PLANAR || rawFrame.samplesPerSec != sampleRate_ ||
	    rawFrame.planes.size() < channels || rawFrame.frames == 0)
		return {};

	av_frame_unref(frame);
	frame->nb_samples = static_cast<int>(rawFrame.frames);
	frame->format = context->sample_fmt;
	frame->sample_rate = context->sample_rate;
	frame->duration = static_cast<int64_t>(rawFrame.frames);
	av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);

	int result = av_frame_get_buffer(frame, 0);
	if (result < 0) {
		setError("Could not allocate FFmpeg audio frame: " + ffmpegError(result));
		return {};
	}

	result = av_frame_make_writable(frame);
	if (result < 0) {
		setError("FFmpeg audio frame is not writable: " + ffmpegError(result));
		return {};
	}

	const size_t bytesPerPlane = static_cast<size_t>(rawFrame.frames) * sizeof(float);
	for (size_t i = 0; i < channels; ++i)
		std::memcpy(frame->data[i], rawFrame.planes[i].data(), std::min(bytesPerPlane, rawFrame.planes[i].size()));

	frame->pts = nsToPts(rawFrame.timestampNs, context->time_base);

	result = avcodec_send_frame(context, frame);
	if (result < 0) {
		setError("Could not encode AAC audio frame: " + ffmpegError(result));
		return {};
	}

	return receivePackets();
}

std::vector<EncodedPacket> FfmpegAudioEncoder::flush()
{
	if (!active_ || !codecContext_)
		return {};

	auto packets = drainPendingFrames(true);
	if (!active_)
		return packets;

	const int result = avcodec_send_frame(asCodecContext(codecContext_), nullptr);
	if (result < 0 && result != AVERROR_EOF) {
		setError("Could not flush AAC encoder: " + ffmpegError(result));
		return packets;
	}

	auto flushed = receivePackets();
	packets.insert(packets.end(), flushed.begin(), flushed.end());
	return packets;
}

std::vector<EncodedPacket> FfmpegAudioEncoder::receivePackets()
{
	std::vector<EncodedPacket> packets;
	auto *context = asCodecContext(codecContext_);
	auto *packet = asPacket(packet_);
	if (!context || !packet)
		return packets;

	for (;;) {
		const int result = avcodec_receive_packet(context, packet);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0) {
			setError("Could not receive AAC packet: " + ffmpegError(result));
			break;
		}

		EncodedPacket encoded;
		encoded.kind = EncodedPacketKind::Audio;
		const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		encoded.timestampNs = rescaleToNs(timestamp, context->time_base);
		encoded.durationNs =
			packet->duration > 0 ? rescaleToNs(packet->duration, context->time_base) : fallbackFrameDurationNs_;
		encoded.keyframe = false;
		encoded.codecConfig = codecConfig_;
		encoded.data.assign(packet->data, packet->data + packet->size);
		packets.push_back(std::move(encoded));
		av_packet_unref(packet);
	}

	return packets;
}

void FfmpegAudioEncoder::setError(const std::string &message)
{
	status_ = message;
	active_ = false;
}

FfmpegAudioDecoder::FfmpegAudioDecoder()
{
	status_ = "Idle";
}

FfmpegAudioDecoder::~FfmpegAudioDecoder()
{
	close();
}

bool FfmpegAudioDecoder::open(const std::vector<uint8_t> &codecConfig)
{
	close();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!codec) {
		setError("FFmpeg AAC decoder is unavailable");
		return false;
	}

	auto *context = avcodec_alloc_context3(codec);
	auto *frame = av_frame_alloc();
	auto *packet = av_packet_alloc();
	if (!context || !frame || !packet) {
		if (context)
			avcodec_free_context(&context);
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		setError("Could not allocate FFmpeg audio decoder buffers");
		return false;
	}

	if (!codecConfig.empty()) {
		context->extradata = static_cast<uint8_t *>(av_mallocz(codecConfig.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!context->extradata) {
			avcodec_free_context(&context);
			av_frame_free(&frame);
			av_packet_free(&packet);
			setError("Could not allocate AAC decoder config");
			return false;
		}
		std::memcpy(context->extradata, codecConfig.data(), codecConfig.size());
		context->extradata_size = static_cast<int>(codecConfig.size());
	}

	context->time_base = kNsTimeBase;
	const int result = avcodec_open2(context, codec, nullptr);
	if (result < 0) {
		avcodec_free_context(&context);
		av_frame_free(&frame);
		av_packet_free(&packet);
		setError("Could not open FFmpeg AAC decoder: " + ffmpegError(result));
		return false;
	}

	codecContext_ = context;
	frame_ = frame;
	packet_ = packet;
	active_ = true;
	status_ = "FFmpeg AAC audio decoder active";
	return true;
}

void FfmpegAudioDecoder::close()
{
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	auto *packet = asPacket(packet_);

	if (packet)
		av_packet_free(&packet);
	if (frame)
		av_frame_free(&frame);
	if (context)
		avcodec_free_context(&context);

	codecContext_ = nullptr;
	frame_ = nullptr;
	packet_ = nullptr;
	haveNextAudioTimestamp_ = false;
	nextAudioTimestampNs_ = 0;
	active_ = false;
	status_ = "Stopped";
}

void FfmpegAudioDecoder::reset()
{
	if (codecContext_)
		avcodec_flush_buffers(asCodecContext(codecContext_));
	haveNextAudioTimestamp_ = false;
	nextAudioTimestampNs_ = 0;
}

bool FfmpegAudioDecoder::active() const
{
	return active_;
}

const std::string &FfmpegAudioDecoder::status() const
{
	return status_;
}

std::vector<RawAudioFrame> FfmpegAudioDecoder::decode(const EncodedPacket &encodedPacket)
{
	if (!active_ || !codecContext_ || !packet_ || encodedPacket.kind != EncodedPacketKind::Audio ||
	    encodedPacket.data.empty())
		return {};

	auto *context = asCodecContext(codecContext_);
	auto *packet = asPacket(packet_);
	av_packet_unref(packet);

	int result = av_new_packet(packet, static_cast<int>(encodedPacket.data.size()));
	if (result < 0) {
		setError("Could not allocate AAC decode packet: " + ffmpegError(result));
		return {};
	}
	std::memcpy(packet->data, encodedPacket.data.data(), encodedPacket.data.size());
	packet->pts = nsToPts(encodedPacket.timestampNs, context->time_base);
	packet->dts = packet->pts;
	packet->duration = nsToPts(encodedPacket.durationNs, context->time_base);
	if (!haveNextAudioTimestamp_ || encodedPacket.timestampNs > nextAudioTimestampNs_) {
		haveNextAudioTimestamp_ = true;
		nextAudioTimestampNs_ = encodedPacket.timestampNs;
	}

	result = avcodec_send_packet(context, packet);
	av_packet_unref(packet);
	if (result < 0) {
		setError("Could not send AAC packet to decoder: " + ffmpegError(result));
		return {};
	}

	return receiveFrames();
}

std::vector<RawAudioFrame> FfmpegAudioDecoder::flush()
{
	if (!active_ || !codecContext_)
		return {};

	const int result = avcodec_send_packet(asCodecContext(codecContext_), nullptr);
	if (result < 0 && result != AVERROR_EOF) {
		setError("Could not flush AAC decoder: " + ffmpegError(result));
		return {};
	}

	return receiveFrames();
}

std::vector<RawAudioFrame> FfmpegAudioDecoder::receiveFrames()
{
	std::vector<RawAudioFrame> frames;
	auto *context = asCodecContext(codecContext_);
	auto *frame = asFrame(frame_);
	if (!context || !frame)
		return frames;

	for (;;) {
		const int result = avcodec_receive_frame(context, frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0) {
			setError("Could not receive AAC frame: " + ffmpegError(result));
			break;
		}

		if (static_cast<AVSampleFormat>(frame->format) != AV_SAMPLE_FMT_FLTP) {
			setError("AAC decoder returned unsupported sample format");
			av_frame_unref(frame);
			break;
		}

		RawAudioFrame decoded;
		decoded.frames = static_cast<uint32_t>(frame->nb_samples);
		decoded.samplesPerSec = static_cast<uint32_t>(frame->sample_rate);
		decoded.speakers = speakersForChannels(frame->ch_layout.nb_channels);
		decoded.format = AUDIO_FORMAT_FLOAT_PLANAR;
		decoded.planes.resize(static_cast<size_t>(frame->ch_layout.nb_channels));

		if (frame->pts != AV_NOPTS_VALUE)
			decoded.timestampNs = rescaleToNs(frame->pts, context->time_base);
		else if (haveNextAudioTimestamp_)
			decoded.timestampNs = nextAudioTimestampNs_;
		else
			decoded.timestampNs = 0;

		const size_t bytesPerPlane = static_cast<size_t>(frame->nb_samples) * sizeof(float);
		for (size_t i = 0; i < decoded.planes.size(); ++i) {
			decoded.planes[i].resize(bytesPerPlane);
			std::memcpy(decoded.planes[i].data(), frame->data[i], bytesPerPlane);
		}

		frames.push_back(std::move(decoded));
		haveNextAudioTimestamp_ = true;
		nextAudioTimestampNs_ = frames.back().timestampNs + audioDurationNs(frames.back().frames, frames.back().samplesPerSec);
		av_frame_unref(frame);
	}

	return frames;
}

void FfmpegAudioDecoder::setError(const std::string &message)
{
	status_ = message;
	active_ = false;
}

} // namespace comp_delay
