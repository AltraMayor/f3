#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>

#include "utils.h"

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

void full_fn_from_number(char *full_fn, const char **filename,
	const char *path, int num)
{
	static char format[32] = "";
	if (!format[0]) {
		assert(FILENAME_NUM_DIGITS >  0);
		assert(FILENAME_NUM_DIGITS < 10);
		sprintf(format, "%%s/%%%02ii.fff", FILENAME_NUM_DIGITS);
	}
	assert(snprintf(full_fn, PATH_MAX, format, path, num + 1) < PATH_MAX);
	*filename = full_fn + strlen(path) + 1;
}

static int number_from_filename(const char *filename)
{
	char str[FILENAME_NUM_DIGITS + 1];
	assert(is_my_file(filename));
	strncpy(str, filename, FILENAME_NUM_DIGITS);
	str[FILENAME_NUM_DIGITS] = '\0';
	return strtol(str, NULL, 10) - 1;
}

/* Don't call this function directly, use ls_my_files() instead. */
static int *__ls_my_files(DIR *dir, int *pcount, int *pindex)
{
	struct dirent *entry;
	const char *filename;

	entry = readdir(dir);
	if (!entry) {
		int *ret = malloc(sizeof(const int) * (*pcount + 1));
		assert(ret);
		*pindex = *pcount - 1;
		ret[*pcount] = -1;
		closedir(dir);
		return ret;
	}

	filename = entry->d_name;
	if (is_my_file(filename)) {
		int number, my_index, *ret;
		/* Cache @number because @entry may go away. */
		number = number_from_filename(filename);
		(*pcount)++;
		ret = __ls_my_files(dir, pcount, &my_index);
		ret[my_index] = number;
		*pindex = my_index - 1;
		return ret;
	}
	
	return __ls_my_files(dir, pcount, pindex);
}

/* To be used with qsort(3). */
static int cmpintp(const void *p1, const void *p2)
{
	return *(const int *)p1 - *(const int *)p2;
}

const int *ls_my_files(const char *path)
{
	DIR *dir = opendir(path);
	int my_count;
	int my_index;
	int *ret;

	if (!dir)
		err(errno, "Can't open path %s", path);

	my_count = 0;
	ret = __ls_my_files(dir, &my_count, &my_index);
	assert(my_index == -1);
	qsort(ret, my_count, sizeof(*ret), cmpintp);
	return ret;
}

void print_header(FILE *f, char *name)
{
	fprintf(f,
	"F3 %s 2.1\n"
	"Copyright (C) 2010 Digirati Internet LTDA.\n"
	"This is free software; see the source for copying conditions.\n"
	"\n", name);
}

#ifdef APPLE_MAC

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

#endif	/* APPLE_MAC */
