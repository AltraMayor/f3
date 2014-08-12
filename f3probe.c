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
	{"debug-type",		't',	"TYPE",		OPTION_HIDDEN,
		"Set the type of the fake flash",	0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		debug;
	enum fake_type	fake_type;
	/* 2 bytes free. */
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

	case 't':
		if (!strcmp(arg, "good"))
			args->fake_type = FKTY_GOOD;
		else if (!strcmp(arg, "limbo"))
			args->fake_type = FKTY_LIMBO;
		else if (!strcmp(arg, "wraparound"))
			args->fake_type = FKTY_WRAPAROUND;
		else
			argp_error(state,
				"Fake type must be either `good', `limbo' or `wraparound'");
		args->debug = true;
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

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.debug		= false,
		.fake_type	= FKTY_LIMBO,
		.file_size_gb	= 1,
		.fake_size_gb	= 2,
	};
	struct device *dev;
	enum fake_type fake_type;
	int real_size_gb;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);

	dev = args.debug
		? create_file_device(args.filename, args.file_size_gb,
			args.fake_size_gb, args.fake_type)
		: create_block_device(args.filename);
	assert(dev);

	fake_type = test_device(dev, &real_size_gb);
	switch (fake_type) {
	case FKTY_GOOD:
		printf("Nice! The device `%s' is the real thing, and its size is %iGB\n",
			args.filename, real_size_gb);
		break;

	case FKTY_LIMBO:
	case FKTY_WRAPAROUND:
		printf("Bad news: The device `%s' is a counterfeit of type %s, and its *real* size is %iGB\n",
			args.filename, fake_type_to_name(fake_type),
			real_size_gb);
		break;

	default:
		assert(0);
		break;
	}

	free_device(dev);
	return 0;
}
