#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
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
	uint64_t announced_size_byte, int wrap)
{
	/* Check general ranges. */
	if (real_size_byte > announced_size_byte || wrap < 0 || wrap >= 64)
		return false;

	/* If good, @wrap must make sense. */
	if (real_size_byte == announced_size_byte) {
		uint64_t two_wrap = ((uint64_t)1) << wrap;
		return announced_size_byte <= two_wrap;
	}

	return true;
}

enum fake_type dev_param_to_type(uint64_t real_size_byte,
	uint64_t announced_size_byte, int wrap)
{
	uint64_t two_wrap;

	assert(dev_param_valid(real_size_byte, announced_size_byte, wrap));

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
	int (*read_block)(struct device *dev, char *buf, uint64_t block);
	int (*write_block)(struct device *dev, const char *buf, uint64_t block);
	int (*reset)(struct device *dev);
	uint64_t (*get_size_byte)(struct device *dev);
	void (*free)(struct device *dev);
};

struct file_device {
	/* This must be the first field. See dev_fdev() for details. */
	struct device dev;

	int fd;
	uint64_t real_size_byte;
	uint64_t fake_size_byte;
	uint64_t address_mask;
};

static inline struct file_device *dev_fdev(struct device *dev)
{
	return (struct file_device *)dev;
}

#define GIGABYTE	(1 << 30)

/* XXX Replace expressions block * BLOCK_SIZE to block << BLOCK_SIZE_BITS,
 * and do the same for expressions like GIGABYTE * variable.
 */
static int fdev_read_block(struct device *dev, char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = (block * BLOCK_SIZE) & fdev->address_mask;
	int done;

	if ((uint64_t)offset >= fdev->real_size_byte) {
		memset(buf, 0, BLOCK_SIZE);
		return 0;
	}

	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);

	done = 0;
	do {
		ssize_t rc = read(fdev->fd, buf + done, BLOCK_SIZE - done);
		assert(rc >= 0);
		if (!rc) {
			/* Tried to read beyond the end of the file. */
			assert(!done);
			memset(buf, 0, BLOCK_SIZE);
			done += BLOCK_SIZE;
		}
		done += rc;
	} while (done < BLOCK_SIZE);

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

static int fdev_write_block(struct device *dev, const char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = (block * BLOCK_SIZE) & fdev->address_mask;
	if ((uint64_t)offset >= fdev->real_size_byte)
		return 0;
	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);
	return write_all(fdev->fd, buf, BLOCK_SIZE);
}

static uint64_t fdev_get_size_byte(struct device *dev)
{
	return dev_fdev(dev)->fake_size_byte;
}

static void fdev_free(struct device *dev)
{
	struct file_device *fdev = dev_fdev(dev);
	assert(!close(fdev->fd));
}

struct device *create_file_device(const char *filename,
	uint64_t real_size_byte, uint64_t fake_size_byte, int wrap)
{
	struct file_device *fdev;

	if (!dev_param_valid(real_size_byte, fake_size_byte, wrap))
		goto error;

	fdev = malloc(sizeof(*fdev));
	if (!fdev)
		goto error;

	fdev->fd = open(filename, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fdev->fd < 0) {
		err(errno, "Can't create file `%s'", filename);
		goto fdev;
	}
	/* Unlinking the file now guarantees that it won't exist if
	 * there is a crash.
	 */
	assert(!unlink(filename));

	fdev->real_size_byte = real_size_byte;
	fdev->fake_size_byte = fake_size_byte;
	fdev->address_mask = (((uint64_t)1) << wrap) - 1;

	fdev->dev.read_block = fdev_read_block;
	fdev->dev.write_block = fdev_write_block;
	fdev->dev.reset = NULL;
	fdev->dev.get_size_byte = fdev_get_size_byte;
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

static int bdev_read_block(struct device *dev, char *buf, uint64_t block)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t offset = block * BLOCK_SIZE;
	assert(lseek(bdev->fd, offset, SEEK_SET) == offset);
	return read_all(bdev->fd, buf, BLOCK_SIZE);
}

static int bdev_write_block(struct device *dev, const char *buf, uint64_t block)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t offset = block * BLOCK_SIZE;
	assert(lseek(bdev->fd, offset, SEEK_SET) == offset);
	return write_all(bdev->fd, buf, BLOCK_SIZE);
}

