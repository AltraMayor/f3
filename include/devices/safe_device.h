#ifndef INCLUDE_DEVICES_SAFE_DEVICE_H
#define INCLUDE_DEVICES_SAFE_DEVICE_H

#include "libdevs.h"
#include <stdint.h>

/* Create a safe device wrapper: snapshots writes for rollback. */
struct device *create_safe_device(struct device *dev,
	uint64_t max_blocks, int min_memory);

/* Recover and flush saved blocks. */
void sdev_recover(struct device *dev, uint64_t very_last_pos);
void sdev_flush(struct device *dev);

#endif	/* INCLUDE_DEVICES_SAFE_DEVICE_H */
