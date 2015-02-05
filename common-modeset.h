#ifndef _COMMON_MODESET_H_
#define _COMMON_MODESET_H_

#include "common-drm.h"

struct modeset_dev {
	struct modeset_dev *next;

	int fd;

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

void modeset_prepare(int fd, struct modeset_dev **dev_list);
void modeset_alloc_fbs(struct modeset_dev *list, int num_buffers);
void modeset_set_modes(struct modeset_dev *list);
void modeset_draw(int fd, drmEventContext *ev,
			     struct modeset_dev *dev_list);
void modeset_cleanup(int fd, drmEventContext *ev,
			     struct modeset_dev *dev_list);
void flip(int fd, struct modeset_dev *dev);

#endif

