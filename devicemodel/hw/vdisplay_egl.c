/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Virtual Display for VMs
 *
 */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <pixman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "log.h"
#include "vdisplay.h"
#include "atomic.h"
#include "timer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define VDPY_MAX_WIDTH 3840
#define VDPY_MAX_HEIGHT 2160
#define VDPY_DEFAULT_WIDTH 1024
#define VDPY_DEFAULT_HEIGHT 768
#define VDPY_MIN_WIDTH 640
#define VDPY_MIN_HEIGHT 480
#define transto_10bits(color) (uint16_t)(color * 1024 + 0.5)
#define VSCREEN_MAX_NUM VDPY_MAX_NUM
#define EDID_BASIC_BLOCK_SIZE 128
#define EDID_CEA861_EXT_BLOCK_SIZE 128

static unsigned char default_raw_argb[VDPY_DEFAULT_WIDTH * VDPY_DEFAULT_HEIGHT * 4];

struct state {
	bool is_ui_realized;
	bool is_active;
	bool is_wayland;
	bool is_x11;
	bool is_fullscreen;
	uint64_t updates;
	int n_connect;
};

struct egl_display_ops {
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
	PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES;
	PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES;
	PFNGLDELETEVERTEXARRAYSOESPROC glDeleteVertexArraysOES;
};

typedef struct{
  short x, y;
  short w, h;
} SDL_Rect;

struct vscreen {
	struct display_info info;
	int pscreen_id;
	SDL_Rect pscreen_rect;
	bool is_fullscreen;
	int org_x;
	int org_y;
	int width; // sdl window width
	int height; // sdl window height
	int guest_width; // image/tex width
	int guest_height; // image/tex width
	struct surface surf;
	struct cursor cur;
	uint64_t modifier;
	GLuint surf_tex;
	GLuint cur_tex;
	GLuint bogus_tex;
	int surf_format;
	int surf_updates;
	int cur_updates;
	pixman_image_t *img;
	EGLImage egl_img;
	/* Record the update_time that is activated from guest_vm */
	struct timespec last_time;
};

static struct display {
	struct state s;
	struct vscreen *vscrs;
	int vscrs_num;
	pthread_t tid;
	/* Add one UI_timer(33ms) to render the buffers from guest_vm */
	struct acrn_timer ui_timer;
	struct vdpy_display_bh ui_timer_bh;
	// protect the request_list
	pthread_mutex_t vdisplay_mutex;
	// receive the signal that request is submitted
	pthread_cond_t  vdisplay_signal;
	TAILQ_HEAD(display_list, vdpy_display_bh) request_list;
	/* add the below two fields for calling eglAPI directly */
	bool egl_dmabuf_supported;
	// SDL_GLContext eglContext;
	EGLContext eglContext;
	EGLDisplay eglDisplay;
    EGLSurface eglSurface;
	struct egl_display_ops gl_ops;
} vdpy = {
	.s.is_ui_realized = false,
	.s.is_active = false,
	.s.is_wayland = false,
	.s.is_x11 = false,
	.s.n_connect = 0,
	.eglDisplay = EGL_NO_DISPLAY,
    .eglContext = EGL_NO_CONTEXT,    
	.eglSurface = EGL_NO_SURFACE
};

typedef enum {
	ESTT = 1, // Established Timings I & II
	STDT,    // Standard Timings
	ESTT3,   // Established Timings III
	CEA861,	// CEA-861 Timings
} TIMING_MODE;

static const struct timing_entry {
	uint32_t hpixel;// Horizontal pixels
	uint32_t vpixel;// Vertical pixels
	uint32_t byte;  // byte idx in the Established Timings I & II
	uint32_t byte_t3;// byte idx in the Established Timings III Descriptor
	uint32_t bit;   // bit idx
	uint8_t hz;     // frequency
	bool	is_std; // the flag of standard mode
	bool	is_cea861; // CEA-861 timings
	uint8_t vic;	// Video Indentification Code
} timings[] = {
	/* Established Timings I & II (all @ 60Hz) */
	{ .hpixel = 1280, .vpixel = 1024, .byte  = 36, .bit = 0, .hz = 75},
	{ .hpixel = 1024, .vpixel =  768, .byte  = 36, .bit = 1, .hz = 75},
	{ .hpixel = 1024, .vpixel =  768, .byte  = 36, .bit = 3, .hz = 60},
	{ .hpixel =  800, .vpixel =  600, .byte  = 35, .bit = 0, .hz = 60 },
	{ .hpixel =  640, .vpixel =  480, .byte  = 35, .bit = 5, .hz = 60 },

	/* Standard Timings */
	{ .hpixel = 1920, .vpixel = 1080, .hz = 60,  .is_std = true },
	{ .hpixel = 1680, .vpixel = 1050, .hz = 60,  .is_std = true },
	{ .hpixel = 1600, .vpixel = 1200, .hz = 60,  .is_std = true },
	{ .hpixel = 1600, .vpixel =  900, .hz = 60,  .is_std = true },
	{ .hpixel = 1440, .vpixel =  900, .hz = 60,  .is_std = true },

	/* CEA-861 Timings */
	{ .hpixel = 3840, .vpixel = 2160, .hz = 60,  .is_cea861 = true, .vic = 97 },
};

typedef struct frame_param{
	uint32_t hav_pixel;     // Horizontal Addressable Video in pixels
	uint32_t hb_pixel;      // Horizontal Blanking in pixels
	uint32_t hfp_pixel;     // Horizontal Front Porch in pixels
	uint32_t hsp_pixel;     // Horizontal Sync Pulse Width in pixels
	uint32_t lhb_pixel;     // Left Horizontal Border or Right Horizontal
	                        // Border in pixels

	uint32_t vav_line;      // Vertical Addressable Video in lines
	uint32_t vb_line;       // Vertical Blanking in lines
	uint32_t vfp_line;      // Vertical Front Porch in Lines
	uint32_t vsp_line;      // Vertical Sync Pulse Width in Lines
	uint32_t tvb_line;      // Top Vertical Border or Bottom Vertical
	                        // Border in Lines

	uint64_t pixel_clock;   // Hz
	uint32_t width;         // mm
	uint32_t height;        // mm
}frame_param;

typedef struct base_param{
	uint32_t h_pixel;       // pixels
	uint32_t v_pixel;       // lines
	uint32_t rate;          // Hz
	uint32_t width;         // mm
	uint32_t height;        // mm

	const char *id_manuf;   // ID Manufacturer Name, ISA 3-character ID Code
	uint16_t id_product;    // ID Product Code
	uint32_t id_sn;         // ID Serial Number and it is a number only.

	const char *sn;         // Serial number.
	const char *product_name;// Product name.
}base_param;

static void
vdpy_edid_set_baseparam(base_param *b_param, uint32_t width, uint32_t height)
{
	b_param->h_pixel = width;
	b_param->v_pixel = height;
	b_param->rate = 60;
	b_param->width = width;
	b_param->height = height;

	b_param->id_manuf = "ACRN";
	b_param->id_product = 4321;
	b_param->id_sn = 12345678;

	b_param->sn = "A0123456789";
	b_param->product_name = "ACRN_Monitor";
}

static void
vdpy_edid_set_frame(frame_param *frame, const base_param *b_param)
{
	frame->hav_pixel = b_param->h_pixel;
	frame->hb_pixel = b_param->h_pixel * 35 / 100;
	frame->hfp_pixel = b_param->h_pixel * 25 / 100;
	frame->hsp_pixel = b_param->h_pixel * 3 / 100;
	frame->lhb_pixel = 0;
	frame->vav_line = b_param->v_pixel;
	frame->vb_line = b_param->v_pixel * 35 / 1000;
	frame->vfp_line = b_param->v_pixel * 5 / 1000;
	frame->vsp_line = b_param->v_pixel * 5 / 1000;
	frame->tvb_line = 0;
	frame->pixel_clock = b_param->rate *
			(frame->hav_pixel + frame->hb_pixel + frame->lhb_pixel * 2) *
			(frame->vav_line + frame->vb_line + frame->tvb_line * 2);
	frame->width = b_param->width;
	frame->height = b_param->height;
}

