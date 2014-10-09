#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <err.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#include <libudev.h>

#include "libprobe.h"

static const char const *ftype_to_name[FKTY_MAX] = {
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

struct device {
	uint64_t	size_byte;
	int		block_order;

	int (*read_block)(struct device *dev, char *buf, int length,
		uint64_t offset);
	int (*write_block)(struct device *dev, const char *buf, int length,
		uint64_t offset);
	int (*reset)(struct device *dev);
	void (*free)(struct device *dev);
};

static inline uint64_t dev_get_size_byte(struct device *dev)
{
	return dev->size_byte;
}

static inline int dev_get_block_order(struct device *dev)
{
	return dev->block_order;
}

static inline int dev_get_block_size(struct device *dev)
{
	return 1 << dev->block_order;
}

static inline int dev_read_block(struct device *dev, char *buf, uint64_t block)
{
	const int block_size = 1 << dev->block_order;
	uint64_t offset = block << dev->block_order;
	assert(offset + block_size <= dev->size_byte);
	return dev->read_block(dev, buf, block_size, offset);
}

static inline int dev_write_block(struct device *dev, const char *buf,
	uint64_t block)
{
	const int block_size = 1 << dev->block_order;
	uint64_t offset = block << dev->block_order;
	assert(offset + block_size <= dev->size_byte);
	return dev->write_block(dev, buf, block_size, offset);
}

static inline int dev_reset(struct device *dev)
{
	return dev->reset ? dev->reset(dev) : 0;
}

static inline int dev_write_and_reset(struct device *dev, const char *buf,
	uint64_t block)
{
	int rc = dev_write_block(dev, buf, block);
	return rc ? rc : dev_reset(dev);
}

void free_device(struct device *dev)
{
	if (dev->free)
		dev->free(dev);
	free(dev);
}

struct file_device {
	/* This must be the first field. See dev_fdev() for details. */
	struct device dev;

	int fd;
	uint64_t real_size_byte;
	uint64_t address_mask;
};

static inline struct file_device *dev_fdev(struct device *dev)
{
	return (struct file_device *)dev;
}

static int fdev_read_block(struct device *dev, char *buf, int length,
	uint64_t offset)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t off_ret;
	int done;

	offset &= fdev->address_mask;
	if (offset >= fdev->real_size_byte) {
		memset(buf, 0, length);
		return 0;
	}

	off_ret = lseek(fdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert((uint64_t)off_ret == offset);

	done = 0;
	do {
		ssize_t rc = read(fdev->fd, buf + done, length - done);
		assert(rc >= 0);
		if (!rc) {
			/* Tried to read beyond the end of the file. */
			assert(!done);
			memset(buf, 0, length);
			done += length;
		}
		done += rc;
	} while (done < length);

	return 0;
}

static int write_all(int fd, const char *buf, int count)
{
	int done = 0;
	do {
		ssize_t rc = write(fd, buf + done, count - done);
		assert(rc >= 0); /* Did the write() went right? */
		done += rc;
	} while (done < count);
	return 0;
}

static int fdev_write_block(struct device *dev, const char *buf, int length,
	uint64_t offset)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t off_ret;

	offset &= fdev->address_mask;
	if (offset >= fdev->real_size_byte)
		return 0;

	off_ret = lseek(fdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert((uint64_t)off_ret == offset);

	return write_all(fdev->fd, buf, length);
}

static void fdev_free(struct device *dev)
{
	struct file_device *fdev = dev_fdev(dev);
	assert(!close(fdev->fd));
}

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap,
	int block_order, int keep_file)
{
	struct file_device *fdev;

	if (!dev_param_valid(real_size_byte, fake_size_byte, wrap, block_order))
		goto error;

	fdev = malloc(sizeof(*fdev));
	if (!fdev)
		goto error;

	fdev->fd = open(filename, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fdev->fd < 0) {
		err(errno, "Can't create file `%s'", filename);
		goto fdev;
	}
	if (!keep_file) {
		/* Unlinking the file now guarantees that it won't exist if
		 * there is a crash.
		 */
		assert(!unlink(filename));
	}

	fdev->real_size_byte = real_size_byte;
	fdev->address_mask = (((uint64_t)1) << wrap) - 1;

	fdev->dev.size_byte = fake_size_byte;
	fdev->dev.block_order = block_order;
	fdev->dev.read_block = fdev_read_block;
	fdev->dev.write_block = fdev_write_block;
	fdev->dev.reset = NULL;
	fdev->dev.free = fdev_free;

	return &fdev->dev;

fdev:
	free(fdev);
error:
	return NULL;
}

