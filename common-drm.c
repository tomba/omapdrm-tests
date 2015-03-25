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
