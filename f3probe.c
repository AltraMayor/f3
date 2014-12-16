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

static struct argp_option options[] = {
	{"debug",		'd',	NULL,		OPTION_HIDDEN,
		"Enable debugging; only needed if none --debug-* option used",
		1},
	{"debug-real-size",	'r',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Real size of the emulated drive",	0},
	{"debug-fake-size",	'f',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Fake size of the emulated drive",	0},
	{"debug-wrap",		'w',	"N",		OPTION_HIDDEN,
		"Wrap parameter of the emulated drive",	0},
	{"debug-block-order",	'b',	"ORDER",	OPTION_HIDDEN,
		"Block size the emulated drive is 2^ORDER Byte",	0},
	{"debug-keep-file",	'k',	NULL,		OPTION_HIDDEN,
		"Don't remove file used for emulating the drive",	0},
	{"debug-unit-test",	'u',	NULL,		OPTION_HIDDEN,
		"Run a unit test; it ignores all other debug options",	0},
	{"destructive",		'n',	NULL,		0,
		"Do not restore blocks of the device after probing it",	2},
	{"min-memory",		'l',	NULL,		0,
		"Trade speed for less use of memory",		0},
	{"reset-type",		's',	"TYPE",		0,
		"Reset method to use during the probe",		0},
	{"time-ops",		't',	NULL,		0,
		"Time reads, writes, and resets",		0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		debug;
	bool		unit_test;
	bool		keep_file;

	/* Behavior options. */
	bool		save;
	bool		min_mem;
	enum reset_type	reset_type;
	bool		time_ops;
	/* 1 free bytes. */

	/* Geometry. */
	uint64_t	real_size_byte;
	uint64_t	fake_size_byte;
	int		wrap;
	int		block_order;
};

static long long arg_to_long_long(const struct argp_state *state,
	const char *arg)
{
	char *end;
	long long ll = strtoll(arg, &end, 0);
	if (end == arg)
		argp_error(state, "An integer must be provided");

	/* Deal with units. */
	switch (*end) {
	case 's':
	case 'S': /* Sectors */
		ll <<= 9;
		end++;
		break;

	case 'k':
	case 'K': /* KB */
		ll <<= 10;
		end++;
		break;

	case 'm':
	case 'M': /* MB */
		ll <<= 20;
		end++;
		break;

	case 'g':
	case 'G': /* GB */
		ll <<= 30;
		end++;
		break;

	case 't':
	case 'T': /* TB */
		ll <<= 40;
		end++;
		break;
	}

	if (*end)
		argp_error(state, "`%s' is not an integer", arg);
	return ll;
}

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

	case 'b':
		ll = arg_to_long_long(state, arg);
		if (ll < 9 || ll > 20)
			argp_error(state,
				"Block order must be in the interval [9, 20]");
		args->block_order = ll;
		args->debug = true;
		break;

	case 'k':
		args->keep_file = true;
		break;

	case 'u':
		args->unit_test = true;
		break;

	case 'n':
		args->save = false;
		break;

	case 'l':
		args->min_mem = true;
		break;

	case 's':
		ll = arg_to_long_long(state, arg);
		if (ll < 0 || ll >= RT_MAX)
			argp_error(state,
				"Reset type must be in the interval [0, %i]",
				RT_MAX - 1);
		args->reset_type = ll;
		break;

	case 't':
		args->time_ops = true;
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
				args->fake_size_byte, args->wrap,
				args->block_order))
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
	int		block_order;
};

static const struct unit_test_item ftype_to_params[] = {
	/* Smallest good drive. */
	{1ULL << 20,	1ULL << 20,	20,	9},

	/* Good, 4KB-block, 1GB drive. */
	{1ULL << 30,	1ULL << 30,	30,	12},

	/* Bad drive. */
	{0,		1ULL << 30,	30,	9},

	/* Geometry of a real limbo drive. */
	{1777645568ULL,	32505331712ULL,	35,	9},

	/* Wraparound drive. */
	{1ULL << 31,	1ULL << 34,	31,	9},

	/* Chain drive. */
	{1ULL << 31,	1ULL << 34,	32,	9},

	/* Extreme case for memory usage (limbo drive). */
	{1ULL << 20,	1ULL << 40,	40,	9},
};

#define UNIT_TEST_N_CASES \
	((int)(sizeof(ftype_to_params)/sizeof(struct unit_test_item)))