struct block_device {
	/* This must be the first field. See dev_bdev() for details. */
	struct device dev;

	const char *filename;
	int fd;
	int hw_fd; /* Underlying hardware of the block device. */
};

static inline struct block_device *dev_bdev(struct device *dev)
{
	return (struct block_device *)dev;
}

static int read_all(int fd, char *buf, int count)
{
	int done = 0;
	do {
		ssize_t rc = read(fd, buf + done, count - done);
		assert(rc >= 0); /* Did the read() went right? */
		assert(rc != 0); /* We should never hit the end of the file. */
		done += rc;
	} while (done < count);
	return 0;
}

static int bdev_read_block(struct device *dev, char *buf, int length,
	uint64_t offset)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t off_ret = lseek(bdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert((uint64_t)off_ret == offset);
	return read_all(bdev->fd, buf, length);
}

static int bdev_write_block(struct device *dev, const char *buf, int length,
	uint64_t offset)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t off_ret = lseek(bdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert((uint64_t)off_ret == offset);
	return write_all(bdev->fd, buf, length);
}

static inline int bdev_open(const char *filename)
{
	return open(filename, O_RDWR | O_DIRECT | O_SYNC);
}

/* XXX Monitor the USB subsytem to know when the drive was unplugged and
 * plugged back to continue instead of waiting for a key.
 */
static int bdev_manual_reset(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	assert(!close(bdev->fd));
	printf("Please unplug and plug back the USB drive, and press a key to continue...\n");
	getchar();
	bdev->fd = bdev_open(bdev->filename);
	if (bdev->fd < 0)
		err(errno, "Can't REopen device `%s'", bdev->filename);
	return 0;
}

static int bdev_reset(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	assert(!close(bdev->fd));
	assert(!ioctl(bdev->hw_fd, USBDEVFS_RESET));
	bdev->fd = bdev_open(bdev->filename);
	if (bdev->fd < 0)
		err(errno, "Can't REopen device `%s'", bdev->filename);
	return 0;
}

static void bdev_free(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	if (bdev->hw_fd >= 0)
		assert(!close(bdev->hw_fd));
	if (bdev->fd >= 0)
		assert(!close(bdev->fd));
	free((void *)bdev->filename);
}

static bool is_block_dev(int fd)
{
	struct stat stat;
	assert(!fstat(fd, &stat));
	return S_ISBLK(stat.st_mode);
}

static char *map_block_to_usb_dev(const char *block_dev)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	char *usb_dev_path = NULL;

	udev = udev_new();
	if (!udev)
		err(errno, "Can't create udev");

	/* XXX Avoid the enumeration using udev_device_new_from_devnum(). */
	enumerate = udev_enumerate_new(udev);
	assert(enumerate);
	assert(!udev_enumerate_add_match_subsystem(enumerate, "block"));
	assert(!udev_enumerate_scan_devices(enumerate));
	devices = udev_enumerate_get_list_entry(enumerate);
	assert(devices);

	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *sys_path, *dev_path;
		struct udev_device *dev, *parent_dev;

		/* Get the filename of the /sys entry for the device,
		 * and create a udev_device object (dev) representing it.
		 */
		sys_path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, sys_path);

		/* usb_device_get_devnode() returns the path to
		 * the device node itself in /dev.
		 */
		dev_path = udev_device_get_devnode(dev);
		if (strcmp(block_dev, dev_path)) {
			assert(!udev_device_unref(dev));
			continue;
		}

		/* The device pointed to by dev contains information about
		 * the USB device.
		 * In order to get information about the USB device,
		 * get the parent device with the subsystem/devtype pair of
		 * "usb"/"usb_device".
		 * This will be several levels up the tree,
		 * but the function will find it.
		 */
		parent_dev = udev_device_get_parent_with_subsystem_devtype(
			dev, "usb", "usb_device");
		if (!parent_dev)
			err(errno, "Unable to find parent usb device of `%s'",
				block_dev);

		usb_dev_path = strdup(udev_device_get_devnode(parent_dev));
		/* @parent_dev is not referenced, and will be freed when
		 * the child (i.e. @dev) is freed.
		 * See udev_device_get_parent_with_subsystem_devtype() for
		 * details.
		 */
		assert(!udev_device_unref(dev));
		break;
	}
	/* Free the enumerator object. */
	assert(!udev_enumerate_unref(enumerate));

	assert(!udev_unref(udev));
	return usb_dev_path;
}

