#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <err.h>
#include <inttypes.h>

#include "libdevs.h"
#include "devices/safe_device.h"
#include "libutils.h"

#define SDEV_BITMAP_WORD long
#define SDEV_BITMAP_BITS_PER_WORD (8 * sizeof(SDEV_BITMAP_WORD))

struct safe_device {
	/* This must be the first field. See dev_sdev() for details. */
	struct device		dev;

	struct device		*shadow_dev;

	char			*saved_blocks;
	uint64_t		*sb_positions;
	SDEV_BITMAP_WORD	*sb_bitmap;
	uint64_t		sb_n;
	uint64_t		sb_max;
};

static inline struct safe_device *dev_sdev(struct device *dev)
{
	return (struct safe_device *)dev;
}

static int sdev_read_blocks(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct safe_device *sdev = dev_sdev(dev);
	return sdev->shadow_dev->read_blocks(sdev->shadow_dev, buf,
		first_pos, last_pos);
}

static int sdev_is_block_saved(struct safe_device *sdev, uint64_t pos)
{
	lldiv_t idx;
	SDEV_BITMAP_WORD set_bit;

	if (!sdev->sb_bitmap) {
		uint64_t i;
		/* Running without bitmap. */
		for (i = 0; i < sdev->sb_n; i++)
			if (sdev->sb_positions[i] == pos) {
				/* The block is already saved. */
				return true;
			}
		return false;
	}

	idx = lldiv(pos, SDEV_BITMAP_BITS_PER_WORD);
	set_bit = (SDEV_BITMAP_WORD)1 << idx.rem;
	return !!(sdev->sb_bitmap[idx.quot] & set_bit);
}

static void sdev_mark_blocks(struct safe_device *sdev,
		uint64_t first_pos, uint64_t last_pos)
{
	uint64_t pos;

	for (pos = first_pos; pos <= last_pos; pos++) {
		if (sdev->sb_bitmap) {
			lldiv_t idx = lldiv(pos, SDEV_BITMAP_BITS_PER_WORD);
			SDEV_BITMAP_WORD set_bit = (SDEV_BITMAP_WORD)1 <<
				idx.rem;
			sdev->sb_bitmap[idx.quot] |= set_bit;
		}
		sdev->sb_positions[sdev->sb_n] = pos;
		sdev->sb_n++;
	}
}

/* Load blocks into cache. */
static int sdev_load_blocks(struct safe_device *sdev,
		uint64_t first_pos, uint64_t last_pos)
{
	const int block_order = dev_get_block_order(sdev->shadow_dev);
	char *block_buf = (char *)align_mem(sdev->saved_blocks, block_order) +
		(sdev->sb_n << block_order);
	int rc;

	assert(sdev->sb_n + (last_pos - first_pos + 1) < sdev->sb_max);

	rc = sdev->shadow_dev->read_blocks(sdev->shadow_dev, block_buf,
		first_pos, last_pos);
	if (rc)
		return rc;

	/* Bookkeeping. */
	sdev_mark_blocks(sdev, first_pos, last_pos);
	return 0;
}

static int sdev_save_block(struct safe_device *sdev,
		uint64_t first_pos, uint64_t last_pos)
{
	uint64_t pos, start_pos;
	int rc;

	start_pos = first_pos;
	for (pos = first_pos; pos <= last_pos; pos++) {
		if (sdev_is_block_saved(sdev, pos)) {
			if (start_pos < pos) {
				/* The blocks haven't been saved before.
				 * Save them now.
				 */
				rc = sdev_load_blocks(sdev, start_pos, pos - 1);
				if (rc)
					return rc;
			} else if (start_pos == pos) {
				/* Do nothing. */
			} else {
				assert(0);
			}
			start_pos = pos + 1;
		}
	}

	if (start_pos <= last_pos) {
		rc = sdev_load_blocks(sdev, start_pos, last_pos);
		if (rc)
			return rc;
	}

	return 0;
}

static int sdev_write_blocks(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct safe_device *sdev = dev_sdev(dev);
	int rc = sdev_save_block(sdev, first_pos, last_pos);

	if (rc)
		return rc;

	return sdev->shadow_dev->write_blocks(sdev->shadow_dev, buf,
		first_pos, last_pos);
}

static int sdev_reset(struct device *dev)
{
	return dev_reset(dev_sdev(dev)->shadow_dev);
}

