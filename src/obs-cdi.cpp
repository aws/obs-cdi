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

#include <sys/stat.h>
#include <mutex>

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

struct obs_source_info cdi_source_info;
struct obs_output_info cdi_output_info;

QLibrary* loaded_lib = nullptr;
OutputSettings* output_settings;
CdiLogMethodData log_method_data;

static std::mutex adapter_mutex;
static volatile int adapter_ref_count = 0;
static CdiAdapterHandle adapter_handle = nullptr;
static CdiAdapterData adapter_data{};

CdiAdapterHandle NetworkAdapterInitialize(const char* local_adapter_ip_str, void** ret_tx_buffer_ptr)
{
	std::lock_guard<std::mutex> guard(adapter_mutex);

	if (0 == adapter_ref_count) {
		adapter_data.adapter_ip_addr_str = local_adapter_ip_str;
        adapter_data.tx_buffer_size_bytes = MAX_PAYLOAD_SIZE * MAX_NUMBER_OF_TX_PAYLOADS;
		adapter_data.adapter_type = kCdiAdapterTypeEfa;

        blog(LOG_INFO, "Local IP: %s", local_adapter_ip_str);
		if (kCdiStatusOk != CdiCoreNetworkAdapterInitialize(&adapter_data, &adapter_handle)) {
			return nullptr;
		}
	}
	adapter_ref_count++;

	if (ret_tx_buffer_ptr) {
		*ret_tx_buffer_ptr = adapter_data.ret_tx_buffer_ptr;
	}

	return adapter_handle;
}

void NetworkAdapterDestroy(void)
{
	std::lock_guard<std::mutex> guard(adapter_mutex);

	assert(0 != adapter_ref_count);
	if (1 == adapter_ref_count) {
        CdiCoreNetworkAdapterDestroy(adapter_handle);
		adapter_handle = nullptr;
		memset(&adapter_data, 0, sizeof(adapter_data));
	}
	adapter_ref_count--;
}

void TestConsoleLogMessageCallback(const CdiLogMessageCbData* cb_data_ptr)
{
    if (CdiLoggerIsEnabled(NULL, cb_data_ptr->component, cb_data_ptr->log_level)) {
        // We need to generate a single log message that contains an optional function name and line number for the
        // first line. Multiline messages need to have each line separated with a line ending character. This is all
        // handled by the Multiline API functions, so we will just use them.
        CdiLogMultilineState m_state;
        CdiLoggerMultilineBegin(NULL, cb_data_ptr->component, cb_data_ptr->log_level,
                                cb_data_ptr->source_code_function_name_ptr, cb_data_ptr->source_code_line_number,
                                &m_state);
        // Walk through each line and write to the new single log message buffer.
        const char* line_str = cb_data_ptr->message_str;
        for (int i = 0; i < cb_data_ptr->line_count; i++) {
            CdiLoggerMultiline(&m_state, line_str);
            line_str += strlen(line_str) + 1; // Advance pointer to byte just past end of the current string.
        }

        char* log_str = CdiLoggerMultilineGetBuffer(&m_state);
		// Logs are written to: C:\Users\<username>\AppData\Roaming\OBS\logs
		blog(LOG_INFO, "%s", log_str); // send to stdout
        CdiLoggerMultilineEnd(&m_state);
    }
}

bool obs_module_load(void)
{
	QMainWindow* main_window = (QMainWindow*)obs_frontend_get_main_window();

	cdi_source_info = create_cdi_source_info();
	obs_register_source(&cdi_source_info);

	cdi_output_info = create_cdi_output_info();
	obs_register_output(&cdi_output_info);

    // CDI-SDK log messages got to a callback, so they can use the OBS blog() API.
    log_method_data.log_method = kLogMethodCallback;
    log_method_data.callback_data.log_msg_cb_ptr = TestConsoleLogMessageCallback;
    log_method_data.callback_data.log_user_cb_param = NULL;

    CdiCoreConfigData core_config;
    core_config.default_log_level = kLogDebug;
    core_config.global_log_method_data_ptr = &log_method_data;
    core_config.cloudwatch_config_ptr = NULL; //Don't use cloudwatch for this. This can be changed later if someone wants to have the
                                              // payloads tracked in CloudWatch.

    // Init CDI with the core config we just built.
    CdiReturnStatus rs = CdiCoreInitialize(&core_config);
    blog(LOG_INFO, "CdiCoreInitialize: %d", rs);
	if (kCdiStatusOk != rs) {
		return false;
	}

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

void obs_module_unload(void)
{
	CdiCoreShutdown();
}

const char* obs_module_name()
{
	return "obs-cdi";
}

const char* obs_module_description()
{
	return "CDI Output for OBS Studio";
}
