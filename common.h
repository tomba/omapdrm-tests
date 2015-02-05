#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>
#include <time.h>

/* common.c */
void get_time_now(struct timespec *ts);
uint64_t get_time_elapsed_us(const struct timespec *ts_start, const struct timespec *ts_end);

#endif
