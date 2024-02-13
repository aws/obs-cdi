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

/**
 * @file
 * @brief
 * This file contains definitions and functions for the receive-side CDI minimal test application.
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <QString>

#include "Config.h"

#include <assert.h>
#include <stdbool.h>

extern "C" {
#include "obs-cdi.h"
#include "cdi_os_api.h"
#include "cdi_core_api.h"
#include "cdi_avm_api.h"
#include "cdi_baseline_profile_api.h"
#include "cdi_log_api.h"
#include "cdi_log_enums.h"
#include "cdi_logger_api.h"
#include "cdi_pool_api.h"
#include "cdi_queue_api.h"
#include "cdi_raw_api.h"
#include "cdi_utility_api.h"
#include "cdi_baseline_profile_02_00_api.h"
};

//*********************************************************************************************************************
//***************************************** START OF DEFINITIONS AND TYPES ********************************************
//*********************************************************************************************************************

#define PROP_LOCAL_IP   "local_ip"
#define PROP_PORT       "listen_port"
#define PROP_AUDIO      "audio_enable"

// Maximum size is 1920x1080, 4 color planes (RGB has alpha), 16-bit pixel size.
#define MAX_VIDEO_FRAME_SIZE        (1920*1080*4*2)

// Maximum size of CDI linear receive buffer.
#define LINEAR_RX_BUFFER_SIZE       (MAX_VIDEO_FRAME_SIZE * 10)

// Maximum size of OBS audio frame (for CDI -> OBS conversion).
#define MAX_OBS_AUDIO_FRAME_SIZE    (10*10000)

/**
 * @brief A structure that holds all the test settings as set from the command line.
 */
struct TestSettings {
    const char* local_adapter_ip_str;  ///< The local network adapter IP address.
    const char* bind_ip_str;           ///< The local network adapter IP to bind to.
    int dest_port;                     ///< The destination port number.
    CdiConnectionProtocolType protocol_type; ///< Protocol type (AVM or RAW).
    int payload_size;                  ///< Payload size in bytes.
};

/**
 * @brief A structure for storing all info related to a specific connection, including test settings, connection
 * configuration data from the SDK, and state information for the test connection.
 */
struct TestConnectionInfo {
    CdiConnectionHandle connection_handle; ///< The connection handle returned by CdiRawTxCreate().

    TestSettings test_settings;            ///< Test settings data structure provided by the user.

    /// @brief Number of payloads successfully received. NOTE: This variable is used by multiple threads and not
    /// declared volatile. It is used as an example of how to use the CdiOsAtomic...() API functions.
    uint32_t payload_received_count;
    volatile bool payload_error;           ///< true if Rx callback got a payload error.

    CdiSignalType connection_state_change_signal;   ///< Signal used for connection state changes.
    volatile CdiConnectionStatus connection_status; ///< Current status of the connection.
};

/**
 * @brief CDI Source configuration structure.
 */
struct cdi_source_config {
    QByteArray cdi_source_name; // CDI source name.
	bool audio_enabled{true}; // Audio enable/disable.
};

/**
 * @brief CDI Source structure.
 */
struct cdi_source {
	obs_source_t* obs_source; // Pointer to OBS source data.
	cdi_source_config config; // CDI source configuration data.

    obs_source_frame obs_video_frame; // OBS video frame structure data.
    obs_source_audio obs_audio_frame; // OBS audio frame structure data.

    uint8_t* conv_buffer; // Buffer used to convert CDI to OBS frame data.

    TestConnectionInfo con_info{}; // Test connection information.
    CdiAvmVideoConfig video_config{}; // AVM video configuration.
    CdiAvmAudioConfig audio_config{}; // AVM audio configuration.

    uint8_t obs_audio_buffer[MAX_OBS_AUDIO_FRAME_SIZE];
};

//*********************************************************************************************************************
//******************************************* START OF STATIC FUNCTIONS ***********************************************
//*********************************************************************************************************************

/**
 * Handle the CDI connection callback.
 *
 * @param cb_data_ptr Pointer to CdiCoreConnectionCbData callback data.
 */
static void TestConnectionCallback(const CdiCoreConnectionCbData* cb_data_ptr)
{
    cdi_source* cdi_ptr = (cdi_source*)cb_data_ptr->connection_user_cb_param;

    // Update connection state and set state change signal.
    cdi_ptr->con_info.connection_status = cb_data_ptr->status_code;
    CdiOsSignalSet(cdi_ptr->con_info.connection_state_change_signal);
}

