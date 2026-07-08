#pragma once

#include "codec/ffmpeg-video-codec.hpp"
#include "core/encoded-ring-buffer.hpp"
#include "core/delay-settings.hpp"

#include <media-io/audio-io.h>
#include <obs.h>

#include <chrono>
#include <memory>
#include <string>

typedef struct gs_stage_surface gs_stagesurf_t;
typedef struct gs_texture_render gs_texrender_t;

namespace comp_delay {

class SceneCaptureEncoder {
public:
	SceneCaptureEncoder();

	void start(const DelaySettings &settings);
	void stop();
	void tick();
	void updateRetention(const DelaySettings &settings);

	bool active() const;
	uint32_t bufferDepthSeconds() const;
	bool playbackReady(uint32_t delaySeconds) const;
	std::shared_ptr<EncodedRingBuffer> encodedBuffer() const;
	const std::string &backendStatus() const;

private:
	static void renderCallback(void *param, uint32_t cx, uint32_t cy);
	static void rawAudioCallback(void *param, size_t mixIdx, struct audio_data *audioData);
	void renderSourceFrame();
	void handleVideoFrame(const uint8_t *data, uint32_t lineSize, uint64_t timestampNs);
	void handleSceneAudioMix(uint64_t timestampNs, uint32_t frames);
	void failStart(std::string message);
	void cleanupView();

	DelaySettings settings_;
	std::shared_ptr<EncodedRingBuffer> encodedBuffer_;
	FfmpegVideoEncoder videoEncoder_;
	FfmpegAudioEncoder audioEncoder_;
	bool active_ = false;
	obs_source_t *sourceScene_ = nullptr;
	obs_view_t *audioView_ = nullptr;
	bool renderConnected_ = false;
	bool rawAudioConnected_ = false;
	bool audioViewAdded_ = false;
	bool sourceSceneActivated_ = false;
	gs_texrender_t *texrender_ = nullptr;
	gs_stagesurf_t *stageSurface_ = nullptr;
	bool stageSurfaceReady_ = false;
	uint64_t stagedTimestampNs_ = 0;
	uint64_t lastAudioMixTimestampNs_ = 0;
	uint64_t lastVideoRenderNs_ = 0;
	uint64_t lastLoggedBufferStatsNs_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t fpsNumerator_ = 60;
	uint32_t fpsDenominator_ = 1;
	uint32_t audioSampleRate_ = 48000;
	enum speaker_layout audioSpeakers_ = SPEAKERS_STEREO;
	enum audio_format audioFormat_ = AUDIO_FORMAT_FLOAT_PLANAR;
	uint64_t videoFramesSeen_ = 0;
	uint64_t videoPacketsSeen_ = 0;
	uint64_t audioPacketsSeen_ = 0;
	uint32_t lastLoggedDepthSeconds_ = 0;
	bool loggedFirstVideoFrame_ = false;
	bool loggedFirstVideoPacket_ = false;
	bool loggedFirstKeyframe_ = false;
	std::string backendStatus_;
};

} // namespace comp_delay
