#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/time.h>

#include "version.h"
#include "libprobe.h"

/* XXX Refactor utils library since f3probe barely uses it. */
#include "utils.h"

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
	{"debug",		'd',	NULL,		OPTION_HIDDEN,
		"Enable debugging; only needed if none --debug-* option used",
		1},
	{"debug-real-size",	'r',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Real size of the emulated flash",	0},
	{"debug-fake-size",	'f',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Fake size of the emulated flash",	0},
	{"debug-wrap",		'w',	"N",		OPTION_HIDDEN,
		"Wrap parameter of the emulated flash",	0},
	{"debug-unit-test",	'u',	NULL,		OPTION_HIDDEN,
		"Run a unit test; it ignores all other debug options",	0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		unit_test;
	bool		debug;
	/* 2 bytes free. */
	uint64_t	real_size_byte;
	uint64_t	fake_size_byte;
	int		wrap;
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

/* XXX Add a friendly way to enter real and fake sizes: 1, 1k, 1m, 1g... */
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		args->debug = true;
		break;

	case 'r':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"Real size must be greater or equal to zero");
		args->real_size_byte = ll;
		args->debug = true;
		break;

	case 'f':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"Fake size must be greater or equal to zero");
		args->fake_size_byte = ll;
		args->debug = true;
		break;

	case 'w':
		ll = arg_to_long_long(state, arg);
		if (ll < 0 || ll >= 64)
			argp_error(state,
				"Wrap must be in the interval [0, 63]");
		args->wrap = ll;
		args->debug = true;
		break;

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
		if (args->debug &&
			!dev_param_valid(args->real_size_byte,
				args->fake_size_byte, args->wrap))
			argp_error(state,
				"The debugging parameters are not valid");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

struct unit_test_item {
	uint64_t	real_size_byte;
	uint64_t	fake_size_byte;
	int		wrap;
};

static const struct unit_test_item ftype_to_params[] = {
	{1ULL << 30,	1ULL << 30,	30},
	{0,		1ULL << 30,	30},
	{1ULL << 30,	1ULL << 34,	34},
	{1ULL << 31,	1ULL << 34,	31},
	{1ULL << 31,	1ULL << 34,	32},
};

#define UNIT_TEST_N_CASES \
	((int)(sizeof(ftype_to_params)/sizeof(struct unit_test_item)))

static int unit_test(const char *filename)
{
	int i, success = 0;
	for (i = 0; i < UNIT_TEST_N_CASES; i++) {
		const struct unit_test_item *item = &ftype_to_params[i];
		enum fake_type origin_type = dev_param_to_type(
			item->real_size_byte, item->fake_size_byte, item->wrap);
		double f_real = item->real_size_byte;
		double f_fake = item->fake_size_byte;
		const char *unit_real = adjust_unit(&f_real);
		const char *unit_fake = adjust_unit(&f_fake);

		enum fake_type fake_type;
		uint64_t real_size_byte, announced_size_byte;
		int wrap;
		struct device *dev;

		dev = create_file_device(filename, item->real_size_byte,
			item->fake_size_byte, item->wrap);
		assert(dev);
		probe_device(dev, &real_size_byte, &announced_size_byte, &wrap);
		free_device(dev);
		fake_type = dev_param_to_type(real_size_byte,
			announced_size_byte, wrap);

		/* Report */
		printf("Test %i (type %s, real-size=%.2f %s, fake-size=%.2f %s, module=2^%i)\n",
			i + 1, fake_type_to_name(origin_type),
			f_real, unit_real, f_fake, unit_fake, item->wrap);
		if (real_size_byte == item->real_size_byte &&
			announced_size_byte == item->fake_size_byte &&
			wrap == item->wrap) {
			success++;
			printf("\tPerfect!\n\n");
		} else {
			double ret_f_real = real_size_byte;
			double ret_f_fake = announced_size_byte;
			const char *ret_unit_real = adjust_unit(&ret_f_real);
			const char *ret_unit_fake = adjust_unit(&ret_f_fake);
			printf("\tFound type %s, real size %.2f %s, fake size %.2f %s, and module 2^%i\n\n",
				fake_type_to_name(fake_type),
				ret_f_real, ret_unit_real,
				ret_f_fake, ret_unit_fake, wrap);
		}
	}

	printf("SUMMARY: ");
	if (success == UNIT_TEST_N_CASES)
		printf("Perfect!\n");
	else
		printf("Missed %i tests out of %i\n",
			UNIT_TEST_N_CASES - success, UNIT_TEST_N_CASES);
	return 0;
}

static void report(const char *prefix, uint64_t bytes)
{
	double f = (double)bytes;
	const char *unit = adjust_unit(&f);
	assert(bytes % SECTOR_SIZE == 0);
	printf("%s %.2f %s (%" PRIu64 " sectors)\n", prefix, f, unit,
		bytes / SECTOR_SIZE);
}

static int test_device(struct args *args)
{
	struct timeval t1, t2;
	double time_s;
	struct device *dev;
	enum fake_type fake_type;
	uint64_t real_size_byte, announced_size_byte;
	int wrap;

	dev = args->debug
		? create_file_device(args->filename, args->real_size_byte,
			args->fake_size_byte, args->wrap)
		: create_block_device(args->filename);
	assert(dev);
	assert(!gettimeofday(&t1, NULL));
	probe_device(dev, &real_size_byte, &announced_size_byte, &wrap);
	assert(!gettimeofday(&t2, NULL));
	free_device(dev);

	fake_type = dev_param_to_type(real_size_byte, announced_size_byte,
		wrap);
	switch (fake_type) {
	case FKTY_GOOD:
		printf("Good news: The device `%s' is the real thing\n",
			args->filename);
		break;

	case FKTY_BAD:
	case FKTY_LIMBO:
	case FKTY_WRAPAROUND:
	case FKTY_CHAIN:
		printf("Bad news: The device `%s' is a counterfeit of type %s\n",
			args->filename, fake_type_to_name(fake_type));
		break;

	default:
		assert(0);
		break;
	}

	time_s = (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec)/1000000.;
	printf("\nDevice geometry:\n");
	report("\t   *Real* size:", real_size_byte);
	report("\tAnnounced size:", announced_size_byte);
	printf("\t        Module: 2^%i\n", wrap);
	printf("\nProbe time: %.2f seconds\n", time_s);
	return 0;
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.unit_test	= false,
		.debug		= false,
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "probe");

	if (args.unit_test)
		return unit_test(args.filename);
	return test_device(&args);
}
