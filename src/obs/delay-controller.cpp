#include "delay-controller.hpp"

#include "codec/ffmpeg-video-codec.hpp"
#include "obs/playback-source.hpp"
#include "obs/scene-utils.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/platform.h>

#include <cstring>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace comp_delay {

namespace {

constexpr const char *kSaveRoot = "comp_delay";
constexpr const char *kSourceScene = "source_scene";
constexpr const char *kTransitionScene = "transition_scene";
constexpr const char *kDelayScene = "delay_scene";
constexpr const char *kVideoEncoder = "video_encoder";
constexpr const char *kAudioEncoder = "audio_encoder";
constexpr const char *kTargetDelay = "target_delay_seconds";
constexpr const char *kVideoBitrate = "video_bitrate_kbps";
constexpr const char *kAudioBitrate = "audio_bitrate_kbps";
constexpr const char *kKeyframeInterval = "keyframe_interval_seconds";
constexpr const char *kMemoryCap = "memory_cap_bytes";
constexpr const char *kCountdownToken = "%delay_countdown%";
constexpr const char *kTextSetting = "text";
constexpr const char *kGdiTextSourceId = "text_gdiplus";
constexpr const char *kFreeTypeTextSourceId = "text_ft2_source";

std::string getString(obs_data_t *data, const char *key)
{
	const char *value = obs_data_get_string(data, key);
	return value ? value : "";
}

bool containsCountdownToken(const std::string &text)
{
	return text.find(kCountdownToken) != std::string::npos;
}

std::string replaceCountdownToken(std::string text, uint32_t remainingSeconds)
{
	const std::string replacement = std::to_string(remainingSeconds);
	size_t pos = 0;
	while ((pos = text.find(kCountdownToken, pos)) != std::string::npos) {
		text.replace(pos, std::strlen(kCountdownToken), replacement);
		pos += replacement.size();
	}
	return text;
}

std::string sourceKey(obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	if (uuid && *uuid)
		return uuid;

	const char *name = obs_source_get_name(source);
	return name && *name ? name : "";
}

bool isTextSource(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (std::strcmp(id, kGdiTextSourceId) == 0 || std::strcmp(id, kFreeTypeTextSourceId) == 0);
}

void updateTextSourceCountdown(obs_source_t *source, std::unordered_map<std::string, std::string> &templates,
			       uint32_t remainingSeconds, bool restore)
{
	if (!source || !isTextSource(source))
		return;

	const std::string key = sourceKey(source);
	if (key.empty())
		return;

	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings)
		return;

	const std::string currentText = getString(settings, kTextSetting);
	auto templateIt = templates.find(key);
	if (containsCountdownToken(currentText)) {
		templateIt = templates.insert_or_assign(key, currentText).first;
	}

	if (templateIt == templates.end()) {
		obs_data_release(settings);
		return;
	}

	const std::string nextText = restore ? templateIt->second : replaceCountdownToken(templateIt->second, remainingSeconds);
	if (currentText != nextText) {
		obs_data_set_string(settings, kTextSetting, nextText.c_str());
		obs_source_update(source, settings);
	}

	obs_data_release(settings);
}

struct CountdownTextUpdate {
	std::unordered_map<std::string, std::string> *templates = nullptr;
	uint32_t remainingSeconds = 0;
	bool restore = false;
	std::unordered_set<std::string> visitedScenes;
};

bool enumCountdownSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *update = static_cast<CountdownTextUpdate *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || !update || !update->templates)
		return true;

	updateTextSourceCountdown(source, *update->templates, update->remainingSeconds, update->restore);

	obs_scene_t *nestedScene = obs_scene_from_source(source);
	if (!nestedScene)
		return true;

	const std::string key = sourceKey(source);
	if (!key.empty() && update->visitedScenes.insert(key).second)
		obs_scene_enum_items(nestedScene, enumCountdownSceneItem, update);

	return true;
}

} // namespace

DelayController::DelayController() = default;

DelayController::~DelayController()
{
	restoreTransitionCountdownTemplates();
	capture_.stop();
}

