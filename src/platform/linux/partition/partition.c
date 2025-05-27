#include <stdbool.h>
#include <assert.h>
#include <parted/parted.h>

#include <f3/platform/partition.h>

// Convert physical sector to logical sector
static PedSector map_sector_to_logical_sector(PedSector sector,
	int logical_sector_size)
{
	assert(logical_sector_size >= 512);
	assert(logical_sector_size % 512 == 0);
	return sector / (logical_sector_size / 512);
}

// Original fix_disk function
static int parted_fix_disk(PedDevice *dev, PedDiskType *type,
	PedFileSystemType *fs_type, int boot, PedSector start, PedSector end)
{
	PedDisk *disk;
	PedPartition *part;
	PedGeometry *geom;
	PedConstraint *constraint;
	int ret = 0;

	disk = ped_disk_new_fresh(dev, type);
	if (!disk)
		goto out;

	start = map_sector_to_logical_sector(start, dev->sector_size);
	end = map_sector_to_logical_sector(end, dev->sector_size);
	part = ped_partition_new(disk, PED_PARTITION_NORMAL,
		fs_type, start, end);
	if (!part)
		goto disk;
	if (boot && !ped_partition_set_flag(part, PED_PARTITION_BOOT, 1))
		goto part;

	geom = ped_geometry_new(dev, start, end - start + 1);
	if (!geom)
		goto part;
	constraint = ped_constraint_exact(geom);
	ped_geometry_destroy(geom);
	if (!constraint)
		goto part;

	ret = ped_disk_add_partition(disk, part, constraint);
	ped_constraint_destroy(constraint);
	if (!ret)
		goto part;
	/* ped_disk_print(disk); */

	ret = ped_disk_commit(disk);
	goto disk;

part:
	ped_partition_destroy(part);
disk:
	ped_disk_destroy(disk);
out:
	return ret;
}

// Create partition on a device
int partition_create(const char *dev_filename, const struct partition_options *options)
{
	PedDevice *ped_dev;
	PedDiskType *disk_type;
	PedFileSystemType *fs_type;
	int ret = 1;  // assume failure

	// Get parted device
	ped_dev = ped_device_get(dev_filename);
	if (!ped_dev) {
		goto out;
	}

	// Get disk type
	disk_type = ped_disk_type_get(options->disk_type);
	if (!disk_type) {
		goto pdev;
	}

	// Get file system type
	fs_type = ped_file_system_type_get(options->fs_type);
	if (!fs_type) {
		goto pdev;
	}

	// Create the partition
	ret = !parted_fix_disk(ped_dev, disk_type, fs_type, options->boot,
		options->first_sector, options->last_sector);

	// Cleanup
pdev:
	ped_device_destroy(ped_dev);
out:
	return ret;
}

/* Type validation functions */
bool is_valid_disk_type(const char *disk_type)
{
	return ped_disk_type_get(disk_type) != NULL;
}

bool is_valid_fs_type(const char *fs_type)
{
	return ped_file_system_type_get(fs_type) != NULL;
}

/* Generic array helpers */
typedef const void *(*IterFn)(const void *prev);
typedef const char *(*NameFn)(const void *type);

static size_t build_types_array(char ***out, IterFn get_next, NameFn name_fn)
{
	const void *type = NULL;
	char **types;
	size_t count = 0;
	size_t i = 0;

	/* First pass: count the number of types */
	while ((type = get_next(type)))
		++count;

	/* Allocate memory for the array (count + 1 for null terminator) */
	types = calloc(count + 1, sizeof(char*));
	if (!types)
		goto err;

	/* Second pass: duplicate strings and populate the array */
	for (type = get_next(NULL); type;
		 type = get_next(type), ++i) {
		types[i] = strdup(name_fn(type));
		if (!types[i])
			goto oom;
	}

	*out = types;
	return count;

oom:
	/* free partial array */
	while (i)
		free(types[--i]);
	free(types);
err:
	*out = NULL;
	return 0;
}

/* Concrete array adapters */
static const void *disk_next(const void *p)
{
	return ped_disk_type_get_next((const PedDiskType *)p);
}

static const char *disk_name(const void *p)
{
	return ((const PedDiskType *)p)->name;
}

static const void *fs_next(const void *p)
{
	return ped_file_system_type_get_next((const PedFileSystemType *)p);
}

static const char *fs_name(const void *p)
{
	return ((const PedFileSystemType *)p)->name;
}

/* Public wrappers */
size_t partition_list_disk_types(char ***out_array)
{
	return build_types_array(out_array, disk_next, disk_name);
}

size_t partition_list_fs_types(char ***out_array)
{
	return build_types_array(out_array, fs_next, fs_name);
}

void partition_free_types_array(char **array)
{
	if (!array) return;
	for (char **p = array; *p; ++p)
		free(*p);
	free(array);
}