/**
 * @brief Convert a CDI YCbCr 4:2:2 video frame to OBS.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 * @param payload_ptr Pointer to CDI payload data.
 * @param payload_size Size of CDI payload in bytes.
 * @param timestamp CDI timestamp of the audio frame.
 * @param config_ptr Pointer to AVM CDI video configuration structure.
 *
 * @return true if successful, other false.
 */
static bool Cdi422ToObsVideoFrame(cdi_source* cdi_ptr, uint8_t* payload_ptr, int payload_size, CdiAvmVideoConfig* config_ptr)
{
    bool ret = true;
    obs_source_frame* frame_ptr = &cdi_ptr->obs_video_frame;
    uint8_t* src_ptr = payload_ptr;

    // CDI YCbCr 4:2:2.
    if (kCdiAvmVidBitDepth8 == config_ptr->depth) {
        frame_ptr->format = VIDEO_FORMAT_UYVY;
        frame_ptr->data[0] = cdi_ptr->conv_buffer;
        frame_ptr->linesize[0] = config_ptr->width*2;

        // Flip image vertically by swapping lines, first to last and last to first.
        int linesize = frame_ptr->linesize[0];
        for (int i = 0; i < config_ptr->height/2; i++) {
            uint32_t first_offset = i * linesize;
            uint32_t last_offset = (config_ptr->height - i - 1) * linesize;
            memcpy(cdi_ptr->conv_buffer + first_offset, src_ptr + last_offset, linesize);
            memcpy(cdi_ptr->conv_buffer + last_offset, src_ptr + first_offset, linesize);
        }
    } else if (kCdiAvmVidBitDepth10 == config_ptr->depth) {
        frame_ptr->format = VIDEO_FORMAT_I210;

        // Using 16-bits to hold each 10-bit pixel.
        frame_ptr->data[0] = cdi_ptr->conv_buffer; // Y. One Y for each frame pixel.
        frame_ptr->data[1] = frame_ptr->data[0] + (config_ptr->height * config_ptr->width*2); // U. One U for every 2 frame pixels.
        frame_ptr->data[2] = frame_ptr->data[1] + (config_ptr->height * config_ptr->width); // V. One V for every 2 frame pixels.

        frame_ptr->linesize[0] = config_ptr->width*2; // Y
        frame_ptr->linesize[1] = config_ptr->width; // U
        frame_ptr->linesize[2] = config_ptr->width; // V

        // Have to flip image vertically too, so start writing at last line and work to line zero.
        for (int r = config_ptr->height - 1; r >= 0; r--) {
            uint16_t* y_dest = (uint16_t*)(frame_ptr->data[0] + frame_ptr->linesize[0] * r);
            uint16_t* u_dest = (uint16_t*)(frame_ptr->data[1] + frame_ptr->linesize[1] * r);
            uint16_t* v_dest = (uint16_t*)(frame_ptr->data[2] + frame_ptr->linesize[2] * r);

            // Process 5-bytes of CDI data (SMPTE ST 2110-20: 10-bit packed 4:2:2).
            for (int i = 0; i < config_ptr->width; i += 2) {
                uint16_t val = (uint16_t)*src_ptr << 2 | *(src_ptr+1) >> 6; // U. 512 means zero
                *(u_dest++) = val;

                val = ((uint16_t)*(src_ptr+1) << 4 | *(src_ptr+2) >> 4) & 0x3ff; // Y. 16= black, 960= white
                *(y_dest++) = val;

                val = ((uint16_t)*(src_ptr+2) << 6 | *(src_ptr+3) >> 2) & 0x3ff; // V. 512 means zero
                *(v_dest++) = val;

                val = ((uint16_t)*(src_ptr+3) << 8 | *(src_ptr+4)) & 0x3ff; // Y. 16= black, 960= white
                *(y_dest++) = val;
                
                src_ptr += 5;
            }
        }
    }
    else if (kCdiAvmVidBitDepth12 == config_ptr->depth) {
        blog(LOG_WARNING, "12-bit CDI source not supported.");
        ret = false;
    }

    return ret;
}

