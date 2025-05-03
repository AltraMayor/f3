#ifndef INCLUDE_DEVICES_PERF_DEVICE_H
#define INCLUDE_DEVICES_PERF_DEVICE_H

#include "libdevs.h"
#include <stdint.h>

/* Create a performance‐measuring wrapper around a device. */
struct device *create_perf_device(struct device *dev);

/* Sample counters: read/write/reset counts and times in microseconds. */
void perf_device_sample(struct device *dev,
	uint64_t *pread_count, uint64_t *pread_time_us,
	uint64_t *pwrite_count, uint64_t *pwrite_time_us,
	uint64_t *preset_count, uint64_t *preset_time_us);

/* Detach underlying device and free the wrapper, returning the original device. */
struct device *pdev_detach_and_free(struct device *dev);

#endif	/* INCLUDE_DEVICES_PERF_DEVICE_H */
