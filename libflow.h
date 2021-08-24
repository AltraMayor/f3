#ifndef HEADER_LIBFLOW_H
#define HEADER_LIBFLOW_H

#include <stdint.h>

struct flow;

typedef int (*flow_func_flush_chunk_t)(const struct flow *fw, int fd);

struct flow {
	/* Total number of bytes to be processed. */
	uint64_t	total_size;
	/* Total number of bytes already processed. */
	uint64_t	total_processed;
	/* If true, show progress. */
	int		progress;
	/* Block size in bytes. */
	int		block_size;
	/* Delay intended between measurements in milliseconds. */
	unsigned int	delay_ms;
	/* Increment to apply to @blocks_per_delay. */
	int64_t		step;
	/* Blocks to process before measurement. */
	int64_t		blocks_per_delay;
	/* Maximum processing rate in bytes per second. */
	double		max_process_rate;
	/* Number of measured blocks. */
	uint64_t	measured_blocks;
	/* Measured time. */
	uint64_t	measured_time_ms;
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

	/* Number of blocks processed since last measurement. */
	int64_t		processed_blocks;
	/*
	 * Accumulated delay before @processed_blocks reaches @blocks_per_delay
	 * in microseconds.
	 */
	uint64_t	acc_delay_us;
	/* Range of blocks_per_delay while in FW_SEARCH state. */
	int64_t		bpd1, bpd2;
	/* Time measurements. */
	struct timeval	t1;
};

/* If @max_process_rate <= 0, the maximum processing rate is infinity.
 * The unit of @max_process_rate is KB per second.
 */
void init_flow(struct flow *fw, uint64_t total_size,
	long max_process_rate, int progress,
	flow_func_flush_chunk_t func_flush_chunk);

void start_measurement(struct flow *fw);
int measure(int fd, struct flow *fw, long processed);
int end_measurement(int fd, struct flow *fw);

static inline int has_enough_measurements(const struct flow *fw)
{
	return fw->measured_time_ms > fw->delay_ms;
}

static inline double get_avg_speed_given_time(struct flow *fw,
	uint64_t total_time_ms)
{
	return (double)(fw->measured_blocks * fw->block_size * 1000) /
		total_time_ms;
}

/* Average writing speed in byte/s. */
static inline double get_avg_speed(struct flow *fw)
{
	return get_avg_speed_given_time(fw, fw->measured_time_ms);
}

static inline uint64_t get_rem_chunk_size(struct flow *fw)
{
	assert(fw->blocks_per_delay > fw->processed_blocks);
	return (fw->blocks_per_delay - fw->processed_blocks) * fw->block_size;
}

#define MAX_BUFFER_SIZE	(1<<21)	/* 2MB */

#endif	/* HEADER_LIBFLOW_H */
