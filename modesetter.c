
#include "test.h"

static struct modeset_dev *modeset_list = NULL;

int main(int argc, char **argv)
{
	int fd;
	const char *card = "/dev/dri/card0";
	int opt;

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

	// Draw test pattern
	for (struct modeset_dev *dev = modeset_list; dev; dev = dev->next) {
		struct framebuffer *buf;
		buf = &dev->bufs[0];
		drm_draw_test_pattern(buf);
	}

	// set modes repeatedly
	while (true) {
		modeset_set_modes(modeset_list);
	}
}