static inline int bdev_open(const char *filename)
{
	return open(filename, O_RDWR | O_DIRECT | O_SYNC);
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

static uint64_t bdev_get_size_byte(struct device *dev)
{
	uint64_t size_byte;
	assert(!ioctl(dev_bdev(dev)->fd, BLKGETSIZE64, &size_byte));
	return size_byte;
}

static void bdev_free(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	assert(!close(bdev->hw_fd));
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

/* XXX Test if it's a device, or a partition.
 * If a partition, warn user, and ask for confirmation before
 * going ahead.
 * Suggest how to call f3probe with the correct device name if
 * the block device is a partition.
 * Use udev to do these tests.
 */
/* XXX Test for write access of the block device to give
 * a nice error message.
 * If it fails, suggest running f3probe as root.
 */
struct device *create_block_device(const char *filename)
{
	const char *usb_filename;
	struct block_device *bdev;

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

	/* XXX Add support for block devices backed by SCSI, SATA, and ATA. */
	usb_filename = map_block_to_usb_dev(filename);
	if (!usb_filename) {
		err(EINVAL, "Block device `%s' is not backed by a USB device",
			filename);
		goto fd;
	}

	bdev->hw_fd = open(usb_filename, O_WRONLY | O_NONBLOCK);
	if (bdev->hw_fd < 0) {
		err(errno, "Can't open device `%s'", usb_filename);
		goto usb_filename;
	}

	bdev->dev.read_block = bdev_read_block;
	bdev->dev.write_block = bdev_write_block;
	bdev->dev.reset	= bdev_reset;
	bdev->dev.get_size_byte = bdev_get_size_byte;
	bdev->dev.free = bdev_free;

	free((void *)usb_filename);
	return &bdev->dev;

usb_filename:
	free((void *)usb_filename);
fd:
	assert(!close(bdev->fd));
filename:
	free((void *)bdev->filename);
bdev:
	free(bdev);
error:
	return NULL;
}

void free_device(struct device *dev)
{
	dev->free(dev);
	free(dev);
}

static inline int dev_read_block(struct device *dev, char *buf, uint64_t block)
{
	return dev->read_block(dev, buf, block);
}

static inline int dev_write_block(struct device *dev, const char *buf,
	uint64_t block)
{
	return dev->write_block(dev, buf, block);
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

static inline uint64_t dev_get_size_byte(struct device *dev)
{
	return dev->get_size_byte(dev);
}

static inline int equal_blk(const char *b1, const char *b2)
{
	return !memcmp(b1, b2, BLOCK_SIZE);
}

/* Minimum size of the memory chunk used to build flash drives.
 * It must be a power of two.
 */
#define	INITAIL_HIGH_BIT_BLOCK	(1 << (20 - BLOCK_SIZE_BITS))

/* Caller must guarantee that the left bock is good, and written. */
static int search_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t high_bit = INITAIL_HIGH_BIT_BLOCK;
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
		if (equal_blk(stamp_blk, probe_blk)) {
			/* XXX Test wraparound hypothesis. */
			*pright_pos = high_bit - 1;
			return false;
		}
		high_bit <<= 1;
		pos = high_bit + left_pos;
	}

	return false;
}

/* Return true if @b1 and b2 are at most @tolerance_byte bytes different. */
static int similar_blk(const char *b1, const char *b2, int tolerance_byte)
{
	int i;

	for (i = 0; i < BLOCK_SIZE; i++) {
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

/* Return true if the block @pos is damaged. */
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

	if (equal_blk(stamp_blk, probe_blk))
		return false;

	/* Save time with certainly damaged blocks. */
	if (!similar_blk(stamp_blk, probe_blk, 8)) {
		/* The probe block is damaged. */
		return true;
	}

	/* The probe block seems to be damaged.
	 * Trying a second time...
	 */
	return 	dev_write_and_reset(dev, stamp_blk, pos) ||
		dev_read_block(dev, probe_blk, pos)  ||
		!equal_blk(stamp_blk, probe_blk);
}

/* Caller must guarantee that the left bock is good, and written. */
static int search_edge(struct device *dev,
	uint64_t *pleft_pos, uint64_t right_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t pos = right_pos;
	do {
		if (test_block(dev, stamp_blk, probe_blk, pos))
			right_pos = pos;
		else
			*pleft_pos = pos;
		pos = (*pleft_pos + right_pos) / 2;
	} while (right_pos - *pleft_pos >= 2);
	return  false;
}

/* XXX Write random data to make it harder for fake chips to become "smarter".
 * There would be a random seed.
 * Buffer cannot be all 0x00 or all 0xFF.
 */
static void fill_buffer(char *buf, int len)
{
	memset(buf, 0xAA, len);
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

static inline void *align_512(void *p)
{
	uintptr_t ip = (uintptr_t)p;
	return (void *)(   (ip + 511) & ~511   );
}

/* XXX Properly handle read and write errors.
 * Review each assert to check if them can be removed.
 */
void probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap)
{
	uint64_t dev_size_byte = dev_get_size_byte(dev);
	char stack[511 + 2 * BLOCK_SIZE];
	char *stamp_blk, *probe_blk;
	/* XXX Don't write at the very beginning of the card to avoid
	 * losing the partition table.
	 * But write at a random locations to make harder for fake chips
	 * to become "smarter".
	 * And try a couple of blocks if they keep failing.
	 */
	uint64_t left_pos = 10;
	uint64_t right_pos = (dev_size_byte >> BLOCK_SIZE_BITS) - 1;

	assert(dev_size_byte % BLOCK_SIZE == 0);
	assert(left_pos < right_pos);

	/* Aligning these pointers is necessary to directly read and write
	 * the block device.
	 * For the file device, this is superfluous.
	 */
	stamp_blk = align_512(stack);
	probe_blk = stamp_blk + BLOCK_SIZE;

	fill_buffer(stamp_blk, BLOCK_SIZE);

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */
	if (test_block(dev, stamp_blk, probe_blk, left_pos))
		goto bad;

	if (search_wrap(dev, left_pos, &right_pos, stamp_blk, probe_blk))
		goto bad;

	if (search_edge(dev, &left_pos, right_pos, stamp_blk, probe_blk))
		goto bad;

	*preal_size_byte = (left_pos + 1) << BLOCK_SIZE_BITS;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(right_pos * BLOCK_SIZE);
	return;

bad:
	*preal_size_byte = 0;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(dev_size_byte);
}
