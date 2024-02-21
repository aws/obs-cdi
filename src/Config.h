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

#ifndef CONFIG_H
#define CONFIG_H

#include <QString>
#include <obs-module.h>
#include "cdi_baseline_profile_02_00_api.h"

#define SECTION_NAME "CDIPlugin"
#define PARAM_MAIN_OUTPUT_ENABLED "MainOutputEnabled"
#define PARAM_MAIN_OUTPUT_NAME "MainOutputName"
#define PARAM_MAIN_OUTPUT_DEST "MainOutputDest"
#define PARAM_MAIN_OUTPUT_PORT "MainOutputPort"
#define PARAM_MAIN_OUTPUT_IP "MainOutputIP"
#define PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID "MainOutputVideoStreamId"
#define PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID "MainOutputAudioStreamId"
#define PARAM_MAIN_OUTPUT_VIDEO_SAMPLING "MainOutputComboBoxVideoSampling"
#define PARAM_MAIN_OUTPUT_ALPHA_USED "MainOutputCheckBoxAlphaUsed"
#define PARAM_MAIN_OUTPUT_BIT_DEPTH "MainOutputComboBoxBitDepth"

class Config {
  public:
	Config();
	static void OBSSaveCallback(obs_data_t* save_data,
		bool saving, void* private_data);
	static Config* Current();
	void Load();
	void Save();

	bool OutputEnabled;
	QString OutputName;
	QString OutputDest;
	int OutputPort;
	QString OutputIP;
	int OutputVideoStreamId;
	int OutputAudioStreamId;
	bool PreviewOutputEnabled;
	CdiAvmVideoSampling OutputVideoSampling;
	bool OutputAlphaUsed;
	CdiAvmVideoBitDepth OutputBitDepth;

  private:
	static Config* _instance;
};

#endif // CONFIG_H