/* Count the number of 1 bits. */
static int pop(uint64_t x)
{
	int n = 0;
	while (x) {
		n++;
		x = x & (x - 1);
	}
	return n;
}

static int ilog2(uint64_t x)
{
	x = x | (x >>  1);
	x = x | (x >>  2);
	x = x | (x >>  4);
	x = x | (x >>  8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	return pop(x) - 1;
}

/* XXX Test if it's a device, or a partition.
 * If a partition, warn user, and ask for confirmation before
 * going ahead.
 * Suggest how to call f3probe with the correct device name if
 * the block device is a partition.
 * Use udev to do these tests.
 * Make sure that no partition of the drive is mounted.
 */
/* XXX Test for write access of the block device to give
 * a nice error message.
 * If it fails, suggest running f3probe as root.
 */
struct device *create_block_device(const char *filename, int manual_reset)
{
	struct block_device *bdev;
	int block_size;

	bdev = malloc(sizeof(*bdev));
	if (!bdev)
		goto error;

	bdev->filename = strdup(filename);
	if (!bdev->filename)
		goto bdev;

	bdev->fd = bdev_open(filename);
	if (bdev->fd < 0) {
		err(errno, "Can't open device `%s'", filename);
		goto filename;
	}

	if (!is_block_dev(bdev->fd)) {
		err(EINVAL, "File `%s' is not a block device", filename);
		goto fd;
	}

	if (manual_reset) {
		bdev->hw_fd = -1;
		bdev->dev.reset	= bdev_manual_reset;
	} else {
		/* XXX Add support for block devices backed by SCSI and ATA. */
		const char *usb_filename = map_block_to_usb_dev(filename);
		if (!usb_filename) {
			err(EINVAL, "Block device `%s' is not backed by a USB device",
				filename);
			goto fd;
		}

		bdev->hw_fd = open(usb_filename, O_WRONLY | O_NONBLOCK);
		free((void *)usb_filename);
		if (bdev->hw_fd < 0) {
			err(errno, "Can't open device `%s'", usb_filename);
			goto fd;
		}
		bdev->dev.reset = bdev_reset;
	}

	assert(!ioctl(bdev->fd, BLKGETSIZE64, &bdev->dev.size_byte));

	assert(!ioctl(bdev->fd, BLKBSZGET, &block_size));
	bdev->dev.block_order = ilog2(block_size);
	assert(block_size == (1 << bdev->dev.block_order));

	bdev->dev.read_block = bdev_read_block;
	bdev->dev.write_block = bdev_write_block;
	bdev->dev.free = bdev_free;

	return &bdev->dev;

fd:
	assert(!close(bdev->fd));
filename:
	free((void *)bdev->filename);
bdev:
	free(bdev);
error:
	return NULL;
}

struct perf_device {
	/* This must be the first field. See dev_pdev() for details. */
	struct device		dev;

	struct device		*shadow_dev;

	uint64_t		read_count;
	uint64_t		read_time_us;
	uint64_t		write_count;
	uint64_t		write_time_us;
	uint64_t		reset_count;
	uint64_t		reset_time_us;
};

static inline struct perf_device *dev_pdev(struct device *dev)
{
	return (struct perf_device *)dev;
}

static inline uint64_t diff_timeval_us(const struct timeval *t1,
	const struct timeval *t2)
{
	return (t2->tv_sec - t1->tv_sec) * 1000000ULL +
		t2->tv_usec - t1->tv_usec;
}

static int pdev_read_block(struct device *dev, char *buf, int length,
	uint64_t offset)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct timeval t1, t2;
	int rc;

	assert(!gettimeofday(&t1, NULL));
	rc = pdev->shadow_dev->read_block(pdev->shadow_dev, buf,
		length, offset);
	assert(!gettimeofday(&t2, NULL));
	pdev->read_count++;
	pdev->read_time_us += diff_timeval_us(&t1, &t2);
	return rc;
}