static void
vdpy_edid_set_color(uint8_t *edid, float red_x, float red_y,
				   float green_x, float green_y,
				   float blue_x, float blue_y,
				   float white_x, float white_y)
{
	uint8_t *color;
	uint16_t rx, ry, gx, gy, bx, by, wx, wy;

	rx = transto_10bits(red_x);
	ry = transto_10bits(red_y);
	gx = transto_10bits(green_x);
	gy = transto_10bits(green_y);
	bx = transto_10bits(blue_x);
	by = transto_10bits(blue_y);
	wx = transto_10bits(white_x);
	wy = transto_10bits(white_y);

	color = edid + 25;
	color[0] = ((rx & 0x03) << 6) |
		   ((ry & 0x03) << 4) |
		   ((gx & 0x03) << 2) |
		    (gy & 0x03);
	color[1] = ((bx & 0x03) << 6) |
		   ((by & 0x03) << 4) |
		   ((wx & 0x03) << 2) |
		    (wy & 0x03);
	color[2] = rx >> 2;
	color[3] = ry >> 2;
	color[4] = gx >> 2;
	color[5] = gy >> 2;
	color[6] = bx >> 2;
	color[7] = by >> 2;
	color[8] = wx >> 2;
	color[9] = wy >> 2;
}

static uint8_t
vdpy_edid_set_timing(uint8_t *addr, TIMING_MODE mode)
{
	static uint16_t idx;
	static uint16_t size;
	const struct timing_entry *timing;
	uint8_t stdcnt;
	uint16_t hpixel;
	int16_t AR;
	uint8_t num_timings;

	stdcnt = 0;
	num_timings = 0;

	if(mode == STDT) {
		addr += 38;
	}

	idx = 0;
	size = sizeof(timings) / sizeof(timings[0]);
	for(; idx < size; idx++){
		timing = timings + idx;

		switch(mode){
		case ESTT: // Established Timings I & II
			if(timing->byte) {
				addr[timing->byte] |= (1 << timing->bit);
				break;
			} else {
				continue;
			}
		case ESTT3: // Established Timings III
			if(timing->byte_t3){
				addr[timing->byte_t3] |= (1 << timing->bit);
				break;
			} else {
				continue;
			}
		case STDT: // Standard Timings
			if(stdcnt < 8 && timing->is_std) {
				hpixel = (timing->hpixel >> 3) - 31;
				if (timing->hpixel == 0 ||
				    timing->vpixel == 0) {
					AR = -1;
				} else if (hpixel & 0xff00) {
					AR = -2;
				} else if (timing->hpixel * 10 ==
				    timing->vpixel * 16) {
					AR = 0;
				} else if (timing->hpixel * 3 ==
				    timing->vpixel * 4) {
					AR = 1;
				} else if (timing->hpixel * 4 ==
				    timing->vpixel * 5) {
					AR = 2;
				} else if (timing->hpixel * 9 ==
				    timing->vpixel * 16) {
					AR = 3;
				} else {
					AR = -2;
				}
				if (AR >= 0) {
					addr[0] = hpixel & 0xff;
					addr[1] = (AR << 6) | ((timing->hz - 60) & 0x3f);
					addr += 2;
					stdcnt++;
				} else if (AR == -1){
					addr[0] = 0x01;
					addr[1] = 0x01;
					addr += 2;
					stdcnt++;
				}
				break;
			} else {
				continue;
			}
			break;
		case CEA861: // CEA-861 Timings
			if (timing->is_cea861) {
				addr[0] = timing->vic;
				addr += 1;
				num_timings++;
			}
			break;
		default:
			continue;
		}
	}
	while(mode == STDT && stdcnt < 8){
		addr[0] = 0x01;
		addr[1] = 0x01;
		addr += 2;
		stdcnt++;
	}

	return num_timings;
}

static void
vdpy_edid_set_dtd(uint8_t *dtd, const frame_param *frame)
{
	uint16_t pixel_clk;

	if ((frame->pixel_clock / 10000) > 65535) {
		/*
		 * Large screen. The pixel_clock won't fit in two bytes.
		 * We fill in a dummy DTD here and OS will pick up PTM
		 * from extension block.
		 */
		dtd[3] = 0x10; /* Tag 0x10: Dummy descriptor */
		return;
	}

	// Range: 10 kHz to 655.35 MHz in 10 kHz steps
	pixel_clk = frame->pixel_clock / 10000;
	memcpy(dtd, &pixel_clk, sizeof(pixel_clk));
	dtd[2] = frame->hav_pixel & 0xff;
	dtd[3] = frame->hb_pixel & 0xff;
	dtd[4] = ((frame->hav_pixel & 0xf00) >> 4) |
		 ((frame->hb_pixel & 0xf00) >> 8);
	dtd[5] = frame->vav_line & 0xff;
	dtd[6] = frame->vb_line & 0xff;
	dtd[7] = ((frame->vav_line & 0xf00) >> 4) |
		 ((frame->vb_line & 0xf00) >> 8);
	dtd[8] = frame->hfp_pixel & 0xff;
	dtd[9] = frame->hsp_pixel & 0xff;
	dtd[10] = ((frame->vfp_line & 0xf) << 4) |
		   (frame->vsp_line & 0xf);
	dtd[11] = ((frame->hfp_pixel & 0x300) >> 2) |
		  ((frame->hsp_pixel & 0x300) >> 4) |
		  ((frame->vfp_line & 0x030) >> 6) |
		  ((frame->vsp_line & 0x030) >> 8);
	dtd[12] = frame->width & 0xff;
	dtd[13] = frame->height & 0xff;
	dtd[14] = ((frame->width & 0xf00) >> 4) |
		  ((frame->height & 0xf00) >> 8);
	dtd[15] = frame->lhb_pixel & 0xff;
	dtd[16] = frame->tvb_line & 0xff;
	dtd[17] = 0x18;
}

static void
vdpy_edid_set_descripter(uint8_t *desc, uint8_t is_dtd,
		uint8_t tag, const base_param *b_param)
{
	frame_param frame;
	const char* text;
	uint16_t len;


	if (is_dtd) {
		vdpy_edid_set_frame(&frame, b_param);
		vdpy_edid_set_dtd(desc, &frame);
		return;
	}
	desc[3] = tag;
	text = NULL;
	switch(tag){
	// Established Timings III Descriptor (tag #F7h)
	case 0xf7:
		desc[5] = 0x0a; // Revision Number
		vdpy_edid_set_timing(desc, ESTT3);
		break;
	// Display Range Limits & Additional Timing Descriptor (tag #FDh)
	case 0xfd:
		desc[5] =  50; // Minimum Vertical Rate. (50 -> 125 Hz)
		desc[6] = 125; // Maximum Vertical Rate.
		desc[7] =  30; // Minimum Horizontal Rate.(30 -> 160 kHz)
		desc[8] = 160; // Maximum Horizontal Rate.
		desc[9] = 2550 / 10; // Max Pixel Clock. (2550 MHz)
		desc[10] = 0x01; // no extended timing information
		desc[11] = '\n'; // padding
		break;
	// Display Product Name (ASCII) String Descriptor (tag #FCh)
	case 0xfc:
	// Display Product Serial Number Descriptor (tag #FFh)
	case 0xff:
		text = (tag == 0xff) ? b_param->sn : b_param->product_name;
		memset(desc + 5, ' ', 13);
		if (text == NULL)
			break;
		len = strlen(text);
		if (len > 12)
			len = 12;
		memcpy(desc + 5, text, len);
		desc[len + 5] = '\n';
		break;
	// Dummy Descriptor (Tag #10h)
	case 0x10:
	default:
		break;
	}
}

static uint8_t
vdpy_edid_get_checksum(uint8_t *edid)
{
	uint8_t sum;
	int i;

	sum = 0;
	for (i = 0; i < 127; i++) {
		sum += edid[i];
	}

	return 0x100 - sum;
}

