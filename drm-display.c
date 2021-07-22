/*
 * Copyright (C) 2019-2021 Paul Kocialkowski <contact@paulk.fr>
 * Copyright (C) 2020 Bootlin
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <libudev.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <xf86drm.h>

#include <drm-display.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct drm_display_buffer *drm_display_primary_buffer_cycle(struct drm_display *display)
{
	struct drm_display_buffer *buffer;
	unsigned int count;
	unsigned int index;

	if (!display)
		return NULL;

	count = display->primary_buffers_count;
	index = display->primary_buffers_index;
	buffer = &display->primary_buffers[index];

	index++;
	index %= count;

	display->primary_buffers_index = index;

	return buffer;
}

struct drm_display_buffer *drm_display_overlay_buffer_cycle(struct drm_display *display)
{
	struct drm_display_buffer *buffer;
	unsigned int count;
	unsigned int index;

	if (!display)
		return NULL;

	count = display->overlay_buffers_count;
	index = display->overlay_buffers_index;
	buffer = &display->overlay_buffers[index];

	index++;
	index %= count;

	display->overlay_buffers_index = index;

	return buffer;
}

int drm_display_buffer_dma_buf_export(struct drm_display *display,
				      struct drm_display_buffer *buffer,
				      int *fd)
{
	int ret;

	if (!display || !buffer || !fd)
		return -EINVAL;

	ret = drmPrimeHandleToFD(display->drm_fd, buffer->handles[0], 0, fd);
	if (ret)
		return -errno;

	return 0;
}

int drm_display_buffer_setup(struct drm_display *display,
			     struct drm_display_buffer *buffer,
			     struct drm_display_plane_setup *plane_setup)
{
	struct drm_mode_create_dumb create_dumb = { 0 };
	struct drm_mode_map_dumb map_dumb = { 0 };
	int ret;

	if (!display || !buffer || !plane_setup)
		return -EINVAL;

	buffer->width = plane_setup->buffer_width;
	buffer->height = plane_setup->buffer_height;
	buffer->format = plane_setup->buffer_format;

	switch (buffer->format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		create_dumb.width = buffer->width;
		create_dumb.height = buffer->height;
		create_dumb.bpp = 32;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_YUV420:
		create_dumb.width = buffer->width;
		/* cdw * cdh * 32 / 8 = bw * bh * 3 / 2 */
		create_dumb.height = (buffer->height * 3 + 7) / 8;
		create_dumb.bpp = 32;
		break;
	default:
		return -EINVAL;
	}

	ret = drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (ret)
		return -errno;

	buffer->handles[0] = create_dumb.handle;
	buffer->strides[0] = create_dumb.pitch;
	buffer->sizes[0] = create_dumb.size;

	map_dumb.handle = buffer->handles[0];

	ret = drmIoctl(display->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret)
		goto error;

	buffer->data[0] = mmap(0, buffer->sizes[0], PROT_READ | PROT_WRITE,
			       MAP_SHARED, display->drm_fd, map_dumb.offset);
	if (buffer->data[0] == MAP_FAILED)
		goto error;

	switch (buffer->format) {
	case DRM_FORMAT_NV12:
		buffer->strides[0] /= 4;
		buffer->handles[1] = buffer->handles[0];
		buffer->offsets[1] = buffer->width * buffer->height;
		buffer->data[1] = buffer->data[0] + buffer->offsets[1];
		buffer->strides[1] = buffer->strides[0];
		break;
	case DRM_FORMAT_YUV420:
		buffer->strides[0] /= 4;
		buffer->handles[1] = buffer->handles[0];
		buffer->handles[2] = buffer->handles[0];
		buffer->offsets[1] = buffer->width * buffer->height;
		buffer->offsets[2] = buffer->offsets[1] + buffer->offsets[1] / 4;
		buffer->data[1] = buffer->data[0] + buffer->offsets[1];
		buffer->data[2] = buffer->data[1] + buffer->offsets[2];
		buffer->strides[1] = buffer->strides[0];
		buffer->strides[2] = buffer->strides[1];
		break;
	}

	ret = drmModeAddFB2(display->drm_fd, buffer->width, buffer->height,
			    buffer->format, buffer->handles, buffer->strides,
			    buffer->offsets, &buffer->fb_id, 0);
	if (ret)
		goto error;

	return 0;

