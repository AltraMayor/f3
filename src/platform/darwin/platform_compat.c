#include <fcntl.h>	/* For fcntl().	*/
#include <unistd.h>	/* For chdir and chroot	*/
#include <assert.h>

#include <f3/platform/platform_compat.h>

void msleep_compat(double wait_ms)
{
	assert(!usleep(wait_ms * 1000));
}

/* This function is a _rough_ approximation of fdatasync(2). */
int fdatasync_compat(int fd)
{
	return fcntl(fd, F_FULLFSYNC);
}

/* This function is a _rough_ approximation of posix_fadvise(2). */
int posix_fadvise_compat(int fd, off_t offset, off_t len, int advice)
{
	UNUSED(offset);
	UNUSED(len);
	switch (advice) {
	case POSIX_FADV_SEQUENTIAL:
		return fcntl(fd, F_RDAHEAD, 1);
	case POSIX_FADV_DONTNEED:
		return fcntl(fd, F_NOCACHE, 1);
	default:
		assert(0);
	}
}
