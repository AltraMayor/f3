#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>
#include <argp.h>	/* For struct argp_state.	*/

int ilog2(uint64_t x);
int ceiling_log2(uint64_t x);

static inline int align_head(int order)
{
	return (1 << order) - 1;
}

void *align_mem(void *p, int order);

long long arg_to_ll_bytes(const struct argp_state *state, const char *arg);

#endif	/* HEADER_LIBUTILS_H */
