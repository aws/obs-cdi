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

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/profiler.h>
#include <util/circlebuf.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "config.h"
#include "obs-frontend-api.h"
#include "config.h"
#include <util/config-file.h>
#include <algorithm>

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

// @brief Default timeout in milliseconds for sending CDI payloads.
#define DEFAULT_TIMEOUT                (20000)

/**
 * @brief A structure that holds all the settings.
 */
struct TestSettings {
    const char* local_adapter_ip_str;  ///< The local network adapter IP address.
    int dest_port;                     ///< The destination port number.
    const char* remote_adapter_ip_str; ///< The remote network adapter IP address.
    int rate_numerator;                ///< The numerator for the number of payloads per second to send.
    int rate_denominator;              ///< The denominator for the number of payloads per second to send.
    int tx_timeout;                    ///< The transmit timeout in microseconds for a Tx payload.

    int video_stream_id;               ///< CDI video stream identifier.
    int audio_stream_id;               ///< CDI audio stream identifier.
    CdiAvmVideoSampling video_sampling; ///< Video sampling.
    CdiAvmVideoBitDepth bit_depth;     ///< Video frame bit depth.
};

/**
 * @brief A structure for storing all info related to a specific connection, including test settings, connection
 * configuration data from the SDK, and state information for the test connection.
 */
struct TestConnectionInfo {
    CdiConnectionHandle connection_handle; ///< The connection handle returned by CdiRawTxCreate().

    TestSettings test_settings{0};         ///< Test settings data structure provided by the user.

    volatile bool payload_error;           ///< true if Tx callback got a payload error.

    CdiSignalType connection_state_change_signal;   ///< Signal used for connection state changes.
    volatile CdiConnectionStatus connection_status; ///< Current status of the connection.

    CdiPoolHandle tx_user_data_pool_handle ; ///< Handle of memory pool used to hold Tx user data.

    uint64_t payload_start_time;           ///< Payload start time, used by Tx Callback functions.
    int rate_period_microseconds;          ///< Calculated Tx rate period.

    /// @brief Number of times payload callback function has been invoked. NOTE: This variable is used by multiple
    /// threads and not declared volatile. It is used as an example of how to use the CdiOsAtomic...() API functions.
    uint32_t payload_cb_count;
};

// This is the structure that holds things about our audio and video including the buffers we will later use
// for manipulation of the data.
struct cdi_output
{
    obs_output_t* output;
    const char* cdi_name;
    bool uses_video;
    bool uses_audio;
    bool started;
    uint32_t frame_width;
    uint32_t frame_height;
    const char* frame_type;
    double video_framerate;
    size_t audio_channels;
    uint32_t audio_samplerate;
    uint8_t* conv_buffer;
    uint32_t conv_linesize;
    uint8_t* audio_conv_buffer;
    size_t audio_conv_buffer_size;
    os_performance_token_t* perf_token;

    TestConnectionInfo con_info{0};
    CdiAvmConfig avm_video_config{0};
    CdiAvmConfig avm_audio_config{0};
};

/**
 * @brief Structure used to hold CDI payload data for a single frame.
 */
struct TestTxUserData {
    cdi_output* cdi_ptr; // Pointer to CDI output data.
    CdiSgList sglist; // SGL for the payload
    CdiSglEntry sgl_entry; // Single SGL entry for the payload (linear buffer format).
};

//*********************************************************************************************************************
//******************************************* START STATIC FUNCTIONS ***********************************************
//*********************************************************************************************************************

/**
 * @brief Initialize a pool memory item.
 * 
 * @param context_ptr Pointer to user defined parameter (a pointer to a buffer pointer).
 * @param item_ptr Pointer to the new pool item to initialize.
 *
 * @return Always returns true.
 */
static bool InitPoolItem(const void* context_ptr, void* item_ptr)
{
    uint8_t** buffer_ptr = (uint8_t**)context_ptr;
    TestTxUserData* user_ptr = (TestTxUserData*)item_ptr;

    memset(user_ptr, 0, sizeof(*user_ptr));

    // Initialize SG List.
    user_ptr->sglist.sgl_head_ptr = &user_ptr->sgl_entry;
    user_ptr->sglist.sgl_tail_ptr = user_ptr->sglist.sgl_head_ptr;

    // Initialize SGL entry.
    user_ptr->sgl_entry.address_ptr = *buffer_ptr;
    user_ptr->sgl_entry.size_in_bytes = MAX_PAYLOAD_SIZE;

    // Adjust buffer pointer for next item's use.
    *buffer_ptr += MAX_PAYLOAD_SIZE;

    return true;
}

