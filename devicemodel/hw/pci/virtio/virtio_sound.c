/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/*
 * virtio sound
 * audio mediator device model
 */

#include <err.h>
#include <errno.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sysexits.h>

#include <alsa/asoundlib.h>
#include <alsa/control.h>
#include <sys/queue.h>

#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "virtio_sound.h"
#include "log.h"

#define VIRTIO_SOUND_RINGSZ	256
#define VIRTIO_SOUND_VQ_NUM	4

/*
 * Host capabilities
 */
#define VIRTIO_SND_S_HOSTCAPS		((1UL << VIRTIO_F_VERSION_1) | (1UL << VIRTIO_SND_F_CTLS))

#define VIRTIO_SOUND_CTL_SEGS	8
#define VIRTIO_SOUND_EVENT_SEGS	2
#define VIRTIO_SOUND_XFER_SEGS	4

#define VIRTIO_SOUND_CARD	4
#define VIRTIO_SOUND_STREAMS	4
#define VIRTIO_SOUND_CTLS	128
#define VIRTIO_SOUND_JACKS	64
#define VIRTIO_SOUND_CHMAPS	64

#define VIRTIO_SOUND_CARD_NAME	64
#define VIRTIO_SOUND_DEVICE_NAME	64
#define VIRTIO_SOUND_IDENTIFIER	128

#define VIRTIO_TLV_SIZE	1024

#define HDA_JACK_LINE_OUT	0
#define HDA_JACK_SPEAKER	1
#define HDA_JACK_HP_OUT	2
#define HDA_JACK_CD	3
#define HDA_JACK_SPDIF_OUT	4
#define HDA_JACK_DIG_OTHER_OUT	5
#define HDA_JACK_LINE_IN	8
#define HDA_JACK_AUX 9
#define HDA_JACK_MIC_IN	10
#define HDA_JACK_SPDIF_IN	12
#define HDA_JACK_DIG_OTHER_IN	13
#define HDA_JACK_OTHER	0xf

#define HDA_JACK_LOCATION_INTERNAL	0x00
#define HDA_JACK_LOCATION_EXTERNAL	0x01
#define HDA_JACK_LOCATION_SEPARATE	0x02

#define HDA_JACK_LOCATION_NONE	0
#define HDA_JACK_LOCATION_REAR	1
#define HDA_JACK_LOCATION_FRONT	2

#define HDA_JACK_LOCATION_HDMI	0x18

#define HDA_JACK_DEFREG_DEVICE_SHIFT	20
#define HDA_JACK_DEFREG_LOCATION_SHIFT	24

#define VIRTIO_SND_START_SHERHOLD 2

#define WPRINTF(format, arg...) pr_err(format, ##arg)

enum {
	VIRTIO_SND_BE_INITED = 1,
	VIRTIO_SND_BE_PRE,
	VIRTIO_SND_BE_PENDING,
	VIRTIO_SND_BE_START,
	VIRTIO_SND_BE_STOP,
	VIRTIO_SND_BE_RELEASE,
	VIRTIO_SND_BE_DEINITED,
};
struct virtio_sound_pcm_param {
	uint32_t features;
	uint64_t formats;
	uint64_t rates;

	uint8_t channels_min;
	uint8_t channels_max;

	uint32_t buffer_bytes;
	uint32_t period_bytes;
	uint8_t	channels;
	uint8_t	format;
	uint8_t rate;

	uint32_t rrate;
};

struct virtio_sound_msg_node {
	STAILQ_ENTRY(virtio_sound_msg_node) link;
	struct iovec *iov;
	struct virtio_vq_info *vq;
	int cnt;
	uint16_t idx;
};

struct virtio_sound_chmap {
	uint8_t channels;
	uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
};

struct virtio_sound_pcm {
	snd_pcm_t *handle;
	int hda_fn_nid;
	int dir;
	int status;
	pthread_mutex_t ctl_mtx;
	int xfer_iov_cnt;
	int id;


	pthread_t tid;
	struct pollfd *poll_fd;
	unsigned int pfd_count;

	char dev_name[VIRTIO_SOUND_DEVICE_NAME];
	struct virtio_sound_pcm_param param;
	STAILQ_HEAD(, virtio_sound_msg_node) head;
	pthread_mutex_t mtx;

	struct virtio_sound_chmap *chmaps[VIRTIO_SOUND_CHMAPS];
	uint32_t chmap_cnt;
};

struct vbs_ctl_elem {
	snd_hctl_elem_t *elem;
	struct vbs_card *card;
};

struct vbs_jack_elem {
	snd_hctl_elem_t *elem;
	uint32_t hda_reg_defconf;
	uint32_t connected;
	struct vbs_card *card;
};

struct vbs_card {
	char card[VIRTIO_SOUND_CARD_NAME];
	snd_hctl_t *handle;
	int count;
	int start;
};

/*dev struct*/
struct virtio_sound {
	struct virtio_base base;
	struct virtio_vq_info vq[VIRTIO_SOUND_VQ_NUM];
	pthread_mutex_t mtx;
	struct virtio_snd_config snd_cfg;
	uint64_t	features;

	struct virtio_sound_pcm *streams[VIRTIO_SOUND_STREAMS];
	int stream_cnt;
	int chmap_cnt;

	struct vbs_ctl_elem *ctls[VIRTIO_SOUND_CTLS];
	int ctl_cnt;

	struct vbs_jack_elem *jacks[VIRTIO_SOUND_JACKS];
	int jack_cnt;

	struct vbs_card *cards[VIRTIO_SOUND_CARD];
	int card_cnt;

	int max_tx_iov_cnt;
	int max_rx_iov_cnt;
	int status;
};

static int virtio_sound_cfgread(void *vdev, int offset, int size, uint32_t *retval);
static int virtio_sound_send_event(struct virtio_sound *virt_snd, struct virtio_snd_event *event);
static int virtio_sound_event_callback(snd_hctl_elem_t *helem, unsigned int mask);
static void virtio_sound_reset(void *vdev);
static int virtio_sound_create_pcm_thread(struct virtio_sound_pcm *stream);
static int virtio_sound_xfer(struct virtio_sound_pcm *stream);

static struct virtio_sound *vsound = NULL;

static struct virtio_ops virtio_snd_ops = {
	"virtio_sound",		/* our name */
	VIRTIO_SOUND_VQ_NUM,	/* we support 4 virtqueue */
	sizeof(struct virtio_snd_config),			/* config reg size */
	virtio_sound_reset,	/* reset */
	NULL,	/* device-wide qnotify */
	virtio_sound_cfgread,				/* read virtio config */
	NULL,				/* write virtio config */
	NULL,				/* apply negotiated features */
	NULL,   /* called on guest set status */
};

/*
 * This array should be added as the same order
 * as enum of VIRTIO_SND_PCM_FMT_XXX.
 */
static const snd_pcm_format_t virtio_sound_v2s_format[] = {
	SND_PCM_FORMAT_IMA_ADPCM,
	SND_PCM_FORMAT_MU_LAW,
	SND_PCM_FORMAT_A_LAW,
	SND_PCM_FORMAT_S8,
	SND_PCM_FORMAT_U8,
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_U16_LE,
	SND_PCM_FORMAT_S18_3LE,
	SND_PCM_FORMAT_U18_3LE,
	SND_PCM_FORMAT_S20_3LE,
	SND_PCM_FORMAT_U20_3LE,
	SND_PCM_FORMAT_S24_3LE,
	SND_PCM_FORMAT_U24_3LE,
	SND_PCM_FORMAT_S20_LE,
	SND_PCM_FORMAT_U20_LE,
	SND_PCM_FORMAT_S24_LE,
	SND_PCM_FORMAT_U24_LE,
	SND_PCM_FORMAT_S32_LE,
	SND_PCM_FORMAT_U32_LE,
	SND_PCM_FORMAT_FLOAT_LE,
	SND_PCM_FORMAT_FLOAT64_LE,
	SND_PCM_FORMAT_DSD_U8,
	SND_PCM_FORMAT_DSD_U16_LE,
	SND_PCM_FORMAT_DSD_U32_LE,
	SND_PCM_FORMAT_IEC958_SUBFRAME_LE
};

/*
 * This array should be added as the same order
 * as enum of VIRTIO_SND_PCM_RATE_XXX.
 */
static const uint32_t virtio_sound_t_rate[] = {
	5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000
};

static const int32_t virtio_sound_s2v_type[] = {
	-1,
	VIRTIO_SND_CTL_TYPE_BOOLEAN,
	VIRTIO_SND_CTL_TYPE_INTEGER,
	VIRTIO_SND_CTL_TYPE_ENUMERATED,
	VIRTIO_SND_CTL_TYPE_BYTES,
	VIRTIO_SND_CTL_TYPE_IEC958,
	VIRTIO_SND_CTL_TYPE_INTEGER64
};

