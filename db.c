

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"

struct modeset_dev {
	struct modeset_dev *next;

	unsigned int front_buf;
	struct framebuffer bufs[2];

	uint32_t output_id;

	drmModeModeInfo mode;
	uint32_t conn_id;
	uint32_t enc_id;
	uint32_t crtc_id;
	drmModeCrtc *saved_crtc;

	bool pflip_pending;
	bool cleanup;

	int bar_xpos;

	unsigned num_frames_drawn;
	struct timespec flip_time;
	struct timespec draw_start_time;
	uint64_t draw_total_time;

	uint64_t min_flip_time, max_flip_time;
};

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_prepare(int fd);
static void modeset_draw(int fd);
static void modeset_cleanup(int fd);

const int bar_width = 40;

static struct modeset_dev *modeset_list = NULL;

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_dev *dev;
	int r;
	uint32_t output_id = 0;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn_id = conn->connector_id;
		dev->output_id = output_id++;

		/* call helper function to prepare this connector */
		r = modeset_setup_dev(fd, res, conn, dev);
		if (r) {
			if (r != -ENOENT) {
				errno = -r;
				fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	int r;
	uint32_t width, height;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	width = conn->modes[0].hdisplay;
	height = conn->modes[0].vdisplay;

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));

	/* find a crtc for this connector */
	r = modeset_find_crtc(fd, res, conn, dev);
	if (r) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return r;
	}

	/* create framebuffer #1 for this CRTC */
	r = drm_create_dumb_fb(fd, width, height, &dev->bufs[0]);
	if (r) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		return r;
	}

	/* create framebuffer #2 for this CRTC */
	r = drm_create_dumb_fb(fd, width, height, &dev->bufs[1]);
	if (r) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		drm_destroy_dumb_fb(&dev->bufs[0]);
		return r;
	}

	return 0;
}


static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc_id;
	struct modeset_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc_id = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					crtc_id = -1;
					break;
				}
			}

			if (crtc_id >= 0) {
				dev->enc_id = enc->encoder_id;
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc_id;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc_id = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					crtc_id = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc_id >= 0) {
				dev->enc_id = enc->encoder_id;
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc_id;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}


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
	r = modeset_prepare(fd);
	if (r)
		goto out_close;

	/* perform actual modesetting on each found connector+CRTC */
	for (struct modeset_dev *dev = modeset_list; dev; dev = dev->next) {
		fprintf(stderr, "Output %u: Connector %u, Encoder %u, CRTC %u, FB %u/%u, Mode %ux%u\n",
			dev->output_id,
			dev->conn_id, dev->enc_id, dev->crtc_id,
			dev->bufs[0].fb_id, dev->bufs[1].fb_id,
			dev->mode.hdisplay, dev->mode.vdisplay);

		dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id);
		buf = &dev->bufs[dev->front_buf];
		r = drmModeSetCrtc(fd, dev->crtc_id, buf->fb_id, 0, 0,
				     &dev->conn_id, 1, &dev->mode);
		if (r)
			fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
				dev->conn_id, errno);
	}

	/* draw some colors for 5seconds */
	modeset_draw(fd);

	/* cleanup everything */
	modeset_cleanup(fd);

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

static void flip(int fd, struct modeset_dev *dev)
{
	struct framebuffer *buf;
	int r;

	/* back buffer */
	buf = &dev->bufs[dev->front_buf ^ 1];

	r = drmModePageFlip(fd, dev->crtc_id, buf->fb_id, DRM_MODE_PAGE_FLIP_EVENT, dev);
	if (r) {
		fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
			dev->conn_id, errno);
		return;
	}

	dev->front_buf ^= 1;
	dev->pflip_pending = true;
}

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modeset_dev *dev = data;
	struct timespec now;

	dev->pflip_pending = false;

	if (dev->cleanup)
		return;

	get_time_now(&now);

	/* initialize values on first flip */
	if (dev->num_frames_drawn == 0) {
		dev->min_flip_time = UINT64_MAX;
		dev->max_flip_time = 0;
		dev->draw_start_time = now;
		dev->flip_time = now;
	}

	/* measure min/max flip time */
	if (dev->num_frames_drawn > 0) {
		uint64_t us;

		us = get_time_elapsed_us(&dev->flip_time, &now);

		dev->flip_time = now;

		if (us < dev->min_flip_time)
			dev->min_flip_time = us;

		if (us > dev->max_flip_time)
			dev->max_flip_time = us;
	}

	const int measure_interval = 100;

	if (dev->num_frames_drawn > 0 &&
		dev->num_frames_drawn % measure_interval == 0) {
		uint64_t us;
		float flip_avg, draw_avg;

		us = get_time_elapsed_us(&dev->draw_start_time, &now);
		flip_avg = (float)us / measure_interval / 1000;

		draw_avg = (float)dev->draw_total_time / measure_interval / 1000;

		printf("Output %u: draw %f ms, flip avg/min/max %f/%f/%f\n",
			dev->output_id,
			draw_avg,
			flip_avg,
			dev->min_flip_time / 1000.0,
			dev->max_flip_time / 1000.0);

		dev->draw_start_time = now;
		dev->draw_total_time = 0;

		dev->min_flip_time = UINT64_MAX;
		dev->max_flip_time = 0;
	}

	/* draw */
	{
		/* back buffer */
		struct framebuffer *buf = &dev->bufs[dev->front_buf ^ 1];

		struct timespec ts1, ts2;

		get_time_now(&ts1);

		dev->bar_xpos = (dev->bar_xpos + 8) % (buf->width - bar_width);

		drm_draw_color_bar(buf, dev->bar_xpos, bar_width);

		get_time_now(&ts2);

		dev->draw_total_time += get_time_elapsed_us(&ts1, &ts2);
	}

	dev->num_frames_drawn += 1;

	flip(fd, dev);
}

static void modeset_draw(int fd)
{
	drmEventContext ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = modeset_page_flip_event,
	};

	/* start the page flips */
	for (struct modeset_dev *dev = modeset_list; dev; dev = dev->next)
		flip(fd, dev);

	fd_set fds;

	FD_ZERO(&fds);

	while (true) {
		int r;

		FD_SET(0, &fds);
		FD_SET(fd, &fds);

		r = select(fd + 1, &fds, NULL, NULL, NULL);
		if (r < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(fd, &fds)) {
			drmHandleEvent(fd, &ev);
		}
	}
}

static void modeset_cleanup(int fd)
{
	struct modeset_dev *iter;
	drmEventContext ev;
	int r;

	/* init variables */
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = modeset_page_flip_event;

	while (modeset_list) {
		/* remove from global list */
		iter = modeset_list;
		modeset_list = iter->next;

		/* if a pageflip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			r = drmHandleEvent(fd, &ev);
			if (r)
				break;
		}

		/* restore saved CRTC configuration */
		if (!iter->pflip_pending)
			drmModeSetCrtc(fd,
				       iter->saved_crtc->crtc_id,
				       iter->saved_crtc->buffer_id,
				       iter->saved_crtc->x,
				       iter->saved_crtc->y,
				       &iter->conn_id,
				       1,
				       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* destroy framebuffers */
		drm_destroy_dumb_fb(&iter->bufs[1]);
		drm_destroy_dumb_fb(&iter->bufs[0]);

		/* free allocated memory */
		free(iter);
	}
}