/**
 * Handle the connection callback.
 *
 * @param cb_data_ptr Pointer to CdiCoreConnectionCbData callback data.
 */
static void TestConnectionCallback(const CdiCoreConnectionCbData* cb_data_ptr)
{
    cdi_output* cdi_ptr = (cdi_output*)cb_data_ptr->connection_user_cb_param;

    // Update connection state and set state change signal.
    cdi_ptr->con_info.connection_status = cb_data_ptr->status_code;
    CdiOsSignalSet(cdi_ptr->con_info.connection_state_change_signal);
}

/**
 * Handle the Tx AVM callback. NOTE: Only used by the AVM API functions.
 *
 * @param cb_data_ptr Pointer to Tx AVM callback data.
 */
static void TestAvmTxCallback(const CdiAvmTxCbData* cb_data_ptr)
{
    TestTxUserData* user_data_ptr = (TestTxUserData*)cb_data_ptr->core_cb_data.user_cb_param;

    int count = CdiOsAtomicInc32(&user_data_ptr->cdi_ptr->con_info.payload_cb_count);

    if (kCdiStatusOk != cb_data_ptr->core_cb_data.status_code) {
        blog(LOG_ERROR, "Send payload failed[%s].",	CdiCoreStatusToString(cb_data_ptr->core_cb_data.status_code));
    }
    else {
        uint64_t timeout_time = user_data_ptr->cdi_ptr->con_info.payload_start_time + user_data_ptr->cdi_ptr->con_info.test_settings.tx_timeout;
        uint64_t current_time = CdiOsGetMicroseconds();
        if (current_time > timeout_time) {
            blog(LOG_WARNING, "Payload [%d] late by [%llu]microseconds.", count, current_time - timeout_time);
        }
    }

    // Set next payload's expected start time.
    user_data_ptr->cdi_ptr->con_info.payload_start_time += user_data_ptr->cdi_ptr->con_info.rate_period_microseconds;

    // Return user data to memory pool.
    CdiPoolPut(user_data_ptr->cdi_ptr->con_info.tx_user_data_pool_handle, user_data_ptr);
}

/**
 * Creates the CDI video configuration structure to use when sending AVM video payloads.
 *
 * @param connection_info_ptr Pointer to a structure containing user settings needed for the configuration.
 * @param avm_config_ptr Address of where to write the generated generic configuration structure.
 * @param payload_unit_size_ptr Pointer to the location into which the payload unit size is to be written. This value
 *                              needs to be set in payload_config_ptr->core_config_data.unit_size for calls to
 *                              CdiAvmTxPayload().
 * @param video_info Pointer to OBS video output information.
 *
 * @return CdiReturnStatus kCdiStatusOk if the configuration structure was created successfully, kCdiStatusFatal if not.
 */
static CdiReturnStatus MakeVideoConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
                                       int* payload_unit_size_ptr, const video_output_info* video_info)
{
    //OBS and CDI support 601 or 709. If you set something else in OBS, this will default to 709 anyway.
    CdiAvmColorimetry Colorimetry;
    if (video_info->colorspace == VIDEO_CS_601) {
        Colorimetry = kCdiAvmVidColorimetryBT601;
    } else {
        Colorimetry = kCdiAvmVidColorimetryBT709;
    }

    //Similarly, we default the range to full unless the user specifies partial
    CdiAvmVideoRange Range;
    if (video_info->range == VIDEO_RANGE_PARTIAL) {
        Range = kCdiAvmVidRangeNarrow;
    } else {
        Range = kCdiAvmVidRangeFull;
    }

    CdiAvmBaselineConfig baseline_config{};
    baseline_config.payload_type = kCdiAvmVideo;
    baseline_config.video_config.version.major = 01; // Using baseline profile V01.00
    baseline_config.video_config.version.minor = 00;  
    baseline_config.video_config.width = (uint16_t)video_info->width;
    baseline_config.video_config.height = (uint16_t)video_info->height;
    baseline_config.video_config.sampling = connection_info_ptr->test_settings.video_sampling;
    baseline_config.video_config.alpha_channel = kCdiAvmAlphaUnused; // No alpha channel
    baseline_config.video_config.depth = connection_info_ptr->test_settings.bit_depth;
    baseline_config.video_config.frame_rate_num = (uint32_t)connection_info_ptr->test_settings.rate_numerator;
    baseline_config.video_config.frame_rate_den = (uint32_t)connection_info_ptr->test_settings.rate_denominator;
    baseline_config.video_config.colorimetry = Colorimetry;
    baseline_config.video_config.tcs = kCdiAvmVidTcsSDR; // Standard Dynamic Range video stream.
    baseline_config.video_config.range = Range;
    baseline_config.video_config.par_width = 1;
    baseline_config.video_config.par_height = 1;

    return CdiAvmMakeBaselineConfiguration(&baseline_config, avm_config_ptr, payload_unit_size_ptr);
}

