#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>

#include "utils.h"

static inline void update_dt(struct timeval *dt, const struct timeval *t1,
	const struct timeval *t2)
{
	dt->tv_sec  += t2->tv_sec  - t1->tv_sec;
	dt->tv_usec += t2->tv_usec - t1->tv_usec;
	if (dt->tv_usec >= 1000000) {
		dt->tv_sec++;
		dt->tv_usec -= 1000000;
	}
}

#define TOLERANCE	2

#define PRINT_STATUS(s)	printf("%s%7" PRIu64 "/%9" PRIu64 "/%7" PRIu64 "/%7" \
	PRIu64, (s), *ptr_ok, *ptr_corrupted, *ptr_changed, *ptr_overwritten)

#define BLANK	"                                 "
#define CLEAR	("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b" \
		 "\b\b\b\b\b\b\b\b\b\b\b\b\b")

static void validate_file(const char *path, int number,
	uint64_t *ptr_ok, uint64_t *ptr_corrupted, uint64_t *ptr_changed,
	uint64_t *ptr_overwritten, uint64_t *ptr_size, int *ptr_read_all,
	struct timeval *ptr_dt, int progress)
{
	char full_fn[PATH_MAX];
	const char *filename;
	uint8_t sector[SECTOR_SIZE], *p, *ptr_end;
	FILE *f;
	int fd;
	int offset_match, error_count;
	size_t sectors_read;
	uint64_t offset, expected_offset;
	int final_errno;
	struct timeval t1, t2;
	/* Progress time. */
	struct timeval pt1 = { .tv_sec = -1000, .tv_usec = 0 };

	*ptr_ok = *ptr_corrupted = *ptr_changed = *ptr_overwritten = 0;

	full_fn_from_number(full_fn, &filename, path, number);
	printf("Validating file %s ... %s", filename, progress ? BLANK : "");
	fflush(stdout);
#ifdef CYGWIN
	/* We don't need write access, but some kernels require that
	 * the file descriptor passed to fdatasync(2) to be writable.
	 */
	f = fopen(full_fn, "rb+");
#else
	f = fopen(full_fn, "rb");
#endif
	if (!f)
		err(errno, "Can't open file %s", full_fn);
	fd = fileno(f);
	assert(fd >= 0);

	/* If the kernel follows our advice, f3read won't ever read from cache
	 * even when testing small memory cards without a remount, and
	 * we should have a better reading-speed measurement.
	 */
	assert(!fdatasync(fd));
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	/* Obtain initial time. */
	assert(!gettimeofday(&t1, NULL));
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));

	ptr_end = sector + SECTOR_SIZE;
	sectors_read = fread(sector, SECTOR_SIZE, 1, f);
	final_errno = errno;
	expected_offset = (uint64_t)number * GIGABYTES;
	while (sectors_read > 0) {
		uint64_t rn;

		assert(sectors_read == 1);
		offset = *((uint64_t *) sector);
		offset_match = offset == expected_offset;

		rn = offset;
		p = sector + sizeof(offset);
		error_count = 0;
		for (; error_count <= TOLERANCE && p < ptr_end;
			p += sizeof(rn)) {
			rn = random_number(rn);
			if (rn != *((typeof(rn) *) p))
				error_count++;
		}

		sectors_read = fread(sector, SECTOR_SIZE, 1, f);
		final_errno = errno;
		expected_offset += SECTOR_SIZE;

		if (offset_match) {
			if (error_count == 0)
				(*ptr_ok)++;
			else if (error_count <= TOLERANCE)
				(*ptr_changed)++;
			else
				(*ptr_corrupted)++;
		} else if (error_count <= TOLERANCE)
			(*ptr_overwritten)++;
		else
			(*ptr_corrupted)++;

		if (progress) {
			struct timeval pt2;
			assert(!gettimeofday(&pt2, NULL));
			/* Avoid often printouts. */
			if (delay_ms(&pt1, &pt2) >= 200) {
				PRINT_STATUS(CLEAR);
				fflush(stdout);
				pt1 = pt2;
			}
		}
	}
	assert(!gettimeofday(&t2, NULL));
	update_dt(ptr_dt, &t1, &t2);

	*ptr_read_all = feof(f);
	*ptr_size = ftell(f);
	assert(*ptr_size >= 0);

	PRINT_STATUS(progress ? CLEAR : "");
	if (!*ptr_read_all) {
		assert(ferror(f));
		printf(" - NOT fully read due to \"%s\"",
			strerror(final_errno));
	}
	printf("\n");

	fclose(f);
}

