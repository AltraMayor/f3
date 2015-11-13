#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

#include "libdevs.h"

uint64_t probe_device_max_blocks(struct device *dev);

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap,
	uint64_t *pcache_size_block, int *pneed_reset, int *pblock_order);

#endif	/* HEADER_LIBPROBE_H */
