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

#define OBS_CDI_VERSION "1.6.0"

#define blog(level, msg, ...) blog(level, "[obs-cdi] " msg, ##__VA_ARGS__)

void main_output_start(const char* output_name);
void main_output_stop();
bool main_output_is_running();

#endif // OBSCDI_H