/**
 * Creates the CDI audio configuration structure to use when sending AVM audio payloads.
 *
 * @param connection_info_ptr Pointer to a structure containing user settings needed for the configuration.
 * @param avm_config_ptr Address of where to write the generated generic configuration structure.
 * @param payload_unit_size_ptr Pointer to the location into which the payload unit size is to be written. This value
 *                              needs to be set in payload_config_ptr->core_config_data.unit_size for calls to
 *                              CdiAvmTxPayload().
 *
 * @return CdiReturnStatus kCdiStatusOk if the configuration structure was created successfully, kCdiStatusFatal if not.
 */
static CdiReturnStatus MakeAudioConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
                                       int* payload_unit_size_ptr)
{
    CdiAvmBaselineConfig audio_config;
    audio_config.payload_type = kCdiAvmAudio;
    audio_config.audio_config.version = { 01, 00 }; //CDI version
    audio_config.audio_config.grouping = kCdiAvmAudioST; // Force stereo for now.
    audio_config.audio_config.sample_rate_khz = kCdiAvmAudioSampleRate48kHz; //48k

    // English.
    audio_config.audio_config.language[0] = 'e';
    audio_config.audio_config.language[1] = 'n';
    audio_config.audio_config.language[2] = 'g';

    return CdiAvmMakeBaselineConfiguration(&audio_config, avm_config_ptr, payload_unit_size_ptr);
}

/**
 * Send a payload using an AVM API function.
 *
 * @param user_data_ptr Pointer to user data.
 * @param timestamp_ptr Pointer to timestamp.
 * @param avm_config_ptr Pointer to the generic configuration structure to use for the stream.
 * @param stream_identifier CDI stream identifier.
 *
 * @return true if successfully queued payload to be sent.
 */
static bool SendAvmPayload(TestTxUserData* user_data_ptr, CdiPtpTimestamp* timestamp_ptr, CdiAvmConfig* avm_config_ptr,
                           int stream_identifier)
 {
    CdiReturnStatus rs = kCdiStatusOk;

    CdiAvmTxPayloadConfig payload_config = {
        *timestamp_ptr, // origination_ptp_timestamp
        0, // payload_user_data
        user_data_ptr, // user_cb_param
        0, // unit_size
        (uint16_t)stream_identifier // stream_identifier
    };

    // Send the payload, retrying if the queue is full.
    do {
        rs = CdiAvmTxPayload(user_data_ptr->cdi_ptr->con_info.connection_handle, &payload_config, avm_config_ptr,
                             &user_data_ptr->sglist, user_data_ptr->cdi_ptr->con_info.test_settings.tx_timeout);
    } while (kCdiStatusQueueFull == rs);

    return kCdiStatusOk == rs;
}

/**
 * @brief Convert three plane YUV to single plane YCbCr.
 * 
 * @param YUV Array of pointers to 3 planes for source YUV data.
 * @param in_linesize Linesize of YUV source data in bytes.
 * @param start_y Starting line.
 * @param end_y Ending line.
 * @param output Pointer where to write the converted YCbCr data.
 * @param out_linesize Linesize of output YCbCr data in bytes.
 */
static void i444_to_ycbcr(uint8_t* YUV[], uint32_t in_linesize[], uint32_t start_y, uint32_t end_y, uint8_t* output,
                          uint32_t out_linesize)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    uint32_t width = in_linesize[0] < out_linesize ? in_linesize[0] : out_linesize;
    for (uint32_t y = start_y; y < end_y; ++y) {
        Y = YUV[0] + ((size_t)y * (size_t)in_linesize[0]);
        U = YUV[1] + ((size_t)y * (size_t)in_linesize[1]);
        V = YUV[2] + ((size_t)y * (size_t)in_linesize[2]);

        out = output + ((size_t)y * (size_t)out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            *(out++) = *(U++); U++;
            *(out++) = *(Y++);
            *(out++) = *(V++); V++;
            *(out++) = *(Y++);
        }
    }
}

/**
 * @brief Called by OBS to get the name of the output from the configuration.
 * 
 * @param data Pointer to user data.
 *
 * @return Pointer to output name string.
 */
const char* cdi_output_getname(void*)
{
    return obs_module_text("CDIPlugin.OutputName");
}

/**
 * @brief Called by OBS to create and get the output's properties.
 * 
 * @return Pointer to new properties object.
 */
