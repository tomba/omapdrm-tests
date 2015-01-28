#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* common.c */
void get_time_now(struct timespec *ts);
uint64_t get_time_elapsed_us(const struct timespec *ts_start, const struct timespec *ts_end);

/* send fd to another process */
ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);
/* receive fd from another process */
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd);

#endif
