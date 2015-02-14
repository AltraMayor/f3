#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>

int ilog2(uint64_t x);
int ceiling_log2(uint64_t x);

static inline int align_head(int order)
{
	return (1 << order) - 1;
}

void *align_mem(void *p, int order);

#endif	/* HEADER_LIBUTILS_H */
