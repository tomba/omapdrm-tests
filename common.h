#ifndef _COMMON_H_
#define _COMMON_H_

/* common.c */
void get_time_now(struct timespec *ts);
uint64_t get_time_elapsed_us(const struct timespec *ts_start, const struct timespec *ts_end);

/* common-drm.c */
struct framebuffer {
	int fd;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb_id;
};

int drm_open_dev_dumb(const char *node, int *out);
int drm_create_dumb_fb(int fd, uint32_t width, uint32_t height, struct framebuffer *buf);
void drm_destroy_dumb_fb(struct framebuffer *buf);
void drm_draw_color_bar(struct framebuffer *buf, int xpos, int width);
int drm_set_dpms(int fd, uint32_t conn_id, int dpms);

#endif
