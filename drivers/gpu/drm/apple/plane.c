// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Display Coprocessor (DCP) plane support
 *
 * The DCPs display engine is controlled entirely by its RTKit-based
 * firmawre. The firmware implements IOMobileFramebuffer (IOMFB), an
 * interface that was once implemented as a Mach kernel extension.
 *
 * Rather than directly program the display engine, we submit an IOMFB
 * "swap request" containing metadata about each "surface" (plane in KMS
 * parlance). This metadata includes the pixel format, plane information
 * in the case of {semi,}planar framebuffers, and an IOVA to the plane's
 * framebuffer. The firmware then programs the hardware appropriately,
 * performs the pageflip, then notifies us if/when the pageflip completes.
 *
 * Despite the allowing up to four surfaces to be submitted, the display
 * engine itself will crash if any more than two are on screen at once. The
 * firmware will crash if any rectangle is smaller than 32x32 pixels, or
 * if any rectangle extends beyond screen space.
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

#include "plane.h"

#define FRAC_16_16(mult, div)    (((mult) << 16) / (div))

static int apple_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_rect *dst;
	int ret;

	new_plane_state = drm_atomic_get_new_plane_state(state, plane);

	if (!new_plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state, new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	/*
	 * DCP limits downscaling to 2x and upscaling to 4x. Attempting to
	 * scale outside these bounds errors out when swapping.
	 *
	 * This function also takes care of clipping the src/dest rectangles,
	 * which is required for correct operation. Partially off-screen
	 * surfaces may appear corrupted.
	 *
	 * DCP does not distinguish plane types in the hardware, so we set
	 * can_position. If the primary plane does not fill the screen, the
	 * hardware will fill in zeroes (black).
	 */
	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  FRAC_16_16(1, 2),
						  FRAC_16_16(4, 1),
						  true, true);
	if (ret < 0)
		return ret;

	if (!new_plane_state->visible)
		return 0;

	/*
	 * DCP does not allow a surface to clip off the screen, and will crash
	 * if any blended surface is smaller than 32x32. Reject the atomic op
	 * if the plane will crash DCP.
	 *
	 * This is most pertinent to cursors. Userspace should fall back to
	 * software cursors if the plane check is rejected.
	 */
	dst = &new_plane_state->dst;
	if (drm_rect_width(dst) < 32 || drm_rect_height(dst) < 32) {
		dev_err_once(state->dev->dev,
			"Plane operation would have crashed DCP! Rejected!\n\
			DCP requires 32x32 of every plane to be within screen space.\n\
			Your compositor asked to overlay [%dx%d, %dx%d] on %dx%d.\n\
			This is not supported, and your compositor should have\n\
			switched to software compositing when this operation failed.\n\
			You should not have noticed this at all. If your screen\n\
			froze/hitched, or your compositor crashed, please report\n\
			this to the your compositor's developers. We will not\n\
			throw this error again until you next reboot.\n",
			dst->x1, dst->y1, dst->x2, dst->y2,
			crtc_state->mode.hdisplay, crtc_state->mode.vdisplay);
		return -EINVAL;
	}

	return 0;
}

/*
 * DRM specifies rectangles as start and end coordinates.  DCP specifies
 * rectangles as a start coordinate and a width/height. Convert a DRM rectangle
 * to a DCP rectangle.
 */
static struct dcp_rect drm_to_dcp_rect(struct drm_rect *rect)
{
	return (struct dcp_rect){ .x = rect->x1,
				  .y = rect->y1,
				  .w = drm_rect_width(rect),
				  .h = drm_rect_height(rect) };
}

static u32 apple_plane_drm_format_to_dcp(u32 drm)
{
	switch (drm) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return fourcc_code('A', 'R', 'G', 'B');

	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return fourcc_code('A', 'B', 'G', 'R');

	case DRM_FORMAT_XRGB2101010:
		return fourcc_code('r', '0', '3', 'w');

	case DRM_FORMAT_NV12:
		return fourcc_code('v', '0', '2', '4');
	}

	pr_warn("DRM format %X not supported in DCP\n", drm);
	return 0;
}

static u32 apple_plane_drm_colour_to_dcp(u32 enc)
{
	switch (enc) {
	case DRM_COLOR_YCBCR_BT601:
	case DRM_COLOR_YCBCR_BT709:
		return DCP_COLORSPACE_BT709;
	case DRM_COLOR_YCBCR_BT2020:
		return DCP_COLORSPACE_BG_BT2020;
	default:
		return DCP_COLORSPACE_NATIVE;
	}
}

static u32 apple_plane_determine_xfer_func(const struct drm_format_info *fmt, u32 colour_enc)
{
	switch (fmt->format) {
	case DRM_FORMAT_NV12:
		switch (colour_enc) {
		case DRM_COLOR_YCBCR_BT709:
		case DRM_COLOR_YCBCR_BT2020:
			return DCP_XFER_FUNC_BT1886;
		default:
			return DCP_XFER_FUNC_SDR;
		}
	default:
		return DCP_XFER_FUNC_SDR;
	}
}

