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

#ifdef _WIN32
#include <Windows.h>
#endif

#include <sys/stat.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QLibrary>
#include <QMainWindow>
#include <QAction>
#include <QMessageBox>
#include <QString>
#include <QStringList>

#include "obs-cdi.h"
#include "main-output.h"
#include "Config.h"
#include "output-settings.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Amazon Web Services")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-cdi", "en-US")


extern struct obs_output_info create_cdi_output_info();
struct obs_output_info cdi_output_info;


QLibrary* loaded_lib = nullptr;
OutputSettings* output_settings;

bool obs_module_load(void)
{
	QMainWindow* main_window = (QMainWindow*)obs_frontend_get_main_window();

	cdi_output_info = create_cdi_output_info();
	obs_register_output(&cdi_output_info);

	if (main_window) {
		Config* conf = Config::Current();
		conf->Load();

		main_output_init(conf->OutputName.toUtf8().constData());

		// Ui setup
		QAction* menu_action = (QAction*)obs_frontend_add_tools_menu_qaction(
			obs_module_text("CDIPlugin.Menu.OutputSettings"));

		obs_frontend_push_ui_translation(obs_module_get_string);
		output_settings = new OutputSettings(main_window);
		obs_frontend_pop_ui_translation();

		auto menu_cb = [] {
			output_settings->ToggleShowHide();
		};
		menu_action->connect(menu_action, &QAction::triggered, menu_cb);

		obs_frontend_add_event_callback([](enum obs_frontend_event event, void* private_data) {
			Config* conf = (Config*)private_data;

			if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
				if (conf->OutputEnabled) {
					main_output_start(conf->OutputName.toUtf8().constData());
				}

			}
			else if (event == OBS_FRONTEND_EVENT_EXIT) {
				main_output_stop();
				main_output_deinit();
			}
			}, (void*)conf);
	}

	return true;
}


const char* obs_module_name()
{
	return "obs-cdi";
}

const char* obs_module_description()
{
	return "CDI Output for OBS Studio";
}

