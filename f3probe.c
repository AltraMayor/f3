#define _POSIX_C_SOURCE 200809L

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
#include "libutils.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Probe " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<DISK_DEV>";

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
		"Block size of the emulated drive is 2^ORDER Bytes",	0},
	{"debug-cache-order",	'c',	"ORDER",	OPTION_HIDDEN,
		"Cache size of the emulated drive is 2^ORDER blocks",	0},
	{"debug-strict-cache",	'o',	NULL,		OPTION_HIDDEN,
		"Force the cache to be strict",				0},
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
	int		cache_order;
	int		strict_cache;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		args->debug = true;
		break;

	case 'r':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"Real size must be greater or equal to zero");
		args->real_size_byte = ll;
		args->debug = true;
		break;

	case 'f':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"Fake size must be greater or equal to zero");
		args->fake_size_byte = ll;
		args->debug = true;
		break;

	case 'w':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0 || ll >= 64)
			argp_error(state,
				"Wrap must be in the interval [0, 63]");
		args->wrap = ll;
		args->debug = true;
		break;

	case 'b':
		ll = arg_to_ll_bytes(state, arg);
		if (ll != 0 && (ll < 9 || ll > 20))
			argp_error(state,
				"Block order must be in the interval [9, 20] or be zero");
		args->block_order = ll;
		args->debug = true;
		break;

	case 'c':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < -1 || ll > 64)
			argp_error(state,
				"Cache order must be in the interval [-1, 64]");
		args->cache_order = ll;
		args->debug = true;
		break;

	case 'o':
		args->strict_cache = true;
		args->debug = true;
		break;

	case 'k':
		args->keep_file = true;
		args->debug = true;
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
		ll = arg_to_ll_bytes(state, arg);
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
				"The disk device was not specified");
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
	int		cache_order;
	int		strict_cache;
};

static const struct unit_test_item ftype_to_params[] = {
	/* Smallest good drive. */
	{1ULL << 21,	1ULL << 21,	21,	9,	-1,	false},

	/* Good, 4KB-block, 1GB drive. */
	{1ULL << 30,	1ULL << 30,	30,	12,	-1,	false},

	/* Bad drive. */
	{0,		1ULL << 30,	30,	9,	-1,	false},

	/* Geometry of a real limbo drive. */
	{1777645568ULL,	32505331712ULL,	35,	9,	-1,	false},

	/* Wraparound drive. */
	{1ULL << 31,	1ULL << 34,	31,	9,	-1,	false},

	/* Chain drive. */
	{1ULL << 31,	1ULL << 34,	32,	9,	-1,	false},

	/* Extreme case for memory usage (limbo drive). */
	{(1ULL<<20)+512,1ULL << 40,	40,	9,	-1,	false},

	/* Geomerty of a real limbo drive with 256MB of strict cache. */
	{7600799744ULL,	67108864000ULL,	36,	9,	19,	true},

	/* The drive before with a non-strict cache. */
	{7600799744ULL,	67108864000ULL,	36,	9,	19,	false},

	/* The devil drive I. */
	{0,		1ULL << 40,	40,	9,	21,	true},

	/* The devil drive II. */
	{0,		1ULL << 40,	40,	9,	21,	false},
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
		uint64_t item_cache_byte = item->cache_order < 0 ? 0 :
			1ULL << (item->cache_order + item->block_order);
		double f_real = item->real_size_byte;
		double f_fake = item->fake_size_byte;
		double f_cache = item_cache_byte;
		const char *unit_real = adjust_unit(&f_real);
		const char *unit_fake = adjust_unit(&f_fake);
		const char *unit_cache = adjust_unit(&f_cache);

		enum fake_type fake_type;
		uint64_t real_size_byte, announced_size_byte, cache_size_block;
		int wrap, need_reset, block_order, max_probe_blocks;
		struct device *dev;

		dev = create_file_device(filename, item->real_size_byte,
			item->fake_size_byte, item->wrap, item->block_order,
			item->cache_order, item->strict_cache, false);
		assert(dev);
		max_probe_blocks = probe_device_max_blocks(dev);
		assert(!probe_device(dev, &real_size_byte, &announced_size_byte,
			&wrap, &cache_size_block, &need_reset, &block_order));
		free_device(dev);
		fake_type = dev_param_to_type(real_size_byte,
			announced_size_byte, wrap, block_order);

		/* Report */
		printf("Test %i\t\ttype/real size/fake size/module/cache size/reset/block size\n",
			i + 1);
		printf("\t\t%s/%.2f %s/%.2f %s/2^%i Byte/%.2f %s/no/2^%i Byte\n",
			fake_type_to_name(origin_type),
			f_real, unit_real, f_fake, unit_fake, item->wrap,
			f_cache, unit_cache, item->block_order);
		if (real_size_byte == item->real_size_byte &&
			announced_size_byte == item->fake_size_byte &&
			wrap == item->wrap &&
			/* probe_device() returns an upper bound of
			 * the cache size.
			 */
			item_cache_byte <= (cache_size_block << block_order) &&
			!need_reset &&
			block_order == item->block_order) {
			success++;
			printf("\t\tPerfect!\tMax # of probed blocks: %i\n\n",
				max_probe_blocks);
		} else {
			double ret_f_real = real_size_byte;
			double ret_f_fake = announced_size_byte;
			double ret_f_cache = cache_size_block << block_order;
			const char *ret_unit_real = adjust_unit(&ret_f_real);
			const char *ret_unit_fake = adjust_unit(&ret_f_fake);
			const char *ret_unit_cache = adjust_unit(&ret_f_cache);
			printf("\tError\t%s/%.2f %s/%.2f %s/2^%i Byte/%.2f %s/%s/2^%i Byte\n\n",
				fake_type_to_name(fake_type),
				ret_f_real, ret_unit_real,
				ret_f_fake, ret_unit_fake, wrap,
				ret_f_cache, ret_unit_cache,
				need_reset ? "yes" : "no", block_order);
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

static void report_size(const char *prefix, uint64_t bytes, int block_order)
{
	double f = bytes;
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " blocks)\n", prefix, f, unit,
		bytes >> block_order);
}

static void report_order(const char *prefix, int order)
{
	double f = (1ULL << order);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (2^%i Bytes)\n", prefix, f, unit, order);
}

static void report_cache(const char *prefix, uint64_t cache_size_block,
	int need_reset, int order)
{
	double f = (cache_size_block << order);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " blocks), need-reset=%s\n",
		prefix, f, unit, cache_size_block,
		need_reset ? "yes" : "no");
}

