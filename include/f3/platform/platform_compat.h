#ifndef HEADER_PLATFORM_COMPAT_H
#define HEADER_PLATFORM_COMPAT_H

#include <sys/types.h>	/* For off_t	*/

#define UNUSED(x)	((void)x)

/* Define compatibility names to avoid clashes with standard library functions if they exist */
void msleep_compat(double wait_ms);
int fdatasync_compat(int fd);
int posix_fadvise_compat(int fd, off_t offset, off_t len, int advice);

/* Define POSIX_FADV_* compatibility macros if they are not already defined by
 * standard headers included above.
 * These provide the interface values for code calling posix_fadvise_compat.
 * The compat implementations will map these values to platform specifics.
 */
#if !defined(POSIX_FADV_SEQUENTIAL)
#define POSIX_FADV_SEQUENTIAL	2 /* Expect sequential page references.	*/
#endif
#if !defined(POSIX_FADV_DONTNEED)
#define POSIX_FADV_DONTNEED	4 /* Don't need these pages.		*/
#endif

#endif	/* HEADER_PLATFORM_COMPAT_H */