void DelayController::applySettings(const DelaySettings &settings)
{
	DelaySettings next = settings;
	next.targetDelaySeconds = clampDelaySeconds(next.targetDelaySeconds);
	const RuntimeState state = stateMachine_.state();
	const bool delayActive = state == RuntimeState::Filling || state == RuntimeState::Delayed;
	const uint32_t previousDelaySeconds = settings_.targetDelaySeconds;
	const bool retargetActive = delayActive && canRetargetActiveDelay(settings_, next);
	settings_ = next;
	lastError_.clear();

	if (!delayActive)
		return;

	if (settings_.targetDelaySeconds == 0) {
		goLive();
		return;
	}

	if (retargetActive) {
		retargetActiveDelay(previousDelaySeconds);
		return;
	}

	applyConfiguredDelay();
}

void DelayController::applyConfiguredDelay()
{
	blog(LOG_INFO,
	     "[obs-comp-delay] applying configured delay: source='%s', transition='%s', delay='%s', target=%us",
	     settings_.sourceSceneName.c_str(), settings_.transitionSceneName.c_str(), settings_.delaySceneName.c_str(),
	     settings_.targetDelaySeconds);
	lastError_.clear();
	switchedToDelayScene_ = false;
	delaySceneSwitchNotBeforeNs_ = 0;

	if (settings_.targetDelaySeconds == 0) {
		blog(LOG_INFO, "[obs-comp-delay] configured delay is 0s; going live");
		goLive();
		return;
	}

	capture_.stop();
	setPlaybackBuffers(nullptr, 0);
	blog(LOG_INFO, "[obs-comp-delay] validating configured delay");

	std::string error;
	if (!validateSettings(settings_, error)) {
		lastError_ = error;
		blog(LOG_WARNING, "[obs-comp-delay] apply rejected: %s", lastError_.c_str());
		stateMachine_.fail(error);
		switchToTransitionOrFail();
		return;
	}
	blog(LOG_INFO, "[obs-comp-delay] configured delay accepted");

	if (!obs_ui::switchToSceneByName(settings_.transitionSceneName)) {
		lastError_ = "Could not switch to transition scene";
		blog(LOG_WARNING, "[obs-comp-delay] apply rejected: %s", lastError_.c_str());
		stateMachine_.fail(lastError_);
		return;
	}
	blog(LOG_INFO, "[obs-comp-delay] switched to transition scene '%s'", settings_.transitionSceneName.c_str());
	updateTransitionCountdown(settings_.targetDelaySeconds);

	capture_.start(settings_);
	if (!capture_.active()) {
		lastError_ = capture_.backendStatus();
		blog(LOG_WARNING, "[obs-comp-delay] capture failed to start: %s", lastError_.c_str());
		stateMachine_.fail(lastError_);
		return;
	}

	stateMachine_.requestDelay(settings_.targetDelaySeconds);
}

void DelayController::goLive()
{
	restoreTransitionCountdownTemplates();
	capture_.stop();
	setPlaybackBuffers(nullptr, 0);
	stateMachine_.goLive();
	switchedToDelayScene_ = false;
	delaySceneSwitchNotBeforeNs_ = 0;
	lastError_.clear();
	if (!settings_.sourceSceneName.empty())
		obs_ui::switchToSceneByName(settings_.sourceSceneName);
}

void DelayController::tick()
{
	capture_.tick();

	const RuntimeState state = stateMachine_.state();
	if (state == RuntimeState::Filling) {
		const uint32_t depth = capture_.bufferDepthSeconds();
		const uint32_t remaining = depth < settings_.targetDelaySeconds ? settings_.targetDelaySeconds - depth : 0;
		updateTransitionCountdown(remaining);
	} else {
		restoreTransitionCountdownTemplates();
	}

	if ((state == RuntimeState::Filling || state == RuntimeState::Delayed) && !capture_.active()) {
		failRuntime(capture_.backendStatus().empty() ? "Scene capture stopped unexpectedly" : capture_.backendStatus());
		return;
	}

	const uint32_t bufferDepth = capture_.bufferDepthSeconds();
	if (state == RuntimeState::Filling && bufferDepth >= settings_.targetDelaySeconds &&
	    !capture_.playbackReady(settings_.targetDelaySeconds))
		return;

	stateMachine_.updateBufferDepth(bufferDepth);

	if (stateMachine_.state() == RuntimeState::Delayed) {
		if (delaySceneSwitchNotBeforeNs_ != 0 && os_gettime_ns() < delaySceneSwitchNotBeforeNs_)
			return;
		delaySceneSwitchNotBeforeNs_ = 0;

		if (capture_.bufferDepthSeconds() < settings_.targetDelaySeconds) {
			failRuntime("Buffer underrun: encoded A/V buffer is below the target delay");
			return;
		}
		if (!capture_.playbackReady(settings_.targetDelaySeconds)) {
			failRuntime("Buffer underrun: no decodable video keyframe for the target delay");
			return;
		}

		setPlaybackBuffers(capture_.encodedBuffer(), settings_.targetDelaySeconds);
		if (!switchedToDelayScene_) {
			if (!obs_ui::switchToSceneByName(settings_.delaySceneName)) {
				failRuntime("Could not switch to delay scene");
				return;
			}
			switchedToDelayScene_ = true;
			restoreTransitionCountdownTemplates();
		}
	}
}

