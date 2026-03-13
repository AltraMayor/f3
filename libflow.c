#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <string.h>

#include "libflow.h"
#include "libutils.h"

/* Apple Macintosh / OpenBSD */
#if (__APPLE__ && __MACH__) || defined(__OpenBSD__)

#include <unistd.h>
static inline void ussleep(double wait_us)
{
	assert(!usleep(wait_us));
}

#else	/* Everyone else */

#include <time.h> /* For clock_gettime() and clock_nanosleep(). */
static void ussleep(double wait_us)
{
	struct timespec req;
	int ret;

	assert(!clock_gettime(CLOCK_MONOTONIC, &req));

	/* Add @wait_us to @req. */
	if (wait_us > 1000000) {
		time_t sec = wait_us / 1000000;
		wait_us -= sec * 1000000;
		assert(wait_us > 0);
		req.tv_sec += sec;
	}
	req.tv_nsec += wait_us * 1000;

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

#endif	/* ussleep() */

static inline void move_to_inc_at_start(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_INC;
}

void init_flow(struct flow *fw, int block_size, uint64_t total_size,
	long max_process_rate, progress_cb cb,
	flow_func_flush_chunk_t func_flush_chunk)
{
	fw->total_size		= total_size;
	fw->total_processed	= 0;
	fw->cb			= cb;
	fw->block_size		= block_size; /* Bytes		*/
	fw->blocks_per_delay	= 1;	/* block_size B/s	*/
	fw->delay_ns		= 1000000000ULL;	/* 1s	*/
	fw->max_process_rate	= max_process_rate <= 0
		? DBL_MAX : max_process_rate * 1024.;
	fw->measured_blocks	= 0;
	fw->measured_time_ns	= 0;
	fw->erase		= 0;
	fw->func_flush_chunk	= func_flush_chunk;
	fw->processed_blocks	= 0;
	fw->acc_delay_ns	= 0;
	assert(fw->block_size > 0);
	assert(fw->block_size % SECTOR_SIZE == 0);

	move_to_inc_at_start(fw);
}

static inline unsigned int repeat_ch(char *buf, char ch, int count)
{
	int i;

	for (i = 0; i < count; i++)
		buf[i] = ch;
	return count;
}

void clear_progress(struct flow *fw)
{
	char buf[512], *at_buf = buf;

	if (fw->erase <= 0)
		goto out;
	assert((size_t)fw->erase * 3 + 1 <= sizeof(buf));

	at_buf += repeat_ch(at_buf, '\b', fw->erase);
	at_buf += repeat_ch(at_buf, ' ', fw->erase);
	at_buf += repeat_ch(at_buf, '\b', fw->erase);
	at_buf[0] = '\0';

	/* Pass buf as the format, so the implementation of cb can check that
	 * the intention is to clear the previously reported progress.
	 */
	fw->cb(buf);
out:
	fw->erase = 0;
}

#define CHECK_AND_MOVE do {			\
		assert(c > 0);			\
		len += c;			\
		assert((size_t)c < rem_size);	\
		rem_size -= c;			\
		at_buf += c;			\
	} while (0)

static int pr_time(char *buf, const size_t size, double sec)
{
	char *at_buf = buf;
	size_t rem_size = size;
	bool has_h, has_m;
	int c, len = 0;

	c = snprintf(at_buf, rem_size, " -- ");
	CHECK_AND_MOVE;

	has_h = sec >= 3600;
	if (has_h) {
		double h = floor(sec / 3600);
		c = snprintf(at_buf, rem_size, "%i:", (int)h);
		CHECK_AND_MOVE;
		sec -= h * 3600;
	}

	has_m = has_h || sec >= 60;
	if (has_m) {
		double m = floor(sec / 60);
		if (has_h)
			c = snprintf(at_buf, rem_size, "%02i:", (int)m);
		else
			c = snprintf(at_buf, rem_size, "%i:", (int)m);
		CHECK_AND_MOVE;
		sec -= m * 60;
	}

	if (has_m)
		c = snprintf(at_buf, rem_size, "%02i", (int)round(sec));
	else
		c = snprintf(at_buf, rem_size, "%is", (int)round(sec));
	CHECK_AND_MOVE;

	return len;
}

static inline double get_avg_speed_given_time(const struct flow *fw,
	uint64_t total_time_ns)
{
	return ((double)(fw->measured_blocks * fw->block_size) * 1000000000.0)
		/ total_time_ns;
}

/* Average writing speed in byte/s. */
static inline double get_avg_speed(const struct flow *fw)
{
	return get_avg_speed_given_time(fw, fw->measured_time_ns);
}

