#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <err.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#include <libudev.h>

#include "libutils.h"
#include "libdevs.h"

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

struct device {
	uint64_t	size_byte;
	int		block_order;

	int (*read_blocks)(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos);
	int (*write_blocks)(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos);
	int (*reset)(struct device *dev);
	void (*free)(struct device *dev);
	const char *(*get_filename)(struct device *dev);
};

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

static int read_all(int fd, char *buf, size_t count)
{
	size_t done = 0;
	do {
		ssize_t rc = read(fd, buf + done, count - done);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EIO || errno == ENODATA) {
				/* These errors are "expected",
				 * so ignore them.
				 */
			} else {
				/* Execution should not come here. */
				err(errno,
					"%s(): unexpected error code from read(2) = %i",
					__func__, errno);
			}
			return - errno;
		}
		assert(rc != 0); /* We should never hit the end of the file. */
		done += rc;
	} while (done < count);
	return 0;
}

static int bdev_read_blocks(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct block_device *bdev = dev_bdev(dev);
	const int block_order = dev_get_block_order(dev);
	size_t length = (last_pos - first_pos + 1) << block_order;
	off_t offset = first_pos << block_order;
	off_t off_ret = lseek(bdev->fd, offset, SEEK_SET);
	if (off_ret < 0)
		return - errno;
	assert(off_ret == offset);
	return read_all(bdev->fd, buf, length);
}

static int bdev_write_blocks(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct block_device *bdev = dev_bdev(dev);
	const int block_order = dev_get_block_order(dev);
	size_t length = (last_pos - first_pos + 1) << block_order;
	off_t offset = first_pos << block_order;
	off_t off_ret = lseek(bdev->fd, offset, SEEK_SET);
	int rc;
	if (off_ret < 0)
		return - errno;
	assert(off_ret == offset);
	rc = write_all(bdev->fd, buf, length);
	if (rc)
		return rc;
	rc = fsync(bdev->fd);
	if (rc)
		return rc;
	return posix_fadvise(bdev->fd, 0, 0, POSIX_FADV_DONTNEED);
}

static inline int bdev_open(const char *filename)
{
	return open(filename, O_RDWR | O_DIRECT);
}

static struct udev_device *map_dev_to_usb_dev(struct udev_device *dev)
{
	struct udev_device *usb_dev;

	/* The device pointed to by dev contains information about
	 * the USB device.
	 * In order to get information about the USB device,
	 * get the parent device with the subsystem/devtype pair of
	 * "usb"/"usb_device".
	 * This will be several levels up the tree,
	 * but the function will find it.
	 */
	usb_dev = udev_device_get_parent_with_subsystem_devtype(
		dev, "usb", "usb_device");

	/* @usb_dev is not referenced, and will be freed when
	 * the child (i.e. @dev) is freed.
	 * See udev_device_get_parent_with_subsystem_devtype() for
	 * details.
	 */
	return udev_device_ref(usb_dev);
}

static struct udev_device *dev_from_block_fd(struct udev *udev, int block_fd)
{
	struct stat fd_stat;

	if (fstat(block_fd, &fd_stat)) {
		warn("Can't fstat() FD %i", block_fd);
		return NULL;
	}

	if (!S_ISBLK(fd_stat.st_mode)) {
		warnx("FD %i is not a block device", block_fd);
		return NULL;
	}

	return udev_device_new_from_devnum(udev, 'b', fd_stat.st_rdev);
}

static struct udev_monitor *create_monitor(struct udev *udev,
	const char *subsystem, const char *devtype)
{
	struct udev_monitor *mon;
	int mon_fd, flags;

	mon = udev_monitor_new_from_netlink(udev, "udev");
	assert(mon);
	assert(!udev_monitor_filter_add_match_subsystem_devtype(mon,
		subsystem, devtype));
	assert(!udev_monitor_enable_receiving(mon));
	mon_fd = udev_monitor_get_fd(mon);
	assert(mon_fd >= 0);
	flags = fcntl(mon_fd, F_GETFL);
	assert(flags >= 0);
	assert(!fcntl(mon_fd, F_SETFL, flags & ~O_NONBLOCK));

	return mon;
}

static uint64_t get_udev_dev_size_byte(struct udev_device *dev)
{
	const char *str_size_sector =
		udev_device_get_sysattr_value(dev, "size");
	char *end;
	long long size_sector;
	if (!str_size_sector)
		return 0;
	size_sector = strtoll(str_size_sector, &end, 10);
	assert(!*end);
	return size_sector * 512LL;
}

