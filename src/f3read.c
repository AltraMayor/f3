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
#include <sys/types.h>
#include <sys/stat.h>
#include <argp.h>
#include <inttypes.h>

#include "libutils.h"
#include "libfile.h"
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
	uint64_t    start_at;
	uint64_t    end_at;
	uint64_t    max_read_rate;
	int	    show_progress;
	const char  *dev_path;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 's':
		ll = arg_to_ll_bytes(state, arg);
		if (ll <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->start_at = ll - 1;
		break;

	case 'e':
		ll = arg_to_ll_bytes(state, arg);
		if (ll <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->end_at = ll - 1;
		break;

	case 'r':
		ll = arg_to_ll_bytes(state, arg);
		if (ll <= 0)
			argp_error(state,
				"KB/s must be greater than zero");
		args->max_read_rate = ll;
		break;

	case 'p':
		args->show_progress = !!arg_to_ll_bytes(state, arg);
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
	struct block_stats secs;

	uint64_t bytes_read;
	int read_all;
};

static inline void zero_fstats(struct file_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

static inline void check_sector(char *sector, uint64_t expected_offset,
	struct file_stats *stats)
{
	uint64_t found_offset;
	validate_block_update_stats(sector, SECTOR_ORDER, expected_offset,
		&found_offset, 0, &stats->secs);
}

static void check_buffer(char *buf, uint64_t sectors,
	uint64_t *pexpected_offset, struct file_stats *stats)
{
	uint64_t i;
	for (i = 0; i < sectors; i++) {
		check_sector(buf, *pexpected_offset, stats);
		buf += SECTOR_SIZE;
		*pexpected_offset += SECTOR_SIZE;
	}
}

static int read_all(int fd, char *buf, size_t *pbytes)
{
	ssize_t rc;
	size_t done = 0;
	do {
		rc = read(fd, buf + done, *pbytes - done);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			rc = errno;
			goto out;
		}
		if (rc == 0)
			goto out;
		done += rc;
	} while (done < *pbytes);
	rc = 0;

out:
	*pbytes = done;
	return rc;
}

static int check_chunk(struct flow *fw, struct dynamic_buffer *dbuf,
	int fd, uint64_t *pexpected_offset, struct file_stats *stats,
	size_t *ptot_bytes_read)
{
	uint64_t chunk_size = get_rem_chunk_blocks(fw) <<
		fw_get_block_order(fw);
	size_t len = chunk_size;
	char * const buf = dbuf_get_buf(dbuf, 0, &len);
	size_t tot_bytes_read = 0;
	int rc = 0;

	while (chunk_size > 0) {
		size_t bytes_read = MIN(chunk_size, len);
		rc = read_all(fd, buf, &bytes_read);
		tot_bytes_read += bytes_read;

		if (bytes_read == 0)
			break;

		chunk_size -= bytes_read;
		assert((bytes_read & (SECTOR_SIZE - 1)) == 0);
		check_buffer(buf, bytes_read >> SECTOR_ORDER,
			pexpected_offset, stats);

		if (rc != 0)
			break;
	}

	stats->bytes_read += tot_bytes_read;
	*ptot_bytes_read = tot_bytes_read;
	return rc;
}

static inline void print_status(const struct file_stats *stats)
{
	printf("%7" PRIu64 "/%9" PRIu64 "/%7" PRIu64 "/%7" PRIu64,
		stats->secs.ok, stats->secs.bad, stats->secs.changed,
		stats->secs.overwritten);
}

static void validate_file(struct flow *fw, struct dynamic_buffer *dbuf,
	const char *path, uint64_t number, struct file_stats *stats)
{
	const unsigned int block_size = fw_get_block_size(fw);
	const unsigned int block_order = fw_get_block_order(fw);
	char *full_fn;
	const char *filename;
	int fd, saved_errno;
	uint64_t expected_offset;
	struct timespec file_t1, file_t2;

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
	if (fdatasync(fd) < 0) {
		int saved_errno = errno;
		/* The issue https://github.com/AltraMayor/f3/issues/211
		 * motivated the warning below.
		 */
		printf("\nWARNING:\nThe operating system returned errno=%i for fdatasync(): %s\nThis error is unexpected and you may find more information on the log of the kernel (e.g. command dmesg(1) on Linux).\n\n",
			saved_errno, strerror(saved_errno));
		exit(saved_errno);
	}
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));

	saved_errno = 0;
	expected_offset = number << GIGABYTE_ORDER;
	assert(!clock_gettime(CLOCK_MONOTONIC, &file_t1));
	start_measurement(fw);
	while (true) {
		size_t bytes_read;
		int rc = check_chunk(fw, dbuf, fd, &expected_offset, stats,
			&bytes_read);
		if (rc == 0 && bytes_read == 0) {
			stats->read_all = true;
			break;
		}
		assert((bytes_read & (block_size - 1)) == 0);
		measure(fw, bytes_read >> block_order);
		if (rc != 0) {
			saved_errno = rc;
			break;
		}
	}
	end_measurement(fw);
	assert(!clock_gettime(CLOCK_MONOTONIC, &file_t2));

	print_status(stats);
	if (!stats->read_all) {
		assert(saved_errno != 0);
		printf(" - NOT fully read due to \"%s\"",
			strerror(saved_errno));
	} else if (saved_errno != 0) {
		printf(" - %s", strerror(saved_errno));
	} else if (stats->bytes_read > 0) {
		uint64_t file_time_ns = diff_timespec_ns(&file_t1, &file_t2);

		if (file_time_ns > 0) {
			double file_avg_speed =
				stats->bytes_read * 1000000000.0 / file_time_ns;
			const char *unit = adjust_unit(&file_avg_speed);
			printf(" (Average: %.2f %s/s)", file_avg_speed, unit);
		}
	}
	printf("\n");

	close(fd);
	free(full_fn);
}

