#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <stdbool.h>
#include <assert.h>

#include "version.h"
#include "libprobe.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Probe " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<BLOCK_DEV>";

static char doc[] = "F3 Probe -- probe a block device for "
	"counterfeit flash memory. If counterfeit, "
	"f3probe identifies the fake type and real memory size";

/* XXX Have an option to back up blocks on memory, or in a file to be
 * restored at the end of the test, or even later in the case of the file.
 *
 * The default should be off, so user must know that she is calling for
 * this extra function; it is important for when things go wrong during
 * the test.
 *
 * f3probe should preallocate memory to avoid failing during the test
 * when memory is low.
 * If memory cannot be preallocated, suggest using a file.
 *
 * When this option is not being used, warning that all data will be lost,
 * ask for confirmation, and suggest the option.
 * A way to disable confirmation is necessary for scripting.
 */

static struct argp_option options[] = {
	{"debug-file-size",	'd',	"SIZE_GB",	OPTION_HIDDEN,
		"Enable debuging with a regular file",	1},
	{"debug-fake-size",	'f',	"SIZE_GB",	OPTION_HIDDEN,
		"Fake size of the emulated flash",	0},
	{"debug-type",		't',	"N",		OPTION_HIDDEN,
		"Set the type of the fake flash",	0},
	{"debug-unit-test",	'u',	NULL,		OPTION_HIDDEN,
		"Run a unit test; it ignores all other debug options",	0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		unit_test;
	bool		debug;
	enum fake_type	fake_type;
	/* 1 bytes free. */
	int		file_size_gb;
	int		fake_size_gb;
};

static long arg_to_long(const struct argp_state *state, const char *arg)
{
	char *end;
	long l = strtol(arg, &end, 0);
	if (!arg)
		argp_error(state, "An integer must be provided");
	if (!*arg || *end)
		argp_error(state, "`%s' is not an integer", arg);
	return l;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;

	switch (key) {
	case 'd':
		args->file_size_gb = arg_to_long(state, arg);
		if (args->file_size_gb < 1)
			argp_error(state,
				"Size of the regular file to be used for debugging must be at least 1GB");
		args->debug = true;
		break;

	case 'f':
		args->fake_size_gb = arg_to_long(state, arg);
		if (args->fake_size_gb < 1)
			argp_error(state,
				"Fake size of the emulated flash must be at least 1GB");
		args->debug = true;
		break;

	case 't': {
		long l = arg_to_long(state, arg);
		if (l < FKTY_GOOD || l >= FKTY_MAX)
			argp_error(state,
				"Fake type must be a number in the interval [%i, %i]",
				FKTY_GOOD, FKTY_MAX);
		args->fake_type = l;
		args->debug = true;
		break;
	}

	case 'u':
		args->unit_test = true;
		break;

	case ARGP_KEY_INIT:
		args->filename = NULL;
		break;

	case ARGP_KEY_ARG:
		if (args->filename)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->filename = arg;
		break;

	case ARGP_KEY_END:
		if (!args->filename)
			argp_error(state,
				"The block device was not specified");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

struct unit_test_item {
	enum fake_type fake_type;
	int file_size_gb;
	int fake_size_gb;
};

static const struct unit_test_item ftype_to_params[] = {
	{FKTY_GOOD,		1, 1},
	{FKTY_BAD,		0, 1},
	{FKTY_LIMBO,		1, 2},
	{FKTY_WRAPAROUND,	1, 2},
	{FKTY_WRAPAROUND,	2, 3},
};

#define UNIT_TEST_N_CASES \
	((int)(sizeof(ftype_to_params)/sizeof(struct unit_test_item)))

static int unit_test(const char *filename)
{
	int i, success = 0;
	for (i = 0; i < UNIT_TEST_N_CASES; i++) {
		enum fake_type fake_type;
		int real_size_gb, good = 0;
		struct device *dev = create_file_device(filename,
			ftype_to_params[i].file_size_gb,
			ftype_to_params[i].fake_size_gb,
			ftype_to_params[i].fake_type);
		assert(dev);
		fake_type = probe_device(dev, &real_size_gb);
		free_device(dev);

		/* Report */
		printf("Test %i (type %s, file-size=%iGB, fake-size=%iGB): ",
			i + 1, fake_type_to_name(ftype_to_params[i].fake_type),
			ftype_to_params[i].file_size_gb,
			ftype_to_params[i].fake_size_gb);
		if (fake_type == ftype_to_params[i].fake_type) {
			if (real_size_gb == ftype_to_params[i].file_size_gb) {
				good = 1;
				success++;
				printf("Perfect!\n");
			} else {
				printf("Correct type, wrong size\n");
			}
		} else if (real_size_gb == ftype_to_params[i].file_size_gb) {
			printf("Wrong type, correct size\n");
		} else {
			printf("Got it all wrong\n");
		}
		if (!good)
			printf("\tFound type %s and real size %iGB\n",
				fake_type_to_name(fake_type), real_size_gb);
		printf("\n");
	}

	printf("SUMMARY: ");
	if (success == UNIT_TEST_N_CASES)
		printf("Perfect!\n");
	else
		printf("Missed %i tests out of %i\n",
			UNIT_TEST_N_CASES - success, UNIT_TEST_N_CASES);
	return 0;
}

static int test_device(struct args *args)
{
	struct device *dev;
	enum fake_type fake_type;
	int real_size_gb;

	dev = args->debug
		? create_file_device(args->filename, args->file_size_gb,
			args->fake_size_gb, args->fake_type)
		: create_block_device(args->filename);
	assert(dev);

	fake_type = probe_device(dev, &real_size_gb);
	switch (fake_type) {
	case FKTY_GOOD:
		printf("Nice! The device `%s' is the real thing, and its size is %iGB\n",
			args->filename, real_size_gb);
		break;

	case FKTY_LIMBO:
	case FKTY_WRAPAROUND:
		printf("Bad news: The device `%s' is a counterfeit of type %s, and its *real* size is %iGB\n",
			args->filename, fake_type_to_name(fake_type),
			real_size_gb);
		break;

	default:
		assert(0);
		break;
	}

	free_device(dev);
	return 0;
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.unit_test	= false,
		.debug		= false,
		.fake_type	= FKTY_LIMBO,
		.file_size_gb	= 1,
		.fake_size_gb	= 2,
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);

	if (args.unit_test)
		return unit_test(args.filename);
	return test_device(&args);
}
