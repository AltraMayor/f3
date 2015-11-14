#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>
#include <argp.h>	/* For struct argp_state.	*/
#include <sys/time.h>	/* For struct timeval.		*/

#define UNUSED(x)	((void)x)

int ilog2(uint64_t x);

/* Least power of 2 greater than or equal to x. */
uint64_t clp2(uint64_t x);

int ceiling_log2(uint64_t x);

const char *adjust_unit(double *ptr_bytes);

/*
 * The functions align_head() and align_mem() are used to align pointers.
 *
 * The following example allocates two block on stack and makes sure that
 * the blocks are aligned with the block size.
 *
 *	// The number 2 below means two blocks.
 *	char stack[align_head(block_order) + (2 << block_order)];
 *	char *stamp_blk, *probe_blk;
 *	stamp_blk = align_mem(stack, block_order);
 *	probe_blk = stamp_blk + block_size;
 */

static inline int align_head(int order)
{
	return (1 << order) - 1;
}

void *align_mem(void *p, int order);

void print_header(FILE *f, const char *name);

long long arg_to_ll_bytes(const struct argp_state *state, const char *arg);

/* Dependent on the byte order of the processor (i.e. endianness). */
void fill_buffer_with_block(void *buf, int block_order, uint64_t offset,
	uint64_t salt);

/* Dependent on the byte order of the processor (i.e. endianness). */
int validate_buffer_with_block(const void *buf, int block_order,
	uint64_t *pfound_offset, uint64_t salt);

static inline uint64_t diff_timeval_us(const struct timeval *t1,
	const struct timeval *t2)
{
	return (t2->tv_sec - t1->tv_sec) * 1000000ULL +
		t2->tv_usec - t1->tv_usec;
}

#endif	/* HEADER_LIBUTILS_H */
