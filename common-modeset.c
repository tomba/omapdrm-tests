#include "common-modeset.h"

int modeset_prepare(int fd, int num_buffers,
           struct modeset_dev **dev_list)
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
		dev->num_buffers = num_buffers;

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

	return 0;
}

int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev, struct modeset_dev *dev_list)
{
	int r;
	uint32_t width, height;
	unsigned int i, j;

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
	r = modeset_find_crtc(fd, res, conn, dev, dev_list);
	if (r) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return r;
	}

	/* create framebuffers for this CRTC */
	dev->bufs = malloc(dev->num_buffers*sizeof(*dev->bufs));
	if(!dev->bufs) {
		fprintf(stderr, "Unable to allocate framebuffer memory for connector %u\n",
		conn->connector_id);
		return -ENOMEM;
	}

	for(i = 0 ; i < dev->num_buffers; i++) {
		r = drm_create_dumb_fb(fd, width, height, &dev->bufs[i]);
		if (r) {
			fprintf(stderr, "cannot create framebuffer for connector %u\n",
				conn->connector_id);
			for(j = 0; j < i; j++) drm_destroy_dumb_fb(&dev->bufs[j]);
			free(dev->bufs);
			return r;
		}
	}

	return 0;
}

int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
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

void modeset_draw(int fd, drmEventContext *ev,
            struct modeset_dev *dev_list)
{

	/* start the page flips */
	for (struct modeset_dev *dev = dev_list; dev; dev = dev->next)
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
	if (r) {
		fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
			dev->conn_id, errno);
		return;
	}

	dev->front_buf = (dev->front_buf + 1) % dev->num_buffers;
	dev->pflip_pending = true;
}
