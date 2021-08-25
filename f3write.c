#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <err.h>
#include <argp.h>

#include "utils.h"
#include "libflow.h"
#include "version.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Write " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<PATH>";

static char doc[] = "F3 Write -- fill a drive out with .h2w files "
	"to test its real capacity";

static struct argp_option options[] = {
	{"start-at",		's',	"NUM",		0,
		"First NUM.h2w file to be written",			1},
	{"end-at",		'e',	"NUM",		0,
		"Last NUM.h2w file to be written",			0},
	{"max-write-rate",	'w',	"KB/s",		0,
		"Maximum write rate",					0},
	{"show-progress",	'p',	"NUM",		0,
		"Show progress if NUM is not zero",			0},
	{ 0 }
};

struct args {
	long		start_at;
	long		end_at;
	long		max_write_rate;
	int		show_progress;
	const char	*dev_path;
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

	case 'w':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"KB/s must be greater than zero");
		args->max_write_rate = l;
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

static uint64_t fill_buffer(void *buf, size_t size, uint64_t offset)
{
	const int num_int64 = SECTOR_SIZE >> 3;
	uint8_t *p, *ptr_end;

	assert(size > 0);
	assert(size % SECTOR_SIZE == 0);

	p = buf;
	ptr_end = p + size;
	while (p < ptr_end) {
		uint64_t *sector = (uint64_t *)p;
		int i;
		sector[0] = offset;
		for (i = 1; i < num_int64; i++)
			sector[i] = random_number(sector[i - 1]);
		p += SECTOR_SIZE;
		offset += SECTOR_SIZE;
	}

	return offset;
}

/* XXX Avoid duplicate this function, which was copied from libdevs.c. */
static int write_all(int fd, const char *buf, size_t count)
{
	size_t done = 0;
	do {
		ssize_t rc = write(fd, buf + done, count - done);
		if (rc < 0) {
			/* The write() failed. */
			return errno;
		}
		done += rc;
	} while (done < count);
	return 0;
}

static int write_chunk(int fd, size_t chunk_size, uint64_t *poffset)
{
	char buf[MAX_BUFFER_SIZE];

	while (chunk_size > 0) {
		size_t turn_size = chunk_size <= MAX_BUFFER_SIZE
			? chunk_size : MAX_BUFFER_SIZE;
		int ret;
		chunk_size -= turn_size;
		*poffset = fill_buffer(buf, turn_size, *poffset);
		ret = write_all(fd, buf, turn_size);
		if (ret)
			return ret;
	}

	return 0;
}

/* Return true when disk is full. */
static int create_and_fill_file(const char *path, long number, size_t size,
	int *phas_suggested_max_write_rate, struct flow *fw)
{
	char *full_fn;
	const char *filename;
	int fd, saved_errno;
	size_t remaining;
	uint64_t offset;

	assert(size > 0);
	assert(size % fw->block_size == 0);

	/* Create the file. */
	full_fn = full_fn_from_number(&filename, path, number);
	assert(full_fn);
	printf("Creating file %s ... ", filename);
	fflush(stdout);
	fd = open(full_fn, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == ENOSPC) {
			printf("No space left.\n");
			free(full_fn);
			return true;
		}
		err(errno, "Can't create file %s", full_fn);
	}
	assert(fd >= 0);

	/* Write content. */
	saved_errno = 0;
	offset = (uint64_t)number * GIGABYTES;
	remaining = size;
	start_measurement(fw);
	while (remaining > 0) {
		uint64_t write_size = get_rem_chunk_size(fw);
		if (write_size > remaining)
			write_size = remaining;
		saved_errno = write_chunk(fd, write_size, &offset);
		if (saved_errno)
			break;
		remaining -= write_size;
		if (measure(fd, fw, write_size) < 0) {
			saved_errno = errno;
			break;
		}
	}
	if (end_measurement(fd, fw) < 0) {
		/* If a write failure has happened before, preserve it. */
		if (!saved_errno)
			saved_errno = errno;
	}
	close(fd);
	free(full_fn);

	if (saved_errno == 0 || saved_errno == ENOSPC) {
		if (saved_errno == 0)
			assert(remaining == 0);
		printf("OK!\n");
		return saved_errno == ENOSPC;
	}

	/* Something went wrong. */
	assert(saved_errno);
	printf("Write failure: %s\n", strerror(saved_errno));
	if (saved_errno == EIO && !*phas_suggested_max_write_rate) {
		*phas_suggested_max_write_rate = true;
		printf("\nWARNING:\nThe write error above may be due to your memory card overheating\nunder constant, maximum write rate. You can test this hypothesis\ntouching your memory card. If it is hot, you can try f3write\nagain, once your card has cooled down, using parameter --max-write-rate=2048\nto limit the maximum write rate to 2MB/s, or another suitable rate.\n\n");
	}
	return false;
}

