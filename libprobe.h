#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

#include "libdevs.h"

uint64_t probe_device_max_blocks(struct device *dev);

typedef void (*probe_progress_cb)(const char *format, ...);

void printf_cb(const char *format, ...);

void report_probed_size(probe_progress_cb cb, const char *prefix,
	uint64_t bytes, int block_order);

void report_probed_order(probe_progress_cb cb, const char *prefix, int order);

void report_probed_cache(probe_progress_cb cb, const char *prefix,
	uint64_t cache_size_block, int need_reset, int order);

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap,
	uint64_t *pcache_size_block, int *pneed_reset, int *pblock_order,
	probe_progress_cb cb);

#endif	/* HEADER_LIBPROBE_H */