/**
 * @brief Convert a CDI YCbCr 4:4:4 video frame to OBS.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 * @param payload_ptr Pointer to CDI payload data.
 * @param payload_size Size of CDI payload in bytes.
 * @param timestamp CDI timestamp of the audio frame.
 * @param config_ptr Pointer to AVM CDI video configuration structure.
 *
 * @return true if successful, other false.
 */
static bool Cdi444ToObsVideoFrame(cdi_source* cdi_ptr, uint8_t* payload_ptr, int payload_size, CdiAvmVideoConfig* config_ptr)
{
    bool ret = true;

    // TODO: Add logic here.
    blog(LOG_WARNING, "YCbCr 4:4:4 CDI source not supported.");
    ret = false;

    return ret;
}

/**
 * @brief Convert a CDI RGB video frame to OBS.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 * @param payload_ptr Pointer to CDI payload data.
 * @param payload_size Size of CDI payload in bytes.
 * @param timestamp CDI timestamp of the audio frame.
 * @param config_ptr Pointer to AVM CDI video configuration structure.
 *
 * @return true if successful, other false.
 */
static bool CdiRgbToObsVideoFrame(cdi_source* cdi_ptr, uint8_t* payload_ptr, int payload_size, CdiAvmVideoConfig* config_ptr)
{
    bool ret = true;

    // TODO: Add logic here.
    blog(LOG_WARNING, "RGB CDI source not supported.");
    ret = false;

    return ret;
}

/**
 * @brief Convert a CDI video frame to OBS.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 * @param payload_ptr Pointer to CDI payload data.
 * @param payload_size Size of CDI payload in bytes.
 * @param timestamp CDI timestamp of the audio frame.
 * @param config_ptr Pointer to AVM CDI video configuration structure.
 */
static void ProcessVideoFrame(cdi_source* cdi_ptr, uint8_t* payload_ptr, int payload_size, uint64_t timestamp, CdiAvmVideoConfig* config_ptr)
{
    obs_source_frame* frame_ptr = &cdi_ptr->obs_video_frame;

    frame_ptr->timestamp = timestamp;

    frame_ptr->width = config_ptr->width;
    frame_ptr->height = config_ptr->height;

    video_colorspace colorspace = VIDEO_CS_709;
    if (kCdiAvmVidColorimetryBT601 == config_ptr->colorimetry) {
        colorspace = VIDEO_CS_601;
    }

    video_range_type range = VIDEO_RANGE_FULL;
    if (kCdiAvmVidRangeNarrow == config_ptr->range) {
        range = VIDEO_RANGE_PARTIAL;
    }

    switch (config_ptr->sampling) {
        case kCdiAvmVidYCbCr422:
            if (!Cdi422ToObsVideoFrame(cdi_ptr, payload_ptr, payload_size, config_ptr)) {
                return;
            }
            break;
        case kCdiAvmVidYCbCr444:
            if (!Cdi444ToObsVideoFrame(cdi_ptr, payload_ptr, payload_size, config_ptr)) {
                return;
            }
            break;
        case kCdiAvmVidRGB:
            if (!CdiRgbToObsVideoFrame(cdi_ptr, payload_ptr, payload_size, config_ptr)) {
                return;
            }
            colorspace = VIDEO_CS_SRGB;
            break;
    }

	video_format_get_parameters(colorspace, range, frame_ptr->color_matrix, frame_ptr->color_range_min, frame_ptr->color_range_max);

	obs_source_output_video(cdi_ptr->obs_source, frame_ptr);
}

/**
 * @brief Convert a CDI audio frame to OBS.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 * @param payload_ptr Pointer to CDI payload data.
 * @param payload_size Size of CDI payload in bytes.
 * @param timestamp CDI timestamp of the audio frame.
 * @param config_ptr Pointer to AVM CDI audio configuration structure.
 */
