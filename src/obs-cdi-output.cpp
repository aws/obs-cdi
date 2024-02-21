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
#include <mutex>

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
    bool alpha_used;                   ///< Alpha used (only for RGB)
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


    /// @brief Number of times payload callback function has been invoked. NOTE: This variable is used by multiple
    /// threads and not declared volatile. It is used as an example of how to use the CdiOsAtomic...() API functions.
    uint32_t payload_cb_count;
};

// This is the structure that holds things about our audio and video including the buffers we will later use
// for manipulation of the data.
struct cdi_output
{
    std::mutex connection_mutex;
    obs_output_t* output;
    const char* cdi_name;
    bool uses_video;
    bool uses_audio;
    bool started;
    uint32_t frame_width;
    uint32_t frame_height;
    const char* frame_type;
    int audio_channels;
    uint32_t audio_samplerate;
    uint8_t* conv_buffer;
    uint32_t conv_linesize;

    TestConnectionInfo con_info{0};
    
    CdiAvmConfig avm_video_config{0};
    int video_unit_size;

    CdiAvmConfig avm_audio_config{0};
    int audio_unit_size;
};

/**
 * @brief Structure used to hold CDI payload data for a single frame.
 */
struct TestTxUserData {
    cdi_output* cdi_ptr; // Pointer to CDI output data.
    CdiSgList sglist; // SGL for the payload
    CdiSglEntry sgl_entry; // Single SGL entry for the payload (linear buffer format).
};

// Output 5 bytes of CDI 10-bit pixel data.
#define CDI_10_BIT_OUT_5_BYTES(OUT, A0, B0, C0, A1) \
        *(OUT++) = (uint8_t)(A0 >> 2);                      /* A0 bits 9-2 */ \
        *(OUT++) = (uint8_t)((A0 << 6 & 0xC0) | (B0 >> 4)); /* A9 bits 1-0 and B0 bits 9-4 */ \
        *(OUT++) = (uint8_t)((B0 << 4) | (C0 >> 6));        /* B0 bits 3-0 and C0 bits 9-6 */ \
        *(OUT++) = (uint8_t)((C0 << 2) | (A1 >> 8));        /* C0 bits 5-0 and A1 bits 9-8 */ \
        *(OUT++) = (uint8_t)(A1 & 0xFF)                     /* A1 bits 7-0 */

// Output 3 bytes of CDI 12-bit pixel data.
#define CDI_12_BIT_OUT_3_BYTES(OUT, A0, B0) \
            *(OUT++) = (uint8_t)(A0 >> 4);               /* A0 bits 11-4 */ \
            *(OUT++) = (uint8_t)((A0 << 4) | (B0 >> 8)); /* A0 bits 3-0 and B0 bits 11-8 */ \
            *(OUT++) = (uint8_t)(B0 & 0xFF)              /* B0 bits 7-0 */

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
 * @param video Pointer to OBS video information.
 *
 * @return CdiReturnStatus kCdiStatusOk if the configuration structure was created successfully, kCdiStatusFatal if not.
 */