error:
	if (create_dumb.handle) {
		struct drm_mode_destroy_dumb destroy_dumb = { 0 };

		destroy_dumb.handle = buffer->handles[0];

		drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB,
			 &destroy_dumb);
	}

	if (buffer->data[0] && buffer->data[0] != MAP_FAILED)
		munmap(buffer->data[0], buffer->sizes[0]);

	memset(buffer, 0, sizeof(*buffer));

	return -1;
}

int drm_display_buffer_teardown(struct drm_display *display,
				struct drm_display_buffer *buffer)
{
	struct drm_mode_destroy_dumb destroy_dumb = { 0 };

	if (!display || !buffer)
		return -EINVAL;

	drmModeRmFB(display->drm_fd, buffer->fb_id);

	if (buffer->data[0])
		munmap(buffer->data[0], buffer->sizes[0]);

	destroy_dumb.handle = buffer->handles[0];
	drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	memset(buffer, 0, sizeof(*buffer));

	return 0;
}

int drm_display_detach(struct drm_display *display,
		       struct drm_display_plane_setup *plane_setup)
{
	drmModeAtomicReqPtr request;
	struct drm_display_plane_properties *plane_properties;
	uint32_t flags = 0;
	uint32_t plane_id;
	int ret;

	if (!display || !plane_setup)
		return -EINVAL;

	if (!plane_setup->configured)
		return -1;

	plane_properties = &plane_setup->plane.properties;
	plane_id = plane_setup->plane.id;

	request = drmModeAtomicAlloc();
	if (!request)
		return -ENOMEM;

	drmModeAtomicAddProperty(request, plane_id, plane_properties->fb_id, 0);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_id,
				 0);

	ret = drmModeAtomicCommit(display->drm_fd, request, flags, NULL);
	if (ret) {
		ret = -errno;
		goto complete;
	}

	plane_setup->buffer_visible = NULL;
	plane_setup->configured = false;

complete:
	drmModeAtomicFree(request);

	return ret;
}

int drm_display_page_flip(struct drm_display *display,
			  struct drm_display_plane_setup *plane_setup,
			  struct drm_display_buffer *buffer)
{
	drmModeAtomicReqPtr request;
	struct drm_display_plane_properties *plane_properties;
	uint32_t flags = 0;
	uint32_t plane_id;
	int ret;

	if (!display || !buffer || !plane_setup)
		return -EINVAL;

	if (!plane_setup->configured)
		return -1;

	plane_properties = &plane_setup->plane.properties;
	plane_id = plane_setup->plane.id;

	request = drmModeAtomicAlloc();
	if (!request)
		return -ENOMEM;

	drmModeAtomicAddProperty(request, plane_id, plane_properties->fb_id,
				 buffer->fb_id);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_id,
				 display->output.crtc_id);

	ret = drmModeAtomicCommit(display->drm_fd, request, flags, NULL);
	if (ret) {
		ret = -errno;
		goto complete;
	}

	plane_setup->buffer_visible = buffer;

complete:
	drmModeAtomicFree(request);

	return ret;
}

int drm_display_configure(struct drm_display *display,
			  struct drm_display_plane_setup *plane_setup,
			  struct drm_display_buffer *buffer)
{
	drmModeAtomicReqPtr request;
	struct drm_display_plane_properties *plane_properties;
	struct drm_display_crtc_properties *crtc_properties;
	struct drm_display_connector_properties *connector_properties;
	uint32_t flags = 0;
	uint32_t plane_id;
	uint32_t crtc_id;
	uint32_t connector_id;
	int ret;

