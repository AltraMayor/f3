#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <argp.h>

#include "utils.h"
#include "libflow.h"
#include "version.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Read " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<PATH>";

static char doc[] = "F3 Read -- validate .h2w files to test "
	"the real capacity of the drive";

static struct argp_option options[] = {
	{"start-at",		's',	"NUM",		0,
		"First NUM.h2w file to be read",			1},
	{"end-at",		'e',	"NUM",		0,
		"Last NUM.h2w file to be read",				0},
	{"max-read-rate",	'r',	"KB/s",		0,
		"Maximum read rate",					0},
	{"show-progress",	'p',	"NUM",		0,
		"Show progress if NUM is not zero",			0},
	{ 0 }
};

struct args {
	long        start_at;
	long        end_at;
	long        max_read_rate;
	int	    show_progress;
	const char  *dev_path;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long l;

	switch (key) {
	case 's':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->start_at = l - 1;
		break;

	case 'e':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->end_at = l - 1;
		break;

	case 'r':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"KB/s must be greater than zero");
		args->max_read_rate = l;
		break;

	case 'p':
		args->show_progress = !!arg_to_long(state, arg);
		break;

	case ARGP_KEY_INIT:
		args->dev_path = NULL;
		break;

	case ARGP_KEY_ARG:
		if (args->dev_path)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->dev_path = arg;
		break;

	case ARGP_KEY_END:
		if (!args->dev_path)
			argp_error(state,
				"The disk path was not specified");
		if (args->start_at > args->end_at)
			argp_error(state,
				"Option --start-at must be less or equal to option --end-at");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

struct file_stats {
	uint64_t secs_ok;
	uint64_t secs_corrupted;
	uint64_t secs_changed;
	uint64_t secs_overwritten;

	uint64_t bytes_read;
	int read_all;
};

static inline void zero_fstats(struct file_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

#define TOLERANCE	2

static void check_sector(char *_sector, uint64_t expected_offset,
	struct file_stats *stats)
{
	uint64_t *sector = (uint64_t *)_sector;
	uint64_t rn;
	const int num_int64 = SECTOR_SIZE >> 3;
	int error_count, i;

	rn = sector[0];
	error_count = 0;
	for (i = 1; error_count <= TOLERANCE && i < num_int64; i++) {
		rn = random_number(rn);
		if (rn != sector[i])
			error_count++;
	}

	if (expected_offset == sector[0]) {
		if (error_count == 0)
			stats->secs_ok++;
		else if (error_count <= TOLERANCE)
			stats->secs_changed++;
		else
			stats->secs_corrupted++;
	} else if (error_count <= TOLERANCE)
		stats->secs_overwritten++;
	else
		stats->secs_corrupted++;
}

static uint64_t check_buffer(char *buf, size_t size, uint64_t expected_offset,
	struct file_stats *stats)
{
	char *beyond_buf = buf + size;

	assert(size % SECTOR_SIZE == 0);

	while (buf < beyond_buf) {
		check_sector(buf, expected_offset, stats);
		buf += SECTOR_SIZE;
		expected_offset += SECTOR_SIZE;
	}
	return expected_offset;
}

static ssize_t read_all(int fd, char *buf, size_t count)
{
	size_t done = 0;
	do {
		ssize_t rc = read(fd, buf + done, count - done);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return - errno;
		}
		if (rc == 0)
			break;
		done += rc;
	} while (done < count);
	return done;
}

static ssize_t check_chunk(struct dynamic_buffer *dbuf, int fd,
	uint64_t *p_expected_offset, uint64_t chunk_size,
	struct file_stats *stats)
{
	char *buf = dbuf_get_buf(dbuf, chunk_size);
	size_t len = dbuf_get_len(dbuf);
	ssize_t tot_bytes_read = 0;

	while (chunk_size > 0) {
		size_t turn_size = chunk_size <= len ? chunk_size : len;
		ssize_t bytes_read = read_all(fd, buf, turn_size);

		if (bytes_read < 0) {
			stats->bytes_read += tot_bytes_read;
			return bytes_read;
		}

		if (bytes_read == 0)
			break;

		tot_bytes_read += bytes_read;
		chunk_size -= bytes_read;
		*p_expected_offset = check_buffer(buf, bytes_read,
			*p_expected_offset, stats);
	}

	stats->bytes_read += tot_bytes_read;
	return tot_bytes_read;
}

static inline void print_status(const struct file_stats *stats)
{
	printf("%7" PRIu64 "/%9" PRIu64 "/%7" PRIu64 "/%7" PRIu64,
		stats->secs_ok, stats->secs_corrupted, stats->secs_changed,
		stats->secs_overwritten);
}

static void validate_file(const char *path, int number, struct flow *fw,
	struct file_stats *stats)
{
	char *full_fn;
	const char *filename;
	int fd, saved_errno;
	ssize_t bytes_read;
	uint64_t expected_offset;
	struct dynamic_buffer dbuf;

	zero_fstats(stats);

	full_fn = full_fn_from_number(&filename, path, number);
	assert(full_fn);
	printf("Validating file %s ... ", filename);
	fflush(stdout);
#ifdef __CYGWIN__
	/* We don't need write access, but some kernels require that
	 * the file descriptor passed to fdatasync(2) to be writable.
	 */
	fd = open(full_fn, O_RDWR);
#else
	fd = open(full_fn, O_RDONLY);
#endif
	if (fd < 0)
		err(errno, "Can't open file %s", full_fn);