static void report_progress(struct flow *fw, double inst_speed)
{
	const char *unit = adjust_unit(&inst_speed);
	double percent;
	char buf[256];
	int c, len = 0;

	/* The following shouldn't be necessary, but sometimes
	 * the initial free space isn't exactly reported
	 * by the kernel; this issue has been seen on Macs.
	 */
	if (fw->total_size < fw->total_processed)
		fw->total_size = fw->total_processed;

	percent = (double)fw->total_processed * 100 / fw->total_size;
	c = snprintf(buf, sizeof(buf), "%.2f%% -- %.2f %s/s",
		percent, inst_speed, unit);
	assert(c > 0);
	len += c;

	if (has_enough_measurements(fw)) {
		c = pr_time(buf + len, sizeof(buf) - len,
			(fw->total_size - fw->total_processed) /
			get_avg_speed(fw));
		assert(c > 0);
		len += c;
	}

	assert((size_t)len + 1 <= sizeof(buf));
	clear_progress(fw);
	fw->cb("%s", buf);
	fw->erase = len;
}

static inline void __start_measurement(struct flow *fw)
{
	assert(!clock_gettime(CLOCK_MONOTONIC, &fw->t1));
}

void start_measurement(struct flow *fw)
{
	/*
	 * The report below is especially useful when a single measurement spans
	 * multiple files; this happens when a drive is faster than 1GB/s.
	 */
	report_progress(fw,
		fw->blocks_per_delay * fw->block_size * 1000000000.0 /
			fw->delay_ns);
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
	uint64_t delay_ns, double inst_speed)
{
	/* We use logical or here to enforce the lowest limit. */
	return delay_ns > fw->delay_ns || inst_speed > fw->max_process_rate;
}

static inline int is_rate_below(const struct flow *fw,
	uint64_t delay_ns, double inst_speed)
{
	/* We use logical and here to enforce both limits. */
	return delay_ns <= fw->delay_ns && inst_speed < fw->max_process_rate;
}

static inline int flush_chunk(const struct flow *fw, int fd)
{
	if (fw->func_flush_chunk)
		return fw->func_flush_chunk(fw, fd);
	return 0;
}