static CdiReturnStatus MakeVideoConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
                                       int* payload_unit_size_ptr, const video_t *video)
{
    const video_output_info* video_info = video_output_get_info(video);

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
 * @param Pointer to OBS audio structure.
 *
 * @return CdiReturnStatus kCdiStatusOk if the configuration structure was created successfully, kCdiStatusFatal if not.
 */
static CdiReturnStatus MakeAudioConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
                                       int* payload_unit_size_ptr, audio_t* audio_ptr)
{
    CdiReturnStatus rs = kCdiStatusOk;

    CdiAvmBaselineConfig baselne_config;
    baselne_config.payload_type = kCdiAvmAudio;
    baselne_config.audio_config.version = { 01, 00 }; //CDI version
    baselne_config.audio_config.grouping = kCdiAvmAudioST; // Force stereo for now.
    baselne_config.audio_config.sample_rate_khz = kCdiAvmAudioSampleRate48kHz; //48k

    int no_channels = (int)audio_output_get_channels(audio_ptr);
    // Audio grouping. Maps number of audio channels to audio grouping.
    switch (no_channels) {
        case 1: // Mono.
            baselne_config.audio_config.grouping = kCdiAvmAudioM;
        break;
        case 2: // Standard Stereo (left, right).
            baselne_config.audio_config.grouping = kCdiAvmAudioST;
        break;
        case 4: // One SDI audio group (1, 2, 3, 4).
            baselne_config.audio_config.grouping = kCdiAvmAudioSGRP;
        break;
        case 6: // 5.1 Surround (L, R, C, LFE, Ls, Rs).
            baselne_config.audio_config.grouping = kCdiAvmAudio51;
        break;
        case 8: // Surround (L, R, C, LFE, Lss, Rss, Lrs, Rrs).
            baselne_config.audio_config.grouping = kCdiAvmAudio71;
        break;
        case 24: // 22.2 Surround (SMPTE ST 2036-2, Table 1).
            baselne_config.audio_config.grouping = kCdiAvmAudio222;
        break;
        default: // Number of channels is not supported.
            CDI_LOG_THREAD(kLogInfo, "[%d]channel audio is not supported in CDI.", no_channels);
            rs = kCdiStatusInvalidPayload;
        break;
    }

    if (kCdiStatusOk == rs) {
        // English.
        baselne_config.audio_config.language[0] = 'e';
        baselne_config.audio_config.language[1] = 'n';
        baselne_config.audio_config.language[2] = 'g';

        rs = CdiAvmMakeBaselineConfiguration(&baselne_config, avm_config_ptr, payload_unit_size_ptr);
    }

    return rs;
}

/**
 * Send a payload using an AVM API function.
 *
 * @param user_data_ptr Pointer to user data.
 * @param timestamp_ptr Pointer to timestamp.
 * @param avm_config_ptr Pointer to the generic configuration structure to use for the stream.
 * @param unit_size Size of units in bits to ensure a single unit is not split across sgl.
 * @param stream_identifier CDI stream identifier.
 *
 * @return true if successfully queued payload to be sent.
 */
static bool SendAvmPayload(TestTxUserData* user_data_ptr, CdiPtpTimestamp* timestamp_ptr, CdiAvmConfig* avm_config_ptr,
                           int unit_size, int stream_identifier)
 {
    CdiReturnStatus rs = kCdiStatusOk;

    CdiAvmTxPayloadConfig payload_config = { 0 };
    payload_config.core_config_data.core_extra_data.origination_ptp_timestamp = *timestamp_ptr;
    payload_config.core_config_data.user_cb_param = user_data_ptr;
    payload_config.core_config_data.unit_size = unit_size;
    payload_config.avm_extra_data.stream_identifier = stream_identifier;

    // Send the payload, retrying if the queue is full.
    do {
        rs = CdiAvmTxPayload(user_data_ptr->cdi_ptr->con_info.connection_handle, &payload_config, avm_config_ptr,
                             &user_data_ptr->sglist, user_data_ptr->cdi_ptr->con_info.test_settings.tx_timeout);
    } while (kCdiStatusQueueFull == rs);

    return kCdiStatusOk == rs;
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

    // Get the output settings from the OBS configuration.
    config_t* obs_config = obs_frontend_get_global_config();
    const video_output_info* video_info = nullptr;

    // Use those settings to populate our con_info.
    cdi_ptr->con_info.test_settings.local_adapter_ip_str = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_IP);
    cdi_ptr->con_info.test_settings.dest_port = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT);
    cdi_ptr->con_info.test_settings.remote_adapter_ip_str = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST);
    cdi_ptr->con_info.test_settings.tx_timeout = DEFAULT_TIMEOUT;

    cdi_ptr->con_info.test_settings.video_stream_id = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_STREAM_ID);
    cdi_ptr->con_info.test_settings.audio_stream_id = config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_AUDIO_STREAM_ID);
    cdi_ptr->con_info.test_settings.video_sampling = (CdiAvmVideoSampling)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_VIDEO_SAMPLING);
    cdi_ptr->con_info.test_settings.alpha_used = config_get_bool(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_ALPHA_USED);
    cdi_ptr->con_info.test_settings.bit_depth = (CdiAvmVideoBitDepth)config_get_int(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_BIT_DEPTH);

    // Get some information about it.
    if (cdi_ptr->uses_video && video) {
        video_info = video_output_get_info(video);
        cdi_ptr->con_info.test_settings.rate_numerator = video_info->fps_num;
        cdi_ptr->con_info.test_settings.rate_denominator = video_info->fps_den;

        video_format format = video_output_get_format(video);
        uint32_t width = video_output_get_width(video);
        uint32_t height = video_output_get_height(video);

        blog(LOG_INFO, "Video Format[%d] Width[%d] Height[%d]", format, width, height);
        if (kCdiAvmVidRGB == cdi_ptr->con_info.test_settings.video_sampling) {
            if (VIDEO_FORMAT_BGRA != format) {
                blog(LOG_ERROR, "For RGB output, OSB Studio pixel format must be BGRA. [%d] is not supported.", format);
                return false;
            }
        } else {
            // 4:2:2 and 4:4:4.
            if (VIDEO_FORMAT_I444 != format) {
                blog(LOG_ERROR, "For YCbCr output, OSB Studio pixel format must be I444. [%d] is not supported.", format);
                return false;
            }
        }

        cdi_ptr->frame_width = width;
        cdi_ptr->frame_height = height;
        flags |= OBS_OUTPUT_VIDEO;
    }

    if (cdi_ptr->uses_audio && audio) {
        cdi_ptr->audio_samplerate = audio_output_get_sample_rate(audio);
        cdi_ptr->audio_channels = (int)audio_output_get_channels(audio);
        flags |= OBS_OUTPUT_AUDIO;
    }

    // Fill in the AVM configuration structure and payload unit size for both video and audio.
    if (video) {
        MakeVideoConfig(&cdi_ptr->con_info, &cdi_ptr->avm_video_config, &cdi_ptr->video_unit_size, video);
    }
    if (audio) {
        MakeAudioConfig(&cdi_ptr->con_info, &cdi_ptr->avm_audio_config, &cdi_ptr->audio_unit_size, audio);
    }

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
                true, // true= Make thread-safe (use OS resource locks)
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

	std::lock_guard<std::mutex> guard(cdi_ptr->connection_mutex);

    cdi_ptr->started = false;
    obs_output_end_data_capture(cdi_ptr->output);

    cdi_ptr->frame_width = 0;
    cdi_ptr->frame_height = 0;
    
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
 * @brief Called by OBS to update the output with setting changes.
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
    delete cdi_ptr; // Allocated using C++ new.
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:2:2 8-bit.
 */
