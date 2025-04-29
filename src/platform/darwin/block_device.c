#include "devices/block_device.h"
#include "libdevs.h"
#include <stdio.h>
#include <errno.h>

// macOS stub for raw block device
struct device *create_block_device(const char *filename, enum reset_type rt)
{
    // Not implemented for macOS
    fprintf(stderr, "[macOS] create_block_device: Not implemented yet.\n");
    return NULL;
}
