/*
OBS Comp Delay
Copyright (C) 2026 ne0lines

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "obs/delay-controller.hpp"
#include "obs/delay-settings-dialog.hpp"
#include "obs/playback-source.hpp"

#include <obs-frontend-api.h>
#include <obs-hotkey.h>

#include <QTimer>
#include <QWidget>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kHotkeyApply = "obs_comp_delay.apply_configured_delay";
constexpr const char *kHotkeyApplyDescription = "Comp Delay: Apply configured delay";
constexpr const char *kHotkeyGoLive = "obs_comp_delay.go_live";
constexpr const char *kHotkeyGoLiveDescription = "Comp Delay: Go live / 0 delay";
constexpr const char *kSaveRoot = "comp_delay";
constexpr const char *kApplyHotkey = "apply_hotkey";
constexpr const char *kGoLiveHotkey = "go_live_hotkey";
constexpr int kNoPendingHotkeyAction = -1;
constexpr int kApplyConfiguredHotkeyAction = -2;
constexpr int kGoLiveHotkeyAction = -3;

struct PresetHotkey {
	const char *name;
	const char *description;
	const char *saveKey;
	uint32_t delaySeconds;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

std::unique_ptr<comp_delay::DelayController> gController;
comp_delay::DelaySettingsDialog *gSettingsDialog = nullptr;
QTimer *gRuntimeTimer = nullptr;
obs_hotkey_id gApplyHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id gGoLiveHotkey = OBS_INVALID_HOTKEY_ID;
std::atomic<int> gPendingHotkeyAction{kNoPendingHotkeyAction};
std::array<PresetHotkey, 3> gPresetHotkeys = {{
	{"obs_comp_delay.apply_30s", "Comp Delay: Apply 30s delay", "preset_30_hotkey", 30},
	{"obs_comp_delay.apply_60s", "Comp Delay: Apply 60s delay", "preset_60_hotkey", 60},
	{"obs_comp_delay.apply_300s", "Comp Delay: Apply 300s delay", "preset_300_hotkey", 300},
}};

void applyHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && gController) {
		blog(LOG_INFO, "[obs-comp-delay] apply hotkey pressed");
		gPendingHotkeyAction.store(kApplyConfiguredHotkeyAction);
	}
}

void goLiveHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && gController) {
		blog(LOG_INFO, "[obs-comp-delay] go-live hotkey pressed");
		gPendingHotkeyAction.store(kGoLiveHotkeyAction);
	}
}

void presetHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !gController || !data)
		return;

	const auto *preset = static_cast<const PresetHotkey *>(data);
	blog(LOG_INFO, "[obs-comp-delay] preset hotkey pressed: %us", preset->delaySeconds);
	gPendingHotkeyAction.store(static_cast<int>(preset->delaySeconds));
}

void consumePendingHotkeyAction()
{
	if (!gController)
		return;

	const int action = gPendingHotkeyAction.exchange(kNoPendingHotkeyAction);
	if (action == kNoPendingHotkeyAction)
		return;

	if (action == kApplyConfiguredHotkeyAction) {
		gController->applyConfiguredDelay();
		return;
	}

	if (action == kGoLiveHotkeyAction) {
		gController->goLive();
		return;
	}

	if (action >= 0) {
		comp_delay::DelaySettings settings = gController->settings();
		settings.targetDelaySeconds = static_cast<uint32_t>(action);
		gController->applySettings(settings);
	}
}

void showSettingsDialog(void *)
{
	if (!gController)
		return;

	if (!gSettingsDialog) {
		auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		gSettingsDialog = new comp_delay::DelaySettingsDialog(*gController, mainWindow);
	}

	gSettingsDialog->refreshScenes();
	gSettingsDialog->refreshEncoders();
	gSettingsDialog->syncFromController();
	gSettingsDialog->updateStatus();
	gSettingsDialog->show();
	gSettingsDialog->raise();
	gSettingsDialog->activateWindow();
}

void saveLoadCallback(obs_data_t *saveData, bool saving, void *)
{
	if (!gController)
		return;

	if (saving) {
		gController->save(saveData);
		obs_data_t *root = obs_data_get_obj(saveData, kSaveRoot);
		if (root) {
			obs_data_array_t *apply = obs_hotkey_save(gApplyHotkey);
			obs_data_array_t *goLive = obs_hotkey_save(gGoLiveHotkey);
			obs_data_set_array(root, kApplyHotkey, apply);
			obs_data_set_array(root, kGoLiveHotkey, goLive);
			for (auto &preset : gPresetHotkeys) {
				obs_data_array_t *presetData = obs_hotkey_save(preset.id);
				obs_data_set_array(root, preset.saveKey, presetData);
				if (presetData)
					obs_data_array_release(presetData);
			}
			if (apply)
				obs_data_array_release(apply);
			if (goLive)
				obs_data_array_release(goLive);
			obs_data_release(root);
		}
		return;
	}

	gController->load(saveData);

	obs_data_t *root = obs_data_get_obj(saveData, kSaveRoot);
	if (root) {
		obs_data_array_t *apply = obs_data_get_array(root, kApplyHotkey);
		obs_data_array_t *goLive = obs_data_get_array(root, kGoLiveHotkey);
		if (apply) {
			obs_hotkey_load(gApplyHotkey, apply);
			obs_data_array_release(apply);
		}
		if (goLive) {
			obs_hotkey_load(gGoLiveHotkey, goLive);
			obs_data_array_release(goLive);
		}
		for (auto &preset : gPresetHotkeys) {
			obs_data_array_t *presetData = obs_data_get_array(root, preset.saveKey);
			if (presetData) {
				obs_hotkey_load(preset.id, presetData);
				obs_data_array_release(presetData);
			}
		}
		obs_data_release(root);
	}

	if (gSettingsDialog) {
		gSettingsDialog->refreshScenes();
		gSettingsDialog->refreshEncoders();
		gSettingsDialog->syncFromController();
	}
}

} // namespace

bool obs_module_load(void)
{
	comp_delay::registerPlaybackSource();

	gController = std::make_unique<comp_delay::DelayController>();
	gApplyHotkey = obs_hotkey_register_frontend(kHotkeyApply, kHotkeyApplyDescription, applyHotkey, nullptr);
	gGoLiveHotkey = obs_hotkey_register_frontend(kHotkeyGoLive, kHotkeyGoLiveDescription, goLiveHotkey, nullptr);
	for (auto &preset : gPresetHotkeys)
		preset.id = obs_hotkey_register_frontend(preset.name, preset.description, presetHotkey, &preset);
	obs_frontend_add_save_callback(saveLoadCallback, nullptr);
	obs_frontend_add_tools_menu_item("Comp Delay Settings", showSettingsDialog, nullptr);

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	gRuntimeTimer = new QTimer(mainWindow);
	QObject::connect(gRuntimeTimer, &QTimer::timeout, []() {
		consumePendingHotkeyAction();
		if (gController)
			gController->tick();
		if (gSettingsDialog)
			gSettingsDialog->updateStatus();
	});
	gRuntimeTimer->start(250);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(saveLoadCallback, nullptr);

	if (gApplyHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(gApplyHotkey);
		gApplyHotkey = OBS_INVALID_HOTKEY_ID;
	}
	if (gGoLiveHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(gGoLiveHotkey);
		gGoLiveHotkey = OBS_INVALID_HOTKEY_ID;
	}
	for (auto &preset : gPresetHotkeys) {
		if (preset.id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(preset.id);
			preset.id = OBS_INVALID_HOTKEY_ID;
		}
	}

	if (gRuntimeTimer) {
		gRuntimeTimer->stop();
		delete gRuntimeTimer;
		gRuntimeTimer = nullptr;
	}

	if (gSettingsDialog) {
		delete gSettingsDialog;
		gSettingsDialog = nullptr;
	}

	gController.reset();
	obs_log(LOG_INFO, "plugin unloaded");
}
