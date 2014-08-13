#ifndef HEADER_LIBPROBE_H
#define HEADER_LIBPROBE_H

enum fake_type {
	FKTY_GOOD,	/* Device is good.				*/
	FKTY_BAD,	/* Device is at least partially damaged.	*/
	FKTY_LIMBO,	/* Device discards data after a given limit.	*/
	FKTY_WRAPAROUND,/* Device overwrites data after a given limit.	*/
	FKTY_MAX,
};

const char *fake_type_to_name(enum fake_type fake_type);

struct device;

struct device *create_file_device(const char *filename,
	int file_size_gb, int fake_size_gb, enum fake_type fake_type);

struct device *create_block_device(const char *filename);

void free_device(struct device *dev);

enum fake_type probe_device(struct device *dev, int *real_size_gb);

#endif	/* HEADER_LIBPROBE_H */