static int wait_for_reset(struct udev *udev, const char *id_serial,
	uint64_t original_size_byte, const char **pfinal_dev_filename)
{
	bool done = false, went_to_zero = false, already_changed_size = false;
	struct udev_monitor *mon;
	int rc;

	mon = create_monitor(udev, "block", "disk");
	if (!mon) {
		warnx("%s(): Can't instantiate a monitor", __func__);
		rc = - ENOMEM;
		goto out;
	}

	do {
		struct udev_device *dev;
		const char *dev_id_serial, *action;
		uint64_t new_size_byte;
		const char *devnode;

		dev = udev_monitor_receive_device(mon);
		if (!dev) {
			warnx("%s(): Can't monitor device", __func__);
			rc = - ENOMEM;
			goto mon;
		}
		dev_id_serial = udev_device_get_property_value(dev,
			"ID_SERIAL");
		if (!dev_id_serial || strcmp(dev_id_serial, id_serial))
			goto next;

		action = udev_device_get_action(dev);
		new_size_byte = get_udev_dev_size_byte(dev);
		if (!strcmp(action, "add")) {
			/* Deal with the case in which the user pulls
			 * the USB device.
			 *
			 * DO NOTHING.
			 */
		} else if (!strcmp(action, "change")) {
			/* Deal with the case in which the user pulls
			 * the memory card from the card reader.
			 */

			if (!new_size_byte) {
				/* Memory card removed. */
				went_to_zero = true;
				goto next;
			}

			if (!went_to_zero)
				goto next;
		} else {
			/* Ignore all other actions. */
			goto next;
		}

		if (new_size_byte != original_size_byte) {
			/* This is an edge case. */

			if (!already_changed_size) {
				already_changed_size = true;
				went_to_zero = false;
				printf("\nThe drive changed its size of %"
					PRIu64 " Bytes to %" PRIu64
					" Bytes after the reset.\nPlease try to unplug and plug it back again...",
					original_size_byte, new_size_byte);
				fflush(stdout);
				goto next;
			}

			printf("\nThe reset failed. The drive has not returned to its original size.\n\n");
			fflush(stdout);
			rc = - ENXIO;
			goto mon;
		}

		devnode = strdup(udev_device_get_devnode(dev));
		if (!devnode) {
			warnx("%s(): Out of memory", __func__);
			rc = - ENOMEM;
			goto mon;
		}
		free((void *)*pfinal_dev_filename);
		*pfinal_dev_filename = devnode;
		done = true;

next:
		udev_device_unref(dev);
	} while (!done);

	rc = 0;

mon:
	assert(!udev_monitor_unref(mon));
out:
	return rc;
}

static int bdev_manual_usb_reset(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	struct udev *udev;
	struct udev_device *udev_dev, *usb_dev;
	const char *id_serial;
	int rc;

	if (bdev->fd < 0) {
		/* We don't have a device open.
		 * This can happen when the previous reset failed, and
		 * a reset is being called again.
		 */
		rc = - EBADF;
		goto out;
	}

	udev = udev_new();
	if (!udev) {
		warnx("Can't load library udev");
		rc = - EOPNOTSUPP;
		goto out;
	}

	/* Identify which drive we are going to reset. */
	udev_dev = dev_from_block_fd(udev, bdev->fd);
	if (!udev_dev) {
		warnx("Library udev can't find device `%s'",
			dev_get_filename(dev));
		rc = - EINVAL;
		goto udev;
	}
	usb_dev = map_dev_to_usb_dev(udev_dev);
	if (!usb_dev) {
		warnx("Block device `%s' is not backed by a USB device",
			dev_get_filename(dev));
		rc = - EINVAL;
		goto udev_dev;
	}
	id_serial = udev_device_get_property_value(udev_dev, "ID_SERIAL");
	if (!id_serial) {
		warnx("%s(): Out of memory", __func__);
		rc = - ENOMEM;
		goto usb_dev;
	}

	/* Close @bdev->fd before the drive is removed to increase
	 * the chance that the device will receive the same filename.
	 * The code is robust enough to deal with the case the drive doesn't
	 * receive the same file name, though.
	 */
	assert(!close(bdev->fd));
	bdev->fd = -1;

	printf("Please unplug and plug back the USB drive. Waiting...");
	fflush(stdout);
	rc = wait_for_reset(udev, id_serial, dev_get_size_byte(dev),
		&bdev->filename);
	if (rc) {
		assert(rc < 0);
		goto usb_dev;
	}
	printf(" Thanks\n\n");

	bdev->fd = bdev_open(bdev->filename);
	if (bdev->fd < 0) {
		rc = - errno;
		warn("Can't reopen device `%s'", bdev->filename);
		goto usb_dev;
	}

	rc = 0;

usb_dev:
	udev_device_unref(usb_dev);
udev_dev:
	udev_device_unref(udev_dev);
udev:
	assert(!udev_unref(udev));
out:
	return rc;
}

