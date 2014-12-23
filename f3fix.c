#include <error.h>
#include <assert.h>
#include <parted/parted.h>

/* TODO Modify f3probe to suggest using f3fix when the drive is fake.
 * Wait until f3fix is stable to implement this.
 */

/* TODO Add parameters. */

/* TODO List types of the partition table. */

/* TODO List types of file systems (function below).
 * One still needs a calling option to list the types.
 */
static void list_fs_types(void)
{
	PedFileSystemType *fs_type;
	for (fs_type = ped_file_system_type_get_next(NULL); fs_type;
		fs_type = ped_file_system_type_get_next(fs_type))
		puts(fs_type->name);
}

/* Use example: sudo ./f3fix /dev/sdb msdos 2048 15010455 */

/* TODO This code needs a careful review to deal with errors. */
int main (int argc, char *argv[])
{
	PedDevice *dev;
	PedDiskType *type;
	PedDisk *disk;
	PedPartition *part; 
	PedConstraint *constraint;
	PedFileSystemType *fs_type;
	PedGeometry *geom;
	PedSector start = atoi(argv[3]);
	PedSector   end = atoi(argv[4]);
	int ret = 1;

	if (argc != 5)
		error(EXIT_FAILURE, 0, "wrong number of arguments");

	dev = ped_device_get(argv[1]);
	if (!dev)
		goto out;

	type = ped_disk_type_get(argv[2]);
	if (!type)
		goto device;

	disk = ped_disk_new_fresh(dev, type);
	if (!disk)
		goto device;

	fs_type = ped_file_system_type_get("fat32");
	assert(fs_type);

	part = ped_partition_new(disk, PED_PARTITION_NORMAL,
		fs_type, start, end);
	if (!part)
		goto disk;
	assert(ped_partition_set_flag(part, PED_PARTITION_BOOT, 1));

	geom = ped_geometry_new(dev, start, end - start + 1);
	assert(geom);
	constraint = ped_constraint_exact(geom);
	assert(constraint);
	assert(ped_disk_add_partition(disk, part, constraint));

	ped_disk_print(disk);

	assert(ped_disk_commit(disk));

	ret = 0;

disk:
	ped_disk_destroy(disk);
device:
	ped_device_destroy(dev);
out:
	return ret;
}
