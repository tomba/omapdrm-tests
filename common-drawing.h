#ifndef _DRAWING_H_
#define _DRAWING_H_

static inline uint32_t MAKE_RGB(uint8_t r, uint8_t g, uint8_t b)
{
	return (r << 16) | (g << 8) | (b << 0);
}

static inline uint32_t MAKE_YUV_601_Y(uint8_t r, uint8_t g, uint8_t b)
{
	return (((66 * r + 129 * g +  25 * b + 128) >> 8) + 16);
}

static inline uint32_t MAKE_YUV_601_U(uint8_t r, uint8_t g, uint8_t b)
{
	return (((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128);
}

static inline uint32_t MAKE_YUV_601_V(uint8_t r, uint8_t g, uint8_t b)
{
	return (((112 * r -  94 * g -  18 * b + 128) >> 8) + 128);
}

void drm_draw_color_bar(struct framebuffer *buf, int old_xpos, int xpos, int width);
void draw_pixel(struct framebuffer *buf, int x, int y, uint32_t color);
void drm_draw_test_pattern(struct framebuffer *fb, int pattern);
void drm_clear_fb(struct framebuffer *fb);

#endif
