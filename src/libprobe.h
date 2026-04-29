#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

#include "libutils.h"
#include "libdevs.h"
#include "libflow.h" /* Enable usage of FW_MAX_PROCESS_RATE_NONE */

/* Provide an upper bound on the number of *unique* blocks that will be written
 * when probe_device(dev) is called.
 *
 * Notice that probe_device() may read more blocks than it writes, and
 * it may write the same block multiple times. For example, the helper function
 * find_wrap() only writes 1 block, but reads multiple blocks. And the helper
 * function overwhelm_cache() is called multiple times over the same blocks.
 */
uint64_t probe_max_written_blocks(const struct device *dev);

void report_probed_size(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t bytes, unsigned int block_order);

void report_probed_order(unsigned int indent, progress_cb cb,
	const char *prefix, unsigned int order);

void report_probed_cache(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t cache_size_block,
	unsigned int block_order);

struct probe_results {
	uint64_t real_size_byte;
	uint64_t announced_size_byte;
	int wrap;
	uint64_t cache_size_block;
	unsigned int block_order;

	uint64_t seqw_blocks, seqw_time_ns;
	uint64_t randw_blocks, randw_time_ns;
	uint64_t randr_blocks, randr_time_ns;
};

int probe_device(struct device *dev, struct probe_results *results,
	progress_cb cb, int show_progress,
	long max_read_rate, long max_write_rate);

#endif	/* HEADER_LIBPROBE_H */
