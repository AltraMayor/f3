#include <stdio.h>	/* For fprintf().	*/
#include <stdlib.h>	/* For strtoll().	*/
#include <stdbool.h>
#include <assert.h>

#include "libutils.h"
#include "version.h"

/* Count the number of 1 bits. */
static int pop(uint64_t x)
{
	int n = 0;
	while (x) {
		n++;
		x = x & (x - 1);
	}
	return n;
}

int ilog2(uint64_t x)
{
	x = x | (x >>  1);
	x = x | (x >>  2);
	x = x | (x >>  4);
	x = x | (x >>  8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	return pop(x) - 1;
}

/* Least power of 2 greater than or equal to x. */
static uint64_t clp2(uint64_t x)
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

int ceiling_log2(uint64_t x)
{
	return ilog2(clp2(x));
}

const char *adjust_unit(double *ptr_bytes)
{
	const char *units[] = { "Byte", "KB", "MB", "GB", "TB", "PB", "EB" };
	int i = 0;
	double final = *ptr_bytes;

	while (i < 7 && final >= 1024) {
		final /= 1024;
		i++;
	}
	*ptr_bytes = final;
	return units[i];
}

void *align_mem(void *p, int order)
{
	uintptr_t ip = (uintptr_t)p;
	uintptr_t head = align_head(order);
	return (void *)(   (ip + head) & ~head   );
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

static inline uint64_t next_random_number(uint64_t random_number)
{
	return random_number * 4294967311ULL + 17;
}

void fill_buffer_with_block(void *buf, int block_order, uint64_t offset,
	uint64_t salt)
{
	uint64_t *int64_array = buf;
	int i, num_int64 = 1 << (block_order - 3);
	uint64_t random_number = offset ^ salt;

	assert(block_order >= 9);

	/* The offset is known by drives,
	 * so one doesn't have to encrypt it.
	 * Plese don't add @salt here!
	 */
	int64_array[0] = offset;

	/* Thanks to @salt, a drive has to guess the seeed. */
	for (i = 1; i < num_int64; i++)
		int64_array[i] = random_number =
			next_random_number(random_number);
}

int validate_buffer_with_block(const void *buf, int block_order,
	uint64_t *pfound_offset, uint64_t salt)
{
	const uint64_t *int64_array = buf;
	int i, num_int64 = 1 << (block_order - 3);
	uint64_t found_offset = int64_array[0];
	uint64_t random_number = found_offset ^ salt;

	assert(block_order >= 9);

	for (i = 1; i < num_int64; i++) {
		random_number = next_random_number(random_number);
		if (int64_array[i] != random_number)
			return true;
	}

	*pfound_offset = found_offset;
	return false;
}
