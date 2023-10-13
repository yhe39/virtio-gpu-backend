/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <linux/videodev2.h>
#include <cstring>
#include <stdio.h>
#include <Parameters.h>
#include <ICamera.h>

using namespace icamera;
#include "../include/vICamera.h"

int get_physical_id(int camera_logical_id)
{
	return camera_logical_id; // TODO, this should get from a xml or other script
}

/**
 * Return the number of cameras
 * This should be called before any other calls
 *
 * @return > 0  return cameras number
 * @return == 0 failed to get cameras number
 **/
int vcamera_get_number_of_cameras()
{
	return icamera::get_number_of_cameras();
}


static void parameters_to_metadata(icamera::Parameters &param, void *metadata)
{
	// TODO
}

static void metadata_to_parameter(void *metadata, Parameters &param)
{
	// TODO
}

/**
 * Get capability related camera info.
 * Should be called after get_number_of_cameras
 *
 * @return error code
 */
int vcamera_get_camera_info(int camera_id, vcamera_info_t *info)
{
	int ret;
	camera_info_t tmp;
	Parameters param;

	tmp.capability = &param;

	ret = icamera::get_camera_info(get_physical_id(camera_id), tmp);
	if (ret == 0)
	{
		memcpy(info, &tmp, sizeof(camera_info_t));
		parameters_to_metadata(param, info->metadata);
	}

	return ret;
}

/**
 * Initialize camera HAL
 *
 * @return error code
 **/
int vcamera_hal_init()
{
	return icamera::camera_hal_init();
}

/**
 * De-Initialize camera HAL
 *
 * @return error code
 **/
int vcamera_hal_deinit()
{
	return icamera::camera_hal_deinit();
}

/**
 * Open one camera device
 *
 * @param camera_id camera index
 *
 * @return error code
 **/
int vcamera_device_open(int camera_id)
{
	return icamera::camera_device_open(get_physical_id(camera_id));
}

/**
 * Close camera device
 *
 * @param camera_id The ID that opened before
 **/
void vcamera_device_close(int camera_id)
{
	icamera::camera_device_close(get_physical_id(camera_id));
}

/**
 * Configure the sensor input of the device
 *
 * @param camera_id The camera ID that was opened
 * @param input_config  sensor input configuration
 *
 * @return 0 succeed <0 error
 **/
int vcamera_device_config_sensor_input(int camera_id, const stream_t *inputConfig)
{
	return camera_device_config_sensor_input(get_physical_id(camera_id), inputConfig);
}

/**
 * Add stream to device
 *
 * @param camera_id The camera ID that was opened
 * @param stream_id
 * @param stream_conf stream configuration
 *
 * @return 0 succeed <0 error
 **/
int vcamera_device_config_streams(int camera_id, stream_config_t *stream_list)
{
	return icamera::camera_device_config_streams(get_physical_id(camera_id), stream_list);
}

/**
 * Start device
 *
 * Start all streams in device.
 *
 * @param camera_id The Caemra ID that opened before
 *
 * @return error code
 **/
int vcamera_device_start(int camera_id)
{
	return icamera::camera_device_start(get_physical_id(camera_id));
}

/**
 * Stop device
 *
 * Stop all streams in device.
 *
 * @param camera_id The Caemra ID that opened before
 *
 * @return error code
 **/
int vcamera_device_stop(int camera_id)
{
	return icamera::camera_device_stop(get_physical_id(camera_id));
}

/**
 * Allocate memory for mmap & dma export io-mode
 *
 * @param camera_id The camera ID that opened before
 * @param buffer stream buff
 *
 * @return error code
 **/
int vcamera_device_allocate_memory(int camera_id, camera_buffer_t *buffer)
{
	return icamera::camera_device_allocate_memory(get_physical_id(camera_id), buffer);
}

/**
 * Queue a buffer(or more buffers) to a stream
 *
 * @param camera_id The camera ID that opened before
 * @param buffer The array of pointers to the camera_buffer_t
 * @param num_buffers The number of buffers in the array
 *
 * @return error code
 **/
int vcamera_stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *metadata)
{
	return icamera::camera_stream_qbuf(get_physical_id(camera_id), buffer, num_buffers, NULL);
}

/**
 * Dequeue a buffer from a stream
 *
 * @param camera_id The camera ID that opened before
 * @param stream_id the stream ID that add to device before
 * @param camera_buff stream buff
 *
 * @return error code
 **/
int vcamera_stream_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *metadata)
{
	return icamera::camera_stream_dqbuf(camera_id, stream_id, buffer, NULL);
}

int vcamera_set_parameters(int camera_id, void *metadata)
{
	icamera::Parameters param;

	metadata_to_parameter(metadata, param);

	return icamera::camera_set_parameters(get_physical_id(camera_id), param);
}

int vcamera_get_parameters(int camera_id, void *metadata, int64_t sequence)
{
	int ret;
	icamera::Parameters param;
	ret = icamera::camera_get_parameters(get_physical_id(camera_id), param);
	parameters_to_metadata(param, metadata);

	return ret;
}

int vcamera_get_frame_size(int camera_id, int format, int width, int height, int field, int *bpp)
{
	return icamera::get_frame_size(get_physical_id(camera_id), format, width, height, field, bpp);
}


int vcamera_get_formats_number(int camera_id)
{
	int ret;
	int i;
	camera_info_t tmp;
	stream_array_t config;

	ret = icamera::get_camera_info(get_physical_id(camera_id),tmp);
	if(ret == 0) {
		ret = tmp.capability->getSupportedStreamConfig(config);
		if (ret == 0) {
			return config.size();
		}
	}
	return -1;
}

int vcamera_get_formats(int camera_id, stream_t *p, int *streams_number)
{
	int ret;
	int i;
	camera_info_t tmp;
	stream_array_t config;

	ret = icamera::get_camera_info(get_physical_id(camera_id),tmp);
	if(ret == 0) {
		ret = tmp.capability->getSupportedStreamConfig(config);
		if (ret == 0) {
			for (i = 0; i < config.size(); i++,p++)
				*p = config[i];
			*streams_number = i;
			return 0;
		}
	}
	return -1;
}

camera_callback_ops_t g_callback;
vcamera_notify g_notify;

void event_notify(const camera_callback_ops* cb, const camera_msg_data_t& data){
	g_notify(1234);//TODO, we need update the notify function.
}

void vcamera_callback_register(int camera_id, vcamera_notify callback){
	g_notify = callback;
	g_callback.notify = event_notify;
	icamera::camera_callback_register(get_physical_id(camera_id),  &g_callback);
}

int vcamera_set_exposure(int camera_id, int millisecond)
{
	Parameters param;
	camera_get_parameters(get_physical_id(camera_id), param);

	camera_ae_mode_t aeMode = AE_MODE_MANUAL;
	int64_t expTime = millisecond * 1000;
	param.setAeMode(aeMode);
	param.setExposureTime(expTime);

	return camera_set_parameters(get_physical_id(camera_id), param);
}