static struct udev_device *map_block_to_usb_dev(struct udev *udev, int block_fd)
{
	struct udev_device *dev, *usb_dev;

	dev = dev_from_block_fd(udev, block_fd);
	if (!dev)
		return NULL;
	usb_dev = map_dev_to_usb_dev(dev);
	udev_device_unref(dev);
	return usb_dev;
}

/* Return an open fd to the underlying hardware of the block device. */
static int usb_fd_from_block_dev(int block_fd, int open_flags)
{
	struct udev *udev;
	struct udev_device *usb_dev;
	const char *usb_filename;
	int usb_fd;

	udev = udev_new();
	if (!udev) {
		warnx("Can't load library udev");
		usb_fd = -EOPNOTSUPP;
		goto out;
	}

	usb_dev = map_block_to_usb_dev(udev, block_fd);
	if (!usb_dev) {
		warnx("Block device is not backed by a USB device");
		usb_fd = -EINVAL;
		goto udev;
	}

	usb_filename = udev_device_get_devnode(usb_dev);
	if (!usb_filename) {
		warnx("%s(): Out of memory", __func__);
		usb_fd = -ENOMEM;
		goto usb_dev;
	}

	usb_fd = open(usb_filename, open_flags | O_NONBLOCK);
	if (usb_fd < 0) {
		usb_fd = - errno;
		warn("Can't open device `%s'", usb_filename);
		goto usb_dev;
	}

usb_dev:
	udev_device_unref(usb_dev);
udev:
	assert(!udev_unref(udev));
out:
	return usb_fd;
}

static int bdev_usb_reset(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	int usb_fd;

	if (bdev->fd < 0) {
		/* We don't have a device open.
		 * This can happen when the previous reset failed, and
		 * a reset is being called again.
		 */
		return - EBADF;
	}

	usb_fd = usb_fd_from_block_dev(bdev->fd, O_WRONLY);
	if (usb_fd < 0)
		return usb_fd;

	assert(!close(bdev->fd));
	bdev->fd = -1;
	assert(!ioctl(usb_fd, USBDEVFS_RESET));
	assert(!close(usb_fd));
	bdev->fd = bdev_open(bdev->filename);
	if (bdev->fd < 0) {
		int rc = - errno;
		warn("Can't reopen device `%s'", bdev->filename);
		return rc;
	}
	return 0;
}

static int bdev_none_reset(struct device *dev)
{
	UNUSED(dev);
	return 0;
}

static void bdev_free(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	if (bdev->fd >= 0)
		assert(!close(bdev->fd));
	free((void *)bdev->filename);
}

static const char *bdev_get_filename(struct device *dev)
{
	return dev_bdev(dev)->filename;
}

static struct udev_device *map_partition_to_disk(struct udev_device *dev)
{
	struct udev_device *disk_dev;

	disk_dev = udev_device_get_parent_with_subsystem_devtype(
		dev, "block", "disk");

	/* @disk_dev is not referenced, and will be freed when
	 * the child (i.e. @dev) is freed.
	 * See udev_device_get_parent_with_subsystem_devtype() for
	 * details.
	 */
	return udev_device_ref(disk_dev);
}

/* XXX This is borrowing from glibc.
 * A better solution would be to return proper errors,
 * so callers write their own messages.
 */
extern const char *__progname;

struct device *create_block_device(const char *filename, enum reset_type rt)
{
	struct block_device *bdev;
	struct udev *udev;
	struct udev_device *fd_dev;
	const char *s;
	int block_size, block_order;

	bdev = malloc(sizeof(*bdev));
	if (!bdev)
		goto error;

	bdev->filename = strdup(filename);
	if (!bdev->filename)
		goto bdev;

	bdev->fd = bdev_open(filename);
	if (bdev->fd < 0) {
		if (errno == EACCES && getuid()) {
			fprintf(stderr, "Your user doesn't have access to device `%s'.\n"
				"Try to run this program as root:\n"
				"sudo %s %s\n"
				"In case you don't have access to root, use f3write/f3read.\n",
				filename, __progname, filename);
		} else {
			err(errno, "Can't open device `%s'", filename);
		}
		goto filename;
	}

