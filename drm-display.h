/*
 * Copyright (C) 2019-2020 Paul Kocialkowski <contact@paulk.fr>
 * Copyright (C) 2020 Bootlin
 */

#ifndef _DRM_DISPLAY_H_
#define _DRM_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <xf86drm.h>

struct drm_display;

struct drm_display_buffer {
	unsigned int width;
	unsigned int height;
	uint32_t format;

	uint32_t fb_id;

	uint32_t handles[4];
	uint32_t offsets[4];
	uint32_t strides[4];
	uint32_t sizes[4];

	void *data[4];
};

struct drm_display_property {
	const char *name;
	uint32_t *id;
	uint32_t *value;
};

struct drm_display_connector_properties {
	uint32_t crtc_id;
};

struct drm_display_crtc_properties {
	uint32_t active;
	uint32_t mode_id;
};

struct drm_display_plane_properties {
	uint32_t type;
	uint32_t fb_id;
	uint32_t crtc_id;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
};

struct drm_display_plane {
	uint32_t id;
	uint32_t type;

	struct drm_display_plane_properties properties;
};

struct drm_display_plane_setup {
	struct drm_display_plane plane;

	struct drm_display_buffer *buffer_visible;

	unsigned int buffer_width;
	unsigned int buffer_height;
	uint32_t buffer_format;

	unsigned int display_width;
	unsigned int display_height;
	unsigned int display_x;
	unsigned int display_y;

	bool configured;
};

struct drm_display_output {
	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	bool mode_set;

	uint32_t connector_id;
	struct drm_display_connector_properties connector_properties;

	uint32_t crtc_id;
	struct drm_display_crtc_properties crtc_properties;
};

struct drm_display {
	char *drm_path;
	int drm_fd;

	struct drm_display_output output;

	struct drm_display_plane_setup primary_setup;
	struct drm_display_buffer primary_buffers[2];
	unsigned int primary_buffers_count;
	unsigned int primary_buffers_index;

	struct drm_display_plane_setup overlay_setup;
	struct drm_display_buffer overlay_buffers[2];
	unsigned int overlay_buffers_count;
	unsigned int overlay_buffers_index;

	bool up;

	void *private;
};

struct drm_display_buffer *drm_display_primary_buffer_cycle(struct drm_display *display);
struct drm_display_buffer *drm_display_overlay_buffer_cycle(struct drm_display *display);
int drm_display_buffer_dma_buf_export(struct drm_display *display,
				      struct drm_display_buffer *buffer,
				      int *fd);
int drm_display_detach(struct drm_display *display,
		       struct drm_display_plane_setup *plane_setup);
int drm_display_page_flip(struct drm_display *display,
			  struct drm_display_plane_setup *plane_setup,
			  struct drm_display_buffer *buffer);
int drm_display_configure(struct drm_display *display,
			  struct drm_display_plane_setup *plane_setup,
			  struct drm_display_buffer *buffer);
int drm_display_setup(struct drm_display *display);
int drm_display_teardown(struct drm_display *display);
int drm_display_probe(struct drm_display *display);
int drm_display_open(struct drm_display *display);
void drm_display_close(struct drm_display *display);

#endif
