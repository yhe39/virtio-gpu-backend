/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#pragma once

#include <stdlib.h> // For including definition of NULL
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vcamera_notify)(int data);
/**
 * @brief
 *   Get number of camera
 *
 * @note
 *   This allows user to get the number of camera without init or open camera device.
 *
 * @return
 *   >0 the number of camera
 * @return
 *   <= 0 failed to get camera number
 *
 * @par Sample code:
 *
 * @code
 *   int num = vcamera_get_number_of_cameras();
 * @endcode
 **/
int vcamera_get_number_of_cameras();

/**
 * @struct vcamera_info_t: Define each camera basic information
 */
typedef struct {
    int facing;
    int orientation;
    int device_version;
    const char* name;             /**< Sensor name */
    const char* description;      /**< Sensor description */
    void* metadata;
} vcamera_info_t;

/**
 * @brief
 *   Get camera info including camera capability.
 *
 * @note
 *   It can be called before hal init
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[out]
 *   vcamera_info_t info: Camera info filled by libvcamhal
 *
 * @return
 *   0 succeed to get camera info
 * @return
 *   <0 error code, failed to get camera info
 *
 * @par Sample code
 *
 * @code
 *   int camera_id = 0;
 *   vcamera_info_t *info;
 *   vcamera_get_camera_info(camera_id, info);
 *
 * @endcode
 *
 **/
int vcamera_get_camera_info(int camera_id, vcamera_info_t *info);

/**
 * @brief
 *   Initialize camera HAL
 *
 * @return
 *   0 succeed to init camera HAL
 * @return
 *   <0 error code, failed to init camera HAL
 *
 * @par Sample code:
 *
 * @code
 *   int ret = vcamera_hal_init();
 * @endcode
 **/
int vcamera_hal_init();

/**
 * @brief
 *   De-initialize camera HAL
 *
 * @return
 *   0 succeed to deinit camera HAL
 * @return
 *   <0 error code, failed to deinit camera device
 *
 * @par Sample code:
 *
 * @code
 *   int ret = vcamera_hal_deinit();
 * @endcode
 **/
int vcamera_hal_deinit();

/**
 * @brief
 *   Open camera device by camera ID
 *
 * @param[in]
 *   int camera_id: ID of the camera
 *
 * @return
 *   0 succeed to open camera device
 * @return
 *   <0 error code, failed to open camera device
 *
 * @par Sample code:
 *
 * @code
 *   int camera_id = 0;
 *   int ret = vcamera_device_open(camera_id);
 * @endcode
 **/
int vcamera_device_open(int camera_id);

/**
 * @brief
 *   Close camera device by camera ID
 *
 * @param[in]
 *   int camera_id: ID of the camera
 *
 * @par Sample code:
 *
 * @code
 *   int camera_id = 0;
 *   int ret = vcamera_device_open(camera_id);
 *   vcamera_device_close(camera_id);
 * @endcode
 **/
void vcamera_device_close(int camera_id);

/**
 * @brief
 *   Configure sensor input of camera device, it is not allowed to call this when camera is started.
 *   Optional call.
 *
 * @note
 *   1. To re-configure sensor input, camera device must be stopped first.
 *   2. The new sensor configuration will overwrite the previous config.
 *   3. The new "inputConfig" will be used for all the future operation until the device is closed.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   int inputConfig: Specify which input format, resolution(the output of ISYS) should be used.
 *
 * @return
 *   0 succeed to configure streams
 * @return
 *   <0 error code, failed to configure stream
 *
 * @par Sample code:
 *
 * @code
 *   int camera_id = 0;
 *   stream_t input_config;
 *   CLEAR(input_config);
 *   input_config.format = V4L2_PIX_FMT_SGRBG8V32;
 *   ret = vcamera_device_config_sensor_input(camera_id, &input_config);
 * @endcode
 **/
int vcamera_device_config_sensor_input(int camera_id, const stream_t *inputConfig);

/**
 * @brief
 *   Configure streams to camera device, it is not allowed to call this when camera is started.
 *
 * @note
 *   1. To re-configure streams, camera device must be stopped first.
 *   2. The new streams configuration will overwrite the previous streams.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   stream_config_t stream_list: stream configuration list, if success, stream id is filled in
 *								streams[]
 *
 * @return
 *   0 succeed to configure streams
 * @return
 *   <0 error code, failed to configure stream
 *
 * @par Sample code:
 *
 * @code
 *   int camera_id = 0;
 *   stream_config_t stream_list;
 *   stream_t streams[1];
 *   streams[0].format = V4L2_PIX_FMT_SGRBG8;
 *   streams[0].width = 1920;
 *   streams[0].height = 1080;
 *   streams[0].memType = V4L2_MEMORY_USERPTR;
 *   stream_list.num_streams = 1;
 *   stream_list.streams = streams;
 *   ret = vcamera_device_config_streams(camera_id, &stream_list);
 * @endcode
 **/
int vcamera_device_config_streams(int camera_id, stream_config_t *stream_list);

/**
 * @brief
 *   Start camera device
 *
 * @param[in]
 *   int camera_id: ID of the camera
 *
 * @return
 *   0 succeed to start device
 * @return
 *   <0 error code, failed to start device
 *
 * @par Sample code:
 *
 * @code
 *   int camera_id=0;
 *   stream_config_t stream_list;
 *   ...
 *   ret = vcamera_device_config_streams(camera_id, &stream_list);
 *   ret = vcamera_device_start(camera_id);
 *   ... ...
 *   ret = vcamera_device_stop(camera_id);
 * @endcode
 *
 **/
int vcamera_device_start(int camera_id);

