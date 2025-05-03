#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <libudev.h>

#include "devices/block_device.h"
#include "devices/usb_reset.h"
#include "libutils.h"

/* XXX This is borrowing from glibc.
 * A better solution would be to return proper errors,
 * so callers write their own messages.
 */
extern const char *__progname;

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

// Write "count" bytes via repeated write
static int write_all(int fd, const char *buf, size_t count)
{
	size_t done = 0;
	do {
		ssize_t rc = write(fd, buf + done, count - done);
		if (rc < 0)
			return -errno;
		done += rc;
	} while (done < count);
	return 0;
}

static int bdev_read_blocks(struct device *dev, char *buf,
	uint64_t first_pos, uint64_t last_pos)
{
	struct block_device *bdev = dev_bdev(dev);
	const int bo = dev_get_block_order(dev);
	size_t length = (last_pos - first_pos + 1) << bo;
	off_t offset = first_pos << bo;
	off_t ret = lseek(bdev->fd, offset, SEEK_SET);
	if (ret < 0)
		return -errno;
	assert(ret == offset);
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

// Map a partition udev_device to its parent disk device
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
