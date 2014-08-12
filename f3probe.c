#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <stdbool.h>

/* Argp's global variables. */
const char *argp_program_version = "F3 Probe 1.0";

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
	{"debug-file",	'd',	"SIZE_GB",	OPTION_HIDDEN,
		"Enable debuging with a regular file",	1},
	{"debug-type",	't',	"TYPE",		OPTION_HIDDEN,
		"Set the type of the fake flash",	0},
	{ 0 }
};

enum fake_type {
	FKTY_LIMBO,
	FKTY_WRAPAROUND,
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		debug;
	enum fake_type	fake_type;
	/* 2 bytes free. */
	int		file_size_gb;
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

	case 't':
		if (!strcmp(arg, "limbo"))
			args->fake_type = FKTY_LIMBO;
		else if (!strcmp(arg, "wraparound"))
			args->fake_type = FKTY_WRAPAROUND;
		else
			argp_error(state,
				"Fake type must be either `limbo' or `wraparound'");
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
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);

	/* XXX Test if it's a device, or a partition.
	 * If a partition, warn user, and ask for confirmation before
	 * going ahead.
	 * Suggest how to call f3probe with the correct device name if
	 * the block device is a partition.
	 */

 	/* XXX Test for write access of the block device to give
	 * a nice error message.
	 * If it fails, suggest running f3probe as root.
	 */

	/* TODO */
	printf("File = %s\n", args.filename);
	printf("Debug = %i\n", args.debug);
	printf("Fake type = %i\n", args.fake_type);
	printf("File size = %i GB\n", args.file_size_gb);

	/* XXX Don't write at the very beginning of the card to avoid
	 * losing the partition table.
	 * But write at a random locations to make harder for fake chips
	 * to become "smarter".
	 */

	/* XXX Write random data for testing.
	 * There would be a random seed, and all the other blocks would be
	 * this seed XOR'd with the number of the test.
	 */

	/* XXX Finish testing the last block, and the next one that should fail.
	 * Then report the last block, so user can create the largest partition.
	 */

	return 0;
}
