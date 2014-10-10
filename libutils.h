#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>

int ilog2(uint64_t x);
int ceiling_log2(uint64_t x);

static inline void *align_512(void *p)
{
	uintptr_t ip = (uintptr_t)p;
	return (void *)(   (ip + 511) & ~511   );
}

#endif	/* HEADER_LIBUTILS_H */