static void i444_to_cdi_422_8bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 2;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);
        out = output + (y * out_linesize);

        // 4:2:2 8-bit: CB,Y0,CR,Y1 (CB = U, CR= V, Y=Y)
        for (int x = 0; x < width; x += 2) {
            *(out++) = *(U++); U++; // 1st CB and skip 2nd CB
            *(out++) = *(Y++);      // 1st Y
            *(out++) = *(V++); V++; // 1st CR and skip 2nd CR
            *(out++) = *(Y++);      // 2nd Y
        }
    }
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:2:2 10-bit.
 */
static void i444_to_cdi_422_10bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 2.5;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);

        out = output + (y * out_linesize);

        // 4:2:2 10-bit: CB,Y0,CR,Y1 (C'B = U, C'R= V, Y=Y)
        for (int x = 0; x < width; x += 2) {
            uint16_t CB = (uint16_t)*(U++) << 2; U++; // Get 1st CB converting to 10-bit and skip 2nd CB
            uint16_t Y0 = (uint16_t)*(Y++) << 2;      // Get 1st Y converting to 10-bit
            uint16_t Y1 = (uint16_t)*(Y++) << 2;      // Get 2nd Y converting to 10-bit
            uint16_t CR = (uint16_t)*(V++) << 2; V++; // Get 1st CR converting to 10-bit and skip 2nd CR

            *(out++) = (uint8_t)(CB >> 2);                      // CB bits 9-2
            *(out++) = (uint8_t)((CB << 6 & 0xC0) | (Y0 >> 4)); // CB bits 1-0 and Y0 bits 9-4
            *(out++) = (uint8_t)((Y0 << 4) | (CR >> 6));        // Y0 bits 3-0 and CR bits 9-6
            *(out++) = (uint8_t)((CR << 2) | (Y1 >> 8));        // CR bits 5-0 and Y1 bits 9-8
            *(out++) = (uint8_t)(Y1);                           // Y1 bits 7-0
        }
    }
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:2:2 12-bit.
 */
