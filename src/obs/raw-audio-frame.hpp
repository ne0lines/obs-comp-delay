#pragma once

#include <media-io/audio-io.h>

#include <cstdint>
#include <vector>

namespace comp_delay {

struct RawAudioFrame {
	uint32_t frames = 0;
	uint64_t timestampNs = 0;
	enum speaker_layout speakers = SPEAKERS_STEREO;
	enum audio_format format = AUDIO_FORMAT_FLOAT_PLANAR;
	uint32_t samplesPerSec = 48000;
	std::vector<std::vector<uint8_t>> planes;
};

} // namespace comp_delay
