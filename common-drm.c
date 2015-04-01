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

struct format_plane_info
{
	uint8_t bitspp;	/* bits per (macro) pixel */
	uint8_t xsub;
	uint8_t ysub;
};

struct format_info
{
	uint32_t format;
	const char *fourcc;
	uint8_t num_planes;
	struct format_plane_info planes[4];
};

static const struct format_info format_info_array[] = {
	/* YUV packed */
	{ DRM_FORMAT_UYVY, "UYVY", 1, { { 32, 2, 1 } }, },
	{ DRM_FORMAT_YUYV, "YUYV", 1, { { 32, 2, 1 } }, },
	/* YUV semi-planar */
	{ DRM_FORMAT_NV12, "NV12", 2, { { 8, 1, 1, }, { 16, 2, 2 } }, },
	/* RGB16 */
	{ DRM_FORMAT_RGB565, "RG16", 1, { { 16, 1, 1 } }, },
	/* RGB32 */
	{ DRM_FORMAT_XRGB8888, "XR24", 1, { { 32, 1, 1 } }, },
};

static const struct format_info *find_format(uint32_t format)
{
	for (int i = 0; i < ARRAY_SIZE(format_info_array); ++i) {
		if (format == format_info_array[i].format)
			return &format_info_array[i];
	}

	return NULL;
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

	const struct format_info *format_info = find_format(format);

	ASSERT(format_info);

	buf->num_planes = format_info->num_planes;

	for (int i = 0; i < format_info->num_planes; ++i) {
		const struct format_plane_info *pi = &format_info->planes[i];
		struct framebuffer_plane *plane = &buf->planes[i];

		/* create dumb buffer */
		struct drm_mode_create_dumb creq = {
			.width = buf->width / pi->xsub,
			.height = buf->height / pi->ysub,
			.bpp = pi->bitspp,
		};
		r = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
		ASSERT(r == 0);

		plane->handle = creq.handle;
		plane->stride = creq.pitch;
		plane->size = creq.height * creq.pitch;

		/*
		printf("buf %d: %dx%d, bitspp %d, stride %d, size %d\n",
			i, creq.width, creq.height, pi->bitspp, plane->stride, plane->size);
		*/

		/* prepare buffer for memory mapping */
		struct drm_mode_map_dumb mreq = {
			.handle = plane->handle,
		};
		r = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
		ASSERT(r == 0);

		/* perform actual memory mapping */
		buf->planes[i].map = mmap(0, plane->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			        fd, mreq.offset);
		ASSERT(plane->map != MAP_FAILED);

		/* clear the framebuffer to 0 */
		memset(plane->map, 0, plane->size);
	}

	/* create framebuffer object for the dumb-buffer */
	uint32_t bo_handles[4] = { buf->planes[0].handle, buf->planes[1].handle };
	uint32_t pitches[4] = { buf->planes[0].stride, buf->planes[1].stride };
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
		struct framebuffer_plane *plane = &buf->planes[i];

		/* unmap buffer */
		munmap(plane->map, plane->size);

		/* delete dumb buffer */
		drm_destroy_dumb(buf->fd, plane->handle);
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
