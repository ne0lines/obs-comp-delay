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

#include <QBoxLayout>
#include <QDockWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kHotkeyApply = "obs_comp_delay.apply_configured_delay";
constexpr const char *kHotkeyApplyDescription = "Comp Delay: Activate delay";
constexpr const char *kHotkeyGoLive = "obs_comp_delay.go_live";
constexpr const char *kHotkeyGoLiveDescription = "Comp Delay: Deactivate delay";
constexpr const char *kHotkeyDelayedGoLive = "obs_comp_delay.deactivate_after_delay";
constexpr const char *kHotkeyDelayedGoLiveDescription = "Comp Delay: Deactivate after delay";
constexpr const char *kActivateDelayText = "Activate delay";
constexpr const char *kDeactivateDelayText = "Deactivate delay";
constexpr const char *kControlsToggleObjectName = "compDelayToggleButton";
constexpr const char *kActivateDelayButtonStyle =
	"QPushButton#compDelayToggleButton { background-color: #1f883d; color: white; font-weight: 600; }"
	"QPushButton#compDelayToggleButton:hover { background-color: #2da44e; }"
	"QPushButton#compDelayToggleButton:pressed { background-color: #116329; }"
	"QPushButton#compDelayToggleButton:disabled { background-color: #6e7781; color: #d0d7de; }";
constexpr const char *kDeactivateDelayButtonStyle =
	"QPushButton#compDelayToggleButton { background-color: #cf222e; color: white; font-weight: 600; }"
	"QPushButton#compDelayToggleButton:hover { background-color: #da3633; }"
	"QPushButton#compDelayToggleButton:pressed { background-color: #a40e26; }"
	"QPushButton#compDelayToggleButton:disabled { background-color: #6e7781; color: #d0d7de; }";
constexpr const char *kSaveRoot = "comp_delay";
constexpr const char *kApplyHotkey = "apply_hotkey";
constexpr const char *kGoLiveHotkey = "go_live_hotkey";
constexpr const char *kDelayedGoLiveHotkey = "delayed_go_live_hotkey";
constexpr int kNoPendingHotkeyAction = -1;
constexpr int kActivateDelayHotkeyAction = -2;
constexpr int kDeactivateDelayHotkeyAction = -3;
constexpr int kDelayedDeactivateHotkeyAction = -4;

enum class DeactivateChoice {
	Cancel,
	Now,
	AfterDelay,
};

std::unique_ptr<comp_delay::DelayController> gController;
comp_delay::DelaySettingsDialog *gSettingsDialog = nullptr;
QTimer *gRuntimeTimer = nullptr;
obs_hotkey_id gApplyHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id gGoLiveHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id gDelayedGoLiveHotkey = OBS_INVALID_HOTKEY_ID;
QPointer<QPushButton> gControlsToggleButton;
std::atomic<int> gPendingHotkeyAction{kNoPendingHotkeyAction};
bool gControlsToggleInstallWarningLogged = false;

bool isDelayActive()
{
	if (!gController)
		return false;

	const auto state = gController->state();
	return state == comp_delay::RuntimeState::Filling || state == comp_delay::RuntimeState::Delayed;
}

void queueActivateDelay(const char *source)
{
	if (gController) {
		blog(LOG_INFO, "[obs-comp-delay] activate delay requested from %s", source);
		gPendingHotkeyAction.store(kActivateDelayHotkeyAction);
	}
}

void queueDeactivateDelay(const char *source)
{
	if (gController) {
		blog(LOG_INFO, "[obs-comp-delay] deactivate delay requested from %s", source);
		gPendingHotkeyAction.store(kDeactivateDelayHotkeyAction);
	}
}

void queueDelayedDeactivateDelay(const char *source)
{
	if (gController) {
		blog(LOG_INFO, "[obs-comp-delay] delayed deactivate requested from %s", source);
		gPendingHotkeyAction.store(kDelayedDeactivateHotkeyAction);
	}
}

