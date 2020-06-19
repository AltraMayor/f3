#ifndef HEADER_LIBFLOW_H
#define HEADER_LIBFLOW_H

#include <stdint.h>

struct flow;

typedef int (*flow_func_flush_chunk_t)(const struct flow *fw, int fd);

struct flow {
	/* Total number of bytes to be written. */
	uint64_t	total_size;
	/* Total number of bytes already written. */
	uint64_t	total_written;
	/* If true, show progress. */
	int		progress;
	/* Block size in bytes. */
	int		block_size;
	/* Delay intended between measurements in miliseconds. */
	unsigned int	delay_ms;
	/* Increment to apply to @blocks_per_delay. */
	int64_t		step;
	/* Blocks to write before measurement. */
	int64_t		blocks_per_delay;
	/* Maximum write rate in bytes per second. */
	double		max_write_rate;
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

	/* Number of blocks written since last measurement. */
	int64_t		written_blocks;
	/*
	 * Accumulated delay before @written_blocks reaches @blocks_per_delay
	 * in microseconds.
	 */
	uint64_t	acc_delay_us;
	/* Range of blocks_per_delay while in FW_SEARCH state. */
	int64_t		bpd1, bpd2;
	/* Time measurements. */
	struct timeval	t1;
};

/* If @max_write_rate <= 0, the maximum write rate is infinity.
 * The unit of @max_write_rate is KB per second.
 */
void init_flow(struct flow *fw, uint64_t total_size,
	long max_write_rate, int progress,
	flow_func_flush_chunk_t func_flush_chunk);

void start_measurement(struct flow *fw);
int measure(int fd, struct flow *fw, ssize_t written);
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

#define MAX_WRITE_SIZE	(1<<21)	/* 2MB */

#endif	/* HEADER_LIBFLOW_H */