	if (!display || !buffer || !plane_setup)
		return -EINVAL;

	plane_properties = &plane_setup->plane.properties;
	plane_id = plane_setup->plane.id;

	crtc_properties = &display->output.crtc_properties;
	crtc_id = display->output.crtc_id;

	connector_properties = &display->output.connector_properties;
	connector_id = display->output.connector_id;

	request = drmModeAtomicAlloc();
	if (!request)
		return -ENOMEM;

	if (!display->output.mode_set) {
		drmModeCreatePropertyBlob(display->drm_fd,
					  &display->output.mode,
					  sizeof(display->output.mode),
					  &display->output.mode_blob_id);

		drmModeAtomicAddProperty(request, connector_id,
					 connector_properties->crtc_id,
					 crtc_id);

		drmModeAtomicAddProperty(request, crtc_id,
					 crtc_properties->active, 1);
		drmModeAtomicAddProperty(request, crtc_id,
					 crtc_properties->mode_id,
					 display->output.mode_blob_id);

		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	}

	drmModeAtomicAddProperty(request, plane_id, plane_properties->fb_id,
				 buffer->fb_id);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_id,
				 display->output.crtc_id);

	drmModeAtomicAddProperty(request, plane_id, plane_properties->src_w,
				 plane_setup->buffer_width << 16);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->src_h,
				 plane_setup->buffer_height << 16);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->src_x, 0);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->src_y, 0);

	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_w,
				 plane_setup->display_width);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_h,
				 plane_setup->display_height);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_x,
				 plane_setup->display_x);
	drmModeAtomicAddProperty(request, plane_id, plane_properties->crtc_y,
				 plane_setup->display_y);

	ret = drmModeAtomicCommit(display->drm_fd, request, flags, NULL);
	if (ret) {
		ret = -errno;
		goto complete;
	}

	plane_setup->buffer_visible = buffer;
	plane_setup->configured = true;

	if (!display->output.mode_set)
		display->output.mode_set = true;

complete:
	drmModeAtomicFree(request);

	return ret;
}

int drm_display_setup(struct drm_display *display)
{
	unsigned int i;
	int ret;

	if (!display || display->up)
		return -EINVAL;

	display->primary_buffers_count = ARRAY_SIZE(display->primary_buffers);
	display->primary_buffers_index = 0;

	for (i = 0; i < display->primary_buffers_count; i++) {
		struct drm_display_buffer *buffer =
			&display->primary_buffers[i];

		ret = drm_display_buffer_setup(display, buffer,
					       &display->primary_setup);
		if (ret)
			goto error;
	}

	if (!display->primary_setup.display_width ||
	    !display->primary_setup.display_height) {
		display->primary_setup.display_width =
			display->primary_setup.buffer_width;
		display->primary_setup.display_height =
			display->primary_setup.buffer_height;
	}

	if (!display->overlay_setup.buffer_format)
		goto complete;

	display->overlay_buffers_count = ARRAY_SIZE(display->overlay_buffers);
	display->overlay_buffers_index = 0;

	for (i = 0; i < display->overlay_buffers_count; i++) {
		struct drm_display_buffer *buffer =
			&display->overlay_buffers[i];

		ret = drm_display_buffer_setup(display, buffer,
					       &display->overlay_setup);
		if (ret)
			goto error;
	}

	if (!display->overlay_setup.display_width ||
	    !display->overlay_setup.display_height) {
		display->overlay_setup.display_width =
			display->overlay_setup.buffer_width;
		display->overlay_setup.display_height =
			display->overlay_setup.buffer_height;
	}

complete:
	display->up = true;

	return 0;

error:
	/* TODO: teardown created buffers. */

	return -1;
}