static void ProcessAudioFrame(cdi_source* cdi_ptr, void* payload_ptr, int payload_size, uint64_t timestamp, CdiAvmAudioConfig* config_ptr)
{
	if (!cdi_ptr->config.audio_enabled) {
		return;
	}

    obs_source_audio* frame_ptr = &cdi_ptr->obs_audio_frame;

    if (config_ptr->sample_rate_khz == kCdiAvmAudioSampleRate48kHz) {
        frame_ptr->samples_per_sec = 48000;
    } else if (config_ptr->sample_rate_khz == kCdiAvmAudioSampleRate96kHz) {
        frame_ptr->samples_per_sec = 96000;
    }

    int num_channels = 0;
    frame_ptr->speakers = SPEAKERS_UNKNOWN;
    // Grouping. Maps number of audio channels to audio grouping.
    switch (config_ptr->grouping) {
        case kCdiAvmAudioM: // Mono.
            num_channels = 1;
            frame_ptr->speakers = SPEAKERS_MONO;
        break;
        case kCdiAvmAudioST: // Standard Stereo (left, right).
            num_channels = 2;
            frame_ptr->speakers = SPEAKERS_STEREO;
        break;
        case kCdiAvmAudioSGRP: // One SDI audio group (1, 2, 3, 4).
            num_channels = 4;
            frame_ptr->speakers = SPEAKERS_4POINT0;
        break;
        case kCdiAvmAudio51: // 5.1 Surround (L, R, C, LFE, Ls, Rs).
            num_channels = 6;
            frame_ptr->speakers = SPEAKERS_5POINT1;
        break;
        case kCdiAvmAudio71: // Surround (L, R, C, LFE, Lss, Rss, Lrs, Rrs).
            num_channels = 8;
            frame_ptr->speakers = SPEAKERS_7POINT1;
        break;
        case kCdiAvmAudio222: // 22.2 Surround (SMPTE ST 2036-2, Table 1).
            num_channels = 8; // 8 is the maximum number of channels that OBS supports.
            frame_ptr->speakers = SPEAKERS_UNKNOWN;
        break;
    }

    frame_ptr->timestamp = timestamp;
	frame_ptr->format = AUDIO_FORMAT_FLOAT_PLANAR;

    int num_samples_per_channel = payload_size / CDI_BYTES_PER_AUDIO_SAMPLE / num_channels;
    frame_ptr->frames = num_samples_per_channel;

    // Validate CDI audio contains the correct number of 24-bit audio samples.
    assert(payload_size <= num_channels * num_samples_per_channel * CDI_BYTES_PER_AUDIO_SAMPLE);

    int ndi_audio_size = num_channels * num_samples_per_channel * sizeof(float);

    // Validate that the NDI buffer is large enough to hold the NDI float audio samples.
    assert(ndi_audio_size <= sizeof(cdi_ptr->obs_audio_buffer));

    uint8_t* ndi_audio_byte_ptr = cdi_ptr->obs_audio_buffer;
    const uint8_t* cdi_audio_ptr = (uint8_t*)payload_ptr;
    int obs_channel_stride_in_bytes = num_samples_per_channel * sizeof(float);

    // For each channel, insert 24-bit int audio segment in correct spot of temp buffer.
    for (int current_channel = 0; current_channel < num_channels; current_channel++) {
        // Memory location of where to read CDI 24-bit int for this channel.
        const uint8_t* interleaved_src_ptr = cdi_audio_ptr + (current_channel * CDI_BYTES_PER_AUDIO_SAMPLE);

        // Memory location of where to write OBS 32-bit float for this channel.
        float* channel_dest_ptr = (float*)(ndi_audio_byte_ptr + (current_channel * obs_channel_stride_in_bytes));

        frame_ptr->data[current_channel] = (uint8_t*)channel_dest_ptr; // Set pointer the the start of the channel sample data in the OBS audio frame.

        // For each channel sample, convert CDI 24-bit int to OBS 32-bit float and store in OBS audio buffer.
        for (int current_sample = 0; current_sample < num_samples_per_channel; current_sample++) {
            // Get 24-bit Big-Endian CDI sample and convert to 32-bit Little-Endian.
            // Shift the 3 bytes to most significant position.
            signed int scaled_signed_int = (interleaved_src_ptr[0] << 24) | (interleaved_src_ptr[1] << 16) |
                                           (interleaved_src_ptr[2] << 8);
            double scaled_double = (double)scaled_signed_int;
            float sample_float = scaled_double / 0x7fffffff;
            sample_float = max(-1.0, min(1.0, sample_float));
            *channel_dest_ptr = sample_float;

            // Moves NDI audio memory location for next 32-bit float.
            channel_dest_ptr++;

            // Updates memory location of where to read next 24-bit CDI audio for channel.
            // Updates by number of 3 byte channels in between current location and next interleaved location
            // for same channel.
            interleaved_src_ptr += num_channels * CDI_BYTES_PER_AUDIO_SAMPLE;
        }
    }

	obs_source_output_audio(cdi_ptr->obs_source, frame_ptr);
}

