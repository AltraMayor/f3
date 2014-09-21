#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

#include <stdint.h>

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

	FKTY_MAX,
};

const char *fake_type_to_name(enum fake_type fake_type);

int dev_param_valid(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order);

enum fake_type dev_param_to_type(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order);

struct device;

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap,
	int block_order, int keep_file);

struct device *create_block_device(const char *filename, int manual_reset);

struct device *create_safe_device(struct device *dev, int max_blocks,
	int min_memory);

void free_device(struct device *dev);

int probe_device_max_blocks(struct device *dev);
void probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap, int *block_order);

#endif	/* HEADER_LIBPROBE_H */