int drm_display_teardown(struct drm_display *display)
{
	unsigned int i;

	if (!display || !display->up)
		return -EINVAL;

	if (display->primary_setup.configured)
		drm_display_detach(display, &display->primary_setup);

	for (i = 0; i < display->primary_buffers_count; i++) {
		struct drm_display_buffer *buffer =
			&display->primary_buffers[i];

		drm_display_buffer_teardown(display, buffer);
	}

	if (display->overlay_setup.configured)
		drm_display_detach(display, &display->overlay_setup);

	for (i = 0; i < display->overlay_buffers_count; i++) {
		struct drm_display_buffer *buffer =
			&display->overlay_buffers[i];

		drm_display_buffer_teardown(display, buffer);
	}

	if (display->output.mode_blob_id) {
		drmModeDestroyPropertyBlob(display->drm_fd,
					   display->output.mode_blob_id);
		display->output.mode_blob_id = 0;
	}

	display->up = false;

	return 0;
}

static int display_properties_probe(struct drm_display *display,
				    uint32_t id, uint32_t type,
				    struct drm_display_property *display_properties,
				    unsigned int display_properties_count)
{
	drmModeObjectPropertiesPtr properties = NULL;
	unsigned int i;
	int ret;

	properties = drmModeObjectGetProperties(display->drm_fd, id, type);
	if (!properties)
		return -errno;

	for (i = 0; i < properties->count_props; i++) {
		drmModePropertyPtr property;
		unsigned int j;

		property = drmModeGetProperty(display->drm_fd,
					      properties->props[i]);
		if (property == NULL)
			continue;

		for (j = 0; j < display_properties_count; j++) {
			if (strcmp(property->name, display_properties[j].name))
				continue;

			*display_properties[j].id = property->prop_id;

			if (display_properties[j].value)
				*display_properties[j].value =
					properties->prop_values[i];

			break;
		}

		drmModeFreeProperty(property);
	}

	for (i = 0; i < display_properties_count; i++)
		if (!*display_properties[i].id)
			goto error;

	ret = 0;
	goto complete;

error:
	ret = -1;

complete:
	if (properties)
		drmModeFreeObjectProperties(properties);

	return ret;
}

static int connector_properties_probe(struct drm_display *display)
{

	struct drm_display_connector_properties *connector_properties =
		&display->output.connector_properties;
	struct drm_display_property display_properties[] = {
		{ "CRTC_ID",	&connector_properties->crtc_id },
	};

	return display_properties_probe(display, display->output.connector_id,
					DRM_MODE_OBJECT_CONNECTOR,
					(struct drm_display_property *)&display_properties,
					ARRAY_SIZE(display_properties));
}

static int crtc_properties_probe(struct drm_display *display)
{

	struct drm_display_crtc_properties *crtc_properties =
		&display->output.crtc_properties;
	struct drm_display_property display_properties[] = {
		{ "ACTIVE",	&crtc_properties->active },
		{ "MODE_ID",	&crtc_properties->mode_id },
	};

	return display_properties_probe(display, display->output.crtc_id,
					DRM_MODE_OBJECT_CRTC,
					(struct drm_display_property *)&display_properties,
					ARRAY_SIZE(display_properties));
}

static int plane_properties_probe(struct drm_display *display,
				  struct drm_display_plane *plane)
{
	struct drm_display_plane_properties *plane_properties =
		&plane->properties;
	struct drm_display_property display_properties[] = {
		{ "type",	&plane_properties->type,	&plane->type },
		{ "FB_ID",	&plane_properties->fb_id },
		{ "CRTC_ID",	&plane_properties->crtc_id },
		{ "SRC_X",	&plane_properties->src_x },
		{ "SRC_Y",	&plane_properties->src_y },
		{ "SRC_W",	&plane_properties->src_w },
		{ "SRC_H",	&plane_properties->src_h },
		{ "CRTC_X",	&plane_properties->crtc_x },
		{ "CRTC_Y",	&plane_properties->crtc_y },
		{ "CRTC_W",	&plane_properties->crtc_w },
		{ "CRTC_H",	&plane_properties->crtc_h },
	};

