#include "common-modeset.h"
#include "common.h"

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev, struct modeset_dev *dev_list)
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
			for (iter = dev_list; iter; iter = iter->next) {
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
		ASSERT(enc);

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc_id = res->crtcs[j];
			for (iter = dev_list; iter; iter = iter->next) {
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

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev, struct modeset_dev *dev_list)
{
	int r;

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

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));

	/* find a crtc for this connector */
	r = modeset_find_crtc(fd, res, conn, dev, dev_list);
	if (r) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return r;
	}

	return 0;
}

void modeset_prepare(int fd, struct modeset_dev **dev_list)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_dev *dev;
	struct modeset_dev *d_list=NULL;
	int r;
	uint32_t output_id = 0;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	ASSERT(res);

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		ASSERT(conn);

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->fd = fd;
		dev->conn_id = conn->connector_id;
		dev->output_id = output_id++;

		/* call helper function to prepare this connector */
		r = modeset_setup_dev(fd, res, conn, dev, d_list);
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

		/* free connector data and link device into device list */
		drmModeFreeConnector(conn);
		dev->next = d_list;
		d_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);

	*dev_list = d_list;
}

void modeset_alloc_fbs(struct modeset_dev *list, int num_buffers)
{
	for_each_dev(dev, list) {
		struct framebuffer *bufs;
		int i;

		bufs = malloc(num_buffers * sizeof(*bufs));
		ASSERT(bufs);

		for(i = 0 ; i < num_buffers; i++)
			drm_create_dumb_fb(dev->fd,
				dev->mode.hdisplay, dev->mode.vdisplay, &bufs[i]);

		dev->bufs = bufs;
		dev->num_buffers = num_buffers;
	}
}

void modeset_set_modes(struct modeset_dev *list)
{
	for_each_dev(dev, list) {
		struct framebuffer *buf;
		int r;

		fprintf(stderr, "Output %u: Connector %u, Encoder %u, CRTC %u, FB %u/%u, Mode %ux%u\n",
			dev->output_id,
			dev->conn_id, dev->enc_id, dev->crtc_id,
			dev->bufs[0].fb_id, dev->bufs[1].fb_id,
			dev->mode.hdisplay, dev->mode.vdisplay);

		dev->saved_crtc = drmModeGetCrtc(dev->fd, dev->crtc_id);
		buf = &dev->bufs[0];

		r = drmModeSetCrtc(dev->fd, dev->crtc_id, buf->fb_id, 0, 0,
				     &dev->conn_id, 1, &dev->mode);
		ASSERT(r == 0);
	}
}

void modeset_draw(int fd, drmEventContext *ev,
            struct modeset_dev *dev_list)
{
	/* start the page flips */
	for_each_dev(dev, dev_list)
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
			drmHandleEvent(fd, ev);
		}
	}
}

void modeset_cleanup(int fd, drmEventContext *ev,
            struct modeset_dev *dev_list)
{
	struct modeset_dev *iter;
	int r;
	unsigned int i;

	while (dev_list) {
		/* remove from global list */
		iter = dev_list;
		dev_list = iter->next;

		/* if a pageflip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			r = drmHandleEvent(fd, ev);
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
		for(i = 0; i < iter->num_buffers; i++) {
			drm_destroy_dumb_fb(&iter->bufs[i]);
		}

		/* free allocated memory */
		free(iter->bufs);
		free(iter);
	}
}

void flip(int fd, struct modeset_dev *dev)
{
	struct framebuffer *buf;
	int r;

	/* back buffer */
	buf = &dev->bufs[(dev->front_buf + 1) % dev->num_buffers];

	r = drmModePageFlip(fd, dev->crtc_id, buf->fb_id, DRM_MODE_PAGE_FLIP_EVENT, dev);
	ASSERT(r == 0);

	dev->front_buf = (dev->front_buf + 1) % dev->num_buffers;
	dev->pflip_pending = true;
}