static void
vdpy_edid_generate(uint8_t *edid, size_t size, struct edid_info *info)
{
	uint16_t id_manuf;
	uint16_t id_product;
	uint32_t serial;
	uint8_t *desc;
	base_param b_param;
	uint8_t num_cea_timings;

	vdpy_edid_set_baseparam(&b_param, info->prefx, info->prefy);

	memset(edid, 0, size);
	/* edid[7:0], fixed header information, (00 FF FF FF FF FF FF 00)h */
	memset(edid + 1, 0xff, 6);

	/* edid[17:8], Vendor & Product Identification */
	// Manufacturer ID is a big-endian 16-bit value.
	id_manuf = ((((b_param.id_manuf[0] - '@') & 0x1f) << 10) |
		    (((b_param.id_manuf[1] - '@') & 0x1f) << 5) |
		    (((b_param.id_manuf[2] - '@') & 0x1f) << 0));
	edid[8] = id_manuf >> 8;
	edid[9] = id_manuf & 0xff;

	// Manufacturer product code is a little-endian 16-bit number.
	id_product = b_param.id_product;
	memcpy(edid+10, &id_product, sizeof(id_product));

	// Serial number is a little-endian 32-bit value.
	serial = b_param.id_sn;
	memcpy(edid+12, &serial, sizeof(serial));

	edid[16] = 0;           // Week of Manufacture
	edid[17] = 2018 - 1990; // Year of Manufacture or Model Year.
				// Acrn is released in 2018.

	edid[18] = 1;   // Version Number
	edid[19] = 4;   // Revision Number

	/* edid[24:20], Basic Display Parameters & Features */
	// Video Input Definition: 1 Byte
	edid[20] = 0xa5; // Digital input;
			 // 8 Bits per Primary Color;
			 // DisplayPort is supported

	// Horizontal and Vertical Screen Size or Aspect Ratio: 2 Bytes
	// screen size, in centimetres
	edid[21] = info->prefx / 10;
	edid[22] = info->prefy / 10;

	// Display Transfer Characteristics (GAMMA): 1 Byte
	// Stored Value = (GAMMA x 100) - 100
	edid[23] = 120; // display gamma: 2.2

	// Feature Support: 1 Byte
	edid[24] = 0x06; // sRGB Standard is the default color space;
			 // Preferred Timing Mode includes the native
			 // pixel format and preferred.

	/* edid[34:25], Display x, y Chromaticity Coordinates */
	vdpy_edid_set_color(edid, 0.6400, 0.3300,
				  0.3000, 0.6000,
				  0.1500, 0.0600,
				  0.3127, 0.3290);

	/* edid[37:35], Established Timings */
	vdpy_edid_set_timing(edid, ESTT);

	/* edid[53:38], Standard Timings: Identification 1 -> 8 */
	vdpy_edid_set_timing(edid, STDT);

	/* edid[125:54], Detailed Timing Descriptor - 18 bytes x 4 */
	// Preferred Timing Mode
	desc = edid + 54;
	vdpy_edid_set_descripter(desc, 0x1, 0, &b_param);
	// Display Range Limits & Additional Timing Descriptor (tag #FDh)
	desc += 18;
	vdpy_edid_set_descripter(desc, 0, 0xfd, &b_param);
	// Display Product Name (ASCII) String Descriptor (tag #FCh)
	desc += 18;
	vdpy_edid_set_descripter(desc, 0, 0xfc, &b_param);
	// Display Product Serial Number Descriptor (tag #FFh)
	desc += 18;
	vdpy_edid_set_descripter(desc, 0, 0xff, &b_param);

	/* EDID[126], Extension Block Count */
	edid[126] = 0;  // no Extension Block

	/* Checksum */
	edid[127] = vdpy_edid_get_checksum(edid);

	if (size >= (EDID_BASIC_BLOCK_SIZE + EDID_CEA861_EXT_BLOCK_SIZE)) {
		edid[126] = 1;
		edid[127] = vdpy_edid_get_checksum(edid);

		// CEA EDID Extension
		edid[EDID_BASIC_BLOCK_SIZE + 0] = 0x02;
		// Revision Number
		edid[EDID_BASIC_BLOCK_SIZE + 1] = 0x03;
		// SVDs
		edid[EDID_BASIC_BLOCK_SIZE + 4] |= 0x02 << 5;
		desc = edid + EDID_BASIC_BLOCK_SIZE + 5;
		num_cea_timings = vdpy_edid_set_timing(desc, CEA861);
		edid[EDID_BASIC_BLOCK_SIZE + 4] |= num_cea_timings;
		edid[EDID_BASIC_BLOCK_SIZE + 2] |= 5 + num_cea_timings;

		desc = edid + EDID_BASIC_BLOCK_SIZE;
		edid[EDID_BASIC_BLOCK_SIZE + 127] = vdpy_edid_get_checksum(desc);
	}
}

void
vdpy_get_edid(int handle, int scanout_id, uint8_t *edid, size_t size)
{
	struct edid_info edid_info;
	struct vscreen *vscr;

	if (scanout_id >= vdpy.vscrs_num)
		return;

	vscr = vdpy.vscrs + scanout_id;

	if (handle == vdpy.s.n_connect) {
		edid_info.prefx = vscr->info.width;
		edid_info.prefy = vscr->info.height;
		edid_info.maxx = VDPY_MAX_WIDTH;
		edid_info.maxy = VDPY_MAX_HEIGHT;
	} else {
		edid_info.prefx = VDPY_DEFAULT_WIDTH;
		edid_info.prefy = VDPY_DEFAULT_HEIGHT;
		edid_info.maxx = VDPY_MAX_WIDTH;
		edid_info.maxy = VDPY_MAX_HEIGHT;
	}
	edid_info.refresh_rate = 0;
	edid_info.vendor = NULL;
	edid_info.name = NULL;
	edid_info.sn = NULL;

	vdpy_edid_generate(edid, size, &edid_info);
}

void
vdpy_get_display_info(int handle, int scanout_id, struct display_info *info)
{
	struct vscreen *vscr;

	if (scanout_id >= vdpy.vscrs_num)
		return;

	vscr = vdpy.vscrs + scanout_id;

	if (handle == vdpy.s.n_connect) {
		info->xoff = vscr->info.xoff;
		info->yoff = vscr->info.yoff;
		info->width = vscr->info.width;
		info->height = vscr->info.height;
	} else {
		info->xoff = 0;
		info->yoff = 0;
		info->width = 0;
		info->height = 0;
	}
}

static void
sdl_gl_display_init(void)
{	
	struct egl_display_ops *gl_ops = &vdpy.gl_ops;
	struct vscreen *vscr;
	int i;

	/* obtain the eglDisplay/eglContext */
	vdpy.eglDisplay = eglGetCurrentDisplay();

	/* Try to use the eglGetProcaddress to obtain callback API for
	 * eglCreateImageKHR/eglDestroyImageKHR
	 * glEGLImageTargetTexture2DOES
	 */
	gl_ops->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
				eglGetProcAddress("eglCreateImageKHR");
	gl_ops->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
				eglGetProcAddress("eglDestroyImageKHR");
	gl_ops->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
				eglGetProcAddress("glEGLImageTargetTexture2DOES");
	gl_ops->glGenVertexArraysOES = (PFNGLGENVERTEXARRAYSOESPROC)
				eglGetProcAddress("glGenVertexArraysOES");
	gl_ops->glBindVertexArrayOES = (PFNGLBINDVERTEXARRAYOESPROC)
				eglGetProcAddress("glBindVertexArrayOES");
	gl_ops->glDeleteVertexArraysOES = (PFNGLDELETEVERTEXARRAYSOESPROC)
				eglGetProcAddress("glDeleteVertexArraysOES");

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		vscr->egl_img = EGL_NO_IMAGE_KHR;
	}

	if ((gl_ops->eglCreateImageKHR == NULL) ||
		(gl_ops->eglDestroyImageKHR == NULL) ||
		(gl_ops->glEGLImageTargetTexture2DOES == NULL)) {
		pr_info("DMABuf is not supported.\n");
		vdpy.egl_dmabuf_supported = false;
	} else
		vdpy.egl_dmabuf_supported = true;

	return;
}

