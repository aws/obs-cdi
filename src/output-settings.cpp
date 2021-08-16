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
	connect(ui->buttonBox, SIGNAL(accepted()),
		this, SLOT(onFormAccepted()));

	ui->cdiVersionLabel->setText("OBS CDI plugin v1.6.0 GA");
	ui->cdiNotesLabel->setText("Requires I444 Color and Stereo Audio. Set accordingly in Settings.");
}

void OutputSettings::onFormAccepted() {
	Config* conf = Config::Current();

	conf->OutputEnabled = ui->mainOutputGroupBox->isChecked();
	conf->OutputName = ui->mainOutputName->text();
	conf->OutputDest = ui->mainOutputDest->text();
	conf->OutputPort = ui->mainOutputPort->text();
	conf->OutputEFA = ui->mainOutputEFA->text();
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
	ui->mainOutputPort->setText(conf->OutputPort);
	ui->mainOutputEFA->setText(conf->OutputEFA);
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
