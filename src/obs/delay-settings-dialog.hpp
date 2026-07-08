#pragma once

#include "obs/delay-controller.hpp"

#include <QDialog>

#include <string>

class QComboBox;
class QLabel;
class QProgressBar;
class QSpinBox;
class QTimer;

namespace comp_delay {

class DelaySettingsDialog : public QDialog {
public:
	explicit DelaySettingsDialog(DelayController &controller, QWidget *parent = nullptr);

	void refreshScenes();
	void refreshEncoders();
	DelaySettings currentSettings() const;
	void syncFromController();
	void updateStatus();

private:
	void applyClicked();
	void goLiveClicked();
	void setComboText(QComboBox *combo, const std::string &text);
	void setComboData(QComboBox *combo, const std::string &data, const std::string &missingLabel);

	DelayController &controller_;
	QComboBox *sourceScene_ = nullptr;
	QComboBox *transitionScene_ = nullptr;
	QComboBox *delayScene_ = nullptr;
	QComboBox *videoEncoder_ = nullptr;
	QComboBox *audioEncoder_ = nullptr;
	QSpinBox *delaySeconds_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *error_ = nullptr;
	QProgressBar *bufferProgress_ = nullptr;
};

} // namespace comp_delay