static int egl_render_copy(GLuint src_tex,
                   const SDL_Rect * dstrect)
{
	struct egl_display_ops *gl_ops = &vdpy.gl_ops;
	// Create a VAO.
	GLuint vao;
	gl_ops->glGenVertexArraysOES(1, &vao);
	gl_ops->glBindVertexArrayOES(vao);

	// Create a VBO.
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);

	// Set the vertex and texture coordinates for the rectangle.
	GLfloat vertices[] = {
	0.0f, 0.0f, 0.0f, 0.0f,
	1.0f, 0.0f, 1.0f, 0.0f,
	1.0f, 1.0f, 1.0f, 1.0f,
	0.0f, 1.0f, 0.0f, 1.0f
	};
	GLfloat vertices2[] = {
	dstrect->x, dstrect->y, 0.0f, 0.0f,
	dstrect->x + dstrect->w, dstrect->y, 1.0f, 0.0f,
	dstrect->x + dstrect->w, dstrect->y + dstrect->h, 1.0f, 1.0f,
	dstrect->x, dstrect->y + dstrect->h, 0.0f, 1.0f
	};

	// Copy the vertex data to the VBO.
	if (!dstrect)
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	else
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices2), vertices2, GL_STATIC_DRAW);

	// Set the vertex attribute pointer.
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
	glEnableVertexAttribArray(0);

	// Set the texture attribute pointer.
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
	glEnableVertexAttribArray(1);

	// Unbind the VBO and VAO.
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	gl_ops->glBindVertexArrayOES(0);

	// Bind the texture to the active texture unit.
	glBindTexture(GL_TEXTURE_2D, src_tex);

	// Bind the VAO.
	gl_ops->glBindVertexArrayOES(vao);

	// Draw the rectangle.
	glDrawArrays(GL_TRIANGLES, 0, 4);

	// Disable the vertex attribute arrays.
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	// Unbind the VAO.
	gl_ops->glBindVertexArrayOES(0);
	gl_ops->glDeleteVertexArraysOES(1, &vao);
	return 0;
}

// to Replace SDL_CreateTexture
static int egl_create_tex(GLuint *texid, uint32_t format,
                                int w, int h)
{
    glGenTextures(1, texid);
    glBindTexture(GL_TEXTURE_2D, *texid);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, w, h, 0, format, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return 0;
}

static int egl_create_dma_tex(GLuint *texid)
{
    glGenTextures(1, texid);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, *texid);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return 0;
}

// to Replace SDL_UpdateTexture
static int egl_update_tex(GLuint texid,
                      const SDL_Rect * rect, int format,
                      const void *pixels)
{
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexSubImage2D(GL_TEXTURE_2D, 0,
					 rect->x, rect->y, rect->w, rect->h,
					 format, GL_UNSIGNED_BYTE, pixels);
	return 0;
}

static void sdl_gl_prepare_draw(struct vscreen *vscr)
{
	SDL_Rect bogus_rect;

	if (vscr == NULL)
		return;

	bogus_rect.x = 0;
	bogus_rect.y = 0;
	bogus_rect.w = 32;
	bogus_rect.h = 32;
	/* The limitation in libSDL causes that ACRN can't display framebuffer
	 * correctly on one window when using multi SDL_context to displaying
	 * the framebuffers under multi-display scenario.
	 * The small texture is added to workaround the display issue caused by
	 * libSDL limitation.
	 * Todo: Keep monitoring the libSDL to check whether the limitation is
	 * fixed.
	 */
	// SDL_RenderClear(vscr->renderer);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// SDL_RenderCopy(vscr->renderer, vscr->bogus_tex, NULL, &bogus_rect);
	egl_render_copy(vscr->bogus_tex, &bogus_rect);
	return;
}

void
vdpy_set_modifier(int handle __attribute__((unused)), int scanout_id, uint64_t modifier)
{
	struct vscreen *vscr;

	if (scanout_id >= vdpy.vscrs_num) {
		return;
	}

	vscr = vdpy.vscrs + scanout_id;
	vscr->modifier = modifier;
}

void
vdpy_surface_set(int handle, int scanout_id, struct surface *surf)
{
	pixman_image_t *src_img;
	int format;
	int i;
	struct vscreen *vscr;

	if (handle != vdpy.s.n_connect) {
		return;
	}

	if (vdpy.tid != pthread_self()) {
		pr_err("%s: unexpected code path as unsafe 3D ops in multi-threads env.\n",
			__func__);
		return;
	}

	if (scanout_id >= vdpy.vscrs_num) {
		return;
	}

	vscr = vdpy.vscrs + scanout_id;

	if (surf == NULL ) {
		vscr->surf.width = 0;
		vscr->surf.height = 0;
		/* Need to use the default 640x480 for the SDL_Texture */
		src_img = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			VDPY_MIN_WIDTH, VDPY_MIN_HEIGHT,
			(uint32_t *)default_raw_argb,
			VDPY_MIN_WIDTH * 4);
		if (src_img == NULL) {
			pr_err("failed to create pixman_image\n");
			return;
		}
		vscr->guest_width = VDPY_MIN_WIDTH;
		vscr->guest_height = VDPY_MIN_HEIGHT;
	} else if (surf->surf_type == SURFACE_PIXMAN) {
		src_img = pixman_image_create_bits(surf->surf_format,
			surf->width, surf->height, surf->pixel,
			surf->stride);
		if (src_img == NULL) {
			pr_err("failed to create pixman_image\n");
			return;
		}
		vscr->surf = *surf;
		vscr->guest_width = surf->width;
		vscr->guest_height = surf->height;
	} else if (surf->surf_type == SURFACE_DMABUF) {
		src_img = NULL;
		vscr->surf = *surf;
		vscr->guest_width = surf->width;
		vscr->guest_height = surf->height;
	} else {
		/* Unsupported type */
		return;
	}

	if (vscr->surf_tex) {
		// SDL_DestroyTexture(vscr->surf_tex);
		glDeleteTextures(1, &vscr->surf_tex);
	}
	if (surf && (surf->surf_type == SURFACE_DMABUF)) {
		// access = SDL_TEXTUREACCESS_STATIC;
		// format = SDL_PIXELFORMAT_EXTERNAL_OES;
		vscr->surf_format = GL_BGRA_EXT; // this value is not used for SURFACE_DMABUF, set an arbitrary value
		egl_create_dma_tex(&vscr->surf_tex);
	} else {
		// access = SDL_TEXTUREACCESS_STREAMING;
		// format = SDL_PIXELFORMAT_ARGB8888;
		format = GL_BGRA_EXT;
		switch (pixman_image_get_format(src_img)) {
		case PIXMAN_a8r8g8b8:
		case PIXMAN_x8r8g8b8:
			// format = SDL_PIXELFORMAT_ARGB8888;
			format = GL_BGRA_EXT; //GL_ARGB;
			break;
		case PIXMAN_a8b8g8r8:
		case PIXMAN_x8b8g8r8:
			// format = SDL_PIXELFORMAT_ABGR8888;
			format = GL_RGBA; //GL_ABGR;
			break;
		case PIXMAN_r8g8b8a8:
			// format = SDL_PIXELFORMAT_RGBA8888;
			format = GL_RGBA; //GL_RGBA;
		case PIXMAN_r8g8b8x8:
			// format = SDL_PIXELFORMAT_RGBX8888;
			format = GL_RGBA; //GL_RGBX;
			break;
		case PIXMAN_b8g8r8a8:
		case PIXMAN_b8g8r8x8:
			// format = SDL_PIXELFORMAT_RGBX8888;
			format = GL_RGBA; //GL_RGBX;
			break;
		default:
			pr_err("Unsupported format. %x\n",
					pixman_image_get_format(src_img));
		}

		vscr->surf_format = format;
		egl_create_tex(&vscr->surf_tex, format,
						vscr->guest_width, vscr->guest_height);
	}
	// vscr->surf_tex = SDL_CreateTexture(vscr->renderer,
	// 		format, access,
	// 		vscr->guest_width, vscr->guest_height);

	// if (vscr->surf_tex == NULL) {
	// 	pr_err("Failed to create SDL_texture for surface.\n");
	// }


	/* For the surf_switch, it will be updated in surface_update */
	if (!surf) {
		// SDL_UpdateTexture(vscr->surf_tex, NULL,
		// 		  pixman_image_get_data(src_img),
		// 		  pixman_image_get_stride(src_img));
		SDL_Rect rt = {0, 0, vscr->guest_width, vscr->guest_height};
		egl_update_tex(vscr->surf_tex,
                      &rt, format,
                      pixman_image_get_data(src_img));
		sdl_gl_prepare_draw(vscr);
		// SDL_RenderCopy(vscr->renderer, vscr->surf_tex, NULL, NULL);
		egl_render_copy(vscr->surf_tex, NULL);
		// SDL_RenderPresent(vscr->renderer);
		eglSwapBuffers(vdpy.eglDisplay, vdpy.eglSurface);
	} else if (surf->surf_type == SURFACE_DMABUF) {
		EGLImageKHR egl_img = EGL_NO_IMAGE_KHR;
		EGLint attrs[64];
		struct egl_display_ops *gl_ops;

		gl_ops = &vdpy.gl_ops;
		i = 0;
		attrs[i++] = EGL_WIDTH;
		attrs[i++] = surf->width;
		attrs[i++] = EGL_HEIGHT;
		attrs[i++] = surf->height;
		attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
		attrs[i++] = surf->dma_info.surf_fourcc;
		attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
		attrs[i++] = surf->dma_info.dmabuf_fd;
		attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
		attrs[i++] = surf->stride;
		attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
		attrs[i++] = surf->dma_info.dmabuf_offset;
		if (vscr->modifier) {
			attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
			attrs[i++] = vscr->modifier & 0xffffffff;
			attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
			attrs[i++] = (vscr->modifier & 0xffffffff00000000) >> 32;
		}
		attrs[i++] = EGL_NONE;

		egl_img = gl_ops->eglCreateImageKHR(vdpy.eglDisplay,
				EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT,
				NULL, attrs);
		if (egl_img == EGL_NO_IMAGE_KHR) {
			pr_err("Failed in eglCreateImageKHR.\n");
			return;
		}

		// SDL_GL_BindTexture(vscr->surf_tex, NULL, NULL);
		glBindTexture(GL_TEXTURE_2D, vscr->surf_tex);
		gl_ops->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_img);
		if (vscr->egl_img != EGL_NO_IMAGE_KHR)
			gl_ops->eglDestroyImageKHR(vdpy.eglDisplay,
					vscr->egl_img);

		/* In theory the created egl_img can be released after it is bound
		 * to texture.
		 * Now it is released next time so that it is controlled correctly
		 */
		vscr->egl_img = egl_img;
	}

	if (vscr->img)
		pixman_image_unref(vscr->img);

	// if (surf == NULL) {
	// 	SDL_SetWindowTitle(vscr->win,
	// 			"Not activate display yet!");
	// } else {
	// 	SDL_SetWindowTitle(vscr->win,
	// 			"ACRN Virtual Monitor");
	// }
	/* Replace the cur_img with the created_img */
	vscr->img = src_img;
}

