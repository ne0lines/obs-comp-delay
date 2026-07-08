#include "scene-capture-encoder.hpp"

#include "obs/scene-utils.hpp"

#include <graphics/graphics.h>
#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <utility>

namespace comp_delay {

namespace {

constexpr uint64_t kNsPerSecond = 1000000000ULL;

std::shared_ptr<RawVideoFrame> copyRgbaFrame(const uint8_t *data, uint32_t lineSize, uint32_t width, uint32_t height,
					     uint64_t timestampNs)
{
	if (!data || width == 0 || height == 0)
		return {};

	auto out = std::make_shared<RawVideoFrame>();
	out->width = width;
	out->height = height;
	out->lineSize = width * 4;
	out->timestampNs = timestampNs;
	out->format = VIDEO_FORMAT_RGBA;
	out->data.resize(static_cast<size_t>(out->lineSize) * height);

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *src = data + static_cast<size_t>(lineSize) * y;
		uint8_t *dst = out->data.data() + static_cast<size_t>(out->lineSize) * y;
		std::memcpy(dst, src, out->lineSize);
	}

	return out;
}

std::shared_ptr<RawAudioFrame> copySceneAudioMix(obs_source_t *source, uint64_t timestampNs, uint32_t frames,
						 uint32_t sampleRate, enum speaker_layout speakers)
{
	if (!source || timestampNs == 0 || frames == 0)
		return {};

	const enum audio_format format = AUDIO_FORMAT_FLOAT_PLANAR;
	const size_t planes = get_audio_planes(format, speakers);
	const size_t bytesPerPlane = get_audio_size(format, speakers, frames);
	if (planes == 0 || bytesPerPlane == 0)
		return {};

	struct obs_source_audio_mix mix = {};
	obs_source_get_audio_mix(source, &mix);

	auto out = std::make_shared<RawAudioFrame>();
	out->frames = frames;
	out->timestampNs = timestampNs;
	out->samplesPerSec = sampleRate;
	out->speakers = speakers;
	out->format = format;
	out->planes.resize(planes);

	for (size_t i = 0; i < planes; ++i) {
		if (!mix.output[0].data[i])
			return {};

		out->planes[i].resize(bytesPerPlane);
		std::memcpy(out->planes[i].data(), mix.output[0].data[i], bytesPerPlane);
	}

	return out;
}

} // namespace

SceneCaptureEncoder::SceneCaptureEncoder()
	: encodedBuffer_(std::make_shared<EncodedRingBuffer>(
		  static_cast<uint64_t>(kMaxDelaySeconds + kDefaultKeyframeIntervalSeconds + 2) * kNsPerSecond,
		  kDefaultMemoryCapBytes))
{
	backendStatus_ = "Idle";
}

