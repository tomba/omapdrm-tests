
#include <omap_drmif.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include "test.h"
#include "omap-prod-con.h"

static struct {
	int drm_fd;
	int sfd;
	struct omap_device *omap_dev;
	volatile struct shared_data *sdata;
} global;

struct received_fb {
	TAILQ_ENTRY(received_fb) entries;
	struct framebuffer *fb;
};

struct flip_data {
	unsigned num_frames_drawn;
	struct timespec flip_time;
	struct timespec draw_start_time;

	uint64_t min_flip_time, max_flip_time;

	TAILQ_HEAD(tailhead, received_fb) fb_list_head;
	struct framebuffer *current_fb, *queued_fb;
};

static struct modeset_dev *modeset_list = NULL;

static int count_queued_fbs(struct flip_data *priv)
{
	int count = 0;
	struct received_fb *rfb;

	TAILQ_FOREACH(rfb, &priv->fb_list_head, entries)
		count++;

	return count;
}

static void update_queue_counts()
{
	volatile struct shared_data *sdata = global.sdata;

	int count = 0;

	for_each_dev(dev, modeset_list) {
		volatile struct shared_output *out = &sdata->outputs[count++];

		int c = count_queued_fbs(dev->data);
		out->request_count = c >= 10 ? 0 : 10 - c;
	}

	//printf("C %d, %d\n",
	//	sdata->outputs[0].request_count,
	//	sdata->outputs[1].request_count);

	msync((void *)global.sdata, sizeof(struct shared_data), MS_SYNC);
}

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modeset_dev *dev = data;
	struct timespec now;
	struct flip_data *priv = dev->data;

	//printf("FLIP %d\n", dev->output_id);

	if (priv->current_fb) {
		struct framebuffer *fb = priv->current_fb;
		int r;

		//printf("DELETE %d\n", fb->fb_id);

		r = drmModeRmFB(global.drm_fd, fb->fb_id);
		ASSERT(r == 0);

		omap_bo_del(fb->omap_bo);

		free(priv->current_fb);
	}

	priv->current_fb = priv->queued_fb;
	priv->queued_fb = NULL;

	dev->pflip_pending = false;

	if (dev->cleanup)
		return;

	get_time_now(&now);

	/* initialize values on first flip */
	if (priv->num_frames_drawn == 0) {
		priv->min_flip_time = UINT64_MAX;
		priv->max_flip_time = 0;
		priv->draw_start_time = now;
		priv->flip_time = now;
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
		float flip_avg;

		us = get_time_elapsed_us(&priv->draw_start_time, &now);
		flip_avg = (float)us / measure_interval / 1000;

		printf("Output %u: flip avg/min/max %f/%f/%f\n",
			dev->output_id,
			flip_avg,
			priv->min_flip_time / 1000.0,
			priv->max_flip_time / 1000.0);

		priv->draw_start_time = now;

		priv->min_flip_time = UINT64_MAX;
		priv->max_flip_time = 0;
	}

	priv->num_frames_drawn += 1;

	if (TAILQ_EMPTY(&priv->fb_list_head))
		return;

	struct received_fb *rfb;
	struct framebuffer *fb;
	int r;

	//printf("flip: queue new pflig: %d\n", dev->output_id);

	rfb = TAILQ_FIRST(&priv->fb_list_head);
	TAILQ_REMOVE(&priv->fb_list_head, priv->fb_list_head.tqh_first, entries);

	fb = rfb->fb;
	free(rfb);

	r = drmModePageFlip(dev->fd, dev->crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, dev);
	ASSERT(r == 0);
	dev->pflip_pending = true;
	priv->queued_fb = fb;

	update_queue_counts();
}

static void init_drm()
{
	const char *card = "/dev/dri/card0";

	global.drm_fd = drm_open_dev_dumb(card);

	global.omap_dev = omap_device_new(global.drm_fd);
	ASSERT(global.omap_dev);
}

static void uninit_drm()
{
	omap_device_del(global.omap_dev);
	close(global.drm_fd);
}

static void receive_fb(int sfd, int *output_id, struct framebuffer *fb)
{
	char buf[16];
	int prime_fd;
	int r;

	size_t size = sock_fd_read(sfd, buf, sizeof(buf), &prime_fd);
	ASSERT(size == 1);

	*output_id = buf[0];

	struct modeset_dev *dev = find_dev(modeset_list, *output_id);
	ASSERT(dev);

	int w = dev->mode.hdisplay;
	int h = dev->mode.vdisplay;

	ASSERT(w != 0 && h != 0);

	r = drmPrimeFDToHandle(global.drm_fd, prime_fd, &fb->handle);
	ASSERT(r == 0);

	fb->omap_bo = omap_bo_from_dmabuf(global.omap_dev, prime_fd);
	ASSERT(fb->omap_bo);

	fb->size = omap_bo_size(fb->omap_bo);
	fb->width = w;
	fb->height = h;
	fb->stride = fb->width * 32 / 8;

	r = drmModeAddFB(global.drm_fd, fb->width, fb->height, 24, 32, fb->stride,
		   fb->handle, &fb->fb_id);
	ASSERT(r == 0);

	//printf("received fb handle %x, prime %d, fb %d\n", fb->handle, prime_fd, fb->fb_id);

	r = close(prime_fd);
	ASSERT(r == 0);
}

