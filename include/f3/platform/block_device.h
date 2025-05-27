#ifndef HEADER_PLATFORM_BLOCK_DEVICE_H
#define HEADER_PLATFORM_BLOCK_DEVICE_H

#include <f3/libdevs.h>	/* For device, reset_type.	*/

/* Create a raw block device wrapper.
 * filename: path to the block device (e.g., /dev/sdx).
 * rt: reset type (use enum reset_type).
 * Returns pointer to struct device or NULL on error.
 */
 struct device *create_block_device(const char *filename, enum reset_type rt);

#endif	/* HEADER_PLATFORM_BLOCK_DEVICE_H */
