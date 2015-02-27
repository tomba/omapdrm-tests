
#include "test.h"

static struct modeset_out *modeset_list = NULL;

static void usage()
{
	printf("usage: testpat <pattern>\n");

	exit(1);
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
		default:
			usage();
		}
	}

	int pattern;

	int numargs = argc - optind;
	if (numargs == 0)
		pattern = 0;
	else if (numargs == 1)
		pattern = atoi(argv[optind]);
	else
		usage();

	// open the DRM device
	fd = drm_open_dev_dumb(card);

	// Prepare all connectors and CRTCs
	modeset_prepare(fd, &modeset_list);

	// Allocate buffers
	modeset_alloc_fbs(modeset_list, 1);

	// Draw test pattern
	for_each_output(out, modeset_list) {
		struct framebuffer *buf;
		buf = &out->bufs[0];
		drm_draw_test_pattern(buf, pattern);
	}

	// Set modes
	modeset_set_modes(modeset_list);

	printf("press enter to exit\n");

	getchar();

	// Free modeset data
	modeset_cleanup(modeset_list);

	close(fd);

	fprintf(stderr, "exiting\n");

	return 0;
}
