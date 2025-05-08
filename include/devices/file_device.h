#ifndef HEADER_DEVICES_FILE_DEVICE_H
#define HEADER_DEVICES_FILE_DEVICE_H

#include "libdevs.h"
#include <stdint.h>	/* For type uint64_t.	*/

/* Create a file-backed device that masquerades as a block device.
 * filename: path to the backing file.
 * real_size_byte: actual allocated size on disk.
 * fake_size_byte: size exposed via read/write wrappers.
 * wrap: wrap-around mask power (bits).
 * block_order: log2(block size).
 * cache_order: log2(number of cache entries) or -1 for no cache.
 * strict_cache: if non-zero, track exact block positions.
 * keep_file: if zero, unlink backing file immediately.
 * Returns pointer to allocated struct device or NULL on error.
 */
 struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte,
	int wrap, int block_order, int cache_order,
	int strict_cache, int keep_file);

#endif	/* HEADER_DEVICES_FILE_DEVICE_H */