static int pdev_write_block(struct device *dev, const char *buf, int length,
	uint64_t offset)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct timeval t1, t2;
	int rc;

	assert(!gettimeofday(&t1, NULL));
	rc = pdev->shadow_dev->write_block(pdev->shadow_dev, buf,
		length, offset);
	assert(!gettimeofday(&t2, NULL));
	pdev->write_count++;
	pdev->write_time_us += diff_timeval_us(&t1, &t2);
	return rc;
}

static int pdev_reset(struct device *dev)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct timeval t1, t2;
	int rc;

	assert(!gettimeofday(&t1, NULL));
	rc = dev_reset(pdev->shadow_dev);
	assert(!gettimeofday(&t2, NULL));
	pdev->reset_count++;
	pdev->reset_time_us += diff_timeval_us(&t1, &t2);
	return rc;
}

static void pdev_free(struct device *dev)
{
	struct perf_device *pdev = dev_pdev(dev);
	free_device(pdev->shadow_dev);
}

/* Detach the shadow device of @pdev, free @pdev, and return
 * the shadow device.
 */
static struct device *pdev_detach_and_free(struct device *dev)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct device *shadow_dev = pdev->shadow_dev;
	pdev->shadow_dev = NULL;
	pdev->dev.free = NULL;
	free_device(&pdev->dev);
	return shadow_dev;
}

struct device *create_perf_device(struct device *dev)
{
	struct perf_device *pdev;

	pdev = malloc(sizeof(*pdev));
	if (!pdev)
		return NULL;

	pdev->shadow_dev = dev;
	pdev->read_count = 0;
	pdev->read_time_us = 0;
	pdev->write_count = 0;
	pdev->write_time_us = 0;
	pdev->reset_count = 0;
	pdev->reset_time_us = 0;

	pdev->dev.size_byte = dev->size_byte;
	pdev->dev.block_order = dev->block_order;
	pdev->dev.read_block = pdev_read_block;
	pdev->dev.write_block = pdev_write_block;
	pdev->dev.reset	= pdev_reset;
	pdev->dev.free = pdev_free;

	return &pdev->dev;
}

void perf_device_sample(struct device *dev,
	uint64_t *pread_count, uint64_t *pread_time_us,
	uint64_t *pwrite_count, uint64_t *pwrite_time_us,
	uint64_t *preset_count, uint64_t *preset_time_us)
{
	struct perf_device *pdev = dev_pdev(dev);

	if (pread_count)
		*pread_count = pdev->read_count;
	if (pread_time_us)
		*pread_time_us = pdev->read_time_us;

	if (pwrite_count)
		*pwrite_count = pdev->write_count;
	if (pwrite_time_us)
		*pwrite_time_us = pdev->write_time_us;

	if (preset_count)
		*preset_count = pdev->reset_count;
	if (preset_time_us)
		*preset_time_us = pdev->reset_time_us;
}

#define SDEV_BITMAP_WORD		long
#define SDEV_BITMAP_BITS_PER_WORD	(8*sizeof(SDEV_BITMAP_WORD))
struct safe_device {
	/* This must be the first field. See dev_sdev() for details. */
	struct device		dev;

	struct device		*shadow_dev;

	char			*saved_blocks;
	uint64_t		*sb_offsets;
	SDEV_BITMAP_WORD	*sb_bitmap;
	int			sb_n;
	int			sb_max;
};

static inline struct safe_device *dev_sdev(struct device *dev)
{
	return (struct safe_device *)dev;
}

static int sdev_read_block(struct device *dev, char *buf, int length,
	uint64_t offset)
{
	struct safe_device *sdev = dev_sdev(dev);
	return sdev->shadow_dev->read_block(sdev->shadow_dev, buf,
		length, offset);
}

static inline void *align_512(void *p)
{
	uintptr_t ip = (uintptr_t)p;
	return (void *)(   (ip + 511) & ~511   );
}

