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

extern "C"{
#include "obs-cdi.h"
#include "obs-cdi-output.h"
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
#include "cdi_baseline_profile_01_00_api.h"
#include "cdi_baseline_profile_02_00_api.h"
};

//*********************************************************************************************************************
//***************************************** START OF DEFINITIONS AND TYPES ********************************************
//*********************************************************************************************************************

//we use these later to refer to the configuration elements specific to CDI
#define SECTION_NAME "CDIPlugin"
#define PARAM_MAIN_OUTPUT_ENABLED "MainOutputEnabled"
#define PARAM_MAIN_OUTPUT_NAME "MainOutputName"
#define PARAM_MAIN_OUTPUT_DEST "MainOutputDest"
#define PARAM_MAIN_OUTPUT_PORT "MainOutputPort"
#define PARAM_MAIN_OUTPUT_EFA "MainOutputEFA"

static int notconnect;
static int payload_size_8bit;
static int payload_size_10bit;
CdiPtpTimestamp timestamp; 

//structures that hold metadata about the video
struct cdi_cfg {
	const char* video_settings;
	enum video_format format;
	int width;
	int height;
};

struct cdi_data {

	int64_t total_frames;
	int frame_size;
	uint64_t start_timestamp;
	struct cdi_cfg config;
	bool initialized;
	char* last_error;
};

/* ------------------------------------------------------------------------- */

#define DEFAULT_AUDIO_SIZE                (6144)
#define DEFAULT_TIMEOUT                (16666)

/**
 * Enum for connection protocol types.
 */
typedef enum {
	kTestProtocolRaw, ///< Raw connection
	kTestProtocolAvm, ///< Audio, Video and Metadata (AVM) connection
} TestConnectionProtocolType;

/**
 * @brief A structure that holds all the settings 
 */
typedef struct {
	const char* local_adapter_ip_str;  ///< The local network adapter IP address.
	int dest_port;                     ///< The destination port number.
	const char* remote_adapter_ip_str; ///< The remote network adapter IP address.
	TestConnectionProtocolType protocol_type; ///< Protocol type (AVM or RAW).
	int payload_size;                  ///< Payload size in bytes.
	int rate_numerator;                ///< The numerator for the number of payloads per second to send.
	int rate_denominator;              ///< The denominator for the number of payloads per second to send.
	int tx_timeout;                    ///< The transmit timeout in microseconds for a Tx payload.
} TestSettings;

/**
 * @brief A structure for storing all info related to a specific connection, including test settings, connection
 * configuration data from the SDK, and state information for the test connection.
 */
typedef struct {
	CdiConnectionHandle connection_handle; ///< The connection handle returned by CdiRawTxCreate().

	TestSettings test_settings;            ///< Test settings data structure provided by the user.

	CdiSignalType payload_callback_signal; ///< Signal to indicate when a payload has been delivered.
	volatile bool payload_error;           ///< true if Tx callback got a payload error.

	CdiSignalType connection_state_change_signal;   ///< Signal used for connection state changes.
	volatile CdiConnectionStatus connection_status; ///< Current status of the connection.

	void* adapter_tx_buffer_ptr;           ///< Adapter's Tx buffer pointer.
	void* adapter_tx_audio_buffer_ptr;     ///< Adapter's Tx audio buffer pointer.

	uint64_t payload_start_time;           ///< Payload start time, used by Tx Callback functions.
	int rate_period_microseconds;          ///< Calculated Tx rate period.

	/// @brief Number of times payload callback function has been invoked. NOTE: This variable is used by multiple
	/// threads and not declared volatile. It is used as an example of how to use the CdiOsAtomic...() API functions.
	int payload_cb_count;
} TestConnectionInfo;

//initialize some structs for later
CdiAvmConfig avm_config = { 0 };
CdiAvmConfig audio_config = { 0 };
TestConnectionInfo con_info = {};
CdiSignalType con_state;

