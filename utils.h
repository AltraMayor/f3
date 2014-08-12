#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <sys/time.h>
#include <limits.h>
#include <stdint.h>

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

const char *adjust_unit(double *ptr_bytes);

/* Return true if @filename matches the regex /^[0-9]+\.h2w$/ */
int is_my_file(const char *filename);

/* @filename should be PATH_MAX long. */
void full_fn_from_number(char *full_fn, const char **filename,
	const char *path, int num);

static inline long delay_ms(const struct timeval *t1, const struct timeval *t2)
{
	return	(t2->tv_sec  - t1->tv_sec)  * 1000 +
		(t2->tv_usec - t1->tv_usec) / 1000;
}

/* Parse @param and return the start-at parameter.
 * The string must be of the format "--start-at=NUM"; otherwise it returns -1.
 */
#define START_AT_TEXT "--start-at="
int parse_start_at_param(const char *param);

const int *ls_my_files(const char *path, int start_at);

void print_header(FILE *f, char *name);

static inline uint64_t random_number(uint64_t prv_number)
{
	return prv_number * 4294967311ULL + 17;
}

#if __APPLE__ && __MACH__

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

#endif	/* Apple Macintosh */

#endif	/* HEADER_UTILS_H */
