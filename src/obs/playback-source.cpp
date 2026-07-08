#include "playback-source.hpp"

#include "codec/ffmpeg-video-codec.hpp"

#include <obs-module.h>
#include <obs.h>

#include <cstddef>
#include <inttypes.h>
#include <mutex>
#include <utility>
#include <vector>

namespace comp_delay {

namespace {

std::mutex gBufferMutex;
std::weak_ptr<EncodedRingBuffer> gBuffer;
uint32_t gDelaySeconds = 0;
uint64_t gBufferGeneration = 0;

struct PlaybackSourceContext {
	obs_source_t *source = nullptr;
	uint64_t bufferGeneration = 0;
	bool haveVideoPacketCursor = false;
	bool haveAudioPacketCursor = false;
	bool haveAudioOutputBase = false;
	bool loggedAvSync = false;
	uint64_t lastVideoInputTimestampNs = 0;
	uint64_t lastAudioInputTimestampNs = 0;
	uint64_t lastVideoOutputInputTimestampNs = 0;
	uint64_t audioBaseInputTimestampNs = 0;
	uint64_t audioBaseOutputTimestampNs = 0;
	FfmpegVideoDecoder videoDecoder;
	FfmpegAudioDecoder audioDecoder;
	std::vector<uint8_t> audioCodecConfig;
};

void resetPlaybackContext(PlaybackSourceContext *context)
{
	if (!context)
		return;

	context->haveVideoPacketCursor = false;
	context->haveAudioPacketCursor = false;
	context->haveAudioOutputBase = false;
	context->loggedAvSync = false;
	context->lastVideoInputTimestampNs = 0;
	context->lastAudioInputTimestampNs = 0;
	context->lastVideoOutputInputTimestampNs = 0;
	context->audioBaseInputTimestampNs = 0;
	context->audioBaseOutputTimestampNs = 0;
	context->videoDecoder.close();
	context->audioDecoder.close();
	context->audioCodecConfig.clear();
}

const char *playbackGetName(void *)
{
	return "Comp Delay Playback";
}

void *playbackCreate(obs_data_t *, obs_source_t *source)
{
	auto *context = new PlaybackSourceContext;
	context->source = source;
	return context;
}

void playbackDestroy(void *data)
{
	delete static_cast<PlaybackSourceContext *>(data);
}

uint32_t playbackGetWidth(void *)
{
	struct obs_video_info info = {};
	return obs_get_video_info(&info) ? info.output_width : 1920;
}

uint32_t playbackGetHeight(void *)
{
	struct obs_video_info info = {};
	return obs_get_video_info(&info) ? info.output_height : 1080;
}

void playbackVideoTick(void *data, float)
{
	auto *context = static_cast<PlaybackSourceContext *>(data);
	if (!context || !context->source)
		return;

	if (!obs_source_showing(context->source)) {
		resetPlaybackContext(context);
		return;
	}

	std::shared_ptr<EncodedRingBuffer> buffer;
	uint32_t delaySeconds = 0;
	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(gBufferMutex);
		buffer = gBuffer.lock();
		delaySeconds = gDelaySeconds;
		generation = gBufferGeneration;
	}

	if (context->bufferGeneration != generation) {
		context->bufferGeneration = generation;
		resetPlaybackContext(context);
	}

	if (!buffer) {
		obs_source_output_video(context->source, nullptr);
		return;
	}

	if (!context->videoDecoder.active() && !context->videoDecoder.open()) {
		obs_source_output_video(context->source, nullptr);
		return;
	}

	std::shared_ptr<RawVideoFrame> frame;
	if (!context->haveVideoPacketCursor) {
		const auto batch = buffer->decodeBatchForDelay(delaySeconds, 0);
		if (!batch) {
			obs_source_output_video(context->source, nullptr);
			return;
		}

		context->videoDecoder.reset();
		for (const auto &packet : batch->packets) {
			if (packet.kind != EncodedPacketKind::Video)
				continue;

			for (auto &decoded : context->videoDecoder.decode(packet)) {
				if (decoded.timestampNs <= batch->targetTimestampNs || !frame)
					frame = std::make_shared<RawVideoFrame>(std::move(decoded));
			}
			context->lastVideoInputTimestampNs = packet.timestampNs;
			context->haveVideoPacketCursor = true;
		}
		if (!context->haveAudioPacketCursor && batch->targetTimestampNs > 0) {
			context->lastAudioInputTimestampNs = batch->targetTimestampNs - 1;
			context->haveAudioPacketCursor = true;
		}
	} else {
		const auto videoPackets =
			buffer->packetsReadySince(EncodedPacketKind::Video, delaySeconds,
						  context->lastVideoInputTimestampNs, true, 8);
		for (const auto &packet : videoPackets) {
			for (auto &decoded : context->videoDecoder.decode(packet))
				frame = std::make_shared<RawVideoFrame>(std::move(decoded));
			context->lastVideoInputTimestampNs = packet.timestampNs;
		}
	}