void updateControlsToggleButton()
{
	if (!gControlsToggleButton)
		return;

	const bool active = isDelayActive();
	gControlsToggleButton->setText(active ? kDeactivateDelayText : kActivateDelayText);
	gControlsToggleButton->setStyleSheet(active ? kDeactivateDelayButtonStyle : kActivateDelayButtonStyle);
	gControlsToggleButton->setToolTip(active ? "Deactivate Comp Delay and return to the source scene"
					       : "Activate the configured Comp Delay");
	gControlsToggleButton->setEnabled(gController != nullptr);
}

DeactivateChoice confirmDeactivateDelay()
{
	if (!gController)
		return DeactivateChoice::Cancel;

	uint32_t skippedSeconds = gController->targetDelaySeconds();
	if (skippedSeconds == 0)
		skippedSeconds = gController->settings().targetDelaySeconds;

	QMessageBox confirmation(QMessageBox::Warning, "Deactivate delay?",
				 QString("Viewers will miss the latest %1 seconds of delayed content.").arg(skippedSeconds),
				 QMessageBox::NoButton, gControlsToggleButton ? gControlsToggleButton.data()
									      : static_cast<QWidget *>(obs_frontend_get_main_window()));
	confirmation.setInformativeText("Deactivate delay and return to the source scene live?");
	auto *deactivateNowButton = confirmation.addButton("Deactivate now", QMessageBox::DestructiveRole);
	auto *deactivateAfterDelayButton =
		confirmation.addButton(QString("Deactivate after %1s").arg(skippedSeconds), QMessageBox::AcceptRole);
	auto *cancelButton = confirmation.addButton("Cancel", QMessageBox::RejectRole);
	confirmation.setDefaultButton(static_cast<QPushButton *>(cancelButton));
	confirmation.exec();

	if (confirmation.clickedButton() == deactivateNowButton)
		return DeactivateChoice::Now;
	if (confirmation.clickedButton() == deactivateAfterDelayButton)
		return DeactivateChoice::AfterDelay;
	return DeactivateChoice::Cancel;
}

void controlsToggleClicked()
{
	if (isDelayActive()) {
		const DeactivateChoice choice = confirmDeactivateDelay();
		if (choice == DeactivateChoice::Now)
			queueDeactivateDelay("controls button");
		else if (choice == DeactivateChoice::AfterDelay)
			queueDelayedDeactivateDelay("controls button");
	} else {
		queueActivateDelay("controls button");
	}
}

void insertControlsToggleButtonAtTop(QBoxLayout *buttonsLayout, QPushButton *button)
{
	if (!buttonsLayout || !button)
		return;

	buttonsLayout->removeWidget(button);
	buttonsLayout->insertWidget(0, button);
}

void installControlsToggleButton()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		if (!gControlsToggleInstallWarningLogged) {
			blog(LOG_WARNING, "[obs-comp-delay] could not add controls button: OBS main window is unavailable");
			gControlsToggleInstallWarningLogged = true;
		}
		return;
	}

	auto *controlsDock = mainWindow->findChild<QDockWidget *>("controlsDock");
	if (!controlsDock || !controlsDock->widget()) {
		if (!gControlsToggleInstallWarningLogged) {
			blog(LOG_WARNING, "[obs-comp-delay] could not add controls button: OBS controls dock was not found");
			gControlsToggleInstallWarningLogged = true;
		}
		return;
	}

	auto *buttonsLayout = controlsDock->widget()->findChild<QBoxLayout *>("buttonsVLayout");
	if (!buttonsLayout) {
		if (!gControlsToggleInstallWarningLogged) {
			blog(LOG_WARNING, "[obs-comp-delay] could not add controls button: OBS controls layout was not found");
			gControlsToggleInstallWarningLogged = true;
		}
		return;
	}

	if (auto *existingButton = controlsDock->widget()->findChild<QPushButton *>(kControlsToggleObjectName)) {
		gControlsToggleButton = existingButton;
		insertControlsToggleButtonAtTop(buttonsLayout, existingButton);
		updateControlsToggleButton();
		return;
	}

	auto *button = new QPushButton(kActivateDelayText, controlsDock->widget());
	button->setObjectName(kControlsToggleObjectName);
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	button->setMinimumWidth(150);
	button->setAccessibleName("Comp Delay");
	QObject::connect(button, &QPushButton::clicked, controlsToggleClicked);

	insertControlsToggleButtonAtTop(buttonsLayout, button);
	gControlsToggleButton = button;
	updateControlsToggleButton();
	blog(LOG_INFO, "[obs-comp-delay] controls dock toggle button installed");
}

