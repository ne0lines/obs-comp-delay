#include "delay-state-machine.hpp"

#include "delay-settings.hpp"

#include <utility>

namespace comp_delay {

const char *toString(RuntimeState state)
{
	switch (state) {
	case RuntimeState::Off:
		return "OFF";
	case RuntimeState::Filling:
		return "FILLING";
	case RuntimeState::Delayed:
		return "DELAYED";
	case RuntimeState::Error:
		return "ERROR";
	}

	return "UNKNOWN";
}

void DelayStateMachine::requestDelay(uint32_t delaySeconds)
{
	targetDelaySeconds_ = clampDelaySeconds(delaySeconds);
	activeDelaySeconds_ = 0;
	bufferDepthSeconds_ = 0;
	errorMessage_.clear();
	state_ = targetDelaySeconds_ == 0 ? RuntimeState::Off : RuntimeState::Filling;
}

void DelayStateMachine::updateBufferDepth(uint32_t bufferDepthSeconds)
{
	bufferDepthSeconds_ = bufferDepthSeconds;
	if (state_ == RuntimeState::Filling && targetDelaySeconds_ > 0 && bufferDepthSeconds_ >= targetDelaySeconds_) {
		activeDelaySeconds_ = targetDelaySeconds_;
		state_ = RuntimeState::Delayed;
	}
}

void DelayStateMachine::goLive()
{
	state_ = RuntimeState::Off;
	targetDelaySeconds_ = 0;
	activeDelaySeconds_ = 0;
	bufferDepthSeconds_ = 0;
	errorMessage_.clear();
}

void DelayStateMachine::fail(std::string message)
{
	state_ = RuntimeState::Error;
	errorMessage_ = std::move(message);
	activeDelaySeconds_ = 0;
}

RuntimeState DelayStateMachine::state() const
{
	return state_;
}

uint32_t DelayStateMachine::targetDelaySeconds() const
{
	return targetDelaySeconds_;
}

uint32_t DelayStateMachine::activeDelaySeconds() const
{
	return activeDelaySeconds_;
}

uint32_t DelayStateMachine::bufferDepthSeconds() const
{
	return bufferDepthSeconds_;
}

const std::string &DelayStateMachine::errorMessage() const
{
	return errorMessage_;
}

} // namespace comp_delay