static const uint8_t virtio_sound_s2v_chmap[] = {
	VIRTIO_SND_CHMAP_NONE,
	VIRTIO_SND_CHMAP_NA,
	VIRTIO_SND_CHMAP_MONO,
	VIRTIO_SND_CHMAP_FL,
	VIRTIO_SND_CHMAP_FR,
	VIRTIO_SND_CHMAP_RL,
	VIRTIO_SND_CHMAP_RR,
	VIRTIO_SND_CHMAP_FC,
	VIRTIO_SND_CHMAP_LFE,
	VIRTIO_SND_CHMAP_SL,
	VIRTIO_SND_CHMAP_SR,
	VIRTIO_SND_CHMAP_RC,
	VIRTIO_SND_CHMAP_FLC,
	VIRTIO_SND_CHMAP_FRC,
	VIRTIO_SND_CHMAP_RLC,
	VIRTIO_SND_CHMAP_RRC,
	VIRTIO_SND_CHMAP_FLW,
	VIRTIO_SND_CHMAP_FRW,
	VIRTIO_SND_CHMAP_FLH,
	VIRTIO_SND_CHMAP_FCH,
	VIRTIO_SND_CHMAP_FRH,
	VIRTIO_SND_CHMAP_TC,
	VIRTIO_SND_CHMAP_TFL,
	VIRTIO_SND_CHMAP_TFR,
	VIRTIO_SND_CHMAP_TFC,
	VIRTIO_SND_CHMAP_TRL,
	VIRTIO_SND_CHMAP_TRR,
	VIRTIO_SND_CHMAP_TRC,
	VIRTIO_SND_CHMAP_TFLC,
	VIRTIO_SND_CHMAP_TFRC,
	VIRTIO_SND_CHMAP_TSL,
	VIRTIO_SND_CHMAP_TSR,
	VIRTIO_SND_CHMAP_LLFE,
	VIRTIO_SND_CHMAP_RLFE,
	VIRTIO_SND_CHMAP_BC,
	VIRTIO_SND_CHMAP_BLC,
	VIRTIO_SND_CHMAP_BRC
};

static inline int
virtio_sound_get_frame_size(struct virtio_sound_pcm *stream)
{
	return snd_pcm_format_physical_width(virtio_sound_v2s_format[stream->param.format]) / 8
		* stream->param.channels;
}

static int
virtio_sound_cfgread(void *vdev, int offset, int size, uint32_t *retval)
{
	struct virtio_sound *virt_snd = vdev;
	void* ptr;
	ptr = (uint8_t *)&virt_snd->snd_cfg + offset;
	memcpy(retval, ptr, size);
	return 0;
}

static void
virtio_sound_reset(void *vdev)
{
	struct virtio_sound *virt_snd = vdev;
	/* now reset rings, MSI-X vectors, and negotiated capabilities */
	virtio_reset_dev(&virt_snd->base);
}

static struct virtio_sound *virtio_sound_get_device()
{
	return vsound;
}

static void
virtio_sound_notify_xfer(struct virtio_sound *virt_snd, struct virtio_vq_info *vq, int iov_cnt)
{
	struct virtio_sound_msg_node *msg_node;
	struct virtio_snd_pcm_xfer *xfer_hdr;
	int n, s = -1, i;

	while (vq_has_descs(vq)) {
		msg_node = malloc(sizeof(struct virtio_sound_msg_node));
		if (msg_node == NULL) {
			WPRINTF("%s: malloc data node fail!\n", __func__);
			return;
		}
		msg_node->iov = malloc(sizeof(struct iovec) * iov_cnt);
		if (msg_node->iov == NULL) {
			WPRINTF("%s: malloc iov nodes fail!\n", __func__);
			free(msg_node);
			return;
		}
		n = vq_getchain(vq, &msg_node->idx, msg_node->iov, iov_cnt, NULL);
		if (n <= 0) {
			WPRINTF("%s: fail to getchain!\n", __func__);
			free(msg_node->iov);
			free(msg_node);
			return;
		}
		msg_node->cnt = n;
		msg_node->vq = vq;

		xfer_hdr = (struct virtio_snd_pcm_xfer *)msg_node->iov[0].iov_base;
		s = xfer_hdr->stream_id;

		pthread_mutex_lock(&virt_snd->streams[s]->mtx);
		if (STAILQ_EMPTY(&virt_snd->streams[s]->head)) {
			STAILQ_INSERT_HEAD(&virt_snd->streams[s]->head, msg_node, link);
		} else {
			STAILQ_INSERT_TAIL(&virt_snd->streams[s]->head, msg_node, link);
		}
		pthread_mutex_unlock(&virt_snd->streams[s]->mtx);
	}
	if (s >= 0) {
		pthread_mutex_lock(&virt_snd->streams[s]->ctl_mtx);
		if (virt_snd->streams[s]->status == VIRTIO_SND_BE_PENDING) {
			for (i = 0; i < 2; i++) {
				if (virtio_sound_xfer(virt_snd->streams[s]) < 0) {
					pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
					WPRINTF("%s: stream fn_id %d xfer error!\n", __func__,
						virt_snd->streams[s]->hda_fn_nid);
				}
			}
			if (snd_pcm_start(virt_snd->streams[s]->handle) < 0) {
				pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
				WPRINTF("%s: stream %s start error!\n", __func__, virt_snd->streams[s]->dev_name);
			}
			virt_snd->streams[s]->status = VIRTIO_SND_BE_START;
			if (virtio_sound_create_pcm_thread(virt_snd->streams[s]) < 0) {
				pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
				WPRINTF("%s: create thread fail!\n", __func__);
			}
		}
		pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
	}
}

static void
virtio_sound_notify_tx(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	virtio_sound_notify_xfer(virt_snd, vq, virt_snd->max_tx_iov_cnt);
}

static void
virtio_sound_notify_rx(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	virtio_sound_notify_xfer(virt_snd, vq, virt_snd->max_rx_iov_cnt);
}