static void main_loop(int sfd)
{
	printf("reading...\n");

	drmEventContext ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = modeset_page_flip_event,
	};

	update_queue_counts();

	fd_set fds;

	FD_ZERO(&fds);

	int drm_fd = global.drm_fd;

	int count = 0;

	while (true) {
		int r;

		FD_SET(0, &fds);
		FD_SET(drm_fd, &fds);
		FD_SET(sfd, &fds);

		int max_fd = sfd > drm_fd ? sfd : drm_fd;

		r = select(max_fd + 1, &fds, NULL, NULL, NULL);

		ASSERT(r >= 0);

		if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			return;
		}

		if (FD_ISSET(drm_fd, &fds)) {
			//printf("drm event\n");
			drmHandleEvent(drm_fd, &ev);
		}

		if (FD_ISSET(sfd, &fds)) {
			struct framebuffer *fb = malloc(sizeof(*fb));
			int output_id;

			receive_fb(sfd, &output_id, fb);

			//printf("received fb %d, for output %d, handle %x\n", count, output_id, fb->handle);
			count++;

			struct modeset_dev *dev = find_dev(modeset_list, output_id);
			struct flip_data *priv = dev->data;
			ASSERT(dev);

			if (priv->queued_fb == NULL) {
				//printf("queue pflip %d\n", dev->output_id);
				r = drmModePageFlip(dev->fd, dev->crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, dev);
				ASSERT(r == 0);
				dev->pflip_pending = true;
				priv->queued_fb = fb;
			} else {
				struct received_fb *rfb = malloc(sizeof(*rfb));
				rfb->fb = fb;

				TAILQ_INSERT_TAIL(&priv->fb_list_head, rfb, entries);
			}

			update_queue_counts();
		}
	}

	printf("done\n");

	for_each_dev(dev, modeset_list) {
		dev->cleanup = true;

		if (!dev->pflip_pending)
			continue;

		/* if a pageflip is pending, wait for it to complete */
		fprintf(stderr,
			"wait for pending page-flip to complete for output %d...\n",
			dev->output_id);

		while (dev->pflip_pending) {
			int r;
			r = drmHandleEvent(drm_fd, &ev);
			ASSERT(r == 0);
		}
	}
}

static void setup_config()
{
	int count = 0;

	volatile struct shared_data *sdata = global.sdata;

	for_each_dev(dev, modeset_list) {
		volatile struct shared_output *out = &sdata->outputs[count++];

		out->output_id = dev->output_id;
		out->width = dev->mode.hdisplay;
		out->height = dev->mode.vdisplay;
		out->request_count = 0;
	}

	global.sdata->num_outputs = count;

	msync((void *)global.sdata, sizeof(struct shared_data), MS_SYNC);
}

static int connect_to_producer()
{
	struct sockaddr_un addr = { 0 };
	int sfd;

	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT(sfd != 0);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKNAME);

	int r = connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un));
	ASSERT(r == 0);

	return sfd;
}

static void open_shared_mem()
{
	int fd;

	fd = shm_open(SHARENAME, O_RDWR, S_IRUSR | S_IWUSR);
	ASSERT(fd > 0);

	struct shared_data *sdata = mmap(NULL, sizeof(struct shared_data), PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	ASSERT(sdata);

	global.sdata = sdata;
}

int main(int argc, char **argv)
{
	int r;

	init_drm();

	// Prepare all connectors and CRTCs
	modeset_prepare(global.drm_fd, &modeset_list);

	// Allocate root buffers
	modeset_alloc_fbs(modeset_list, 1);

	// Draw test pattern
	for_each_dev(dev, modeset_list)
		drm_draw_test_pattern(&dev->bufs[0], 0);

	// Allocate private data
	for_each_dev(dev, modeset_list) {
		struct flip_data *priv;
		priv = calloc(1, sizeof(struct flip_data));
		TAILQ_INIT(&priv->fb_list_head);
		dev->data = priv;
	}

	// Set modes
	modeset_set_modes(modeset_list);

	int sfd = connect_to_producer();

	global.sfd = sfd;

	open_shared_mem();

	setup_config();

	main_loop(sfd);

	// Free private data
	for_each_dev(dev, modeset_list)
		free(dev->data);

	r = close(sfd);
	ASSERT(r == 0);

	modeset_cleanup(modeset_list);

	uninit_drm();

	return 0;
}
