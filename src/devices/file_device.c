#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <assert.h>

#include "libdevs.h"
#include "devices/file_device.h"
#include "libutils.h"

struct file_device {
	/* This must be the first field. See dev_fdev() for details. */
	struct device dev;

	const char	*filename;
	int		fd;
	uint64_t	real_size_byte;
	uint64_t	address_mask;
	uint64_t	cache_mask;
	uint64_t	*cache_entries;
	char		*cache_blocks;
};

static inline struct file_device *dev_fdev(struct device *dev)
{
    return (struct file_device *)dev;
}

static int fdev_read_block(struct device *dev, char *buf, uint64_t block_pos)
{
	struct file_device *fdev = dev_fdev(dev);
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	off_t off_ret, offset = block_pos << block_order;
	int done;

	offset &= fdev->address_mask;
	if ((uint64_t)offset >= fdev->real_size_byte) {
		uint64_t cache_pos;

		if (!fdev->cache_blocks)
			goto no_block; /* No cache available. */

		cache_pos = block_pos & fdev->cache_mask;

		if (fdev->cache_entries &&
			fdev->cache_entries[cache_pos] != block_pos)
			goto no_block;

		memmove(buf, &fdev->cache_blocks[cache_pos << block_order],
			block_size);
		return 0;
	}

	off_ret = lseek(fdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert(off_ret == offset);

	done = 0;
	do {
		ssize_t rc = read(fdev->fd, buf + done, block_size - done);
		assert(rc >= 0);
		if (!rc) {
			/* Tried to read beyond the end of the file. */
			assert(!done);
			memset(buf, 0, block_size);
			done += block_size;
		}
		done += rc;
	} while (done < block_size);

	return 0;

no_block:
	memset(buf, 0, block_size);
	return 0;
}

static int fdev_read_blocks(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t pos;

	for (pos = first_pos; pos <= last_pos; pos++) {
		int rc = fdev_read_block(dev, buf, pos);
		if (rc)
			return rc;
		buf += block_size;
	}
	return 0;
}

static int write_all(int fd, const char *buf, size_t count)
{
	size_t done = 0;
	do {
		ssize_t rc = write(fd, buf + done, count - done);
		if (rc < 0) {
			/* The write() failed. */
			return errno;
		}
		done += rc;
	} while (done < count);
	return 0;
}

static int fdev_write_block(struct device *dev, const char *buf,
	uint64_t block_pos)
{
	struct file_device *fdev = dev_fdev(dev);
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	off_t off_ret, offset = block_pos << block_order;

	offset &= fdev->address_mask;
	if ((uint64_t)offset >= fdev->real_size_byte) {
		/* Block beyond real memory. */
		uint64_t cache_pos;

		if (!fdev->cache_blocks)
			return 0; /* No cache available. */
		cache_pos = block_pos & fdev->cache_mask;
		memmove(&fdev->cache_blocks[cache_pos << block_order],
			buf, block_size);

		if (fdev->cache_entries)
			fdev->cache_entries[cache_pos] = block_pos;

		return 0;
	}

	off_ret = lseek(fdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert(off_ret == offset);

	return write_all(fdev->fd, buf, block_size);
}

static int fdev_write_blocks(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t pos;

	for (pos = first_pos; pos <= last_pos; pos++) {
		int rc = fdev_write_block(dev, buf, pos);
		if (rc)
			return rc;
		buf += block_size;
	}
	return 0;
}

static void fdev_free(struct device *dev)
{
	struct file_device *fdev = dev_fdev(dev);
	free(fdev->cache_blocks);
	free(fdev->cache_entries);
	free((void *)fdev->filename);
	assert(!close(fdev->fd));
}

static const char *fdev_get_filename(struct device *dev)
{
	return dev_fdev(dev)->filename;
}

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap,
	int block_order, int cache_order, int strict_cache,
	int keep_file)
{
	struct file_device *fdev;

	fdev = malloc(sizeof(*fdev));
	if (!fdev)
		goto error;

	fdev->filename = strdup(filename);
	if (!fdev->filename)
		goto fdev;

	fdev->cache_mask = 0;
	fdev->cache_entries = NULL;
	fdev->cache_blocks = NULL;
	if (cache_order >= 0) {
		fdev->cache_mask = (((uint64_t)1) << cache_order) - 1;
		if (strict_cache) {
			size_t size = sizeof(*fdev->cache_entries) <<
				cache_order;
			fdev->cache_entries = malloc(size);
			if (!fdev->cache_entries)
				goto cache;
			memset(fdev->cache_entries, 0, size);
		}
		fdev->cache_blocks = malloc(((uint64_t)1) <<
			(cache_order + block_order));
		if (!fdev->cache_blocks)
			goto cache;
	}

	fdev->fd = open(filename, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fdev->fd < 0) {
		err(errno, "Can't create file `%s'", filename);
		goto cache;
	}
	if (!keep_file) {
		/* Unlinking the file now guarantees that it won't exist if
		 * there is a crash.
		 */
		assert(!unlink(filename));
	}

	if (!block_order) {
		struct stat fd_stat;
		blksize_t block_size;
		assert(!fstat(fdev->fd, &fd_stat));
		block_size = fd_stat.st_blksize;
		block_order = ilog2(block_size);
		assert(block_size == (1 << block_order));
	}

	if (!dev_param_valid(real_size_byte, fake_size_byte, wrap, block_order))
		goto keep_file;

	fdev->real_size_byte = real_size_byte;
	fdev->address_mask = (((uint64_t)1) << wrap) - 1;

	fdev->dev.size_byte = fake_size_byte;
	fdev->dev.block_order = block_order;
	fdev->dev.read_blocks = fdev_read_blocks;
	fdev->dev.write_blocks = fdev_write_blocks;
	fdev->dev.reset = NULL;
	fdev->dev.free = fdev_free;
	fdev->dev.get_filename = fdev_get_filename;

	return &fdev->dev;

keep_file:
	if (keep_file)
		unlink(filename);
	assert(!close(fdev->fd));
cache:
	free(fdev->cache_blocks);
	free(fdev->cache_entries);
/* filename:	this label is not being used. */
	free((void *)fdev->filename);
fdev:
	free(fdev);
error:
	return NULL;
}
