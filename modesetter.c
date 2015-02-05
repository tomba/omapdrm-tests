
#include "test.h"

const int bar_width = 40;

static struct modeset_dev *modeset_list = NULL;

int main(int argc, char **argv)
{
	int r, fd = -1;
	const char *card = "/dev/dri/card0";
	struct framebuffer *buf;
	int opt;

	while ((opt = getopt(argc, argv, "c:")) != -1) {
		switch (opt) {
		case 'c':
			card = optarg;
			break;
		}
	}

	fprintf(stderr, "Using card '%s'\n", card);

	/* open the DRM device */
	r = drm_open_dev_dumb(card, &fd);
	if (r)
		goto out_return;

	/* prepare all connectors and CRTCs */
	r = modeset_prepare(fd, 2, &modeset_list);
	if (r)
		goto out_close;

	/* perform actual modesetting on each found connector+CRTC */
	while (true) {
		for (struct modeset_dev *dev = modeset_list; dev; dev = dev->next) {
			fprintf(stderr, "Output %u: Connector %u, Encoder %u, CRTC %u, FB %u/%u, Mode %ux%u\n",
				dev->output_id,
				dev->conn_id, dev->enc_id, dev->crtc_id,
				dev->bufs[0].fb_id, dev->bufs[1].fb_id,
				dev->mode.hdisplay, dev->mode.vdisplay);

		dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id);
		buf = &dev->bufs[dev->front_buf];

		drm_draw_test_pattern(buf);

		r = drmModeSetCrtc(fd, dev->crtc_id, buf->fb_id, 0, 0,
				       &dev->conn_id, 1, &dev->mode);
		if (r)
		fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
		dev->conn_id, errno);
		}
	}

	r = 0;

out_close:
	close(fd);
out_return:
	if (r) {
		errno = -r;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return r;
}
