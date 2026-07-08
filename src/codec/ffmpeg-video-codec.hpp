#pragma once

#include "core/delay-settings.hpp"
#include "core/encoded-ring-buffer.hpp"
#include "obs/raw-audio-frame.hpp"
#include "obs/raw-video-frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace comp_delay {

struct FfmpegEncoderInfo {
	std::string name;
	std::string displayName;
};

std::vector<FfmpegEncoderInfo> listFfmpegVideoEncoders();
std::vector<FfmpegEncoderInfo> listFfmpegAudioEncoders();
bool isFfmpegVideoEncoderAvailable(const std::string &name);
bool isFfmpegAudioEncoderAvailable(const std::string &name);

class FfmpegVideoEncoder {
public:
	FfmpegVideoEncoder();
	~FfmpegVideoEncoder();

	FfmpegVideoEncoder(const FfmpegVideoEncoder &) = delete;
	FfmpegVideoEncoder &operator=(const FfmpegVideoEncoder &) = delete;

	bool open(uint32_t width, uint32_t height, uint32_t fpsNumerator, uint32_t fpsDenominator,
		  const DelaySettings &settings);
	void close();

	bool active() const;
	const std::string &status() const;

	std::vector<EncodedPacket> encode(const RawVideoFrame &frame);
	std::vector<EncodedPacket> flush();

private:
	std::vector<EncodedPacket> receivePackets();
	void setError(const std::string &message);

	void *codecContext_ = nullptr;
	void *frame_ = nullptr;
	void *packet_ = nullptr;
	void *scaleContext_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint64_t frameDurationNs_ = 0;
	bool active_ = false;
	std::string status_;
};

class FfmpegVideoDecoder {
public:
	FfmpegVideoDecoder();
	~FfmpegVideoDecoder();

	FfmpegVideoDecoder(const FfmpegVideoDecoder &) = delete;
	FfmpegVideoDecoder &operator=(const FfmpegVideoDecoder &) = delete;

	bool open();
	void close();
	void reset();

	bool active() const;
	const std::string &status() const;

	std::vector<RawVideoFrame> decode(const EncodedPacket &packet);
	std::vector<RawVideoFrame> flush();

private:
	std::vector<RawVideoFrame> receiveFrames();
	void setError(const std::string &message);

	void *codecContext_ = nullptr;
	void *frame_ = nullptr;
	void *packet_ = nullptr;
	void *scaleContext_ = nullptr;
	bool active_ = false;
	std::string status_;
};

class FfmpegAudioEncoder {
public:
	FfmpegAudioEncoder();
	~FfmpegAudioEncoder();

	FfmpegAudioEncoder(const FfmpegAudioEncoder &) = delete;
	FfmpegAudioEncoder &operator=(const FfmpegAudioEncoder &) = delete;

	bool open(uint32_t sampleRate, enum speaker_layout speakers, const DelaySettings &settings);
	void close();

	bool active() const;
	const std::string &status() const;
	uint32_t frameSize() const;

	std::vector<EncodedPacket> encode(const RawAudioFrame &frame);
	std::vector<EncodedPacket> flush();

private:
	std::vector<EncodedPacket> drainPendingFrames(bool flushRemaining);
	std::vector<EncodedPacket> encodePendingFrame(uint32_t frames, bool padSilence);
	std::vector<EncodedPacket> encodeFrame(const RawAudioFrame &frame);
	std::vector<EncodedPacket> receivePackets();
	void setError(const std::string &message);

	void *codecContext_ = nullptr;
	void *frame_ = nullptr;
	void *packet_ = nullptr;
	uint32_t sampleRate_ = 48000;
	enum speaker_layout speakers_ = SPEAKERS_STEREO;
	uint32_t encoderFrameSize_ = 0;
	uint32_t pendingFrames_ = 0;
	bool havePendingTimestamp_ = false;
	uint64_t pendingTimestampNs_ = 0;
	uint64_t fallbackFrameDurationNs_ = 0;
	bool active_ = false;
	std::string status_;
	std::vector<uint8_t> codecConfig_;
	std::vector<std::vector<uint8_t>> pendingPlanes_;
};

class FfmpegAudioDecoder {
public:
	FfmpegAudioDecoder();
	~FfmpegAudioDecoder();

	FfmpegAudioDecoder(const FfmpegAudioDecoder &) = delete;
	FfmpegAudioDecoder &operator=(const FfmpegAudioDecoder &) = delete;

	bool open(const std::vector<uint8_t> &codecConfig);
	void close();
	void reset();

	bool active() const;
	const std::string &status() const;

	std::vector<RawAudioFrame> decode(const EncodedPacket &packet);
	std::vector<RawAudioFrame> flush();

private:
	std::vector<RawAudioFrame> receiveFrames();
	void setError(const std::string &message);

	void *codecContext_ = nullptr;
	void *frame_ = nullptr;
	void *packet_ = nullptr;
	bool haveNextAudioTimestamp_ = false;
	uint64_t nextAudioTimestampNs_ = 0;
	bool active_ = false;
	std::string status_;
};

} // namespace comp_delay