static int
virtio_sound_set_hwparam(struct virtio_sound_pcm *stream)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_uframes_t buffer_size, period_size;
	int dir = stream->dir;
	int err;

	snd_pcm_hw_params_alloca(&hwparams);
	err = snd_pcm_hw_params_any(stream->handle, hwparams);
	if(err < 0) {
		WPRINTF("%s: no configurations available, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_hw_params_set_access(stream->handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	if(err < 0) {
		WPRINTF("%s: set access, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_hw_params_set_format(stream->handle, hwparams,
		virtio_sound_v2s_format[stream->param.format]);
	if(err < 0) {
		WPRINTF("%s: set format(%d), error number %d!\n", __func__,
			virtio_sound_v2s_format[stream->param.format], err);
		return -1;
	}
	err = snd_pcm_hw_params_set_channels(stream->handle,
		hwparams, stream->param.channels);
	if(err < 0) {
		WPRINTF("%s: set channels(%d) fail, error number %d!\n", __func__,
			stream->param.channels, err);
		return -1;
	}
	stream->param.rrate = virtio_sound_t_rate[stream->param.rate];
	err = snd_pcm_hw_params_set_rate_near(stream->handle, hwparams, &stream->param.rrate, &dir);
	if(err < 0) {
		WPRINTF("%s: set rate(%u) fail, error number %d!\n", __func__,
			virtio_sound_t_rate[stream->param.rate], err);
		return -1;
	}
	buffer_size = stream->param.buffer_bytes / virtio_sound_get_frame_size(stream);
	err = snd_pcm_hw_params_set_buffer_size(stream->handle, hwparams, buffer_size);
	if(err < 0) {
		WPRINTF("%s: set buffer_size(%ld) fail, error number %d!\n", __func__, buffer_size, err);
		return -1;
	}
	period_size = stream->param.period_bytes / virtio_sound_get_frame_size(stream);
	dir = stream->dir;
	err = snd_pcm_hw_params_set_period_size_near(stream->handle, hwparams,
		&period_size, &dir);
	if(err < 0) {
		WPRINTF("%s: set period_size(%ld) fail, error number %d!\n", __func__, period_size, err);
		return -1;
	}
	err = snd_pcm_hw_params(stream->handle, hwparams);
	if (err < 0) {
		WPRINTF("%s: set hw params fail, error number %d!\n", __func__, err);
		return -1;
	}
	return 0;
}


static int
virtio_sound_set_swparam(struct virtio_sound_pcm *stream)
{
	snd_pcm_sw_params_t *swparams;
	snd_pcm_format_t period_size;
	int err;

	snd_pcm_sw_params_alloca(&swparams);
	err = snd_pcm_sw_params_current(stream->handle, swparams);
	if(err < 0) {
		WPRINTF("%s: no sw params available, error number %d!\n", __func__, err);
		return -1;
	}

	err = snd_pcm_sw_params_set_start_threshold(stream->handle, swparams, 1);
	if(err < 0) {
		WPRINTF("%s: set threshold fail, error number %d!\n", __func__, err);
		return -1;
	}
	period_size = stream->param.period_bytes / virtio_sound_get_frame_size(stream);
	err = snd_pcm_sw_params_set_avail_min(stream->handle, swparams, period_size);
	if(err < 0) {
		WPRINTF("%s: set avail min fail, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_sw_params_set_period_event(stream->handle, swparams, 1);
	if(err < 0) {
		WPRINTF("%s: set period event fail, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_sw_params(stream->handle, swparams);
	if (err < 0) {
		WPRINTF("%s: set sw params fail, error number %d!\n", __func__, err);
		return -1;
	}
	return 0;
}

static int
virtio_sound_recover(struct virtio_sound_pcm *stream)
{
	snd_pcm_state_t state = snd_pcm_state(stream->handle);
	struct virtio_snd_event event;
	int err = -1, i;

	if (state == SND_PCM_STATE_XRUN) {
		event.hdr.code = VIRTIO_SND_EVT_PCM_XRUN;
		event.data = stream->id;
		virtio_sound_send_event(virtio_sound_get_device(), &event);
	}
	if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SETUP) {
		err = snd_pcm_prepare(stream->handle);
		if(err < 0) {
			WPRINTF("%s: recorver from xrun prepare fail, error number %d!\n", __func__, err);
			return -1;
		}
		err = snd_pcm_start(stream->handle);
		if(err < 0) {
			WPRINTF("%s: recorver from xrun start fail, error number %d!\n", __func__, err);
			return -1;
		}
	} else if (state == SND_PCM_STATE_SUSPENDED) {
		for (i = 0; i < 10; i++) {
			err = snd_pcm_resume(stream->handle);
			if (err == -EAGAIN) {
				WPRINTF("%s: waiting for resume!\n", __func__);
				usleep(5000);
				continue;
			}
			err = snd_pcm_prepare(stream->handle);
			if(err < 0) {
				WPRINTF("%s: recorver form suspend prepare fail, error number %d!\n", __func__, err);
				return -1;
			}
			err = snd_pcm_start(stream->handle);
			if(err < 0) {
				WPRINTF("%s: recorver from suspend start fail, error number %d!\n", __func__, err);
				return -1;
			}
			break;
		}
	}
	return err;
}

static int
virtio_sound_xfer(struct virtio_sound_pcm *stream)
{
	const snd_pcm_channel_area_t *pcm_areas;
	struct virtio_sound_msg_node *msg_node;
	struct virtio_snd_pcm_status *ret_status;
	snd_pcm_sframes_t avail, xfer = 0;
	snd_pcm_uframes_t pcm_offset, frames;
	void * buf;
	int err, i, frame_size, to_copy, len = 0;

	avail = snd_pcm_avail_update(stream->handle);
	if (avail < 0) {
		err = virtio_sound_recover(stream);
		if (err < 0) {
			WPRINTF("%s: recorver form suspend prepare fail, error number %d!\n", __func__, err);
			return -1;
		}
	}
	frame_size = virtio_sound_get_frame_size(stream);
	frames = stream->param.period_bytes / frame_size;
	/*
	 * For frontend send buffer address period by period, backend copy a period
	 * data in one time.
	 */
	if (avail < frames || (msg_node = STAILQ_FIRST(&stream->head)) == NULL) {
		return 0;
	}
	err = snd_pcm_mmap_begin(stream->handle, &pcm_areas, &pcm_offset, &frames);
	if (err < 0) {
		err = virtio_sound_recover(stream);
		if (err < 0) {
			WPRINTF("%s: mmap begin fail, error number %d!\n", __func__, err);
			return -1;
		}
	}
	/*
	 * 'pcm_areas' is an array which contains num_of_channels elements in it.
	 * For interleaved, all elements in the array has the same addr but different offset ("first" in the structure).
	 */
	buf = pcm_areas[0].addr + pcm_offset * frame_size;
	for (i = 1; i < msg_node->cnt - 1; i++) {
		to_copy = msg_node->iov[i].iov_len;
		/*
		 * memcpy can only be used when SNDRV_PCM_INFO_INTERLEAVED.
		 */
		if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
			memcpy(buf, msg_node->iov[i].iov_base, to_copy);
		} else {
			memcpy(msg_node->iov[i].iov_base, buf, to_copy);
			len += msg_node->iov[i].iov_len;
		}
		xfer += to_copy / frame_size;
		buf += to_copy;
	}
	if (xfer != frames) {
		WPRINTF("%s: write fail, xfer %ld, frame %ld!\n", __func__, xfer, frames);
		return -1;
	}
	xfer = snd_pcm_mmap_commit(stream->handle, pcm_offset, frames);
	if(xfer < 0 || xfer != frames) {
		WPRINTF("%s: mmap commit fail, xfer %ld!\n", __func__, xfer);
		return -1;
	}
	ret_status = (struct virtio_snd_pcm_status *)msg_node->iov[msg_node->cnt - 1].iov_base;
	ret_status->status = VIRTIO_SND_S_OK;
	vq_relchain(msg_node->vq, msg_node->idx, len + sizeof(struct virtio_snd_pcm_status));
	vq_endchains(msg_node->vq, 0);
	pthread_mutex_lock(&stream->mtx);
	STAILQ_REMOVE_HEAD(&stream->head, link);
	pthread_mutex_unlock(&stream->mtx);
	free(msg_node->iov);
	free(msg_node);
	return xfer;
}

static void
virtio_sound_clean_vq(struct virtio_sound_pcm *stream) {
	struct virtio_sound_msg_node *msg_node;
	struct virtio_vq_info *vq = NULL;
	struct virtio_snd_pcm_status *ret_status;

	while ((msg_node = STAILQ_FIRST(&stream->head)) != NULL) {
		vq = msg_node->vq;
		ret_status = (struct virtio_snd_pcm_status *)msg_node->iov[msg_node->cnt - 1].iov_base;
		ret_status->status = VIRTIO_SND_S_BAD_MSG;
		vq_relchain(vq, msg_node->idx, sizeof(struct virtio_snd_pcm_status));

		pthread_mutex_lock(&stream->mtx);
		STAILQ_REMOVE_HEAD(&stream->head, link);
		pthread_mutex_unlock(&stream->mtx);
		free(msg_node->iov);
		free(msg_node);
	}

	if (vq)
		vq_endchains(vq, 0);
}

static void*
virtio_sound_pcm_thread(void *param)
{
	unsigned short revents;
	struct virtio_sound_pcm *stream = (struct virtio_sound_pcm*)param;
	int err;

	do {
		poll(stream->poll_fd, stream->pfd_count, -1);
		snd_pcm_poll_descriptors_revents(stream->handle, stream->poll_fd,
			stream->pfd_count, &revents);
		if (revents & POLLOUT || revents & POLLIN) {
			err = virtio_sound_xfer(stream);
			if (err < 0) {
				WPRINTF("%s: stream error!\n", __func__);
				break;
			}
		} else {
			err = virtio_sound_recover(stream);
			if (err < 0) {
				WPRINTF("%s: poll error %d!\n", __func__, (int)snd_pcm_state(stream->handle));
				break;
			}
		}
		if (stream->status == VIRTIO_SND_BE_STOP) {
			usleep(100);
			continue;
		}
	} while (stream->status == VIRTIO_SND_BE_START || stream->status == VIRTIO_SND_BE_STOP);

	if (stream->status == VIRTIO_SND_BE_RELEASE && !STAILQ_EMPTY(&stream->head)) {
		virtio_sound_clean_vq(stream);
	}
	pthread_mutex_lock(&stream->ctl_mtx);
	if(stream->handle) {
		if (snd_pcm_close(stream->handle) < 0) {
			WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		}
		stream->handle = NULL;
	}
	free(stream->poll_fd);
	stream->poll_fd = NULL;
	stream->status = VIRTIO_SND_BE_INITED;
	pthread_mutex_unlock(&stream->ctl_mtx);
	pthread_exit(NULL);
}

static int
virtio_sound_create_pcm_thread(struct virtio_sound_pcm *stream)
{
	int err;
	pthread_attr_t attr;

	stream->pfd_count = snd_pcm_poll_descriptors_count(stream->handle);
	stream->poll_fd = malloc(sizeof(struct pollfd) * stream->pfd_count);
	if (stream->poll_fd == NULL) {
		WPRINTF("%s: malloc poll fd fail\n", __func__);
		return -1;
	}
	if ((err = snd_pcm_poll_descriptors(stream->handle,
		stream->poll_fd, stream->pfd_count)) <= 0) {
		WPRINTF("%s: get poll descriptor fail, error number %d!\n", __func__, err);
		return -1;
	}
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&stream->tid, &attr, virtio_sound_pcm_thread, (void *)stream);
	return 0;
}

static void virtio_sound_update_iov_cnt(struct virtio_sound *virt_snd, int dir) {
	int i, cnt = 0;

	for (i = 0; i < virt_snd->stream_cnt; i++) {
		if (virt_snd->streams[i]->dir == dir && virt_snd->streams[i]->status != VIRTIO_SND_BE_INITED
			&& cnt < virt_snd->streams[i]->xfer_iov_cnt) {
			cnt = virt_snd->streams[i]->xfer_iov_cnt;
		}
	}
	if (dir == SND_PCM_STREAM_PLAYBACK)
		virt_snd->max_tx_iov_cnt = cnt;
	else
		virt_snd->max_rx_iov_cnt = cnt;
}

static snd_hctl_elem_t *
virtio_sound_get_ctl_elem(snd_hctl_t *hctl, char *identifier)
{
	snd_ctl_elem_id_t *id;
	snd_hctl_elem_t *elem;

	snd_ctl_elem_id_alloca(&id);
	if (snd_ctl_ascii_elem_id_parse(id, identifier) < 0) {
		WPRINTF("%s: wrong identifier %s!\n", __func__, identifier);
		return NULL;
	}
	if ((elem = snd_hctl_find_elem(hctl, id)) == NULL) {
		WPRINTF("%s: find elem fail, identifier is %s!\n", __func__, identifier);
		return NULL;
	}
	return elem;
}

static int
virtio_sound_get_jack_value(snd_hctl_elem_t *elem)
{
	snd_ctl_elem_info_t *ctl;
	snd_ctl_elem_value_t *ctl_value;
	int value;

	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_readable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__, snd_hctl_elem_get_name(elem));
		return -1;
	}
	snd_ctl_elem_value_alloca(&ctl_value);
	if (snd_hctl_elem_read(elem, ctl_value) < 0) {
		WPRINTF("%s: read %s value fail!\n", __func__, snd_hctl_elem_get_name(elem));
		return -1;
	}
	value = snd_ctl_elem_value_get_boolean(ctl_value, 0);
	return value;
}