void
vdpy_cursor_position_transformation(struct display *vdpy, int scanout_id, SDL_Rect *rect)
{
	struct vscreen *vscr;

	if (scanout_id >= vdpy->vscrs_num) {
		return;
	}

	vscr = vdpy->vscrs + scanout_id;
	rect->x = (vscr->cur.x * vscr->width) / vscr->guest_width;
	rect->y = (vscr->cur.y * vscr->height) / vscr->guest_height;
	rect->w = (vscr->cur.width * vscr->width) / vscr->guest_width;
	rect->h = (vscr->cur.height * vscr->height) / vscr->guest_height;
}

void
vdpy_surface_update(int handle, int scanout_id, struct surface *surf)
{
	SDL_Rect cursor_rect;
	struct vscreen *vscr;

	if (handle != vdpy.s.n_connect) {
		return;
	}

	if (vdpy.tid != pthread_self()) {
		pr_err("%s: unexpected code path as unsafe 3D ops in multi-threads env.\n",
			__func__);
		return;
	}

	if (!surf) {
		pr_err("Incorrect order of submitting Virtio-GPU cmd.\n");
	        return;
	}

	if (scanout_id >= vdpy.vscrs_num) {
		return;
	}

	vscr = vdpy.vscrs + scanout_id;
	// if (surf->surf_type == SURFACE_PIXMAN)
	// 	SDL_UpdateTexture(vscr->surf_tex, NULL,
	// 		  surf->pixel,
	// 		  surf->stride);
			  
	if (surf->surf_type == SURFACE_PIXMAN) {
		SDL_Rect rt = {0, 0, vscr->guest_width, vscr->guest_height};
		egl_update_tex(vscr->surf_tex,
                      &rt, vscr->surf_format,
                      surf->pixel);
	}

	sdl_gl_prepare_draw(vscr);
	// SDL_RenderCopy(vscr->renderer, vscr->surf_tex, NULL, NULL);
	egl_render_copy(vscr->surf_tex, NULL);

	/* This should be handled after rendering the surface_texture.
	 * Otherwise it will be hidden
	 */
	if (vscr->cur_tex) {
		vdpy_cursor_position_transformation(&vdpy, scanout_id, &cursor_rect);
		// SDL_RenderCopy(vscr->renderer, vscr->cur_tex,
		// 		NULL, &cursor_rect);
		egl_render_copy(vscr->cur_tex, &cursor_rect);
	}

	// SDL_RenderPresent(vscr->renderer);
	eglSwapBuffers(vdpy.eglDisplay, vdpy.eglSurface);

	/* update the rendering time */
	clock_gettime(CLOCK_MONOTONIC, &vscr->last_time);
}

void
vdpy_cursor_define(int handle, int scanout_id, struct cursor *cur)
{
	struct vscreen *vscr;
	SDL_Rect rt = {0, 0, cur->width, cur->height};

	if (handle != vdpy.s.n_connect) {
		return;
	}

	if (vdpy.tid != pthread_self()) {
		pr_err("%s: unexpected code path as unsafe 3D ops in multi-threads env.\n",
			__func__);
		return;
	}

	if (scanout_id >= vdpy.vscrs_num) {
		return;
	}

	if (cur->data == NULL)
		return;

	vscr = vdpy.vscrs + scanout_id;

	if (vscr->cur_tex)
		// SDL_DestroyTexture(vscr->cur_tex);
		glDeleteTextures(1, &vscr->cur_tex);

	// vscr->cur_tex = SDL_CreateTexture(
	// 		vscr->renderer,
	// 		SDL_PIXELFORMAT_ARGB8888,
	// 		SDL_TEXTUREACCESS_STREAMING,
	// 		cur->width, cur->height);
	// if (vscr->cur_tex == NULL) {
	// 	pr_err("Failed to create sdl_cursor surface for %p.\n", cur);
	// 	return;
	// }
	egl_create_tex(&vscr->cur_tex, GL_BGRA_EXT, cur->width, cur->height);

	// SDL_SetTextureBlendMode(vscr->cur_tex, SDL_BLENDMODE_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);

	vscr->cur = *cur;
	// SDL_UpdateTexture(vscr->cur_tex, NULL, cur->data, cur->width * 4);
	egl_update_tex(vscr->cur_tex, &rt, GL_BGRA_EXT, cur->data);
	glDisable(GL_BLEND);
}

void
vdpy_cursor_move(int handle, int scanout_id, uint32_t x, uint32_t y)
{
	struct vscreen *vscr;

	if (handle != vdpy.s.n_connect) {
		return;
	}

	if (scanout_id >= vdpy.vscrs_num) {
		return;
	}

	vscr = vdpy.vscrs + scanout_id;
	/* Only move the position of the cursor. The cursor_texture
	 * will be handled in surface_update
	 */
	vscr->cur.x = x;
	vscr->cur.y = y;
}

static void
vdpy_sdl_ui_refresh(void *data)
{
	struct display *ui_vdpy;
	struct timespec cur_time;
	uint64_t elapsed_time;
	SDL_Rect cursor_rect;
	struct vscreen *vscr;
	int i;

	ui_vdpy = (struct display *)data;

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = ui_vdpy->vscrs + i;

		/* Skip it if no surface needs to be rendered */
		if (!vscr->surf_tex)
			continue;

		clock_gettime(CLOCK_MONOTONIC, &cur_time);

		elapsed_time = (cur_time.tv_sec - vscr->last_time.tv_sec) * 1000000000 +
				cur_time.tv_nsec - vscr->last_time.tv_nsec;

		/* the time interval is less than 10ms. Skip it */
		if (elapsed_time < 10000000)
			return;

		sdl_gl_prepare_draw(vscr);
		// SDL_RenderCopy(vscr->renderer, vscr->surf_tex, NULL, NULL);
		egl_render_copy(vscr->surf_tex, NULL);

		/* This should be handled after rendering the surface_texture.
		 * Otherwise it will be hidden
		 */
		if (vscr->cur_tex) {
			vdpy_cursor_position_transformation(ui_vdpy, i, &cursor_rect);
			// SDL_RenderCopy(vscr->renderer, vscr->cur_tex,
			// 		NULL, &cursor_rect);
			egl_render_copy(vscr->cur_tex, &cursor_rect);
		}

		// SDL_RenderPresent(vscr->renderer);
		eglSwapBuffers(vdpy.eglDisplay, vdpy.eglSurface);
	}
}

