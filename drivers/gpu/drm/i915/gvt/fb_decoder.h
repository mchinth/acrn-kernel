/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _GVT_FB_DECODER_H_
#define _GVT_FB_DECODER_H_

#define _PLANE_CTL_FORMAT_SHIFT		24
#define _PLANE_CTL_TILED_SHIFT		10
#define _PIPE_V_SRCSZ_SHIFT		0
#define _PIPE_V_SRCSZ_MASK		(0xfff << _PIPE_V_SRCSZ_SHIFT)
#define _PIPE_H_SRCSZ_SHIFT		16
#define _PIPE_H_SRCSZ_MASK		(0x1fff << _PIPE_H_SRCSZ_SHIFT)

#define _PRI_PLANE_FMT_SHIFT		26
#define _PRI_PLANE_STRIDE_MASK		(0x3ff << 6)
#define _PRI_PLANE_X_OFF_SHIFT		0
#define _PRI_PLANE_X_OFF_MASK		(0x1fff << _PRI_PLANE_X_OFF_SHIFT)
#define _PRI_PLANE_Y_OFF_SHIFT		16
#define _PRI_PLANE_Y_OFF_MASK		(0xfff << _PRI_PLANE_Y_OFF_SHIFT)

#define _PLANE_SIZE_HEIGHT_SHIFT	16
#define _PLANE_SIZE_HEIGHT_MASK		(0xfff << _PLANE_SIZE_HEIGHT_SHIFT)
#define _PLANE_SIZE_WIDTH_MASK		0x1fff

#define _CURSOR_MODE			0x3f
#define _CURSOR_ALPHA_FORCE_SHIFT	8
#define _CURSOR_ALPHA_FORCE_MASK	(0x3 << _CURSOR_ALPHA_FORCE_SHIFT)
#define _CURSOR_ALPHA_PLANE_SHIFT	10
#define _CURSOR_ALPHA_PLANE_MASK	(0x3 << _CURSOR_ALPHA_PLANE_SHIFT)
#define _CURSOR_POS_X_SHIFT		0
#define _CURSOR_POS_X_MASK		(0x1fff << _CURSOR_POS_X_SHIFT)
#define _CURSOR_SIGN_X_SHIFT		15
#define _CURSOR_SIGN_X_MASK		(1 << _CURSOR_SIGN_X_SHIFT)
#define _CURSOR_POS_Y_SHIFT		16
#define _CURSOR_POS_Y_MASK		(0xfff << _CURSOR_POS_Y_SHIFT)
#define _CURSOR_SIGN_Y_SHIFT		31
#define _CURSOR_SIGN_Y_MASK		(1 << _CURSOR_SIGN_Y_SHIFT)

#define _SPRITE_FMT_SHIFT		25
#define _SPRITE_COLOR_ORDER_SHIFT	20
#define _SPRITE_YUV_ORDER_SHIFT		16
#define _SPRITE_STRIDE_SHIFT		6
#define _SPRITE_STRIDE_MASK		(0x1ff << _SPRITE_STRIDE_SHIFT)
#define _SPRITE_SIZE_WIDTH_SHIFT	0
#define _SPRITE_SIZE_HEIGHT_SHIFT	16
#define _SPRITE_SIZE_WIDTH_MASK		(0x1fff << _SPRITE_SIZE_WIDTH_SHIFT)
#define _SPRITE_SIZE_HEIGHT_MASK	(0xfff << _SPRITE_SIZE_HEIGHT_SHIFT)
#define _SPRITE_POS_X_SHIFT		0
#define _SPRITE_POS_Y_SHIFT		16
#define _SPRITE_POS_X_MASK		(0x1fff << _SPRITE_POS_X_SHIFT)
#define _SPRITE_POS_Y_MASK		(0xfff << _SPRITE_POS_Y_SHIFT)
#define _SPRITE_OFFSET_START_X_SHIFT	0
#define _SPRITE_OFFSET_START_Y_SHIFT	16
#define _SPRITE_OFFSET_START_X_MASK	(0x1fff << _SPRITE_OFFSET_START_X_SHIFT)
#define _SPRITE_OFFSET_START_Y_MASK	(0xfff << _SPRITE_OFFSET_START_Y_SHIFT)

typedef enum {
	FB_MODE_SET_START = 1,
	FB_MODE_SET_END,
	FB_DISPLAY_FLIP,
}gvt_fb_event_t;

typedef enum {
	DDI_PORT_NONE	= 0,
	DDI_PORT_B	= 1,
	DDI_PORT_C	= 2,
	DDI_PORT_D	= 3,
	DDI_PORT_E	= 4
} ddi_port_t;

struct intel_gvt;

struct gvt_fb_notify_msg {
	unsigned vm_id;
	unsigned pipe_id; /* id starting from 0 */
	unsigned plane_id; /* primary, cursor, or sprite */
};

/* color space conversion and gamma correction are not included */
struct gvt_primary_plane_format {
	u8	enabled;	/* plane is enabled */
	u8	tiled;		/* X-tiled */
	u8	bpp;		/* bits per pixel */
	u32	hw_format;	/* format field in the PRI_CTL register */
	u32	drm_format;	/* format in DRM definition */
	u32	base;		/* framebuffer base in graphics memory */
	u32	x_offset;	/* in pixels */
	u32	y_offset;	/* in lines */
	u32	width;		/* in pixels */
	u32	height;		/* in lines */
	u32	stride;		/* in bytes */
};

struct gvt_sprite_plane_format {
	u8	enabled;	/* plane is enabled */
	u8	tiled;		/* X-tiled */
	u8	bpp;		/* bits per pixel */
	u32	hw_format;	/* format field in the SPR_CTL register */
	u32	drm_format;	/* format in DRM definition */
	u32	base;		/* sprite base in graphics memory */
	u32	x_pos;		/* in pixels */
	u32	y_pos;		/* in lines */
	u32	x_offset;	/* in pixels */
	u32	y_offset;	/* in lines */
	u32	width;		/* in pixels */
	u32	height;		/* in lines */
};

struct gvt_cursor_plane_format {
	u8	enabled;
	u8	mode;		/* cursor mode select */
	u8	bpp;		/* bits per pixel */
	u32	drm_format;	/* format in DRM definition */
	u32	base;		/* cursor base in graphics memory */
	u32	x_pos;		/* in pixels */
	u32	y_pos;		/* in lines */
	u8	x_sign;		/* X Position Sign */
	u8	y_sign;		/* Y Position Sign */
	u32	width;		/* in pixels */
	u32	height;		/* in lines */
	u32	x_hot;		/* in pixels */
	u32	y_hot;		/* in pixels */
};

struct gvt_pipe_format {
	struct gvt_primary_plane_format	primary;
	struct gvt_sprite_plane_format	sprite;
	struct gvt_cursor_plane_format	cursor;
	ddi_port_t ddi_port;  /* the DDI port that the pipe is connected to */
};

struct gvt_fb_format{
	struct gvt_pipe_format	pipes[4];
};

extern int gvt_decode_fb_format(struct intel_gvt *pdev, int vmid,
				struct gvt_fb_format *fb);

#endif
