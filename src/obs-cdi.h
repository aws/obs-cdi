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

#ifndef OBSCDI_H
#define OBSCDI_H

extern "C" {
#include "cdi_logger_api.h"

#define OBS_CDI_VERSION "2.0.0"

/// @brief Number of bytes in CDI audio sample. CDI requests 24-bit int for audio, so needs three bytes.
#define CDI_BYTES_PER_AUDIO_SAMPLE              (3)

#define MAX_PAYLOAD_SIZE								(1920*1080*4*12/8)
#define CDI_MAX_SIMULTANEOUS_TX_PAYLOADS_PER_CONNECTION (8)
#define MAX_NUMBER_OF_TX_PAYLOADS						(CDI_MAX_SIMULTANEOUS_TX_PAYLOADS_PER_CONNECTION + 1)

#define blog(level, msg, ...) blog(level, "[obs-cdi] " msg, ##__VA_ARGS__)

extern CdiLogMethodData log_method_data;

extern obs_source_info create_cdi_source_info();
extern struct obs_output_info create_cdi_output_info();

void main_output_start(const char* output_name);
void main_output_stop();
bool main_output_is_running();

CdiAdapterHandle NetworkAdapterInitialize(const char* local_adapter_ip_str, void** ret_tx_buffer_ptr);
void NetworkAdapterDestroy(void);

void TestConsoleLogMessageCallback(const CdiLogMessageCbData* cb_data_ptr);
};


#endif // OBSCDI_H