obs_properties_t* cdi_output_getproperties(void*)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
    obs_properties_add_text(props, "cdi_name", obs_module_text("CDIPlugin.OutputProps.CDIName"), OBS_TEXT_DEFAULT);

    return props;
}

/**
 * @brief Called by OBS to get the output's default settings.
 * 
 * @param settings Pointer to OBS settings structure.
 */
void cdi_output_getdefaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "cdi_name", "obs-cdi output");
    obs_data_set_default_bool(settings, "uses_video", true);
    obs_data_set_default_bool(settings, "uses_audio", true);
}

/**
 * @brief Called by OBS to start the CDI output.
 * 
 * @param data Pointer to user data.
 *
 * @return true if successfully started, otherwise false is returned.
 */
bool cdi_output_start(void* data)
{
    cdi_output* cdi_ptr = (cdi_output*)data;

    uint32_t flags = 0;
    video_t* video = obs_output_video(cdi_ptr->output);
    audio_t* audio = obs_output_audio(cdi_ptr->output);

    // Check to make sure there is audio or video.
    if (!video && !audio) {
        blog(LOG_ERROR, "'%s': no video and audio available", cdi_ptr->cdi_name);
        return false;
    }
    
    // Get some information about it.
    if (cdi_ptr->uses_video && video) {
        video_format format = video_output_get_format(video);
        uint32_t width = video_output_get_width(video);
        uint32_t height = video_output_get_height(video);

        blog(LOG_INFO, "Video Format[%d] Width[%d] Height[%d]", format, width, height);
        switch (format) {
            // Right now, this plugin only supports OBS I444 to YCbCr conversion. This switch can be
            // extended for other pixel formats.  But for now, if you pick something other than I444, it will stop here.
            case VIDEO_FORMAT_I444:
                cdi_ptr->conv_linesize = width * 2;
                cdi_ptr->conv_buffer = new uint8_t[(size_t)height * (size_t)cdi_ptr->conv_linesize * 2]();
                break;
            default:
                blog(LOG_ERROR, "pixel format [%d] is not supported yet", format);
                return false;
        }

        cdi_ptr->frame_width = width;
        cdi_ptr->frame_height = height;
        cdi_ptr->video_framerate = video_output_get_frame_rate(video);
        flags |= OBS_OUTPUT_VIDEO;
    }

    if (cdi_ptr->uses_audio && audio) {
        cdi_ptr->audio_samplerate = audio_output_get_sample_rate(audio);
        cdi_ptr->audio_channels = audio_output_get_channels(audio);
        flags |= OBS_OUTPUT_AUDIO;
    }

    // Get the output settings from the OBS configuration.
    config_t* obs_config = obs_frontend_get_global_config();
    const video_output_info* video_info = video_output_get_info(video);

    // Use those settings to populate our con_info.
    cdi_ptr->con_info.test_settings.local_adapter_ip_str = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_IP);
    cdi_ptr->con_info.test_settings.dest_port = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT);
    cdi_ptr->con_info.test_settings.remote_adapter_ip_str = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST);
    cdi_ptr->con_info.test_settings.rate_numerator = video_info->fps_num;
    cdi_ptr->con_info.test_settings.rate_denominator = video_info->fps_den;
    cdi_ptr->con_info.test_settings.tx_timeout = DEFAULT_TIMEOUT;

    cdi_ptr->con_info.test_settings.video_stream_id = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID);
    cdi_ptr->con_info.test_settings.audio_stream_id = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID);
    cdi_ptr->con_info.test_settings.video_sampling = (CdiAvmVideoSampling)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_SAMPLING);
    cdi_ptr->con_info.test_settings.bit_depth = (CdiAvmVideoBitDepth)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH);

    int unit_size = 0; // Not used.
    // Fill in the AVM configuration structure and payload unit size for both video and audio.
    MakeVideoConfig(&cdi_ptr->con_info, &cdi_ptr->avm_video_config, &unit_size, video_info);
    MakeAudioConfig(&cdi_ptr->con_info, &cdi_ptr->avm_audio_config, &unit_size);

    CdiOsSignalCreate(&cdi_ptr->con_info.connection_state_change_signal);

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 1: Initialize CDI core (must do before initializing adapter or creating connections).
    //-----------------------------------------------------------------------------------------------------------------
    CdiReturnStatus rs = kCdiStatusOk;
    // See logic in obs_module_load();

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 2: Register the EFA adapter.
    //-----------------------------------------------------------------------------------------------------------------
    CdiAdapterHandle adapter_handle;
    if (kCdiStatusOk == rs) {
        // Initialize the single instance of the adapter, if needed.
        void* ret_tx_buffer_ptr;
        adapter_handle = NetworkAdapterInitialize(cdi_ptr->con_info.test_settings.local_adapter_ip_str, &ret_tx_buffer_ptr);
        if (nullptr == adapter_handle) {
            rs = kCdiStatusFatal;
        } else {
            uint8_t* tx_buffer_ptr = (uint8_t*)ret_tx_buffer_ptr;
            if (!CdiPoolCreateAndInitItems("TestTxUserData Pool", MAX_NUMBER_OF_TX_PAYLOADS, 0, 0, sizeof(TestTxUserData),
                false, // false= Not thread-safe (don't use OS resource locks)
                &cdi_ptr->con_info.tx_user_data_pool_handle,
                InitPoolItem,
                &tx_buffer_ptr)) {
                rs = kCdiStatusNotEnoughMemory;
            }
        }
    }

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 3: Create AVM Tx connection.
    //-----------------------------------------------------------------------------------------------------------------
    if (kCdiStatusOk == rs) {
        CdiTxConfigData config_data = { 0 };
        config_data.adapter_handle = adapter_handle;
        config_data.dest_ip_addr_str = cdi_ptr->con_info.test_settings.remote_adapter_ip_str;
        config_data.dest_port = cdi_ptr->con_info.test_settings.dest_port;
        config_data.thread_core_num = -1; // -1= Let OS decide which CPU core to use.
        config_data.connection_log_method_data_ptr = &log_method_data;
        config_data.connection_cb_ptr = TestConnectionCallback;
        config_data.connection_user_cb_param = cdi_ptr;
        config_data.stats_config.disable_cloudwatch_stats = true;
        
        blog(LOG_INFO, "Creating AVM Tx connection.");
        blog(LOG_INFO, "Local IP: [%s]", cdi_ptr->con_info.test_settings.local_adapter_ip_str);
        blog(LOG_INFO, "Remote: [%s:%d]", config_data.dest_ip_addr_str, config_data.dest_port);

        rs = CdiAvmTxCreate(&config_data, TestAvmTxCallback, &cdi_ptr->con_info.connection_handle);
    }

    if (kCdiStatusOk == rs) {
        blog(LOG_INFO, "CdiAvmTxCreate() succeeded.");
    }

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 5: Can now send the desired number of payloads. Will send at the specified rate. If we get any
    // errors or the connection drops, then exit the loop.
    //----------------------------------------------------------------------------------------------------------------

    // Setup rate period and start times.I don't really think this is needed in this particular case because
    // OBS paces the frame calls. But We keep it here as an example. 
    cdi_ptr->con_info.rate_period_microseconds = ((1000000 * cdi_ptr->con_info.test_settings.rate_denominator) /
        cdi_ptr->con_info.test_settings.rate_numerator);
    cdi_ptr->con_info.payload_start_time = CdiOsGetMicroseconds();
    uint64_t rate_next_start_time = cdi_ptr->con_info.payload_start_time + cdi_ptr->con_info.rate_period_microseconds;

    //Tell OBS we've started and to start capturing the video and audio frames with the flags we set earlier. 
    cdi_ptr->started = obs_output_begin_data_capture(cdi_ptr->output, flags);

    return cdi_ptr->started;
}

