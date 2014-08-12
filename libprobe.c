#define _FILE_OFFSET_BITS 64

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <err.h>

#include "libprobe.h"

static const char const *ftype_to_name[] = {"good", "limbo", "wraparound"};

const char *fake_type_to_name(enum fake_type fake_type)
{
	return ftype_to_name[fake_type];
}

#define BLOCK_SIZE	(1 <<  9)
#define GIGABYTE	(1 << 30)

struct device {
	int (*read_block)(struct device *dev, char *buf, uint64_t block);
	int (*write_block)(struct device *dev, char *buf, uint64_t block);
	int (*get_size_gb)(struct device *dev);
	void (*free)(struct device *dev);
};

struct file_device {
	/* This must be the first field. See dev_fdev() for details. */
	struct device dev;

	const char *filename;
	int fd;
	int file_size_gb;
	int fake_size_gb;
	enum fake_type fake_type;
	/* 3 free bytes. */
};

static inline struct file_device *dev_fdev(struct device *dev)
{
	return (struct file_device *)dev;
}

static int fdev_read_block(struct device *dev, char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = block * BLOCK_SIZE;

	switch (fdev->fake_type) {
	case FKTY_LIMBO:
		if (offset >= GIGABYTE * fdev->file_size_gb) {
			/* XXX Support different types of LIMBO.
			 * For example: all zeros, all ones, and random.
			 */
			memset(buf, 0, BLOCK_SIZE);
			return 0;
		}
		break;

	/* XXX Support FKTY_TRUNCATE.
	 * That is, it drops the highest bits, and addresses the real memory
	 * with the resulting address.
	 *
	 * If @fake_size_gb % @file_size_gb == 0, it's identical to
	 * FKTY_WRAPAROUND.
	 */

	case FKTY_WRAPAROUND:
		offset %= GIGABYTE * fdev->file_size_gb;
		/* Fall through. */

	case  FKTY_GOOD:
		break;

	default:
		assert(0);
	}

	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);
	read(fdev->fd, buf, BLOCK_SIZE); /* TODO */
	return 0;
}

static int fdev_write_block(struct device *dev, char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = block * BLOCK_SIZE;

	switch (fdev->fake_type) {
	case FKTY_LIMBO:
		if (offset >= GIGABYTE * fdev->file_size_gb)
			return 0;
		break;

	case FKTY_WRAPAROUND:
		offset %= GIGABYTE * fdev->file_size_gb;
		/* Fall through. */

	case  FKTY_GOOD:
		break;

	default:
		assert(0);
	}

	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);
	write(fdev->fd, buf, BLOCK_SIZE); /* TODO */
	return 0;
}

static int fdev_get_size_gb(struct device *dev)
{
	return dev_fdev(dev)->fake_size_gb;
}

static void fdev_free(struct device *dev)
{
	struct file_device *fdev = dev_fdev(dev);
	assert(!close(fdev->fd));
	assert(!unlink(fdev->filename));
	free((void *)fdev->filename);
	fdev->filename = NULL;
}

static char *strdup(const char *str)
{
	char *new = malloc(strlen(str) + 1);
	if (!new)
		return NULL;
	return strcpy(new, str);
}

struct device *create_file_device(const char *filename,
	int file_size_gb, int fake_size_gb, enum fake_type fake_type)
{
	struct file_device *fdev = malloc(sizeof(*fdev));
	assert(fdev);

	fdev->filename = strdup(filename);
	if (!fdev->filename)
		goto error;

	fdev->fd = open(filename, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fdev->fd < 0) {
		err(errno, "Can't create file `%s'", filename);
		goto filename;
	}

	fdev->file_size_gb = file_size_gb;
	fdev->fake_size_gb = fake_size_gb;
	fdev->fake_type = fake_type;

	fdev->dev.read_block = fdev_read_block;
	fdev->dev.write_block = fdev_write_block;
	fdev->dev.get_size_gb = fdev_get_size_gb;
	fdev->dev.free = fdev_free;

	return &fdev->dev;

filename:
	free((void *)fdev->filename);
	fdev->filename = NULL;
error:
	return NULL;
}

/* XXX Test if it's a device, or a partition.
 * If a partition, warn user, and ask for confirmation before
 * going ahead.
 * Suggest how to call f3probe with the correct device name if
 * the block device is a partition.
 */
/* XXX Test for write access of the block device to give
 * a nice error message.
 * If it fails, suggest running f3probe as root.
 */
struct device *create_block_device(const char *filename)
{
	/* TODO */
	return NULL;
}

void free_device(struct device *dev)
{
	dev->free(dev);
	free(dev);
}

/* XXX Don't write at the very beginning of the card to avoid
 * losing the partition table.
 * But write at a random locations to make harder for fake chips
 * to become "smarter".
 */
/* XXX Write random data for testing.
 * There would be a random seed, and all the other blocks would be
 * this seed XOR'd with the number of the test.
 */
/* XXX Finish testing the last block, and the next one that should fail.
 * Then report the last block, so user can create the largest partition.
 */
enum fake_type test_device(struct device *dev, int *preal_size_gb)
{
	/* TODO */
	*preal_size_gb = 0;
	return FKTY_GOOD;
}