/**
 * Handle the CDI Rx AVM callback.
 *
 * @param cb_data_ptr Pointer to Rx AVM callback data.
 */
static void TestAvmRxCallback(const CdiAvmRxCbData* cb_data_ptr)
{
    cdi_source* cdi_ptr = (cdi_source*)cb_data_ptr->core_cb_data.user_cb_param;

    if (kCdiStatusOk != cb_data_ptr->core_cb_data.status_code) {
        blog(LOG_ERROR, "Receive payload failed[%s].", CdiCoreStatusToString(cb_data_ptr->core_cb_data.status_code));
    }
    else {
        CdiOsAtomicInc32(&cdi_ptr->con_info.payload_received_count);

        CdiAvmBaselineConfig baseline_config;
        if (NULL != cb_data_ptr->config_ptr) {
            // Attempt to convert the generic configuration structure to a baseline profile configuration structure.
            CdiReturnStatus rc = CdiAvmParseBaselineConfiguration(cb_data_ptr->config_ptr, &baseline_config);
            if (kCdiStatusOk == rc) {
                if (cb_data_ptr->sgl.sgl_head_ptr->next_ptr) {
                    blog(LOG_ERROR, "CDI frame data not in linear format.");
                }
                else {
                    uint64_t timestamp = cb_data_ptr->core_cb_data.core_extra_data.origination_ptp_timestamp.seconds * 1000000000 +
                                         cb_data_ptr->core_cb_data.core_extra_data.origination_ptp_timestamp.nanoseconds;

                    timestamp = timestamp * 100;
                    //timestamp = os_gettime_ns();

                    void* payload_ptr = cb_data_ptr->sgl.sgl_head_ptr->address_ptr;
                    int payload_size = cb_data_ptr->sgl.total_data_size;
                    int stream_identifier = cb_data_ptr->avm_extra_data.stream_identifier;

                    if (kCdiAvmVideo == baseline_config.payload_type) {
                        if (0 != memcmp(&cdi_ptr->video_config, &baseline_config.video_config, sizeof(cdi_ptr->video_config))) {
                            blog(LOG_INFO, "CDI StreamID[%d] Video Payload Size[%d] AVM Data[%s]", stream_identifier,
                                           payload_size, cb_data_ptr->config_ptr->data);
                            memcpy(&cdi_ptr->video_config, &baseline_config.video_config, sizeof(cdi_ptr->video_config));
                        }
                        ProcessVideoFrame(cdi_ptr, (uint8_t*)payload_ptr, payload_size, timestamp, &baseline_config.video_config);
                    }
                    else if (kCdiAvmAudio == baseline_config.payload_type) {
                        if (0 != memcmp(&cdi_ptr->audio_config, &baseline_config.audio_config, sizeof(cdi_ptr->audio_config))) {
                            blog(LOG_INFO, "CDI StreamID[%d] Audio Payload Size[%d] AVM Data[%s]", stream_identifier,
                                           payload_size, cb_data_ptr->config_ptr->data);
                            memcpy(&cdi_ptr->audio_config, &baseline_config.audio_config, sizeof(cdi_ptr->audio_config));
                        }
                        ProcessAudioFrame(cdi_ptr, payload_ptr, payload_size, timestamp, &baseline_config.audio_config);
                    }
                }
            }
            else {
                blog(LOG_ERROR, "Failed to parse baseline configuration [%s].", CdiCoreStatusToString(rc));
            }
        }

        CdiReturnStatus rs = CdiCoreRxFreeBuffer(&cb_data_ptr->sgl);
        if (kCdiStatusOk != rs) {
            blog(LOG_ERROR, "CdiCoreRxFreeBuffer failed[%s].", CdiCoreStatusToString(rs));
        }
    }
}

/**
 * @brief Create a CDI source.
 * 
 * @param cdi_ptr Pointer to CDI source data structure.
 *
 * @return true if successful, other false.
 */