static int sdev_save_block(struct safe_device *sdev,
	int length, uint64_t offset)
{
	const int block_order = dev_get_block_order(sdev->shadow_dev);
	lldiv_t idx = lldiv(offset >> block_order, SDEV_BITMAP_BITS_PER_WORD);
	SDEV_BITMAP_WORD set_bit = (SDEV_BITMAP_WORD)1 << idx.rem;
	char *block;
	int rc;

	/* The current implementation doesn't support variable lengths. */
	assert(length == dev_get_block_size(sdev->shadow_dev));

	/* Is this block already saved? */
	if (!sdev->sb_bitmap) {
		int i;
		/* Running without bitmap. */
		for (i = 0; i < sdev->sb_n; i++)
			if (sdev->sb_offsets[i] == offset) {
				/* The block at @offset is already saved. */
				return 0;
			}
	} else if (sdev->sb_bitmap[idx.quot] & set_bit) {
		/* The block at @offset is already saved. */
		return 0;
	}

	/* The block at @offset hasn't been saved before. Save this block. */
	assert(sdev->sb_n < sdev->sb_max);
	block = (char *)align_512(sdev->saved_blocks) +
		(sdev->sb_n << block_order);
	rc = sdev->shadow_dev->read_block(sdev->shadow_dev, block,
		length, offset);
	if (rc)
		return rc;

	/* Bookkeeping. */
	if (sdev->sb_bitmap)
		sdev->sb_bitmap[idx.quot] |= set_bit;
	sdev->sb_offsets[sdev->sb_n] = offset;
	sdev->sb_n++;
	return 0;
}

static int sdev_write_block(struct device *dev, const char *buf, int length,
	uint64_t offset)
{
	struct safe_device *sdev = dev_sdev(dev);
	int rc;

	rc = sdev_save_block(sdev, length, offset);
	if (rc)
		return rc;

	return sdev->shadow_dev->write_block(sdev->shadow_dev, buf,
		length, offset);
}

static int sdev_reset(struct device *dev)
{
	return dev_reset(dev_sdev(dev)->shadow_dev);
}

static void sdev_free(struct device *dev)
{
	struct safe_device *sdev = dev_sdev(dev);

	if (sdev->sb_n > 0) {
		char *first_block = align_512(sdev->saved_blocks);
		char *block = first_block +
			((sdev->sb_n - 1) <<
			dev_get_block_order(sdev->shadow_dev));
		uint64_t *poffset = &sdev->sb_offsets[sdev->sb_n - 1];
		int block_size = dev_get_block_size(sdev->shadow_dev);
		int failed = false;

		/* Restore blocks in reverse order to cope with
		 * wraparound and chain drives.
		 */
		do {
			failed |= sdev->shadow_dev->write_block(
				sdev->shadow_dev, block, block_size, *poffset);
			block -= block_size;
			poffset--;
		} while (block >= first_block);
		/* Try to recover all bocks before failing. */
		assert(!failed);
	}

	free(sdev->sb_bitmap);
	free(sdev->sb_offsets);
	free(sdev->saved_blocks);
	free_device(sdev->shadow_dev);
}

