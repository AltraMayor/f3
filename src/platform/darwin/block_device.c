#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <err.h>
#include <diskarbitration/diskarbitration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <f3/platform/block_device.h>
#include <f3/libutils.h>

#include "../private/block_device_private.h"
#include "../private/usb_reset_private.h"

/* XXX This is borrowing from glibc.
 * A better solution would be to return proper errors,
 * so callers write their own messages.
 */
extern const char *__progname;

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
		return - errno;
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
	return 0;
}

static inline int bdev_open(const char *filename)
{
	int fd = open(filename, O_RDWR);
	if (fd >= 0) {
		fcntl(fd, F_NOCACHE, 1);
	}
	return fd;
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

/* Map a disk path to its parent whole disk using Disk Arbitration.
 * Returns the parent DADiskRef if the path is a partition, NULL otherwise.
 * The caller is responsible for releasing the returned DADiskRef.
 */
static DADiskRef map_partition_to_disk(const char *path)
{
	DASessionRef session = NULL;
	DADiskRef disk = NULL;
	DADiskRef whole_disk = NULL;
	DADiskRef parent_disk_to_return = NULL;

	session = DASessionCreate(kCFAllocatorDefault);
	if (!session) {
		goto cleanup;
	}

	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, path);
	if (!disk) {
		goto cleanup;
	}

	/* Get the whole disk containing the created disk object.
	 * If 'disk' is a partition, whole_disk will be its parent.
	 * If 'disk' is already a whole disk, whole_disk will be the same as disk.
	 */
	whole_disk = DADiskCopyWholeDisk(disk);
	if (!whole_disk) {
		goto cleanup;
	}

	if (!CFEqual(disk, whole_disk)) {
		/* Original disk is a partition and whole_disk is its parent. */
		parent_disk_to_return = whole_disk;
		whole_disk = NULL;
	}

cleanup:
	if (whole_disk) { CFRelease(whole_disk); }
	if (disk) { CFRelease(disk); }
	if (session) { CFRelease(session); }

	return parent_disk_to_return;
}

struct device *create_block_device(const char *filename, enum reset_type rt)
{
	struct block_device *bdev;
	DASessionRef session;
	DADiskRef disk;
	CFDictionaryRef properties;
	int block_size, block_order;
	uint64_t deviceSize;

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

	/* Get disk properties using Disk Arbitration. */
	session = DASessionCreate(kCFAllocatorDefault);
	if (!session) {
		fprintf(stderr, "Can't create disk session\n");
		goto fd;
	}

	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, filename);
	if (!disk) {
		fprintf(stderr, "Can't get disk reference for `%s'\n", filename);
		goto da_session;
	}

	CFDictionaryRef desc = DADiskCopyDescription(disk);
	if (!desc) {
		fprintf(stderr, "Can't get disk description for `%s'\n", filename);
		goto da_disk;
	}

	properties = CFDictionaryCreateCopy(kCFAllocatorDefault, desc);
	CFRelease(desc);
	desc = NULL;
	if (!properties) {
		fprintf(stderr, "Can't get disk properties for `%s'\n", filename);
		goto da_disk;
	}
	CFRelease(disk);
	CFRelease(session);
	disk = NULL;
	session = NULL;

	/* Check if this is a partition. */
	DADiskRef parent = map_partition_to_disk(filename);
	if (parent) {
		fprintf(stderr, "Device `%s' is a partition.\n"
			"You must run %s on the parent disk device.\n",
			filename, __progname);
		CFRelease(parent);
		goto da_properties;
	}

	/* Get block size and device size using Disk Arbitration. */
	CFNumberRef blockSizeNumber = CFDictionaryGetValue(properties, kDADiskDescriptionMediaBlockSizeKey);
	if (!blockSizeNumber || !CFNumberGetValue(blockSizeNumber, kCFNumberIntType, &block_size)) {
		fprintf(stderr, "Can't get block size for `%s'\n", filename);
		goto da_properties;
	}

	block_order = ilog2(block_size);
	assert(block_size == (1 << block_order));

	/* Get device size. */
	CFNumberRef deviceSizeNumber = CFDictionaryGetValue(properties, kDADiskDescriptionMediaSizeKey);
	if (!deviceSizeNumber || !CFNumberGetValue(deviceSizeNumber, kCFNumberSInt64Type, &deviceSize)) {
		fprintf(stderr, "Can't get device size for `%s'\n", filename);
		goto da_properties;
	}
	CFRelease(properties);
	properties = NULL;

	switch (rt) {
		case RT_MANUAL_USB:
			bdev->dev.reset = bdev_manual_usb_reset;
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

	bdev->dev.size_byte = deviceSize;
	bdev->dev.block_order = block_order;
	bdev->dev.read_blocks = bdev_read_blocks;
	bdev->dev.write_blocks = bdev_write_blocks;
	bdev->dev.free = bdev_free;
	bdev->dev.get_filename = bdev_get_filename;

	return &bdev->dev;

da_properties:
	if (properties) { CFRelease(properties); }
da_disk:
	if (disk) { CFRelease(disk); }
da_session:
	if (session) { CFRelease(session); }
fd:
	assert(!close(bdev->fd));
filename:
	free((void *)bdev->filename);
bdev:
	free(bdev);
error:
	return NULL;
}