static bool SourceCreate(cdi_source* cdi_ptr)
{
    // Setup default test settings.
    cdi_ptr->con_info.test_settings.protocol_type = kProtocolTypeAvm;
    cdi_ptr->con_info.test_settings.payload_size = 0;

    blog(LOG_INFO, "Initializing source.");

    CdiOsSignalCreate(&cdi_ptr->con_info.connection_state_change_signal);

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 1: Initialize CDI core (must do before initializing adapter or creating connections).
    //-----------------------------------------------------------------------------------------------------------------
    CdiReturnStatus rs = kCdiStatusOk;
    // See logic in obs_module_load();

    //-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 2: Register the EFA adapter.
	//-----------------------------------------------------------------------------------------------------------------
    CdiAdapterHandle adapter_handle = NetworkAdapterInitialize(cdi_ptr->con_info.test_settings.local_adapter_ip_str, nullptr);
    if (nullptr == adapter_handle) {
        rs = kCdiStatusFatal;
    }

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 3. Create a AVM Rx connection.
    //-----------------------------------------------------------------------------------------------------------------
    if (kCdiStatusOk == rs) {
        CdiRxConfigData config_data = { 0 };
        config_data.adapter_handle = adapter_handle;
        config_data.dest_port = cdi_ptr->con_info.test_settings.dest_port;
        config_data.bind_ip_addr_str = cdi_ptr->con_info.test_settings.bind_ip_str;
        config_data.thread_core_num = -1; // -1= Let OS decide which CPU core to use.
        config_data.rx_buffer_type = kCdiLinearBuffer;
        config_data.linear_buffer_size = LINEAR_RX_BUFFER_SIZE;
        config_data.user_cb_param = cdi_ptr;
        config_data.connection_log_method_data_ptr = &log_method_data;
        config_data.connection_cb_ptr = TestConnectionCallback;
        config_data.connection_user_cb_param = cdi_ptr;
        config_data.stats_config.disable_cloudwatch_stats = true;

        rs = CdiAvmRxCreate(&config_data, TestAvmRxCallback, &cdi_ptr->con_info.connection_handle);
    }

    return kCdiStatusOk == rs;
}

/**
 * @brief Called by OBS to destroy a source.
 * 
 * @param data Pointer to CDI source data structure.
 */
void cdi_source_destroy(void* data)
{
    cdi_source* cdi_ptr = (cdi_source*)data;

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 6. Shutdown and clean-up CDI SDK resources.
    //-----------------------------------------------------------------------------------------------------------------
    if (cdi_ptr->con_info.connection_handle) {
        CdiCoreConnectionDestroy(cdi_ptr->con_info.connection_handle);
        cdi_ptr->con_info.connection_handle = nullptr;
    }
    
    NetworkAdapterDestroy();

    // CdiCoreShutdown() is invoked in obs_module_unload();

    // Clean-up additional resources used by this application.
    CdiOsSignalDelete(cdi_ptr->con_info.connection_state_change_signal);

    if (cdi_ptr->conv_buffer) {
        delete cdi_ptr->conv_buffer;
        cdi_ptr->conv_buffer = nullptr;
    }

    delete cdi_ptr; // Allocated using C++ new.
}

/**
 * @brief Called by OBS to get the name of the source.
 * 
 * @return const char* 
 */
const char* cdi_source_getname(void*)
{
	return obs_module_text("CDIPlugin.CDISourceName");
}

/**
 * @brief Called by OBS to create and get the source's properties.
 * 
 * @return Pointer to new properties object.
 */
obs_properties_t* cdi_source_getproperties(void*)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_text(props, PROP_LOCAL_IP, obs_module_text("CDIPlugin.SourceProps.LocalIP"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, PROP_PORT, obs_module_text("CDIPlugin.SourceProps.Port"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, PROP_AUDIO, obs_module_text("CDIPlugin.SourceProps.Audio"));

    obs_properties_add_text(props, "Information", "OBS CDI plugin " OBS_CDI_VERSION "\n"
        "Supported CDI sources are 4:2:2, 8 and 10-bit. Audio supports up to 8 channels.", OBS_TEXT_INFO);

    return props;
}

/**
 * @brief Called by OBS to get the source's default settings.
 * 
 * @param settings Pointer to OBS settings structure.
 */
void cdi_source_getdefaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "cdi_name", "obs-cdi source");
    obs_data_set_default_string(settings, PROP_LOCAL_IP, "127.0.0.1");
    obs_data_set_default_string(settings, PROP_PORT, "5000");
    obs_data_set_default_bool(settings, PROP_AUDIO, true);
}

