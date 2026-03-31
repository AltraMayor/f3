#ifndef HEADER_LIBFLOW_H
#define HEADER_LIBFLOW_H

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "libutils.h"

struct flow;

typedef int (*flow_func_flush_chunk_t)(const struct flow *fw, int fd);

struct flow {
	/* Total number of bytes to be processed. */
	uint64_t	total_size;
	/* Total number of bytes already processed. */
	uint64_t	total_processed;
	/* Callback to show progress. */
	progress_cb	cb;
	/* Indentation level for callback. */
	unsigned int	indent;
	/* Block size in bytes. */
	int		block_size;
	/* Delay intended between measurements in nanoseconds. */
	uint64_t	delay_ns;
	/* Increment to apply to @blocks_per_delay. */
	int64_t		step;
	/* Blocks to process before measurement. */
	int64_t		blocks_per_delay;
	/* Maximum processing rate in bytes per second. */
	double		max_process_rate;
	/* Number of measured blocks. */
	uint64_t	measured_blocks;
	/* Measured time. */
	uint64_t	measured_time_ns;
	/* State. */
	enum {FW_INC, FW_DEC, FW_SEARCH, FW_STEADY} state;
	/* Number of characters to erase before printing out progress. */
	int		erase;

	/*
	 * Methods
	 */
	flow_func_flush_chunk_t func_flush_chunk;

	/*
	 * Initialized while measuring
	 */

	/* Has a recommended chunk size? */
	bool		has_rem_chunk_size;
	/* Recommended chunk size. */
	uint64_t	rem_chunk_size;
	/* Speed of the recommended chunk size in bytes per second. */
	double		rem_chunk_speed;

	/* Number of blocks processed since last measurement. */
	int64_t		processed_blocks;
	/*
	 * Accumulated delay before @processed_blocks reaches @blocks_per_delay
	 * in nanoseconds.
	 */
	uint64_t	acc_delay_ns;
	/* Range of blocks_per_delay while in FW_SEARCH state. */
	int64_t		bpd1, bpd2;
	/* Time measurements. */
	struct timespec	t1;
};

/* If @max_process_rate <= 0, the maximum processing rate is infinity.
 * The unit of @max_process_rate is KB per second.
 */
void init_flow(struct flow *fw, int block_size, uint64_t total_size,
	long max_process_rate, progress_cb cb, unsigned int indent,
	flow_func_flush_chunk_t func_flush_chunk);

static inline int fw_get_block_size(const struct flow *fw)
{
	return fw->block_size;
}

static inline int fw_get_block_order(const struct flow *fw)
{
	return ilog2(fw->block_size);
}

static inline void inc_total_size(struct flow *fw, uint64_t size)
{
	fw->total_size = fw->total_processed + size;
}

static inline void fw_set_indent(struct flow *fw, unsigned int indent)
{
	fw->indent = indent;
}

static inline void fw_get_measurements(const struct flow *fw,
	uint64_t *blocks, uint64_t *time_ns)
{
	*blocks = fw->measured_blocks + fw->processed_blocks;
	*time_ns = fw->measured_time_ns + fw->acc_delay_ns;
}

uint64_t get_rem_chunk_size(const struct flow *fw);

void start_measurement(struct flow *fw);
int measure(int fd, struct flow *fw, long processed);
void clear_progress(struct flow *fw);
int end_measurement(int fd, struct flow *fw);

void print_avg_seq_speed(const struct flow *fw, const char *speed_type,
	bool use_sectors);

struct dynamic_buffer {
	char   *buf;
	size_t len;
	bool   max_buf;
	/* Ensure that backup_buf has the same memory alignment as
	 * it would have, had it been returned by malloc().
	 */
	alignas(max_align_t) char backup_buf[1 << 21]; /* 2MB */
};

static inline void dbuf_init(struct dynamic_buffer *dbuf)
{
	dbuf->buf = dbuf->backup_buf;
	dbuf->len = sizeof(dbuf->backup_buf);
	dbuf->max_buf = false;
}

void dbuf_free(struct dynamic_buffer *dbuf);

/*
 * Although the returned buffer may be smaller than @size,
 * this function never returns NULL.
 */
char *dbuf_get_buf(struct dynamic_buffer *dbuf, size_t size);

static inline size_t dbuf_get_len(const struct dynamic_buffer *dbuf)
{
	return dbuf->len;
}

#endif	/* HEADER_LIBFLOW_H */
