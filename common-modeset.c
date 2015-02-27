#include "common-modeset.h"
#include "common.h"

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_out *out, struct modeset_out *out_list)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc_id;
	struct modeset_out *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc_id = enc->crtc_id;
			for (iter = out_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					crtc_id = -1;
					break;
				}
			}

			if (crtc_id >= 0) {
				out->enc_id = enc->encoder_id;
				drmModeFreeEncoder(enc);
				out->crtc_id = crtc_id;
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

			/* check that no other output already uses this CRTC */
			crtc_id = res->crtcs[j];
			for (iter = out_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					crtc_id = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc_id >= 0) {
				out->enc_id = enc->encoder_id;
				drmModeFreeEncoder(enc);
				out->crtc_id = crtc_id;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

static int modeset_setup_output(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_out *out, struct modeset_out *out_list)
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

	/* copy the mode information into our output structure */
	memcpy(&out->mode, &conn->modes[0], sizeof(out->mode));

	/* find a crtc for this connector */
	r = modeset_find_crtc(fd, res, conn, out, out_list);
	if (r) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return r;
	}

	return 0;
}

void modeset_prepare(int fd, struct modeset_out **out_list)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_out *out;
	struct modeset_out *o_list=NULL;
	int r;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	ASSERT(res);

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		ASSERT(conn);

		/* create a output structure */
		out = malloc(sizeof(*out));
		memset(out, 0, sizeof(*out));
		out->fd = fd;
		out->conn_id = conn->connector_id;
		out->output_id = i;

		/* call helper function to prepare this output */
		r = modeset_setup_output(fd, res, conn, out, o_list);
		if (r) {
			if (r != -ENOENT) {
				errno = -r;
				fprintf(stderr, "cannot setup output %d for connector %u (%d): %m\n",
					i, conn->connector_id, errno);
			}
			free(out);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link output into output list */
		drmModeFreeConnector(conn);

		if (o_list == NULL) {
			o_list = out;
		}
		else {
			struct modeset_out *o = o_list;
			while (o->next != NULL)
				o = o->next;
			o->next = out;
		}
	}

	/* free resources again */
	drmModeFreeResources(res);

	*out_list = o_list;
}

void modeset_alloc_fbs(struct modeset_out *list, int num_buffers)
{
	for_each_output(out, list) {
		struct framebuffer *bufs;
		int i;

		bufs = malloc(num_buffers * sizeof(*bufs));
		ASSERT(bufs);

		for(i = 0 ; i < num_buffers; i++)
			drm_create_dumb_fb(out->fd,
				out->mode.hdisplay, out->mode.vdisplay, &bufs[i]);

		out->bufs = bufs;
		out->num_buffers = num_buffers;
	}
}

void modeset_set_modes(struct modeset_out *list)
{
	for_each_output(out, list) {
		struct framebuffer *buf;
		int r;

		fprintf(stderr, "Output %u: Connector %u, Encoder %u, CRTC %u, FB %u/%u, Mode %ux%u\n",
			out->output_id,
			out->conn_id, out->enc_id, out->crtc_id,
			out->bufs[0].fb_id, out->bufs[1].fb_id,
			out->mode.hdisplay, out->mode.vdisplay);

		out->saved_crtc = drmModeGetCrtc(out->fd, out->crtc_id);
		buf = &out->bufs[0];

		r = drmModeSetCrtc(out->fd, out->crtc_id, buf->fb_id, 0, 0,
				     &out->conn_id, 1, &out->mode);
		ASSERT(r == 0);
	}
}

void modeset_start_flip(struct modeset_out *out)
{
	struct framebuffer *buf;
	int r;

	/* back buffer */
	buf = &out->bufs[(out->front_buf + 1) % out->num_buffers];

	r = drmModePageFlip(out->fd, out->crtc_id, buf->fb_id, DRM_MODE_PAGE_FLIP_EVENT, out);
	ASSERT(r == 0);

	out->front_buf = (out->front_buf + 1) % out->num_buffers;
	out->pflip_pending = true;
}

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modeset_out *out = data;

	out->pflip_pending = false;

	if (out->cleanup)
		return;

	if (out->flip_event)
		out->flip_event(data);
}

void modeset_main_loop(struct modeset_out *modeset_list, void (*flip_event)(void *))
{
	drmEventContext ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = modeset_page_flip_event,
	};

	/* start the page flips */
	for_each_output(out, modeset_list) {
		out->flip_event = flip_event;
		modeset_start_flip(out);
	}

	int fd = modeset_list->fd;

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

	for_each_output(out, modeset_list) {
		out->cleanup = true;

		if (!out->pflip_pending)
			continue;

		/* if a pageflip is pending, wait for it to complete */
		fprintf(stderr,
			"wait for pending page-flip to complete for output %d...\n",
			out->output_id);

		while (out->pflip_pending) {
			int r;
			r = drmHandleEvent(fd, &ev);
			ASSERT(r == 0);
		}
	}
}

void modeset_cleanup(struct modeset_out *out_list)
{
	struct modeset_out *iter;
	unsigned int i;

	while (out_list) {
		/* remove from global list */
		iter = out_list;
		out_list = iter->next;

		/* restore saved CRTC configuration */
		if (!iter->pflip_pending)
			drmModeSetCrtc(iter->fd,
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
