#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include "libdevs.h"

/* Map fake_type to string. */
static const char * const ftype_to_name[FKTY_MAX] = {
	[FKTY_GOOD]		= "good",
	[FKTY_BAD]		= "bad",
	[FKTY_LIMBO]		= "limbo",
	[FKTY_WRAPAROUND]	= "wraparound",
	[FKTY_CHAIN]		= "chain",
};

const char *fake_type_to_name(enum fake_type fake_type)
{
	assert(fake_type < FKTY_MAX);
	return ftype_to_name[fake_type];
}

int dev_param_valid(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order)
{
	int block_size;

	/* Check general ranges. */
	if (real_size_byte > announced_size_byte || wrap < 0 || wrap >= 64 ||
		block_order < 9 || block_order > 20)
		return false;

	/* Check alignment of the sizes. */
	block_size = 1 << block_order;
	if (real_size_byte % block_size || announced_size_byte % block_size)
		return false;

	/* If good, @wrap must make sense. */
	if (real_size_byte == announced_size_byte) {
		uint64_t two_wrap = ((uint64_t)1) << wrap;
		return announced_size_byte <= two_wrap;
	}

	return true;
}

enum fake_type dev_param_to_type(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap, int block_order)
{
	uint64_t two_wrap;

	assert(dev_param_valid(real_size_byte, announced_size_byte,
		wrap, block_order));

	if (real_size_byte == announced_size_byte)
		return FKTY_GOOD;

	if (real_size_byte == 0)
		return FKTY_BAD;

	/* real_size_byte < announced_size_byte */

	two_wrap = ((uint64_t)1) << wrap;
	if (two_wrap <= real_size_byte)
		return FKTY_WRAPAROUND;
	if (two_wrap < announced_size_byte)
		return FKTY_CHAIN;
	return FKTY_LIMBO;
}

/* Abstract device interface and wrappers. */
uint64_t dev_get_size_byte(struct device *dev)
{
	return dev->size_byte;
}

int dev_get_block_order(struct device *dev)
{
	return dev->block_order;
}

int dev_get_block_size(struct device *dev)
{
	return 1 << dev->block_order;
}

const char *dev_get_filename(struct device *dev)
{
	return dev->get_filename(dev);
}

int dev_read_blocks(struct device *dev, char *buf,
	uint64_t first_pos, uint64_t last_pos)
{
	if (first_pos > last_pos)
		return false;
	assert(last_pos < (dev->size_byte >> dev->block_order));
	return dev->read_blocks(dev, buf, first_pos, last_pos);
}

int dev_write_blocks(struct device *dev, const char *buf,
	uint64_t first_pos, uint64_t last_pos)
{
	if (first_pos > last_pos)
		return false;
	assert(last_pos < (dev->size_byte >> dev->block_order));
	return dev->write_blocks(dev, buf, first_pos, last_pos);
}

int dev_reset(struct device *dev)
{
	return dev->reset ? dev->reset(dev) : 0;
}

void free_device(struct device *dev)
{
	if (dev->free)
		dev->free(dev);
	free(dev);
}
