#include "common-drm.h"
#include "common.h"

#include <drm/drm_fourcc.h>

int drm_open_dev_dumb(const char *node)
{
	int fd = open(node, O_RDWR | O_CLOEXEC);
	ASSERT(fd >= 0);

	uint64_t has_dumb;

	ASSERT (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) == 0 && has_dumb);

	return fd;
}

void drm_destroy_dumb(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb dreq = {
		.handle = handle,
	};

	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

void drm_create_dumb_fb(int fd, uint32_t width, uint32_t height, struct framebuffer *buf)
{
	drm_create_dumb_fb2(fd, width, height, DRM_FORMAT_XRGB8888, buf);
}

void drm_create_dumb_fb2(int fd, uint32_t width, uint32_t height, uint32_t format,
	struct framebuffer *buf)
{
	int r;

	memset(buf, 0, sizeof(*buf));

	buf->fd = fd;
	buf->width = width;
	buf->height = height;
	buf->format = format;

	unsigned bpps[2];

	switch (format) {
		case DRM_FORMAT_NV12:
		case DRM_FORMAT_NV21:
			buf->num_planes = 2;
			// XXX these work but do not sound correct
			// bpps[1] should be 4?
			bpps[0] = 8;
			bpps[1] = 8;
			break;

		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_UYVY:
			buf->num_planes = 1;
			bpps[0] = 16;
			break;

		case DRM_FORMAT_XRGB8888:
			buf->num_planes = 1;
			bpps[0] = 32;
			break;

		case DRM_FORMAT_RGB565:
			buf->num_planes = 1;
			bpps[0] = 16;
			break;

		default:
			ASSERT(false);
	}

	for (int i = 0; i < buf->num_planes; ++i) {
		/* create dumb buffer */
		struct drm_mode_create_dumb creq = {
			.width = buf->width,
			.height = buf->height,
			.bpp = bpps[i],
		};
		r = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
		ASSERT(r == 0);

		buf->handle[i] = creq.handle;
		buf->stride[i] = creq.pitch;
		buf->size[i] = buf->height * creq.pitch;

		/* prepare buffer for memory mapping */
		struct drm_mode_map_dumb mreq = {
			.handle = buf->handle[i],
		};
		r = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
		ASSERT(r == 0);

		/* perform actual memory mapping */
		buf->map[i] = mmap(0, buf->size[i], PROT_READ | PROT_WRITE, MAP_SHARED,
			        fd, mreq.offset);
		ASSERT(buf->map[i] != MAP_FAILED);

		/* clear the framebuffer to 0 */
		memset(buf->map[i], 0, buf->size[i]);
	}

	/* create framebuffer object for the dumb-buffer */
	uint32_t bo_handles[4] = { buf->handle[0], buf->handle[1] };
	uint32_t pitches[4] = { buf->stride[0], buf->stride[1] };
	uint32_t offsets[4] = { 0 };
	r = drmModeAddFB2(fd, buf->width, buf->height, format,
		bo_handles, pitches, offsets, &buf->fb_id, 0);
	ASSERT(r == 0);
}

void drm_destroy_dumb_fb(struct framebuffer *buf)
{
	/* delete framebuffer */
	drmModeRmFB(buf->fd, buf->fb_id);

	for (int i = 0; i < buf->num_planes; ++i) {
		/* unmap buffer */
		munmap(buf->map[i], buf->size[i]);

		/* delete dumb buffer */
		drm_destroy_dumb(buf->fd, buf->handle[i]);
	}

	memset(buf, 0, sizeof(*buf));
}

void draw_pixel(struct framebuffer *buf, int x, int y, uint32_t color)
{
	uint32_t *p;
	p = (uint32_t*)(buf->map[0] + buf->stride[0] * y + x * 4);
	*p = color;
}

static void drm_draw_test_pattern_default(struct framebuffer *fb)
{
	unsigned x, y;
	unsigned w = fb->width;
	unsigned h = fb->height;

	const int mw = 20;

	const int xm1 = mw;
	const int xm2 = w - mw - 1;
	const int ym1 = mw;
	const int ym2 = h - mw - 1;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			// white margin lines
			if (x == xm1 || x == xm2 || y == ym1 || y == ym2)
				draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
			// white box outlines to corners
			else if ((x == 0 || x == w - 1) && (y < ym1 || y > ym2))
				draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
			// white box outlines to corners
			else if ((y == 0 || y == h - 1) && (x < xm1 || x > xm2))
				draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
			// blue bar on the left
			else if (x < xm1 && (y > ym1 && y < ym2))
				draw_pixel(fb, x, y, MAKE_RGB(0, 0, 255));
			// blue bar on the top
			else if (y < ym1 && (x > xm1 && x < xm2))
				draw_pixel(fb, x, y, MAKE_RGB(0, 0, 255));
			// red bar on the right
			else if (x > xm2 && (y > ym1 && y < ym2))
				draw_pixel(fb, x, y, MAKE_RGB(255, 0, 0));
			// red bar on the bottom
			else if (y > ym2 && (x > xm1 && x < xm2))
				draw_pixel(fb, x, y, MAKE_RGB(255, 0, 0));
			// inside the margins
			else if (x > xm1 && x < xm2 && y > ym1 && y < ym2) {
				// diagonal line
				if (x == y || w - x == h - y)
					draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
				// diagonal line
				else if (w - x == y || x == h - y)
					draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
				else {
					int t = (x - xm1 - 1) * 3 / (xm2 - xm1 - 1);
					unsigned r = 0, g = 0, b = 0;

					unsigned c = (y - ym1 - 1) % 256;

					switch (t) {
					case 0:
						r = c;
						break;
					case 1:
						g = c;
						break;
					case 2:
						b = c;
						break;
					}

					draw_pixel(fb, x, y, MAKE_RGB(r, g, b));
				}
			// black corners
			} else {
				draw_pixel(fb, x, y, 0);
			}
		}
	}
}

