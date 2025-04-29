#include "devices/block_device.h"
#include "libdevs.h"
#include <stdio.h>

// macOS stub for USB reset
int bdev_manual_usb_reset(struct device *dev)
{
    // Not implemented for macOS
    fprintf(stderr, "[macOS] usb_reset: Not implemented yet.\n");
    return -1;
}
