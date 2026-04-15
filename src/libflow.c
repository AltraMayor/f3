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
	fw->step_blocks = 1;
	fw->state = FW_INC;
}

void init_flow(struct flow *fw, int block_size, uint64_t total_blocks,
	uint64_t max_process_rate, progress_cb cb, unsigned int indent)
{
	fw->total_blocks		= total_blocks;
	fw->cb				= cb;
	fw->indent			= indent;
	fw->block_size			= block_size; /* Bytes		*/
	fw->blocks_per_delay		= 1;	/* block_size B/s	*/
	fw->delay_ns			= 1000000000ULL;	/* 1s	*/
	fw->max_process_rate		= max_process_rate == 0
		? DBL_MAX : max_process_rate * 1024.;
	fw->measured_blocks		= 0;
	fw->measured_time_ns		= 0;
	fw->erase			= 0;
	fw->has_rem_chunk_blocks	= false;
	fw->rem_chunk_blocks		= 0;
	fw->rem_chunk_speed		= 0;
	fw->processed_blocks		= 0;
	fw->acc_delay_ns		= 0;
	assert(fw->block_size > 0);
	assert(fw->block_size % SECTOR_SIZE == 0);

	move_to_inc_at_start(fw);
}

uint64_t get_rem_chunk_blocks(const struct flow *fw)
{
	const uint64_t rem_blocks = fw->blocks_per_delay - fw->processed_blocks;
	assert(fw->blocks_per_delay > fw->processed_blocks);
	return fw->has_rem_chunk_blocks && rem_blocks >= fw->rem_chunk_blocks
		? fw->rem_chunk_blocks
		: rem_blocks;
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

	if (fw->erase == 0) {
		if (fw->indent > 0) {
			/* Remove indented empty line. */
			fw->cb(fw->indent, "\b");
		}
		goto out;
	}

	assert(fw->erase * 3 + 1 <= sizeof(buf));
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
	const uint64_t total_processed_blocks =
		fw_get_total_processed_blocks(fw);
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
	if (fw->total_blocks < total_processed_blocks)
		fw->total_blocks = total_processed_blocks;

	percent = total_processed_blocks * 100.0 / fw->total_blocks;
	c = snprintf(at_buf, rem_size, "%.2f%% -- %.2f %s/s",
		percent, inst_speed, unit);
	CHECK_AND_MOVE;

	if (has_enough_measurements(fw)) {
		const double rem_blocks =
			fw->total_blocks - total_processed_blocks;
		const double speed_blocks_per_ns =
			(double)fw->measured_blocks / fw->measured_time_ns;
		const uint64_t rem_time_ns =
			round(rem_blocks / speed_blocks_per_ns);

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

static void move_to_search(struct flow *fw, uint64_t bpd1, uint64_t bpd2)
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
	if (fw->blocks_per_delay > fw->step_blocks) {
		fw->blocks_per_delay -= fw->step_blocks;
		fw->step_blocks *= 2;
	} else {
		move_to_search(fw, 1,
			fw->blocks_per_delay + fw->step_blocks / 2);
	}
}

static inline void inc_step(struct flow *fw)
{
	fw->blocks_per_delay += fw->step_blocks;
	fw->step_blocks *= 2;
}

static inline void move_to_inc(struct flow *fw)
{
	move_to_inc_at_start(fw);
	inc_step(fw);
}

static inline void move_to_dec(struct flow *fw)
{
	fw->step_blocks = 1;
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

static void update_rem_chunk_blocks(struct flow *fw, double inst_speed)
{
	if (fw->rem_chunk_blocks != 0 && inst_speed < fw->rem_chunk_speed)
		return;

	fw->rem_chunk_blocks = fw->blocks_per_delay;
	fw->rem_chunk_speed = inst_speed;
}

void measure(struct flow *fw, uint64_t processed_blocks)
{
	struct timespec t2;
	uint64_t delay_ns;
	double bytes_g, inst_speed;

	fw->processed_blocks += processed_blocks;
	if (fw->processed_blocks < fw->blocks_per_delay)
		return;
	assert(fw->processed_blocks == fw->blocks_per_delay);

	assert(!clock_gettime(CLOCK_MONOTONIC, &t2));
	delay_ns = diff_timespec_ns(&fw->t1, &t2) + fw->acc_delay_ns;
	bytes_g = fw->blocks_per_delay * fw->block_size * 1000000000.0;
	/* Instantaneous speed in bytes per second. */
	inst_speed = bytes_g / delay_ns;

	if (!fw->has_rem_chunk_blocks)
		update_rem_chunk_blocks(fw, inst_speed);

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

	/* Update average. */
	fw->measured_blocks += fw->processed_blocks;
	fw->measured_time_ns += delay_ns;
	/* Reset accumulators. */
	fw->processed_blocks = 0;
	fw->acc_delay_ns = 0;

	switch (fw->state) {
	case FW_INC:
		if (is_rate_above(fw, delay_ns, inst_speed)) {
			if (!fw->has_rem_chunk_blocks) {
				/* Recommend a chunk size to caller. */
				assert(fw->rem_chunk_blocks != 0);
				fw->has_rem_chunk_blocks = true;
			}
			move_to_search(fw,
				fw->blocks_per_delay - fw->step_blocks / 2,
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
				fw->blocks_per_delay + fw->step_blocks / 2);
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
		if (!fw->has_rem_chunk_blocks) {
			/* Recommend a chunk size to caller.
			 * Execution reaches here when fw->max_process_rate is
			 * throttling the flow.
			 */
			assert(fw->rem_chunk_blocks != 0);
			fw->has_rem_chunk_blocks = true;
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
	__start_measurement(fw);
}

void end_measurement(struct flow *fw)
{
	if (fw->processed_blocks > 0) {
		/* Track progress in between files. */
		struct timespec t2;
		assert(!clock_gettime(CLOCK_MONOTONIC, &t2));
		fw->acc_delay_ns += diff_timespec_ns(&fw->t1, &t2);
	}
	clear_progress(fw); /* Erase progress information. */
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

char *dbuf_get_buf(struct dynamic_buffer *dbuf, int align_order,
	size_t *psize)
{
	const int max_align_order = ilog2(alignof(max_align_t));
	const size_t original_size = *psize;
	size_t size = original_size;
	size_t alignment, threshold;
	int shift;
	char *ret;

	if (align_order < max_align_order)
		align_order = max_align_order;
	alignment = 1ULL << align_order;

	/* If enough buffer and aligned, return it. */
	if (size <= dbuf->len && is_aligned(dbuf->buf, alignment)) {
		assert(*psize == original_size);
		ret = dbuf->buf;
		goto out;
	}

	if (dbuf->max_buf) {
		/* It's already the largest buffer. */
		goto align;
	}

	/*
	 * Allocate a new buffer.
	 */
	__dbuf_free(dbuf);
	threshold = sizeof(dbuf->backup_buf) - align_head(align_order);
	do {
		dbuf->buf = aligned_alloc(alignment, size);
		if (dbuf->buf != NULL) {
			dbuf->len = size;
			*psize = size;
			ret = dbuf->buf;
			goto out;
		} else {
			dbuf->max_buf = true;
		}
		size /= 2;
	} while (size > threshold);

	/* A larger buffer is not available; failsafe. */
	dbuf->buf = dbuf->backup_buf;
	dbuf->len = sizeof(dbuf->backup_buf);

align:
	ret = align_mem2(dbuf->buf, align_order, &shift);
	*psize = MIN(*psize, dbuf->len - shift);
out:
	assert(*psize <= original_size);
	return ret;
}