	return display_properties_probe(display, plane->id,
					DRM_MODE_OBJECT_PLANE,
					(struct drm_display_property *)&display_properties,
					ARRAY_SIZE(display_properties));
}

int drm_display_probe(struct drm_display *display)
{
	drmModeResPtr resources = NULL;
	drmModePlaneResPtr plane_resources = NULL;
	drmModeEncoderPtr encoder = NULL;
	drmModeCrtcPtr crtc = NULL;
	drmModeModeInfo mode_best = { 0 };
	uint32_t encoder_id = 0;
	unsigned int crtc_index = 0;
	unsigned int i, j;
	int ret;

	if (!display)
		return -EINVAL;

	/* Set client capabilities. */

	ret = drmSetClientCap(display->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret)
		return -errno;

	ret = drmSetClientCap(display->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES,
			      1);
	if (ret)
		return -errno;

	/* Get DRM resources. */

	resources = drmModeGetResources(display->drm_fd);
	if (!resources)
		return -ENODEV;

	/* Find a connected connector. */

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnectorPtr connector;

		/* Fully probe the connector in case it was not yet configured. */
		connector = drmModeGetConnector(display->drm_fd,
						resources->connectors[i]);
		if (!connector)
			continue;

		if (connector->connection != DRM_MODE_CONNECTED)
			goto next_connector;

		if (connector->encoder_id)
			encoder_id = connector->encoder_id;
		else if (connector->count_encoders)
			encoder_id = connector->encoders[0];
		else
			goto next_connector;

		display->output.connector_id = connector->connector_id;

		for (j = 0; j < connector->count_modes; j++) {
			if (connector->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
				memcpy(&mode_best, &connector->modes[j],
				       sizeof(mode_best));
				break;
			}
		}

next_connector:
		drmModeFreeConnector(connector);

		if (encoder_id)
			break;
	}

	if (i == resources->count_connectors)
		goto error;

	ret = connector_properties_probe(display);
	if (ret)
		goto error;

	/* Find the attached CRTC ID. */

	encoder = drmModeGetEncoder(display->drm_fd, encoder_id);
	if (!encoder)
		goto error;

	if (encoder->crtc_id) {
		display->output.crtc_id = encoder->crtc_id;
	} else for (i = 0; i < resources->count_crtcs; i++) {
		uint32_t crtc_mask = 1 << i;

		if (encoder->possible_crtcs & crtc_mask) {
			display->output.crtc_id = resources->crtcs[i];
			break;
		}
	}

	if (!display->output.crtc_id)
		goto error;

	drmModeFreeEncoder(encoder);
	encoder = NULL;

	/* Find the CRTC index. */

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == display->output.crtc_id) {
			crtc_index = i;
			break;
		}
	}

	if (i == resources->count_crtcs)
		goto error;

	crtc = drmModeGetCrtc(display->drm_fd, display->output.crtc_id);
	if (!crtc)
		goto error;

	if (crtc->mode_valid) {
		memcpy(&display->output.mode, &crtc->mode,
		       sizeof(display->output.mode));
		display->output.mode_set = true;
	} else {
		memcpy(&display->output.mode, &mode_best,
		       sizeof(display->output.mode));
		display->output.mode_set = false;
	}

	ret = crtc_properties_probe(display);
	if (ret)
		goto error;

	/* Get plane resources. */

	plane_resources = drmModeGetPlaneResources(display->drm_fd);
	if (!plane_resources)
		goto error;

	for (i = 0; i < plane_resources->count_planes; i++) {
		struct drm_display_plane display_plane = { 0 };
		drmModePlanePtr plane;
		unsigned int j;
		uint32_t format;

		plane = drmModeGetPlane(display->drm_fd,
					plane_resources->planes[i]);
		if (!plane)
			continue;

		if (!(plane->possible_crtcs & (1 << crtc_index)))
			goto next_plane;

		display_plane.id = plane_resources->planes[i];

		ret = plane_properties_probe(display, &display_plane);
		if (ret)
			goto next_plane;

		switch (display_plane.type) {
		case DRM_PLANE_TYPE_PRIMARY:
			if (display->primary_setup.plane.id)
				goto next_plane;

			format = display->primary_setup.buffer_format;
			break;
		case DRM_PLANE_TYPE_OVERLAY:
			if (display->overlay_setup.plane.id)
				goto next_plane;

			format = display->overlay_setup.buffer_format;
			break;
		default:
			goto next_plane;
		}

		for (j = 0; j < plane->count_formats; j++)
			if (plane->formats[j] == format)
				break;

		if (j == plane->count_formats)
			goto next_plane;

		switch (display_plane.type) {
		case DRM_PLANE_TYPE_PRIMARY:
			memcpy(&display->primary_setup.plane, &display_plane,
			       sizeof(display->primary_setup.plane));
			break;
		case DRM_PLANE_TYPE_OVERLAY:
			memcpy(&display->overlay_setup.plane, &display_plane,
			       sizeof(display->overlay_setup.plane));
			break;
		}

next_plane:
		drmModeFreePlane(plane);

		if (display->primary_setup.plane.id)
			break;
	}

	if (i == plane_resources->count_planes)
		goto error;

	if (!display->primary_setup.buffer_width ||
	    !display->primary_setup.buffer_height) {
		display->primary_setup.buffer_width =
			display->output.mode.hdisplay;
		display->primary_setup.buffer_height =
			display->output.mode.vdisplay;
	}

	ret = 0;
	goto complete;

