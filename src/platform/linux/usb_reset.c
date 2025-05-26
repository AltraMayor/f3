#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <libudev.h>

#include "../private/usb_reset_private.h"
#include "../private/block_device_private.h"
#include "private/linux_private.h"

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

// Reset a USB-backed block device by prompting a detach/reattach
int bdev_manual_usb_reset(struct device *dev)
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

// Return an open fd to the underlying USB hardware for the block device
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

// Reset a USB-backed block device via USBDEVFS_RESET ioctl
int bdev_usb_reset(struct device *dev)
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
