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

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include "obs-cdi.h"

static obs_output_t* main_out = nullptr;
static bool main_output_running = false;

void main_output_init(const char* default_name)
{
	if (main_out) return;

	obs_data_t* settings = obs_data_create();
	obs_data_set_string(settings, "cdi_name", default_name);
	main_out = obs_output_create(
			"cdi_output", "CDI Main Output", settings, nullptr
	);
	obs_data_release(settings);
}

void main_output_start(const char* output_name)
{
	if (main_output_running || !main_out) return;

	blog(LOG_INFO, "starting CDI main output with name '%s'", output_name);

	obs_data_t* settings = obs_output_get_settings(main_out);
	obs_data_set_string(settings, "cdi_name", output_name);
	obs_output_update(main_out, settings);
	obs_data_release(settings);

	obs_output_start(main_out);
	main_output_running = true;
}

void main_output_stop()
{
	if (!main_output_running) return;

	blog(LOG_INFO, "stopping CDI main output");

	obs_output_stop(main_out);
	main_output_running = false;
}

void main_output_deinit()
{
	obs_output_release(main_out);
	main_out = nullptr;
	main_output_running = false;
}

bool main_output_is_running()
{
	return main_output_running;
}