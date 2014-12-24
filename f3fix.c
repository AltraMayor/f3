#include <stdbool.h>
#include <argp.h>
#include <parted/parted.h>

#include "version.h"

/* XXX Refactor utils library since f3probe barely uses it. */
#include "utils.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Fix " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<DISK_DEV>";

static char doc[] = "F3 Fix -- edit the partition table of "
	"a fake flash drive to have a single partition that fully covers "
	"the real capacity of the drive";

static struct argp_option options[] = {
	{"disk-type",		'd',	"TYPE",		0,
		"Disk type of the partition table",			2},
	{"fs-type",		'f',	"TYPE",		0,
		"Type of the file system of the partition",		0},
	{"boot",			'b',	NULL,		0,
		"Mark the partition for boot",				0},
	{"no-boot",		'n',	NULL,		0,
		"Do not mark the partition for boot",			0},
	{"first-sec",		'a',	"SEC-NUM",	0,
		"Sector where the partition starts",			0},
	{"last-sec",		'l',	"SEC-NUM",	0,
		"Sector where the partition ends",			0},
	{"list-disk-types",	'k',	NULL,		0,
		"List all supported disk types",			3},
	{"list-fs-types",	's',	NULL,		0,
		"List all supported types of file systems",		0},
	{ 0 }
};

struct args {
	bool	list_disk_types;
	bool	list_fs_types;

	bool	boot;

	/* 29 free bytes. */

	const char		*dev_filename;
	PedDiskType		*disk_type;
	PedFileSystemType	*fs_type;
	PedSector		first_sec;
	PedSector		last_sec;
};

static long long arg_to_long_long(const struct argp_state *state,
	const char *arg)
{
	char *end;
	long long ll = strtoll(arg, &end, 0);
	if (!arg)
		argp_error(state, "An integer must be provided");
	if (!*arg || *end)
		argp_error(state, "`%s' is not an integer", arg);
	return ll;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		args->disk_type = ped_disk_type_get(arg);
		if (!args->disk_type)
			argp_error(state,
				"Disk type `%s' is not supported; use --list-disk-types to see the supported types");
		break;

	case 'f':
		args->fs_type = ped_file_system_type_get(arg);
		if (!args->fs_type)
			argp_error(state,
				"File system type `%s' is not supported; use --list-fs-types to see the supported types");
		break;

	case 'b':
		args->boot = true;
		break;

	case 'n':
		args->boot = false;
		break;

	case 'a':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"First sector must be greater or equal to 0");
		args->first_sec = ll;
		break;

	case 'l':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"Last sector must be greater or equal to 0");
		args->last_sec = ll;
		break;

	case 'k':
		args->list_disk_types = true;
		break;

	case 's':
		args->list_fs_types = true;
		break;

	case ARGP_KEY_INIT:
		args->dev_filename = NULL;
		args->last_sec = -1;
		break;

	case ARGP_KEY_ARG:
		if (args->dev_filename)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->dev_filename = arg;
		break;

	case ARGP_KEY_END:
		if (args->list_disk_types || args->list_fs_types)
			break;

		if (!args->dev_filename)
			argp_error(state,
				"The disk device was not specified");

		if (args->last_sec < 0)
			argp_error(state,
				"Option --last-sec is required");
		if (args->first_sec > args->last_sec)
			argp_error(state,
				"Option --fist_sec must be less or equal to option --last_sec");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

static void list_disk_types(void)
{
	PedDiskType *type;
	int i = 0;
	printf("Disk types:\n");
	for (type = ped_disk_type_get_next(NULL); type;
		type = ped_disk_type_get_next(type)) {
		printf("%s\t", type->name);
		i++;
		if (i == 5) {
			printf("\n");
			i = 0;
		}
	}
	if (i > 0)
		printf("\n");
	printf("\n");
}

static void list_fs_types(void)
{
	PedFileSystemType *fs_type;
	int i = 0;
	printf("File system types:\n");
	for (fs_type = ped_file_system_type_get_next(NULL); fs_type;
		fs_type = ped_file_system_type_get_next(fs_type)) {
		printf("%s\t", fs_type->name);
		i++;
		if (i == 5) {
			printf("\n");
			i = 0;
		}
	}
	if (i > 0)
		printf("\n");
	printf("\n");
}

/* 0 on failure, 1 otherwise. */
static int fix_disk(PedDevice *dev, PedDiskType *type,
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

int main (int argc, char *argv[])
{
	struct args args = {
		/* Defaults. */
		.list_disk_types	= false,
		.list_fs_types		= false,

		.boot			= true,

		.disk_type		= ped_disk_type_get("msdos"),
		.fs_type		= ped_file_system_type_get("fat32"),
		.first_sec		= 2048,	/* Skip first 1MB. */
	};

	PedDevice *dev;
	int ret;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "fix");

	if (args.list_disk_types)
		list_disk_types();

	if (args.list_fs_types)
		list_fs_types();

	if (args.list_disk_types || args.list_fs_types) {
		/* If the user has asked for the types,
		 * so she doesn't want to fix the drive yet.
		 */
		return 0;
	}

	/* XXX If @dev is a partition, refer the user to
	 * the disk of this partition.
	 */
	dev = ped_device_get(args.dev_filename);
	if (!dev)
		return 1;

	ret = !fix_disk(dev, args.disk_type, args.fs_type, args.boot,
		args.first_sec, args.last_sec);
	printf("Drive `%s' was successfully fixed\n", args.dev_filename);
	ped_device_destroy(dev);
	return ret;
}