	/* Make sure that @bdev->fd is a disk, not a partition. */
	udev = udev_new();
	if (!udev) {
		warnx("Can't load library udev");
		goto fd;
	}
	fd_dev = dev_from_block_fd(udev, bdev->fd);
	if (!fd_dev) {
		fprintf(stderr, "Can't create udev device from `%s'\n",
			filename);
		goto udev;
	}
	assert(!strcmp(udev_device_get_subsystem(fd_dev), "block"));
	s = udev_device_get_devtype(fd_dev);
	if (!strcmp(s, "partition")) {
		struct udev_device *disk_dev = map_partition_to_disk(fd_dev);
		assert(disk_dev);
		s = udev_device_get_devnode(disk_dev);
		fprintf(stderr, "Device `%s' is a partition of disk device `%s'.\n"
			"You must run %s on the disk device as follows:\n"
			"%s %s\n",
			filename, s, __progname, __progname, s);
		udev_device_unref(disk_dev);
		goto fd_dev;
	} else if (strcmp(s, "disk")) {
		fprintf(stderr, "Device `%s' is not a disk, but `%s'",
			filename, s);
		goto fd_dev;
	}

	if (rt != RT_NONE) {
		/* Make sure that @bdev->fd is backed by a USB device. */
		struct udev_device *usb_dev = map_dev_to_usb_dev(fd_dev);
		if (!usb_dev) {
			fprintf(stderr,
				"Device `%s' is not backed by a USB device.\n"
				"You must disable reset, run %s as follows:\n"
				"%s --reset-type=%i %s\n",
				filename, __progname, __progname, RT_NONE,
				filename);
			goto fd_dev;
		}
		udev_device_unref(usb_dev);
	}
	udev_device_unref(fd_dev);
	assert(!udev_unref(udev));

	switch (rt) {
	case RT_MANUAL_USB:
		bdev->dev.reset	= bdev_manual_usb_reset;
		break;
	case RT_USB:
		bdev->dev.reset = bdev_usb_reset;
		break;
	case RT_NONE:
		bdev->dev.reset = bdev_none_reset;
		break;
	default:
		assert(0);
	}

	assert(!ioctl(bdev->fd, BLKGETSIZE64, &bdev->dev.size_byte));

	assert(!ioctl(bdev->fd, BLKSSZGET, &block_size));
	block_order = ilog2(block_size);
	assert(block_size == (1 << block_order));
	bdev->dev.block_order = block_order;

	bdev->dev.read_blocks = bdev_read_blocks;
	bdev->dev.write_blocks = bdev_write_blocks;
	bdev->dev.free = bdev_free;
	bdev->dev.get_filename = bdev_get_filename;

	return &bdev->dev;

fd_dev:
	udev_device_unref(fd_dev);
udev:
	assert(!udev_unref(udev));
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

static int pdev_read_blocks(struct device *dev, char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct timeval t1, t2;
	int rc;

	assert(!gettimeofday(&t1, NULL));
	rc = pdev->shadow_dev->read_blocks(pdev->shadow_dev, buf,
		first_pos, last_pos);
	assert(!gettimeofday(&t2, NULL));
	pdev->read_count += last_pos - first_pos + 1;
	pdev->read_time_us += diff_timeval_us(&t1, &t2);
	return rc;
}

static int pdev_write_blocks(struct device *dev, const char *buf,
		uint64_t first_pos, uint64_t last_pos)
{
	struct perf_device *pdev = dev_pdev(dev);
	struct timeval t1, t2;
	int rc;

	assert(!gettimeofday(&t1, NULL));
	rc = pdev->shadow_dev->write_blocks(pdev->shadow_dev, buf,
		first_pos, last_pos);
	assert(!gettimeofday(&t2, NULL));
	pdev->write_count += last_pos - first_pos + 1;
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

static const char *pdev_get_filename(struct device *dev)
{
	return dev_get_filename(dev_pdev(dev)->shadow_dev);
}

struct device *pdev_detach_and_free(struct device *dev)
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
	pdev->dev.read_blocks = pdev_read_blocks;
	pdev->dev.write_blocks = pdev_write_blocks;
	pdev->dev.reset	= pdev_reset;
	pdev->dev.free = pdev_free;
	pdev->dev.get_filename = pdev_get_filename;

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