//*********************************************************************************************************************
//******************************************* START STATIC FUNCTIONS ***********************************************
//*********************************************************************************************************************
	/**
	 * Handle the connection callback.
	 *
	 * @param cb_data_ptr Pointer to CdiCoreConnectionCbData callback data.
	 */
	static void TestConnectionCallback(const CdiCoreConnectionCbData* cb_data_ptr)
	{
		TestConnectionInfo* connection_info_ptr = (TestConnectionInfo*)cb_data_ptr->connection_user_cb_param;

		// Update connection state and set state change signal.
		connection_info_ptr->connection_status = cb_data_ptr->status_code;
		CdiOsSignalSet(connection_info_ptr->connection_state_change_signal);
	}

	/**
	 * Process core Tx callback, that is common to both AVM and RAW Tx payload callbacks.
	 *
	 * @param connection_info_ptr Pointer to test connection state data.
	 * @param core_cb_data_ptr  Pointer to core callback data.
	 */
	static void ProcessCoreTxCallback(TestConnectionInfo* connection_info_ptr, const CdiCoreCbData* core_cb_data_ptr)
	{
		int count = connection_info_ptr->payload_cb_count;

		if (kCdiStatusOk != core_cb_data_ptr->status_code) {
			//CDI_LOG_THREAD(kLogError, "Send payload failed[%s].",
			//	CdiCoreStatusToString(core_cb_data_ptr->status_code));
			connection_info_ptr->payload_error = true;
		}
		else {
			uint64_t timeout_time = connection_info_ptr->payload_start_time + connection_info_ptr->test_settings.tx_timeout;
			uint64_t current_time = CdiOsGetMicroseconds();
			if (current_time > timeout_time) {
				//CDI_LOG_THREAD(kLogError, "Payload [%d] late by [%llu]microseconds.", count, current_time - timeout_time);
				connection_info_ptr->payload_error = true;
			}
		}

		// Set next payload's expected start time.
		connection_info_ptr->payload_start_time += connection_info_ptr->rate_period_microseconds;

		// Set the payload callback signal to wakeup the app, if it was waiting.
		CdiOsSignalSet(connection_info_ptr->payload_callback_signal);
	}

	/**
	 * Handle the Tx AVM callback. NOTE: Only used by the AVM API functions.
	 *
	 * @param   cb_data_ptr  Pointer to Tx AVM callback data.
	 */
	static void TestAvmTxCallback(const CdiAvmTxCbData* cb_data_ptr)
	{
		TestConnectionInfo* connection_info_ptr = (TestConnectionInfo*)cb_data_ptr->core_cb_data.user_cb_param;
		assert(kTestProtocolAvm == connection_info_ptr->test_settings.protocol_type);

		ProcessCoreTxCallback(connection_info_ptr, &cb_data_ptr->core_cb_data);
	}

	/**
	 * Creates the generic configuration structure to use when sending AVM payloads.
	 *
	 * @param connection_info_ptr Pointer to a structure containing user settings needed for the configuration.
	 * @param avm_config_ptr Address of where to write the generated generic configuration structure.
	 * @param payload_unit_size_ptr Pointer to the location into which the payload unit size is to be written. This value
	 *                              needs to be set in payload_config_ptr->core_config_data.unit_size for calls to
	 *                              CdiAvmTxPayload().
	 *
	 * @return CdiReturnStatus kCdiStatusOk if the configuration structure was created successfully, kCdiStatusFatal if not.
	 */
	static CdiReturnStatus MakeAvmConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
		int* payload_unit_size_ptr, const video_output_info* video_info)
	{

		//OBS and CDI support 601 or 709. If you set something else in OBS, this will default to 709 anyway.
		CdiAvmColorimetry Colorimetry; 
		if (video_info->colorspace == VIDEO_CS_601) { Colorimetry = kCdiAvmVidColorimetryBT601; }
		else { Colorimetry = kCdiAvmVidColorimetryBT709; }

		//Similarly, we default the range to full unless the user specifies partial
		CdiAvmVideoRange Range;
		if (video_info->range == VIDEO_RANGE_PARTIAL) { Range = kCdiAvmVidRangeNarrow; }
		else { Range = kCdiAvmVidRangeFull; }

		CdiAvmBaselineConfig baseline_config = {
			kCdiAvmVideo,
			{
				01, // Using baseline profile V01.00.
				00, //minor version
				video_info->width, //width
				video_info->height, //height
				kCdiAvmVidYCbCr422, //use YCbCr 4:2:2 - this plugin only supports YCbCr 4:2:2 10-bit
				kCdiAvmAlphaUnused, //no alpha channel
				kCdiAvmVidBitDepth10, //10 bit video
				connection_info_ptr->test_settings.rate_numerator,
				connection_info_ptr->test_settings.rate_denominator,
				Colorimetry, //color space
				false,
				false,
				kCdiAvmVidTcsSDR, // Standard Dynamic Range video stream.
				Range,
				1, //par numerator
				1, //par demoninator
				0,
				0, // 0= Use full frame size.
				0,
				0 // 0= Use full frame size.
			}
		};
		return CdiAvmMakeBaselineConfiguration(&baseline_config, avm_config_ptr, payload_unit_size_ptr);
	}

	//we also made an audio config
	static CdiReturnStatus MakeAudioConfig(const TestConnectionInfo* connection_info_ptr, CdiAvmConfig* avm_config_ptr,
		int* payload_unit_size_ptr)
	{
		CdiAvmBaselineConfig audio_config;
		audio_config.payload_type = kCdiAvmAudio;
		audio_config.audio_config.version = { 01, 00 }; //CDI version
		audio_config.audio_config.grouping = kCdiAvmAudioST; //stereo
		audio_config.audio_config.sample_rate_khz = kCdiAvmAudioSampleRate48kHz; //48k
		
		//english
		audio_config.audio_config.language[0] = 'e';
		audio_config.audio_config.language[1] = 'n';
		audio_config.audio_config.language[2] = 'g';
			
		return CdiAvmMakeBaselineConfiguration(&audio_config, avm_config_ptr, payload_unit_size_ptr);
	}

	/**
	 * Send a payload using an AVM API function.
	 *
	 * @param connection_info_ptr Pointer to connection info structure.
	 * @param sgl_ptr Pointer to SGL.
	 * @param timestamp_ptr Pointer to timestamp.
	 * @param avm_config_ptr Pointer to the generic configuration structure to use for the stream.
	 *
	 * @return A value from the CdiReturnStatus enumeration.
	 */
	static CdiReturnStatus SendAvmPayload(TestConnectionInfo* connection_info_ptr, CdiSgList* sgl_ptr,
		CdiPtpTimestamp* timestamp_ptr, CdiAvmConfig* avm_config_ptr)
	{
		CdiReturnStatus rs = kCdiStatusOk;

		CdiAvmTxPayloadConfig payload_config = {
			*timestamp_ptr,
			0,
			connection_info_ptr,
			0
		};

		if (kCdiStatusOk == rs) {
			// Send the payload, retrying if the queue is full.
			do {
				rs = CdiAvmTxPayload(connection_info_ptr->connection_handle, &payload_config, avm_config_ptr, sgl_ptr,
					connection_info_ptr->test_settings.tx_timeout);
			} while (kCdiStatusQueueFull == rs);
		}

		return rs;
	}

	static CdiReturnStatus SendAudioPayload(TestConnectionInfo* connection_info_ptr, CdiSgList* sgl_ptr,
		CdiPtpTimestamp* timestamp_ptr, CdiAvmConfig* avm_config_ptr)
	{
		CdiReturnStatus rs = kCdiStatusOk;

		CdiAvmTxPayloadConfig payload_config = {
			*timestamp_ptr,
			0,
			connection_info_ptr,
			0,
			1 //this is the stream ID. It must be unique for each essence within the CDI session. Note that the video version of this doesn't have this value in the struct because our video stream ID is 0
		};

		if (kCdiStatusOk == rs) {
			// Send the payload, retrying if the queue is full.
			do {
				rs = CdiAvmTxPayload(connection_info_ptr->connection_handle, &payload_config, avm_config_ptr, sgl_ptr,
					connection_info_ptr->test_settings.tx_timeout);
			} while (kCdiStatusQueueFull == rs);
		}

		return rs;
	}

