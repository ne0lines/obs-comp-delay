#include "codec/ffmpeg-video-codec.hpp"
#include "core/delay-settings.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace comp_delay;

static RawVideoFrame makeFrame(uint64_t timestampNs, uint32_t width, uint32_t height, uint8_t seed)
{
	RawVideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.lineSize = width * 4;
	frame.timestampNs = timestampNs;
	frame.format = VIDEO_FORMAT_RGBA;
	frame.data.resize(static_cast<size_t>(frame.lineSize) * height);

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			const size_t offset = static_cast<size_t>(y) * frame.lineSize + static_cast<size_t>(x) * 4;
			frame.data[offset + 0] = static_cast<uint8_t>((x + seed) & 0xff);
			frame.data[offset + 1] = static_cast<uint8_t>((y + seed) & 0xff);
			frame.data[offset + 2] = static_cast<uint8_t>((x + y + seed) & 0xff);
			frame.data[offset + 3] = 0xff;
		}
	}

	return frame;
}

static RawAudioFrame makeAudioFrame(uint64_t timestampNs, uint32_t frames, float seed)
{
	RawAudioFrame frame;
	frame.frames = frames;
	frame.timestampNs = timestampNs;
	frame.samplesPerSec = 48000;
	frame.speakers = SPEAKERS_STEREO;
	frame.format = AUDIO_FORMAT_FLOAT_PLANAR;
	frame.planes.resize(2);

	for (auto &plane : frame.planes)
		plane.resize(static_cast<size_t>(frames) * sizeof(float));

	auto *left = reinterpret_cast<float *>(frame.planes[0].data());
	auto *right = reinterpret_cast<float *>(frame.planes[1].data());
	for (uint32_t i = 0; i < frames; ++i) {
		left[i] = seed + static_cast<float>(i % 64) / 128.0f;
		right[i] = seed + static_cast<float>((i + 17) % 64) / 128.0f;
	}

	return frame;
}

static void testFfmpegVideoEncodeDecode()
{
	assert(isFfmpegVideoEncoderAvailable(kDefaultVideoEncoderName));
	const auto encoders = listFfmpegVideoEncoders();
	assert(!encoders.empty());

	DelaySettings settings;
	settings.videoEncoderName = kDefaultVideoEncoderName;
	settings.videoBitrateKbps = 1500;
	settings.keyframeIntervalSeconds = 1;

	FfmpegVideoEncoder encoder;
	assert(encoder.open(64, 64, 30, 1, settings));

	std::vector<EncodedPacket> packets;
	for (uint32_t i = 0; i < 5; ++i) {
		auto frame = makeFrame(static_cast<uint64_t>(i) * 33333333ULL, 64, 64, static_cast<uint8_t>(i * 7));
		auto encoded = encoder.encode(frame);
		packets.insert(packets.end(), encoded.begin(), encoded.end());
	}
	auto flushed = encoder.flush();
	packets.insert(packets.end(), flushed.begin(), flushed.end());

	assert(!packets.empty());
	bool sawKeyframe = false;
	for (const auto &packet : packets) {
		assert(packet.kind == EncodedPacketKind::Video);
		assert(!packet.data.empty());
		sawKeyframe = sawKeyframe || packet.keyframe;
	}
	assert(sawKeyframe);

	FfmpegVideoDecoder decoder;
	assert(decoder.open());
	size_t decodedCount = 0;
	for (const auto &packet : packets) {
		for (const auto &decoded : decoder.decode(packet)) {
			assert(decoded.width == 64);
			assert(decoded.height == 64);
			assert(decoded.lineSize == 64 * 4);
			assert(!decoded.data.empty());
			++decodedCount;
		}
	}
	for (const auto &decoded : decoder.flush()) {
		assert(decoded.width == 64);
		assert(decoded.height == 64);
		assert(!decoded.data.empty());
		++decodedCount;
	}
	assert(decodedCount > 0);
}

static void testFfmpegAudioEncodeDecode()
{
	assert(isFfmpegAudioEncoderAvailable(kDefaultAudioEncoderName));
	const auto encoders = listFfmpegAudioEncoders();
	assert(!encoders.empty());

	DelaySettings settings;
	settings.audioEncoderName = kDefaultAudioEncoderName;
	settings.audioBitrateKbps = 128;

	FfmpegAudioEncoder encoder;
	assert(encoder.open(48000, SPEAKERS_STEREO, settings));

	const uint32_t frameSize = std::max<uint32_t>(encoder.frameSize(), 1024);
	const uint32_t chunkFrames = (frameSize / 2) + 17;
	const uint64_t chunkDurationNs = (1000000000ULL * static_cast<uint64_t>(chunkFrames)) / 48000ULL;
	std::vector<EncodedPacket> packets;
	for (uint32_t i = 0; i < 7; ++i) {
		auto frame = makeAudioFrame(static_cast<uint64_t>(i) * chunkDurationNs, chunkFrames,
					    static_cast<float>(i) / 16.0f);
		auto encoded = encoder.encode(frame);
		packets.insert(packets.end(), encoded.begin(), encoded.end());
	}
	auto flushed = encoder.flush();
	packets.insert(packets.end(), flushed.begin(), flushed.end());

	assert(!packets.empty());
	for (const auto &packet : packets) {
		assert(packet.kind == EncodedPacketKind::Audio);
		assert(!packet.data.empty());
	}

	FfmpegAudioDecoder decoder;
	assert(decoder.open(packets.front().codecConfig));
	size_t decodedCount = 0;
	for (const auto &packet : packets) {
		for (const auto &decoded : decoder.decode(packet)) {
			assert(decoded.frames > 0);
			assert(decoded.samplesPerSec == 48000);
			assert(decoded.speakers == SPEAKERS_STEREO);
			assert(decoded.format == AUDIO_FORMAT_FLOAT_PLANAR);
			assert(decoded.planes.size() == 2);
			++decodedCount;
		}
	}
	for (const auto &decoded : decoder.flush()) {
		assert(decoded.frames > 0);
		assert(decoded.planes.size() == 2);
		++decodedCount;
	}
	assert(decodedCount > 0);
}

int main()
{
	testFfmpegVideoEncodeDecode();
	testFfmpegAudioEncodeDecode();
	return 0;
}
