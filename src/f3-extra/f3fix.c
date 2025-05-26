#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <argp.h>

#include <f3/version.h>
#include <f3/libutils.h>
#include <f3/platform/partition.h>

/* Sentinel for unset last sector. */
#define SEC_UNSET ((uint64_t)UINT64_MAX)

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
	bool	boot_flag_seen;

	/* 28 free bytes. */

	const char		*dev_filename;
	const char		*disk_type;
	const char		*fs_type;
	uint64_t		first_sec;
	uint64_t		last_sec;
};

static uint64_t arg_to_uint64(const struct argp_state *state, const char *arg)
{
	char *end;
	errno = 0;

	if (*arg == '-')
		argp_error(state, "value must be non-negative");

	unsigned long long v = strtoull(arg, &end, 0);

	/* Reject empty string, garbage suffix, or overflow. */
	if (end == arg || *end || errno == ERANGE)
		argp_error(state, "`%s' is not a valid unsigned integer", arg);

#if ULLONG_MAX > UINT64_MAX
	/* Reject overflow in exotic systems. */
	if (v > UINT64_MAX)
		argp_error(state, "`%s' is too large; maximum is %llu",
			   arg, (unsigned long long)UINT64_MAX);
#endif
	return (uint64_t)v;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;

	switch (key) {
	case 'd': /* disk-type */
		args->disk_type = arg;
		if (!is_valid_disk_type(arg))
			argp_error(state,
				"Disk type `%s' is not supported; "
				"use --list-disk-types to see the supported types",
				arg);
		break;

	case 'f': /* fs-type */
		args->fs_type = arg;
		if (!is_valid_fs_type(arg))
			argp_error(state,
				"File system type `%s' is not supported; "
				"use --list-fs-types to see the supported types",
				arg);
		break;

	case 'b': /* boot */
	case 'n': /* no-boot */
		if (args->boot_flag_seen)
			argp_error(state,
				"Options --boot and --no-boot are mutually exclusive "
				"and may be given only once");
		args->boot_flag_seen = true;
		args->boot = (key == 'b');
		break;

	case 'a': /* first-sec */
		args->first_sec = arg_to_uint64(state, arg);
		break;

	case 'l': /* last-sec */
		args->last_sec = arg_to_uint64(state, arg);
		break;

	case 'k': /* list-disk-types */
		args->list_disk_types = true;
		break;

	case 's': /* list-fs-types */
		args->list_fs_types = true;
		break;

	case ARGP_KEY_INIT:
		args->dev_filename = NULL;
		args->last_sec = SEC_UNSET;
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

		if (args->last_sec == SEC_UNSET)
			argp_error(state,
				"Option --last-sec is required");
		if (args->first_sec > args->last_sec)
			argp_error(state,
				"Option --first_sec must be less or equal to option --last_sec");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

static void print_array(const char *title, size_t (*getter)(char ***))
{
	char **list = NULL;
	size_t n = getter(&list);
	if (!list) {
		fprintf(stderr, "Failed to obtain %s\n", title);
		return;
	}

	printf("%s:\n", title);
	for (size_t i = 0; list[i]; ++i) {
		printf("%s\t", list[i]);
		if (i % 5 == 4)
			putchar('\n');
	}
	if (n % 5)
		putchar('\n');
	putchar('\n');

	partition_free_types_array(list);
}

static void list_disk_types(void)
{
	print_array("Disk types", partition_list_disk_types);
}

static void list_fs_types(void)
{
	print_array("File system types", partition_list_fs_types);
}

int main (int argc, char *argv[])
{
	struct args args = {
		/* Defaults. */
		.list_disk_types	= false,
		.list_fs_types		= false,

		.boot			= true,
		.boot_flag_seen		= false,

		.disk_type		= "msdos",
		.fs_type		= "fat32",
		.first_sec		= 2048,	/* Skip first 1MB. */
	};

	struct device *bdev;
	struct partition_options opt;
	int err;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "fix");

	if (args.list_disk_types)
		list_disk_types();

	if (args.list_fs_types)
		list_fs_types();

	if (args.list_disk_types || args.list_fs_types) {
		/* If the user has asked for the types,
		 * she doesn't want to fix the drive yet.
		 */
		return 0;
	}

	/* XXX If @dev is a partition, refer the user to
	 * the disk of this partition.
	 * create_block_device() will fail if @dev is a partition.
	 */
	bdev = create_block_device(args.dev_filename, RT_NONE);
	if (!bdev) {
		fprintf(stderr, "Failed to open device %s\n", args.dev_filename);
		return 1;
	}

	/* Get and copy the filename before freeing the device. */
	const char *dev_filename = strdup(dev_get_filename(bdev));
	if (!dev_filename) {
		fprintf(stderr, "Failed to duplicate device filename\n");
		free_device(bdev);
		return 1;
	}
	free_device(bdev);

	opt = (struct partition_options){
		.disk_type		= args.disk_type,
		.fs_type		= args.fs_type,
		.boot			= args.boot,
		.first_sector	= args.first_sec,
		.last_sector	= args.last_sec,
	};
	err = partition_create(dev_filename, &opt);
	if (err == 0) {
		printf("Drive `%s' was successfully fixed\n", dev_filename);
	} else {
		fprintf(stderr, "Failed to fix drive `%s'\n", dev_filename);
	}
	return err;
}
