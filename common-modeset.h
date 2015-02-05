#ifndef _COMMON_MODESET_H_
#define _COMMON_MODESET_H_

#include "common-drm.h"

struct modeset_dev {
	struct modeset_dev *next;

	unsigned int front_buf;
	int num_buffers;
	struct framebuffer *bufs;

	uint32_t output_id;

	drmModeModeInfo mode;
	uint32_t conn_id;
	uint32_t enc_id;
	uint32_t crtc_id;
	drmModeCrtc *saved_crtc;

	bool cleanup;

	void *data;

	int pflip_pending;

	int dpms;

};

int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev, struct modeset_dev *dev_list);
int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev, struct modeset_dev *dev_list);
int modeset_prepare(int fd, int num_buffers,
			     struct modeset_dev **dev_list);
void modeset_draw(int fd, drmEventContext *ev,
			     struct modeset_dev *dev_list);
void modeset_cleanup(int fd, drmEventContext *ev,
			     struct modeset_dev *dev_list);
void flip(int fd, struct modeset_dev *dev);

#endif

