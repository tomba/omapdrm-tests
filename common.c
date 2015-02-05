#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

#include "common.h"

static struct timespec timespec_diff(const struct timespec *start,
		const struct timespec *end)
{
	struct timespec temp;
	if ((end->tv_nsec - start->tv_nsec) < 0) {
		temp.tv_sec = end->tv_sec - start->tv_sec - 1;
		temp.tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
	} else {
		temp.tv_sec = end->tv_sec - start->tv_sec;
		temp.tv_nsec = end->tv_nsec - start->tv_nsec;
	}
	return temp;
}

void get_time_now(struct timespec *ts)
{
	int r;

	r = clock_gettime(CLOCK_MONOTONIC, ts);
	ASSERT(r == 0);
}

uint64_t get_time_elapsed_us(const struct timespec *ts_start, const struct timespec *ts_end)
{
	struct timespec res;
	uint64_t usecs;

	res = timespec_diff(ts_start, ts_end);
	usecs = res.tv_nsec / 1000 + ((uint64_t)res.tv_sec) * 1000 * 1000;

	return usecs;
}

/* http://keithp.com/blogs/fd-passing/ */
ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd)
{
	struct msghdr   msg;
	struct iovec	iov;
	union {
		struct cmsghdr  cmsghdr;
		char		control[CMSG_SPACE(sizeof (int))];
	} cmsgu;
	struct cmsghdr  *cmsg;

	iov.iov_base = buf;
	iov.iov_len = buflen;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (fd != -1) {
		msg.msg_control = cmsgu.control;
		msg.msg_controllen = sizeof(cmsgu.control);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_len = CMSG_LEN(sizeof (int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;

		int *pfd = (int *) CMSG_DATA(cmsg);
		*pfd = fd;
	} else {
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
	}

	return sendmsg(sock, &msg, 0);
}

/* http://keithp.com/blogs/fd-passing/ */
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd)
{
	ssize_t	 size;

	if (fd) {
		struct msghdr   msg;
		struct iovec	iov;
		union {
			struct cmsghdr  cmsghdr;
			char		control[CMSG_SPACE(sizeof (int))];
		} cmsgu;
		struct cmsghdr  *cmsg;

		iov.iov_base = buf;
		iov.iov_len = bufsize;

		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsgu.control;
		msg.msg_controllen = sizeof(cmsgu.control);
		size = recvmsg (sock, &msg, 0);
		if (size < 0) {
			perror ("recvmsg");
			exit(1);
		}
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
			if (cmsg->cmsg_level != SOL_SOCKET) {
				fprintf (stderr, "invalid cmsg_level %d\n",
					 cmsg->cmsg_level);
				exit(1);
			}
			if (cmsg->cmsg_type != SCM_RIGHTS) {
				fprintf (stderr, "invalid cmsg_type %d\n",
					 cmsg->cmsg_type);
				exit(1);
			}

			int *pfd = (int *) CMSG_DATA(cmsg);
			*fd = *pfd;
		} else
			*fd = -1;
	} else {
		size = read (sock, buf, bufsize);
		if (size < 0) {
			perror("read");
			exit(1);
		}
	}
	return size;
}
