#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/time.h>

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

const char *adjust_unit(double *ptr_bytes);

static inline int is_my_file(const char *filename)
{
	return	(strlen(filename) == 8)	&& isdigit(filename[0])	&&
		isdigit(filename[1])	&& isdigit(filename[2])	&&
		isdigit(filename[3])	&& (filename[4] == '.')	&&
		(filename[5] == 'f')	&& (filename[6] == 'f')	&&
		(filename[7] == 'f');
}

static inline void get_full_fn(char *full_fn, int len,
	const char *path, const char *filename)
{
	assert(snprintf(full_fn, len, "%s/%s", path, filename) < len);
}

static inline long delay_ms(const struct timeval *t1, const struct timeval *t2)
{
	return	(t2->tv_sec  - t1->tv_sec)  * 1000 +
		(t2->tv_usec - t1->tv_usec) / 1000;
}

#ifdef APPLE_MAC

/* For PATH_MAX. */
#include <limits.h>

#include <fcntl.h>
static inline int fdatasync(int fd)
{
	/* It isn't exactly the same thing, but it's the best available on
	 * Macs, and it's enough to work.
	 */
	return fcntl(fd, F_FULLFSYNC);
}

/* Mac's kernel doesn't take advices from applications. */
#define posix_fadvise(fd, offset, len, advice)	0

/*
 * The following functions were copied from GNU Library C to make F3
 * more portable.
 */

/* Data structure for communication with thread safe versions.  This
 * type is to be regarded as opaque.  It's only exported because users
 * have to allocate objects of this type.
 */
struct drand48_data {
	unsigned short int __x[3];	/* Current state.		    */
	unsigned short int __old_x[3];	/* Old state.			    */
	unsigned short int __c;	/* Additive const. in congruential formula. */
	unsigned short int __init;	/* Flag for initializing.	    */
	unsigned long long int __a;	/* Factor in congruential formula.  */
};

/* Seed random number generator.  */
extern int srand48_r(long int __seedval, struct drand48_data *__buffer)
	__attribute__ ((nonnull(2)));

/* Return non-negative, long integer in [0,2^31).  */
extern int lrand48_r(struct drand48_data *__restrict __buffer,
	long int *__restrict __result) __attribute__ ((nonnull(1, 2)));

#endif	/* APPLE_MAC */

#endif	/* HEADER_UTILS_H */
