#include "libutils.h"

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