error:
	ret = -1;

complete:
	if (encoder)
		drmModeFreeEncoder(encoder);

	if (crtc)
		drmModeFreeCrtc(crtc);

	if (plane_resources)
		drmModeFreePlaneResources(plane_resources);

	if (resources)
		drmModeFreeResources(resources);

	return ret;
}

static int device_open(struct drm_display *display, struct udev *udev,
		       struct udev_device *device)
{
	const char *drm_path = udev_device_get_devnode(device);
	int drm_fd = -1;

	drm_fd = open(drm_path, O_RDWR | O_NONBLOCK);
	if (drm_fd < 0)
		return -errno;

	display->drm_path = strdup(drm_path);
	display->drm_fd = drm_fd;

	return 0;
}

int drm_display_open(struct drm_display *display)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	int ret;

	if (!display)
		return -EINVAL;

	display->drm_fd = -1;

	udev = udev_new();
	if (!udev) {
		ret = -1;
		goto complete;
	}

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		ret = -1;
		goto complete;
	}

	udev_enumerate_add_match_subsystem(enumerate, "drm");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(entry, devices) {
		struct udev_device *device;
		unsigned int dev_minor;
		bool found = false;
		dev_t devnum;
		const char *path;

		path = udev_list_entry_get_name(entry);
		if (!path)
			continue;

		device = udev_device_new_from_syspath(udev, path);
		if (!device)
			continue;

		devnum = udev_device_get_devnum(device);
		dev_minor = minor(devnum);

		if (dev_minor >= 90)
			goto next;

		ret = device_open(display, udev, device);
		if (!ret)
			found = true;

next:
		udev_device_unref(device);

		if (found)
			break;
	}

	udev_enumerate_unref(enumerate);

	ret = 0;

complete:
	udev_unref(udev);

	return ret;
}

void drm_display_close(struct drm_display *display)
{
	if (!display)
		return;

	if (display->drm_path)
		free(display->drm_path);

	if (display->drm_fd > 0) {
		close(display->drm_fd);
		display->drm_fd = -1;
	}
}