struct device *create_safe_device(struct device *dev, int max_blocks,
	int min_memory)
{
	struct safe_device *sdev;
	const int block_order = dev_get_block_order(dev);
	uint64_t length;

	sdev = malloc(sizeof(*sdev));
	if (!sdev)
		goto error;

	length = 511 + (max_blocks << block_order);
	sdev->saved_blocks = malloc(length);
	if (!sdev->saved_blocks)
		goto sdev;

	sdev->sb_offsets = malloc(max_blocks * sizeof(*sdev->sb_offsets));
	if (!sdev->sb_offsets)
		goto saved_blocks;

	if (!min_memory) {
		lldiv_t idx = lldiv(dev_get_size_byte(dev) >> block_order,
			SDEV_BITMAP_BITS_PER_WORD);
		length = (idx.quot + (idx.rem ? 1 : 0)) *
			sizeof(SDEV_BITMAP_WORD);
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
	sdev->dev.read_block = sdev_read_block;
	sdev->dev.write_block = sdev_write_block;
	sdev->dev.reset	= sdev_reset;
	sdev->dev.free = sdev_free;

	return &sdev->dev;

offsets:
	free(sdev->sb_offsets);
saved_blocks:
	free(sdev->saved_blocks);
sdev:
	free(sdev);
error:
	return NULL;
}

static inline int equal_blk(struct device *dev, const char *b1, const char *b2)
{
	return !memcmp(b1, b2, dev_get_block_size(dev));
}

/* Return true if @b1 and b2 are at most @tolerance_byte bytes different. */
static int similar_blk(struct device *dev, const char *b1, const char *b2,
	int tolerance_byte)
{
	const int block_size = dev_get_block_size(dev);
	int i;

	for (i = 0; i < block_size; i++) {
		if (*b1 != *b2) {
			tolerance_byte--;
			if (tolerance_byte <= 0)
				return false;
		}
		b1++;
		b2++;
	}
	return true;
}

/* Return true if the block at @pos is damaged. */
static int test_block(struct device *dev,
	const char *stamp_blk, char *probe_blk, uint64_t pos)
{
	/* Write block. */
	if (dev_write_block(dev, stamp_blk, pos) &&
		dev_write_block(dev, stamp_blk, pos))
		return true;

	/* Reset. */
	if (dev_reset(dev) && dev_reset(dev))
		return true;

	/*
	 *	Test block.
	 */

	if (dev_read_block(dev, probe_blk, pos) &&
		dev_read_block(dev, probe_blk, pos))
		return true;

	if (equal_blk(dev, stamp_blk, probe_blk))
		return false;

	/* Save time with certainly damaged blocks. */
	if (!similar_blk(dev, stamp_blk, probe_blk, 8)) {
		/* The probe block is damaged. */
		return true;
	}

	/* The probe block seems to be damaged.
	 * Trying a second time...
	 */
	return 	dev_write_and_reset(dev, stamp_blk, pos) ||
		dev_read_block(dev, probe_blk, pos)  ||
		!equal_blk(dev, stamp_blk, probe_blk);
}

/* Minimum size of the memory chunk used to build flash drives.
 * It must be a power of two.
 */
static inline uint64_t initial_high_bit_block(struct device *dev)
{
	int block_order = dev_get_block_order(dev);
	assert(block_order <= 20);
	return 1ULL << (20 - block_order);
}

/* Caller must guarantee that the left bock is good, and written. */
static int search_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t high_bit = initial_high_bit_block(dev);
	uint64_t pos = high_bit + left_pos;

	/* The left block must be in the first memory chunk. */
	assert(left_pos < high_bit);

	/* Check that the drive has at least one memory chunk. */
	assert((high_bit - 1) <= *pright_pos);

	while (pos < *pright_pos) {
		if (dev_read_block(dev, probe_blk, pos) &&
			dev_read_block(dev, probe_blk, pos))
			return true;
		/* XXX Deal with flipped bit on reception. */
		if (equal_blk(dev, stamp_blk, probe_blk)) {
			/* XXX Test wraparound hypothesis. */
			*pright_pos = high_bit - 1;
			return false;
		}
		high_bit <<= 1;
		pos = high_bit + left_pos;
	}

	return false;
}

#define MAX_N_BLOCK_ORDER	10

