#include "delay-settings.hpp"

#include <algorithm>
#include <limits>

namespace comp_delay {

uint32_t clampDelaySeconds(uint32_t value)
{
	return std::min(value, kMaxDelaySeconds);
}

uint32_t encodedRetentionSeconds(const DelaySettings &settings)
{
	const uint32_t keyframePreroll = std::max<uint32_t>(settings.keyframeIntervalSeconds, 1);
	return clampDelaySeconds(settings.targetDelaySeconds) + keyframePreroll + 2;
}

size_t estimateEncodedBytes(uint32_t seconds, uint32_t videoBitrateKbps, uint32_t audioBitrateKbps)
{
	const uint64_t totalKbps = static_cast<uint64_t>(videoBitrateKbps) + static_cast<uint64_t>(audioBitrateKbps);
	const uint64_t bytes = (totalKbps * 1000ULL * static_cast<uint64_t>(seconds)) / 8ULL;
	const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
	return static_cast<size_t>(std::min(bytes, maxSize));
}

bool settingsFitMemoryCap(const DelaySettings &settings)
{
	return estimateEncodedBytes(encodedRetentionSeconds(settings), settings.videoBitrateKbps, settings.audioBitrateKbps) <=
	       settings.memoryCapBytes;
}

} // namespace comp_delay
