#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "libflow.h"
#include "utils.h"

static inline void move_to_inc_at_start(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_INC;
}

void init_flow(struct flow *fw, uint64_t total_size,
	long max_write_rate, int progress,
	flow_func_flush_chunk_t func_flush_chunk)
{
	fw->total_size		= total_size;
	fw->total_written	= 0;
	fw->progress		= progress;
	fw->block_size		= 512;	/* Bytes	*/
	fw->blocks_per_delay	= 1;	/* 512B/s	*/
	fw->delay_ms		= 1000;	/* 1s		*/
	fw->max_write_rate	= max_write_rate <= 0
		? DBL_MAX : max_write_rate * 1024.;
	fw->measured_blocks	= 0;
	fw->measured_time_ms	= 0;
	fw->erase		= 0;
	fw->func_flush_chunk	= func_flush_chunk;
	fw->written_blocks	= 0;
	fw->acc_delay_us	= 0;
	assert(fw->block_size > 0);
	assert(fw->block_size % SECTOR_SIZE == 0);

	move_to_inc_at_start(fw);
}

static inline void repeat_ch(char ch, int count)
{
	while (count > 0) {
		printf("%c", ch);
		count--;
	}
}

static void erase(int count)
{
	if (count <= 0)
		return;
	repeat_ch('\b',	count);
	repeat_ch(' ',	count);
	repeat_ch('\b',	count);
}

static int pr_time(double sec)
{
	int has_h, has_m;
	int c, tot;

	tot = printf(" -- ");
	assert(tot > 0);

	has_h = sec >= 3600;
	if (has_h) {
		double h = floor(sec / 3600);
		c = printf("%i:", (int)h);
		assert(c > 0);
		tot += c;
		sec -= h * 3600;
	}

	has_m = has_h || sec >= 60;
	if (has_m) {
		double m = floor(sec / 60);
		if (has_h)
			c = printf("%02i:", (int)m);
		else
			c = printf("%i:", (int)m);
		assert(c > 0);
		tot += c;
		sec -= m * 60;
	}

	if (has_m)
		c = printf("%02i", (int)round(sec));
	else
		c = printf("%is", (int)round(sec));
	assert(c > 0);
	return tot + c;
}

static void report_progress(struct flow *fw, double inst_speed)
{
	const char *unit = adjust_unit(&inst_speed);
	double percent;
	/* The following shouldn't be necessary, but sometimes
	 * the initial free space isn't exactly reported
	 * by the kernel; this issue has been seen on Macs.
	 */
	if (fw->total_size < fw->total_written)
		fw->total_size = fw->total_written;
	percent = (double)fw->total_written * 100 / fw->total_size;
	erase(fw->erase);
	fw->erase = printf("%.2f%% -- %.2f %s/s",
		percent, inst_speed, unit);
	assert(fw->erase > 0);
	if (has_enough_measurements(fw))
		fw->erase += pr_time(
			(fw->total_size - fw->total_written) /
			get_avg_speed(fw));
	fflush(stdout);
}

static inline void __start_measurement(struct flow *fw)
{
	assert(!gettimeofday(&fw->t1, NULL));
}

void start_measurement(struct flow *fw)
{
	/*
	 * The report below is especially useful when a single measurement spans
	 * multiple files; this happens when a drive is faster than 1GB/s.
	 */
	if (fw->progress)
		report_progress(fw, fw->blocks_per_delay * fw->block_size *
			1000.0 / fw->delay_ms);
	__start_measurement(fw);
}

static inline void move_to_steady(struct flow *fw)
{
	fw->state = FW_STEADY;
}

static void move_to_search(struct flow *fw, int64_t bpd1, int64_t bpd2)
{
	assert(bpd1 > 0);
	assert(bpd2 >= bpd1);

	fw->blocks_per_delay = (bpd1 + bpd2) / 2;
	if (bpd2 - bpd1 <= 3) {
		move_to_steady(fw);
		return;
	}

	fw->bpd1 = bpd1;
	fw->bpd2 = bpd2;
	fw->state = FW_SEARCH;
}

static inline void dec_step(struct flow *fw)
{
	if (fw->blocks_per_delay - fw->step > 0) {
		fw->blocks_per_delay -= fw->step;
		fw->step *= 2;
	} else
		move_to_search(fw, 1, fw->blocks_per_delay + fw->step / 2);
}

static inline void inc_step(struct flow *fw)
{
	fw->blocks_per_delay += fw->step;
	fw->step *= 2;
}

static inline void move_to_inc(struct flow *fw)
{
	move_to_inc_at_start(fw);
	inc_step(fw);
}

static inline void move_to_dec(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_DEC;
	dec_step(fw);
}

static inline int is_rate_above(const struct flow *fw,
	long delay, double inst_speed)
{
	/* We use logical or here to enforce the lowest limit. */
	return delay > fw->delay_ms || inst_speed > fw->max_write_rate;
}

static inline int is_rate_below(const struct flow *fw,
	long delay, double inst_speed)
{
	/* We use logical and here to enforce both limist. */
	return delay <= fw->delay_ms && inst_speed < fw->max_write_rate;
}

static inline int flush_chunk(const struct flow *fw, int fd)
{
	if (fw->func_flush_chunk)
		return fw->func_flush_chunk(fw, fd);
	return 0;
}

