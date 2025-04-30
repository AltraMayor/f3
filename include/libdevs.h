#ifndef HEADER_LIBDEVS_H
#define HEADER_LIBDEVS_H

#include <stdint.h>

/*
 *	Device model
 */

enum fake_type {
	/* Device is good. */
	FKTY_GOOD,

	/* Device is at least partially damaged. */
	FKTY_BAD,

	/* Device discards data after a given limit. */
	FKTY_LIMBO,

	/* Device overwrites data after a given limit. */
	FKTY_WRAPAROUND,

	/* Device is a sequence of wraparound and limbo regions. */
	FKTY_CHAIN,

	FKTY_MAX
};

const char *fake_type_to_name(enum fake_type fake_type);

int dev_param_valid(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order);

enum fake_type dev_param_to_type(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order);

/*
 *	Abstract device
 */

struct device {
    uint64_t size_byte;
    int block_order;

    int (*read_blocks)(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos);
	int (*write_blocks)(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos);
	int (*reset)(struct device *dev);
	void (*free)(struct device *dev);
	const char *(*get_filename)(struct device *dev);
};

/*
 *	Properties
 */

uint64_t dev_get_size_byte(struct device *dev);
int dev_get_block_order(struct device *dev);
int dev_get_block_size(struct device *dev);
/* File name of the device.
 * This information is important because the filename may change due to resets.
 */
const char *dev_get_filename(struct device *dev);

/*
 *	Methods
 */

/* One should use the following constant as the size of the buffer needed to
 * batch writes or reads.
 *
 * It must be a power of 2 greater than, or equal to 2^20.
 * The current value is 1MB.
 */
#define BIG_BLOCK_SIZE_BYTE (1 << 20)

int dev_read_blocks(struct device *dev, char *buf,
	uint64_t first_pos, uint64_t last_pos);
int dev_write_blocks(struct device *dev, const char *buf,
	uint64_t first_pos, uint64_t last_pos);

int dev_reset(struct device *dev);
void free_device(struct device *dev);

/*
 *	Concrete devices
 */

enum reset_type {
	RT_MANUAL_USB = 0,
	RT_USB,
	RT_NONE,
	RT_MAX
};

#include "devices/file_device.h"
#include "devices/block_device.h"
#include "devices/perf_device.h"
#include "devices/safe_device.h"

#endif	/* HEADER_LIBDEVS_H */