/**
 * @brief Called by OBS to update a source with setting changes.
 * 
 * @param data Pointer to CDI source data structure.
 * @param settings Pointer to OBS settings.
 */
void cdi_source_update(void *data, obs_data_t *settings)
{
	cdi_source* cdi_ptr = (cdi_source*)data;
	auto obs_source = cdi_ptr->obs_source;
	auto name = obs_source_get_name(obs_source);

    cdi_ptr->con_info.test_settings.local_adapter_ip_str = obs_data_get_string(settings, PROP_LOCAL_IP);
    cdi_ptr->con_info.test_settings.bind_ip_str = obs_data_get_string(settings, PROP_LOCAL_IP); // Use same IP for bind
    cdi_ptr->con_info.test_settings.dest_port = atoi(obs_data_get_string(settings, PROP_PORT));
	cdi_ptr->config.audio_enabled = obs_data_get_bool(settings, PROP_AUDIO);
	obs_source_set_audio_active(obs_source, cdi_ptr->config.audio_enabled);

    obs_source_set_async_unbuffered(obs_source, true);
}

/**
 * @brief Called by OBS to activate a source.
 * 
 * @param data Pointer to CDI source data structure.
 */
void cdi_source_activated(void *data)
{
	cdi_source* cdi_ptr = (cdi_source*)data;
	auto name = obs_source_get_name(cdi_ptr->obs_source);
}

/**
 * @brief Called by OBS to deactivate a source.
 * 
 * @param data Pointer to CDI source data structure.
 */
void cdi_source_deactivated(void *data)
{
	cdi_source* cdi_ptr = (cdi_source*)data;
	auto name = obs_source_get_name(cdi_ptr->obs_source);
}

/**
 * @brief Called by OBS when a source is renamed.
 * 
 * @param data Pointer to CDI source data structure.
 */
void cdi_source_renamed(void *data, calldata_t *)
{
	cdi_source* cdi_ptr = (cdi_source*)data;

	const char* name = obs_source_get_name(cdi_ptr->obs_source);
	cdi_ptr->config.cdi_source_name = QString("OBS-CDI '%1'").arg(name).toUtf8();
}

/**
 * @brief Called by OBS to create a new source.
 * 
 * @param settings Pointer to OBS settings.
 * @param obs_source Pointer to OBS source.
 *
 * @return Pointer to new CDI source data.
 */
void *cdi_source_create(obs_data_t *settings, obs_source_t *obs_source)
{
	const char* name = obs_source_get_name(obs_source);

	cdi_source* cdi_ptr = new cdi_source; // Contains C++ objects, so cannot use bzalloc().
	cdi_ptr->obs_source = obs_source;
    cdi_ptr->config.cdi_source_name = QString("OBS-CDI '%1'").arg(name).toUtf8();

	auto sh = obs_source_get_signal_handler(cdi_ptr->obs_source);
	signal_handler_connect(sh, "rename", cdi_source_renamed, cdi_ptr);

    // Temporary buffer for converting CDI -> OBS.
    cdi_ptr->conv_buffer = new uint8_t[MAX_VIDEO_FRAME_SIZE];

    cdi_source_update(cdi_ptr, settings);

    if (!SourceCreate(cdi_ptr)) {
        if (cdi_ptr->conv_buffer) {
            delete cdi_ptr->conv_buffer;
        }
        delete cdi_ptr;
        cdi_ptr = nullptr;
    }

	return cdi_ptr;
}

/**
 * @brief Create a OBS structure that describes the available functions for this plugin.
 * 
 * @return OBS source structure.
 */
obs_source_info create_cdi_source_info()
{
	obs_source_info cdi_source_info = {};
	cdi_source_info.id = "cdi_source";
	cdi_source_info.type = OBS_SOURCE_TYPE_INPUT;
	cdi_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;

	cdi_source_info.get_name = cdi_source_getname;
	cdi_source_info.get_properties = cdi_source_getproperties;
	cdi_source_info.get_defaults = cdi_source_getdefaults;

	cdi_source_info.create = cdi_source_create;
	cdi_source_info.activate = cdi_source_activated;
	cdi_source_info.update = cdi_source_update;
	cdi_source_info.deactivate = cdi_source_deactivated;
	cdi_source_info.destroy = cdi_source_destroy;

	return cdi_source_info;
}
