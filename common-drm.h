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
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb_id;
	struct omap_bo *omap_bo;
};

int drm_open_dev_dumb(const char *node, int *out);
int drm_create_dumb_fb(int fd, uint32_t width, uint32_t height, struct framebuffer *buf);
void drm_destroy_dumb_fb(struct framebuffer *buf);
void drm_draw_test_pattern(struct framebuffer *buf);
void drm_draw_color_bar(struct framebuffer *buf, int xpos, int width);
int drm_set_dpms(int fd, uint32_t conn_id, int dpms);
void draw_pixel(struct framebuffer *buf, int x, int y, uint32_t color);
void drm_draw_test_pattern(struct framebuffer *fb_info);
void drm_draw_color_bar(struct framebuffer *buf, int xpos, int width);

#endif
