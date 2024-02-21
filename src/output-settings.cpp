/*
-------------------------------------------------------------------------------------------
  Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.

  Licensed under the Apache License, Version 2.0 (the "License").
  You may not use this file except in compliance with the License.
  You may obtain a copy of the License at

	  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-------------------------------------------------------------------------------------------
*/

#include "output-settings.h"
#include "Config.h"
#include "obs-cdi.h"

OutputSettings::OutputSettings(QWidget *parent) :
	QDialog(parent),
	ui(new Ui::OutputSettings)
{
	ui->setupUi(this);
	connect(ui->buttonBox, SIGNAL(accepted()), this, SLOT(onFormAccepted()));

	ui->cdiVersionLabel->setText("OBS CDI plugin " OBS_CDI_VERSION);
	ui->cdiNotesLabel->setText("Requires I444 or RGBA Color. Set accordingly in Settings.");

	ui->mainComboBoxVideoSampling->addItem("YCbCr 4:4:4", (int)kCdiAvmVidYCbCr444);
	ui->mainComboBoxVideoSampling->addItem("YCbCr 4:2:2", (int)kCdiAvmVidYCbCr444);
	ui->mainComboBoxVideoSampling->addItem("RGB", (int)kCdiAvmVidRGB);
	connect(ui->mainComboBoxVideoSampling, &QComboBox::currentIndexChanged, this, &OutputSettings::VideoSamplingChanged);

	ui->mainComboBoxBitDepth->addItem("8-bit", (int)kCdiAvmVidBitDepth8);
	ui->mainComboBoxBitDepth->addItem("10-bit", (int)kCdiAvmVidBitDepth10);
	ui->mainComboBoxBitDepth->addItem("12-bit", (int)kCdiAvmVidBitDepth12);
	connect(ui->mainComboBoxBitDepth, &QComboBox::currentIndexChanged, this, &OutputSettings::BitDepthChanged);
}

void OutputSettings::UpdateControls()
{
	Config* conf = Config::Current();
	bool not_supported = false;

	switch (conf->OutputVideoSampling) {
		case kCdiAvmVidYCbCr422:
		case kCdiAvmVidYCbCr444:
			ui->mainCheckBoxAlphaUsed->setEnabled(false);
			ui->cdiNotesLabel->setText("Requires I444 Color. Set accordingly in Settings.");
		break;
		case kCdiAvmVidRGB:
			ui->mainCheckBoxAlphaUsed->setEnabled(true);
			ui->cdiNotesLabel->setText("Requires RGBA Color. Set accordingly in Settings.");
		break;
	}

	if (not_supported) {
		ui->cdiNotesLabel->setText("Video output configuration is not supported yet.");
	}
}

void OutputSettings::VideoSamplingChanged(int index)
{
	Config* conf = Config::Current();

	conf->OutputVideoSampling = (CdiAvmVideoSampling)ui->mainComboBoxVideoSampling->currentIndex();
	UpdateControls();
}

void OutputSettings::BitDepthChanged(int index)
{
	Config* conf = Config::Current();

	conf->OutputBitDepth = (CdiAvmVideoBitDepth)ui->mainComboBoxBitDepth->currentIndex();
	UpdateControls();
}

void OutputSettings::onFormAccepted() {
	Config* conf = Config::Current();

	conf->OutputEnabled = ui->mainOutputGroupBox->isChecked();
	conf->OutputName = ui->mainOutputName->text();
	conf->OutputDest = ui->mainOutputDest->text();
	conf->OutputPort = ui->mainOutputPort->text().toInt();
	conf->OutputIP = ui->mainOutputIP->text();
	conf->OutputVideoStreamId = ui->mainVideoStreamId->text().toInt();
	conf->OutputAudioStreamId = ui->mainAudioStreamId->text().toInt();
	conf->OutputVideoSampling = (CdiAvmVideoSampling)ui->mainComboBoxVideoSampling->currentIndex();
	conf->OutputAlphaUsed = ui->mainCheckBoxAlphaUsed->isChecked();
	conf->OutputBitDepth = (CdiAvmVideoBitDepth)ui->mainComboBoxBitDepth->currentIndex();

	conf->Save();

	if (conf->OutputEnabled) {
		if (main_output_is_running()) {
			main_output_stop();
		}
		main_output_start(ui->mainOutputName->text().toUtf8().constData());
	} else {
		main_output_stop();
	}
}

void OutputSettings::showEvent(QShowEvent* event) {
	Config* conf = Config::Current();

	ui->mainOutputGroupBox->setChecked(conf->OutputEnabled);
	ui->mainOutputName->setText(conf->OutputName);
	ui->mainOutputDest->setText(conf->OutputDest);
	ui->mainOutputPort->setText(QString::number(conf->OutputPort));
	ui->mainOutputIP->setText(conf->OutputIP);
	ui->mainVideoStreamId->setText(QString::number(conf->OutputVideoStreamId));
	ui->mainAudioStreamId->setText(QString::number(conf->OutputAudioStreamId));
	ui->mainComboBoxVideoSampling->setCurrentIndex(conf->OutputVideoSampling);
	ui->mainCheckBoxAlphaUsed->setChecked(conf->OutputAlphaUsed);
	ui->mainComboBoxBitDepth->setCurrentIndex(conf->OutputBitDepth);
}

void OutputSettings::ToggleShowHide() {
	if (!isVisible())
		setVisible(true);
	else
		setVisible(false);
}

OutputSettings::~OutputSettings() {
	delete ui;
}
