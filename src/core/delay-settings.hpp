#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace comp_delay {

constexpr uint32_t kMaxDelaySeconds = 300;
constexpr uint32_t kDefaultVideoBitrateKbps = 12000;
constexpr uint32_t kDefaultAudioBitrateKbps = 160;
constexpr uint32_t kDefaultKeyframeIntervalSeconds = 2;
constexpr size_t kDefaultMemoryCapBytes = 600ULL * 1024ULL * 1024ULL;
constexpr const char *kDefaultVideoEncoderName = "libx264";
constexpr const char *kDefaultAudioEncoderName = "aac";

struct DelaySettings {
	std::string sourceSceneName;
	std::string transitionSceneName;
	std::string delaySceneName;
	std::string videoEncoderName = kDefaultVideoEncoderName;
	std::string audioEncoderName = kDefaultAudioEncoderName;
	uint32_t targetDelaySeconds = 0;
	uint32_t videoBitrateKbps = kDefaultVideoBitrateKbps;
	uint32_t audioBitrateKbps = kDefaultAudioBitrateKbps;
	uint32_t keyframeIntervalSeconds = kDefaultKeyframeIntervalSeconds;
	size_t memoryCapBytes = kDefaultMemoryCapBytes;
};

uint32_t clampDelaySeconds(uint32_t value);
uint32_t encodedRetentionSeconds(const DelaySettings &settings);
size_t estimateEncodedBytes(uint32_t seconds, uint32_t videoBitrateKbps, uint32_t audioBitrateKbps);
bool settingsFitMemoryCap(const DelaySettings &settings);

} // namespace comp_delay
