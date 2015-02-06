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
	void (*flip_event)(void *);

	int dpms;

};

void modeset_prepare(int fd, struct modeset_dev **dev_list);
void modeset_alloc_fbs(struct modeset_dev *list, int num_buffers);
void modeset_set_modes(struct modeset_dev *list);
void modeset_start_flip(struct modeset_dev *dev);
void modeset_main_loop(struct modeset_dev *modeset_list, void (*flip_event)(void *));
void modeset_cleanup(struct modeset_dev *dev_list);

static inline struct modeset_dev *find_dev(struct modeset_dev *list, int output_id)
{
	for_each_dev(dev, list)
		if (dev->output_id == output_id)
			return dev;
	return NULL;
}

#endif
