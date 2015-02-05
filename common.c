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
	clock_gettime(CLOCK_MONOTONIC, ts);
}

uint64_t get_time_elapsed_us(const struct timespec *ts_start, const struct timespec *ts_end)
{
	struct timespec res;
	uint64_t usecs;

	res = timespec_diff(ts_start, ts_end);
	usecs = res.tv_nsec / 1000 + ((uint64_t)res.tv_sec) * 1000 * 1000;

	return usecs;
}