static void drm_draw_test_pattern_edges(struct framebuffer *fb)
{
	unsigned x, y;
	unsigned w = fb->width;
	unsigned h = fb->height;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				draw_pixel(fb, x, y, MAKE_RGB(255, 255, 255));
			else
				draw_pixel(fb, x, y, 0);
		}
	}
}

static void fill_smpte_rgb32(struct framebuffer *fb)
{
	const uint32_t colors_top[] = {
		MAKE_RGB(192, 192, 192),/* grey */
		MAKE_RGB(192, 192, 0),	/* yellow */
		MAKE_RGB(0, 192, 192),	/* cyan */
		MAKE_RGB(0, 192, 0),	/* green */
		MAKE_RGB(192, 0, 192),	/* magenta */
		MAKE_RGB(192, 0, 0),	/* red */
		MAKE_RGB(0, 0, 192),	/* blue */
	};

	const uint32_t colors_middle[] = {
		MAKE_RGB(0, 0, 192),	/* blue */
		MAKE_RGB(19, 19, 19),	/* black */
		MAKE_RGB(192, 0, 192),	/* magenta */
		MAKE_RGB(19, 19, 19),	/* black */
		MAKE_RGB(0, 192, 192),	/* cyan */
		MAKE_RGB(19, 19, 19),	/* black */
		MAKE_RGB(192, 192, 192),/* grey */
	};

	const uint32_t colors_bottom[] = {
		MAKE_RGB(0, 33, 76),	/* in-phase */
		MAKE_RGB(255, 255, 255),/* super white */
		MAKE_RGB(50, 0, 106),	/* quadrature */
		MAKE_RGB(19, 19, 19),	/* black */
		MAKE_RGB(9, 9, 9),	/* 3.5% */
		MAKE_RGB(19, 19, 19),	/* 7.5% */
		MAKE_RGB(29, 29, 29),	/* 11.5% */
		MAKE_RGB(19, 19, 19),	/* black */
	};

	unsigned width = fb->width;
	unsigned height = fb->height;

	unsigned int x;
	unsigned int y;

	for (y = 0; y < height * 6 / 9; ++y) {
		for (x = 0; x < width; ++x)
			draw_pixel(fb, x, y, colors_top[x * 7 / width]);
	}

	for (; y < height * 7 / 9; ++y) {
		for (x = 0; x < width; ++x)
			draw_pixel(fb, x, y, colors_middle[x * 7 / width]);
	}

	for (; y < height; ++y) {
		for (x = 0; x < width * 5 / 7; ++x)
			draw_pixel(fb, x, y,
				colors_bottom[x * 4 / (width * 5 / 7)]);
		for (; x < width * 6 / 7; ++x)
			draw_pixel(fb, x, y,
				colors_bottom[(x - width * 5 / 7) * 3 / (width / 7) + 4]);
		for (; x < width; ++x)
			draw_pixel(fb, x, y, colors_bottom[7]);
	}
}

