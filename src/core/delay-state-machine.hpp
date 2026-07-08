#pragma once

#include <cstdint>
#include <string>

namespace comp_delay {

enum class RuntimeState {
	Off,
	Filling,
	Delayed,
	Error,
};

const char *toString(RuntimeState state);

class DelayStateMachine {
public:
	void requestDelay(uint32_t delaySeconds);
	void updateBufferDepth(uint32_t bufferDepthSeconds);
	void goLive();
	void fail(std::string message);

	RuntimeState state() const;
	uint32_t targetDelaySeconds() const;
	uint32_t activeDelaySeconds() const;
	uint32_t bufferDepthSeconds() const;
	const std::string &errorMessage() const;

private:
	RuntimeState state_ = RuntimeState::Off;
	uint32_t targetDelaySeconds_ = 0;
	uint32_t activeDelaySeconds_ = 0;
	uint32_t bufferDepthSeconds_ = 0;
	std::string errorMessage_;
};

} // namespace comp_delay
