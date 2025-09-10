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

struct device;

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

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap,
	int block_order, int cache_order, int strict_cache,
	int keep_file);

enum reset_type {
	RT_MANUAL_USB = 0,
	RT_USB,
	RT_NONE,
	RT_MAX
};

struct device *create_block_device(const char *filename, enum reset_type rt);

struct device *create_perf_device(struct device *dev);
void perf_device_sample(struct device *dev,
	uint64_t *pread_count, uint64_t *pread_time_us,
	uint64_t *pwrite_count, uint64_t *pwrite_time_us,
	uint64_t *preset_count, uint64_t *preset_time_us);
/* Detach the shadow device of @pdev, free @pdev, and return
 * the shadow device.
 */
struct device *pdev_detach_and_free(struct device *dev);

struct device *create_safe_device(struct device *dev, uint64_t max_blocks,
	int min_memory);

void sdev_recover(struct device *dev, uint64_t very_last_pos);
void sdev_flush(struct device *dev);

#endif	/* HEADER_LIBDEVS_H */
