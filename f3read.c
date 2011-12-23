#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <dirent.h>
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
	PRIu64 "%s", (s), *ptr_ok, *ptr_corrupted, *ptr_changed, \
	*ptr_overwritten, tail_msg)

#define BLANK	"                                 "
#define CLEAR	("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b" \
		 "\b\b\b\b\b\b\b\b\b\b\b\b\b")

static void validate_file(const char *path, int number,
	uint64_t *ptr_ok, uint64_t *ptr_corrupted, uint64_t *ptr_changed,
	uint64_t *ptr_overwritten, uint64_t *ptr_size, int *read_all,
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
	struct drand48_data state;
	long int rand_int;
	char *tail_msg = "";
	struct timeval t1, t2;
	/* Progress time. */
	struct timeval pt1 = { .tv_sec = -1000, .tv_usec = 0 };

	*ptr_ok = *ptr_corrupted = *ptr_changed = *ptr_overwritten = 0;

	full_fn_from_number(full_fn, &filename, path, number);
	printf("Validating file %s ... %s", filename, progress ? BLANK : "");
	fflush(stdout);
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
	expected_offset = (uint64_t)number * GIGABYTES;
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
	*ptr_size = ftell(f);
	assert(*ptr_size >= 0);
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

static inline double dt_to_s(struct timeval *dt)
{
	double ret = (double)dt->tv_sec + ((double)dt->tv_usec / 1000000.);
	assert(ret >= 0);
	return ret > 0 ? ret : 1;
}

static void iterate_files(const char *path, const int *files, int progress)
{
	uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
	struct timeval tot_dt = { .tv_sec = 0, .tv_usec = 0 };
	double read_speed;
	const char *unit;
	int and_read_all = 1;
	int or_missing_file = 0;
	int number = 0;

	tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
	printf("                     SECTORS "
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
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	read_speed = (double)tot_size / dt_to_s(&tot_dt);
	unit = adjust_unit(&read_speed);
	printf("Average reading speed: %.2f %s/s\n", read_speed, unit);
}

static int number_from_filename(const char *filename)
{
	char str[FILENAME_NUM_DIGITS + 1];
	assert(is_my_file(filename));
	strncpy(str, filename, FILENAME_NUM_DIGITS);
	str[FILENAME_NUM_DIGITS] = '\0';
	return strtol(str, NULL, 10) - 1;
}

/* Don't call this function directly, use ls_my_files instead. */
static int *__ls_my_files(DIR *dir, int *pcount, int *pindex)
{
	struct dirent *entry;
	const char *filename;

	entry = readdir(dir);
	if (!entry) {
		int *ret = malloc(sizeof(const int) * (*pcount + 1));
		*pindex = *pcount - 1;
		ret[*pcount] = -1;
		closedir(dir);
		return ret;
	}

	filename = entry->d_name;
	if (is_my_file(filename)) {
		int my_index;
		int *ret;
		(*pcount)++;
		ret = __ls_my_files(dir, pcount, &my_index);
		ret[my_index] = number_from_filename(filename);
		*pindex = my_index - 1;
		return ret;
	}
	
	return __ls_my_files(dir, pcount, pindex);
}

/* To be used with qsort(3). */
static int cmpintp(const void *p1, const void *p2)
{
	return *(const int *)p1 - *(const int *)p2;
}

static const int *ls_my_files(const char *path)
{
	DIR *dir = opendir(path);
	int my_count;
	int my_index;
	int *ret;

	if (!dir)
		err(errno, "Can't open path %s", path);

	my_count = 0;
	ret = __ls_my_files(dir, &my_count, &my_index);
	assert(my_index == -1);
	qsort(ret, my_count, sizeof(*ret), cmpintp);
	return ret;
}

int main(int argc, char *argv[])
{
	if (argc == 2) {
		const char *path = argv[1];
		const int *files = ls_my_files(path);
		/* If stdout isn't a terminal, supress progress. */
		int progress = isatty(STDOUT_FILENO);
		iterate_files(path, files, progress);
		free((void *)files);
		return 0;
	}

	fprintf(stderr, "Usage: f3read <PATH>\n");
	return 1;
}
