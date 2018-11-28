#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <argp.h>
#include <inttypes.h>
#include <err.h>

#include "version.h"
#include "libutils.h"
#include "libdevs.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 BREW " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<DISK_DEV>";

/* The capital "E" in "REad" in the string below is not a typo.
 * It shows from where the name B-RE-W comes.
 */
static char doc[] = "F3 Block REad and Write -- assess the media of "
	"a block device writing blocks, resetting the drive, and "
	"reading the blocks back";

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
	{"reset-type",		's',	"TYPE",		0,
		"Reset method to use during the probe",		2},
	{"start-at",		'h',	"BLOCK",	0,
		"Where test begins; the default is block zero",	0},
	{"end-at",		'e',	"BLOCK",	0,
		"Where test ends; the default is the very last block",	0},
	{"do-not-write",	'W',	NULL,		0,
		"Do not write blocks",				0},
	{"do-not-read",		'R',	NULL,		0,
		"Do not read blocks",				0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		debug;
	bool		keep_file;

	/* Behavior options. */
	enum reset_type	reset_type;
	bool test_write;
	bool test_read;
	/* 3 free bytes. */

	/* Geometry. */
	uint64_t	real_size_byte;
	uint64_t	fake_size_byte;
	int		wrap;
	int		block_order;
	int		cache_order;
	int		strict_cache;

	/* What to do. */
	uint64_t	first_block;
	uint64_t	last_block;
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

	case 's':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0 || ll >= RT_MAX)
			argp_error(state,
				"Reset type must be in the interval [0, %i]",
				RT_MAX - 1);
		args->reset_type = ll;
		break;

	case 'h':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"The first block must be greater or equal to zero");
		args->first_block = ll;
		break;

	case 'e':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"The last block must be greater or equal to zero");
		args->last_block = ll;
		break;

	case 'W':
		args->test_write = false;
		break;

	case 'R':
		args->test_read = false;
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

		if (args->first_block > args->last_block)
			argp_error(state,
				"The first block parameter must be less or equal to the last block parameter. They are now: first_block=%"
				PRIu64 " > last_block=%" PRIu64,
				args->first_block, args->last_block);

		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

static void write_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + BIG_BLOCK_SIZE_BYTE];
	char *buffer = align_mem(stack, block_order);
	char *stamp_blk = buffer;
	char *flush_blk = buffer + BIG_BLOCK_SIZE_BYTE;
	uint64_t offset = first_block << block_order;
	uint64_t pos, first_pos = first_block;

	assert(BIG_BLOCK_SIZE_BYTE >= block_size);

	for (pos = first_block; pos <= last_block; pos++) {
		fill_buffer_with_block(stamp_blk, block_order, offset, 0);
		stamp_blk += block_size;
		offset += block_size;

		if (stamp_blk == flush_blk || pos == last_block) {
			if (dev_write_blocks(dev, buffer, first_pos, pos))
				warn("Failed to write blocks from 0x%" PRIx64
					" to 0x%" PRIx64, first_pos, pos);
			stamp_blk = buffer;
			first_pos = pos + 1;
		}
	}
}

/* XXX Properly handle return errors. */
static void test_write_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	printf("Writing blocks from 0x%" PRIx64 " to 0x%" PRIx64 "...",
		first_block, last_block);
	fflush(stdout);
	write_blocks(dev, first_block, last_block);
	printf(" Done\n\n");
}

enum block_state {
	bs_unknown,
	bs_good,
	bs_bad,
	bs_overwritten,
};

struct block_range {
	enum block_state	state;
	int			block_order;
	uint64_t		start_sector_offset;
	uint64_t		end_sector_offset;

	/* Only used by state bs_overwritten. */
	uint64_t		found_sector_offset;
};

static const char *block_state_to_str(enum block_state state)
{
	const char *conv_array[] = {
		[bs_unknown] = "Unknown",
		[bs_good] = "Good",
		[bs_bad] = "Bad",
		[bs_overwritten] = "Overwritten",
	};
	return conv_array[state];
}

static int is_block(uint64_t offset, int block_order)
{
	return !(((1ULL << block_order) - 1) & offset);
}

static void print_offset(uint64_t offset, int block_order)
{
	assert(is_block(offset, block_order));
	printf("block 0x%" PRIx64, offset >> block_order);
}

static void print_block_range(const struct block_range *range)
{
	printf("[%s] from ", block_state_to_str(range->state));
	print_offset(range->start_sector_offset, range->block_order);
	printf(" to ");
	print_offset(range->end_sector_offset, range->block_order);

	switch (range->state) {
	case bs_good:
	case bs_bad:
		break;

	case bs_overwritten:
		printf(", found ");
		print_offset(range->found_sector_offset, range->block_order);
		break;

	default:
		assert(0);
		break;
	}
	printf("\n");
}