/**
 * @brief Called by OBS to stop the CDI output if someone decides to stop or exit.
 * 
 * @param data Pointer to user data.
 * @param ts The timestamp to stop. If 0, the output should attempt to stop immediately rather than wait for any more
 *           data to process.
 */
void cdi_output_stop(void* data, uint64_t ts)
{
    cdi_output* cdi_ptr = (cdi_output*)data;

    cdi_ptr->started = false;
    obs_output_end_data_capture(cdi_ptr->output);

    os_end_high_performance(cdi_ptr->perf_token);
    cdi_ptr->perf_token = NULL;

    if (cdi_ptr->conv_buffer) {
        delete cdi_ptr->conv_buffer;
    }

    cdi_ptr->frame_width = 0;
    cdi_ptr->frame_height = 0;
    cdi_ptr->video_framerate = 0.0;

    cdi_ptr->audio_channels = 0;
    cdi_ptr->audio_samplerate = 0;

    //-----------------------------------------------------------------------------------------------------------------
    // CDI SDK Step 6. Shutdown and clean-up CDI SDK resources.
    //-----------------------------------------------------------------------------------------------------------------
    if (&cdi_ptr->con_info.connection_handle) {
        CdiCoreConnectionDestroy(cdi_ptr->con_info.connection_handle);
        cdi_ptr->con_info.connection_handle = nullptr;
    }

    NetworkAdapterDestroy();

    // CdiCoreShutdown() is invoked in obs_module_unload();

    if (cdi_ptr->con_info.tx_user_data_pool_handle) {
        CdiPoolPutAll(cdi_ptr->con_info.tx_user_data_pool_handle);
        CdiPoolDestroy(cdi_ptr->con_info.tx_user_data_pool_handle);
        cdi_ptr->con_info.tx_user_data_pool_handle = nullptr;
    }
    // Clean-up additional resources used by this application.
    if (cdi_ptr->con_info.connection_state_change_signal) {
        CdiOsSignalDelete(cdi_ptr->con_info.connection_state_change_signal);
        cdi_ptr->con_info.connection_state_change_signal = nullptr;
    }
}

