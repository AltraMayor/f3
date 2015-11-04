#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>
#include <argp.h>	/* For struct argp_state.	*/

int ilog2(uint64_t x);
int ceiling_log2(uint64_t x);

const char *adjust_unit(double *ptr_bytes);

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

#endif	/* HEADER_LIBUTILS_H */