static inline uint64_t get_freespace(const char *path)
{
	struct statvfs fs;
	assert(!statvfs(path, &fs));
	return (uint64_t)fs.f_frsize * (uint64_t)fs.f_bfree;
}

static inline void pr_freespace(uint64_t fs)
{
	double f = (double)fs;
	const char *unit = adjust_unit(&f);
	printf("Free space: %.2f %s\n", f, unit);
}

static inline void pr_avg_speed(double speed)
{
	const char *unit = adjust_unit(&speed);
	printf("Average writing speed: %.2f %s/s\n", speed, unit);
}

static int flush_chunk(const struct flow *fw, int fd)
{
	UNUSED(fw);

	if (fdatasync(fd) < 0)
		return -1; /* Caller can read errno(3). */

	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
	return 0;
}

static int fill_fs(const char *path, long start_at, long end_at,
	long max_write_rate, int progress)
{
	uint64_t free_space;
	struct flow fw;
	long i;
	int has_suggested_max_write_rate = max_write_rate > 0;
	struct timeval t1, t2;

	free_space = get_freespace(path);
	pr_freespace(free_space);
	if (free_space <= 0) {
		printf("No space!\n");
		return 1;
	}

	i = end_at - start_at + 1;
	if (i > 0 && (uint64_t)i <= (free_space >> 30)) {
		/* The amount of data to write is less than the space available,
		 * update @free_space to improve estimate of time to finish.
		 */
		free_space = (uint64_t)i << 30;
	} else {
		/* There are more data to write than space available.
		 * Reduce @end_at to reduce the number of error messages
		 * when multiple write failures happens.
		 *
		 * One should not subtract the value below of one because
		 * the expression (free_space >> 30) is an integer division,
		 * that is, it ignores the remainder.
		 */
		end_at = start_at + (free_space >> 30);
	}

	init_flow(&fw, free_space, max_write_rate, progress, flush_chunk);
	assert(!gettimeofday(&t1, NULL));
	for (i = start_at; i <= end_at; i++)
		if (create_and_fill_file(path, i, GIGABYTES,
			&has_suggested_max_write_rate, &fw))
			break;
	assert(!gettimeofday(&t2, NULL));

	/* Final report. */
	pr_freespace(get_freespace(path));
	/* Writing speed. */
	if (has_enough_measurements(&fw)) {
		pr_avg_speed(get_avg_speed(&fw));
	} else {
		/* If the drive is too fast for the measurements above,
		 * try a coarse approximation of the writing speed.
		 */
		int64_t total_time_ms = delay_ms(&t1, &t2);
		if (total_time_ms > 0) {
			pr_avg_speed(get_avg_speed_given_time(&fw,
				total_time_ms));
		} else {
			printf("Writing speed not available\n");
		}
	}

	return 0;
}

static void unlink_old_files(const char *path, long start_at, long end_at)
{
	const long *files = ls_my_files(path, start_at, end_at);
	const long *number = files;
	while (*number >= 0) {
		char *full_fn;
		const char *filename;
		full_fn = full_fn_from_number(&filename, path, *number);
		assert(full_fn);
		printf("Removing old file %s ...\n", filename);
		if (unlink(full_fn))
			err(errno, "Can't remove file %s", full_fn);
		number++;
		free(full_fn);
	}
	free((void *)files);
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.start_at	= 0,
		.end_at		= LONG_MAX - 1,
		.max_write_rate = 0,
		/* If stdout isn't a terminal, suppress progress. */
		.show_progress	= isatty(STDOUT_FILENO),
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "write");

	unlink_old_files(args.dev_path, args.start_at, args.end_at);

	return fill_fs(args.dev_path, args.start_at, args.end_at,
		args.max_write_rate, args.show_progress);
}
