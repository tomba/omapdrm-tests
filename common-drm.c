#include "common-drm.h"
#include "common.h"

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
	int r;

	memset(buf, 0, sizeof(*buf));

	buf->fd = fd;
	buf->width = width;
	buf->height = height;

	/* create dumb buffer */
	struct drm_mode_create_dumb creq = {
		.width = buf->width,
		.height = buf->height,
		.bpp = 32,
	};
	r = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	ASSERT(r == 0);
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	r = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
			   buf->handle, &buf->fb_id);
	ASSERT(r == 0);

	/* prepare buffer for memory mapping */
	struct drm_mode_map_dumb mreq = {
		.handle = buf->handle,
	};
	r = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	ASSERT(r == 0);

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	ASSERT(buf->map != MAP_FAILED);

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);
}

void drm_destroy_dumb_fb(struct framebuffer *buf)
{
	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(buf->fd, buf->fb_id);

	/* delete dumb buffer */
	drm_destroy_dumb(buf->fd, buf->handle);

	memset(buf, 0, sizeof(*buf));
}

void draw_pixel(struct framebuffer *buf, int x, int y, uint32_t color)
{
	uint32_t *p;
	p = (uint32_t*)(buf->map + buf->stride * y + x * 4);
	*p = color;
}

void drm_draw_test_pattern(struct framebuffer *fb_info)
{
	unsigned x, y;
	unsigned w = fb_info->width;
	unsigned h = fb_info->height;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (x < 20 && y < 20)
				draw_pixel(fb_info, x, y, 0xffffff);
			else if (x < 20 && (y > 20 && y < h - 20))
				draw_pixel(fb_info, x, y, 0xff);
			else if (y < 20 && (x > 20 && x < w - 20))
				draw_pixel(fb_info, x, y, 0xff00);
			else if (x > w - 20 && (y > 20 && y < h - 20))
				draw_pixel(fb_info, x, y, 0xff0000);
			else if (y > h - 20 && (x > 20 && x < w - 20))
				draw_pixel(fb_info, x, y, 0xffff00);
			else if (x == 20 || x == w - 20 ||
					y == 20 || y == h - 20)
				draw_pixel(fb_info, x, y, 0xffffff);
			else if (x == y || w - x == h - y)
				draw_pixel(fb_info, x, y, 0xff00ff);
			else if (w - x == y || x == h - y)
				draw_pixel(fb_info, x, y, 0x00ffff);
			else if (x > 20 && y > 20 && x < w - 20 && y < h - 20) {
				int t = x * 3 / w;
				unsigned r = 0, g = 0, b = 0;

				if (t == 0)
					b = (y % 256);
				else if (t == 1)
					g = (y % 256);
				else if (t == 2)
					r = (y % 256);

				unsigned c = (r << 16) | (g << 8) | (b << 0);
				draw_pixel(fb_info, x, y, c);
			} else {
				draw_pixel(fb_info, x, y, 0);
			}
		}
	}
}

void drm_draw_color_bar(struct framebuffer *buf, int xpos, int width)
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
		uint32_t *line = (uint32_t*)(buf->map + buf->stride * y);

		memset(line, 0, buf->width * 4);

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
