#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <stdio.h>	/* For type FILE.		*/
#include <sys/time.h>	/* For struct timeval.		*/
#include <stdint.h>	/* For type uint64_t.		*/
#include <argp.h>	/* For struct argp_state.	*/
#include <f3/platform/platform_compat.h>	/* POSIX_FADV_*		*/

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

void adjust_dev_path(const char **dev_path);

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

/*
 * These functions are provided to abstract the platform-specific
 * implementations of fdatasync(2) and posix_fadvise(2).
 */
int f3_fdatasync(int fd);
int f3_posix_fadvise(int fd, off_t offset, off_t len, int advice);

#endif	/* HEADER_UTILS_H */
