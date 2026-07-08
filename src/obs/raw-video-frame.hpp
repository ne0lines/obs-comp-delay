#pragma once

#include <media-io/video-io.h>

#include <cstdint>
#include <vector>

namespace comp_delay {

struct RawVideoFrame {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t lineSize = 0;
	uint64_t timestampNs = 0;
	enum video_format format = VIDEO_FORMAT_RGBA;
	std::vector<uint8_t> data;
};

} // namespace comp_delay
