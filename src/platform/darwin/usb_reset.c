#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>

#include "devices/block_device.h"
#include "libdevs.h"
#include "devices/usb_reset.h"

/* macOS stub for USB reset. */
int bdev_manual_usb_reset(struct device *dev)
{
	warnx("USB reset is not supported on macOS");
	return -ENOSYS;
}

/* Reset a USB-backed block device by prompting a detach/reattach. */
int bdev_usb_reset(struct device *dev)
{
	warnx("USB reset is not supported on macOS");
	return -ENOSYS;
}
