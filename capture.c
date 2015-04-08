
#include <drm/drm_fourcc.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "test.h"

#define FOURCC_STR(str)    v4l2_fourcc(str[0], str[1], str[2], str[3])

#define BUF_QUEUE_SIZE 5

struct cam_vid_pipe {
	uint32_t input_width, input_height;
	uint32_t output_x, output_y;
	uint32_t output_width, output_height;

	int cap_fd;

	uint32_t plane_id;

	struct framebuffer bufs[BUF_QUEUE_SIZE];
	int prime_fds[BUF_QUEUE_SIZE];

	uint32_t b1, b2, b3;
};

static struct {
	int drm_fd;

	uint32_t crtc_id;
	uint32_t crtc_width;
	uint32_t crtc_height;

	struct cam_vid_pipe pipes[2];
} global;

static void init_drm()
{
	const char *card = "/dev/dri/card0";

	global.drm_fd = drm_open_dev_dumb(card);
}

static void uninit_drm()
{
	close(global.drm_fd);
}

static void find_crtc(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	ASSERT(res);

	for (int i = 0; i < res->count_crtcs; ++i) {
		uint32_t crtc_id = res->crtcs[i];

		drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
		ASSERT(crtc);

		if (!crtc->mode_valid) {
			drmModeFreeCrtc(crtc);
			continue;
		}

		global.crtc_id = crtc->crtc_id;
		global.crtc_width = crtc->width;
		global.crtc_height = crtc->height;

		drmModeFreeCrtc(crtc);
		drmModeFreeResources(res);
		return;
	}

	ASSERT(true);
}

static void create_bufs(struct cam_vid_pipe *pipe, int width, int height)
{
	for (int n = 0; n < BUF_QUEUE_SIZE; ++n) {
		struct framebuffer *fb = &pipe->bufs[n];

		drm_create_dumb_fb2(global.drm_fd, width, height,
			DRM_FORMAT_YUYV, fb);

		int r = drmPrimeHandleToFD(global.drm_fd, fb->planes[0].handle,
			DRM_CLOEXEC, &pipe->prime_fds[n]);
		ASSERT(r == 0);

		drm_draw_test_pattern(fb, 0);
	}
}

static void destroy_bufs(struct cam_vid_pipe *pipe)
{
	for (int n = 0; n < BUF_QUEUE_SIZE; ++n) {
		struct framebuffer *fb = &pipe->bufs[n];

		close(pipe->prime_fds[n]);

		drm_destroy_dumb_fb(fb);
	}
}

static void v4l2_init_capture(struct cam_vid_pipe *pipe, const char *viddev)
{
	int r;

	int fd = open(viddev, O_RDWR | O_NONBLOCK);
	ASSERT(fd >= 0);

	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	};

	r = ioctl(fd, VIDIOC_G_FMT, &fmt);
	ASSERT(r == 0);

	fmt.fmt.pix.pixelformat = FOURCC_STR("YUYV");
	fmt.fmt.pix.width = pipe->input_width;
	fmt.fmt.pix.height = pipe->input_height;

	r = ioctl(fd, VIDIOC_S_FMT, &fmt);
	ASSERT(r == 0);

	pipe->cap_fd = fd;

	struct v4l2_requestbuffers reqbuf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_DMABUF,
		.count = BUF_QUEUE_SIZE,
	};

	r = ioctl(pipe->cap_fd, VIDIOC_REQBUFS, &reqbuf);
	ASSERT(r == 0);
}

static void v4l2_queue_buffer(struct cam_vid_pipe *pipe, int index)
{
	int r;

	int dmafd = pipe->prime_fds[index];

	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_DMABUF,
		.index = index,
		.m.fd = dmafd,
	};

	r = ioctl(pipe->cap_fd, VIDIOC_QBUF, &buf);
	ASSERT(r == 0);
}

static int v4l2_dequeue_buffer(struct cam_vid_pipe *pipe)
{
	int r;

	struct v4l2_buffer v4l2buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_DMABUF,
	};

	r = ioctl(pipe->cap_fd, VIDIOC_DQBUF, &v4l2buf);
	ASSERT(r == 0 || errno == EAGAIN);

	if (r != 0 && errno == EAGAIN)
		return -1;

	int index = v4l2buf.index;

	return index;
}

static void v4l2_stream_on(struct cam_vid_pipe *pipe)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	int r = ioctl(pipe->cap_fd, VIDIOC_STREAMON, &type);
	ASSERT(r == 0);
}

static void init_camera_video_pipe(struct cam_vid_pipe *pipe, const char *viddev,
	uint32_t iw, uint32_t ih, uint32_t ox, uint32_t oy, uint32_t ow, uint32_t oh)
{
	pipe->input_width = iw;
	pipe->input_height = ih;

	pipe->output_x = ox;
	pipe->output_y = oy;
	pipe->output_width = ow;
	pipe->output_height = oh;

	v4l2_init_capture(pipe, viddev);

	create_bufs(pipe, pipe->input_width, pipe->input_height);

	pipe->plane_id = drm_reserve_plane(global.drm_fd);

	for (int i = 0; i < BUF_QUEUE_SIZE; ++i)
		v4l2_queue_buffer(pipe, i);

	v4l2_stream_on(pipe);
}

static void free_camera_video_pipe(struct cam_vid_pipe *pipe)
{
	destroy_bufs(pipe);

	drm_release_plane(pipe->plane_id);
}

static void process_pipe_init(struct cam_vid_pipe *pipe)
{
	pipe->b1 = pipe->b2 = pipe->b3 = -1;
}

static void process_pipe(struct cam_vid_pipe *pipe)
{
	if (pipe->b3 != -1) {
		v4l2_queue_buffer(pipe, pipe->b3);
		pipe->b3 = -1;
	}

	pipe->b1 = v4l2_dequeue_buffer(pipe);

	if (pipe->b1 == -1)
		return;

	struct framebuffer *buf = &pipe->bufs[pipe->b1];

	int r;

	r = drmModeSetPlane(global.drm_fd, pipe->plane_id, global.crtc_id,
		buf->fb_id, 0,
		// output
		pipe->output_x, pipe->output_y,
		pipe->output_width, pipe->output_height,
		//input
		0 << 16, 0 << 16, buf->width << 16, buf->height << 16);

	ASSERT(r == 0);

	pipe->b3 = pipe->b2;
	pipe->b2 = pipe->b1;
}

int main(int argc, char **argv)
{
	init_drm();
	find_crtc(global.drm_fd);

	init_camera_video_pipe(&global.pipes[0], "/dev/video0",
		800, 600, 0, 0, global.crtc_width, global.crtc_height);
	init_camera_video_pipe(&global.pipes[1], "/dev/video1",
		800, 600, 25, 25, global.crtc_width / 3, global.crtc_height / 3);

	process_pipe_init(&global.pipes[0]);
	process_pipe_init(&global.pipes[1]);

	struct pollfd fds[] = {
		{ .fd = 0, .events =  POLLIN },
		{ .fd = global.pipes[0].cap_fd, .events =  POLLIN },
		{ .fd = global.pipes[1].cap_fd, .events =  POLLIN },
	};

	while (true) {
		int r = poll(fds, 3, -1);
		ASSERT(r > 0);

		if (fds[0].revents != 0)
			break;

		process_pipe(&global.pipes[0]);
		process_pipe(&global.pipes[1]);
	}

	free_camera_video_pipe(&global.pipes[1]);
	free_camera_video_pipe(&global.pipes[0]);

	uninit_drm();

	fprintf(stderr, "exiting\n");

	return 0;
}
