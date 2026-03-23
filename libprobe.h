#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

#include "libutils.h"
#include "libdevs.h"

/* Provide an upper bound on the number of blocks that will be written when
 * probe_device(dev) is called.
 *
 * Notice that probe_device() may read more blocks than it writes.
 * For example, the helper function find_wrap() only writes 1 block,
 * but reads multiple blocks.
 */
uint64_t probe_max_written_blocks(const struct device *dev);

void report_probed_size(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t bytes, int block_order);

void report_probed_order(unsigned int indent, progress_cb cb,
	const char *prefix, int order);

void report_probed_cache(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t cache_size_block, int block_order);

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap,
	uint64_t *pcache_size_block, int *pblock_order,
	progress_cb cb, int show_progress);

#endif	/* HEADER_LIBPROBE_H */