struct color
{
	union {
		struct {
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t __unused1;
		};
		struct {
			uint8_t y;
			uint8_t u;
			uint8_t v;
			uint8_t __unused2;
		};
	};
};

static struct color rgb_to_yuv_pixel(struct color rgb)
{
	unsigned r = rgb.r;
	unsigned g = rgb.g;
	unsigned b = rgb.b;

	struct color color = {
		.y = MAKE_YUV_601_Y(r, g, b),
		.u = MAKE_YUV_601_U(r, g, b),
		.v = MAKE_YUV_601_V(r, g, b),
	};

	return color;
}

static struct color read_rgb(struct framebuffer *fb, int x, int y)
{
	uint32_t *pc = (uint32_t *)(fb->map[0] + fb->stride[0] * y);

	uint32_t c = pc[x];

	struct color color = {
		.r = (c >> 16) & 0xff,
		.g = (c >> 8) & 0xff,
		.b = c & 0xff,
	};

	return color;
}

static struct color read_rgb_as_yuv(struct framebuffer *fb, int x, int y)
{
	struct color rgb = read_rgb(fb, x, y);
	return rgb_to_yuv_pixel(rgb);
}

static void rgb_to_packed_yuv(struct framebuffer *dst_fb, struct framebuffer *src_fb)
{
	unsigned w = src_fb->width;
	unsigned h = src_fb->height;

	uint8_t *dst = dst_fb->map[0];

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; x += 2) {
			struct color yuv1 = read_rgb_as_yuv(src_fb, x + 0, y);
			struct color yuv2 = read_rgb_as_yuv(src_fb, x + 1, y);

			switch (dst_fb->format) {
				case DRM_FORMAT_UYVY:
					dst[x * 2 + 0] = (yuv1.u + yuv2.u) / 2;
					dst[x * 2 + 1] = yuv1.y;
					dst[x * 2 + 2] = (yuv1.v + yuv2.v) / 2;
					dst[x * 2 + 3] = yuv2.y;
					break;
				case DRM_FORMAT_YUYV:
					dst[x * 2 + 0] = yuv1.y;
					dst[x * 2 + 1] = (yuv1.u + yuv2.u) / 2;
					dst[x * 2 + 2] = yuv2.y;
					dst[x * 2 + 3] = (yuv1.v + yuv2.v) / 2;
					break;

				default:
					ASSERT(false);
			}
		}

		dst += dst_fb->stride[0];
	}
}

static void rgb_to_semiplanar_yuv(struct framebuffer *dst_fb, struct framebuffer *src_fb)
{
	unsigned w = src_fb->width;
	unsigned h = src_fb->height;

	uint8_t *dst_y = dst_fb->map[0];
	uint8_t *dst_uv = dst_fb->map[1];

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			struct color yuv = read_rgb_as_yuv(src_fb, x, y);
			dst_y[x] = yuv.y;
		}

		dst_y += dst_fb->stride[0];
	}

	for (int y = 0; y < h; y += 2) {
		for (int x = 0; x < w; x += 2) {
			struct color yuv00 = read_rgb_as_yuv(src_fb, x + 0, y + 0);
			struct color yuv01 = read_rgb_as_yuv(src_fb, x + 1, y + 0);
			struct color yuv10 = read_rgb_as_yuv(src_fb, x + 0, y + 1);
			struct color yuv11 = read_rgb_as_yuv(src_fb, x + 1, y + 1);

			unsigned u = (yuv00.u + yuv01.u + yuv10.u + yuv11.u) / 4;
			unsigned v = (yuv00.v + yuv01.v + yuv10.v + yuv11.v) / 4;

			dst_uv[x + 0] = u;
			dst_uv[x + 1] = v;
		}

		dst_uv += dst_fb->stride[1];
	}
}

