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

#include "common.h"

int drm_open_dev_dumb(const char *node, int *out)
{
	int fd;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int r = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return r;
	}

	uint64_t has_dumb;

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

static void drm_destroy_dumb(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb dreq = {
		.handle = handle,
	};

	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

int drm_create_dumb_fb(int fd, uint32_t width, uint32_t height, struct framebuffer *buf)
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
	if (r < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	r = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
			   buf->handle, &buf->fb_id);
	if (r) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		r = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	struct drm_mode_map_dumb mreq = {
		.handle = buf->handle,
	};
	r = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (r) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		r = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		r = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb_id);
err_destroy:
	drm_destroy_dumb(fd, buf->handle);

	return r;
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
		unsigned int bcol = colors32[y / (buf->height / (sizeof(colors32) / 4))];
		uint32_t *line = (uint32_t*)(buf->map + buf->stride * y);

		memset(line, 0, buf->width * 4);

		for (unsigned x = xpos; x < xpos + width; ++x)
			line[x] = bcol;
	}
}

int drm_set_dpms(int fd, uint32_t conn_id, int dpms)
{
	uint32_t prop = 0;
	drmModeObjectProperties *props;
	drmModePropertyRes *propRes;
	int j;
	int r;
	printf("set dpms %d\n", dpms);

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

	if (prop == 0) {
		printf("missing rotation property\n");
		return -ENODEV;
	}

	r = drmModeObjectSetProperty(fd, conn_id,
		DRM_MODE_OBJECT_CONNECTOR, prop, dpms);
	if (r != 0) {
		printf("drmModeObjectSetProperty failed with %d\n", r);
		return r;
	}

	return 0;
}
