#ifndef _COMMON_MODESET_H_
#define _COMMON_MODESET_H_

#include "common-drm.h"

struct modeset_out {
	struct modeset_out *next;

	int fd;

	unsigned int front_buf;
	int num_buffers;
	struct framebuffer *bufs;

	uint32_t output_id;

	drmModeModeInfo mode;
	uint32_t conn_id;
	uint32_t enc_id;
	uint32_t crtc_id;
	uint32_t crtc_idx;
	drmModeCrtc *saved_crtc;

	bool cleanup;

	void *data;

	int pflip_pending;
	void (*flip_event)(void *);

	int dpms;

};

void modeset_prepare(int fd, struct modeset_out **out_list);
void modeset_alloc_fbs(struct modeset_out *list, int num_buffers);
void modeset_set_modes(struct modeset_out *list);
void modeset_start_flip(struct modeset_out *out);
void modeset_main_loop(struct modeset_out *modeset_list, void (*flip_event)(void *));
void modeset_cleanup(struct modeset_out *out_list);

static inline struct modeset_out *find_output(struct modeset_out *list, int output_id)
{
	for_each_output(out, list)
		if (out->output_id == output_id)
			return out;
	return NULL;
}

#endif
