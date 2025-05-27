#ifndef HEADER_PLATFORM_PRIVATE_USB_RESET_H
#define HEADER_PLATFORM_PRIVATE_USB_RESET_H

#include <f3/libdevs.h>	/* For struct device.	*/

int bdev_manual_usb_reset(struct device *dev);
int bdev_usb_reset(struct device *dev);

#endif	/* HEADER_PLATFORM_PRIVATE_USB_RESET_H */