static uint64_t estimate_best_n_block(struct device *dev)
{
	uint64_t write_count, write_time_us;
	uint64_t reset_count, reset_time_us;
	double t_w_us, t_2w_us, t_r_us;
	uint64_t n_block_order;

	perf_device_sample(dev, NULL, NULL, &write_count, &write_time_us,
		&reset_count, &reset_time_us);
	if (!write_count || !reset_count) {
		/* There is not enough measurements. */
		return (1 << 2) - 1;
	}

	/* Let 2^n be the total number of blocks on the drive.
	 * Let p be the total number of passes.
	 * Let w = (2^m - 1) be the number of blocks written on each pass,
	 *   where m >= 1.
	 *
	 * A pass is an iteration of the loop in search_edge(), that is,
	 * a call to write_test_blocks(), dev_reset(), and probe_test_blocks().
	 *
	 * The reason to have w = (2^m - 1) instead of w = 2^m is because
	 * the former leads to a clean relationship between n, p, and m
	 * when m is constant: 2^n / (w + 1)^p = 1 => p = n/m
	 *
	 * Let Tr be the time to reset the device.
	 * Let Tw be the time to write a block to @dev.
	 * Let Tw' be the time to write a block to the underlying device
	 *   of @dev, that is, without overhead due to chaining multiple
	 *   struct device. For example, when struct safe_device is used
	 *   Tw > Tw'.
	 * Let Trd be the time to read a block from @dev.
	 *
	 * Notice that each single-block pass reduces the search space in half,
	 * and that to reduce the search space in half writing blocks,
	 * one has to increase m of one.
	 *
	 * Thus, in order to be better writing more blocks than
	 * going for another pass, the following relation must be true:
	 *
	 * Tr + Tw + Tw' >= (w - 1)(Tw + Tw')
	 *
	 * The relation above assumes Trd = 0.
	 *
	 * The left side of the relation above is the time to do _another_
	 * pass writing a single block, whereas the right side is the time to
	 * stay in the same pass and write (w - 1) more blocks.
	 * In order words, if there is no advantage to write more blocks,
	 * we stick to single-block passes.
	 *
	 * Tw' is there to account for any operation that writes
	 * the blocks back (e.g. using struct safe_device), otherwise
	 * processing operations related per written blocks that is not
	 * being accounted for (e.g. reading the blocks back to test).
	 *
	 * Solving the relation for w: w <= Tr/(Tw + Tw') + 2
	 *
	 * However, we are not interested in any w, but only those of
	 * of the form (2^m - 1) to make sure that we are not better off
	 * calling another pass. Thus, solving the previous relation for m:
	 *
	 * m <= log_2(Tr/(Tw + Tw') + 3)
	 *
	 * We approximate Tw' making it equal to Tw.
	 */
	t_w_us = (double)write_time_us / write_count;
	t_r_us = (double)reset_time_us / reset_count;
	t_2w_us = t_w_us > 0. ? 2. * t_w_us : 1.; /* Avoid zero division. */
	n_block_order = ilog2(round(t_r_us / t_2w_us + 3.));

	/* Bound the maximum number of blocks per pass to limit
	 * the necessary amount of memory struct safe_device pre-allocates.
	 */
	if (n_block_order > MAX_N_BLOCK_ORDER)
		n_block_order = MAX_N_BLOCK_ORDER;

	return (1 << n_block_order) - 1;
}

/* Write blocks whose offsets are after @left_pos but
 * less or equal to @right_pos.
 */
static int write_test_blocks(struct device *dev, const char *stamp_blk,
	uint64_t left_pos, uint64_t right_pos,
	uint64_t *pa, uint64_t *pb, uint64_t *pmax_idx)
{
	uint64_t pos, last_pos;
	uint64_t n_block = estimate_best_n_block(dev);

	assert(n_block >= 1);

	/* Find coeficients of function a*idx + b where idx <= max_idx. */
	assert(left_pos < right_pos);
	*pb = left_pos + 1;
	*pa = round((right_pos - *pb) / (n_block + 1.));
	*pa = !*pa ? 1ULL : *pa;
	*pmax_idx = (right_pos - *pb) / *pa;
	if (*pmax_idx >= n_block) {
		/* Shift the zero of the function to the right.
		 * This avoids picking the leftmost block when a more
		 * informative block to the right is available.
		 * This also biases toward righter blocks,
		 * what improves the time to test good flash drives.
		 */
		*pb += *pa;

		*pmax_idx = n_block - 1;
	}
	last_pos = *pa * *pmax_idx + *pb;
	assert(last_pos <= right_pos);

	/* Write test blocks. */
	for (pos = *pb; pos <= last_pos; pos += *pa)
		if (dev_write_block(dev, stamp_blk, pos) &&
			dev_write_block(dev, stamp_blk, pos))
			return true;
	return false;
}

/* Return true if the test block at @pos is damaged. */
static int test_test_block(struct device *dev,
	const char *stamp_blk, char *probe_blk, uint64_t pos)
{
	if (dev_read_block(dev, probe_blk, pos) &&
		dev_read_block(dev, probe_blk, pos))
		return true;

	return !equal_blk(dev, stamp_blk, probe_blk);
}

static int probe_test_blocks(struct device *dev,
	const char *stamp_blk, char *probe_blk,
	uint64_t *pleft_pos, uint64_t *pright_pos,
	uint64_t a, uint64_t b, uint64_t max_idx)
{
	/* Signed variables. */
	int64_t left_idx = 0;
	int64_t right_idx = max_idx;
	int64_t idx = right_idx;
	while (left_idx <= right_idx) {
		uint64_t pos = a * idx + b;
		if (test_test_block(dev, stamp_blk, probe_blk, pos)) {
			right_idx = idx - 1;
			*pright_pos = pos;
		} else {
			left_idx = idx + 1;
			*pleft_pos = pos;
		}
		idx = (left_idx + right_idx) / 2;
	}
	return  false;
}