void DelayController::save(obs_data_t *saveData) const
{
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, kSourceScene, settings_.sourceSceneName.c_str());
	obs_data_set_string(root, kTransitionScene, settings_.transitionSceneName.c_str());
	obs_data_set_string(root, kDelayScene, settings_.delaySceneName.c_str());
	obs_data_set_string(root, kVideoEncoder, settings_.videoEncoderName.c_str());
	obs_data_set_string(root, kAudioEncoder, settings_.audioEncoderName.c_str());
	obs_data_set_int(root, kTargetDelay, settings_.targetDelaySeconds);
	obs_data_set_int(root, kVideoBitrate, settings_.videoBitrateKbps);
	obs_data_set_int(root, kAudioBitrate, settings_.audioBitrateKbps);
	obs_data_set_int(root, kKeyframeInterval, settings_.keyframeIntervalSeconds);
	obs_data_set_int(root, kMemoryCap, static_cast<long long>(settings_.memoryCapBytes));
	obs_data_set_obj(saveData, kSaveRoot, root);
	obs_data_release(root);
}

void DelayController::load(obs_data_t *saveData)
{
	obs_data_t *root = obs_data_get_obj(saveData, kSaveRoot);
	if (!root)
		return;

	settings_.sourceSceneName = getString(root, kSourceScene);
	settings_.transitionSceneName = getString(root, kTransitionScene);
	settings_.delaySceneName = getString(root, kDelayScene);
	settings_.videoEncoderName = getString(root, kVideoEncoder);
	if (settings_.videoEncoderName.empty())
		settings_.videoEncoderName = kDefaultVideoEncoderName;
	settings_.audioEncoderName = getString(root, kAudioEncoder);
	if (settings_.audioEncoderName.empty())
		settings_.audioEncoderName = kDefaultAudioEncoderName;
	settings_.targetDelaySeconds = clampDelaySeconds(static_cast<uint32_t>(obs_data_get_int(root, kTargetDelay)));
	settings_.videoBitrateKbps =
		static_cast<uint32_t>(obs_data_get_int(root, kVideoBitrate) ? obs_data_get_int(root, kVideoBitrate)
									    : kDefaultVideoBitrateKbps);
	settings_.audioBitrateKbps =
		static_cast<uint32_t>(obs_data_get_int(root, kAudioBitrate) ? obs_data_get_int(root, kAudioBitrate)
									    : kDefaultAudioBitrateKbps);
	settings_.keyframeIntervalSeconds = static_cast<uint32_t>(
		obs_data_get_int(root, kKeyframeInterval) ? obs_data_get_int(root, kKeyframeInterval)
							  : kDefaultKeyframeIntervalSeconds);
	settings_.memoryCapBytes = static_cast<size_t>(obs_data_get_int(root, kMemoryCap) ? obs_data_get_int(root, kMemoryCap)
											  : kDefaultMemoryCapBytes);
	obs_data_release(root);

	blog(LOG_INFO,
	     "[obs-comp-delay] settings loaded: source='%s', transition='%s', delay='%s', target=%us, video_encoder='%s', "
	     "audio_encoder='%s'",
	     settings_.sourceSceneName.c_str(), settings_.transitionSceneName.c_str(), settings_.delaySceneName.c_str(),
	     settings_.targetDelaySeconds, settings_.videoEncoderName.c_str(), settings_.audioEncoderName.c_str());
}

const DelaySettings &DelayController::settings() const
{
	return settings_;
}

RuntimeState DelayController::state() const
{
	return stateMachine_.state();
}

uint32_t DelayController::bufferDepthSeconds() const
{
	return capture_.bufferDepthSeconds();
}

uint32_t DelayController::targetDelaySeconds() const
{
	return stateMachine_.targetDelaySeconds();
}