//convert three plane YUV to single plane YCbCr.
static void i444_to_ycbcr(uint8_t* YUV[], uint32_t in_linesize[],
						  uint32_t start_y, uint32_t end_y,
						  uint8_t* output, uint32_t out_linesize)
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
//this is the structure that holds things about our audio and video including the buffers we will later use for manipulation of the data
struct cdi_output
{
	obs_output_t *output;
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
	const audio_output_info *audio_info;
	os_performance_token_t* perf_token;
};

//get the name of the output from the configuration
const char* cdi_output_getname(void* data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("CDIPlugin.OutputName");
}

//get some properties as well
obs_properties_t* cdi_output_getproperties(void* data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t* props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "cdi_name",
		obs_module_text("CDIPlugin.OutputProps.CDIName"), OBS_TEXT_DEFAULT);

	return props;
}

void cdi_output_getdefaults(obs_data_t* settings)
{
	obs_data_set_default_string(settings,
								"cdi_name", "obs-cdi output");
	obs_data_set_default_bool(settings, "uses_video", true);
	obs_data_set_default_bool(settings, "uses_audio", true);
}

//OBS calls this when we start the CDI output
bool cdi_output_start(void* data)
{
	auto cdi = (struct cdi_output*)data;
	
	uint32_t flags = 0;
	video_t* video = obs_output_video(cdi->output);
	audio_t* audio = obs_output_audio(cdi->output);

	//check to make sure there is audio and video
	if (!video && !audio) {
		blog(LOG_ERROR, "'%s': no video and audio available",cdi->cdi_name);
		return false;
	}

	//and if there is, get some information about it
	if (cdi->uses_video && video) {
		video_format format = video_output_get_format(video);
		uint32_t width = video_output_get_width(video);
		uint32_t height = video_output_get_height(video);

		blog(LOG_WARNING, "%d", format);
		blog(LOG_WARNING, "%d", width);
		blog(LOG_WARNING, "%d", height);
		switch (format) {
			//Right now, this plugin only supports OBS I444 to YCbCr conversion.  This switch can be extended for other pixel formats.  But for now, if you pick something other than I444, it will stop here.
			case VIDEO_FORMAT_I444:
				cdi->conv_linesize = width * 2;
				cdi->conv_buffer = new uint8_t[(size_t)height * (size_t)cdi->conv_linesize * 2]();
				break;

			default:
				blog(LOG_WARNING, "pixel format %d is not supported yet", format);
				return false;
		}
		//uses video
		cdi->frame_width = width;
		cdi->frame_height = height;
		cdi->video_framerate = video_output_get_frame_rate(video);
		flags |= OBS_OUTPUT_VIDEO;
	}

	//uses audio
	if (cdi->uses_audio && audio) {
		cdi->audio_samplerate = audio_output_get_sample_rate(audio);
		cdi->audio_channels = audio_output_get_channels(audio);
		cdi->audio_info = audio_output_get_info(audio);
		flags |= OBS_OUTPUT_AUDIO;
	}

	CdiLoggerInitialize(); // Intialize logger so we can use the CDI_LOG_THREAD() macro to generate console messages.

	//get the output settings from the OBS configuration
	config_t* obs_config = obs_frontend_get_global_config();
	const char*	OutputDest = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_DEST);
	const char* OutputPort = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_PORT);
	const char* OutputEFA = config_get_string(obs_config, SECTION_NAME, PARAM_MAIN_OUTPUT_EFA);

	const video_output_info* video_info = video_output_get_info(video);
	payload_size_10bit = video_info->height * video_info->width * 2.5;
	payload_size_8bit = video_info->height * video_info->width * 2;

	//now use those settings to populate our con_info
	con_info.test_settings.local_adapter_ip_str = OutputEFA;
	con_info.test_settings.dest_port = atoi(OutputPort);
	con_info.test_settings.remote_adapter_ip_str = OutputDest;
	con_info.test_settings.protocol_type = kTestProtocolAvm;
	con_info.test_settings.payload_size = payload_size_10bit + DEFAULT_AUDIO_SIZE;  //payload size is the sum of the video plus audio buffer sizes
	con_info.test_settings.rate_numerator = video_info->fps_num;
	con_info.test_settings.rate_denominator = video_info->fps_den;
	con_info.test_settings.tx_timeout = DEFAULT_TIMEOUT;

	int payload_count = 0;
	int payload_unit_size = 0;
	int payload_audio_size = 0;
	blog(LOG_WARNING, "Making AVM Structure");
	// Fill in the AVM configuration structure and payload unit size for both video and audio.
	MakeAvmConfig(&con_info, &avm_config, &payload_unit_size, video_info);
	MakeAudioConfig(&con_info, &audio_config, &payload_audio_size);

	// Create resources used by this application.
	CdiOsSignalCreate(&con_info.payload_callback_signal);
	CdiOsSignalCreate(&con_info.connection_state_change_signal);
	
	//-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 1: Initialize CDI core (must do before initializing adapter or creating connections).
	//-----------------------------------------------------------------------------------------------------------------
	//log to a CDI session file. This file will show up in the run directory of OBS
	CdiLogMethodData log_method_data;
	log_method_data.log_method = kLogMethodFile;
	log_method_data.log_filename_str = "obscdi.log";

	CdiCoreConfigData core_config;
	core_config.default_log_level = kLogDebug;
	core_config.global_log_method_data_ptr = &log_method_data;
	core_config.cloudwatch_config_ptr = NULL; //Don't use cloudwatch for this. This can be changed later if someone wants to have the payloads tracked in cloudwatch.

	//Init CDI with the core config we just built
	CdiReturnStatus rs = CdiCoreInitialize(&core_config);
	blog(LOG_WARNING, "%d", rs);

	//-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 2: Register the EFA adapter.
	//-----------------------------------------------------------------------------------------------------------------
	CdiAdapterHandle adapter_handle = NULL;
	if (kCdiStatusOk == rs) {
		// Round-up buffer size to a multiple of HUGE_PAGES_BYTE_SIZE.
		int tx_buffer_size_bytes = ((con_info.test_settings.payload_size + 1 - 1) /
			1) * 1;

		//Build the CDI adapter data using the local IP, the size of the buffer we want, and using EFA. Can be changed to use other adapters I guess. 
		CdiAdapterData adapter_data = {
			con_info.test_settings.local_adapter_ip_str,
			tx_buffer_size_bytes,
			nullptr,
			kCdiAdapterTypeEfa // Use EFA adapter.
		};
		//log some stuff for good measure
		blog(LOG_WARNING, "%s", con_info.test_settings.local_adapter_ip_str);
		blog(LOG_WARNING, "%d", tx_buffer_size_bytes);
		blog(LOG_WARNING, "%d", con_info.adapter_tx_buffer_ptr);
		//Now initialize the adapter too
		rs = CdiCoreNetworkAdapterInitialize(&adapter_data, &adapter_handle);
		blog(LOG_WARNING, "AdaptInit");
		blog(LOG_WARNING, "%d", rs);

		// Get Tx buffer allocated by the Adapter.
		con_info.adapter_tx_buffer_ptr = adapter_data.ret_tx_buffer_ptr;
		//The audio pointer should point to the start address immediately following the video in the TX buffer, so we offset it this way. That means audio bytes will be right after video bytes in the buffer.
		con_info.adapter_tx_audio_buffer_ptr = reinterpret_cast<unsigned char*>(adapter_data.ret_tx_buffer_ptr) + payload_size_10bit;
	}

	//-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 3: Create AVM Tx connection.
	//-----------------------------------------------------------------------------------------------------------------
	if (kCdiStatusOk == rs) {
		CdiTxConfigData config_data = {
			// Settings that are unique to a Tx connection.

			adapter_handle,
			con_info.test_settings.remote_adapter_ip_str,
			con_info.test_settings.dest_port,
			 -1, // -1= Let OS decide which CPU core to use.
			 0,
			 0,
			NULL,
			&log_method_data,
			TestConnectionCallback, // Configure connection callback.
			&con_info,
			NULL,
			NULL,
			{0, true}

		};
		if (kTestProtocolAvm == con_info.test_settings.protocol_type) {
			rs = CdiAvmTxCreate(&config_data, TestAvmTxCallback, &con_info.connection_handle);
			blog(LOG_WARNING, "Makin the AVM stuff");
		}
	
	}
	blog(LOG_WARNING, "Connection Created");
	//-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 4: Wait for connection to be established with remote target.
	//-----------------------------------------------------------------------------------------------------------------
	
	time_t futur = time(NULL) + 5;
	while (kCdiStatusOk == rs && kCdiConnectionStatusDisconnected == con_info.connection_status) {
		blog(LOG_WARNING, "Attempting to connect...");
		//CDI_LOG_THREAD(kLogInfo, "Waiting to establish connection with remote target...");
		CdiOsSignalWait(con_info.connection_state_change_signal, 2000, NULL); //Timeout after 2 seconds if a connection isn't established so the program can continue. Don't worry, it will keep trying in the background.
		blog(LOG_WARNING, "Waiting...");
		if (time(NULL) > futur) {
			blog(LOG_WARNING, "CDI connection could not be established, continuing to try in the background...");
			notconnect = 1;
			break;
		}
		CdiOsSignalClear(con_info.connection_state_change_signal);
	}
	//CDI_LOG_THREAD(kLogInfo, "Connected. Sending payloads...");

	//-----------------------------------------------------------------------------------------------------------------
	// CDI SDK Step 5: Can now send the desired number of payloads. Will send at the specified rate. If we get any
	// errors or the connection drops, then exit the loop.
	//----------------------------------------------------------------------------------------------------------------


	// Setup rate period and start times.I don't really think this is needed in this particular case because OBS paces the frame calls. But We keep it here as an example. 
	con_info.rate_period_microseconds = ((1000000 * con_info.test_settings.rate_denominator) /
		con_info.test_settings.rate_numerator);
	con_info.payload_start_time = CdiOsGetMicroseconds();
	uint64_t rate_next_start_time = con_info.payload_start_time + con_info.rate_period_microseconds;

	//initialize the timestamp struct
	timestamp = CdiCoreGetPtpTimestamp(NULL);

	//Tell OBS we've started and to start capturing the video and audio frames with the flags we set earlier. 
	cdi->started = obs_output_begin_data_capture(cdi->output, flags);
	//And we're off to the races...
	return cdi->started;
}