static void i444_to_cdi_422_12bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 3;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);
        out = output + (y * out_linesize);

        // 4:2:2 12-bit: CB,Y0,CR,Y1 (C'B = U, C'R= V, Y=Y)
        for (int x = 0; x < width; x += 2) {
            uint16_t CB = (uint16_t)*(U++) << 4; U++; // Get 1st U converting to 12-bit and skip 2nd U
            uint16_t Y0 = (uint16_t)*(Y++) << 4;      // Get 1st Y converting to 12-bit
            uint16_t Y1 = (uint16_t)*(Y++) << 4;      // Get 2md Y converting to 12-bit
            uint16_t CR = (uint16_t)*(V++) << 4; V++; // Get 1st V converting to 12-bit and skip 2nd V

            *(out++) = (uint8_t)(CB >> 4);               // CB bits 11-4
            *(out++) = (uint8_t)((CB << 4) | (Y0 >> 4)); // CB bits 3-0 and Y0 bits 11-8
            *(out++) = (uint8_t)(Y0 && 0xff);            // Y0 bits 7-0
            *(out++) = (uint8_t)(CR >> 4);               // CR bits 11-4
            *(out++) = (uint8_t)((CR << 4) | (Y1 >> 4)); // CR bits 3-0 and Y1 bits 11-8
            *(out++) = (uint8_t)(Y1);                    // Y1 bits 7-0
        }
    }
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
        blog(LOG_ERROR, "OBS Video format must be I444 (planar 4:4:4 8-bit).");
        ret = false;
    }

    if (ret) {
        uint32_t width = cdi_ptr->frame_width;
        uint32_t height = cdi_ptr->frame_height;
        uint8_t* payload_ptr = (uint8_t*)user_data_ptr->sglist.sgl_head_ptr->address_ptr;
        int payload_size = 0;

        if (kCdiAvmVidBitDepth8 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 2; // 8-bit 4:2:2
            i444_to_cdi_422_8bit(frame->data, frame->linesize, width, height, payload_ptr);
        }
        else if (kCdiAvmVidBitDepth10 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 2 * 10 / 8; // 10-bit 4:2:2
            i444_to_cdi_422_10bit(frame->data, frame->linesize, width, height, payload_ptr);
        }
        else if (kCdiAvmVidBitDepth12 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 2 * 12 / 8; // 12-bit 4:2:2
            i444_to_cdi_422_12bit(frame->data, frame->linesize, width, height, payload_ptr);
        } else {
            ret = false;
        }

        // Setup the SGL size.
        user_data_ptr->sglist.total_data_size = payload_size;
        user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = payload_size;
    }

    return ret;
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:4:4 8-bit.
 */
static void i444_to_cdi_444_8bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 3;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);
        out = output + (y * out_linesize);

        // 4:4:4 8-bit: CB,Y,CR (CB = U, CR= V, Y=Y)
        for (int x = 0; x < width; x += 1) {
            *(out++) = *(U++); // CB
            *(out++) = *(Y++); // Y
            *(out++) = *(V++); // CR
        }
    }
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:4:4 10-bit.
 */
static void i444_to_cdi_444_10bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 3 * 10 / 8;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);
        out = output + (y * out_linesize);

        // 4:4:4 10-bit: C0B,Y0,C0R,C1B,Y1,C1R,C2B,Y2,C2R,C3B,Y3,C3R (C'B = U, C'R= V, Y=Y)
        for (int x = 0; x < width; x += 4) {
            uint16_t C0B = (uint16_t)*(U++) << 2; // Get 1st CB converting to 10-bit
            uint16_t C1B = (uint16_t)*(U++) << 2; // Get 2nd CB converting to 10-bit
            uint16_t C2B = (uint16_t)*(U++) << 2; // Get 3rd CB converting to 10-bit
            uint16_t C3B = (uint16_t)*(U++) << 2; // Get 4th CB converting to 10-bit
            uint16_t Y0 = (uint16_t)*(Y++) << 2;  // Get 1st Y converting to 10-bit
            uint16_t Y1 = (uint16_t)*(Y++) << 2;  // Get 2nd Y converting to 10-bit
            uint16_t Y2 = (uint16_t)*(Y++) << 2;  // Get 3rd Y converting to 10-bit
            uint16_t Y3 = (uint16_t)*(Y++) << 2;  // Get 4th Y converting to 10-bit
            uint16_t C0R = (uint16_t)*(V++) << 2; // Get 1st CR converting to 10-bit
            uint16_t C1R = (uint16_t)*(V++) << 2; // Get 2nd CR converting to 10-bit
            uint16_t C2R = (uint16_t)*(V++) << 2; // Get 3rd CR converting to 10-bit
            uint16_t C3R = (uint16_t)*(V++) << 2; // Get 4th CR converting to 10-bit

            // Output the 3 pixels of data using 15 bytes.
            CDI_10_BIT_OUT_5_BYTES(out, C0B, Y0, C0R, C1B);
            CDI_10_BIT_OUT_5_BYTES(out, Y1, C1R, C2B, Y2);
            CDI_10_BIT_OUT_5_BYTES(out, C2R, C3B, Y3, C3R);
        }
    }
}

