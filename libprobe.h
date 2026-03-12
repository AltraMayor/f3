#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

#include "libutils.h"
#include "libdevs.h"

uint64_t probe_device_max_blocks(const struct device *dev);

void report_probed_size(progress_cb cb, const char *prefix, uint64_t bytes,
	int block_order);

void report_probed_order(progress_cb cb, const char *prefix, int order);

void report_probed_cache(progress_cb cb, const char *prefix,
	uint64_t cache_size_block, int block_order);

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap,
	uint64_t *pcache_size_block, int *pblock_order,
	progress_cb cb);

#endif	/* HEADER_LIBPROBE_H */
