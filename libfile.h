#ifndef HEADER_LIBFILE_H
#define HEADER_LIBFILE_H

#include <stdint.h>	/* For type uint64_t. */

#define GIGABYTES_ORDER	(30)
#define GIGABYTES	(1ULL << GIGABYTES_ORDER)

void adjust_dev_path(const char **dev_path);

int get_block_size(const char *path);

/* Return true if @filename matches the regex /^[0-9]+\.h2w$/ */
int is_my_file(const char *filename);

/* Caller must free(3) the returned pointer. */
char *full_fn_from_number(const char **filename, const char *path,
	uint64_t num);

const uint64_t *ls_my_files(const char *path,
	uint64_t start_at, uint64_t end_at);

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

#ifdef __OpenBSD__

#define POSIX_FADV_SEQUENTIAL	2 /* Expect sequential page references.	*/
#define POSIX_FADV_DONTNEED	4 /* Don't need these pages.		*/

/*
 * OpenBSD doesn't have posix_fadvise() (...).
 * There is some code [in F3] to emulate posix_fadvise for MacOS
 * but it uses various fcntl(2) commands that we don't have [in OpenBSD].
 *
 *  -- Stuart Henderson, OpenBSD developer
 */
#define posix_fadvise(fd, offset, len, advice) (0)

#endif	/* OpenBSD */

#endif	/* HEADER_LIBFILE_H */