static void report_probe_time(const char *prefix, uint64_t usec)
{
	char str[TIME_STR_SIZE];
	usec_to_str(usec, str);
	printf("%s %s\n", prefix, str);
}

static void report_ops(const char *op, uint64_t count, uint64_t time_us)
{
	char str1[TIME_STR_SIZE], str2[TIME_STR_SIZE];
	usec_to_str(time_us, str1);
	usec_to_str(count > 0 ? time_us / count : 0, str2);
	printf("%10s: %s / %" PRIu64 " = %s\n", op, str1, count, str2);
}

static int test_device(struct args *args)
{
	struct timeval t1, t2;
	struct device *dev, *pdev, *sdev;
	enum fake_type fake_type;
	uint64_t real_size_byte, announced_size_byte, cache_size_block;
	int wrap, need_reset, block_order;
	uint64_t read_count, read_time_us;
	uint64_t write_count, write_time_us;
	uint64_t reset_count, reset_time_us;
	const char *final_dev_filename;

	dev = args->debug
		? create_file_device(args->filename, args->real_size_byte,
			args->fake_size_byte, args->wrap, args->block_order,
			args->cache_order, args->strict_cache, args->keep_file)
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

	sdev = NULL;
	if (args->save) {
		sdev = create_safe_device(dev,
			probe_device_max_blocks(dev), args->min_mem);
		if (!sdev) {
			if (!args->min_mem)
				fprintf(stderr, "Out of memory, try `f3probe --min-memory %s'\n",
					dev_get_filename(dev));
			else
				fprintf(stderr, "Out of memory, try `f3probe --destructive %s'\nPlease back your data up before using option --destructive.\nAlternatively, you could use a machine with more memory to run f3probe.\n",
					dev_get_filename(dev));
			exit(1);
		}
		dev = sdev;
	}

	printf("WARNING: Probing normally takes from a few seconds to 15 minutes, but\n");
	printf("         it can take longer. Please be patient.\n\n");

	assert(!gettimeofday(&t1, NULL));
	/* XXX Have a better error handling to recover
	 * the state of the drive.
	 */
	assert(!probe_device(dev, &real_size_byte, &announced_size_byte,
		&wrap, &cache_size_block, &need_reset, &block_order));
	assert(!gettimeofday(&t2, NULL));

	if (!args->debug && args->reset_type == RT_MANUAL_USB) {
		printf("CAUTION\t\tCAUTION\t\tCAUTION\n");
		printf("No more resets are needed, so do not unplug the drive\n");
		fflush(stdout);
	}

	/* Keep free_device() as close of probe_device() as possible to
	 * make sure that the written blocks are recovered when
	 * @args->save is true.
	 */
	if (args->time_ops)
		perf_device_sample(pdev,
			&read_count, &read_time_us,
			&write_count, &write_time_us,
			&reset_count, &reset_time_us);
	if (sdev) {
		uint64_t very_last_pos = real_size_byte >> block_order;
		printf("Probe finished, recovering blocks...");
		fflush(stdout);
		if (very_last_pos > 0) {
			very_last_pos--;
			sdev_recover(sdev, very_last_pos);
		}
		printf(" Done\n");
		sdev_flush(sdev);
	}

	final_dev_filename = strdup(dev_get_filename(dev));
	assert(final_dev_filename);
	free_device(dev);

	if (args->save || (!args->debug && args->reset_type == RT_MANUAL_USB))
		printf("\n");

	if (strcmp(args->filename, final_dev_filename))
		printf("WARNING: device `%s' moved to `%s' due to the resets\n\n",
			args->filename, final_dev_filename);

	fake_type = dev_param_to_type(real_size_byte, announced_size_byte,
		wrap, block_order);
	switch (fake_type) {
	case FKTY_GOOD:
		printf("Good news: The device `%s' is the real thing\n",
			final_dev_filename);
		break;

	case FKTY_BAD:
		printf("Bad news: The device `%s' is damaged\n",
			final_dev_filename);
		break;

	case FKTY_LIMBO:
	case FKTY_WRAPAROUND:
	case FKTY_CHAIN: {
		uint64_t last_good_sector = (real_size_byte >> 9) - 1;
		assert(block_order >= 9);
		printf("Bad news: The device `%s' is a counterfeit of type %s\n\n"
			"You can \"fix\" this device using the following command:\n"
			"f3fix --last-sec=%" PRIu64 " %s\n",
			final_dev_filename, fake_type_to_name(fake_type),
			last_good_sector, final_dev_filename);
		break;
	}

	default:
		assert(0);
		break;
	}

	printf("\nDevice geometry:\n");
	  report_size("\t         *Usable* size:", real_size_byte,
		block_order);
	  report_size("\t        Announced size:", announced_size_byte,
		block_order);
	 report_order("\t                Module:", wrap);
	 report_cache("\tApproximate cache size:", cache_size_block,
		need_reset, block_order);
	 report_order("\t   Physical block size:", block_order);
	report_probe_time("\nProbe time:", diff_timeval_us(&t1, &t2));

	if (args->time_ops) {
		printf(" Operation: total time / count = avg time\n");
		report_ops("Read", read_count, read_time_us);
		report_ops("Write", write_count, write_time_us);
		report_ops("Reset", reset_count, reset_time_us);
	}

	free((void *)final_dev_filename);
	return fake_type == FKTY_GOOD ? 0 : 100 + fake_type;
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

		/* RT_NONE is the only reliable reset type against fake flash.
		 * See issue #81 for details:
		 * https://github.com/AltraMayor/f3/issues/81
		 *
		 * A side benefit of this reset type is that it works on
		 * non-USB-backed drives, such as card readers that are
		 * commonly built in laptops.
		 * See issue #79 for details:
		 * https://github.com/AltraMayor/f3/issues/79
		 *
		 * A negative side effect is that f3probe runs slower
		 * for cases in which RT_USB would work. But users can
		 * still request the reset type RT_USB by
		 * passing --reset-type=1
		 */
		.reset_type	= RT_NONE,

		.time_ops	= false,
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
		.block_order	= 0,
		.cache_order	= -1,
		.strict_cache	= false,
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "probe");

	if (args.unit_test)
		return unit_test(args.filename);
	return test_device(&args);
}
