#ifndef USB_RESET_H
#define USB_RESET_H

#include "devices/block_device.h"

int bdev_manual_usb_reset(struct device *dev);
int bdev_usb_reset(struct device *dev);

#endif /* USB_RESET_H */
