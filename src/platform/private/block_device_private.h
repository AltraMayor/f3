#ifndef PLATFORM_PRIVATE_BLOCK_DEVICE_H
#define PLATFORM_PRIVATE_BLOCK_DEVICE_H

#include "libdevs.h"

struct block_device {
	/* This must be the first field. See dev_bdev() for details. */
	struct device dev;

	const char *filename;
	int fd;
};

static inline struct block_device *dev_bdev(struct device *dev)
{
	return (struct block_device *)dev;
}

#endif	/* PLATFORM_PRIVATE_BLOCK_DEVICE_H */
