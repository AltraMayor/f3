#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/time.h>
#include <unistd.h>

#include "version.h"
#include "libprobe.h"
#include "libutils.h"
#include "libdevs.h"

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
	{"debug-real-size",	'a',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Real size of the emulated drive",	0},
	{"debug-fake-size",	'f',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Fake size of the emulated drive",	0},
	{"debug-wrap",		'm',	"N",		OPTION_HIDDEN,
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
	{"time-ops",		't',	NULL,		0,
		"Time reads, writes, and resets",		0},
	{"verbose",		'v',	NULL,		0,
		"Show detailed progress",		0},
	{"show-progress",	'p',	"NUM",		0,
		"Show progress if NUM is not zero",			0},
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
	bool		time_ops;
	bool		verbose;
	bool		show_progress;

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

	case 'a':
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

	case 'm':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0 || ll >= 64)
			argp_error(state,
				"Wrap must be in the interval [0, 63]");
		args->wrap = ll;
		args->debug = true;
		break;

	case 'b':
		ll = arg_to_ll_bytes(state, arg);
		if (ll != 0 && (ll < SECTOR_ORDER || ll > 20))
			argp_error(state,
				"Block order must be in the interval [%i, 20] or be zero",
				SECTOR_ORDER);
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

	case 't':
		args->time_ops = true;
		break;

	case 'v':
		args->verbose = true;
		break;

	case 'p':
		args->show_progress = !!arg_to_ll_bytes(state, arg);
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
	{1ULL << 21,	1ULL << 21,	21,	SECTOR_ORDER,	-1,	false},

	/* Good, 4KB-block, 1GB drive. */
	{1ULL << 30,	1ULL << 30,	30,	12,		-1,	false},

	/* Bad drive. */
	{0,		1ULL << 30,	30,	SECTOR_ORDER,	-1,	false},

	/* Geometry of a real limbo drive. */
	{1777645568ULL,	32505331712ULL,	35,	SECTOR_ORDER,	-1,	false},

	/* Wraparound drive. */
	{1ULL << 31,	1ULL << 34,	31,	SECTOR_ORDER,	-1,	false},

	/* Chain drive. */
	{1ULL << 31,	1ULL << 34,	32,	SECTOR_ORDER,	-1,	false},

	/* Extreme case for memory usage (limbo drive). */
	{(1ULL<<20)+512,1ULL << 40,	40,	SECTOR_ORDER,	-1,	false},

	/* Geometry of a real limbo drive with 256MB of strict cache. */
	{7600799744ULL,	67108864000ULL,	36,	SECTOR_ORDER,	19,	true},

	/* The drive before with a non-strict cache. */
	{7600799744ULL,	67108864000ULL,	36,	SECTOR_ORDER,	19,	false},

	/* The devil drive I. */
	{0,		1ULL << 40,	40,	SECTOR_ORDER,	21,	true},

	/* The devil drive II. */
	{0,		1ULL << 40,	40,	SECTOR_ORDER,	21,	false},
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
		int wrap, block_order, max_written_blocks;
		struct device *dev;

		dev = create_file_device(filename, item->real_size_byte,
			item->fake_size_byte, item->wrap, item->block_order,
			item->cache_order, item->strict_cache, false);
		assert(dev);
		max_written_blocks = probe_max_written_blocks(dev);
		assert(!probe_device(dev, &real_size_byte, &announced_size_byte,
			&wrap, &cache_size_block, &block_order, dummy_cb,
			false));
		free_device(dev);
		fake_type = dev_param_to_type(real_size_byte,
			announced_size_byte, wrap, block_order);

		/* Report */
		printf("Test %i\t\ttype/real size/fake size/module/cache size/block size\n",
			i + 1);
		printf("\t\t%s/%.2f %s/%.2f %s/2^%i Byte/%.2f %s/2^%i Byte\n",
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
			block_order == item->block_order) {
			success++;
			printf("\t\tPerfect!\tMax # of written blocks: %i\n\n",
				max_written_blocks);
		} else {
			double ret_f_real = real_size_byte;
			double ret_f_fake = announced_size_byte;
			double ret_f_cache = cache_size_block << block_order;
			const char *ret_unit_real = adjust_unit(&ret_f_real);
			const char *ret_unit_fake = adjust_unit(&ret_f_fake);
			const char *ret_unit_cache = adjust_unit(&ret_f_cache);
			printf("\tError\t%s/%.2f %s/%.2f %s/2^%i Byte/%.2f %s/2^%i Byte\n\n",
				fake_type_to_name(fake_type),
				ret_f_real, ret_unit_real,
				ret_f_fake, ret_unit_fake, wrap,
				ret_f_cache, ret_unit_cache,
				block_order);
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

static inline void report_size(const char *prefix, uint64_t bytes,
	int block_order)
{
	report_probed_size(0, printf_cb, prefix, bytes, block_order);
}

static inline void report_order(const char *prefix, int order)
{
	report_probed_order(0, printf_cb, prefix, order);
}

static inline void report_cache(const char *prefix, uint64_t cache_size_block,
	int block_order)
{
	report_probed_cache(0, printf_cb, prefix, cache_size_block, block_order);
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
	int wrap, block_order;
	uint64_t read_count, read_time_us;
	uint64_t write_count, write_time_us;
	uint64_t reset_count, reset_time_us;

	dev = args->debug
		? create_file_device(args->filename, args->real_size_byte,
			args->fake_size_byte, args->wrap, args->block_order,
			args->cache_order, args->strict_cache, args->keep_file)
		: create_block_device(args->filename, RT_NONE);
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
			probe_max_written_blocks(dev), args->min_mem);
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
		&wrap, &cache_size_block, &block_order,
		args->verbose ? printf_flush_cb : dummy_cb,
		args->show_progress));
	assert(!gettimeofday(&t2, NULL));

	if (args->verbose) {
		/* Isolate the verbose output. */
		printf("\n");
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

	free_device(dev);

	if (args->save)
		printf("\n");

	fake_type = dev_param_to_type(real_size_byte, announced_size_byte,
		wrap, block_order);
	switch (fake_type) {
	case FKTY_GOOD:
		printf("Good news: The device `%s' is the real thing\n",
			args->filename);
		break;

	case FKTY_BAD:
		printf("Bad news: The device `%s' is damaged\n",
			args->filename);
		break;

	case FKTY_LIMBO:
	case FKTY_WRAPAROUND:
	case FKTY_CHAIN: {
		uint64_t last_good_sector = (real_size_byte >> SECTOR_ORDER) - 1;
		assert(block_order >= SECTOR_ORDER);
		printf("Bad news: The device `%s' is a counterfeit of type %s\n\n"
			"You can \"fix\" this device using the following command:\n"
			"f3fix --last-sec=%" PRIu64 " %s\n",
			args->filename, fake_type_to_name(fake_type),
			last_good_sector, args->filename);
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
		block_order);
	 report_order("\t   Physical block size:", block_order);
	report_probe_time("\nProbe time:", diff_timeval_us(&t1, &t2));

	if (args->time_ops) {
		printf(" Operation: total time / count = avg time\n");
		report_ops("Read", read_count, read_time_us);
		report_ops("Write", write_count, write_time_us);
		assert(reset_count == 0);
	}

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
		.time_ops	= false,
		.verbose	= false,
		/* If stdout isn't a terminal, suppress progress. */
		.show_progress	= isatty(STDOUT_FILENO),
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
		.block_order	= SECTOR_ORDER,
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
