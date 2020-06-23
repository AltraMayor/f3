#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <stdio.h>	/* For type FILE.		*/
#include <sys/time.h>	/* For struct timeval.		*/
#include <stdint.h>	/* For type uint64_t.		*/
#include <argp.h>	/* For struct argp_state.	*/

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

const char *adjust_unit(double *ptr_bytes);

/* Return true if @filename matches the regex /^[0-9]+\.h2w$/ */
int is_my_file(const char *filename);

/* Caller must free(3) the returned pointer. */
char *full_fn_from_number(const char **filename, const char *path, long num);

static inline int64_t delay_ms(const struct timeval *t1,
	const struct timeval *t2)
{
	return (int64_t)(t2->tv_sec  - t1->tv_sec)  * 1000 +
			(t2->tv_usec - t1->tv_usec) / 1000;
}

void msleep(double wait_ms);

const long *ls_my_files(const char *path, long start_at, long end_at);

void print_header(FILE *f, const char *name);

static inline uint64_t random_number(uint64_t prv_number)
{
	return prv_number * 4294967311ULL + 17;
}

#define UNUSED(x)	((void)x)

long arg_to_long(const struct argp_state *state, const char *arg);

#if __APPLE__ && __MACH__

#include <unistd.h>	/* For type off_t.	*/

#define POSIX_FADV_SEQUENTIAL	2 /* Expect sequential page references.	*/
#define POSIX_FADV_DONTNEED	4 /* Don't need these pages.		*/

int fdatasync(int fd);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);

#endif	/* Apple Macintosh */

#ifdef __FreeBSD__
#define fdatasync(fd) fsync(fd)
#endif

#endif	/* HEADER_UTILS_H */
