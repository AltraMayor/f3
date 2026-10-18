/* macOS implementation of the platform layer of the block-device backend.
 *
 * This file is included by libdevs.c; see the platform-layer comment
 * there for the contract it implements.  It is not built on its own.
 */

#include <sys/disk.h>

static inline int bdev_open(const char *filename)
{
	/* macOS has no O_DIRECT; F_NOCACHE turns the page cache off for
	 * this descriptor, which is what the probe algorithm requires.
	 * Callers should prefer the raw device (/dev/rdiskN); see
	 * bdev_validate_disk().
	 */
	int fd = open(filename, O_RDWR);
	if (fd >= 0 && fcntl(fd, F_NOCACHE, 1))
		warn("Can't disable caching for device `%s'", filename);
	return fd;
}

static int bdev_drop_cache(int fd)
{
	/* The descriptor is opened with F_NOCACHE (see bdev_open()),
	 * so there is no page cache to drop.
	 */
	UNUSED(fd);
	return 0;
}

static int bdev_geometry(int fd, uint64_t *size_byte, int *block_size)
{
	uint64_t block_count;
	uint32_t bs;

	if (ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count))
		return - errno;
	if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs))
		return - errno;

	*size_byte = block_count * bs;
	*block_size = bs;
	return 0;
}

static void msleep_ms(unsigned int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Return true when @filename names a whole disk (e.g. /dev/rdisk5).
 *
 * When it names a partition slice instead (e.g. /dev/rdisk5s2), write
 * the name of the whole disk into @disk_name so that the caller can
 * point the user at it.
 */
static bool darwin_is_whole_disk(const char *filename, char *disk_name,
	size_t disk_name_len)
{
	const char *p = filename;
	const char *num_start;

	if (strncmp(p, "/dev/", 5))
		return false;
	p += 5;
	if (*p == 'r')
		p++;
	if (strncmp(p, "disk", 4))
		return false;
	p += 4;

	num_start = p;
	while (*p >= '0' && *p <= '9')
		p++;
	if (p == num_start)
		return false;
	if (*p == '\0')
		return true;

	if (*p == 's')
		snprintf(disk_name, disk_name_len, "/dev/rdisk%.*s",
			(int)(p - num_start), num_start);
	return false;
}

static int bdev_validate_disk(const char *filename, int fd,
	enum reset_type rt)
{
	char disk_name[32] = "";

	UNUSED(fd);

	if (!darwin_is_whole_disk(filename, disk_name, sizeof(disk_name))) {
		if (disk_name[0])
			fprintf(stderr, "Device `%s' is a partition of disk device `%s'.\n"
				"You must run %s on the disk device as follows:\n"
				"%s %s\n",
				filename, disk_name, __progname,
				__progname, disk_name);
		else
			fprintf(stderr, "Device `%s' is not a whole-disk device.\n"
				"Expected a name like /dev/rdisk5; see `diskutil list'.\n",
				filename);
		return -1;
	}

	if (strncmp(filename, "/dev/rdisk", 10))
		fprintf(stderr, "NOTE: prefer the raw device (/dev/r%s) for correct operation.\n",
			filename + 5);

	/* macOS has no equivalent of libudev's device tree here, so
	 * whether the disk is backed by USB is not checked; the reset
	 * implementations below report it instead.
	 */
	UNUSED(rt);
	return 0;
}

/* Manual reset: wait for the device node to disappear, then for it to
 * come back with the same size, and reopen it.
 *
 * Note that macOS may renumber the disk when other devices enumerate
 * meanwhile; in that case this call times out and the user should run
 * the tool again.
 */
static int bdev_manual_usb_reset(struct device *dev)
{
	struct block_device *bdev = dev_bdev(dev);
	uint64_t orig_size = dev_get_size_byte(dev);
	int waited_ms;

	if (bdev->fd < 0)
		return - EBADF;
	assert(!close(bdev->fd));
	bdev->fd = -1;

	printf("Please unplug and plug back the USB drive. Waiting...");
	fflush(stdout);

	/* Wait for the device to go away. */
	while (1) {
		struct stat st;
		if (stat(bdev->filename, &st))
			break;
		msleep_ms(200);
	}

	/* Wait up to 60s for it to come back with the same size. */
	for (waited_ms = 0; waited_ms < 60000; waited_ms += 200) {
		int fd = bdev_open(bdev->filename);
		if (fd >= 0) {
			uint64_t size_byte;
			int block_size;
			if (!bdev_geometry(fd, &size_byte, &block_size) &&
				size_byte == orig_size) {
				bdev->fd = fd;
				printf(" Thanks\n\n");
				return 0;
			}
			assert(!close(fd));
		}
		msleep_ms(200);
	}

	warnx("Device `%s' did not come back after the reset; "
		"macOS may have renumbered the disk. Check with "
		"`diskutil list' and run again.", bdev->filename);
	return - ENODEV;
}

static int bdev_usb_reset(struct device *dev)
{
	UNUSED(dev);
	warnx("Software USB reset is not supported on macOS; "
		"use the manual reset (unplug/replug) instead");
	return - EOPNOTSUPP;
}