static void
vdpy_sdl_ui_timer(void *data, uint64_t nexp  __attribute__((unused)))
{
	struct display *ui_vdpy;
	struct vdpy_display_bh *bh_task;

	ui_vdpy = (struct display *)data;

	/* Don't submit the display_request if another func already
	 * acquires the mutex.
	 * This is to optimize the mevent thread otherwise it needs
	 * to wait for some time.
	 */
	if (pthread_mutex_trylock(&ui_vdpy->vdisplay_mutex))
		return;

	bh_task = &ui_vdpy->ui_timer_bh;
	if ((bh_task->bh_flag & ACRN_BH_PENDING) == 0) {
		bh_task->bh_flag |= ACRN_BH_PENDING;
		TAILQ_INSERT_TAIL(&ui_vdpy->request_list, bh_task, link);
	}
	pthread_cond_signal(&ui_vdpy->vdisplay_signal);
	pthread_mutex_unlock(&ui_vdpy->vdisplay_mutex);
}

void
vdpy_calibrate_vscreen_geometry(struct vscreen *vscr)
{
	if (vscr->guest_width && vscr->guest_height) {
		/* clip the region between (640x480) and (3840x2160) */
		if (vscr->guest_width < VDPY_MIN_WIDTH)
			vscr->guest_width = VDPY_MIN_WIDTH;
		if (vscr->guest_width > VDPY_MAX_WIDTH)
			vscr->guest_width = VDPY_MAX_WIDTH;
		if (vscr->guest_height < VDPY_MIN_HEIGHT)
			vscr->guest_height = VDPY_MIN_HEIGHT;
		if (vscr->guest_height > VDPY_MAX_HEIGHT)
			vscr->guest_height = VDPY_MAX_HEIGHT;
	} else {
		/* the default window(1920x1080) is created with undefined pos
		 * when no geometry info is passed
		 */
		vscr->org_x = 0xFFFF;
		vscr->org_y = 0xFFFF;
		vscr->guest_width = VDPY_DEFAULT_WIDTH;
		vscr->guest_height = VDPY_DEFAULT_HEIGHT;
	}
}

int
vdpy_create_vscreen_window(struct vscreen *vscr)
{
	// uint32_t win_flags;

	// win_flags = SDL_WINDOW_OPENGL |
	// 	    	SDL_WINDOW_ALWAYS_ON_TOP |
	// 	    	SDL_WINDOW_SHOWN;
	// if (vscr->is_fullscreen) {
	// 	// win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	// 	vscr->org_x = vscr->pscreen_rect.x;
	// 	vscr->org_y = vscr->pscreen_rect.y;
	// 	vscr->width = vscr->pscreen_rect.w;
	// 	vscr->height = vscr->pscreen_rect.h;
	// } else {
	// 	vscr->width = vscr->guest_width;
	// 	vscr->height = vscr->guest_height;
	// }
	// vscr->win = NULL;
	// vscr->renderer = NULL;
	// vscr->img = NULL;
	// Zoom to width and height of pscreen is fullscreen enabled
	// vscr->win = SDL_CreateWindow("ACRN_DM",
	// 			vscr->org_x, vscr->org_y,
	// 			vscr->width, vscr->height,
	// 			win_flags);
	// if (vscr->win == NULL) {
	// 	pr_err("Failed to Create SDL_Window\n");
	// 	return -1;
	// }
	pr_info("SDL display bind to screen %d: [%d,%d,%d,%d].\n", vscr->pscreen_id,
			vscr->org_x, vscr->org_y, vscr->width, vscr->height);

	// vscr->renderer = SDL_CreateRenderer(vscr->win, -1, 0);
	// if (vscr->renderer == NULL) {
	// 	pr_err("Failed to Create GL_Renderer \n");
	// 	return -1;
	// }
	// vscr->bogus_tex = SDL_CreateTexture(vscr->renderer,
	// 			SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
	// 			32, 32);
	// if (vscr->bogus_tex == NULL) {
	// 	pr_err("%s: Failed to create SDL_Texture\n", __func__);
	// 	return -1;
	// }
	egl_create_tex(&vscr->bogus_tex, GL_BGRA_EXT, 32, 32);
	
	// SDL_SetTextureColorMod(vscr->bogus_tex, 0x80, 0x80, 0x80);
	return 0;
}
#if 0
static void *
vdpy_sdl_display_thread(void *data __attribute__((unused)))
{
	struct vdpy_display_bh *bh;
	struct itimerspec ui_timer_spec;

	struct vscreen *vscr;
	int i;

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
	
		vdpy_calibrate_vscreen_geometry(vscr);

		if (vdpy_create_vscreen_window(vscr)) {
			goto sdl_fail;
		}

		vscr->info.xoff = vscr->org_x;
		vscr->info.yoff = vscr->org_y;
		vscr->info.width = vscr->guest_width;
		vscr->info.height = vscr->guest_height;

		clock_gettime(CLOCK_MONOTONIC, &vscr->last_time);
	}
	sdl_gl_display_init();
	pthread_mutex_init(&vdpy.vdisplay_mutex, NULL);
	pthread_cond_init(&vdpy.vdisplay_signal, NULL);
	TAILQ_INIT(&vdpy.request_list);
	vdpy.s.is_active = 1;

	vdpy.ui_timer_bh.task_cb = vdpy_sdl_ui_refresh;
	vdpy.ui_timer_bh.data = &vdpy;
	vdpy.ui_timer.clockid = CLOCK_MONOTONIC;
	acrn_timer_init(&vdpy.ui_timer, vdpy_sdl_ui_timer, &vdpy);
	ui_timer_spec.it_interval.tv_sec = 0;
	ui_timer_spec.it_interval.tv_nsec = 33000000;
	/* Wait for 5s to start the timer */
	ui_timer_spec.it_value.tv_sec = 5;
	ui_timer_spec.it_value.tv_nsec = 0;
	/* Start one periodic timer to refresh UI based on 30fps */
	acrn_timer_settime(&vdpy.ui_timer, &ui_timer_spec);

	pr_info("SDL display thread is created\n");
	/* Begin to process the display_cmd after initialization */
	do {
		if (!vdpy.s.is_active) {
			pr_info("display is exiting\n");
			break;
		}
		pthread_mutex_lock(&vdpy.vdisplay_mutex);

		if (TAILQ_EMPTY(&vdpy.request_list))
			pthread_cond_wait(&vdpy.vdisplay_signal,
					  &vdpy.vdisplay_mutex);

		/* the bh_task is handled in vdisplay_mutex lock */
		while (!TAILQ_EMPTY(&vdpy.request_list)) {
			bh = TAILQ_FIRST(&vdpy.request_list);

			TAILQ_REMOVE(&vdpy.request_list, bh, link);

			bh->task_cb(bh->data);

			if (atomic_load(&bh->bh_flag) & ACRN_BH_FREE) {
				free(bh);
				bh = NULL;
			} else {
				/* free is owned by the submitter */
				atomic_store(&bh->bh_flag, ACRN_BH_DONE);
			}
		}

		pthread_mutex_unlock(&vdpy.vdisplay_mutex);
	} while (1);

	acrn_timer_deinit(&vdpy.ui_timer);
	/* SDL display_thread will exit because of DM request */
	pthread_mutex_destroy(&vdpy.vdisplay_mutex);
	pthread_cond_destroy(&vdpy.vdisplay_signal);

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->img) {
			pixman_image_unref(vscr->img);
			vscr->img = NULL;
		}
		/* Continue to thread cleanup */
		if (vscr->surf_tex) {
			// SDL_DestroyTexture(vscr->surf_tex);
			glDeleteTextures(1, &vscr->surf_tex);
			vscr->surf_tex = 0;
		}
		if (vscr->cur_tex) {
			// SDL_DestroyTexture(vscr->cur_tex);
			glDeleteTextures(1, &vscr->cur_tex);
			vscr->cur_tex = 0;
		}

		if (vdpy.egl_dmabuf_supported && (vscr->egl_img != EGL_NO_IMAGE_KHR))
			vdpy.gl_ops.eglDestroyImageKHR(vdpy.eglDisplay,
						vscr->egl_img);
	}

