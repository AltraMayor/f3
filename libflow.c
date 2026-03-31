#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <string.h>

#include "libflow.h"
#include "libutils.h"

/* Apple Macintosh / OpenBSD */
#if (__APPLE__ && __MACH__) || defined(__OpenBSD__)

static void nssleep(uint64_t wait_ns)
{
	const lldiv_t div = lldiv(wait_ns, 1000000000);
	const struct timespec req = {
		.tv_sec = div.quot,
		.tv_nsec = div.rem,
	};
	assert(!nanosleep(&req, NULL));
}

#else	/* Everyone else */

static void nssleep(uint64_t wait_ns)
{
	struct timespec req;
	lldiv_t div;
	int ret;

	assert(!clock_gettime(CLOCK_MONOTONIC, &req));

	/* Add @wait_ns to @req. */
	div = lldiv(wait_ns, 1000000000);
	req.tv_sec += div.quot;
	req.tv_nsec += div.rem;

	/* Round @req up. */
	if (req.tv_nsec >= 1000000000) {
		req.tv_sec++;
		req.tv_nsec -= 1000000000;
	}

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&req, NULL);
	} while (ret == EINTR);

	assert(ret == 0);
}

#endif	/* nssleep() */

static inline void move_to_inc_at_start(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_INC;
}

void init_flow(struct flow *fw, int block_size, uint64_t total_size,
	long max_process_rate, progress_cb cb, unsigned int indent,
	flow_func_flush_chunk_t func_flush_chunk)
{
	fw->total_size		= total_size;
	fw->total_processed	= 0;
	fw->cb			= cb;
	fw->indent		= indent;
	fw->block_size		= block_size; /* Bytes		*/
	fw->blocks_per_delay	= 1;	/* block_size B/s	*/
	fw->delay_ns		= 1000000000ULL;	/* 1s	*/
	fw->max_process_rate	= max_process_rate <= 0
		? DBL_MAX : max_process_rate * 1024.;
	fw->measured_blocks	= 0;
	fw->measured_time_ns	= 0;
	fw->erase		= 0;
	fw->func_flush_chunk	= func_flush_chunk;
	fw->has_rem_chunk_size	= false;
	fw->rem_chunk_size	= 0;
	fw->rem_chunk_speed	= 0;
	fw->processed_blocks	= 0;
	fw->acc_delay_ns	= 0;
	assert(fw->block_size > 0);
	assert(fw->block_size % SECTOR_SIZE == 0);

	move_to_inc_at_start(fw);
}

uint64_t get_rem_chunk_size(const struct flow *fw)
{
	const int64_t rem_blocks = fw->blocks_per_delay - fw->processed_blocks;
	const uint64_t rem_size = rem_blocks * fw->block_size;
	assert(rem_blocks > 0);
	return fw->has_rem_chunk_size && rem_size >= fw->rem_chunk_size
		? fw->rem_chunk_size
		: rem_size;
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

	if (fw->erase <= 0) {
		if (fw->indent > 0) {
			/* Remove indented empty line. */
			fw->cb(fw->indent, "\b");
		}
		goto out;
	}

	assert((size_t)fw->erase * 3 + 1 <= sizeof(buf));
	at_buf += repeat_ch(at_buf, '\b', fw->erase);
	at_buf += repeat_ch(at_buf, ' ', fw->erase);
	at_buf += repeat_ch(at_buf, '\b', fw->erase);
	at_buf[0] = '\0';

	/* Pass buf as the format, so the implementation of cb can check that
	 * the intention is to clear the previously reported progress.
	 */
	fw->cb(fw->indent, buf);
out:
	fw->erase = 0;
}

static inline bool has_enough_measurements(const struct flow *fw)
{
	return fw->measured_time_ns > fw->delay_ns;
}

#define CHECK_AND_MOVE do {			\
		assert(c > 0);			\
		len += c;			\
		assert((size_t)c < rem_size);	\
		rem_size -= c;			\
		at_buf += c;			\
	} while (0)

static void report_progress(struct flow *fw, double inst_speed)
{
	const char *unit = adjust_unit(&inst_speed);
	double percent;
	char buf[128 + TIME_STR_SIZE];
	char *at_buf = buf;
	size_t rem_size = sizeof(buf);
	int c, len = 0;

	/* The following shouldn't be necessary, but sometimes
	 * the initial free space isn't exactly reported
	 * by the kernel; this issue has been seen on Macs.
	 */
	if (fw->total_size < fw->total_processed)
		fw->total_size = fw->total_processed;

	percent = fw->total_processed * 100.0 / fw->total_size;
	c = snprintf(at_buf, rem_size, "%.2f%% -- %.2f %s/s",
		percent, inst_speed, unit);
	CHECK_AND_MOVE;

	if (has_enough_measurements(fw)) {
		const double rem_size_byte =
			fw->total_size - fw->total_processed;
		const double speed_byte_per_ns =
			(double)(fw->measured_blocks * fw->block_size) /
			fw->measured_time_ns;
		const uint64_t rem_time_ns =
			round(rem_size_byte / speed_byte_per_ns);

		c = snprintf(at_buf, rem_size, " -- ");
		CHECK_AND_MOVE;

		assert(rem_size >= TIME_STR_SIZE);
		c = nsec_to_str(rem_time_ns, at_buf);
		CHECK_AND_MOVE;
	}

	assert((size_t)len + 1 <= sizeof(buf));
	clear_progress(fw);
	fw->cb(fw->indent, "%s", buf);
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

static void update_rem_chunk_size(struct flow *fw, double inst_speed)
{
	if (fw->rem_chunk_size != 0 && inst_speed < fw->rem_chunk_speed)
		return;

	fw->rem_chunk_size = (uint64_t)fw->blocks_per_delay * fw->block_size;
	fw->rem_chunk_speed = inst_speed;
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
	bytes_g = fw->blocks_per_delay * fw->block_size * 1000000000.0;
	/* Instantaneous speed in bytes per second. */
	inst_speed = bytes_g / delay_ns;

	if (!fw->has_rem_chunk_size)
		update_rem_chunk_size(fw, inst_speed);

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
		uint64_t wait_ns = round(
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
			nssleep(wait_ns);

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
			if (!fw->has_rem_chunk_size) {
				/* Recommend a chunk size to caller. */
				assert(fw->rem_chunk_size != 0);
				fw->has_rem_chunk_size = true;
			}
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
		if (!fw->has_rem_chunk_size) {
			/* Recommend a chunk size to caller.
			 * Execution reaches here when fw->max_process_rate is
			 * throttling the flow.
			 */
			assert(fw->rem_chunk_size != 0);
			fw->has_rem_chunk_size = true;
			/* Since it's in steady state, go for another round
			 * before making any change.
			 */
			break;
		}
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

void print_avg_seq_speed(const struct flow *fw, const char *speed_type,
	bool use_sectors)
{
	int block_order = fw_get_block_order(fw);
	uint64_t blocks, time_ns;
	char prefix[128];
	int ret = snprintf(prefix, sizeof(prefix), "Average sequential %s speed:",
		speed_type);
	assert(ret > 0 && (size_t)ret < sizeof(prefix));

	fw_get_measurements(fw, &blocks, &time_ns);
	if (use_sectors && block_order != SECTOR_ORDER) {
		assert(block_order > SECTOR_ORDER);
		blocks <<= block_order - SECTOR_ORDER;
		block_order = SECTOR_ORDER;
	}

	report_io_speed(0, printf_cb, prefix, blocks,
		use_sectors ? "sector" : "block",
		time_ns, block_order);
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