/* Caller must guarantee that the left bock is good, and written. */
static int search_edge(struct device *dev,
	uint64_t *pleft_pos, uint64_t right_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t gap = right_pos - *pleft_pos;
	uint64_t prv_gap = gap + 1;
	while (prv_gap > gap && gap >= 1) {
		uint64_t a, b, max_idx;
		if (write_test_blocks(dev, stamp_blk, *pleft_pos, right_pos,
			&a, &b, &max_idx))
			return true;
		/* Reset. */
		if (dev_reset(dev) && dev_reset(dev))
			return true;
		if (probe_test_blocks(dev, stamp_blk, probe_blk,
			pleft_pos, &right_pos, a, b, max_idx))
			return true;

		prv_gap = gap;
		gap = right_pos - *pleft_pos;
	}
	return false;
}

/* XXX Write random data to make it harder for fake chips to become "smarter".
 * There would be a random seed.
 * Buffer cannot be all 0x00 or all 0xFF.
 */
static void fill_buffer(char *buf, int len)
{
	memset(buf, 0xAA, len);
}

/* Least power of 2 greater than or equal to x. */
static uint64_t clp2(uint64_t x)
{
	x = x - 1;
	x = x | (x >>  1);
	x = x | (x >>  2);
	x = x | (x >>  4);
	x = x | (x >>  8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	return x + 1;
}

static int ceiling_log2(uint64_t x)
{
	return ilog2(clp2(x));
}

int probe_device_max_blocks(struct device *dev)
{
	uint64_t num_blocks = dev_get_size_byte(dev) >>
		dev_get_block_order(dev);
	int n = ceiling_log2(num_blocks);

	/* Make sure that there is no overflow in the formula below.
	 * The number 10 is arbitrary here, that is, it's not tight.
	 */
	assert(MAX_N_BLOCK_ORDER < sizeof(int) - 10);

	return
		/* search_wrap() */
		1 +
		/* Search_edge()
		 *
		 * The number of used blocks is (p * w); see comments in
		 * estimate_best_n_block() for the definition of the variables.
		 *
		 * p * w = n/m * (2^m - 1) < n/m * 2^m = n * (2^m / m)
		 *
		 * Let f(m) be 2^m / m. One can prove that f(m + 1) >= f(m)
		 * for all m >= 1. Therefore, the following bound is true.
		 *
		 * p * w < n * f(max_m)
		 */
		((n << MAX_N_BLOCK_ORDER) / MAX_N_BLOCK_ORDER);
}

/* XXX Properly handle read and write errors.
 * Review each assert to check if them can be removed.
 */
int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap, int *pblock_order)
{
	uint64_t dev_size_byte = dev_get_size_byte(dev);
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	char stack[511 + (2 << block_order)];
	char *stamp_blk, *probe_blk;
	/* XXX Don't write at the very beginning of the card to avoid
	 * losing the partition table.
	 * But write at a random locations to make harder for fake chips
	 * to become "smarter".
	 * And try a couple of blocks if they keep failing.
	 */
	uint64_t left_pos = 10;
	uint64_t right_pos = (dev_size_byte >> block_order) - 1;
	struct device *pdev;

	assert(dev_size_byte % block_size == 0);
	assert(left_pos < right_pos);

	pdev = create_perf_device(dev);
	if (!pdev)
		return -ENOMEM;

	/* Aligning these pointers is necessary to directly read and write
	 * the block device.
	 * For the file device, this is superfluous.
	 */
	stamp_blk = align_512(stack);
	probe_blk = stamp_blk + block_size;

	fill_buffer(stamp_blk, block_size);

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */
	if (test_block(pdev, stamp_blk, probe_blk, left_pos))
		goto bad;

	if (search_wrap(pdev, left_pos, &right_pos, stamp_blk, probe_blk))
		goto bad;

	if (search_edge(pdev, &left_pos, right_pos, stamp_blk, probe_blk))
		goto bad;

	*preal_size_byte = (left_pos + 1) << block_order;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(right_pos << block_order);
	*pblock_order = block_order;
	goto out;

bad:
	*preal_size_byte = 0;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(dev_size_byte);
	*pblock_order = block_order;

out:
	pdev_detach_and_free(pdev);
	return 0;
}
