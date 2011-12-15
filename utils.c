#include "utils.h"

const char *adjust_unit(double *ptr_bytes)
{
	char *units[] = { "Byte", "KB", "MB", "GB", "TB" };
	int i = 0;
	double final = *ptr_bytes;

	while (i < 5 && final >= 1024) {
		final /= 1024;
		i++;
	}
	*ptr_bytes = final;
	return units[i];
}

#ifndef __GLIBC__

#include <stdio.h>
#include <stdint.h>

int srand48_r(long int seedval, struct drand48_data *buffer)
{
	/* The standards say we only have 32 bits.  */
	if (sizeof(long int) > 4)
		seedval &= 0xffffffffl;

	buffer->__x[2] = seedval >> 16;
	buffer->__x[1] = seedval & 0xffffl;
	buffer->__x[0] = 0x330e;

	buffer->__a = 0x5deece66dull;
	buffer->__c = 0xb;
	buffer->__init = 1;

	return 0;
}

static int __drand48_iterate(unsigned short int xsubi[3],
	struct drand48_data *buffer)
{
	uint64_t X;
	uint64_t result;

	/* Initialize buffer, if not yet done.  */
	if (__builtin_expect(!buffer->__init, 0)) {
		buffer->__a = 0x5deece66dull;
		buffer->__c = 0xb;
		buffer->__init = 1;
	}

	/* Do the real work.  We choose a data type which contains at least
	   48 bits.  Because we compute the modulus it does not care how
	   many bits really are computed.  */

	X = (uint64_t) xsubi[2] << 32 | (uint32_t) xsubi[1] << 16 | xsubi[0];

	result = X * buffer->__a + buffer->__c;

	xsubi[0] = result & 0xffff;
	xsubi[1] = (result >> 16) & 0xffff;
	xsubi[2] = (result >> 32) & 0xffff;

	return 0;
}

static int __nrand48_r(unsigned short int xsubi[3],
		       struct drand48_data *buffer, long int *result)
{
	/* Compute next state.  */
	if (__drand48_iterate(xsubi, buffer) < 0)
		return -1;

	/* Store the result.  */
	if (sizeof(unsigned short int) == 2)
		*result = xsubi[2] << 15 | xsubi[1] >> 1;
	else
		*result = xsubi[2] >> 1;

	return 0;
}

int lrand48_r(struct drand48_data *buffer, long int *result)
{
	/* Be generous for the arguments, detect some errors.  */
	if (buffer == NULL)
		return -1;

	return __nrand48_r(buffer->__x, buffer, result);
}

#endif	/* __GLIBC__ */
