/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "ICamera.h"

using namespace icamera;

int main(int argc,char** argv)
{
	int ret;
	int camera_id = 0;
	int buffer_count = 6;
	int num_streams = 2;
	int format = V4L2_PIX_FMT_NV12;
	stream_config_t stream_list;
	stream_t streams[2];

	ret = camera_hal_init();
	printf("camera_test camera_hal_init ret = %d\n", ret);

	ret = camera_device_open(camera_id);
	printf("camera_test camera_device_open ret = %d\n", ret);
	camera_info_t info;
	ret = get_camera_info(camera_id, info);
	printf("camera_test get_camera_info ret = %d info: %s\n", ret, info.description);

	stream_t input_config;
	memset(&input_config, 0, sizeof(stream_t));

	Parameters param;
	camera_get_parameters(camera_id, param);

	camera_ae_mode_t aeMode = AE_MODE_MANUAL;
	int64_t expTime = 20 * 1000;
	param.setAeMode(aeMode);
	param.setExposureTime(expTime);

	camera_set_parameters(camera_id, param);

	input_config.format = -1;

	ret = camera_device_config_sensor_input(camera_id, &input_config);
	printf("camera_test camera_device_config_sensor_input ret = %d\n", ret);


	int bpp = 0;
	int buffer_size_720p = get_frame_size(camera_id, format, 1280, 720, V4L2_FIELD_ANY, &bpp);
	int buffer_size_1080p = get_frame_size(camera_id, format, 1920, 1080, V4L2_FIELD_ANY, &bpp);

	memset(&streams, 0, sizeof(streams));

	streams[0].format = format;
	streams[1].format = format;

	streams[0].width = 1280;
	streams[0].height = 720;
	streams[0].memType = V4L2_MEMORY_USERPTR;
	streams[0].field = 0;

	streams[0].size = buffer_size_720p;
	streams[0].stride = 2560;

	streams[1].width = 1920;
	streams[1].height = 1080;
	streams[1].memType = V4L2_MEMORY_USERPTR;
	streams[1].field = 0;
	streams[1].size = buffer_size_1080p;
	streams[1].stride = 1920*2;

	stream_list.num_streams = num_streams;
	stream_list.streams = streams;
	stream_list.operation_mode = 2;
	ret = camera_device_config_streams(camera_id, &stream_list);

	printf("camera_test camera_device_config_streams ret = %d\n", ret);
	printf("camera_test camera_device_config_streams streams[0].id = %d\n", streams[0].id);
	printf("camera_test camera_device_config_streams streams[1].id = %d\n", streams[1].id);

	camera_buffer_t *buffers = (camera_buffer_t *)malloc(sizeof(camera_buffer_t) * buffer_count * num_streams);

	memset(buffers, 0, sizeof(camera_buffer_t) * buffer_count * num_streams);

	printf("camera_test buffer_size_720p = %d bpp %d\n", buffer_size_720p, bpp);
	printf("camera_test buffer_size_1080p = %d bpp %d\n", buffer_size_1080p, bpp);

	camera_buffer_t *buf = buffers;
	camera_buffer_t* p[2];

	for (int i = 0; i < buffer_count; i++, buf++) {
		p[0] = buf;
		buf->s = streams[0];
		posix_memalign(&buf->addr, getpagesize(), buffer_size_720p);

		p[1] = ++buf;
		buf->s = streams[1];
		posix_memalign(&buf->addr, getpagesize(), buffer_size_1080p);

		ret = camera_stream_qbuf(camera_id, &p[0], 2);
		printf("camera_test camera_stream_qbuf ret = %d getpagesize() %d\n", ret, getpagesize());
	}

	ret = camera_device_start(camera_id);
	printf("camera_test camera_device_start ret = %d\n", ret);

	int stream_id = 0;
	camera_buffer_t buffer2;
	buf = buffers;
	for (int i = 0; i < 1000; i++) {
		stream_id = 0;
		printf("camera_test  call camera_stream_dqbuf stream0 1280 * 721 i = %d\n", i);
		ret = camera_stream_dqbuf(camera_id, stream_id, &buf);
		if (i == 10) {
			FILE *fp = fopen("yuv_720p.yuv", "w");
			fwrite(buf->addr, 1, buffer_size_720p, fp);
			fclose(fp);
		}

		printf("camera_test camera_stream_dqbuf stream0 ret = %d buf->index %d %p\n", ret, buf->index, buf->addr);
		buf->sequence = -1;
		buf->timestamp = 0;
		ret = camera_stream_qbuf(camera_id, &buf);
		// processing data with buf
		printf("camera_test camera_stream_qbuf stream0 ret = %d\n", ret);

		stream_id = 1;
		printf("camera_test call camera_stream_dqbuf stream1 1280 * 721 i = %d\n", i);
		ret = camera_stream_dqbuf(camera_id, stream_id, &buf);
		if (i == 10) {
			FILE *fp = fopen("yuv_1080p.yuv", "w");
			fwrite(buf->addr, 1, buffer_size_1080p, fp);
			fclose(fp);
		}

		printf("camera_test camera_stream_dqbuf stream1 ret = %d buf->index %d %p\n", ret, buf->index, buf->addr);
		buf->sequence = -1;
		buf->timestamp = 0;
		ret = camera_stream_qbuf(camera_id, &buf);
		// processing data with buf
		printf("camera_test camera_stream_qbuf stream1 ret = %d\n", ret);
	}

	camera_device_stop(camera_id);
	camera_device_close(camera_id);
}
