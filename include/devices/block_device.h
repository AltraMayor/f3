#ifndef INCLUDE_DEVICES_BLOCK_DEVICE_H
#define INCLUDE_DEVICES_BLOCK_DEVICE_H

#include "libdevs.h"
#include <stdint.h>

/* Create a raw block device wrapper.
 * filename: path to the block device (e.g., /dev/sdx).
 * rt: reset type (use enum reset_type).
 * Returns pointer to struct device or NULL on error.
 */
 struct device *create_block_device(const char *filename, enum reset_type rt);

#endif	/* INCLUDE_DEVICES_BLOCK_DEVICE_H */
