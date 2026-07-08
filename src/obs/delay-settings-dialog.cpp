#include "delay-settings-dialog.hpp"

#include "codec/ffmpeg-video-codec.hpp"
#include "core/delay-settings.hpp"
#include "obs/scene-utils.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace comp_delay {

DelaySettingsDialog::DelaySettingsDialog(DelayController &controller, QWidget *parent)
	: QDialog(parent), controller_(controller)
{
	setWindowTitle("Comp Delay Settings");
	setModal(false);
	resize(520, 260);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *form = new QFormLayout();
	sourceScene_ = new QComboBox(this);
	transitionScene_ = new QComboBox(this);
	delayScene_ = new QComboBox(this);
	videoEncoder_ = new QComboBox(this);
	audioEncoder_ = new QComboBox(this);
	delaySeconds_ = new QSpinBox(this);
	delaySeconds_->setRange(0, static_cast<int>(kMaxDelaySeconds));
	delaySeconds_->setSuffix(" s");

	form->addRow("Source scene", sourceScene_);
	form->addRow("Delay Transition Scene", transitionScene_);
	form->addRow("Delay scene", delayScene_);
	form->addRow("Video encoder", videoEncoder_);
	form->addRow("Audio encoder", audioEncoder_);
	form->addRow("Delay", delaySeconds_);
	layout->addLayout(form);

	bufferProgress_ = new QProgressBar(this);
	bufferProgress_->setRange(0, static_cast<int>(kMaxDelaySeconds));
	bufferProgress_->setFormat("%v s / %m s");
	layout->addWidget(bufferProgress_);

	status_ = new QLabel("OFF", this);
	status_->setWordWrap(true);
	layout->addWidget(status_);

	error_ = new QLabel(this);
	error_->setWordWrap(true);
	error_->setStyleSheet("color: #c0392b;");
	layout->addWidget(error_);

	auto *buttons = new QHBoxLayout();
	apply_ = new QPushButton("Apply", this);
	auto *close = new QPushButton("Close", this);
	buttons->addStretch();
	buttons->addWidget(apply_);
	buttons->addWidget(close);
	layout->addLayout(buttons);

	connect(apply_, &QPushButton::clicked, this, [this]() { applyClicked(); });
	connect(close, &QPushButton::clicked, this, [this]() { hide(); });
	connect(sourceScene_, &QComboBox::currentTextChanged, this, [this]() { markDirty(); });
	connect(transitionScene_, &QComboBox::currentTextChanged, this, [this]() { markDirty(); });
	connect(delayScene_, &QComboBox::currentTextChanged, this, [this]() { markDirty(); });
	connect(videoEncoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
	connect(audioEncoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
	connect(delaySeconds_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { markDirty(); });

	refreshScenes();
	refreshEncoders();
	syncFromController();
	updateStatus();
}

void DelaySettingsDialog::refreshScenes()
{
	const QString oldSource = sourceScene_->currentText();
	const QString oldTransition = transitionScene_->currentText();
	const QString oldDelay = delayScene_->currentText();

	sourceScene_->clear();
	transitionScene_->clear();
	delayScene_->clear();

	for (const auto &name : obs_ui::listSceneNames()) {
		const QString item = QString::fromStdString(name);
		sourceScene_->addItem(item);
		transitionScene_->addItem(item);
		delayScene_->addItem(item);
	}

	if (!oldSource.isEmpty())
		sourceScene_->setCurrentText(oldSource);
	if (!oldTransition.isEmpty())
		transitionScene_->setCurrentText(oldTransition);
	if (!oldDelay.isEmpty())
		delayScene_->setCurrentText(oldDelay);
}

void DelaySettingsDialog::refreshEncoders()
{
	const QString oldVideo = videoEncoder_->currentData().toString();
	const QString oldAudio = audioEncoder_->currentData().toString();

	videoEncoder_->clear();
	for (const auto &encoder : listFfmpegVideoEncoders())
		videoEncoder_->addItem(QString::fromStdString(encoder.displayName), QString::fromStdString(encoder.name));
	if (videoEncoder_->count() == 0)
		videoEncoder_->addItem("No H.264 encoder available", "");

	audioEncoder_->clear();
	for (const auto &encoder : listFfmpegAudioEncoders())
		audioEncoder_->addItem(QString::fromStdString(encoder.displayName), QString::fromStdString(encoder.name));
	if (audioEncoder_->count() == 0)
		audioEncoder_->addItem("No AAC encoder available", "");

	setComboData(videoEncoder_, oldVideo.toStdString(), "Unavailable video encoder");
	setComboData(audioEncoder_, oldAudio.toStdString(), "Unavailable audio encoder");
}

DelaySettings DelaySettingsDialog::currentSettings() const
{
	DelaySettings settings = controller_.settings();
	settings.sourceSceneName = sourceScene_->currentText().toStdString();
	settings.transitionSceneName = transitionScene_->currentText().toStdString();
	settings.delaySceneName = delayScene_->currentText().toStdString();
	settings.videoEncoderName = videoEncoder_->currentData().toString().toStdString();
	settings.audioEncoderName = audioEncoder_->currentData().toString().toStdString();
	settings.targetDelaySeconds = clampDelaySeconds(static_cast<uint32_t>(delaySeconds_->value()));
	return settings;
}

void DelaySettingsDialog::syncFromController()
{
	syncing_ = true;
	const DelaySettings &settings = controller_.settings();
	setComboText(sourceScene_, settings.sourceSceneName);
	setComboText(transitionScene_, settings.transitionSceneName);
	setComboText(delayScene_, settings.delaySceneName);

	const bool videoEncoderSelected = setComboData(videoEncoder_, settings.videoEncoderName, "Unavailable video encoder");
	const bool audioEncoderSelected = setComboData(audioEncoder_, settings.audioEncoderName, "Unavailable audio encoder");

	delaySeconds_->setValue(static_cast<int>(settings.targetDelaySeconds));
	syncing_ = false;
	setDirty(!videoEncoderSelected || !audioEncoderSelected);
}

void DelaySettingsDialog::applyClicked()
{
	controller_.applySettings(currentSettings());
	setDirty(false);
	updateStatus();
}

void DelaySettingsDialog::markDirty()
{
	if (!syncing_)
		setDirty(true);
}

void DelaySettingsDialog::setDirty(bool dirty)
{
	dirty_ = dirty;
	if (apply_)
		apply_->setEnabled(dirty_);
}

void DelaySettingsDialog::updateStatus()
{
	status_->setText(QString::fromStdString(controller_.statusText()));
	error_->setText(QString::fromStdString(controller_.lastError()));
	bufferProgress_->setMaximum(static_cast<int>(std::max(controller_.targetDelaySeconds(), uint32_t{1})));
	bufferProgress_->setValue(static_cast<int>(controller_.bufferDepthSeconds()));
}

void DelaySettingsDialog::setComboText(QComboBox *combo, const std::string &text)
{
	if (text.empty())
		return;

	const QString value = QString::fromStdString(text);
	const int index = combo->findText(value);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

bool DelaySettingsDialog::setComboData(QComboBox *combo, const std::string &data, const std::string &missingLabel)
{
	(void)missingLabel;
	if (!combo || data.empty())
		return false;

	const QString value = QString::fromStdString(data);
	const int index = combo->findData(value);
	if (index >= 0) {
		combo->setCurrentIndex(index);
		return true;
	}

	return false;
}

} // namespace comp_delay
