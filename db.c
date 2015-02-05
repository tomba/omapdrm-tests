
#include "test.h"

const int bar_width = 40;

static struct modeset_dev *modeset_list = NULL;

struct flip_data {
	int bar_xpos;

	unsigned num_frames_drawn;
	struct timespec flip_time;
	struct timespec draw_start_time;
	uint64_t draw_total_time;

	uint64_t min_flip_time, max_flip_time;
};

static void page_flip_event(void *data)
{
	struct modeset_dev *dev = data;
	struct timespec now;
	struct flip_data *priv = dev->data;

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
			dev->output_id,
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
		struct framebuffer *buf = &dev->bufs[(dev->front_buf + 1) % dev->num_buffers];

		struct timespec ts1, ts2;

		get_time_now(&ts1);

		priv->bar_xpos = (priv->bar_xpos + 8) % (buf->width - bar_width);

		drm_draw_color_bar(buf, priv->bar_xpos, bar_width);

		get_time_now(&ts2);

		priv->draw_total_time += get_time_elapsed_us(&ts1, &ts2);
	}

	priv->num_frames_drawn += 1;

	modeset_start_flip(dev);
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
	for_each_dev(dev, modeset_list)
		dev->data = calloc(1, sizeof(struct flip_data));

	// Set modes
	modeset_set_modes(modeset_list);

	// Draw color bar
	modeset_main_loop(modeset_list, &page_flip_event);

	// Free private data
	for_each_dev(dev, modeset_list)
		free(dev->data);

	// Free modeset data
	modeset_cleanup(modeset_list);

	close(fd);

	fprintf(stderr, "exiting\n");

	return 0;
}