/**
 * @brief Called by OBS to update the output  with setting changes.
 * 
 * @param data Pointer to CDI output data structure.
 * @param settings Pointer to OBS settings.
 */
void cdi_output_update(void* data, obs_data_t* settings)
{
    cdi_output* cdi_ptr = (cdi_output*)data;

    cdi_ptr->cdi_name = obs_data_get_string(settings, "cdi_name");
    cdi_ptr->uses_video = obs_data_get_bool(settings, "uses_video");
    cdi_ptr->uses_audio = obs_data_get_bool(settings, "uses_audio");
}

/**
 * @brief Called by OBS to create the CDI output when CDI output is enabled at startup or
 *        if enabled by changing the configuration.
 * 
 * @param settings Pointer to OBS settings.
 * @param output Pointer to OBS output.
 *
 * @return Pointer to new CDI output data.
 */
void* cdi_output_create(obs_data_t* settings, obs_output_t* output)
{
    cdi_output* cdi_ptr = new cdi_output; // May contain C++ objects, so cannot use bzalloc().

    cdi_ptr->output = output;
    cdi_ptr->started = false;
    cdi_ptr->audio_conv_buffer = nullptr;
    cdi_ptr->audio_conv_buffer_size = 0;
    cdi_ptr->perf_token = NULL;

    cdi_output_update(cdi_ptr, settings);

    return cdi_ptr;
}

/**
 * @brief Destroys the CDI structure and frees up the memory.
 * 
 * @param data Pointer to CDI output data.
 */
void cdi_output_destroy(void* data)
{
    cdi_output* cdi_ptr = (cdi_output*)data;
    if (cdi_ptr->audio_conv_buffer) {
        bfree(cdi_ptr->audio_conv_buffer);
    }
    delete cdi_ptr; // Allocated using C++ new.
}

/**
 * @brief Convert an OBS video frame to CDI 4:2:2.
 * 
 * @param user_data_ptr Pointer to user data related to the frame to convert.
 * @param frame Pointer to OBS video frame data.
 *
 * @return true if successful, otherwise false is returned.
 */
static bool ObsToCdi422VideoFrame(TestTxUserData* user_data_ptr, struct video_data* frame)
{
    bool ret = true;
    cdi_output* cdi_ptr = user_data_ptr->cdi_ptr;

    video_t* video = obs_output_video(cdi_ptr->output);
    video_format format = video_output_get_format(video);
    if (VIDEO_FORMAT_I444 != format) {
        blog(LOG_ERROR, "OBS Video format must be I444.");
    }

    uint32_t width = cdi_ptr->frame_width;
    uint32_t height = cdi_ptr->frame_height;

    // Convert i444 to YCbCr.
    i444_to_ycbcr(frame->data, frame->linesize, 0, height, cdi_ptr->conv_buffer, cdi_ptr->conv_linesize);

    if (kCdiAvmVidBitDepth8 == cdi_ptr->con_info.test_settings.bit_depth) {
        // Nothing needed.
    }
    else if (kCdiAvmVidBitDepth10 == cdi_ptr->con_info.test_settings.bit_depth) {
        // Loop to convert 8 bit to 10 bit. This is needed because lots of professional video software only expects 10 bit.
        // The process to convert 8 to 10 bit is simple.
        uint8_t* txptr = (uint8_t*)user_data_ptr->sglist.sgl_head_ptr->address_ptr;
        uint8_t* srcptr = cdi_ptr->conv_buffer;

        int payload_size_8bit = height * width * 2; // 8-bit 4:2:2
        int payload_size_10bit = height * width * 2.5; // 10-bit 4:2:2

        user_data_ptr->sglist.total_data_size = payload_size_10bit;
        user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = payload_size_10bit;

        //for every pgroup (a pgroup being 2 pixels in this case because we use the U and V elements accross 2 Y values.)
        uint32_t pgroup;
        for (int i = 0; i < payload_size_8bit; i += 4) {
            // The first byte in 8 bit vs 10 bit is the same so we just set the value of the first byte at the memory address
            // of txptr to the value at srcptr 
            *(txptr + 0) = *srcptr;

            // For the rest of the bytes, shift the pixels in (shift 3 bytes into a temp uint32).
            pgroup = *(srcptr + 1) << 22 | *(srcptr + 2) << 12 | *(srcptr + 3) << 2;

            // Shift the pixels out (shift each byte out into 4 new bytes and put them at incrementing memory addresses).
            *(txptr + 1) = pgroup >> 24;
            *(txptr + 2) = pgroup >> 16;
            *(txptr + 3) = pgroup >> 8;
            *(txptr + 4) = pgroup >> 0;
            txptr += 5;
            srcptr += 4;
        }
    }
    else if (kCdiAvmVidBitDepth12 == cdi_ptr->con_info.test_settings.bit_depth) {
        blog(LOG_ERROR, "12-bit not implemented yet.");
        ret = false;
    }

    return ret;
}

