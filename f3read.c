#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>

#include "utils.h"

static uint64_t offset_from_filename(const char *filename)
{
	char str[5];
	uint64_t number;

	/* Obtain number. */
	assert(is_my_file(filename));
	strncpy(str, filename, 4);
	str[4] = '\0';
	number = (uint64_t) strtol(str, NULL, 10) - 1;

	return number * GIGABYTES;
}

#define TOLERANCE	2

#define PRINT_STATUS(s)	printf("%s%7" PRIu64 "/%9" PRIu64 "/%7" PRIu64 "/%7" \
	PRIu64 "%s", (s), *ptr_ok, *ptr_corrupted, *ptr_changed, \
	*ptr_overwritten, tail_msg)

#define BLANK	"                                 "
#define CLEAR	("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b" \
		 "\b\b\b\b\b\b\b\b\b\b\b\b\b")

static void validate_file(const char *path, const char *filename,
	uint64_t *ptr_ok, uint64_t *ptr_corrupted, uint64_t *ptr_changed,
	uint64_t *ptr_overwritten, uint64_t *ptr_size, int *read_all,
	struct timeval *ptr_dt, int progress)
{
	uint8_t sector[SECTOR_SIZE], *p, *ptr_end;
	FILE *f;
	int fd;
	int offset_match, error_count;
	size_t sectors_read;
	uint64_t offset, expected_offset;
	struct drand48_data state;
	long int rand_int;
	char full_fn[PATH_MAX];
	char *tail_msg = "";
	struct timeval t1, t2;
	/* Progress time. */
	struct timeval pt1 = { .tv_sec = -1000, .tv_usec = 0 };

	*ptr_ok = *ptr_corrupted = *ptr_changed = *ptr_overwritten =
		*ptr_size = 0;

	printf("Validating file %s ... %s", filename, progress ? BLANK : "");
	fflush(stdout);
	get_full_fn(full_fn, sizeof(full_fn), path, filename);
	f = fopen(full_fn, "rb");
	if (!f)
		err(errno, "Can't open file %s", full_fn);
	fd = fileno(f);
	assert(fd >= 0);

	/* If the kernel follows our advice, f3read won't ever read from cache
	 * even when testing small memory cards without a remount, and
	 * we should have better reading speed measurement.
	 */
	assert(!fdatasync(fd));
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	/* Obtain initial time. */
	assert(!gettimeofday(&t1, NULL));
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));

	ptr_end = sector + SECTOR_SIZE;
	sectors_read = fread(sector, SECTOR_SIZE, 1, f);
	expected_offset = offset_from_filename(filename);
	while (sectors_read > 0) {
		assert(sectors_read == 1);
		offset = *((uint64_t *) sector);
		offset_match = offset == expected_offset;

		srand48_r(offset, &state);
		p = sector + sizeof(offset);
		error_count = 0;
		for (; error_count <= TOLERANCE && p < ptr_end;
			p += sizeof(long int)) {
			lrand48_r(&state, &rand_int);
			if (rand_int != *((long int *) p))
				error_count++;
		}

		sectors_read = fread(sector, SECTOR_SIZE, 1, f);
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

	*read_all = feof(f);
	assert(*read_all || errno == EIO);
	*ptr_size += ftell(f);
	fclose(f);

	tail_msg = read_all ? "" : " - NOT fully read";
	PRINT_STATUS(progress ? CLEAR : "");
	printf("\n");
}

static void report(const char *prefix, uint64_t i)
{
	double f = (double) (i * SECTOR_SIZE);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " sectors)\n", prefix, f, unit, i);
}

static int iterate_path(const char *path, int progress)
{
	DIR *ptr_dir;
	struct dirent *entry;
	const char *filename, *unit;
	uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
	struct timeval tot_dt = { .tv_sec = 0, .tv_usec = 0 };
	double read_speed;
	int and_read_all;

	ptr_dir = opendir(path);
	if (!ptr_dir)
		err(errno, "Can't open path %s", path);

	entry = readdir(ptr_dir);
	tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
	and_read_all = 1;
	printf("                     SECTORS "
		"     ok/corrupted/changed/overwritten\n");
	while (entry) {
		filename = entry->d_name;
		if (is_my_file(filename)) {
			uint64_t sec_ok, sec_corrupted, sec_changed,
				sec_overwritten, file_size;
			int read_all;
			validate_file(path, filename, &sec_ok, &sec_corrupted,
				&sec_changed, &sec_overwritten,
				&file_size, &read_all, &tot_dt, progress);
			tot_ok += sec_ok;
			tot_corrupted += sec_corrupted;
			tot_changed += sec_changed;
			tot_overwritten += sec_overwritten;
			tot_size += file_size;
			and_read_all = and_read_all && read_all;
		}
		entry = readdir(ptr_dir);
	}
	closedir(ptr_dir);
	assert(tot_size / SECTOR_SIZE ==
		(tot_ok + tot_corrupted + tot_changed + tot_overwritten));

	report("\n  Data OK:", tot_ok);
	report("Data LOST:", tot_corrupted + tot_changed + tot_overwritten);
	report("\t       Corrupted:", tot_corrupted);
	report("\tSlightly changed:", tot_changed);
	report("\t     Overwritten:", tot_overwritten);
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	read_speed = (double)tot_size / dt_to_s(&tot_dt);
	unit = adjust_unit(&read_speed);
	printf("Average reading speed: %.2f %s/s\n", read_speed, unit);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 2) {
		/* If stdout isn't a terminal, supress progress. */
		return iterate_path(argv[1], isatty(STDOUT_FILENO));
	}

	fprintf(stderr, "Usage: f3read <PATH>\n");
	return 1;
}
