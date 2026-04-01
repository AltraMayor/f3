#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdio.h>	/* For fprintf().	*/
#include <stdlib.h>	/* For strtoll().	*/
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>

#include "libutils.h"
#include "version.h"

int ilog2(uint64_t x)
{
	x = x | (x >>  1);
	x = x | (x >>  2);
	x = x | (x >>  4);
	x = x | (x >>  8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	return __builtin_popcountll(x) - 1;
}

uint64_t clp2(uint64_t x)
{
	x = x - 1;
	x = x | (x >>  1);
	x = x | (x >>  2);
	x = x | (x >>  4);
	x = x | (x >>  8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	return x + 1;
}

const char *adjust_unit(double *ptr_bytes)
{
	const char *units[] = {"Bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
	unsigned int i = 0;
	double final = *ptr_bytes;

	while (i < DIM(units) && final >= 1024) {
		final /= 1024;
		i++;
	}
	*ptr_bytes = final;
	if (i > 0 || final != 1.0)
		return units[i];
	return "Byte";
}

int nsec_to_str(uint64_t nsec, char *str)
{
	const uint64_t nsec_in_a_usec	= 1000;
	const uint64_t nsec_in_a_msec	= 1000	* nsec_in_a_usec;
	const uint64_t nsec_in_a_sec	= 1000	* nsec_in_a_msec;
	const uint64_t nsec_in_a_min	= 60	* nsec_in_a_sec;
	const uint64_t nsec_in_an_hour	= 60	* nsec_in_a_min;
	const uint64_t nsec_in_a_day	= 24	* nsec_in_an_hour;
	const uint64_t nsec_in_a_week	= 7	* nsec_in_a_day;

	bool has_w, has_d, has_h, has_m, has_s, has_prv = false;
	lldiv_t div;
	int c, tot = 0;

	has_w = nsec >= nsec_in_a_week;
	if (has_w) {
		div = lldiv(nsec, nsec_in_a_week);
		nsec = div.rem;
		c = sprintf(str + tot, "%i week%s",
			(int)div.quot, div.quot != 1 ? "s" : "");
		assert(c > 0);
		tot += c;
		has_prv = true;
	}

	has_d = nsec >= nsec_in_a_day;
	if (has_d) {
		div = lldiv(nsec, nsec_in_a_day);
		nsec = div.rem;
		c = sprintf(str + tot, "%s%i day%s",
			has_prv ? " " : "", (int)div.quot,
			div.quot != 1 ? "s" : "");
		assert(c > 0);
		tot += c;
		has_prv = true;
	}

	has_h = nsec >= nsec_in_an_hour;
	if (has_h) {
		div = lldiv(nsec, nsec_in_an_hour);
		nsec = div.rem;
		c = sprintf(str + tot, "%s%i:",
			has_prv ? " " : "", (int)div.quot);
		assert(c > 0);
		tot += c;
		has_prv = true;
	}

	has_m = has_h || nsec >= nsec_in_a_min;
	if (has_m) {
		div = lldiv(nsec, nsec_in_a_min);
		nsec = div.rem;
		if (has_h)
			c = sprintf(str + tot, "%02i", (int)div.quot);
		else
			c = sprintf(str + tot, "%s%i'",
				has_prv ? " " : "", (int)div.quot);
		assert(c > 0);
		tot += c;
		has_prv = true;
	}

	has_s = nsec >= nsec_in_a_sec;
	if (has_s) {
		div = lldiv(nsec, nsec_in_a_sec);
		nsec = div.rem;
		if (has_h)
			c = sprintf(str + tot, ":%02i", (int)div.quot);
		else if (has_m)
			c = sprintf(str + tot, "%02i\"", (int)div.quot);
		else if (has_prv)
			c = sprintf(str + tot, " %is", (int)div.quot);
		else
			c = sprintf(str + tot, "%i.%02is", (int)div.quot,
				(int)(nsec / (10 * nsec_in_a_msec)));
		assert(c > 0);
		tot += c;
		has_prv = true;
	}

	if (has_prv)
		return tot;

	if (nsec >= nsec_in_a_msec) {
		div = lldiv(nsec, nsec_in_a_msec);
		nsec = div.rem;
		c = sprintf(str + tot, "%i.%ims", (int)div.quot,
			(int)(nsec / (100 * nsec_in_a_usec)));
	} else if (nsec >= nsec_in_a_usec) {
		div = lldiv(nsec, nsec_in_a_usec);
		nsec = div.rem;
		c = sprintf(str + tot, "%i.%ius", (int)div.quot,
			(int)(nsec / 100));
	} else {
		c = sprintf(str + tot, "%ins", (int)nsec);
	}
	assert(c > 0);
	tot += c;

	return tot;
}

void *align_mem2(void *p, int order, int *shift)
{
	uintptr_t ip0 = (uintptr_t)p;
	uintptr_t head = align_head(order);
	uintptr_t ip1 = (ip0 + head) & ~head;
	*shift = ip1 - ip0;
	return (void *)ip1;
}

void print_header(FILE *f, const char *name)
{
	fprintf(f,
	"F3 %s " F3_STR_VERSION "\n"
	"Copyright (C) 2010 Digirati Internet LTDA.\n"
	"This is free software; see the source for copying conditions.\n"
	"\n", name);
}

long long arg_to_ll_bytes(const struct argp_state *state,
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
		ll <<= SECTOR_ORDER;
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

static inline uint64_t next_random_number(uint64_t random_number)
{
	return random_number * 4294967311ULL + 17;
}

void fill_buffer_with_block(void *buf, int block_order, uint64_t offset,
	uint64_t salt)
{
	const unsigned int num_int64 = 1 << (block_order - 3);
	uint64_t *int64_array = buf;
	uint64_t random_number = offset ^ salt;
	unsigned int i;

	assert(block_order >= SECTOR_ORDER);

	/* DO NOT add salt here!
	 * Drives know the offset, so applying salt to it leaks the salt.
	 */
	int64_array[0] = offset;

	/* Thanks to salt, a drive has to guess the seed. */
	for (i = 1; i < num_int64; i++) {
		int64_array[i] = random_number =
			next_random_number(random_number);
	}
}

const char *block_state_to_str(enum block_state state)
{
	const char *conv_array[] = {
		[bs_unknown] = "Unknown",
		[bs_good] = "Good",
		[bs_bad] = "Bad",
		[bs_changed] = "Changed",
		[bs_overwritten] = "Overwritten",
	};
	return conv_array[state];
}

enum block_state validate_buffer_with_block(const void *buf, int block_order,
	uint64_t expected_offset, uint64_t *pfound_offset, uint64_t salt)
{
	const uint64_t *int64_array = buf;
	const uint64_t found_offset = int64_array[0];
	const int num_int64 = 1 << (block_order - 3);
	uint64_t random_number = found_offset ^ salt;
	const unsigned int bit_error_tolerance = 7;
	unsigned int bit_error_count = 0;
	int i;

	assert(block_order >= SECTOR_ORDER);

	for (i = 1; i < num_int64; i++) {
		random_number = next_random_number(random_number);
		if (int64_array[i] != random_number) {
			bit_error_count += __builtin_popcountll(
				int64_array[i] ^ random_number);
			if (bit_error_count > bit_error_tolerance)
				break;
		}
	}

	*pfound_offset = found_offset;

	if (expected_offset == found_offset) {
		if (bit_error_count == 0)
			return bs_good;
		if (bit_error_count <= bit_error_tolerance)
			return bs_changed;
		return bs_bad;
	}

	if (bit_error_count <= bit_error_tolerance)
		return bs_overwritten;

	return bs_bad;
}

enum block_state validate_block_update_stats(const void *buf, int block_order,
	uint64_t expected_offset, uint64_t *pfound_offset, uint64_t salt,
	struct block_stats *stats)
{
	enum block_state state = validate_buffer_with_block(buf, block_order,
		expected_offset, pfound_offset, salt);

	switch (state) {
	case bs_good:
		stats->ok++;
		break;
	case bs_changed:
		stats->changed++;
		break;
	case bs_bad:
		stats->bad++;
		break;
	case bs_overwritten:
		stats->overwritten++;
		break;
	default:
		assert(0);
	}

	return state;
}

static void print_stat(const char *prefix, uint64_t count,
	int block_size, const char *unit_name)
{
	double f = (double)count * block_size;
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " %s%s)\n",
		prefix, f, unit, count, unit_name, count != 1 ? "s" : "");
}

void print_stats(const struct block_stats *stats, int block_size,
	const char *unit_name)
{
	print_stat("\n  Data OK:", stats->ok, block_size, unit_name);
	print_stat("Data LOST:",
		stats->bad + stats->changed + stats->overwritten,
		block_size, unit_name);
	print_stat("\t       Corrupted:", stats->bad, block_size, unit_name);
	print_stat("\tSlightly changed:", stats->changed, block_size, unit_name);
	print_stat("\t     Overwritten:", stats->overwritten, block_size, unit_name);
}

void report_io_speed(unsigned int indent, progress_cb cb, const char *prefix,
	uint64_t blocks, const char *block_unit, uint64_t time_ns,
	int block_order)
{
	double speed;
	const char *unit;
	char time_str[TIME_STR_SIZE];

	if (time_ns == 0) {
		cb(indent, "%s NO DATA\n", prefix);
		return;
	}

	speed = (blocks << block_order) * 1000000000.0 / time_ns;
	unit = adjust_unit(&speed);
	nsec_to_str(time_ns, time_str);
	cb(indent, "%s %.2f %s/s (%" PRIu64 " %s%s / %s)\n",
		prefix, speed, unit, blocks, block_unit,
		blocks != 1 ? "s" : "", time_str);
}

static void print_indent(unsigned int indent, const char *indent_str)
{
	unsigned int i;
	for (i = 0; i < indent; i++)
		printf("%s", indent_str);
}

static void vprintf_cb(unsigned int indent, const char *format, va_list args)
{
	const char *indent_str = "        ";
	const char  *erase_str = "\b\b\b\b\b\b\b\b";

	assert(format != NULL);
	if (format[0] != '\b') {
		print_indent(indent, indent_str);
		vprintf(format, args);
		return;
	}

	vprintf(format, args);
	print_indent(indent, erase_str);
}

void printf_cb(unsigned int indent, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf_cb(indent, format, args);
	va_end(args);
}

void printf_flush_cb(unsigned int indent, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf_cb(indent, format, args);
	va_end(args);
	fflush(stdout);
}

void dummy_cb(unsigned int indent, const char *format, ...)
{
	/* Do nothing */
	UNUSED(indent);
	UNUSED(format);
}
