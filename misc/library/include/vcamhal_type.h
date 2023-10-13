/*
 * Copyright (C) 2013 The Android Open Source Project
 * Copyright (C) 2015-2023 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

typedef struct {
	int format; /**< stream format refer to v4l2 definition
	       https://linuxtv.org/downloads/v4l-dvb-apis/pixfmt.html */
	int width;  /**< image width */
	int height; /**< image height */
	int field;
	int stride; /**< stride = aligned bytes per line */
	int size;   /**< real buffer size */

	int id; /**< Id that is filled by HAL. */
	int memType;
	uint32_t max_buffers;

	int usage;      /**<The usage of this stream defined in camera_stream_usage_t. */
	int streamType; /**<The stream type of this stream defined in camera_stream_type_t. */
} stream_t;

typedef struct {
	int num_streams;   /**< number of streams in this configuration */
	stream_t *streams; /**< streams list */
	uint32_t operation_mode;
} stream_config_t;

typedef struct {
	stream_t s;         /**< stream info */
	void *addr;         /**< buffer addr for userptr and mmap memory mode */
	int index;          /**< buffer index, filled by HAL. it is used for qbuf and dqbuf in order */
	int64_t sequence;   /**< buffer sequence, filled by HAL, to record buffer dqueue sequence from
	           device */
	int dmafd;          /**< buffer dmafd for DMA import and export mode */
	int flags;          /**< buffer flags, its type is camera_buffer_flags_t, used to specify buffer
	               properties */
	uint64_t timestamp; /**< buffer timestamp, it's a time reference measured in nanosecond */
	uint32_t requestId; /**< buffer requestId, it's a request id of buffer */
	int reserved;       /**< reserved for future */
} camera_buffer_t;