	if (!frame || frame->data.empty()) {
		return;
	}

	struct obs_source_frame obsFrame = {};
	obsFrame.data[0] = frame->data.data();
	obsFrame.linesize[0] = frame->lineSize;
	obsFrame.width = frame->width;
	obsFrame.height = frame->height;
	obsFrame.timestamp = obs_get_video_frame_time();
	obsFrame.format = frame->format;
	obsFrame.full_range = true;
	obsFrame.flip = false;

	obs_source_output_video(context->source, &obsFrame);
	context->lastVideoOutputInputTimestampNs = frame->timestampNs;

	const auto audioPackets = buffer->packetsReadySince(EncodedPacketKind::Audio, delaySeconds,
							    context->lastAudioInputTimestampNs,
							    context->haveAudioPacketCursor, 16);
	const uint64_t now = obs_get_video_frame_time();
	for (const auto &packet : audioPackets) {
		if (packet.data.empty())
			continue;

		if (!context->audioDecoder.active() || context->audioCodecConfig != packet.codecConfig) {
			context->audioDecoder.close();
			if (!context->audioDecoder.open(packet.codecConfig))
				break;
			context->audioCodecConfig = packet.codecConfig;
			context->haveAudioOutputBase = false;
			context->audioBaseInputTimestampNs = 0;
			context->audioBaseOutputTimestampNs = 0;
		}

		const auto decodedFrames = context->audioDecoder.decode(packet);
		context->lastAudioInputTimestampNs = packet.timestampNs;
		context->haveAudioPacketCursor = true;

		for (const auto &audioFrame : decodedFrames) {
			if (audioFrame.planes.empty())
				continue;

			if (!context->haveAudioOutputBase) {
				context->audioBaseInputTimestampNs = audioFrame.timestampNs;
				context->audioBaseOutputTimestampNs = now;
				context->haveAudioOutputBase = true;
				if (!context->loggedAvSync && context->lastVideoOutputInputTimestampNs != 0) {
					const int64_t deltaNs = static_cast<int64_t>(audioFrame.timestampNs) -
								static_cast<int64_t>(context->lastVideoOutputInputTimestampNs);
					blog(LOG_INFO,
					     "[obs-comp-delay] playback A/V sync base: video_input=%" PRIu64
					     " ns, audio_input=%" PRIu64 " ns, delta=%.2f ms",
					     context->lastVideoOutputInputTimestampNs, audioFrame.timestampNs,
					     static_cast<double>(deltaNs) / 1000000.0);
					context->loggedAvSync = true;
				}
			}

			struct obs_source_audio obsAudio = {};
			for (size_t i = 0; i < audioFrame.planes.size() && i < MAX_AV_PLANES; ++i)
				obsAudio.data[i] = audioFrame.planes[i].data();
			obsAudio.frames = audioFrame.frames;
			obsAudio.speakers = audioFrame.speakers;
			obsAudio.format = audioFrame.format;
			obsAudio.samples_per_sec = audioFrame.samplesPerSec;
			obsAudio.timestamp =
				context->audioBaseOutputTimestampNs +
				(audioFrame.timestampNs - context->audioBaseInputTimestampNs);
			obs_source_output_audio(context->source, &obsAudio);
		}
	}
}

} // namespace

void registerPlaybackSource()
{
	struct obs_source_info info = {};
	info.id = kPlaybackSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = playbackGetName;
	info.create = playbackCreate;
	info.destroy = playbackDestroy;
	info.get_width = playbackGetWidth;
	info.get_height = playbackGetHeight;
	info.video_tick = playbackVideoTick;

	obs_register_source(&info);
}

void setPlaybackBuffers(std::shared_ptr<EncodedRingBuffer> buffer, uint32_t delaySeconds)
{
	std::lock_guard<std::mutex> lock(gBufferMutex);
	const auto currentBuffer = gBuffer.lock();
	const bool changed = currentBuffer != buffer || gDelaySeconds != delaySeconds;
	gBuffer = std::move(buffer);
	gDelaySeconds = delaySeconds;
	if (changed)
		++gBufferGeneration;
}

} // namespace comp_delay