static uint64_t get_total_blocks(const char *path, const uint64_t *files,
	unsigned int block_order)
{
	const unsigned int block_size = 1U << block_order;
	uint64_t total_blocks = 0;

	while (*files != (uint64_t)-1) {
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

		assert((st.st_size & (block_size - 1)) == 0);
		total_blocks += st.st_size >> block_order;

		free(full_fn);
		files++;
	}
	return total_blocks;
}

static void iterate_files(const char *path, const uint64_t *files,
	uint64_t start_at, uint64_t end_at, uint64_t max_read_rate,
	int progress)
{
	const unsigned int block_order = get_block_order(path);
	struct block_stats tot_stats = {0, 0, 0, 0};
	uint64_t tot_size = 0;
	int and_read_all = 1;
	int or_missing_file = 0;
	uint64_t number = start_at;
	struct flow fw;
	struct dynamic_buffer dbuf;

	UNUSED(end_at);

	init_flow(&fw, block_order, get_total_blocks(path, files, block_order),
		max_read_rate, progress ? printf_flush_cb : dummy_cb, 0);
	dbuf_init(&dbuf);

	printf("                  SECTORS      ok/corrupted/changed/overwritten\n");
	while (*files != (uint64_t)-1) {
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

		validate_file(&fw, &dbuf, path, *files, &stats);
		tot_stats.ok += stats.secs.ok;
		tot_stats.bad += stats.secs.bad;
		tot_stats.changed += stats.secs.changed;
		tot_stats.overwritten += stats.secs.overwritten;
		tot_size += stats.bytes_read;
		and_read_all = and_read_all && stats.read_all;
		files++;
	}
	assert((tot_stats.ok + tot_stats.bad + tot_stats.changed +
		tot_stats.overwritten) << SECTOR_ORDER == tot_size);

	/* Notice that not reporting `missing' files after the last file
	 * in @files is important since @end_at could be very large.
	 */

	print_stats(&tot_stats, SECTOR_ORDER, "sector");
	if (or_missing_file)
		printf("WARNING: Not all F3 files in the range %" PRIu64 " to %" PRIu64 " are available\n",
			start_at + 1, number);
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	print_avg_seq_speed(&fw, "read", true);

	dbuf_free(&dbuf);
}

int main(int argc, char **argv)
{
	const uint64_t *files;

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

	adjust_dev_path(&args.dev_path);

	files = ls_my_files(args.dev_path, args.start_at, args.end_at);

	iterate_files(args.dev_path, files, args.start_at, args.end_at,
		args.max_read_rate, args.show_progress);
	free((void *)files);
	return 0;
}