/**
 * @brief Convert three plane YUV 4:4:4 8-bit to single plane YCbCr 4:4:4 12-bit.
 */
static void i444_to_cdi_444_12bit(uint8_t* YUV[], uint32_t in_linesize[], int width, int height, uint8_t* output)
{
    uint8_t* Y;
    uint8_t* U;
    uint8_t* V;
    uint8_t* out;
    int out_linesize = width * 3 * 12 / 8;

    for (int y = 0; y < height; y++) {
        Y = YUV[0] + (y * in_linesize[0]);
        U = YUV[1] + (y * in_linesize[1]);
        V = YUV[2] + (y * in_linesize[2]);
        out = output + (y * out_linesize);

        // 4:4:4 12-bit: C0B,Y0,C0R,C1B,Y1,C1R (CB = U, CR= V, Y=Y)
        for (int x = 0; x < width; x += 2) {
            uint16_t C0B = (uint16_t)*(U++) << 4; // Get 1st U converting to 12-bit
            uint16_t C1B = (uint16_t)*(U++) << 4; // Get 2nd U converting to 12-bit
            uint16_t Y0 = (uint16_t)*(Y++) << 4;  // Get 1st Y converting to 12-bit
            uint16_t Y1 = (uint16_t)*(Y++) << 4;  // Get 2nd Y converting to 12-bit
            uint16_t C0R = (uint16_t)*(V++) << 4; // Get 1st V converting to 12-bit
            uint16_t C1R = (uint16_t)*(V++) << 4; // Get 2nd V converting to 12-bit

            CDI_12_BIT_OUT_3_BYTES(out, C0B, Y0);
            CDI_12_BIT_OUT_3_BYTES(out, C0R, C1B);
            CDI_12_BIT_OUT_3_BYTES(out, Y1, C1R);
        }
    }
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
    cdi_output* cdi_ptr = user_data_ptr->cdi_ptr;

    video_t* video = obs_output_video(cdi_ptr->output);
    video_format format = video_output_get_format(video);
    if (VIDEO_FORMAT_I444 != format) {
        blog(LOG_ERROR, "OBS Video format must be I444 (planar 4:4:4 8-bit).");
        ret = false;
    }

    if (ret) {
        uint32_t width = cdi_ptr->frame_width;
        uint32_t height = cdi_ptr->frame_height;
        uint8_t* payload_ptr = (uint8_t*)user_data_ptr->sglist.sgl_head_ptr->address_ptr;
        int payload_size = 0;

        if (kCdiAvmVidBitDepth8 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3; // 8-bit 4:4:4
            i444_to_cdi_444_8bit(frame->data, frame->linesize, width, height, payload_ptr);
        }
        else if (kCdiAvmVidBitDepth10 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3 * 10 / 8; // 10-bit 4:4:4
            i444_to_cdi_444_10bit(frame->data, frame->linesize, width, height, payload_ptr);
        }
        else if (kCdiAvmVidBitDepth12 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3 * 12 / 8; // 12-bit 4:4:4
            i444_to_cdi_444_12bit(frame->data, frame->linesize, width, height, payload_ptr);
        } else {
            ret = false;
        }

        // Setup the SGL size.
        user_data_ptr->sglist.total_data_size = payload_size;
        user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = payload_size;
    }

    return ret;
}

/**
 * @brief Convert three plane RGBA 8-bit to single plane RGB 8-bit with optional alpha channel.
 */