void SceneCaptureEncoder::start(const DelaySettings &settings)
{
	stop();
	settings_ = settings;
	encodedBuffer_ = std::make_shared<EncodedRingBuffer>(static_cast<uint64_t>(encodedRetentionSeconds(settings)) * kNsPerSecond,
							     settings.memoryCapBytes);
	videoFramesSeen_ = 0;
	videoPacketsSeen_ = 0;
	audioPacketsSeen_ = 0;
	lastLoggedDepthSeconds_ = 0;
	lastAudioMixTimestampNs_ = 0;
	lastVideoRenderNs_ = 0;
	lastLoggedBufferStatsNs_ = 0;
	loggedFirstVideoFrame_ = false;
	loggedFirstVideoPacket_ = false;
	loggedFirstKeyframe_ = false;

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		failStart("OBS video is not initialized");
		return;
	}

	sourceScene_ = obs_ui::getSceneRefByName(settings.sourceSceneName);
	if (!sourceScene_) {
		failStart("Source scene is missing");
		return;
	}

	width_ = ovi.output_width;
	height_ = ovi.output_height;
	fpsNumerator_ = ovi.fps_num ? ovi.fps_num : 60;
	fpsDenominator_ = ovi.fps_den ? ovi.fps_den : 1;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.range = VIDEO_RANGE_FULL;

	if (!videoEncoder_.open(width_, height_, fpsNumerator_, fpsDenominator_, settings)) {
		failStart(videoEncoder_.status());
		return;
	}

	struct obs_audio_info audioInfo = {};
	if (obs_get_audio_info(&audioInfo)) {
		audioSampleRate_ = audioInfo.samples_per_sec;
		audioSpeakers_ = audioInfo.speakers == SPEAKERS_UNKNOWN ? SPEAKERS_STEREO : audioInfo.speakers;
	}
	audioFormat_ = AUDIO_FORMAT_FLOAT_PLANAR;
	if (!audioEncoder_.open(audioSampleRate_, audioSpeakers_, settings)) {
		failStart(audioEncoder_.status());
		return;
	}

	audioView_ = obs_view_create();
	if (!audioView_) {
		failStart("Could not create OBS audio capture view");
		return;
	}

	if (!obs_view_add2(audioView_, &ovi)) {
		failStart("Could not attach OBS audio capture view");
		return;
	}
	audioViewAdded_ = true;
	obs_view_set_source(audioView_, 0, sourceScene_);

	obs_source_inc_active(sourceScene_);
	sourceSceneActivated_ = true;

	struct audio_convert_info audioConversion = {};
	audioConversion.samples_per_sec = audioSampleRate_;
	audioConversion.speakers = audioSpeakers_;
	audioConversion.format = audioFormat_;
	audioConversion.allow_clipping = true;
	obs_add_raw_audio_callback(0, &audioConversion, rawAudioCallback, this);
	rawAudioConnected_ = true;

	obs_add_main_render_callback(renderCallback, this);
	renderConnected_ = true;

	active_ = true;
	backendStatus_ = "Encoded H.264/AAC scene capture active";
	blog(LOG_INFO, "[obs-comp-delay] capture started: source='%s', %ux%u, fps=%u/%u, video_encoder='%s', audio_encoder='%s'",
		settings.sourceSceneName.c_str(), width_, height_, fpsNumerator_, fpsDenominator_,
		settings.videoEncoderName.c_str(), settings.audioEncoderName.c_str());
}

void SceneCaptureEncoder::stop()
{
	active_ = false;
	cleanupView();
	if (encodedBuffer_)
		encodedBuffer_->clear();
	videoEncoder_.close();
	audioEncoder_.close();
	backendStatus_ = "Stopped";
}

void SceneCaptureEncoder::tick()
{
}

void SceneCaptureEncoder::updateRetention(const DelaySettings &settings)
{
	settings_ = settings;
	if (encodedBuffer_) {
		encodedBuffer_->setLimits(static_cast<uint64_t>(encodedRetentionSeconds(settings)) * kNsPerSecond,
					  settings.memoryCapBytes);
	}
}

bool SceneCaptureEncoder::active() const
{
	return active_;
}

uint32_t SceneCaptureEncoder::bufferDepthSeconds() const
{
	return encodedBuffer_ ? encodedBuffer_->durationSecondsFloor() : 0;
}

bool SceneCaptureEncoder::playbackReady(uint32_t delaySeconds) const
{
	return encodedBuffer_ && encodedBuffer_->decodeBatchForDelay(delaySeconds, 0).has_value();
}

std::shared_ptr<EncodedRingBuffer> SceneCaptureEncoder::encodedBuffer() const
{
	return encodedBuffer_;
}

const std::string &SceneCaptureEncoder::backendStatus() const
{
	return backendStatus_;
}

void SceneCaptureEncoder::renderCallback(void *param, uint32_t, uint32_t)
{
	auto *encoder = static_cast<SceneCaptureEncoder *>(param);
	if (encoder)
		encoder->renderSourceFrame();
}

