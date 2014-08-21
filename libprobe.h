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
	uint64_t announced_size_byte, int wrap);

enum fake_type dev_param_to_type(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap);

struct device;

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap);

struct device *create_block_device(const char *filename);

void free_device(struct device *dev);

void probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap);

#endif	/* HEADER_LIBPROBE_H */
