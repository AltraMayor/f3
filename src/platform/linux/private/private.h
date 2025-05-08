#ifndef LINUX_PLATFORM_PRIVATE_H
#define LINUX_PLATFORM_PRIVATE_H

#include <libudev.h> // For struct udev, struct udev_device
#include <sys/stat.h> // For struct stat, S_ISBLK, fstat

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

#endif /* LINUX_PLATFORM_PRIVATE_H */