void SceneCaptureEncoder::rawAudioCallback(void *param, size_t, struct audio_data *audioData)
{
	auto *encoder = static_cast<SceneCaptureEncoder *>(param);
	if (!encoder || !audioData)
		return;

	const uint64_t timestampNs = encoder->sourceScene_ ? obs_source_get_audio_timestamp(encoder->sourceScene_) : 0;
	if (timestampNs == 0 || timestampNs == encoder->lastAudioMixTimestampNs_)
		return;

	encoder->lastAudioMixTimestampNs_ = timestampNs;
	encoder->handleSceneAudioMix(timestampNs, audioData->frames);
}

void SceneCaptureEncoder::renderSourceFrame()
{
	if (!active_ || !sourceScene_ || width_ == 0 || height_ == 0)
		return;

	const uint64_t nowNs = os_gettime_ns();
	const uint64_t frameIntervalNs =
		fpsNumerator_ == 0
			? 16666667ULL
			: (1000000000ULL * static_cast<uint64_t>(std::max<uint32_t>(fpsDenominator_, 1))) /
				  static_cast<uint64_t>(fpsNumerator_);
	const uint64_t minRenderIntervalNs = std::max<uint64_t>(1, frameIntervalNs * 3 / 4);
	if (lastVideoRenderNs_ != 0 && nowNs > lastVideoRenderNs_ && nowNs - lastVideoRenderNs_ < minRenderIntervalNs)
		return;
	lastVideoRenderNs_ = nowNs;

	if (!texrender_)
		texrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!stageSurface_)
		stageSurface_ = gs_stagesurface_create(width_, height_, GS_RGBA);
	if (!texrender_ || !stageSurface_) {
		backendStatus_ = "Could not allocate OBS offscreen capture textures";
		active_ = false;
		return;
	}

	if (stageSurfaceReady_) {
		uint8_t *mappedData = nullptr;
		uint32_t mappedLineSize = 0;
		if (gs_stagesurface_map(stageSurface_, &mappedData, &mappedLineSize)) {
			handleVideoFrame(mappedData, mappedLineSize, stagedTimestampNs_);
			gs_stagesurface_unmap(stageSurface_);
		}
		stageSurfaceReady_ = false;
	}

	gs_texrender_reset(texrender_);
	if (!gs_texrender_begin(texrender_, width_, height_))
		return;

	struct vec4 clearColor = {};
	gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);
	gs_ortho(0.0f, static_cast<float>(width_), 0.0f, static_cast<float>(height_), -100.0f, 100.0f);
	obs_source_video_render(sourceScene_);
	gs_texrender_end(texrender_);

	gs_texture_t *texture = gs_texrender_get_texture(texrender_);
	if (texture) {
		stagedTimestampNs_ = nowNs;
		gs_stage_texture(stageSurface_, texture);
		stageSurfaceReady_ = true;
	}
}

