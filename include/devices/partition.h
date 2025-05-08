#ifndef HEADER_DEVICES_PARTITION_H
#define HEADER_DEVICES_PARTITION_H

#include "libdevs.h"
#include <stdint.h>	/* For type uint64_t.	*/
#include <stdbool.h>	/* For type bool.	*/

/* Partition creation options. */
struct partition_options {
	const char *disk_type;  // default: "msdos"
	const char *fs_type;    // default: "fat32"
	bool boot;
	uint64_t first_sector;
	uint64_t last_sector;
};

/* Partition management functions */
int partition_create(const char *dev_filename, const struct partition_options *options);

/* Type validation functions */
bool is_valid_disk_type(const char *disk_type);
bool is_valid_fs_type(const char *fs_type);

/* List supported disk and file system types
 * Returns an array of `count` NUL-terminated strings.
 * The caller *must* free the array with partition_free_types_array().
 */
size_t partition_list_disk_types(char ***out_array);
size_t partition_list_fs_types(char ***out_array);
void partition_free_types_array(char **array);

#endif	/* HEADER_DEVICES_PARTITION_H */