static int unit_test(const char *filename)
{
	int i, success = 0;
	for (i = 0; i < UNIT_TEST_N_CASES; i++) {
		const struct unit_test_item *item = &ftype_to_params[i];
		enum fake_type origin_type = dev_param_to_type(
			item->real_size_byte, item->fake_size_byte,
			item->wrap, item->block_order);
		double f_real = item->real_size_byte;
		double f_fake = item->fake_size_byte;
		const char *unit_real = adjust_unit(&f_real);
		const char *unit_fake = adjust_unit(&f_fake);

		enum fake_type fake_type;
		uint64_t real_size_byte, announced_size_byte;
		int wrap, block_order, max_probe_blocks;
		struct device *dev;

		dev = create_file_device(filename, item->real_size_byte,
			item->fake_size_byte, item->wrap, item->block_order,
			false);
		assert(dev);
		max_probe_blocks = probe_device_max_blocks(dev);
		assert(!probe_device(dev, &real_size_byte, &announced_size_byte,
			&wrap, &block_order));
		free_device(dev);
		fake_type = dev_param_to_type(real_size_byte,
			announced_size_byte, wrap, block_order);

		/* Report */
		printf("Test %i\t\ttype/real size/fake size/module/block size\n",
			i + 1);
		printf("\t\t%s/%.2f %s/%.2f %s/2^%i Byte/2^%i Byte\n",
			fake_type_to_name(origin_type),
			f_real, unit_real, f_fake, unit_fake, item->wrap,
			item->block_order);
		if (real_size_byte == item->real_size_byte &&
			announced_size_byte == item->fake_size_byte &&
			wrap == item->wrap &&
			block_order == item->block_order) {
			success++;
			printf("\t\tPerfect!\tMax # of probed blocks: %i\n\n",
				max_probe_blocks);
		} else {
			double ret_f_real = real_size_byte;
			double ret_f_fake = announced_size_byte;
			const char *ret_unit_real = adjust_unit(&ret_f_real);
			const char *ret_unit_fake = adjust_unit(&ret_f_fake);
			printf("\tError\t%s/%.2f %s/%.2f %s/2^%i Byte/2^%i Byte\n\n",
				fake_type_to_name(fake_type),
				ret_f_real, ret_unit_real,
				ret_f_fake, ret_unit_fake, wrap, block_order);
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

static void report_size(const char *prefix, uint64_t bytes)
{
	double f = bytes;
	const char *unit = adjust_unit(&f);
	assert(bytes % SECTOR_SIZE == 0);
	printf("%s %.2f %s (%" PRIu64 " sector)\n", prefix, f, unit,
		bytes / SECTOR_SIZE);
}

static void report_order(const char *prefix, int order)
{
	double f = (1ULL << order);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (2^%i Byte)\n", prefix, f, unit, order);
}

static void report_ops(const char *op, uint64_t count, uint64_t time_us)
{
	printf("Probe %s op: count=%" PRIu64
		", total time=%.2fs, avg op time=%.2fms\n",
		op, count, time_us / 1e6, (time_us / count) / 1e3);
}

static int test_device(struct args *args)
{
	struct timeval t1, t2;
	double time_s;
	struct device *dev, *pdev;
	enum fake_type fake_type;
	uint64_t real_size_byte, announced_size_byte;
	int wrap, block_order;
	uint64_t read_count, read_time_us;
	uint64_t write_count, write_time_us;
	uint64_t reset_count, reset_time_us;

	dev = args->debug
		? create_file_device(args->filename, args->real_size_byte,
			args->fake_size_byte, args->wrap, args->block_order,
			args->keep_file)
		: create_block_device(args->filename, args->reset_type);
	if (!dev) {
		fprintf(stderr, "\nApplication cannot continue, finishing...\n");
		exit(1);
	}

	if (args->time_ops) {
		pdev = create_perf_device(dev);
		assert(pdev);
		dev = pdev;
	} else {
		pdev = NULL;
	}

	if (args->save) {
		dev = create_safe_device(dev, probe_device_max_blocks(dev),
			args->min_mem);
		if (!dev) {
			if (!args->min_mem)
				fprintf(stderr, "Out of memory, try `f3probe --min-memory %s'\n",
					args->filename);
			else
				fprintf(stderr, "Out of memory, try `f3probe --destructive %s'\nPlease back your data up before using option --destructive.\nAlternatively, you could use a machine with more memory to run f3probe.\n",
					args->filename);
			exit(1);
		}
		assert(dev);
	}

	assert(!gettimeofday(&t1, NULL));
	/* XXX Have a better error handling to recover
	 * the state of the drive.
	 */
	assert(!probe_device(dev, &real_size_byte, &announced_size_byte,
		&wrap, &block_order));
	assert(!gettimeofday(&t2, NULL));
	/* Keep free_device() as close of probe_device() as possible to
	 * make sure that the written blocks are recovered when
	 * @args->save is true.
	 */
	if (args->time_ops)
		perf_device_sample(pdev,
			&read_count, &read_time_us,
			&write_count, &write_time_us,
			&reset_count, &reset_time_us);
	if (args->save) {
		printf("Probe finished, recovering blocks...");
		fflush(stdout);
	}
	free_device(dev);
	if (args->save)
		printf(" Done\n\n");

	fake_type = dev_param_to_type(real_size_byte, announced_size_byte,
		wrap, block_order);
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
	 report_size("\t   *Real* size:", real_size_byte);
	 report_size("\tAnnounced size:", announced_size_byte);
	report_order("\t        Module:", wrap);
	report_order("\t    Block size:", block_order);
	printf("\nProbe time: %.2f seconds\n", time_s);

	if (args->time_ops) {
		report_ops("read", read_count, read_time_us);
		report_ops("write", write_count, write_time_us);
		report_ops("reset", reset_count, reset_time_us);
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.debug		= false,
		.unit_test	= false,
		.keep_file	= false,
		.save		= true,
		.min_mem	= false,
		.reset_type	= RT_DEFAULT,
		.time_ops	= false,
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
		.block_order	= 9,
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "probe");

	if (args.unit_test)
		return unit_test(args.filename);
	return test_device(&args);
}