void SceneCaptureEncoder::handleVideoFrame(const uint8_t *data, uint32_t lineSize, uint64_t timestampNs)
{
	if (!active_ || !encodedBuffer_)
		return;

	auto copied = copyRgbaFrame(data, lineSize, width_, height_, timestampNs);
	if (!copied)
		return;

	++videoFramesSeen_;
	if (!loggedFirstVideoFrame_) {
		loggedFirstVideoFrame_ = true;
		blog(LOG_INFO, "[obs-comp-delay] received first source-scene video frame at %" PRIu64 " ns",
			copied->timestampNs);
	}

	for (auto &packet : videoEncoder_.encode(*copied)) {
		++videoPacketsSeen_;
		if (!loggedFirstVideoPacket_) {
			loggedFirstVideoPacket_ = true;
			blog(LOG_INFO, "[obs-comp-delay] encoded first H.264 packet at %" PRIu64 " ns", packet.timestampNs);
		}
		if (packet.keyframe && !loggedFirstKeyframe_) {
			loggedFirstKeyframe_ = true;
			blog(LOG_INFO, "[obs-comp-delay] encoded first H.264 keyframe at %" PRIu64 " ns", packet.timestampNs);
		}
		encodedBuffer_->push(std::move(packet));
	}

	const uint32_t depth = encodedBuffer_->durationSecondsFloor();
	const uint64_t nowNs = os_gettime_ns();
	const bool periodicStatsDue =
		lastLoggedBufferStatsNs_ == 0 || nowNs - lastLoggedBufferStatsNs_ >= 10ULL * kNsPerSecond;
	if (depth > lastLoggedDepthSeconds_ || periodicStatsDue) {
		lastLoggedDepthSeconds_ = depth;
		lastLoggedBufferStatsNs_ = nowNs;
		const double bufferMiB = static_cast<double>(encodedBuffer_->byteSize()) / 1024.0 / 1024.0;
		blog(LOG_INFO,
		     "[obs-comp-delay] buffer depth %us (video_packets=%" PRIu64 ", audio_packets=%" PRIu64
		     ", packets=%zu, bytes=%.1f MiB)",
		     depth, videoPacketsSeen_, audioPacketsSeen_, encodedBuffer_->packetCount(), bufferMiB);
	}

	if (!videoEncoder_.active()) {
		backendStatus_ = videoEncoder_.status();
		active_ = false;
	}
}

void SceneCaptureEncoder::handleSceneAudioMix(uint64_t timestampNs, uint32_t frames)
{
	if (!active_ || !encodedBuffer_ || !sourceScene_)
		return;

	auto copied = copySceneAudioMix(sourceScene_, timestampNs, frames, audioSampleRate_, audioSpeakers_);
	if (!copied)
		return;

	for (auto &packet : audioEncoder_.encode(*copied)) {
		++audioPacketsSeen_;
		encodedBuffer_->push(std::move(packet));
	}

	if (!audioEncoder_.active()) {
		backendStatus_ = audioEncoder_.status();
		active_ = false;
	}
}

void SceneCaptureEncoder::failStart(std::string message)
{
	active_ = false;
	cleanupView();
	if (encodedBuffer_)
		encodedBuffer_->clear();
	videoEncoder_.close();
	audioEncoder_.close();
	backendStatus_ = std::move(message);
}

void SceneCaptureEncoder::cleanupView()
{
	if (rawAudioConnected_) {
		obs_remove_raw_audio_callback(0, rawAudioCallback, this);
		rawAudioConnected_ = false;
	}

	if (renderConnected_) {
		obs_remove_main_render_callback(renderCallback, this);
		renderConnected_ = false;
	}

	if (sourceScene_ && sourceSceneActivated_) {
		obs_source_dec_active(sourceScene_);
		sourceSceneActivated_ = false;
	}

	if (audioView_) {
		obs_view_set_source(audioView_, 0, nullptr);
		if (audioViewAdded_) {
			obs_view_remove(audioView_);
			audioViewAdded_ = false;
		}
		obs_view_destroy(audioView_);
		audioView_ = nullptr;
	}

	if (texrender_ || stageSurface_) {
		obs_enter_graphics();
		if (stageSurface_) {
			gs_stagesurface_destroy(stageSurface_);
			stageSurface_ = nullptr;
		}
		if (texrender_) {
			gs_texrender_destroy(texrender_);
			texrender_ = nullptr;
		}
		obs_leave_graphics();
	}
	stageSurfaceReady_ = false;
	stagedTimestampNs_ = 0;
	lastAudioMixTimestampNs_ = 0;
	lastVideoRenderNs_ = 0;
	lastLoggedBufferStatsNs_ = 0;

	if (sourceScene_) {
		obs_source_release(sourceScene_);
		sourceScene_ = nullptr;
	}

	width_ = 0;
	height_ = 0;
	fpsNumerator_ = 60;
	fpsDenominator_ = 1;
}

} // namespace comp_delay