/**
 * @brief Convert an OBS video frame to CDI 4:4:4.
 * 
 * @param user_data_ptr Pointer to user data related to the frame to convert.
 * @param frame Pointer to OBS video frame data.
 *
 * @return true if successful, otherwise false is returned.
 */
static bool ObsToCdi444VideoFrame(TestTxUserData* user_data_ptr, struct video_data* frame)
{
    bool ret = true;

    // TODO: Add logic here.
    blog(LOG_ERROR, "YCbCr 4:4:4 not implemented yet.");
    ret = false;

    return ret;
}

/**
 * @brief Convert an OBS video frame to CDI RGB.
 * 
 * @param user_data_ptr Pointer to user data related to the frame to convert.
 * @param frame Pointer to OBS video frame data.
 *
 * @return true if successful, otherwise false is returned.
 */
static bool ObsToCdiRgbVideoFrame(TestTxUserData* user_data_ptr, struct video_data* frame)
{
    bool ret = true;

    // TODO: Add logic here.
    blog(LOG_ERROR, "RGB not implemented yet.");
    ret = false;

    return ret;
}

/**
 * @brief Called by OBS to output a video frame.
 * 
 * @param data Pointer to CDI output data structure.
 * @param frame Pointer to OBS video frame structure.
 */
void cdi_output_rawvideo(void* data, struct video_data* frame)
{
    cdi_output* cdi_ptr = (cdi_output*)data;

    if (kCdiConnectionStatusConnected != cdi_ptr->con_info.connection_status) {
        return; // Not connected, so cannot output the frame.
    }

    if (!cdi_ptr->started || !cdi_ptr->frame_width || !cdi_ptr->frame_height)
        return;

    TestTxUserData* user_data_ptr = NULL;
    if (!CdiPoolGet(cdi_ptr->con_info.tx_user_data_pool_handle, (void**)&user_data_ptr)) {
        blog(LOG_ERROR, "Failed to get user data buffer from memory pool.");
        return;
    }
    user_data_ptr->cdi_ptr = cdi_ptr;

    bool send_frame = false;
    if (kCdiAvmVidYCbCr422 == cdi_ptr->con_info.test_settings.video_sampling) {
        send_frame = ObsToCdi422VideoFrame(user_data_ptr, frame);
    }
    else if (kCdiAvmVidYCbCr444 == cdi_ptr->con_info.test_settings.video_sampling) {
        send_frame = ObsToCdi444VideoFrame(user_data_ptr , frame);
    }
    else if (kCdiAvmVidRGB == cdi_ptr->con_info.test_settings.video_sampling) {
        send_frame = ObsToCdiRgbVideoFrame(user_data_ptr, frame);
    }

    if (send_frame) {
        CdiPtpTimestamp timestamp;

        //make a properly paced timestamp
        timestamp.seconds = floor(frame->timestamp / 1000000000);
        timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

        // Send the payload.
        if (!SendAvmPayload(user_data_ptr, &timestamp, &cdi_ptr->avm_video_config, cdi_ptr->con_info.test_settings.video_stream_id)) {
            send_frame = false;
        }
    }

    if (!send_frame) {
        // Frame was not sent, so return the user data to memory pool.
        CdiPoolPut(cdi_ptr->con_info.tx_user_data_pool_handle, user_data_ptr);
    }
}

/**
 * @brief Called by OBS to output a audio frame.
 * 
 * @param data Pointer to CDI output data structure.
 * @param frame Pointer to OBS video frame structure.
 */
