
#include "test.h"
#include <drm/drm_fourcc.h>

static struct modeset_out *modeset_list = NULL;

struct flip_data {
	uint32_t plane_id;
	struct framebuffer plane_buf;
	int w, h;
};

static void find_planes(int fd, struct modeset_out *modeset_list)
{
	drmModePlaneRes *plane_resources;
	int cur_plane;

	plane_resources = drmModeGetPlaneResources(fd);
	ASSERT(plane_resources);

	cur_plane = 0;

	for_each_output(out, modeset_list) {
		struct flip_data *pdata = out->data;

		for (; cur_plane < plane_resources->count_planes; cur_plane++) {
			drmModePlane *ovr;

			ovr = drmModeGetPlane(fd, plane_resources->planes[cur_plane]);
			ASSERT(ovr);

			printf("Output %d: using plane %d\n",
				out->output_id, ovr->plane_id);

			pdata->plane_id = ovr->plane_id;

			cur_plane++;

			drmModeFreePlane(ovr);

			break;
		}
	}

	drmModeFreePlaneResources(plane_resources);
}

int main(int argc, char **argv)
{
	int fd;
	int opt;
	const char *card = "/dev/dri/card0";

	while ((opt = getopt(argc, argv, "c:")) != -1) {
		switch (opt) {
		case 'c':
			card = optarg;
			break;
		}
	}

	// open the DRM device
	fd = drm_open_dev_dumb(card);

	// Prepare all connectors and CRTCs
	modeset_prepare(fd, &modeset_list);

	// Allocate buffers
	modeset_alloc_fbs(modeset_list, 2);

	// Allocate private data
	for_each_output(out, modeset_list)
		out->data = calloc(1, sizeof(struct flip_data));

	find_planes(fd, modeset_list);

	for_each_output(out, modeset_list) {
		struct flip_data *pdata = out->data;

		drm_create_dumb_fb2(out->fd,
			out->mode.hdisplay, out->mode.vdisplay,
			DRM_FORMAT_NV12,
			&pdata->plane_buf);

		drm_draw_test_pattern(&pdata->plane_buf, 0);

		pdata->w = out->mode.hdisplay;
		pdata->h = out->mode.vdisplay;
	}

	// Set modes
	modeset_set_modes(modeset_list);

	while (true) {

	for_each_output(out, modeset_list) {
		struct flip_data *pdata = out->data;
		struct framebuffer *buf;

		buf = &pdata->plane_buf;

		if (pdata->w < buf->width / 4)
			pdata->w = buf->width;
		if (pdata->h < buf->height / 4)
			pdata->h = buf->height;

		printf("%d: %4dx%4d -> %4dx%4d (%1.2f x %1.2f)    ",
			out->output_id,
			buf->width, buf->height, pdata->w, pdata->h,
			(float)pdata->w / buf->width,
			(float)pdata->h / buf->height);
	}

	printf("\n"); fflush(stdout);

	for_each_output(out, modeset_list) {
		struct flip_data *pdata = out->data;
		struct framebuffer *buf;
		int r;

		buf = &pdata->plane_buf;

		r = drmModeSetPlane(out->fd, pdata->plane_id, out->crtc_id,
			buf->fb_id, 0,
			0, 0, pdata->w, pdata->h,
			0 << 16, 0 << 16,
			buf->width << 16, buf->height << 16);
		ASSERT(r == 0);
	}

	for_each_output(out, modeset_list) {
		struct flip_data *pdata = out->data;
		pdata->w -= 1;
		pdata->h -= 1;
	}

	usleep(1000 * 32);

	}

	// Free private data
	for_each_output(out, modeset_list)
		free(out->data);

	// Free modeset data
	modeset_cleanup(modeset_list);

	close(fd);

	fprintf(stderr, "exiting\n");

	return 0;
}