static void msleep(double wait_ms)
{
	struct timespec req;
	int ret;

	assert(!clock_gettime(CLOCK_MONOTONIC, &req));

	/* Add @wait_ms to @req. */
	if (wait_ms > 1000) {
		time_t sec = wait_ms / 1000;
		wait_ms -= sec * 1000;
		assert(wait_ms > 0);
		req.tv_sec += sec;
	}
	req.tv_nsec += wait_ms * 1000000;

	/* Round @req up. */
	if (req.tv_nsec >= 1000000000) {
		ldiv_t result = ldiv(req.tv_nsec, 1000000000);
		req.tv_sec += result.quot;
		req.tv_nsec = result.rem;
	}

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&req, NULL);
	} while (ret == EINTR);

	assert(ret == 0);
}

/* XXX Avoid duplicate this function, which was copied from libutils.h. */
static inline uint64_t diff_timeval_us(const struct timeval *t1,
	const struct timeval *t2)
{
	return (t2->tv_sec - t1->tv_sec) * 1000000ULL +
		t2->tv_usec - t1->tv_usec;
}

int measure(int fd, struct flow *fw, ssize_t written)
{
	ldiv_t result = ldiv(written, fw->block_size);
	struct timeval t2;
	int64_t delay;
	double bytes_k, inst_speed;

	assert(result.rem == 0);
	fw->written_blocks += result.quot;
	fw->total_written += written;

	if (fw->written_blocks < fw->blocks_per_delay)
		return 0;
	assert(fw->written_blocks == fw->blocks_per_delay);

	if (flush_chunk(fw, fd) < 0)
		return -1; /* Caller can read errno(3). */

	assert(!gettimeofday(&t2, NULL));
	delay = (diff_timeval_us(&fw->t1, &t2) + fw->acc_delay_us) / 1000;

	/* Instantaneous speed in bytes per second. */
	bytes_k = fw->blocks_per_delay * fw->block_size * 1000.0;
	inst_speed = bytes_k / delay;

	if (delay < fw->delay_ms && inst_speed > fw->max_write_rate) {
		/* Wait until inst_speed == fw->max_write_rate (if possible). */
		double wait_ms = round((bytes_k - delay * fw->max_write_rate)
			/ fw->max_write_rate);

		 if (wait_ms < 0) {
			/* Wait what is possible. */
			wait_ms = fw->delay_ms - delay;
		} else if (delay + wait_ms < fw->delay_ms) {
			/* wait_ms is not the largest possible value, so
			 * force the flow algorithm to keep increasing it.
			 * Otherwise, the delay to print progress may be
			 * too small.
			 */
			wait_ms++;
		}

		if (wait_ms > 0) {
			/* Slow down. */
			msleep(wait_ms);

			/* Adjust measurements. */
			delay += wait_ms;
			inst_speed = bytes_k / delay;
		}
	}

	/* Update mean. */
	fw->measured_blocks += fw->written_blocks;
	fw->measured_time_ms += delay;

	switch (fw->state) {
	case FW_INC:
		if (is_rate_above(fw, delay, inst_speed)) {
			move_to_search(fw,
				fw->blocks_per_delay - fw->step / 2,
				fw->blocks_per_delay);
		} else if (is_rate_below(fw, delay, inst_speed)) {
			inc_step(fw);
		} else
			move_to_steady(fw);
		break;

	case FW_DEC:
		if (is_rate_above(fw, delay, inst_speed)) {
			dec_step(fw);
		} else if (is_rate_below(fw, delay, inst_speed)) {
			move_to_search(fw, fw->blocks_per_delay,
				fw->blocks_per_delay + fw->step / 2);
		} else
			move_to_steady(fw);
		break;

	case FW_SEARCH:
		if (fw->bpd2 - fw->bpd1 <= 3) {
			move_to_steady(fw);
			break;
		}

		if (is_rate_above(fw, delay, inst_speed)) {
			fw->bpd2 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else if (is_rate_below(fw, delay, inst_speed)) {
			fw->bpd1 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else
			move_to_steady(fw);
		break;

	case FW_STEADY: {
		if (delay <= fw->delay_ms) {
			if (inst_speed < fw->max_write_rate) {
				move_to_inc(fw);
			} else if (inst_speed > fw->max_write_rate) {
				move_to_dec(fw);
			}
		} else if (fw->blocks_per_delay > 1) {
			move_to_dec(fw);
		}
		break;
	}

	default:
		assert(0);
	}

	if (fw->progress)
		report_progress(fw, inst_speed);

	/* Reset accumulators. */
	fw->written_blocks = 0;
	fw->acc_delay_us = 0;
	__start_measurement(fw);
	return 0;
}

int end_measurement(int fd, struct flow *fw)
{
	struct timeval t2;
	int saved_errno;
	int ret = 0;

	if (fw->written_blocks <= 0)
		goto out;

	if (flush_chunk(fw, fd) < 0) {
		saved_errno = errno;
		ret = -1;
		goto out;
	}

	/* Save time in between closing ongoing file and creating a new file. */
	assert(!gettimeofday(&t2, NULL));
	fw->acc_delay_us += diff_timeval_us(&fw->t1, &t2);

out:
	/* Erase progress information. */
	erase(fw->erase);
	fw->erase = 0;
	fflush(stdout);

	if (ret < 0) {
		/* Propagate errno(3) to caller. */
		errno = saved_errno;
	}
	return ret;
}
