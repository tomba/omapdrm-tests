#ifndef _COMMON_DRM_H_
#define _COMMON_DRM_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct omap_bo;

struct framebuffer {
	int fd;

	uint32_t width;
	uint32_t height;
	uint32_t format;

	int num_planes;
	uint32_t stride[2];
	uint32_t size[2];
	uint32_t handle[2];
	uint8_t *map[2];

	uint32_t fb_id;
	struct omap_bo *omap_bo;
};

int drm_open_dev_dumb(const char *node);
void drm_create_dumb_fb(int fd, uint32_t width, uint32_t height, struct framebuffer *buf);
void drm_create_dumb_fb2(int fd, uint32_t width, uint32_t height, uint32_t format,
	struct framebuffer *buf);
void drm_destroy_dumb_fb(struct framebuffer *buf);
void drm_draw_color_bar(struct framebuffer *buf, int old_xpos, int xpos, int width);
void drm_set_dpms(int fd, uint32_t conn_id, int dpms);
void draw_pixel(struct framebuffer *buf, int x, int y, uint32_t color);
void drm_draw_test_pattern(struct framebuffer *fb, int pattern);
void drm_clear_fb(struct framebuffer *fb);

#define for_each_output(pos, head) \
	for (struct modeset_out *(pos) = (head); (pos); (pos) = (pos)->next)

#endif