int measure(int fd, struct flow *fw, long processed)
{
	ldiv_t result = ldiv(processed, fw->block_size);
	struct timespec t2;
	uint64_t delay_ns;
	double bytes_g, inst_speed;

	assert(result.rem == 0);
	fw->processed_blocks += result.quot;
	fw->total_processed += processed;

	if (fw->processed_blocks < fw->blocks_per_delay)
		return 0;
	assert(fw->processed_blocks == fw->blocks_per_delay);

	if (flush_chunk(fw, fd) < 0)
		return -1; /* Caller can read errno(3). */

	assert(!clock_gettime(CLOCK_MONOTONIC, &t2));
	delay_ns = diff_timespec_ns(&fw->t1, &t2) + fw->acc_delay_ns;

	/* Instantaneous speed in bytes per second. */
	bytes_g = fw->blocks_per_delay * fw->block_size * 1000000000.0;
	inst_speed = bytes_g / delay_ns;

	if (delay_ns < fw->delay_ns && inst_speed > fw->max_process_rate) {
		/* delay_ns should be such that
		 * inst_speed <= fw->max_process_rate.
		 * To accomplish this, the code below adds a wait.
		 *
		 * inst_speed <= fw->max_process_rate [=>]
		 * bytes_g / (delay_ns + wait_ns) <= fw->max_process_rate [=>]
		 * bytes_g / fw->max_process_rate <= delay_ns + wait_ns [=>]
		 * wait_ns >= bytes_g / fw->max_process_rate - delay_ns
		 *
		 * The step below minimizes rounding errors.
		 *
		 * wait_ns >= (bytes_g - delay_ns * fw->max_process_rate) /
		 *	fw->max_process_rate
		 *
		 * Round wait_ns, so it operates as an integer when used in
		 * nanoseconds.
		 */
		double wait_ns = round(
			(bytes_g - delay_ns * fw->max_process_rate) /
			fw->max_process_rate);

		/* From the if-test,
		 * 	inst_speed > fw->max_process_rate [=>]
		 * 	bytes_g / delay_ns > fw->max_process_rate [=>]
		 *	bytes_g > delay_ns * fw->max_process_rate
		 *
		 * For wait_ns to be negative,
		 *	wait_ns < 0 [=>]
		 *	(bytes_g - delay_ns * fw->max_process_rate) /
		 *		fw->max_process_rate < 0 [=>]
		 *	bytes_g < delay_ns * fw->max_process_rate
		 *
		 * Therefore, wait_ns cannot be negative.
		 */
		assert(wait_ns >= 0);

		if (delay_ns + wait_ns < fw->delay_ns) {
			/* In this case, There is a factor f > 1 that
			 * satisfies the following equation:
			 *
			 * (delay_ns + wait_ns) * f = fw->delay_ns
			 *
			 * This means that both delay_ns and wait_ns should be
			 * increased to make f = 1. To signal that to the flow
			 * algorithm below, wait to fw->delay_ns.
			 */
			wait_ns = fw->delay_ns - delay_ns;
		}

		if (wait_ns > 0) {
			/* Slow down. */
			ussleep(wait_ns / 1000.);

			/* Adjust measurements. */
			delay_ns += wait_ns;
			inst_speed = bytes_g / delay_ns;
		}
	}

	/* Update mean. */
	fw->measured_blocks += fw->processed_blocks;
	fw->measured_time_ns += delay_ns;

	switch (fw->state) {
	case FW_INC:
		if (is_rate_above(fw, delay_ns, inst_speed)) {
			move_to_search(fw,
				fw->blocks_per_delay - fw->step / 2,
				fw->blocks_per_delay);
		} else if (is_rate_below(fw, delay_ns, inst_speed)) {
			inc_step(fw);
		} else
			move_to_steady(fw);
		break;

	case FW_DEC:
		if (is_rate_above(fw, delay_ns, inst_speed)) {
			dec_step(fw);
		} else if (is_rate_below(fw, delay_ns, inst_speed)) {
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

		if (is_rate_above(fw, delay_ns, inst_speed)) {
			fw->bpd2 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else if (is_rate_below(fw, delay_ns, inst_speed)) {
			fw->bpd1 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else
			move_to_steady(fw);
		break;

	case FW_STEADY: {
		if (delay_ns <= fw->delay_ns) {
			if (inst_speed < fw->max_process_rate) {
				move_to_inc(fw);
			} else if (inst_speed > fw->max_process_rate) {
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

	report_progress(fw, inst_speed);

	/* Reset accumulators. */
	fw->processed_blocks = 0;
	fw->acc_delay_ns = 0;
	__start_measurement(fw);
	return 0;
}

int end_measurement(int fd, struct flow *fw)
{
	struct timespec t2;
	int saved_errno;
	int ret = 0;

	if (fw->processed_blocks <= 0)
		goto out;

	if (flush_chunk(fw, fd) < 0) {
		saved_errno = errno;
		ret = -1;
		goto out;
	}

	/* Save time in between closing ongoing file and creating a new file. */
	assert(!clock_gettime(CLOCK_MONOTONIC, &t2));
	fw->acc_delay_ns += diff_timespec_ns(&fw->t1, &t2);

out:
	/* Erase progress information. */
	clear_progress(fw);

	if (ret < 0) {
		/* Propagate errno(3) to caller. */
		errno = saved_errno;
	}
	return ret;
}

static inline void pr_avg_speed(const char *speed_type, double speed)
{
	const char *unit = adjust_unit(&speed);
	printf("Average %s speed: %.2f %s/s\n", speed_type, speed, unit);
}

static inline int64_t delay_ms(const struct timeval *t1,
	const struct timeval *t2)
{
	return (int64_t)(t2->tv_sec  - t1->tv_sec)  * 1000 +
			(t2->tv_usec - t1->tv_usec) / 1000;
}

void print_measured_speed(const struct flow *fw, const struct timeval *t1,
	const struct timeval *t2, const char *speed_type)
{
	if (has_enough_measurements(fw)) {
		pr_avg_speed(speed_type, get_avg_speed(fw));
	} else {
		/* If the drive is too fast for the measurements above,
		 * try a coarse approximation of the speed.
		 */
		int64_t total_time_ms = delay_ms(t1, t2);
		if (total_time_ms > 0) {
			pr_avg_speed(speed_type,
				get_avg_speed_given_time(fw, total_time_ms *
					1000000ULL));
		} else {
			assert(strlen(speed_type) > 0);
			printf("%c%s speed not available\n",
				toupper(speed_type[0]), speed_type + 1);
		}
	}
}

static inline void __dbuf_free(struct dynamic_buffer *dbuf)
{
	if (dbuf->buf != dbuf->backup_buf)
		free(dbuf->buf);
}

void dbuf_free(struct dynamic_buffer *dbuf)
{
	__dbuf_free(dbuf);
	dbuf->buf = NULL;
	dbuf->len = 0;
	dbuf->max_buf = true;
}

char *dbuf_get_buf(struct dynamic_buffer *dbuf, size_t size)
{
	/* If enough buffer, or it's already the largest buffer, return it. */
	if (size <= dbuf->len || dbuf->max_buf)
		return dbuf->buf;

	/*
	 * Allocate a new buffer.
	 */

	__dbuf_free(dbuf);
	do {
		dbuf->buf = malloc(size);
		if (dbuf->buf != NULL) {
			dbuf->len = size;
			return dbuf->buf;
		} else {
			dbuf->max_buf = true;
		}
		size /= 2;
	} while (size > sizeof(dbuf->backup_buf));

	/* A larger buffer is not available; failsafe. */
	dbuf->buf = dbuf->backup_buf;
	dbuf->len = sizeof(dbuf->backup_buf);
	return dbuf->buf;
}