//Stop the CDI output if someone decides to stop or exit
void cdi_output_stop(void* data, uint64_t ts)
{
	auto cdi = (struct cdi_output*)data;

	cdi->started = false;
	obs_output_end_data_capture(cdi->output);

	os_end_high_performance(cdi->perf_token);
	cdi->perf_token = NULL;

	if (cdi->conv_buffer) {
		delete cdi->conv_buffer;
	}
	
	cdi->frame_width = 0;
	cdi->frame_height = 0;
	cdi->video_framerate = 0.0;

	cdi->audio_channels = 0;
	cdi->audio_samplerate = 0;

	//-----------------------------------------------------------------------------------------------------------------
   // CDI SDK Step 6. Shutdown and clean-up CDI SDK resources.
   //-----------------------------------------------------------------------------------------------------------------
	if (&con_info.connection_handle) {
		CdiCoreConnectionDestroy(con_info.connection_handle);
	}
	CdiCoreShutdown();

	// Clean-up additional resources used by this application.
	CdiOsSignalDelete(con_info.connection_state_change_signal);
	CdiOsSignalDelete(con_info.payload_callback_signal);
}

//OBS calls this when you update the CDI settings to store the settings in the global config
void cdi_output_update(void* data, obs_data_t* settings)
{
	auto cdi = (struct cdi_output*)data;
	cdi->cdi_name = obs_data_get_string(settings, "cdi_name");
	cdi->uses_video = obs_data_get_bool(settings, "uses_video");
	cdi->uses_audio = obs_data_get_bool(settings, "uses_audio");
}

