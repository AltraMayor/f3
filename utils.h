#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <sys/time.h>
#include <limits.h>

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

const char *adjust_unit(double *ptr_bytes);

/* Return true if @filename matches the regex /^[0-9]+\.fff$/ */
int is_my_file(const char *filename);

/* @filename should be PATH_MAX long. */
void full_fn_from_number(char *full_fn, const char **filename,
	const char *path, int num);

static inline long delay_ms(const struct timeval *t1, const struct timeval *t2)
{
	return	(t2->tv_sec  - t1->tv_sec)  * 1000 +
		(t2->tv_usec - t1->tv_usec) / 1000;
}

const int *ls_my_files(const char *path);

void print_header(FILE *f, char *name);

#ifdef APPLE_MAC

/* For function fcntl. */
#include <fcntl.h>
/* For type off_t. */
#include <unistd.h>

/* This function is a _rough_ approximation of fdatasync(2). */
static inline int fdatasync(int fd)
{
	return fcntl(fd, F_FULLFSYNC);
}

#define POSIX_FADV_SEQUENTIAL	2 /* Expect sequential page references.	*/
#define POSIX_FADV_DONTNEED	4 /* Don't need these pages.		*/

/* This function is a _rough_ approximation of posix_fadvise(2). */
static inline int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	switch (advice) {
	case POSIX_FADV_SEQUENTIAL:
		return fcntl(fd, F_RDAHEAD, 1);
	case POSIX_FADV_DONTNEED:
		return fcntl(fd, F_NOCACHE, 1);
	default:
		assert(0);
	}
}

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
