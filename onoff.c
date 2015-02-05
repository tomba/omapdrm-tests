
#include "test.h"

static struct modeset_dev *modeset_list = NULL;

int main(int argc, char **argv)
{
	int opt;
	int fd;
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
	modeset_alloc_fbs(modeset_list, 1);

	// Draw test pattern
	for_each_dev(dev, modeset_list) {
		struct framebuffer *buf;
		buf = &dev->bufs[0];
		drm_draw_test_pattern(buf);
	}

	// Set modes
	modeset_set_modes(modeset_list);

	// turnd connectors on and off repeatedly
	int c = 0;

	while (true) {
		usleep(500000);
		for_each_dev(dev, modeset_list) {
			usleep(1500000);
			drm_set_dpms(fd, dev->conn_id, dev->dpms);

			dev->dpms = (c & 1) ? DRM_MODE_DPMS_OFF : DRM_MODE_DPMS_ON;
		}

		c++;
	}
}
