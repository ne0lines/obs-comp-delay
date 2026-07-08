#pragma once

#include "core/delay-settings.hpp"
#include "core/delay-state-machine.hpp"
#include "obs/scene-capture-encoder.hpp"

#include <obs-data.h>

#include <string>

namespace comp_delay {

class DelayController {
public:
	DelayController();
	~DelayController();

	void applySettings(const DelaySettings &settings);
	void applyConfiguredDelay();
	void goLive();
	void tick();

	void save(obs_data_t *saveData) const;
	void load(obs_data_t *saveData);

	const DelaySettings &settings() const;
	RuntimeState state() const;
	uint32_t bufferDepthSeconds() const;
	uint32_t targetDelaySeconds() const;
	std::string statusText() const;
	const std::string &lastError() const;

private:
	bool validateSettings(const DelaySettings &settings, std::string &error) const;
	bool runtimeBuffersFitMemoryCap(const DelaySettings &settings, std::string &error) const;
	bool canRetargetActiveDelay(const DelaySettings &current, const DelaySettings &next) const;
	void retargetActiveDelay(uint32_t previousDelaySeconds);
	void trimBufferForDelay(uint32_t delaySeconds);
	void failRuntime(const std::string &message);
	void switchToTransitionOrFail();

	DelaySettings settings_;
	DelayStateMachine stateMachine_;
	SceneCaptureEncoder capture_;
	std::string lastError_;
	uint64_t delaySceneSwitchNotBeforeNs_ = 0;
	bool switchedToDelayScene_ = false;
};

} // namespace comp_delay