std::string DelayController::statusText() const
{
	std::ostringstream out;
	out << toString(stateMachine_.state());
	if (stateMachine_.state() == RuntimeState::Off)
		out << " | configured " << settings_.targetDelaySeconds << "s";
	else
		out << " | target " << stateMachine_.targetDelaySeconds() << "s";
	out << " | buffer " << capture_.bufferDepthSeconds() << "s";
	if (stateMachine_.state() == RuntimeState::Filling && stateMachine_.targetDelaySeconds() > 0) {
		const uint32_t depth = capture_.bufferDepthSeconds();
		const uint32_t remaining = depth < stateMachine_.targetDelaySeconds() ? stateMachine_.targetDelaySeconds() - depth : 0;
		out << " | ready in " << remaining << "s";
	}
	if (!lastError_.empty())
		out << " | " << lastError_;
	else if (capture_.active())
		out << " | " << capture_.backendStatus();
	return out.str();
}

const std::string &DelayController::lastError() const
{
	return lastError_;
}

bool DelayController::validateSettings(const DelaySettings &settings, std::string &error) const
{
	if (settings.sourceSceneName.empty() || settings.transitionSceneName.empty() || settings.delaySceneName.empty()) {
		error = "Select source, transition, and delay scenes";
		return false;
	}

	if (settings.sourceSceneName == settings.transitionSceneName || settings.sourceSceneName == settings.delaySceneName ||
	    settings.transitionSceneName == settings.delaySceneName) {
		error = "Source, transition, and delay scenes must be distinct";
		return false;
	}

	obs_source_t *sourceScene = obs_ui::getSceneRefByName(settings.sourceSceneName);
	obs_source_t *transitionScene = obs_ui::getSceneRefByName(settings.transitionSceneName);
	obs_source_t *delayScene = obs_ui::getSceneRefByName(settings.delaySceneName);

	if (!sourceScene || !transitionScene || !delayScene) {
		if (sourceScene)
			obs_source_release(sourceScene);
		if (transitionScene)
			obs_source_release(transitionScene);
		if (delayScene)
			obs_source_release(delayScene);
		error = "One or more configured scenes no longer exists";
		return false;
	}

	const bool sourceContainsPlayback = obs_ui::sceneContainsSourceId(sourceScene, kPlaybackSourceId);
	const bool delayContainsPlayback = obs_ui::sceneContainsSourceId(delayScene, kPlaybackSourceId);
	obs_source_release(sourceScene);
	obs_source_release(transitionScene);
	obs_source_release(delayScene);

	if (sourceContainsPlayback) {
		error = "Source scene must not contain the Comp Delay Playback source";
		return false;
	}

	if (!delayContainsPlayback) {
		error = "Delay scene must contain the Comp Delay Playback source";
		return false;
	}

	if (!settingsFitMemoryCap(settings)) {
		error = "Configured bitrate and delay exceed the 600 MB memory cap";
		return false;
	}

	if (!isFfmpegVideoEncoderAvailable(settings.videoEncoderName)) {
		error = "Selected video encoder is unavailable or not H.264: " + settings.videoEncoderName;
		return false;
	}

	if (!isFfmpegAudioEncoderAvailable(settings.audioEncoderName)) {
		error = "Selected audio encoder is unavailable or not AAC: " + settings.audioEncoderName;
		return false;
	}

	if (!runtimeBuffersFitMemoryCap(settings, error))
		return false;

	return true;
}

bool DelayController::runtimeBuffersFitMemoryCap(const DelaySettings &settings, std::string &error) const
{
	if (settings.targetDelaySeconds == 0)
		return true;

	struct obs_video_info videoInfo = {};
	if (!obs_get_video_info(&videoInfo)) {
		error = "OBS video is not initialized";
		return false;
	}

	(void)videoInfo;

	const double estimatedBytes = static_cast<double>(
		estimateEncodedBytes(encodedRetentionSeconds(settings), settings.videoBitrateKbps, settings.audioBitrateKbps));
	if (estimatedBytes <= static_cast<double>(settings.memoryCapBytes))
		return true;

	std::ostringstream out;
	out << "Configured encoded A/V buffer needs about "
	    << static_cast<uint64_t>(std::ceil(estimatedBytes / 1024.0 / 1024.0))
	    << " MB for this delay; lower bitrate or delay";
	error = out.str();
	return false;
}