//This creates the CDI output and is called if CDI is enabled at startup or if someone turns it on
void* cdi_output_create(obs_data_t* settings, obs_output_t* output)
{
	auto cdi = (struct cdi_output*)bzalloc(sizeof(struct cdi_output));
	cdi->output = output;
	cdi->started = false;
	cdi->audio_conv_buffer = nullptr;
	cdi->audio_conv_buffer_size = 0;
	cdi->perf_token = NULL;

	auto audio = (struct audio_output_info*)bzalloc(sizeof(struct audio_output_info));
	audio->format = AUDIO_FORMAT_16BIT_PLANAR;
	cdi->audio_info = audio;

	cdi_output_update(cdi, settings);
	return cdi;
}

//Destroys the CDI structure and frees up the memory
void cdi_output_destroy(void* data)
{
	auto cdi = (struct cdi_output*)data;
	if (cdi->audio_conv_buffer) {
		bfree(cdi->audio_conv_buffer);
	}
	bfree(cdi);
}

//This is the magic right here. OBS calls this every frame. 
void cdi_output_rawvideo(void* data, struct video_data* frame)
{
	//check if the connection was made and if so run this and reset the notconnect indicator. Remember when we said we would keep trying in the background? Well this is where we check if a connection was made while we weren't looking.
	if (kCdiConnectionStatusConnected == con_info.connection_status && notconnect == 1 ){ 
		CdiOsSignalClear(con_info.connection_state_change_signal); 
		notconnect = 0;
	}
 
	auto cdi = (struct cdi_output*)data;

	if (!cdi->started || !cdi->frame_width || !cdi->frame_height)
		return;

	uint32_t width = cdi->frame_width;
	uint32_t height = cdi->frame_height;

		//convert i444 to YCbCr
		i444_to_ycbcr(frame->data, frame->linesize,
			0, height,
			cdi->conv_buffer, cdi->conv_linesize);
	//Fill TX Buffer

	//loop to convert 8 bit to 10 bit. This is needed because lots of professional video software only expects 10 bit. The process to convert 8 to 10 bit is simple...
		uint32_t pgroup;
		uint8_t* txptr = (uint8_t*)con_info.adapter_tx_buffer_ptr;
		uint8_t* srcptr = cdi->conv_buffer;

		//for every pgroup (a pgroup being 2 pixels in this case because we use the U and V elements accross 2 Y values.)
		for (int i = 0; i < payload_size_8bit; i+=4) {

			//the first byte in 8 bit vs 10 bit is the same so we just set the value of the first byte at the memory address of txptr to the value at srcptr 
			*(txptr + 0) = *srcptr;
			//For the rest of the bytes...
			//You shift the pixels in (shift 3 bytes into a temp uint32)
			pgroup = *(srcptr + 1) << 22 | *(srcptr + 2) << 12 | *(srcptr + 3) << 2;
			//You shift the pixels out (shift each byte out into 4 new bytes and put them at incrementing memory addresses)
			*(txptr + 1) = pgroup >> 24;
			*(txptr + 2) = pgroup >> 16;
			*(txptr + 3) = pgroup >> 8;
			*(txptr + 4) = pgroup >> 0;
			txptr += 5;
			srcptr += 4;
		}


	// Setup Scatter-gather-list entry for the payload data to send. NOTE: The buffers the SGL entries point to must
	// persist until the payload callback has been made. Since we are reusing the same buffer for each payload, we
	// don't need any additional logic here.
	CdiSglEntry sgl_entry = {
		con_info.adapter_tx_buffer_ptr,
		payload_size_10bit,
	};
	CdiSgList sgl = {
		payload_size_10bit,
		&sgl_entry,
		&sgl_entry,
	};

//make a properly paced timestamp
	timestamp.seconds = floor(frame->timestamp / 1000000000);
	timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

	//Send the payload
	CdiReturnStatus rs;
	rs = SendAvmPayload(&con_info, &sgl, &timestamp, &avm_config);
}

