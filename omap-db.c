#include <omap_drmif.h>

#include "test.h"

static const int bar_width = 40;
static const int bar_speed = 8;

#define ALIGN2(x,n)   (((x) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))
#define PAGE_SHIFT 12

static struct modeset_out *modeset_list = NULL;

struct {
	int drm_fd;
	struct omap_device *omap_dev;
} global;

struct flip_data {
	int bar_xpos;

	unsigned num_frames_drawn;
	struct timespec flip_time;
	struct timespec draw_start_time;
	uint64_t draw_total_time;

	uint64_t min_flip_time, max_flip_time;
};

static int drm_open_dev_omap(const char *node, struct omap_device **omap_dev)
{
	int fd = open(node, O_RDWR | O_CLOEXEC);
	ASSERT(fd >= 0);

	struct omap_device *dev = omap_device_new(fd);
	ASSERT(dev);

	*omap_dev = dev;
	return fd;
}

static void drm_close_dev_omap(int fd, struct omap_device *omap_dev)
{
	omap_device_del(omap_dev);
	close(fd);
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

static void create_omap_fb(int fd, struct omap_device *omap_dev,
	uint32_t width, uint32_t height, uint32_t format,
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

		int bpp = pi->bitspp;

		uint32_t flags = 0
			| OMAP_BO_WC
			| OMAP_BO_SCANOUT
			| OMAP_BO_TILED
			;

		if (flags & OMAP_BO_TILED) {
			flags &= ~OMAP_BO_TILED;
			if (bpp == 8) {
				flags |= OMAP_BO_TILED_8;
			} else if (bpp == 16) {
				flags |= OMAP_BO_TILED_16;
			} else if (bpp == 32) {
				flags |= OMAP_BO_TILED_32;
			} else {
				ASSERT(false);
			}
		}

		struct omap_bo *bo;

		if (flags & OMAP_BO_TILED) {
			bo = omap_bo_new_tiled(omap_dev, width, height, flags);
		} else {
			bo = omap_bo_new(omap_dev, width * height * bpp / 8, flags);
		}

		ASSERT(bo);

		plane->handle = omap_bo_handle(bo);
		plane->stride = (flags & OMAP_BO_TILED) ?
				ALIGN2(width * bpp / 8, PAGE_SHIFT) :
				width * bpp / 8;
		// XXX omap_bo_size() doesn't return correct value for NV12?
		plane->size = plane->stride * height;	//omap_bo_size(bo);
		plane->omap_bo = bo;

		/*
		printf("buf %d: %dx%d, bitspp %d, stride %d, size %d\n",
			i, creq.width, creq.height, pi->bitspp, plane->stride, plane->size);
		*/

		plane->map = omap_bo_map(bo);
		ASSERT(plane->map);

		/* clear the framebuffer to 0 */
		memset(plane->map, 0, plane->size);
	}

	/* create framebuffer object */
	uint32_t bo_handles[4] = { buf->planes[0].handle, buf->planes[1].handle };
	uint32_t pitches[4] = { buf->planes[0].stride, buf->planes[1].stride };
	uint32_t offsets[4] = { 0 };
	r = drmModeAddFB2(fd, buf->width, buf->height, format,
		bo_handles, pitches, offsets, &buf->fb_id, 0);
	ASSERT(r == 0);
}

static void destroy_omap_fb(struct framebuffer *fb)
{
	drmModeRmFB(fb->fd, fb->fb_id);

	for (int i = 0; i < fb->num_planes; ++i) {
		struct framebuffer_plane *plane = &fb->planes[i];

		/* unmap buffer */
		munmap(plane->map, plane->size);

		/* delete buffer */
		omap_bo_del(plane->omap_bo);
	}

	memset(fb, 0, sizeof(*fb));
}