static void apple_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *ns = drm_atomic_get_new_plane_state(state, plane);
	struct apple_plane_state *ps;
	struct drm_gem_dma_object *obj;
	struct drm_rect src_rect;

	if (!ns)
		return;

	ps = to_apple_plane_state(ns);

	if (!ns->fb || !ns->visible) {
		memset(&ps->surface, 0, sizeof(ps->surface));
		return;
	}

	drm_rect_fp_to_int(&src_rect, &ns->src);

	ps->src_rect = drm_to_dcp_rect(&src_rect);
	ps->dst_rect = drm_to_dcp_rect(&ns->dst);

	ps->surface = (struct dcp_surface) {
		.is_tiled = false, /* Has nothing to do with tiled FBs. No clue... */
		.is_tearing_allowed = true,
		.is_premultiplied = !ns->fb->format->has_alpha,
		.plane_cnt = ns->fb->format->num_planes,
		.plane_cnt2 = ns->fb->format->num_planes,
		.format = apple_plane_drm_format_to_dcp(ns->fb->format->format),
		.xfer_func = apple_plane_determine_xfer_func(ns->fb->format, ns->color_encoding),
		.colorspace = apple_plane_drm_colour_to_dcp(ns->color_encoding),
		.stride = ns->fb->pitches[0],
		.width = ns->fb->width,
		.height = ns->fb->height,
		.buf_size = ns->fb->format->num_planes == 1 ? ns->fb->height * ns->fb->pitches[0] : 0,
		.surface_id = plane ? plane->base.id : 0,

		/* Only used for tiled/compressed surfaces */
		.pix_size = 1,
		.pel_w = 1,
		.pel_h = 1,
		.has_comp = ns->fb->modifier == DRM_FORMAT_MOD_APPLE_GPU_TILED_COMPRESSED,
	};

	if (ns->fb->format->num_planes > 1) {
		int i;

		ps->surface.has_planes = true;
		for (i = 0; i < ns->fb->format->num_planes; i++) {
			ps->surface.planes[i] = (struct dcp_plane_info) {
				.width = drm_format_info_plane_width(ns->fb->format, ps->surface.width, i),
				.height = drm_format_info_plane_height(ns->fb->format, ps->surface.height, i),
				.base = i ? drm_format_info_plane_height(ns->fb->format, ps->surface.height, i - 1) * ns->fb->pitches[i - 1] : 0,
				.offset = i ? drm_format_info_plane_height(ns->fb->format, ps->surface.height, i - 1) * ns->fb->pitches[i - 1] : 0,
				.stride = ns->fb->pitches[i],
				.size = drm_format_info_plane_height(ns->fb->format, ps->surface.height, i) * ns->fb->pitches[i],
				.tile_w = drm_format_info_block_width(ns->fb->format, i),
				.tile_h = drm_format_info_block_height(ns->fb->format, i),
				.tile_size = drm_format_info_block_width(ns->fb->format, i) * drm_format_info_block_height(ns->fb->format, i),
			};

			ps->surface.buf_size += ps->surface.planes[i].size;
		}
	}

	/* the obvious helper call drm_fb_dma_get_gem_addr() adjusts
	 * the address for source x/y offsets. Since IOMFB has a direct
	 * support source position prefer that.
	 */
	obj = drm_fb_dma_get_gem_obj(ns->fb, 0);
	if (obj)
		ps->iova = obj->dma_addr + ns->fb->offsets[0];
}

static const struct drm_plane_helper_funcs apple_primary_plane_helper_funcs = {
	.atomic_check	= apple_plane_atomic_check,
	.atomic_update	= apple_plane_atomic_update,
	.get_scanout_buffer = drm_fb_dma_get_scanout_buffer,
};

static const struct drm_plane_helper_funcs apple_plane_helper_funcs = {
	.atomic_check	= apple_plane_atomic_check,
	.atomic_update	= apple_plane_atomic_update,
};

static void apple_plane_cleanup(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
	kfree(plane);
}

static struct drm_plane_state *apple_plane_duplicate_state(struct drm_plane *plane)
{
	struct apple_plane_state *new_state, *old_state;

	old_state = to_apple_plane_state(plane->state);

	new_state = kzalloc(sizeof(*new_state), GFP_KERNEL);
	if (!new_state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &new_state->base);

	new_state->surface = old_state->surface;

	return &new_state->base;
}

static const struct drm_plane_funcs apple_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= apple_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = apple_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/*
 * Table of supported formats, mapping from DRM fourccs to DCP fourccs.
 *
 * For future work, DCP supports more formats not listed, including YUV
 * formats, an extra RGBA format, and a biplanar RGB10_A8 format (fourcc b3a8)
 * used for HDR.
 *
 * Note: we don't have non-alpha formats but userspace breaks without XRGB. It
 * doesn't matter for the primary plane, but cursors/overlays must not
 * advertise formats without alpha.
 */
static const u32 dcp_primary_formats[] = {
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_NV12,
};

static const u32 dcp_overlay_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_NV12,
};

u64 apple_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

struct drm_plane *apple_plane_init(struct drm_device *dev,
				   unsigned long possible_crtcs,
				   enum drm_plane_type type)
{
	int ret;
	struct drm_plane *plane;

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);

	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		ret = drm_universal_plane_init(dev, plane, possible_crtcs,
				       &apple_plane_funcs,
				       dcp_primary_formats, ARRAY_SIZE(dcp_primary_formats),
				       apple_format_modifiers, type, NULL);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
	case DRM_PLANE_TYPE_CURSOR:
		ret = drm_universal_plane_init(dev, plane, possible_crtcs,
				       &apple_plane_funcs,
				       dcp_overlay_formats, ARRAY_SIZE(dcp_overlay_formats),
				       apple_format_modifiers, type, NULL);
		break;
	default:
		return NULL;
	}

	if (ret)
		return ERR_PTR(ret);

	if (type == DRM_PLANE_TYPE_PRIMARY)
		drm_plane_helper_add(plane, &apple_primary_plane_helper_funcs);
	else
		drm_plane_helper_add(plane, &apple_plane_helper_funcs);

	return plane;
}