void removeControlsToggleButton()
{
	if (gControlsToggleButton) {
		delete gControlsToggleButton;
		gControlsToggleButton = nullptr;
	}
}

void applyHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueActivateDelay("hotkey");
}

void goLiveHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueDeactivateDelay("hotkey");
}

void delayedGoLiveHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueDelayedDeactivateDelay("hotkey");
}

void consumePendingHotkeyAction()
{
	if (!gController)
		return;

	const int action = gPendingHotkeyAction.exchange(kNoPendingHotkeyAction);
	if (action == kNoPendingHotkeyAction)
		return;

	if (action == kActivateDelayHotkeyAction) {
		gController->applyConfiguredDelay();
		return;
	}

	if (action == kDeactivateDelayHotkeyAction) {
		gController->goLive();
		return;
	}

	if (action == kDelayedDeactivateHotkeyAction) {
		gController->scheduleGoLiveAfterCurrentDelay();
		return;
	}

	if (action >= 0)
		blog(LOG_WARNING, "[obs-comp-delay] ignoring unknown queued hotkey action: %d", action);
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
			obs_data_array_t *delayedGoLive = obs_hotkey_save(gDelayedGoLiveHotkey);
			obs_data_set_array(root, kApplyHotkey, apply);
			obs_data_set_array(root, kGoLiveHotkey, goLive);
			obs_data_set_array(root, kDelayedGoLiveHotkey, delayedGoLive);
			if (apply)
				obs_data_array_release(apply);
			if (goLive)
				obs_data_array_release(goLive);
			if (delayedGoLive)
				obs_data_array_release(delayedGoLive);
			obs_data_release(root);
		}
		return;
	}

	gController->load(saveData);

	obs_data_t *root = obs_data_get_obj(saveData, kSaveRoot);
	if (root) {
		obs_data_array_t *apply = obs_data_get_array(root, kApplyHotkey);
		obs_data_array_t *goLive = obs_data_get_array(root, kGoLiveHotkey);
		obs_data_array_t *delayedGoLive = obs_data_get_array(root, kDelayedGoLiveHotkey);
		if (apply) {
			obs_hotkey_load(gApplyHotkey, apply);
			obs_data_array_release(apply);
		}
		if (goLive) {
			obs_hotkey_load(gGoLiveHotkey, goLive);
			obs_data_array_release(goLive);
		}
		if (delayedGoLive) {
			obs_hotkey_load(gDelayedGoLiveHotkey, delayedGoLive);
			obs_data_array_release(delayedGoLive);
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
	gDelayedGoLiveHotkey =
		obs_hotkey_register_frontend(kHotkeyDelayedGoLive, kHotkeyDelayedGoLiveDescription, delayedGoLiveHotkey, nullptr);
	obs_frontend_add_save_callback(saveLoadCallback, nullptr);
	obs_frontend_add_tools_menu_item("Comp Delay Settings", showSettingsDialog, nullptr);
	installControlsToggleButton();

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	gRuntimeTimer = new QTimer(mainWindow);
	QObject::connect(gRuntimeTimer, &QTimer::timeout, []() {
		consumePendingHotkeyAction();
		if (gController)
			gController->tick();
		if (!gControlsToggleButton)
			installControlsToggleButton();
		updateControlsToggleButton();
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
	if (gDelayedGoLiveHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(gDelayedGoLiveHotkey);
		gDelayedGoLiveHotkey = OBS_INVALID_HOTKEY_ID;
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

	removeControlsToggleButton();
	gController.reset();
	obs_log(LOG_INFO, "plugin unloaded");
}