/**
 * @brief
 *   Stop camera device.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 *
 * @return
 *   0 succeed to stop device
 * @return
 *   <0 error code, failed to stop device
 *
 * @see camera_device_start()
 **/
int vcamera_device_stop(int camera_id);

/**
 * @brief
 *   Allocate memory for mmap & dma export io-mode
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[out]
 *   camera_buffer_t buffer: in Mmap mode, mmaped address is filled in addr;
 *   in DMA export mode, the dma fd is flled in dmafd.
 *
 * @return
 *   0 succeed to allocate memory
 * @return
 *   <0 error code, failed to allocate memory
 *
 * @par Sample code:
 *
 * @code
 *   camera_buffer_t *buffers = (camera_buffer_t *)malloc(sizeof(camera_buffer_t)*buffer_count);
 *   camera_buffer_t *buf = buffers;
 *   buf.s = stream;
 *   for (int i = 0; i < buffer_count; i++, buf++) {
 *	 camera_device_allocate_memory(camera_id, buf):
 *   }
 *
 *   buf = buffers;
 *   for (int i = 0; i < buffer_count; i++, buf++) {
 *	   camera_stream_qbuf(camera_id, &buf);
 *   }
 *
 *   camera_device_start(camera_id);
 * @endcode
 *
 */
int vcamera_device_allocate_memory(int camera_id, camera_buffer_t *buffer);

/**
 * @brief
 *   Queue one or serveral buffers to the camera device
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   camera_buffer_t buffer: array of pointers to camera_buffer_t
 *   buffer[i]->s MUST be filled before calling this API.
 * @param[in]
 *   int num_buffers: indicates how many buffers are in the buffer pointer array,
 *					and these buffers MUST be for different streams. Stream id is
 *					filled and give back to app when camera_device_config_streams()
 *					is called, HAL will do the mapping when parsing queued buffers
 *					according to num_buffers.
 * @param[in]
 *   void *metadata: metadata used for this group of buffers.
 *						This is used for per-frame setting, which means the metadata should be
 *						applied for the group of buffers.
 *
 * @return
 *   0 succeed to queue buffers
 * @return
 *   <0 error code, failed to queue buffers
 *
 * @see vcamera_stream_qbuf();
 **/
int vcamera_stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *metadata);

/**
 * @brief
 *   Dequeue a buffer from device per stream id.
 *
 * @note
 *   It's a block function, that means the caller will be blocked until buffer is ready.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   int stream_id: ID of stream
 * @param[out]
 *   camera_buffer_t buffer: buffer dqueued from device
 * @param[out]
 *   void *metadata: metadata used for this buffer.
 *
 * @return
 *   0 succeed to dqueue buffer
 * @return
 *   <0 error code, failed to dqueue buffer
 *
 **/
int vcamera_stream_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *metadata);
;
/**
 * @brief
 *   Set a set of parameters to the gaven camera device.
 *
 * @note
 *   It MUST be called after device opened, otherwise error will be returned.
 *   Which buffer the paramters takes effect in is not guaranteed.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   void *metadata: A set of metadata.
 *
 * @return
 *   0 succeed to set camera parameters
 * @return
 *   <0 error code, failed to set camera parameters
 *
 *
 **/
int vcamera_set_parameters(int camera_id, void *metadata);

/**
 * @brief
 *   Get parameter from the gaven camera device.
 *
 * @note
 *   It MUST be called after device opened, otherwise error will be returned.
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[out]
 *   void *metadata:  metadata need to be filled in
 * @param[in]
 *   int64_t sequence: sequence used to find target parameter and results, default is -1
 *
 * @return
 *   0 succeed to get camera parameters
 * @return
 *   <0 error code, failed to get camera parameters
 *
 **/
int vcamera_get_parameters(int camera_id, void *metadata, int64_t sequence);

/**************************************Optional API ******************************
 * The API defined in this section is optional.
 */

/**
 * @brief
 *   Return the size information of a frame.
 *
 * @note
 *   It is used to assist the test cases to double confirm the final buffer size
 *
 * @param[in]
 *   int camera_id: The camera device index
 * @param[in]
 *   int format: The v4l2 format of the frame
 * @param[in]
 *   int width: The width of the frame
 * @param[in]
 *   int height: The height of the frame
 * @param[in]
 *   int field: The interlace field of the frame
 * @param[out]
 *   int bpp: The bpp of the format
 *
 * @return
 *   frame size.
 **/
int vcamera_get_frame_size(int camera_id, int format, int width, int height, int field, int *bpp);

/**
 * @brief
 *   Register callback function
 *
 * @param[in]
 *   int camera_id: ID of the camera
 * @param[in]
 *   vcamera_notify  callback: callback handle
 *
 **/
void vcamera_callback_register(int camera_id, vcamera_notify callback);

/**
 * @brief
 *   set the exposure of one camera.
 *
 * @param[in]
 *   int camera_id: The camera device index.
 * @param [in]
 *   int millisecond: The time of exposure time.
 *
 * @return
 *   frame size.
 **/
int vcamera_set_exposure(int camera_id, int millisecond);

/**
 * @brief
 *   get supported formats number
 *
 * @param[in]
 *   int camera_id: The camera device index.
 * @return
 *   >0  success and return the formats number
 *   <=0 failed to get formats number
 **/
int vcamera_get_formats_number(int camera_id);

/**
 * @brief
 *   get all supported formats
 *
 * @param[in]
 *   int camera_id: The camera device index.
 * @param [out]
 *   stream_t* p: The addres of stream_t array.
 * @param [out]
 *   int* streams_number: The number of supported formats
 * @return
 *   <0 error code, failed to get formats
 **/
int vcamera_get_formats(int camera_id, stream_t* p, int* streams_number);

#ifdef __cplusplus
}
#endif