sdl_fail:
	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->bogus_tex) {
			// SDL_DestroyTexture(vscr->bogus_tex);
			glDeleteTextures(1, &vscr->bogus_tex);
			vscr->bogus_tex = 0;
		}
		// if (vscr->renderer) {
		// 	SDL_DestroyRenderer(vscr->renderer);
		// 	vscr->renderer = NULL;
		// }
		// if (vscr->win) {
		// 	SDL_DestroyWindow(vscr->win);
		// 	vscr->win = NULL;
		// }
	}

	/* This is used to workaround the TLS issue of libEGL + libGLdispatch
	 * after unloading library.
	 */
	eglReleaseThread();
	return NULL;
}
#endif
void *
vdpy_sdl_display_init()
{
	struct itimerspec ui_timer_spec;

	struct vscreen *vscr;
	int i;

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
	
		vdpy_calibrate_vscreen_geometry(vscr);

		// used passed in window handle, not created here
		if (vdpy_create_vscreen_window(vscr)) {
			goto sdl_fail;
		}

		vscr->info.xoff = vscr->org_x;
		vscr->info.yoff = vscr->org_y;
		vscr->info.width = vscr->guest_width;
		vscr->info.height = vscr->guest_height;

		clock_gettime(CLOCK_MONOTONIC, &vscr->last_time);
	}
	sdl_gl_display_init();
	pthread_mutex_init(&vdpy.vdisplay_mutex, NULL);
	pthread_cond_init(&vdpy.vdisplay_signal, NULL);
	TAILQ_INIT(&vdpy.request_list);
	vdpy.s.is_active = 1;

	vdpy.ui_timer_bh.task_cb = vdpy_sdl_ui_refresh;
	vdpy.ui_timer_bh.data = &vdpy;
	vdpy.ui_timer.clockid = CLOCK_MONOTONIC;
	acrn_timer_init(&vdpy.ui_timer, vdpy_sdl_ui_timer, &vdpy);
	ui_timer_spec.it_interval.tv_sec = 0;
	ui_timer_spec.it_interval.tv_nsec = 33000000;
	/* Wait for 5s to start the timer */
	ui_timer_spec.it_value.tv_sec = 5;
	ui_timer_spec.it_value.tv_nsec = 0;
	/* Start one periodic timer to refresh UI based on 30fps */
	acrn_timer_settime(&vdpy.ui_timer, &ui_timer_spec);

	pr_info("SDL display thread is created\n");
	return NULL;

sdl_fail:
	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->bogus_tex) {
			// SDL_DestroyTexture(vscr->bogus_tex);
			glDeleteTextures(1, &vscr->bogus_tex);
			vscr->bogus_tex = 0;
		}
		// if (vscr->renderer) {
		// 	SDL_DestroyRenderer(vscr->renderer);
		// 	vscr->renderer = NULL;
		// }
		// if (vscr->win) {
		// 	SDL_DestroyWindow(vscr->win);
		// 	vscr->win = NULL;
		// }
	}

	/* This is used to workaround the TLS issue of libEGL + libGLdispatch
	 * after unloading library.
	 */
	eglReleaseThread();
	return NULL;
}

void *
vdpy_sdl_display_term()
{
	struct vscreen *vscr;
	int i;

	acrn_timer_deinit(&vdpy.ui_timer);
	/* SDL display_thread will exit because of DM request */
	pthread_mutex_destroy(&vdpy.vdisplay_mutex);
	pthread_cond_destroy(&vdpy.vdisplay_signal);

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->img) {
			pixman_image_unref(vscr->img);
			vscr->img = NULL;
		}
		/* Continue to thread cleanup */
		if (vscr->surf_tex) {
			// SDL_DestroyTexture(vscr->surf_tex);
			glDeleteTextures(1, &vscr->surf_tex);
			vscr->surf_tex = 0;
		}
		if (vscr->cur_tex) {
			// SDL_DestroyTexture(vscr->cur_tex);
			glDeleteTextures(1, &vscr->cur_tex);
			vscr->cur_tex = 0;
		}

		if (vdpy.egl_dmabuf_supported && (vscr->egl_img != EGL_NO_IMAGE_KHR))
			vdpy.gl_ops.eglDestroyImageKHR(vdpy.eglDisplay,
						vscr->egl_img);
	}

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->bogus_tex) {
			// SDL_DestroyTexture(vscr->bogus_tex);
			glDeleteTextures(1, &vscr->bogus_tex);
			vscr->bogus_tex = 0;
		}
		// if (vscr->renderer) {
		// 	SDL_DestroyRenderer(vscr->renderer);
		// 	vscr->renderer = NULL;
		// }
		// if (vscr->win) {
		// 	SDL_DestroyWindow(vscr->win);
		// 	vscr->win = NULL;
		// }
	}

	/* This is used to workaround the TLS issue of libEGL + libGLdispatch
	 * after unloading library.
	 */
	eglReleaseThread();
	return NULL;
}

void *
vdpy_sdl_display_proc()
{
	struct vdpy_display_bh *bh;

	if (!vdpy.s.is_active) {
		pr_info("display is exiting\n");
		return NULL;
	}
	pthread_mutex_lock(&vdpy.vdisplay_mutex);

	if (TAILQ_EMPTY(&vdpy.request_list))
		pthread_cond_wait(&vdpy.vdisplay_signal,
					&vdpy.vdisplay_mutex);

	/* the bh_task is handled in vdisplay_mutex lock */
	while (!TAILQ_EMPTY(&vdpy.request_list)) {
		bh = TAILQ_FIRST(&vdpy.request_list);

		TAILQ_REMOVE(&vdpy.request_list, bh, link);

		bh->task_cb(bh->data);

		if (atomic_load(&bh->bh_flag) & ACRN_BH_FREE) {
			free(bh);
			bh = NULL;
		} else {
			/* free is owned by the submitter */
			atomic_store(&bh->bh_flag, ACRN_BH_DONE);
		}
	}

	pthread_mutex_unlock(&vdpy.vdisplay_mutex);

	return NULL;
}

bool vdpy_submit_bh(int handle, struct vdpy_display_bh *bh_task)
{
	bool bh_ok = false;

	if (handle != vdpy.s.n_connect) {
		return bh_ok;
	}

	if (!vdpy.s.is_active)
		return bh_ok;

	pthread_mutex_lock(&vdpy.vdisplay_mutex);

	if ((bh_task->bh_flag & ACRN_BH_PENDING) == 0) {
		bh_task->bh_flag |= ACRN_BH_PENDING;
		TAILQ_INSERT_TAIL(&vdpy.request_list, bh_task, link);
		bh_ok = true;
	}
	pthread_cond_signal(&vdpy.vdisplay_signal);
	pthread_mutex_unlock(&vdpy.vdisplay_mutex);

	return bh_ok;
}

int
vdpy_init(int *num_vscreens __attribute__((unused)))
{
	// int err, count;
#if 0
	if (vdpy.s.n_connect) {
		return 0;
	}

	/* start one vdpy_sdl_display_thread to handle the 3D request
	 * in this dedicated thread. Otherwise the libSDL + 3D doesn't
	 * work.
	 */
	err = pthread_create(&vdpy.tid, NULL, vdpy_sdl_display_thread, &vdpy);
	if (err) {
		pr_err("Failed to create the sdl_display_thread.\n");
		return 0;
	}
	pthread_setname_np(vdpy.tid, "acrn_vdisplay");

	count = 0;
	/* Wait up to 200ms so that the vdpy_sdl_display_thread is ready to
	 * handle the 3D request
	 */
	while (!vdpy.s.is_active && count < 20) {
		usleep(10000);
		count++;
	}
	if (!vdpy.s.is_active) {
		pr_err("display_thread is not ready.\n");
	}

	vdpy.s.n_connect++;
	if (num_vscreens)
		*num_vscreens = vdpy.vscrs_num;
	return vdpy.s.n_connect;
#endif
	*num_vscreens = 1;
	vdpy_sdl_display_init();
	return 0;
}

int
vdpy_deinit(int handle __attribute__((unused)))
{
	#if 0
	if (handle != vdpy.s.n_connect) {
		return -1;
	}

	vdpy.s.n_connect--;

	if (!vdpy.s.is_active) {
		return -1;
	}

	pthread_mutex_lock(&vdpy.vdisplay_mutex);
	vdpy.s.is_active = 0;
	/* Wakeup the vdpy_sdl_display_thread if it is waiting for signal */
	pthread_cond_signal(&vdpy.vdisplay_signal);
	pthread_mutex_unlock(&vdpy.vdisplay_mutex);

	pthread_join(vdpy.tid, NULL);
	pr_info("Exit SDL display thread\n");
	#endif
	vdpy_sdl_display_term();
	return 0;
}