static void rgb_to_rgb565(struct framebuffer *dst_fb, struct framebuffer *src_fb)
{
	unsigned w = src_fb->width;
	unsigned h = src_fb->height;

	uint8_t *dst = dst_fb->map[0];

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			struct color rgb = read_rgb(src_fb, x, y);

			unsigned r = rgb.r * 32 / 256;
			unsigned g = rgb.g * 64 / 256;
			unsigned b = rgb.b * 32 / 256;

			((uint16_t *)dst)[x] = (r << 11) | (g << 5) | (b << 0);
		}

		dst += dst_fb->stride[0];
	}
}

static void color_convert_fb(struct framebuffer *dst, struct framebuffer *src)
{
	switch (dst->format) {
		case DRM_FORMAT_NV12:
		case DRM_FORMAT_NV21:
			rgb_to_semiplanar_yuv(dst, src);
			break;

		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_UYVY:
			rgb_to_packed_yuv(dst, src);
			break;

		case DRM_FORMAT_RGB565:
			rgb_to_rgb565(dst, src);
			break;

		default:
			ASSERT(false);
	}
}

static void draw_rgb_test_pattern(struct framebuffer *fb, int pattern)
{
	switch (pattern) {
	case 0:
	default:
		drm_draw_test_pattern_default(fb);
		break;
	case 1:
		fill_smpte_rgb32(fb);
		break;
	case 2:
		drm_draw_test_pattern_edges(fb);
		break;
	}
}

void drm_draw_test_pattern(struct framebuffer *fb, int pattern)
{
	if (fb->format == DRM_FORMAT_XRGB8888) {
		draw_rgb_test_pattern(fb, pattern);
		return;
	}

	/* draw and convert */

	struct framebuffer *orig_fb = fb;

	struct framebuffer new_fb = {
		.width = fb->width,
		.height = fb->height,
		.format = DRM_FORMAT_XRGB8888,
		.stride[0] = fb->width * 4,
		.size[0] = fb->width * 4 * fb->height,
	};

	fb = &new_fb;
	size_t size = fb->stride[0] * fb->height;
	fb->map[0] = malloc(size);

	draw_rgb_test_pattern(fb, pattern);

	color_convert_fb(orig_fb, fb);

	free(fb->map[0]);
}

void drm_clear_fb(struct framebuffer *fb)
{
	for (int i = 0; i < fb->num_planes; ++i)
		memset(fb->map[i], 0, fb->size[i]);
}

void drm_draw_color_bar(struct framebuffer *buf, int old_xpos, int xpos, int width)
{
	const unsigned int colors32[] = {
		0xffffff,
		0xff0000,
		0xffffff,
		0x00ff00,
		0xffffff,
		0x0000ff,
		0xffffff,
		0xaaaaaa,
		0xffffff,
		0x777777,
		0xffffff,
		0x333333,
		0xffffff,
	};

	for (unsigned y = 0; y < buf->height; ++y) {
		unsigned int bcol = colors32[y * sizeof(colors32) / 4 / buf->height];
		uint32_t *line = (uint32_t*)(buf->map[0] + buf->stride[0] * y);

		if (old_xpos >= 0) {
			for (unsigned x = old_xpos; x < old_xpos + width; ++x)
				line[x] = 0;
		}

		for (unsigned x = xpos; x < xpos + width; ++x)
			line[x] = bcol;
	}
}

void drm_set_dpms(int fd, uint32_t conn_id, int dpms)
{
	uint32_t prop = 0;
	drmModeObjectProperties *props;
	drmModePropertyRes *propRes;
	int j;
	int r;

	printf("set dpms %u: %d\n", conn_id, dpms);

	props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
	for (j = 0; j < props->count_props; j++) {
		propRes = drmModeGetProperty(fd, props->props[j]);

		if (propRes == NULL)
			continue;

		if (strcmp(propRes->name, "DPMS") == 0) {
			prop = props->props[j];
			break;
		}
	}

	ASSERT(prop);

	r = drmModeObjectSetProperty(fd, conn_id,
		DRM_MODE_OBJECT_CONNECTOR, prop, dpms);
	ASSERT(r == 0);
}