	/* If the kernel follows our advice, f3read won't ever read from cache
	 * even when testing small memory cards without a remount, and
	 * we should have a better reading-speed measurement.
	 */
	assert(!fdatasync(fd));
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));

	dbuf_init(&dbuf);
	saved_errno = 0;
	expected_offset = (uint64_t)number * GIGABYTES;
	start_measurement(fw);
	while (true) {
		bytes_read = check_chunk(&dbuf, fd, &expected_offset,
			get_rem_chunk_size(fw), stats);
		if (bytes_read == 0)
			break;
		if (bytes_read < 0) {
			saved_errno = - bytes_read;
			break;
		}
		if (measure(fd, fw, bytes_read) < 0) {
			saved_errno = errno;
			break;
		}
	}
	if (end_measurement(fd, fw) < 0) {
		/* If a write failure has happened before, preserve it. */
		if (!saved_errno)
			saved_errno = errno;
	}

	print_status(stats);
	stats->read_all = bytes_read == 0;
	if (!stats->read_all) {
		assert(saved_errno);
		printf(" - NOT fully read due to \"%s\"",
			strerror(saved_errno));
	} else if (saved_errno) {
		printf(" - %s", strerror(saved_errno));
	}
	printf("\n");

	dbuf_free(&dbuf);
	close(fd);
	free(full_fn);
}

static void report(const char *prefix, uint64_t i)
{
	double f = (double) (i * SECTOR_SIZE);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " sectors)\n", prefix, f, unit, i);
}

static uint64_t get_total_size(const char *path, const long *files)
{
	uint64_t total_size = 0;

	while (*files >= 0) {
		struct stat st;
		int ret;
		const char *filename;
		char *full_fn = full_fn_from_number(&filename, path, *files);
		assert(full_fn);

		ret = stat(full_fn, &st);
		if (ret < 0)
			err(errno, "Can't stat file %s", full_fn);
		if ((st.st_mode & S_IFMT) != S_IFREG)
			err(EINVAL, "File %s is not a regular file", full_fn);
		assert(st.st_size >= 0);
		total_size += st.st_size;

		free(full_fn);
		files++;
	}
	return total_size;
}

static inline void pr_avg_speed(double speed)
{
	const char *unit = adjust_unit(&speed);
	printf("Average reading speed: %.2f %s/s\n", speed, unit);
}

static void iterate_files(const char *path, const long *files,
	long start_at, long end_at, long max_read_rate, int progress)
{
	uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
	int and_read_all = 1;
	int or_missing_file = 0;
	long number = start_at;
	struct flow fw;
	struct timeval t1, t2;

	UNUSED(end_at);

	init_flow(&fw, get_total_size(path, files), max_read_rate,
		progress, NULL);
	tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
	printf("                  SECTORS "
		"     ok/corrupted/changed/overwritten\n");

	assert(!gettimeofday(&t1, NULL));
	while (*files >= 0) {
		struct file_stats stats;

		or_missing_file = or_missing_file || (*files != number);
		for (; number < *files; number++) {
			const char *filename;
			char *full_fn = full_fn_from_number(&filename, "",
				number);
			assert(full_fn);
			printf("Missing file %s\n", filename);
			free(full_fn);
		}
		number++;

		validate_file(path, *files, &fw, &stats);
		tot_ok += stats.secs_ok;
		tot_corrupted += stats.secs_corrupted;
		tot_changed += stats.secs_changed;
		tot_overwritten += stats.secs_overwritten;
		tot_size += stats.bytes_read;
		and_read_all = and_read_all && stats.read_all;
		files++;
	}
	assert(!gettimeofday(&t2, NULL));
	assert(tot_size == SECTOR_SIZE *
		(tot_ok + tot_corrupted + tot_changed + tot_overwritten));

	/* Notice that not reporting `missing' files after the last file
	 * in @files is important since @end_at could be very large.
	 */

	report("\n  Data OK:", tot_ok);
	report("Data LOST:", tot_corrupted + tot_changed + tot_overwritten);
	report("\t       Corrupted:", tot_corrupted);
	report("\tSlightly changed:", tot_changed);
	report("\t     Overwritten:", tot_overwritten);
	if (or_missing_file)
		printf("WARNING: Not all F3 files in the range %li to %li are available\n",
			start_at + 1, number);
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	if (has_enough_measurements(&fw)) {
		pr_avg_speed(get_avg_speed(&fw));
	} else {
		/* If the drive is too fast for the measurements above,
		 * try a coarse approximation of the reading speed.
		 */
		int64_t total_time_ms = delay_ms(&t1, &t2);
		if (total_time_ms > 0) {
			pr_avg_speed(get_avg_speed_given_time(&fw,
				total_time_ms));
		} else {
			printf("Reading speed not available\n");
		}
	}
}

int main(int argc, char **argv)
{
	const long *files;

	struct args args = {
		/* Defaults. */
		.start_at	= 0,
		.end_at		= LONG_MAX - 1,
		.max_read_rate	= 0,
		/* If stdout isn't a terminal, suppress progress. */
		.show_progress	= isatty(STDOUT_FILENO),
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "read");

	files = ls_my_files(args.dev_path, args.start_at, args.end_at);

	iterate_files(args.dev_path, files, args.start_at, args.end_at,
		args.max_read_rate, args.show_progress);
	free((void *)files);
	return 0;
}
