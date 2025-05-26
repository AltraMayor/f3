#ifndef HEADER_DEVICES_PERF_DEVICE_H
#define HEADER_DEVICES_PERF_DEVICE_H

#include <f3/libdevs.h>	/* For struct device.	*/
#include <stdint.h>	/* For type uint64_t.	*/

/* Create a performance‐measuring wrapper around a device. */
struct device *create_perf_device(struct device *dev);

/* Sample counters: read/write/reset counts and times in microseconds. */
void perf_device_sample(struct device *dev,
	uint64_t *pread_count, uint64_t *pread_time_us,
	uint64_t *pwrite_count, uint64_t *pwrite_time_us,
	uint64_t *preset_count, uint64_t *preset_time_us);

#endif	/* HEADER_DEVICES_PERF_DEVICE_H */
