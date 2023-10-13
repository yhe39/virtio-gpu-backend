/*
 *
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef __VIRTIO_CAMERA_H__
#define __VIRTIO_CAMERA_H__

#include "vcamhal_type.h"

#define VIRTIO_CAMERA_NUMQ 8
// set the specific ID for frontend to recongnise
#define VIRTIO_TYPE_CAMERA 42 /* virtio camera */
#define MAX_BUFFER_COUNT 10
#define MAX_PIPELINE_NUMBER 4
#define VIRTIO_CAMERA_RINGSZ 64

struct virtio_camera_config {
	__u8 name[256];
	__le32 number_of_virtual_camera;
	__le32 nr_per_virtual_camera[16];
} __attribute__((packed));

struct virtio_vq_related {
	pthread_mutex_t req_mutex;
	pthread_cond_t req_cond;
	int in_process;
};

/*
 * virtio_camera device struct
 */
struct virtio_camera {
	struct virtio_base base;
	struct virtio_vq_info queues[VIRTIO_CAMERA_NUMQ];
	struct virtio_vq_related vq_related[VIRTIO_CAMERA_NUMQ];

	pthread_t vcamera_tid[VIRTIO_CAMERA_NUMQ];
	pthread_mutex_t vcamera_mutex;

	int closing;

	/* socket handle to camera */
	int fd;

	// for camera config r/w
	struct virtio_camera_config config;
};

struct virtio_v4l2_device {
	void *buffer;
	size_t size;
	char *dev_path;
	int fd;
};

struct virtio_media_device {
	char *dev_path;
	int fd;
};

struct virtio_v4l2sub_device {
	char *dev_path;
	int fd;
};

struct virtio_camera_format_size {
	union {
		uint32_t min_width;
		uint32_t width;
	};
	uint32_t max_width;
	uint32_t step_width;

	union {
		uint32_t min_height;
		uint32_t height;
	};
	uint32_t max_height;
	uint32_t step_height;
	uint32_t stride;
	uint32_t sizeimage;
};

struct virtio_camera_req_format {
	uint32_t pixelformat;
	struct virtio_camera_format_size size;
};

struct vcamera_format {
	uint32_t width;
	uint32_t max_width;
	uint32_t step_width;
	uint32_t height;
	uint32_t max_height;
	uint32_t step_height;
	uint32_t stride;
	uint32_t sizeimage;
};

struct picture_format {
	uint32_t pixel_format_type;
	struct vcamera_format camera_format;
};

struct camera_buffer_ref {
	uint32_t segment;
	char uuid[16];
};

struct dma_buf_info {
	int32_t ref_count;
	int dmabuf_fd;
};

typedef enum {
	VIRTIO_CAMERA_GET_FORMAT = 1,
	VIRTIO_CAMERA_SET_FORMAT = 2,
	VIRTIO_CAMERA_TRY_FORMAT = 3,
	VIRTIO_CAMERA_ENUM_FORMAT = 4,
	VIRTIO_CAMERA_ENUM_SIZE = 5,
	VIRTIO_CAMERA_CREATE_BUFFER = 6,
	VIRTIO_CAMERA_DEL_BUFFER = 7,
	VIRTIO_CAMERA_QBUF = 8,
	VIRTIO_CAMERA_STREAM_ON = 9,
	VIRTIO_CAMERA_STREAM_OFF = 10,
	VIRTIO_CAMERA_OPEN = 11,
	VIRTIO_CAMERA_CLOSE = 12,

	VIRTIO_CAMERA_RET_OK = 0x100,

	VIRTIO_CAMERA_RET_UNSPEC = 0x200,
	VIRTIO_CAMERA_RET_BUSY = 0x201,
	VIRTIO_CAMERA_RET_OUT_OF_MEMORY = 0x202,
	VIRTIO_CAMERA_RET_INVALID = 0x203,
} virtio_camera_request_type;

struct virtio_camera_request {
	virtio_camera_request_type type;
	int index; // zw

	union {
		struct picture_format format;
		struct camera_buffer_ref buffer;
		char reserve[24];
	} u;
};

struct capture_buffer {
	char uuid[16];
	uint32_t segment;
	struct iovec *iov;
	int dmabuf_fd;
	char *remapped_addr;
	int length;
	STAILQ_ENTRY(capture_buffer) link;
	uint16_t idx;
	camera_buffer_t buffer;
	struct virtio_camera_request *response;
	char reserve[2];
};

struct camera_ops {
	int (*get_camera_info)(int camera_id, void *data);

	int (*open)(int camera_id);
	void (*close)(int camera_id);

	int (*allocate_memory)(int camera_id, camera_buffer_t *buffer);

	int (*config_streams)(int camera_id, stream_config_t *stream_list);
	int (*start_stream)(int camera_id);
	int (*stop_stream)(int camera_id);

	int (*stream_qbuf)(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings);
	int (*stream_dqbuf)(int camera_id, int stream_id, camera_buffer_t **buffer, void *settings);

	int (*hal_init)();
	int (*hal_deinit)();
	int (*config_sensor_input)(int camera_id, const stream_t *inputConfig);
	int (*get_frame_size)(int camera_id, int format, int width, int height, int field, int *bpp);
	int (*set_exposure)(int camera_id, int date);
	int (*set_parameters)(int camera_id, void *data);
	int (*get_parameters)(int camera_id, void *data, int64_t sequence);
	int (*req_bufs)(int camera_id);
	int (*get_formats_number)(int camera_id);
	int (*get_formats)(int camera_id, stream_t* p, int* streams_number);
};

typedef enum _type {
	V4L2_INTERFACE = 0,
	HAL_INTERFACE = 1,
} interface_type;

struct camera_dev {
	int id;
	int fd;
	char name[10];
	interface_type type;
	struct camera_ops ops;

	stream_config_t supported_stream_list;
	stream_config_t stream_list;
	stream_t streams[1];
	int stream_state;
	uint8_t buffer_count;
	struct capture_buffer capture_buffers[MAX_BUFFER_COUNT];
	STAILQ_HEAD(, capture_buffer) capture_list;
	pthread_mutex_t capture_list_mutex;
	pthread_t vtid;
};

struct camera_info {
	int camera_id;
	struct virtio_camera *vcamera;
};
#endif //__VIRTIO_CAMERA_H__