void cdi_output_rawaudio(void* data, struct audio_data* frame)
{
    cdi_output* cdi_ptr = (cdi_output*)data;

    if (kCdiConnectionStatusConnected != cdi_ptr->con_info.connection_status) {
        return; // Not connected, so cannot output the frame.
    }

    if (!cdi_ptr->started || !cdi_ptr->audio_samplerate || !cdi_ptr->audio_channels)
        return;

    TestTxUserData* user_data_ptr = NULL;
    if (!CdiPoolGet(cdi_ptr->con_info.tx_user_data_pool_handle, (void**)&user_data_ptr)) {
        blog(LOG_ERROR, "Failed to get user data buffer from memory pool.");
        return;
    }
    user_data_ptr->cdi_ptr = cdi_ptr;

    // Only 2 channel stereo supported here.
    int no_channels = 2;
    int no_samples = frame->frames;
    int samplebytes = frame->frames * 3; // Each audio sample in CDI uses 3 bytes (24-bit PCM).

    const int data_size = no_channels * samplebytes;

    if (data_size > cdi_ptr->audio_conv_buffer_size) {
        if (cdi_ptr->audio_conv_buffer) {
            bfree(cdi_ptr->audio_conv_buffer);
        }
        cdi_ptr->audio_conv_buffer = (uint8_t*)bmalloc(data_size);
        cdi_ptr->audio_conv_buffer_size = data_size;
    }

    signed int tempa;
    double scaleda;
    float samplea;
    float* pa;
    signed int tempb;
    double scaledb;
    float sampleb;
    float* pb;

    // OBS uses 32-bit float audio. CDI wants 24 bit PCM. So we convert the 32 bit float to 24 bit PCM.
    for (int i = 0; i < no_samples; i++) {
        //left channel
        memcpy(&samplea, &frame->data[0][i * 4], 4);
        pa = &samplea;
        scaleda = max(-1.0, min(1.0, *pa)) * 0x7FFFFFFF; //some clipping because of hard limiter
        tempa = signed int(scaleda);
        //right channel
        memcpy(&sampleb, &frame->data[1][i * 4], 4);
        pb = &sampleb;
        scaledb = max(-1.0, min(1.0, *pb)) * 0x7FFFFFFF; //some clipping because of hard limiter
        tempb = signed int(scaledb);

        // The audio in OBS is in separate planes and we need to interleave them to make our single stream of stereo audio.
        // So for 24 bit audio, this means 6 bytes (3 for left ch, 3 for right).
        *(cdi_ptr->audio_conv_buffer + i * 6 + 2) = (unsigned char)(tempa >> 8);
        *(cdi_ptr->audio_conv_buffer + i * 6 + 1) = (unsigned char)(tempa >> 16);
        *(cdi_ptr->audio_conv_buffer + i * 6 + 0) = (unsigned char)(tempa >> 24);
        *(cdi_ptr->audio_conv_buffer + i * 6 + 5) = (unsigned char)(tempb >> 8);
        *(cdi_ptr->audio_conv_buffer + i * 6 + 4) = (unsigned char)(tempb >> 16);
        *(cdi_ptr->audio_conv_buffer + i * 6 + 3) = (unsigned char)(tempb >> 24);
    }

    //Fill TX Buffer
    memcpy(user_data_ptr->sglist.sgl_head_ptr->address_ptr, cdi_ptr->audio_conv_buffer, data_size);

    user_data_ptr->sglist.total_data_size = data_size;
    user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = data_size;

    CdiPtpTimestamp timestamp;
    timestamp.seconds = floor(frame->timestamp / 1000000000);
    timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

    // Send the payload.
    if (!SendAvmPayload(user_data_ptr, &timestamp, &cdi_ptr->avm_audio_config, cdi_ptr->con_info.test_settings.audio_stream_id)) {
        // Error occurred, so return the user data to memory pool.
        CdiPoolPut(user_data_ptr->cdi_ptr->con_info.tx_user_data_pool_handle, user_data_ptr);
    }
}

/**
 * @brief Create a OBS structure that describes the available functions for this plugin.
 * 
 * @return OBS output structure.
 */
struct obs_output_info create_cdi_output_info()
{
    struct obs_output_info cdi_output_info = {};

    cdi_output_info.id = "cdi_output";
    cdi_output_info.flags = OBS_OUTPUT_AV;
    cdi_output_info.get_name = cdi_output_getname;
    cdi_output_info.get_properties = cdi_output_getproperties;
    cdi_output_info.get_defaults = cdi_output_getdefaults;
    cdi_output_info.create = cdi_output_create;
    cdi_output_info.destroy = cdi_output_destroy;
    cdi_output_info.update = cdi_output_update;
    cdi_output_info.start = cdi_output_start;
    cdi_output_info.stop = cdi_output_stop;
    cdi_output_info.raw_video = cdi_output_rawvideo;
    cdi_output_info.raw_audio = cdi_output_rawaudio;

    return cdi_output_info;
}
