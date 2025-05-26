#include <unistd.h>	/* fdatasync, posix_fadvise 	*/
#include <math.h>	/* For fmod	*/
#include <time.h>	/* For clock_gettime() and clock_nanosleep().	*/
#include <assert.h>	/* For assert()	*/
#include <errno.h>	/* For EINTR	*/

#include <f3/platform/platform_compat.h>

void msleep_compat(double wait_ms)
{
	struct timespec req;
	int ret;

	assert(!clock_gettime(CLOCK_MONOTONIC, &req));

	/* Add @wait_ms to @req. */
	if (wait_ms > 1000) {
		time_t sec = wait_ms / 1000;
		wait_ms -= sec * 1000;
		assert(wait_ms > 0);
		req.tv_sec += sec;
	}
	req.tv_nsec += wait_ms * 1000000;

	/* Round @req up. */
	if (req.tv_nsec >= 1000000000) {
		ldiv_t result = ldiv(req.tv_nsec, 1000000000);
		req.tv_sec += result.quot;
		req.tv_nsec = result.rem;
	}

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&req, NULL);
	} while (ret == EINTR);

	assert(ret == 0);
}

/* POSIX function wrappers */

int fdatasync_compat(int fd)
{
	return fsync(fd);
}

int posix_fadvise_compat(int fd, off_t offset, off_t len, int advice)
{
	return posix_fadvise(fd, offset, len, advice);
}