// deprecated, replaced with vdpy_gfx_ui_init
int
gfx_ui_init()
{
#if 0
	struct vscreen *vscr;
	SDL_SysWMinfo info;
	int num_pscreen;
	int i;

	setenv("SDL_VIDEO_X11_FORCE_EGL", "1", 1);
	setenv("SDL_OPENGL_ES_DRIVER", "1", 1);
	setenv("SDL_RENDER_DRIVER", "opengles2", 1);
	setenv("SDL_RENDER_SCALE_QUALITY", "linear", 1);

	if (SDL_Init(SDL_INIT_VIDEO)) {
		pr_err("Failed to init SDL2 system\n");
		return -1;
	}

	if (vdpy.vscrs_num <= 0) {
		pr_err("Incorrect geometry parameter for virtio-gpu\n");
		return -1;
	}
	num_pscreen = SDL_GetNumVideoDisplays();

	for (i = 0; i < vdpy.vscrs_num; i++) {
		vscr = vdpy.vscrs + i;
		if (vscr->pscreen_id >= num_pscreen) {
			pr_err("Monitor id %d is out of avalble range [0~%d].\n",
					vscr->pscreen_id, num_pscreen);
			SDL_Quit();
			return -1;
		}

		SDL_GetDisplayBounds(vscr->pscreen_id, &vscr->pscreen_rect);

		if (vscr->pscreen_rect.w < VDPY_MIN_WIDTH ||
	    	    vscr->pscreen_rect.h < VDPY_MIN_HEIGHT) {
			pr_err("Too small resolutions. Please check the "
		       		" graphics system\n");
			SDL_Quit();
			return -1;
		}

		if (vscr->is_fullscreen) {
			vscr->guest_width = vscr->pscreen_rect.w;
			vscr->guest_height = vscr->pscreen_rect.h;
		}
	}

	SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
	memset(&info, 0, sizeof(info));
	SDL_VERSION(&info.version);

	/* Set the GL_parameter for Window/Renderer */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
			    SDL_GL_CONTEXT_PROFILE_ES);
	/* GLES2.0 is used */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	/* GL target surface selects A8/R8/G8/B8 */
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#endif

	vdpy.s.is_ui_realized = true;

	return 0;
}

// deprecated, replaced with vdpy_gfx_ui_deinit
void
gfx_ui_deinit()
{
	if (!vdpy.s.is_ui_realized) {
		return;
	}

	// free(vdpy.vscrs);
	// SDL_Quit();
	// pr_info("SDL_Quit\r\n");
}

// 1, hardcode vdisplay/vscreen number to 1
// 2, init egl context
// called in 'hook_before_init', before 'vos_backend_init'
int
vdpy_gfx_ui_init(void *data)
{
	struct vscreen *vscr;
	EGLint majorVersion;
    EGLint minorVersion;
	EGLint numConfigs = 0, n = -1;
	EGLConfig myConfig;
	int w, h;

	// only support 1 physical screen now
	vdpy.vscrs_num = 1;
	vdpy.vscrs = calloc(VSCREEN_MAX_NUM, sizeof(struct vscreen));
	if (!vdpy.vscrs) {
		pr_err("%s, memory allocation for vscrs failed.", __func__);
		return -1;
	}
	vdpy.s.n_connect = 1;
	
	vscr = &vdpy.vscrs[0];
	vscr->is_fullscreen = true;
	vscr->pscreen_id = 0;

	// SDL_GetDisplayBounds(vscr->pscreen_id, &vscr->pscreen_rect);
	vdpy.eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (eglInitialize(vdpy.eglDisplay, &majorVersion, &minorVersion) != EGL_TRUE) {
		return -1;
	}

    EGLint s_configAttribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE };
	eglChooseConfig(vdpy.eglDisplay, s_configAttribs, 0, 0, &numConfigs);
    if (numConfigs <= 0) {
		return -1;
    }

	EGLConfig* const configs = malloc(sizeof(EGLConfig) * numConfigs);
	eglChooseConfig(vdpy.eglDisplay, s_configAttribs, configs, numConfigs, &n);
	myConfig = configs[0];
	free(configs);

	// vdpy.eglContext = SDL_GL_GetCurrentContext();
	EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	vdpy.eglContext = eglCreateContext(vdpy.eglDisplay, myConfig, EGL_NO_CONTEXT, context_attribs);

    EGLNativeWindowType window = (EGLNativeWindowType)data;
    vdpy.eglSurface = eglCreateWindowSurface(vdpy.eglDisplay, myConfig, window, NULL);
    if (vdpy.eglSurface == EGL_NO_SURFACE) {
        printf("gelCreateWindowSurface failed.\n");
        return -1;
    }

	EGLBoolean returnValue;
    returnValue = eglMakeCurrent(vdpy.eglDisplay, vdpy.eglSurface, vdpy.eglSurface, vdpy.eglContext);
    // checkEglError("eglMakeCurrent", returnValue);
    if (returnValue != EGL_TRUE) {
        return -1;
    }
    eglQuerySurface(vdpy.eglDisplay, vdpy.eglSurface, EGL_WIDTH, &w);
    eglQuerySurface(vdpy.eglDisplay, vdpy.eglSurface, EGL_HEIGHT, &h);
	vscr->pscreen_rect.x = vscr->pscreen_rect.y = 0;
	vscr->guest_width = vscr->width = vscr->pscreen_rect.w = w;
	vscr->guest_height = vscr->height = vscr->pscreen_rect.h = h;

	if (vscr->pscreen_rect.w < VDPY_MIN_WIDTH ||
			vscr->pscreen_rect.h < VDPY_MIN_HEIGHT) {
		pr_err("Too small resolutions. Please check the "
				" graphics system\n");
		// SDL_Quit();
		return -1;
	}

	return 0;
}

void
vdpy_gfx_ui_deinit()
{
    if (vdpy.eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(vdpy.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (vdpy.eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(vdpy.eglDisplay, vdpy.eglContext);
        }
        if (vdpy.eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(vdpy.eglDisplay, vdpy.eglSurface);
        }
        eglTerminate(vdpy.eglDisplay);
    }
    vdpy.eglDisplay = EGL_NO_DISPLAY;
    vdpy.eglContext = EGL_NO_CONTEXT;
    vdpy.eglSurface = EGL_NO_SURFACE;

	if (vdpy.vscrs) {
		free(vdpy.vscrs);
		vdpy.vscrs = NULL;
	}
}

int vdpy_parse_cmd_option(const char *opts)
{
	char *str, *stropts, *tmp;
	int snum, error;
	struct vscreen *vscr;

	error = 0;
	vscr = calloc(VSCREEN_MAX_NUM, sizeof(struct vscreen));
	if (!vscr) {
		pr_err("%s, memory allocation for vscrs failed.", __func__);
		return -1;
	}
	vdpy.vscrs = vscr;
	vdpy.vscrs_num = 0;

	stropts = strdup(opts);
	while ((str = strsep(&stropts, ",")) != NULL) {
		vscr = vdpy.vscrs + vdpy.vscrs_num;
		if ((tmp = strcasestr(str, "geometry=fullscreen")) != NULL) {
			snum = sscanf(tmp, "geometry=fullscreen:%d", &vscr->pscreen_id);
			if (snum != 1) {
				vscr->pscreen_id = 0;
			}
			vscr->org_x = 0;
			vscr->org_y = 0;
			vscr->guest_width = VDPY_MAX_WIDTH;
			vscr->guest_height = VDPY_MAX_HEIGHT;
			vscr->is_fullscreen = true;
			pr_info("virtual display: fullscreen on monitor %d.\n",
					vscr->pscreen_id);
			vdpy.vscrs_num++;
		} else if ((tmp = strcasestr(str, "geometry=")) != NULL) {
			snum = sscanf(tmp, "geometry=%dx%d+%d+%d",
					&vscr->guest_width, &vscr->guest_height,
					&vscr->org_x, &vscr->org_y);
			if (snum != 4) {
				pr_err("incorrect geometry option. Should be"
						" WxH+x+y\n");
				error = -1;
			}
			vscr->is_fullscreen = false;
			vscr->pscreen_id = 0;
			pr_info("virtual display: windowed on monitor %d.\n",
					vscr->pscreen_id);
			vdpy.vscrs_num++;
		}

		if (vdpy.vscrs_num > VSCREEN_MAX_NUM) {
			pr_err("%d virtual displays are too many that acrn-dm can't support!\n");
			break;
		}
	}
	free(stropts);

	return error;
}