static void rgba_to_cdi_8bit(uint8_t* BGRA[], uint32_t in_linesize[], int width, int height, uint8_t* output, bool alpha_used)
{
    uint8_t* in;
    uint8_t* out;
    int out_linesize = width * 3;

    for (int y = 0; y < height; y++) {
        in = BGRA[0] + (y * in_linesize[0]); // Input is BGRA in a single plane
        out = output + y * out_linesize;

        // RGB 8-bit: R, G, B
        for (int x = 0; x < width; x += 1) {
            // Process 1 pixel of RGB data.
            *(out++) = *(in+2); // R
            *(out++) = *(in+1); // G
            *(out++) = *(in); // B
            in += 4; // Skip alpha here.
        }
    }

    if (alpha_used) {
        int out_offset = height * out_linesize;
        int a_out_linesize = width; // Linesize for CDI alpha.
        uint8_t* A;
        for (int y = 0; y < height; y++) {
            A = BGRA[0] + (y * in_linesize[0] + 3); // BGRA format, so A is + 3.
            out = output + out_offset + (y * a_out_linesize);

            // Alpha uses a single plane.
            for (int x = 0; x < width; x += 1) {
                *(out++) = *(A); // Alpha
                A += 4;
            }
        }
    }
}

/**
 * @brief Convert three plane RGBA 8-bit to single plane RGB 8-bit with optional alpha channel.
 */
static void rgba_to_cdi_10bit(uint8_t* BGRA[], uint32_t in_linesize[], int width, int height, uint8_t* output,bool alpha_used)
{
    uint8_t* in;
    uint8_t* out;
    int out_linesize = width * 3 * 10 / 8;

    for (int y = 0; y < height; y++) {
        in = BGRA[0] + (y * in_linesize[0]); // Input is BGRA in a single plane
        out = output + y * out_linesize;

        // RGB 10-bit: R0, G0, B0, R1, G1, B1, R2, G2, B2, R3, G3, B3
        for (int x = 0; x < width; x += 4) {
            // Get 4 pixels of RGB data, converting to 10-bit.
            uint16_t B0 = (uint16_t)*(in++) << 2;
            uint16_t G0 = (uint16_t)*(in++) << 2;
            uint16_t R0 = (uint16_t)*(in++) << 2;
            in++; // skip alpha
            uint16_t B1 = (uint16_t)*(in++) << 2;
            uint16_t G1 = (uint16_t)*(in++) << 2;
            uint16_t R1 = (uint16_t)*(in++) << 2;
            in++; // skip alpha
            uint16_t B2 = (uint16_t)*(in++) << 2;
            uint16_t G2 = (uint16_t)*(in++) << 2;
            uint16_t R2 = (uint16_t)*(in++) << 2;
            in++; // skip alpha
            uint16_t B3 = (uint16_t)*(in++) << 2;
            uint16_t G3 = (uint16_t)*(in++) << 2;
            uint16_t R3 = (uint16_t)*(in++) << 2;
            in++; // skip alpha

            // Output the 4 pixels of data using 15 bytes.
            CDI_10_BIT_OUT_5_BYTES(out, R0, G0, B0, R1);
            CDI_10_BIT_OUT_5_BYTES(out, G1, B1, R2, G2);
            CDI_10_BIT_OUT_5_BYTES(out, B2, R3, G3, B3);
        }
    }

    if (alpha_used) {
        int out_offset = height * out_linesize;
        int a_out_linesize = width * 10 / 8; // Linesize for CDI alpha.
        uint8_t* A;
        for (int y = 0; y < height; y++) {
            A = BGRA[0] + (y * in_linesize[0] + 3); // BGRA format, so A is + 3.
            out = output + out_offset + (y * a_out_linesize);

            // Since CDI alpha only uses one plane, we can process 4 alpha values at a time.
            for (int x = 0; x < width; x += 4) {
                // Get 4 alpha values, converting to 10-bit. If bit 0 is set, replicate to bits 1-0;
                uint16_t A0 = (uint16_t)*(A) << 2 | ((*(A) & 0x01) ? 0x03 : 0);
                A += 4; // Skip alpha
                uint16_t A1 = (uint16_t)*(A) << 2 | ((*(A) & 0x01) ? 0x03 : 0);
                A += 4; // Skip alpha
                uint16_t A2 = (uint16_t)*(A) << 2 | ((*(A) & 0x01) ? 0x03 : 0);
                A += 4; // Skip alpha
                uint16_t A3 = (uint16_t)*(A) << 2 | ((*(A) & 0x01) ? 0x03 : 0);
                A += 4; // Skip alpha
                CDI_10_BIT_OUT_5_BYTES(out, A0, A1, A2, A3); // Output using 5 bytes.
            }
        }
    }
}

