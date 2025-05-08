#ifndef HEADER_DEVICES_USB_RESET_H
#define HEADER_DEVICES_USB_RESET_H

#include "libdevs.h"

int bdev_manual_usb_reset(struct device *dev);
int bdev_usb_reset(struct device *dev);

#endif  /* HEADER_DEVICES_USB_RESET_H */