bool DelayController::canRetargetActiveDelay(const DelaySettings &current, const DelaySettings &next) const
{
	if (!capture_.active() || next.targetDelaySeconds == 0)
		return false;

	const RuntimeState state = stateMachine_.state();
	if (state != RuntimeState::Filling && state != RuntimeState::Delayed)
		return false;

	return current.sourceSceneName == next.sourceSceneName &&
	       current.transitionSceneName == next.transitionSceneName && current.delaySceneName == next.delaySceneName &&
	       current.videoEncoderName == next.videoEncoderName && current.audioEncoderName == next.audioEncoderName &&
	       current.videoBitrateKbps == next.videoBitrateKbps && current.audioBitrateKbps == next.audioBitrateKbps &&
	       current.keyframeIntervalSeconds == next.keyframeIntervalSeconds &&
	       current.memoryCapBytes == next.memoryCapBytes;
}

void DelayController::retargetActiveDelay(uint32_t previousDelaySeconds)
{
	lastError_.clear();
	switchedToDelayScene_ = false;
	setPlaybackBuffers(nullptr, 0);
	delaySceneSwitchNotBeforeNs_ = os_gettime_ns() + 1000000000ULL;

	std::string error;
	if (!validateSettings(settings_, error)) {
		blog(LOG_WARNING, "[obs-comp-delay] retarget rejected: %s", error.c_str());
		failRuntime(error);
		return;
	}

	if (!obs_ui::switchToSceneByName(settings_.transitionSceneName)) {
		blog(LOG_WARNING, "[obs-comp-delay] retarget rejected: could not switch to transition scene");
		failRuntime("Could not switch to transition scene");
		return;
	}
	updateTransitionCountdown(settings_.targetDelaySeconds);

	capture_.updateRetention(settings_);

	if (settings_.targetDelaySeconds < previousDelaySeconds)
		trimBufferForDelay(settings_.targetDelaySeconds);

	stateMachine_.requestDelay(settings_.targetDelaySeconds);
}

void DelayController::trimBufferForDelay(uint32_t delaySeconds)
{
	auto buffer = capture_.encodedBuffer();
	if (!buffer)
		return;

	const auto targetTimestamp = buffer->targetTimestampForDelay(delaySeconds);
	if (!targetTimestamp)
		return;

	const auto decodeStart = buffer->videoKeyframeAtOrBefore(*targetTimestamp);
	if (decodeStart)
		buffer->dropBefore(*decodeStart);
}

void DelayController::failRuntime(const std::string &message)
{
	blog(LOG_WARNING, "[obs-comp-delay] runtime failure: %s", message.c_str());
	lastError_ = message;
	setPlaybackBuffers(nullptr, 0);
	capture_.stop();
	stateMachine_.fail(message);
	switchedToDelayScene_ = false;
	delaySceneSwitchNotBeforeNs_ = 0;
	switchToTransitionOrFail();
	restoreTransitionCountdownTemplates();
}

void DelayController::switchToTransitionOrFail()
{
	if (!settings_.transitionSceneName.empty())
		obs_ui::switchToSceneByName(settings_.transitionSceneName);
}

void DelayController::updateTransitionCountdown(uint32_t remainingSeconds)
{
	obs_source_t *transitionScene = obs_ui::getSceneRefByName(settings_.transitionSceneName);
	if (!transitionScene)
		return;

	obs_scene_t *scene = obs_scene_from_source(transitionScene);
	if (scene) {
		CountdownTextUpdate update;
		update.templates = &transitionCountdownTemplates_;
		update.remainingSeconds = remainingSeconds;
		update.visitedScenes.insert(sourceKey(transitionScene));
		obs_scene_enum_items(scene, enumCountdownSceneItem, &update);
	}

	obs_source_release(transitionScene);
}

void DelayController::restoreTransitionCountdownTemplates()
{
	if (transitionCountdownTemplates_.empty())
		return;

	obs_source_t *transitionScene = obs_ui::getSceneRefByName(settings_.transitionSceneName);
	if (transitionScene) {
		obs_scene_t *scene = obs_scene_from_source(transitionScene);
		if (scene) {
			CountdownTextUpdate update;
			update.templates = &transitionCountdownTemplates_;
			update.restore = true;
			update.visitedScenes.insert(sourceKey(transitionScene));
			obs_scene_enum_items(scene, enumCountdownSceneItem, &update);
		}
		obs_source_release(transitionScene);
	}

	transitionCountdownTemplates_.clear();
}

} // namespace comp_delay