//repeat the above for audio
void cdi_output_rawaudio(void* data, struct audio_data* frame)
{
	if (kCdiConnectionStatusConnected == con_info.connection_status && notconnect == 1) {
		CdiOsSignalClear(con_info.connection_state_change_signal);
		notconnect = 0;
	}

	auto cdi = (struct cdi_output*)data;

	if (!cdi->started || !cdi->audio_samplerate || !cdi->audio_channels)
		return;
	
	int no_channels = 2;
	int no_samples = frame->frames;
	int samplebytes = frame->frames * 3;
	
	const size_t data_size =
		(size_t)no_channels * (size_t)samplebytes;

	if (data_size > cdi->audio_conv_buffer_size) {
		if (cdi->audio_conv_buffer) {
			bfree(cdi->audio_conv_buffer);
		}
		cdi->audio_conv_buffer = (uint8_t*)bmalloc(data_size);
		cdi->audio_conv_buffer_size = data_size;
	}

	signed int tempa;
	double scaleda;
	float samplea;
	float* pa;
	signed int tempb;
	double scaledb;
	float sampleb;
	float* pb;
	//OBS uses 32-bit float audio.  CDI wants 24 bit PCM. So we convert the 32 bit float to 24 bit PCM.
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

		//also the audio in OBS is in separate planes and we need to interleave them to make our single stream of stereo audio.  So for 24 bit audio, this means 6 bytes (3 for left ch, 3 for right)
		*(cdi->audio_conv_buffer + i * 6 + 2) = (unsigned char)(tempa >> 8);
		*(cdi->audio_conv_buffer + i * 6 + 1) = (unsigned char)(tempa >> 16);
		*(cdi->audio_conv_buffer + i * 6 + 0) = (unsigned char)(tempa >> 24);
		*(cdi->audio_conv_buffer + i * 6 + 5) = (unsigned char)(tempb >> 8);
		*(cdi->audio_conv_buffer + i * 6 + 4) = (unsigned char)(tempb >> 16);
		*(cdi->audio_conv_buffer + i * 6 + 3) = (unsigned char)(tempb >> 24);
	}
	//Fill TX Buffer
	memcpy(con_info.adapter_tx_audio_buffer_ptr, cdi->audio_conv_buffer, data_size);
	

	// Setup Scatter-gather-list entry for the payload data to send. NOTE: The buffers the SGL entries point to must
	// persist until the payload callback has been made. Since we are reusing the same buffer for each payload, we
	// don't need any additional logic here.
	CdiSglEntry audio_sgl_entry = {
		con_info.adapter_tx_audio_buffer_ptr,
		data_size,
	};
	CdiSgList audio_sgl = {
		data_size,
		&audio_sgl_entry,
		&audio_sgl_entry,
	};

	timestamp.seconds = floor(frame->timestamp / 1000000000);
	timestamp.nanoseconds = frame->timestamp - (timestamp.seconds * 1000000000);

	// Send the payload.
	CdiReturnStatus rs;
	rs = SendAudioPayload(&con_info, &audio_sgl, &timestamp, &audio_config);
}

//OBS structure that describes the available functions for OBS-CDI
struct obs_output_info create_cdi_output_info()
{
	struct obs_output_info cdi_output_info = {};
	cdi_output_info.id				= "cdi_output";
	cdi_output_info.flags			= OBS_OUTPUT_AV;
	cdi_output_info.get_name		= cdi_output_getname;
	cdi_output_info.get_properties	= cdi_output_getproperties;
	cdi_output_info.get_defaults	= cdi_output_getdefaults;
	cdi_output_info.create			= cdi_output_create;
	cdi_output_info.destroy			= cdi_output_destroy;
	cdi_output_info.update			= cdi_output_update;
	cdi_output_info.start			= cdi_output_start;
	cdi_output_info.stop			= cdi_output_stop;
	cdi_output_info.raw_video		= cdi_output_rawvideo;
	cdi_output_info.raw_audio		= cdi_output_rawaudio;
	return cdi_output_info;
}
