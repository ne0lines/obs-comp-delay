#pragma once

#include "core/encoded-ring-buffer.hpp"

#include <memory>

namespace comp_delay {

constexpr const char *kPlaybackSourceId = "comp_delay_playback_source";

void registerPlaybackSource();
void setPlaybackBuffers(std::shared_ptr<EncodedRingBuffer> buffer, uint32_t delaySeconds);

} // namespace comp_delay