static int
virtio_sound_r_jack_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_jack_info *jack_info = iov[2].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int j = 0, i = 0, ret_len;


	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->jack_cnt) {
		WPRINTF("%s: invalid jack, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret_len = info->count * sizeof(struct virtio_snd_jack_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	memset(jack_info, 0, ret_len);
	j = info->start_id;
	for (i = 0; i < info->count; i++) {
		jack_info[i].connected = virt_snd->jacks[j]->connected;
		jack_info[i].hda_reg_defconf = virt_snd->jacks[j]->hda_reg_defconf;
		j++;
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	int i, ret_len;
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_pcm_info *pcm_info = iov[2].iov_base;
	struct virtio_sound_pcm *stream;
	struct virtio_snd_hdr *ret = iov[1].iov_base;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret_len = info->count * sizeof(struct virtio_snd_pcm_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	for (i = 0; i < info->count; i++) {
		stream = virt_snd->streams[info->start_id + i];
		if (stream == NULL) {
			WPRINTF("%s: invalid stream, start %d, count = %d!\n", __func__,
				info->start_id, info->count);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
		pcm_info[i].hdr.hda_fn_nid = stream->hda_fn_nid;
		pcm_info[i].features = stream->param.features;
		pcm_info[i].formats = stream->param.formats;
		pcm_info[i].rates = stream->param.rates;
		pcm_info[i].direction = stream->dir;
		pcm_info[i].channels_min = stream->param.channels_min;
		pcm_info[i].channels_max = stream->param.channels_max;
		memset(pcm_info[i].padding, 0, sizeof(pcm_info[i].padding));
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static int
virtio_sound_r_set_params(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_set_params *params = (struct virtio_snd_pcm_set_params *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	struct virtio_sound_pcm *stream;
	int err;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (params->hdr.stream_id >= virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream %d!\n", __func__, params->hdr.stream_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	stream = virt_snd->streams[params->hdr.stream_id];
	if (stream->status == VIRTIO_SND_BE_RELEASE) {
		WPRINTF("%s: stream %d is releasing!\n", __func__, stream->id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if((stream->param.formats && (1 << params->format) == 0) ||
		(stream->param.rates && (1 << params->rate) == 0) ||
		(params->channels < stream->param.channels_min) ||
		(params->channels > stream->param.channels_max)) {
		WPRINTF("%s: invalid parameters sample format %d, frame rate %d, channels %d!\n", __func__,
		params->format, params->rate, params->channels);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret->code = VIRTIO_SND_S_OK;
	stream->param.buffer_bytes = params->buffer_bytes;
	stream->param.period_bytes = params->period_bytes;
	stream->param.features = params->features;
	stream->param.channels = params->channels;
	stream->param.format = params->format;
	stream->param.rate = params->rate;

	/*
	 * In extreme case, each data page is disconinuous and the start and end
	 * of data buffer is not 4k align. The total xfer_iov_cnt is
	 * period bytes / 4k + 2 (start and end) + 2 (xfer msg + status).
	 */
	stream->xfer_iov_cnt = stream->param.period_bytes / 4096 + VIRTIO_SOUND_XFER_SEGS;
	if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
		if (stream->xfer_iov_cnt > virt_snd->max_tx_iov_cnt)
			virt_snd->max_tx_iov_cnt = stream->xfer_iov_cnt;
	} else {
		if (stream->xfer_iov_cnt > virt_snd->max_rx_iov_cnt)
			virt_snd->max_rx_iov_cnt = stream->xfer_iov_cnt;
	}

	if(!stream->handle)
		if (snd_pcm_open(&stream->handle, stream->dev_name,
			stream->dir, SND_PCM_NONBLOCK) < 0 || stream->handle == NULL) {
			WPRINTF("%s: stream %s open fail!\n", __func__, stream->dev_name);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
	err = virtio_sound_set_hwparam(stream);
	if (err < 0) {
		WPRINTF("%s: set hw params fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}
	err = virtio_sound_set_swparam(stream);
	if (err < 0) {
		WPRINTF("%s: set sw params fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_prepare(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((s = pcm->stream_id) >= virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	pthread_mutex_lock(&virt_snd->streams[s]->ctl_mtx);
	if (virt_snd->streams[s]->status == VIRTIO_SND_BE_RELEASE) {
		pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
		WPRINTF("%s: stream %d is releasing!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret->code = VIRTIO_SND_S_OK;
	if(!virt_snd->streams[s]->handle)
		if (snd_pcm_open(&virt_snd->streams[s]->handle, virt_snd->streams[s]->dev_name,
			virt_snd->streams[s]->dir, SND_PCM_NONBLOCK) < 0  || virt_snd->streams[s]->handle == NULL) {
			pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
			WPRINTF("%s: stream %s open fail!\n", __func__, virt_snd->streams[s]->dev_name);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
	if (snd_pcm_prepare(virt_snd->streams[s]->handle) < 0) {
		pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
		WPRINTF("%s: stream %s prepare fail!\n", __func__, virt_snd->streams[s]->dev_name);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	virt_snd->streams[s]->status = VIRTIO_SND_BE_PRE;
	pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_release(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	int s;
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((s = pcm->stream_id) >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	pthread_mutex_lock(&virt_snd->streams[s]->ctl_mtx);
	virt_snd->streams[s]->status = VIRTIO_SND_BE_RELEASE;
	pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);
	ret->code = VIRTIO_SND_S_OK;
	virtio_sound_update_iov_cnt(virt_snd, virt_snd->streams[s]->dir);

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_start(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	struct virtio_sound_pcm *stream;
	int i;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (pcm->stream_id >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, pcm->stream_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	stream = virt_snd->streams[pcm->stream_id];
	pthread_mutex_lock(&stream->ctl_mtx);
	if (stream->status == VIRTIO_SND_BE_RELEASE) {
		pthread_mutex_unlock(&stream->ctl_mtx);
		WPRINTF("%s: stream %d is releasing!\n", __func__, stream->id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
		if (STAILQ_EMPTY(&stream->head)) {
			stream->status = VIRTIO_SND_BE_PENDING;
			pthread_mutex_unlock(&stream->ctl_mtx);
			return (int)iov[1].iov_len;
		}
		/*
		 * For start threshold is 1, send 2 period before start.
		 * Less start periods benifit the frontend hw_ptr updating.
		 * After 1 period finished, xfer thread will full fill the buffer.
		 * Send 2 periods here to avoid the empty buffer which may cause
		 * pops and clicks.
		 */
		for (i = 0; i < 2; i++) {
			if (virtio_sound_xfer(stream) < 0) {
				pthread_mutex_unlock(&stream->ctl_mtx);
				WPRINTF("%s: stream fn_id %d xfer error!\n", __func__, stream->hda_fn_nid);
				ret->code = VIRTIO_SND_S_BAD_MSG;
				return (int)iov[1].iov_len;
			}
		}
	}
	if (snd_pcm_start(stream->handle) < 0) {
		pthread_mutex_unlock(&stream->ctl_mtx);
		WPRINTF("%s: stream %s start error!\n", __func__, stream->dev_name);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	stream->status = VIRTIO_SND_BE_START;
	if (virtio_sound_create_pcm_thread(stream) < 0) {
		WPRINTF("%s: create thread fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}
	pthread_mutex_unlock(&stream->ctl_mtx);

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_stop(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}

	if ((s = pcm->stream_id) >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	pthread_mutex_lock(&virt_snd->streams[s]->ctl_mtx);
	if (snd_pcm_drop(virt_snd->streams[s]->handle) < 0) {
		WPRINTF("%s: stream %s drop error!\n", __func__, virt_snd->streams[s]->dev_name);
	}
	virt_snd->streams[s]->status = VIRTIO_SND_BE_STOP;
	pthread_mutex_unlock(&virt_snd->streams[s]->ctl_mtx);

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_chmap_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_chmap_info *chmap_info = iov[2].iov_base;
	struct virtio_sound_pcm *stream = virt_snd->streams[0];
	struct virtio_sound_chmap *chmap;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s, c = 0, i = 0, ret_len;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->chmap_cnt) {
		WPRINTF("%s: invalid chmap, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret_len = info->count * sizeof(struct virtio_snd_chmap_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	for(s = 0; s < virt_snd->stream_cnt; s++) {
		if (info->start_id >= i && info->start_id < i + virt_snd->streams[s]->chmap_cnt) {
			c = info->start_id - i;
			stream = virt_snd->streams[s];
			break;
		}
		i += virt_snd->streams[s]->chmap_cnt;
	}

	for (i = 0; i < info->count; i++) {
		chmap = stream->chmaps[c];
		chmap_info[i].hdr.hda_fn_nid = stream->hda_fn_nid;
		chmap_info[i].direction = stream->dir;
		chmap_info[i].channels = chmap->channels;
		memcpy(chmap_info[i].positions, chmap->positions, VIRTIO_SND_CHMAP_MAX_SIZE);

		if (++c >= stream->chmap_cnt) {
			if(++s >= virt_snd->stream_cnt)
				break;
			stream = virt_snd->streams[s];
			c = 0;
		}
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static int
virtio_sound_set_access(snd_ctl_elem_info_t *ctl)
{
	return (snd_ctl_elem_info_is_readable(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_READ : 0) |
		(snd_ctl_elem_info_is_writable(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_WRITE : 0) |
		(snd_ctl_elem_info_is_volatile(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_VOLATILE : 0) |
		(snd_ctl_elem_info_is_inactive(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_VOLATILE : 0) |
		(snd_ctl_elem_info_is_tlv_readable(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_TLV_READ : 0) |
		(snd_ctl_elem_info_is_tlv_writable(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_TLV_WRITE : 0) |
		(snd_ctl_elem_info_is_tlv_commandable(ctl) ? 1 << VIRTIO_SND_CTL_ACCESS_TLV_COMMAND : 0);
}

static int
virtio_sound_r_ctl_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_ctl_info *ctl_info = iov[2].iov_base;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int c = 0, i = 0, ret_len;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->ctl_cnt) {
		WPRINTF("%s: invalid kcontrol, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret_len = info->count * sizeof(struct virtio_snd_ctl_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	snd_ctl_elem_info_alloca(&ctl);
	c = info->start_id;
	for (i = 0; i < info->count; i++) {
		elem = virt_snd->ctls[c]->elem;
		if (snd_hctl_elem_info(elem, ctl) < 0) {
			WPRINTF("%s: find elem info fail, identifier is %s!\n", __func__,
				snd_hctl_elem_get_name(virt_snd->ctls[c]->elem));
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
		ctl_info[i].type = virtio_sound_s2v_type[(int)snd_ctl_elem_info_get_type(ctl)];
		ctl_info[i].access = virtio_sound_set_access(ctl);
		ctl_info[i].count = snd_ctl_elem_info_get_count(ctl);
		ctl_info[i].index = snd_ctl_elem_info_get_index(ctl);
		memcpy(ctl_info[i].name, snd_ctl_elem_info_get_name(ctl), 44);

		switch (ctl_info[i].type) {
			case VIRTIO_SND_CTL_TYPE_INTEGER:
				ctl_info[i].value.integer.min = snd_ctl_elem_info_get_min(ctl);
				ctl_info[i].value.integer.max = snd_ctl_elem_info_get_max(ctl);
				ctl_info[i].value.integer.step = snd_ctl_elem_info_get_step(ctl);
				break;

			case VIRTIO_SND_CTL_TYPE_INTEGER64:
				ctl_info[i].value.integer64.min = snd_ctl_elem_info_get_min64(ctl);
				ctl_info[i].value.integer64.max = snd_ctl_elem_info_get_max64(ctl);
				ctl_info[i].value.integer64.step = snd_ctl_elem_info_get_step64(ctl);
				break;

			case VIRTIO_SND_CTL_TYPE_ENUMERATED:
				ctl_info[i].value.enumerated.items = snd_ctl_elem_info_get_items(ctl);
				break;

			default:
				break;
		}
		c++;
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static int
virtio_sound_r_ctl_enum_items(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int items, i;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	snd_ctl_elem_info_alloca(&ctl);
	elem = virt_snd->ctls[info->control_id]->elem;
	if (snd_hctl_elem_info(elem, ctl) < 0) {
		WPRINTF("%s: get elem info fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (snd_ctl_elem_info_get_type(ctl) != SND_CTL_ELEM_TYPE_ENUMERATED) {
		WPRINTF("%s: elem is not enumerated, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	items = snd_ctl_elem_info_get_items(ctl);
	if (items != (iov[2].iov_len / sizeof(struct virtio_snd_ctl_enum_item))) {
		WPRINTF("%s: %s item count(%d) err!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem), items);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	for(i = 0; i < items; i++) {
		snd_ctl_elem_info_set_item(ctl, i);
		if (snd_hctl_elem_info(elem, ctl) < 0) {
			WPRINTF("%s: %s get item %d err!\n", __func__,
				snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem), i);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
		strncpy((iov[2].iov_base + sizeof(struct virtio_snd_ctl_enum_item) * i),
			snd_ctl_elem_info_get_item_name(ctl), sizeof(struct virtio_snd_ctl_enum_item));
	}

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[2].iov_len + (int)iov[1].iov_len;
}

static int
virtio_sound_r_ctl_read(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;;
	snd_ctl_elem_value_t *ctl_value;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;
	struct virtio_snd_hdr *ret = iov[1].iov_base;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	elem = virt_snd->ctls[info->control_id]->elem;
	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_readable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	snd_ctl_elem_value_alloca(&ctl_value);
	if (snd_hctl_elem_read(elem, ctl_value) < 0) {
		WPRINTF("%s: read %s value fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	memcpy(iov[1].iov_base + sizeof(struct virtio_snd_hdr), snd_ctl_elem_value_get_bytes(ctl_value),
		iov[1].iov_len - sizeof(struct virtio_snd_hdr));

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_ctl_write(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;
	struct virtio_snd_ctl_value *val =
		(struct virtio_snd_ctl_value *)(iov[0].iov_base + sizeof(struct virtio_snd_ctl_hdr));
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	snd_ctl_elem_value_t *ctl_value;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	elem = virt_snd->ctls[info->control_id]->elem;
	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_writable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	snd_ctl_elem_value_alloca(&ctl_value);
	if (snd_hctl_elem_read(elem, ctl_value) < 0) {
		WPRINTF("%s: read %s value fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	snd_ctl_elem_set_bytes(ctl_value, val, sizeof(struct virtio_snd_ctl_value));
	if (snd_hctl_elem_write(elem, ctl_value) < 0) {
		WPRINTF("%s: write %s value fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[1].iov_len;

}

static int
virtio_sound_r_ctl_tlv_read(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	elem = virt_snd->ctls[info->control_id]->elem;
	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_tlv_readable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (snd_hctl_elem_tlv_read(elem, iov[2].iov_base, iov[2].iov_len / sizeof(int)) < 0) {
		WPRINTF("%s: read %s tlv fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	return iov[2].iov_len + iov[1].iov_len;
}

static int
virtio_sound_r_ctl_tlv_write(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[2].iov_base;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;
	uint32_t *tlv = (uint32_t *)iov[1].iov_base;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}

	elem = virt_snd->ctls[info->control_id]->elem;
	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_tlv_writable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}
	if (snd_hctl_elem_tlv_write(elem, tlv) < 0) {
		WPRINTF("%s: write %s tlv fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[2].iov_len;
}

static int
virtio_sound_r_ctl_tlv_command(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_ctl_hdr *info = (struct virtio_snd_ctl_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[2].iov_base;
	snd_hctl_elem_t *elem;
	snd_ctl_elem_info_t *ctl;
	uint32_t *tlv = (uint32_t *)iov[1].iov_base;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (virt_snd->ctl_cnt <= info->control_id) {
		WPRINTF("%s: invalid ctrl, control_id %d!\n", __func__, info->control_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}

	elem = virt_snd->ctls[info->control_id]->elem;
	snd_ctl_elem_info_alloca(&ctl);
	if (snd_hctl_elem_info(elem, ctl) < 0 || snd_ctl_elem_info_is_tlv_commandable(ctl) == 0) {
		WPRINTF("%s: access check fail, identifier is %s!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}
	if (snd_hctl_elem_tlv_command(elem, tlv) < 0) {
		WPRINTF("%s: %s tlv command fail!\n", __func__,
			snd_hctl_elem_get_name(virt_snd->ctls[info->control_id]->elem));
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[2].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[2].iov_len;
}

static void
virtio_sound_notify_ctl(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	struct virtio_snd_query_info *info;
	struct iovec iov[VIRTIO_SOUND_CTL_SEGS];
	int n, ret_len = 0;
	uint16_t idx;

	while (vq_has_descs(vq)) {
		n = vq_getchain(vq, &idx, iov, VIRTIO_SOUND_CTL_SEGS, NULL);
		if (n <= 0) {
			WPRINTF("%s: fail to getchain!\n", __func__);
			return;
		}

		info = (struct virtio_snd_query_info *)iov[0].iov_base;
		switch (info->hdr.code) {
			case VIRTIO_SND_R_JACK_INFO:
				ret_len = virtio_sound_r_jack_info (virt_snd, iov, n);
				break;
			// case VIRTIO_SND_R_JACK_REMAP:
			//	break;
			case VIRTIO_SND_R_PCM_INFO:
				ret_len = virtio_sound_r_pcm_info(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_SET_PARAMS:
				ret_len = virtio_sound_r_set_params(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_PREPARE:
				ret_len = virtio_sound_r_pcm_prepare(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_RELEASE:
				ret_len = virtio_sound_r_pcm_release(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_START:
				ret_len = virtio_sound_r_pcm_start(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_STOP:
				ret_len = virtio_sound_r_pcm_stop(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CHMAP_INFO:
				ret_len = virtio_sound_r_chmap_info(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_INFO:
				ret_len = virtio_sound_r_ctl_info(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_ENUM_ITEMS:
				ret_len = virtio_sound_r_ctl_enum_items(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_READ:
				ret_len = virtio_sound_r_ctl_read(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_WRITE:
				ret_len = virtio_sound_r_ctl_write(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_TLV_READ:
				ret_len = virtio_sound_r_ctl_tlv_read(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_TLV_WRITE:
				ret_len = virtio_sound_r_ctl_tlv_write(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CTL_TLV_COMMAND:
				ret_len = virtio_sound_r_ctl_tlv_command(virt_snd, iov, n);
				break;
			default:
				WPRINTF("%s: unsupported request 0x%X!\n", __func__, n);
				break;
		}

		vq_relchain(vq, idx, ret_len);
	}
	vq_endchains(vq, 1);
}

static void
virtio_sound_notify_event(void *vdev, struct virtio_vq_info *vq)
{
}

/*init*/
static void
virtio_sound_cfg_init(struct virtio_sound *virt_snd)
{
	virt_snd->snd_cfg.streams = virt_snd->stream_cnt;
	virt_snd->snd_cfg.jacks = virt_snd->jack_cnt;
	virt_snd->snd_cfg.chmaps = virt_snd->chmap_cnt;
	virt_snd->snd_cfg.controls = virt_snd->ctl_cnt;
}

static bool
virtio_sound_format_support(snd_pcm_t *handle, uint32_t format)
{
	snd_pcm_hw_params_t *hwparams;

	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return false;
	}
	return (snd_pcm_hw_params_test_format(handle, hwparams, format) == 0);
}

static bool
virtio_sound_rate_support(snd_pcm_t *handle, uint32_t rate, int dir)
{
	snd_pcm_hw_params_t *hwparams;
	uint32_t rrate;

	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return false;
	}
	rrate = rate;
	return (snd_pcm_hw_params_set_rate_near(handle, hwparams, &rrate, &dir) == 0
		&& rrate == rate);
}

static int
virtio_sound_pcm_param_init(struct virtio_sound_pcm *stream, int dir, char *name, int fn_id)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_chmap_query_t **chmaps;
	uint32_t channels_min, channels_max, i, j;

	stream->dir = dir;
	strncpy(stream->dev_name, name, VIRTIO_SOUND_DEVICE_NAME);
	stream->hda_fn_nid = fn_id;

	if (snd_pcm_open(&stream->handle, stream->dev_name,
		stream->dir, SND_PCM_NONBLOCK) < 0 || stream->handle == NULL) {
		WPRINTF("%s: stream %s open fail!\n", __func__, stream->dev_name);
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(virtio_sound_v2s_format); i++) {
		if (virtio_sound_format_support(stream->handle, virtio_sound_v2s_format[i]))
			stream->param.formats |= (1 << i);
	}
	for (i = 0; i < ARRAY_SIZE(virtio_sound_t_rate); i++) {
		if(virtio_sound_rate_support(stream->handle, virtio_sound_t_rate[i], dir))
			stream->param.rates |= (1 << i);
	}
	if (stream->param.rates == 0 || stream->param.formats == 0) {
		WPRINTF("%s: get param fail rates 0x%lx formats 0x%lx!\n", __func__,
			stream->param.rates, stream->param.formats);
		return -1;
	}
	stream->param.features = (1 << VIRTIO_SND_PCM_F_EVT_XRUNS);
	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(stream->handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return -1;
	}
	if (snd_pcm_hw_params_get_channels_min(hwparams, &channels_min) < 0 ||
		snd_pcm_hw_params_get_channels_max(hwparams, &channels_max) < 0) {
			WPRINTF("%s: get channel info fail!\n", __func__);
			return -1;
	}
	stream->param.channels_min = channels_min;
	stream->param.channels_max = channels_max;

	i = 0;
	chmaps = snd_pcm_query_chmaps(stream->handle);
	while (chmaps != NULL && chmaps[i] != NULL) {
		stream->chmaps[i] = malloc(sizeof(struct virtio_sound_chmap));
		if (stream->chmaps[i] == NULL) {
			WPRINTF("%s: malloc chmap buffer fail!\n", __func__);
			return -1;
		}
		stream->chmaps[i]->channels = chmaps[i]->map.channels;
		for (j = 0; j < chmaps[i]->map.channels; j++)
			stream->chmaps[i]->positions[j] = virtio_sound_s2v_chmap[chmaps[i]->map.pos[j]];
		stream->chmap_cnt++;
		i++;
	}
	snd_pcm_free_chmaps(chmaps);
	STAILQ_INIT(&stream->head);

	if (snd_pcm_close(stream->handle) < 0) {
		WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		return -1;
	}
	stream->handle = NULL;

	return 0;
}

static int
virtio_sound_pcm_init(struct virtio_sound *virt_snd, char *device, char *hda_fn_nid, int dir)
{
	struct virtio_sound_pcm *stream;
	int err;

	if (virt_snd->stream_cnt >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: too many audio streams(%d)!\n", __func__, VIRTIO_SOUND_VQ_NUM);
		return -1;
	}
	stream = malloc(sizeof(struct virtio_sound_pcm));
	if (stream == NULL) {
		WPRINTF("%s: malloc data node fail!\n", __func__);
		return -1;
	}
	memset(stream, 0, sizeof(struct virtio_sound_pcm));

	stream->id = virt_snd->stream_cnt;
	strncpy(stream->dev_name, device, VIRTIO_SOUND_DEVICE_NAME);
	stream->hda_fn_nid = atoi(hda_fn_nid);
	if (virtio_sound_pcm_param_init(stream, dir, stream->dev_name, stream->hda_fn_nid) == 0) {
		virt_snd->streams[virt_snd->stream_cnt] = stream;
		virt_snd->stream_cnt++;
		virt_snd->chmap_cnt += stream->chmap_cnt;
	} else {
		WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		free(stream);
		return -1;
	}
	err = pthread_mutex_init(&stream->mtx, NULL);
	if (err) {
		WPRINTF("%s: mutex init failed with error %d!\n", __func__, err);
		free(stream);
		return -1;
	}
	err = pthread_mutex_init(&stream->ctl_mtx, NULL);
	if (err) {
		WPRINTF("%s: mutex init failed with error %d!\n", __func__, err);
		pthread_mutex_destroy(&stream->mtx);
		free(stream);
		return -1;
	}
	return 0;
}

static uint32_t
virtio_snd_jack_parse(char *identifier)
{
	uint32_t location, device;

	if (strstr(identifier, "Dock")) {
		location = HDA_JACK_LOCATION_SEPARATE;
	} else if (strstr(identifier, "Internal")) {
		location = HDA_JACK_LOCATION_INTERNAL;
	} else if (strstr(identifier, "Rear")) {
		location = HDA_JACK_LOCATION_REAR;
	} else if (strstr(identifier, "Front")) {
		location = HDA_JACK_LOCATION_FRONT;
	} else {
		location = HDA_JACK_LOCATION_NONE;
	}

	if (strstr(identifier, "Line Out")) {
		device = HDA_JACK_LINE_OUT;
	} else if (strstr(identifier, "Line")) {
		device = HDA_JACK_LINE_IN;
	} else if (strstr(identifier, "Speaker")) {
		device = HDA_JACK_SPEAKER;
		location = HDA_JACK_LOCATION_INTERNAL;
	} else if (strstr(identifier, "Mic")) {
		device = HDA_JACK_MIC_IN;
	} else if (strstr(identifier, "CD")) {
		device = HDA_JACK_CD;
	} else if (strstr(identifier, "Headphone")) {
		device = HDA_JACK_HP_OUT;
	} else if (strstr(identifier, "Aux")) {
		device = HDA_JACK_AUX;
	} else if (strstr(identifier, "SPDIF In")) {
		device = HDA_JACK_SPDIF_IN;
	} else if (strstr(identifier, "Digital In")) {
		device = HDA_JACK_DIG_OTHER_IN;
	} else if (strstr(identifier, "SPDIF")) {
		device = HDA_JACK_SPDIF_OUT;
	} else if (strstr(identifier, "HDMI")) {
		device = HDA_JACK_DIG_OTHER_OUT;
		location = HDA_JACK_LOCATION_HDMI;
	} else {
		device = HDA_JACK_OTHER;
	}

	return (device << HDA_JACK_DEFREG_DEVICE_SHIFT) | (location << HDA_JACK_DEFREG_LOCATION_SHIFT);
}

static struct vbs_card*
virtio_sound_get_card(struct virtio_sound *virt_snd, char *card)
{
	int i, num;

	for (i = 0; i < virt_snd->card_cnt; i++) {
		if(strcmp(virt_snd->cards[i]->card, card) == 0) {
			return virt_snd->cards[i];
		}
	}
	if (virt_snd->card_cnt >= VIRTIO_SOUND_CARD) {
		WPRINTF("%s: too many cards %d!\n", __func__, virt_snd->card_cnt);
		return NULL;
	}
	num = virt_snd->card_cnt;
	virt_snd->cards[num] = malloc(sizeof(struct vbs_card));
	if (virt_snd->cards[num] == NULL) {
		WPRINTF("%s: malloc card node %d fail!\n", __func__, num);
		return NULL;
	}
	strncpy(virt_snd->cards[num]->card, card, VIRTIO_SOUND_CARD_NAME);
	if (snd_hctl_open(&virt_snd->cards[num]->handle, virt_snd->cards[num]->card, 0)) {
		WPRINTF("%s: hctl open fail, card %s!\n", __func__, virt_snd->cards[num]->card);
		return NULL;
	}
	if (snd_hctl_load(virt_snd->cards[num]->handle) < 0) {
		WPRINTF("%s: hctl load fail, card %s!\n", __func__, virt_snd->cards[num]->card);
		snd_hctl_close(virt_snd->cards[num]->handle);
		return NULL;
	}
	virt_snd->card_cnt++;
	return virt_snd->cards[num];
}

static int
virtio_sound_init_ctl_elem(struct virtio_sound *virt_snd, char *card_str, char *identifier)
{
	snd_ctl_elem_info_t *info;
	snd_hctl_elem_t *elem;

	struct vbs_card *card;
	char card_name[VIRTIO_SOUND_CARD_NAME];
	int idx;

	if (strspn(card_str, "0123456789") == strlen(card_str)) {
		idx = snd_card_get_index(card_str);
		if (idx >= 0 && idx < 32)
#if defined(SND_LIB_VER)
#if SND_LIB_VER(1, 2, 5) <= SND_LIB_VERSION
			snprintf(card_name, VIRTIO_SOUND_CARD_NAME, "sysdefault:%i", idx);
#else
			snprintf(card_name, VIRTIO_SOUND_CARD_NAME, "hw:%i", idx);
#endif
#else
			snprintf(card_name, VIRTIO_SOUND_CARD_NAME, "hw:%i", idx);
#endif
		else {
			WPRINTF("%s: card(%s) err, get %s ctl elem fail!\n", __func__, card_str, identifier);
			return -1;
		}
	} else {
		strncpy(card_name, card_str, VIRTIO_SOUND_CARD_NAME);
	}
	card = virtio_sound_get_card(virt_snd, card_name);
	if(card == NULL) {
		WPRINTF("%s: set card(%s) fail!\n", __func__, card_name);
		return -1;
	}
	snd_ctl_elem_info_alloca(&info);

	elem = virtio_sound_get_ctl_elem(card->handle, identifier);
	if (elem == NULL) {
		WPRINTF("%s: get %s ctl elem fail!\n", __func__, identifier);
		return -1;
	}
	if (strstr(identifier, "Jack") != NULL) {
		virt_snd->jacks[virt_snd->jack_cnt] = malloc(sizeof(struct vbs_jack_elem));
		if (virt_snd->jacks[virt_snd->jack_cnt] == NULL) {
			WPRINTF("%s: malloc jack elem fail!\n", __func__);
			return -1;
		}
		virt_snd->jacks[virt_snd->jack_cnt]->elem = elem;
		virt_snd->jacks[virt_snd->jack_cnt]->card = card;
		virt_snd->jacks[virt_snd->jack_cnt]->hda_reg_defconf = virtio_snd_jack_parse(identifier);
		virt_snd->jacks[virt_snd->jack_cnt]->connected = virtio_sound_get_jack_value(elem);
		snd_hctl_elem_set_callback(elem, virtio_sound_event_callback);
		virt_snd->jack_cnt++;
	} else {
		virt_snd->ctls[virt_snd->ctl_cnt] = malloc(sizeof(struct vbs_ctl_elem));
		if (virt_snd->ctls[virt_snd->ctl_cnt] == NULL) {
			WPRINTF("%s: malloc ctl elem fail!\n", __func__);
			return -1;
		}
		virt_snd->ctls[virt_snd->ctl_cnt]->elem = elem;
		virt_snd->ctls[virt_snd->ctl_cnt]->card = card;
		snd_hctl_elem_set_callback(elem, virtio_sound_event_callback);
		virt_snd->ctl_cnt++;
	}

	return 0;
}

static int
virtio_sound_parse_opts(struct virtio_sound *virt_snd, char *opts)
{
	char *str, *type, *cpy, *c, *param, *device, *identifier;

	/*
	 * Virtio sound command line should be:
	 * '-s n virtio-sound,...'.
	 * Playback substreams can be added by
	 * 	'pcmp=pcm1_name_str@hda_fn_nid[|pcm2_name_str@hda_fn_nid]'.
	 * Capture substreams can be added by
	 * 	'pcmc=pcm1_name_str@hda_fn_nid[|pcm2_name_str@hda_fn_nid]'.
	 * Kcontrol element can be added by
	 * 	'ctl=kctl1_identifer@card_name[|kctl2_identifer@card_name].
	 * The 'kctl_identifer' should be got by
	 * 	'amixer controls' such as
	 * 	'numid=99,iface=MIXER,name='PCM Playback Volume'.
	 * Substreams and kcontrols should be seperated by '&', as
	 * 	'-s n virtio-sound,pcmp=...&pcmc=...&ctl=...'.
	 */
	c = cpy = strdup(opts);
	while ((str = strsep(&cpy, "&")) != NULL) {
		type = strsep(&str, "=");
		if (strstr("pcmp", type)) {
			while ((param = strsep(&str, "|")) != NULL) {
				device = strsep(&param, "@");
				if (virtio_sound_pcm_init(virt_snd, device, param, VIRTIO_SND_D_OUTPUT) < 0) {
					WPRINTF("%s: fail to init pcm stream %s!\n", __func__, param);
					free(c);
					return -1;
				}
			}
		} else if (strstr("pcmc", type)) {
			while ((param = strsep(&str, "|")) != NULL) {
				device = strsep(&param, "@");
				if (virtio_sound_pcm_init(virt_snd, device, param, VIRTIO_SND_D_INPUT) < 0) {
					WPRINTF("%s: fail to init pcm stream %s!\n", __func__, param);
					free(c);
					return -1;
				}
			}
		} else if (strstr("ctl", type)) {
			while ((param = strsep(&str, "|")) != NULL) {
				identifier = strsep(&param, "@");
				if (virtio_sound_init_ctl_elem(virt_snd, param, identifier) < 0) {
					WPRINTF("%s: ctl elem %s init error!\n", __func__, identifier);
					free(c);
					return -1;
				}
			}
		} else {
			WPRINTF("%s: unknow type %s!\n", __func__, type);
			free(c);
			return -1;
		}
	}
	free(c);
	return 0;
}

static int
virtio_sound_send_event(struct virtio_sound *virt_snd, struct virtio_snd_event *event)
{
	struct virtio_vq_info *vq = &virt_snd->vq[VIRTIO_SND_VQ_EVENT];
	struct iovec iov[VIRTIO_SOUND_EVENT_SEGS];
	int n;
	uint16_t idx;

	if(!vq_has_descs(vq)) {
		WPRINTF("%s: vq has no descriptors!\n", __func__);
		return -1;
	}
	n = vq_getchain(vq, &idx, iov, VIRTIO_SOUND_EVENT_SEGS, NULL);
	if (n <= 0) {
		WPRINTF("%s: fail to getchain!\n", __func__);
		return -1;
	}
	if (n > VIRTIO_SOUND_EVENT_SEGS) {
		pr_warn("%s: invalid chain, desc number %d!\n", __func__, n);
		vq_retchain(vq);
		return -1;
	}

	memcpy(iov[0].iov_base, event, sizeof(struct virtio_snd_event));
	vq_relchain(vq, idx, sizeof(struct virtio_snd_event));
	vq_endchains(vq, 0);

	return 0;
}

static int
virtio_sound_event_callback(snd_hctl_elem_t *helem, unsigned int mask)
{
	struct virtio_sound *virt_snd = virtio_sound_get_device();
	struct virtio_snd_event event;
	int i;

	/*
	 * When close hctl handle while deinit virtio sound,
	 * alsa lib will trigger a poll event. Just return immediately.
	 */
	if (virt_snd->status == VIRTIO_SND_BE_DEINITED)
		return 0;

	if (strstr(snd_hctl_elem_get_name(helem), "Jack")) {
		for (i = 0; i < virt_snd->jack_cnt; i++) {
			if (helem == virt_snd->jacks[i]->elem) {
				virt_snd->jacks[i]->connected = virtio_sound_get_jack_value(virt_snd->jacks[i]->elem);
				if (virt_snd->jacks[i]->connected < 0) {
					WPRINTF("%s: Jack %s read value fail!\n", __func__,
						snd_hctl_elem_get_name(helem));
					return 0;
				}
				if (virt_snd->jacks[i]->connected > 0)
					event.hdr.code = VIRTIO_SND_EVT_JACK_CONNECTED;
				else
					event.hdr.code = VIRTIO_SND_EVT_JACK_DISCONNECTED;
				event.data = i;
				break;
			}
		}
		if (i == virt_snd->jack_cnt) {
			WPRINTF("%s: %d Jack %s miss matched!\n", __func__, i, snd_hctl_elem_get_name(helem));
			return 0;
		}
	} else {
		for (i = 0; i < virt_snd->ctl_cnt; i++) {
			if (helem == virt_snd->ctls[i]->elem) {
				event.hdr.code = VIRTIO_SND_EVT_CTL_NOTIFY;
				event.data = (i << 16) | (mask & 0xffff);
				break;
			}
		}
		if (i == virt_snd->ctl_cnt) {
			WPRINTF("%s: ctl %s miss matched!\n", __func__, snd_hctl_elem_get_name(helem));
			return 0;
		}
	}
	if (virtio_sound_send_event(virt_snd, &event) != 0) {
		WPRINTF("%s: event send fail!\n", __func__);
	}
	return 0;
}

static void *
virtio_sound_event_thread(void *param)
{
	struct virtio_sound *virt_snd = (struct virtio_sound *)param;
	struct pollfd *pfd;
	unsigned short *revents;
	int i, j, err, npfds = 0, max = 0;

	for (i = 0; i < virt_snd->card_cnt; i++) {
		npfds += virt_snd->cards[i]->count;
		max = (max > virt_snd->cards[i]->count) ? max : virt_snd->cards[i]->count;
	}

	pfd = alloca(sizeof(*pfd) * npfds);
	revents = alloca(sizeof(*revents) * max);
	for (i = 0; i < virt_snd->card_cnt; i++) {
		err = snd_hctl_poll_descriptors(virt_snd->cards[i]->handle, &pfd[virt_snd->cards[i]->start],
			virt_snd->cards[i]->count);
		if (err < 0) {
			WPRINTF("%s: fail to get poll descriptors!\n", __func__);
			pthread_exit(NULL);
		}
	}

	do {
		err = poll(pfd, npfds, -1);
		if (err < 0)
			continue;
		for (i = 0; i < virt_snd->card_cnt; i++) {
			snd_hctl_poll_descriptors_revents(virt_snd->cards[i]->handle, &pfd[virt_snd->cards[i]->start],
				virt_snd->cards[i]->count, revents);
			for (j = 0; j < virt_snd->cards[i]->count; j++) {
				if((revents[j] & (POLLIN | POLLOUT)) != 0) {
					snd_hctl_handle_events(virt_snd->cards[i]->handle);
					break;
				}
			}
		}
	} while (virt_snd->status != VIRTIO_SND_BE_DEINITED);

	pthread_exit(NULL);
}

static int
virtio_sound_event_init(struct virtio_sound *virt_snd, char *opts)
{
	pthread_attr_t attr;
	pthread_t tid;
	int i, start = 0;

	for (i = 0; i < virt_snd->card_cnt; i++) {
		virt_snd->cards[i]->count = snd_hctl_poll_descriptors_count(virt_snd->cards[i]->handle);
		virt_snd->cards[i]->start = start;
		start += virt_snd->cards[i]->count;
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&tid, &attr, virtio_sound_event_thread, (void *)virt_snd);
	return 0;
}

static int
virtio_sound_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_sound *virt_snd;
	pthread_mutexattr_t attr;
	int err;

	virt_snd = calloc(1, sizeof(struct virtio_sound));
	if (!virt_snd) {
		WPRINTF(("virtio_sound: calloc returns NULL\n"));
		return -1;
	}
	err = pthread_mutexattr_init(&attr);
	if (err) {
		WPRINTF("%s: mutexattr init failed with erro %d!\n", __func__, err);
		free(virt_snd);
		return -1;
	}
	err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (err) {
		WPRINTF("%s: mutexattr_settype failed with error %d.\n",
			       __func__, err);
		free(virt_snd);
		return -1;
	}
	err = pthread_mutex_init(&virt_snd->mtx, &attr);
	if (err) {
		WPRINTF("mutex init failed with error %d!\n", err);
		free(virt_snd);
		return -1;
	}

	virtio_linkup(&virt_snd->base,
		      &virtio_snd_ops,
		      virt_snd,
		      dev,
		      virt_snd->vq,
		      BACKEND_VBSU);

	virt_snd->base.mtx = &virt_snd->mtx;
	virt_snd->base.device_caps = VIRTIO_SND_S_HOSTCAPS;

	virt_snd->vq[0].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[1].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[2].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[3].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[0].notify = virtio_sound_notify_ctl;
	virt_snd->vq[1].notify = virtio_sound_notify_event;
	virt_snd->vq[2].notify = virtio_sound_notify_tx;
	virt_snd->vq[3].notify = virtio_sound_notify_rx;

	/* initialize config space */
	pci_set_cfgdata16(dev, PCIR_DEVICE, VIRTIO_TYPE_SOUND + 0x1040);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(dev, PCIR_CLASS, PCIC_MULTIMEDIA);
	pci_set_cfgdata8(dev, PCIR_SUBCLASS, PCIS_MULTIMEDIA_AUDIO);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_SOUND);
	if (is_winvm == true)
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, ORACLE_VENDOR_ID);
	else
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	if (virtio_interrupt_init(&virt_snd->base, virtio_uses_msix())) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return -1;
	}
	err = virtio_set_modern_bar(&virt_snd->base, false);
	if (err != 0) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return err;
	}

	virt_snd->stream_cnt = 0;
	virt_snd->chmap_cnt = 0;
	virt_snd->ctl_cnt = 0;
	virt_snd->jack_cnt = 0;
	virt_snd->card_cnt = 0;

	err = virtio_sound_parse_opts(virt_snd, opts);
	if (err != 0) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return err;
	}

	err = virtio_sound_event_init(virt_snd, opts);
	if (err != 0) {
		free(virt_snd);
		return err;
	}

	virtio_sound_cfg_init(virt_snd);
	virt_snd->status = VIRTIO_SND_BE_INITED;
	vsound = virt_snd;
    return 0;
}

static void
virtio_sound_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_sound *virt_snd = (struct virtio_sound *)dev->arg;
	int s, i;
	virt_snd->status = VIRTIO_SND_BE_DEINITED;
	for (s = 0; s < virt_snd->stream_cnt; s++) {
		if (virt_snd->streams[s]->handle && snd_pcm_close(virt_snd->streams[s]->handle) < 0) {
			WPRINTF("%s: stream %s close error!\n", __func__, virt_snd->streams[s]->dev_name);
		}
		pthread_mutex_destroy(&virt_snd->streams[s]->mtx);
		for (i = 0; i < virt_snd->streams[s]->chmap_cnt; i++) {
			free(virt_snd->streams[s]->chmaps[i]);
		}
		pthread_mutex_destroy(&virt_snd->streams[s]->ctl_mtx);
		free(virt_snd->streams[s]);
	}
	pthread_mutex_destroy(&virt_snd->mtx);
	for (i = 0; i < virt_snd->ctl_cnt; i++) {
		free(virt_snd->ctls[i]);
	}
	for (i = 0; i < virt_snd->jack_cnt; i++) {
		free(virt_snd->jacks[i]);
	}
	for (i = 0; i < virt_snd->card_cnt; i++) {
		snd_hctl_close(virt_snd->cards[i]->handle);
		free(virt_snd->cards[i]);
	}
	free(virt_snd);
}

struct pci_vdev_ops pci_ops_virtio_sound = {
	.class_name	= "virtio-sound",
	.vdev_init	= virtio_sound_init,
	.vdev_deinit	= virtio_sound_deinit,
	.vdev_barwrite	= virtio_pci_write,
	.vdev_barread	= virtio_pci_read
};

DEFINE_PCI_DEVTYPE(pci_ops_virtio_sound);