static void validate_block(uint64_t expected_sector_offset,
	const char *probe_blk, int block_order, struct block_range *range)
{
	uint64_t found_sector_offset;
	enum block_state state;
	bool push_range;

	if (validate_buffer_with_block(probe_blk, block_order,
		&found_sector_offset, 0))
		state = bs_bad; /* Bad block. */
	else if (expected_sector_offset == found_sector_offset)
		state = bs_good; /* Good block. */
	else
		state = bs_overwritten; /* Overwritten block. */

	push_range = (range->state != state) || (
			state == bs_overwritten
			&& (
				(expected_sector_offset
					- range->start_sector_offset)
				!=
				(found_sector_offset
					- range->found_sector_offset)
			)
		);

	if (push_range) {
		if (range->state != bs_unknown)
			print_block_range(range);
		range->state = state;
		range->start_sector_offset = expected_sector_offset;
		range->end_sector_offset = expected_sector_offset;
		range->found_sector_offset = found_sector_offset;
	} else {
		range->end_sector_offset = expected_sector_offset;
	}
}

static void read_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	char stack[align_head(block_order) + BIG_BLOCK_SIZE_BYTE];
	char *buffer = align_mem(stack, block_order);
	uint64_t expected_sector_offset = first_block << block_order;
	uint64_t first_pos = first_block;
	uint64_t step = (BIG_BLOCK_SIZE_BYTE >> block_order) - 1;
	struct block_range range = {
		.state = bs_unknown,
		.block_order = block_order,
		.start_sector_offset = 0,
		.end_sector_offset = 0,
		.found_sector_offset = 0,
	};

	assert(BIG_BLOCK_SIZE_BYTE >= block_size);

	while (first_pos <= last_block) {
		char *probe_blk = buffer;
		uint64_t pos, next_pos = first_pos + step;

		if (next_pos > last_block)
			next_pos = last_block;
		if (dev_read_blocks(dev, buffer, first_pos, next_pos))
			warn("Failed to read blocks from 0x%" PRIx64
				" to 0x%" PRIx64, first_pos, next_pos);

		for (pos = first_pos; pos <= next_pos; pos++) {
			validate_block(expected_sector_offset, probe_blk,
				block_order, &range);
			expected_sector_offset += block_size;
			probe_blk += block_size;
		}

		first_pos = next_pos + 1;
	}
	if (range.state != bs_unknown)
		print_block_range(&range);
	else
		assert(first_block > last_block);
}

/* XXX Properly handle return errors. */
static void test_read_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	printf("Reading blocks from 0x%" PRIx64 " to 0x%" PRIx64 ":\n",
		first_block, last_block);
	read_blocks(dev, first_block, last_block);
	printf("\n");
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.debug		= false,
		.keep_file	= false,
		.reset_type	= RT_MANUAL_USB,
		.test_write	= true,
		.test_read	= true,
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
		.block_order	= 0,
		.cache_order	= -1,
		.strict_cache	= false,
		.first_block	= 0,
		.last_block	= -1ULL,
	};
	struct device *dev;
	uint64_t very_last_block;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "brew");

	dev = args.debug
		? create_file_device(args.filename, args.real_size_byte,
			args.fake_size_byte, args.wrap, args.block_order,
			args.cache_order, args.strict_cache, args.keep_file)
		: create_block_device(args.filename, args.reset_type);
	if (!dev) {
		fprintf(stderr, "\nApplication cannot continue, finishing...\n");
		exit(1);
	}

	printf("Physical block size: 2^%i Bytes\n\n", dev_get_block_order(dev));

	very_last_block =
		(dev_get_size_byte(dev) >> dev_get_block_order(dev)) - 1;
	if (args.first_block > very_last_block)
		args.first_block = very_last_block;
	if (args.last_block > very_last_block)
		args.last_block = very_last_block;

	if (args.test_write)
		test_write_blocks(dev, args.first_block, args.last_block);

	if (args.test_write && args.test_read) {
		const char *final_dev_filename;

		assert(!dev_reset(dev));
		final_dev_filename = dev_get_filename(dev);
		if (strcmp(args.filename, final_dev_filename))
			printf("\nWARNING: device `%s' moved to `%s' due to the reset\n\n",
				args.filename, final_dev_filename);
	}

	if (args.test_read)
		test_read_blocks(dev, args.first_block, args.last_block);

	free_device(dev);
	return 0;
}