/**
 * @brief Convert three plane BGRA 8-bit to single plane RGB 12-bit with optional alpha channel.
 */
static void rgba_to_cdi_12bit(uint8_t* BGRA[], uint32_t in_linesize[], int width, int height, uint8_t* output,bool alpha_used)
{
    uint8_t* in;
    uint8_t* out;
    int out_linesize = width * 3 * 12 / 8;

    for (int y = 0; y < height; y++) {
        in = BGRA[0] + (y * in_linesize[0]); // Input is BGRA in a single plane
        out = output + y * out_linesize;

        // RGB 12-bit: R0, G0, B0, R1, G1, B1
        for (int x = 0; x < width; x += 2) {
            // Get 2 pixels of RGB data, converting to 12-bit.
            uint16_t B0 = (uint16_t)*(in++) << 4;
            uint16_t G0 = (uint16_t)*(in++) << 4;
            uint16_t R0 = (uint16_t)*(in++) << 4;
            in++; // skip alpha
            uint16_t B1 = (uint16_t)*(in++) << 4;
            uint16_t G1 = (uint16_t)*(in++) << 4;
            uint16_t R1 = (uint16_t)*(in++) << 4;
            in++; // skip alpha

            // Output the two pixels of data using 9 bytes.
            CDI_12_BIT_OUT_3_BYTES(out, R0, G0);
            CDI_12_BIT_OUT_3_BYTES(out, B0, R1);
            CDI_12_BIT_OUT_3_BYTES(out, G1, B1);
        }
    }

    if (alpha_used) {
        int out_offset = height * out_linesize;
        int a_out_linesize = width * 12 / 8; // Linesize for CDI alpha.
        uint8_t* A;
        for (int y = 0; y < height; y++) {
            A = BGRA[0] + (y * in_linesize[0] + 3); // BGRA format, so A is + 3.
            out = output + out_offset + (y * a_out_linesize);

            // Since CDI alpha only uses one plane, we can process 2 alpha values at a time.
            for (int x = 0; x < width; x += 2) {
                // Get 2 alpha values, converting to 12-bit. If bit 0 is set, replicate to bits 3-0;
                uint16_t A0 = (uint16_t)*(A) << 4 | ((*(A) & 0x01) ? 0x0F : 0);
                A += 4;
                uint16_t A1 = (uint16_t)*(A) << 4 | ((*(A) & 0x01) ? 0x0F : 0);
                A += 4;
                CDI_12_BIT_OUT_3_BYTES(out, A0, A1); // Output using 3 bytes.

            }
        }
    }
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
    cdi_output* cdi_ptr = user_data_ptr->cdi_ptr;

    video_t* video = obs_output_video(cdi_ptr->output);
    video_format format = video_output_get_format(video);
    if (VIDEO_FORMAT_BGRA != format) {
        blog(LOG_ERROR, "OBS Video format must be BGRA (RGB w/alpha 8-bit).");
        ret = false;
    }

    if (ret) {
        uint32_t width = cdi_ptr->frame_width;
        uint32_t height = cdi_ptr->frame_height;
        uint8_t* payload_ptr = (uint8_t*)user_data_ptr->sglist.sgl_head_ptr->address_ptr;
        int payload_size = 0;
        bool alpha_used = cdi_ptr->con_info.test_settings.alpha_used;

        if (kCdiAvmVidBitDepth8 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3; // 8-bit RGB
            if (alpha_used) {
                payload_size += height * width;
            }
            rgba_to_cdi_8bit(frame->data, frame->linesize, width, height, payload_ptr, alpha_used);
        }
        else if (kCdiAvmVidBitDepth10 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3 * 10 / 8; // 10-bit RGB
            if (alpha_used) {
                payload_size += height * width * 10 / 8;
            }
            rgba_to_cdi_10bit(frame->data, frame->linesize, width, height, payload_ptr, alpha_used);
        }
        else if (kCdiAvmVidBitDepth12 == cdi_ptr->con_info.test_settings.bit_depth) {
            payload_size = height * width * 3 * 12 / 8; // 12-bit RGB
            if (alpha_used) {
                payload_size += height * width * 12 / 8;
            }
            rgba_to_cdi_12bit(frame->data, frame->linesize, width, height, payload_ptr, alpha_used);
        }

        // Setup the SGL size.
        user_data_ptr->sglist.total_data_size = payload_size;
        user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = payload_size;
    }

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

    std::lock_guard<std::mutex> guard(cdi_ptr->connection_mutex);

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
    } else if (kCdiAvmVidYCbCr444 == cdi_ptr->con_info.test_settings.video_sampling) {
        send_frame = ObsToCdi444VideoFrame(user_data_ptr, frame);
    } else if (kCdiAvmVidRGB == cdi_ptr->con_info.test_settings.video_sampling) {
        send_frame = ObsToCdiRgbVideoFrame(user_data_ptr, frame);
    }

    if (send_frame) {
        CdiPtpTimestamp timestamp;

        //make a properly paced timestamp
        timestamp.seconds = floor(frame->timestamp / 1000000000);
        timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

        // Send the video payload.
        if (!SendAvmPayload(user_data_ptr, &timestamp, &cdi_ptr->avm_video_config, cdi_ptr->video_unit_size,
                            cdi_ptr->con_info.test_settings.video_stream_id)) {
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

	std::lock_guard<std::mutex> guard(cdi_ptr->connection_mutex);

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

    const int num_channels = cdi_ptr->audio_channels;
    const int num_samples = frame->frames;
    const int samplebytes = num_samples * CDI_BYTES_PER_AUDIO_SAMPLE; // Each audio sample in CDI uses 3 bytes (24-bit PCM).
    const int data_size = num_channels * samplebytes;

    // for each NDI channel, insert 24-bit int audio segment in correct spot of temp buffer.
    for (int current_channel = 0; current_channel < num_channels; current_channel++) {
        // Memory location of where to write 24-bit int for this channel in temp buffer.
        uint8_t* interleaved_dest_ptr = (uint8_t*)user_data_ptr->sglist.sgl_head_ptr->address_ptr + (current_channel * CDI_BYTES_PER_AUDIO_SAMPLE);
        // Memory location of where to read 32-bit float for this channel.
        float* channel_src_ptr = (float*)frame->data[current_channel];

        // for each channel sample, convert 32-bit float to 24-bit int + insert segment in correct spot of temp buffer.
        for (int current_sample = 0; current_sample < num_samples; current_sample++) {
            // Get 4 byte sample of 32-bit float of current NDI audio memory location.
            float sample_float = *channel_src_ptr;
            // 4 byte sample is a frequency wave constrained to [-1, 1].
            // Code ensures sample is in range, but will pick sample frequency to scale up.
            // Scaled sample will make a large number where most significant bits are no longer in decimal place.
            double scaled_double = max(-1.0, min(1.0, sample_float)) * 0x7fffffff;
            // Get integer portion of large number.
            // Integer portion now represents audio frequency.
            signed int scaled_signed_int = (signed int)scaled_double;

            // Shifting of 3 most important bytes to turn Little-Endian into Big-Endian with 3 byte representation
            // for CDI interleaved destination.
            interleaved_dest_ptr[2] = (unsigned char)(scaled_signed_int >> 8);
            interleaved_dest_ptr[1] = (unsigned char)(scaled_signed_int >> 16);
            interleaved_dest_ptr[0] = (unsigned char)(scaled_signed_int >> 24);

            // Moves original NDI audio memory location by 4 bytes for next 32-bit float read for channel.
            // Note: The audio samples in the NDI source audio are not channel interleaved.
            channel_src_ptr++;

            // Updates memory location of where to put next 24-bit int audio for channel.
            // Updates by number of 3 byte channels in between current location and next interleaved location
            // for same channel.
            interleaved_dest_ptr += num_channels * CDI_BYTES_PER_AUDIO_SAMPLE;
        }
    }

    user_data_ptr->sglist.total_data_size = data_size;
    user_data_ptr->sglist.sgl_head_ptr->size_in_bytes = data_size;

    CdiPtpTimestamp timestamp;
    timestamp.seconds = floor(frame->timestamp / 1000000000);
    timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

    // Send the audio payload.
    if (!SendAvmPayload(user_data_ptr, &timestamp, &cdi_ptr->avm_audio_config, cdi_ptr->audio_unit_size,
                        cdi_ptr->con_info.test_settings.audio_stream_id)) {
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
