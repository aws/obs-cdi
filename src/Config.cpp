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

#include "Config.h"
#include "obs-cdi.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

Config* Config::_instance = nullptr;

Config::Config() :
	OutputEnabled(false),
	OutputName("OBS"),
	OutputDest("Enter Dest IP Address"),
	OutputPort(5000),
	OutputIP("Enter Local EFA Address"),
	OutputVideoStreamId(1),
	OutputAudioStreamId(2),
	OutputVideoSampling(kCdiAvmVidYCbCr422),
	OutputBitDepth(kCdiAvmVidBitDepth10)
{
	config_t* obs_config = obs_frontend_get_global_config();
	if (obs_config) {
		config_set_default_bool(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_ENABLED, OutputEnabled);
		config_set_default_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_NAME, OutputName.toUtf8().constData());
		config_set_default_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST, OutputDest.toUtf8().constData());
		config_set_default_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT, OutputPort);
		config_set_default_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_IP, OutputIP.toUtf8().constData());
		config_set_default_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID, OutputVideoStreamId);
		config_set_default_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID, OutputAudioStreamId);
		config_set_default_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_SAMPLING, (int)kCdiAvmVidYCbCr422);
		config_set_default_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH, (int)kCdiAvmVidBitDepth10);
	}
}

void Config::Load() {
	config_t* obs_config = obs_frontend_get_global_config();
	if (obs_config) {
		OutputEnabled = config_get_bool(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_ENABLED);
		OutputName = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_NAME);
		OutputDest = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST);
		OutputPort = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT);
		OutputIP = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_IP);
		OutputVideoStreamId = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID);
		OutputAudioStreamId = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID);
		OutputVideoSampling = (CdiAvmVideoSampling)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH);
		OutputBitDepth = (CdiAvmVideoBitDepth)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH);
	}
}

void Config::Save() {
	config_t* obs_config = obs_frontend_get_global_config();
	if (obs_config) {
		config_set_bool(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_ENABLED, OutputEnabled);
		config_set_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_NAME, OutputName.toUtf8().constData());
		config_set_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST, OutputDest.toUtf8().constData());
		config_set_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT, OutputPort);
		config_set_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_IP, OutputIP.toUtf8().constData());
		config_set_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID, OutputVideoStreamId);
		config_set_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID, OutputAudioStreamId);
		config_set_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_SAMPLING, (int)OutputVideoSampling);
		config_set_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH, (int)OutputBitDepth);
		config_save(obs_config);
	}
}

Config* Config::Current() {
	if (!_instance) {
		_instance = new Config();
	}
	return _instance;
}
