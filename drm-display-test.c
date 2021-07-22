/*
 * Copyright (C) 2019-2021 Paul Kocialkowski <contact@paulk.fr>
 * Copyright (C) 2020 Bootlin
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <drm-display.h>

static int test_color(struct drm_display *display)
{
	struct drm_display_buffer *buffer;
	int ret;

	display->primary_setup.buffer_format = DRM_FORMAT_XRGB8888;

	ret = drm_display_probe(display);
	if (ret)
		return 1;

	ret = drm_display_setup(display);
	if (ret)
		return ret;

	buffer = drm_display_primary_buffer_cycle(display);
	if (!buffer)
		return 1;

	memset(buffer->data[0], 0x33, buffer->strides[0] * buffer->height);

	ret = drm_display_configure(display, &display->primary_setup, buffer);
	if (ret)
		return ret;

	buffer = drm_display_primary_buffer_cycle(display);
	if (!buffer)
		return 1;

	printf("Press enter to continue ");
	getchar();

	memset(buffer->data[0], 0x99, buffer->strides[0] * buffer->height);

	ret = drm_display_page_flip(display, &display->primary_setup, buffer);
	if (ret)
		return ret;

	printf("Press enter to continue ");
	getchar();

	buffer = drm_display_primary_buffer_cycle(display);
	if (!buffer)
		return 1;

	ret = drm_display_page_flip(display, &display->primary_setup, buffer);
	if (ret)
		return ret;
}

int main(int argc, char *argv[])
{
	struct drm_display display = { 0 };
	int ret;

	ret = drm_display_open(&display);
	if (ret)
		return 1;

	ret = test_color(&display);
	if (ret)
		return 1;

	printf("Press enter to continue ");
	getchar();

	drm_display_close(&display);

	return 0;
}