static void page_flip_event(void *data)
{
	struct modeset_out *out = data;
	struct timespec now;
	struct flip_data *priv = out->data;

	get_time_now(&now);

	/* initialize values on first flip */
	if (priv->num_frames_drawn == 0) {
		priv->min_flip_time = UINT64_MAX;
		priv->max_flip_time = 0;
		priv->draw_start_time = now;
		priv->flip_time = now;
		priv->draw_total_time = 0;
	}

	/* measure min/max flip time */
	if (priv->num_frames_drawn > 0) {
		uint64_t us;

		us = get_time_elapsed_us(&priv->flip_time, &now);

		priv->flip_time = now;

		if (us < priv->min_flip_time)
			priv->min_flip_time = us;

		if (us > priv->max_flip_time)
			priv->max_flip_time = us;
	}

	const int measure_interval = 100;

	if (priv->num_frames_drawn > 0 &&
		priv->num_frames_drawn % measure_interval == 0) {
		uint64_t us;
		float flip_avg, draw_avg;

		us = get_time_elapsed_us(&priv->draw_start_time, &now);
		flip_avg = (float)us / measure_interval / 1000;

		draw_avg = (float)priv->draw_total_time / measure_interval / 1000;

		printf("Output %u: draw %f ms, flip avg/min/max %f/%f/%f\n",
			out->output_id,
			draw_avg,
			flip_avg,
			priv->min_flip_time / 1000.0,
			priv->max_flip_time / 1000.0);

		priv->draw_start_time = now;
		priv->draw_total_time = 0;

		priv->min_flip_time = UINT64_MAX;
		priv->max_flip_time = 0;
	}

	/* draw */
	{
		/* back buffer */
		struct framebuffer *buf = &out->bufs[(out->front_buf + 1) % out->num_buffers];

		struct timespec ts1, ts2;

		get_time_now(&ts1);

		int old_xpos = (priv->bar_xpos + (buf->width - bar_width - bar_speed)) %
			(buf->width - bar_width);

		priv->bar_xpos = (priv->bar_xpos + bar_speed) % (buf->width - bar_width);

		//omap_bo_cpu_prep(buf->omap_bo, OMAP_GEM_WRITE);
		drm_draw_color_bar(buf, old_xpos, priv->bar_xpos, bar_width);
		//omap_bo_cpu_fini(buf->omap_bo, OMAP_GEM_WRITE);

		get_time_now(&ts2);

		priv->draw_total_time += get_time_elapsed_us(&ts1, &ts2);
	}

	priv->num_frames_drawn += 1;

	modeset_start_flip(out);
}

int main(int argc, char **argv)
{
	// open the DRM device
	global.drm_fd = drm_open_dev_omap("/dev/dri/card0", &global.omap_dev);

	// Prepare all connectors and CRTCs
	modeset_prepare(global.drm_fd, &modeset_list);

	// Allocate buffers
	int num_buffers = 2;
	for_each_output(out, modeset_list) {
		struct framebuffer *bufs;
		int i;

		bufs = malloc(num_buffers * sizeof(*bufs));
		ASSERT(bufs);

		for(i = 0 ; i < num_buffers; i++)
			create_omap_fb(global.drm_fd, global.omap_dev,
				out->mode.hdisplay, out->mode.vdisplay,
				DRM_FORMAT_XRGB8888,
				&bufs[i]);

		out->bufs = bufs;
		out->num_buffers = num_buffers;
	}

	// Allocate private data
	for_each_output(out, modeset_list)
		out->data = calloc(1, sizeof(struct flip_data));

	// Set modes
	modeset_set_modes(modeset_list);

	// Draw color bar
	modeset_main_loop(modeset_list, &page_flip_event);

	// Free private data
	for_each_output(out, modeset_list)
		free(out->data);

	// Free modeset data
	{
	struct modeset_out *out_list = modeset_list;
	struct modeset_out *iter;
	unsigned int i;

	while (out_list) {
		/* remove from global list */
		iter = out_list;
		out_list = iter->next;

		/* destroy framebuffers */
		for(i = 0; i < iter->num_buffers; i++) {
			destroy_omap_fb(&iter->bufs[i]);
		}

		/* free allocated memory */
		free(iter->bufs);
		free(iter);
	}
	}

	drm_close_dev_omap(global.drm_fd, global.omap_dev);

	fprintf(stderr, "exiting\n");

	return 0;
}