static void sdev_carefully_recover(struct safe_device *sdev, char *buffer,
		uint64_t first_pos, uint64_t last_pos)
{
	const int block_size = dev_get_block_size(sdev->shadow_dev);
	uint64_t pos;
	int rc = sdev->shadow_dev->write_blocks(sdev->shadow_dev,
		buffer, first_pos, last_pos);
	if (!rc)
		return;

	for (pos = first_pos; pos <= last_pos; pos++) {
		int rc = sdev->shadow_dev->write_blocks(sdev->shadow_dev,
			buffer, pos, pos);
		if (rc) {
			/* Do not abort, try to recover all bocks. */
			warn("Failed to recover block 0x%" PRIx64
				" due to a write error", pos);
		}
		buffer += block_size;
	}
}

static uint64_t sdev_bitmap_length(struct device *dev)
{
	const int block_order = dev_get_block_order(dev);
	lldiv_t idx = lldiv(dev_get_size_byte(dev) >> block_order,
		SDEV_BITMAP_BITS_PER_WORD);
	return (idx.quot + (idx.rem ? 1 : 0)) * sizeof(SDEV_BITMAP_WORD);
}

void sdev_recover(struct device *dev, uint64_t very_last_pos)
{
	struct safe_device *sdev = dev_sdev(dev);
	const int block_order = dev_get_block_order(sdev->shadow_dev);
	char *first_block = align_mem(sdev->saved_blocks, block_order);
	uint64_t i, first_pos, last_pos;
	char *start_buf;
	int has_seq;

	has_seq = false;
	for (i = 0; i < sdev->sb_n; i++) {
		uint64_t pos = sdev->sb_positions[i];

		if (!has_seq) {
			if (pos > very_last_pos)
				continue;

			last_pos = first_pos = pos;
			start_buf = first_block + (i << block_order);
			has_seq = true;
			continue;
		}

		if (pos <= very_last_pos && pos == last_pos + 1) {
			last_pos++;
			continue;
		}

		sdev_carefully_recover(sdev, start_buf, first_pos, last_pos);

		has_seq = pos <= very_last_pos;
		if (has_seq) {
			last_pos = first_pos = pos;
			start_buf = first_block + (i << block_order);
		}
	}

	if (has_seq) {
		sdev_carefully_recover(sdev, start_buf, first_pos, last_pos);
		has_seq = false;
	}
}

void sdev_flush(struct device *dev)
{
	struct safe_device *sdev = dev_sdev(dev);

	if (sdev->sb_n <= 0)
		return;

	sdev->sb_n = 0;

	if (sdev->sb_bitmap)
		memset(sdev->sb_bitmap, 0,
			sdev_bitmap_length(sdev->shadow_dev));
}

static void sdev_free(struct device *dev)
{
	struct safe_device *sdev = dev_sdev(dev);

	sdev_recover(dev, UINT_LEAST64_MAX);
	sdev_flush(dev);

	free(sdev->sb_bitmap);
	free(sdev->sb_positions);
	free(sdev->saved_blocks);
	free_device(sdev->shadow_dev);
}

static const char *sdev_get_filename(struct device *dev)
{
	return dev_get_filename(dev_sdev(dev)->shadow_dev);
}

struct device *create_safe_device(struct device *dev, uint64_t max_blocks,
	int min_memory)
{
	struct safe_device *sdev;
	const int block_order = dev_get_block_order(dev);
	uint64_t length;

	sdev = malloc(sizeof(*sdev));
	if (!sdev)
		goto error;

	length = align_head(block_order) + (max_blocks << block_order);
	sdev->saved_blocks = malloc(length);
	if (!sdev->saved_blocks)
		goto sdev;

	sdev->sb_positions = malloc(max_blocks * sizeof(*sdev->sb_positions));
	if (!sdev->sb_positions)
		goto saved_blocks;

	if (!min_memory) {
		length = sdev_bitmap_length(dev);
		sdev->sb_bitmap = malloc(length);
		if (!sdev->sb_bitmap)
			goto offsets;
		memset(sdev->sb_bitmap, 0, length);
	} else {
		sdev->sb_bitmap = NULL;
	}

	sdev->shadow_dev = dev;
	sdev->sb_n = 0;
	sdev->sb_max = max_blocks;

	sdev->dev.size_byte = dev->size_byte;
	sdev->dev.block_order = block_order;
	sdev->dev.read_blocks = sdev_read_blocks;
	sdev->dev.write_blocks = sdev_write_blocks;
	sdev->dev.reset	= sdev_reset;
	sdev->dev.free = sdev_free;
	sdev->dev.get_filename = sdev_get_filename;

	return &sdev->dev;

offsets:
	free(sdev->sb_positions);
saved_blocks:
	free(sdev->saved_blocks);
sdev:
	free(sdev);
error:
	return NULL;
}