static void report(const char *prefix, uint64_t i)
{
	double f = (double) (i * SECTOR_SIZE);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " sectors)\n", prefix, f, unit, i);
}

static inline double dt_to_s(struct timeval *dt)
{
	double ret = (double)dt->tv_sec + ((double)dt->tv_usec / 1000000.);
	assert(ret >= 0);
	return ret > 0 ? ret : 1;
}

static void iterate_files(const char *path, const int *files, int start_at,
	int progress)
{
	uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
	struct timeval tot_dt = { .tv_sec = 0, .tv_usec = 0 };
	double read_speed;
	const char *unit;
	int and_read_all = 1;
	int or_missing_file = 0;
	int number = start_at;

	tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
	printf("                  SECTORS "
		"     ok/corrupted/changed/overwritten\n");

	while (*files >= 0) {
		uint64_t sec_ok, sec_corrupted, sec_changed,
			sec_overwritten, file_size;
		int read_all;

		or_missing_file = or_missing_file || (*files != number);
		for (; number < *files; number++) {
			char full_fn[PATH_MAX];
			const char *filename;
			full_fn_from_number(full_fn, &filename, "", number);
			printf("Missing file %s\n", filename);
		}
		number++;

		validate_file(path, *files, &sec_ok, &sec_corrupted,
			&sec_changed, &sec_overwritten,
			&file_size, &read_all, &tot_dt, progress);
		tot_ok += sec_ok;
		tot_corrupted += sec_corrupted;
		tot_changed += sec_changed;
		tot_overwritten += sec_overwritten;
		tot_size += file_size;
		and_read_all = and_read_all && read_all;
		files++;
	}
	assert(tot_size / SECTOR_SIZE ==
		(tot_ok + tot_corrupted + tot_changed + tot_overwritten));

	report("\n  Data OK:", tot_ok);
	report("Data LOST:", tot_corrupted + tot_changed + tot_overwritten);
	report("\t       Corrupted:", tot_corrupted);
	report("\tSlightly changed:", tot_changed);
	report("\t     Overwritten:", tot_overwritten);
	if (or_missing_file)
		printf("WARNING: Not all F3 files are available\n");
	if (start_at > 0)
		printf("WARNING: Not all F3 files were tested due to parameter %s%i\n",
		START_AT_TEXT, start_at + 1);
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	read_speed = (double)tot_size / dt_to_s(&tot_dt);
	unit = adjust_unit(&read_speed);
	printf("Average reading speed: %.2f %s/s\n", read_speed, unit);
}

int main(int argc, char *argv[])
{
	int start_at;
	const char *path;
	const int *files;
	int progress;

	switch (argc) {
	case 2:
		start_at = 0;
		path = argv[1];
		break;

	case 3:
		start_at = parse_start_at_param(argv[1]);
		if (start_at < 0)
			goto error;
		path = argv[2];
		break;

	default:
		goto error;
	}

	files = ls_my_files(path, start_at);
	/* If stdout isn't a terminal, supress progress. */
	progress = isatty(STDOUT_FILENO);
	iterate_files(path, files, start_at, progress);
	free((void *)files);
	return 0;

error:
	print_header(stderr, "read");
	fprintf(stderr, "Usage: f3read [%sNUM] <PATH>\n", START_AT_TEXT);
	return 1;
}
