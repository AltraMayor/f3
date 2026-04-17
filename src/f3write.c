#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <err.h>
#include <argp.h>
#include <inttypes.h>

#include "libutils.h"
#include "libfile.h"
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
	uint64_t	start_at;
	uint64_t	end_at;
	uint64_t	max_write_rate;
	int		show_progress;
	const char	*dev_path;
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

	case 'w':
		ll = arg_to_ll_bytes(state, arg);
		if (ll <= 0)
			argp_error(state,
				"KB/s must be greater than zero");
		args->max_write_rate = ll;
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

static void fill_buffer(char *buf, uint64_t sectors, uint64_t *poffset)
{
	uint64_t i;
	for (i = 0; i < sectors; i++) {
		fill_buffer_with_block(buf, SECTOR_ORDER, *poffset, 0);
		buf += SECTOR_SIZE;
		*poffset += SECTOR_SIZE;
	}
}

static int write_all(int fd, const char *buf, size_t *pbytes)
{
	ssize_t rc;
	size_t done = 0;
	do {
		rc = write(fd, buf + done, *pbytes - done);
		if (rc < 0) {
			rc = errno;
			goto out;
		}
		done += rc;
	} while (done < *pbytes);
	rc = 0;

out:
	*pbytes = done;
	return rc;
}

static int write_chunk(struct flow *fw, struct dynamic_buffer *dbuf,
	int fd, uint64_t remaining_blocks, uint64_t *poffset,
	size_t *ptot_bytes_written)
{
	int rc = 0;
	uint64_t chunk_size = MIN(get_rem_chunk_blocks(fw), remaining_blocks)
		<< fw_get_block_order(fw);
	size_t len = chunk_size;
	char * const buf = dbuf_get_buf(dbuf, 0, &len);
	size_t tot_bytes_written = 0;

	while (chunk_size > 0) {
		const size_t turn_size = MIN(chunk_size, len);
		size_t bytes_written = turn_size;

		assert((turn_size & (SECTOR_SIZE - 1)) == 0);
		fill_buffer(buf, turn_size >> SECTOR_ORDER, poffset);

		rc = write_all(fd, buf, &bytes_written);
		tot_bytes_written += bytes_written;
		if (rc != 0)
			goto out;
		assert(bytes_written == turn_size);
		chunk_size -= turn_size;
	}

out:
	*ptot_bytes_written = tot_bytes_written;
	return rc;
}

/* Return true when disk is full. */
static int create_and_fill_file(struct flow *fw, struct dynamic_buffer *dbuf,
	const char *path, uint64_t number, int *phas_suggested_max_write_rate)
{
	const unsigned int block_size = fw_get_block_size(fw);
	const unsigned int block_order = fw_get_block_order(fw);
	const uint64_t total_file_blocks =
		1ULL << (GIGABYTE_ORDER - block_order);
	uint64_t remaining_blocks = total_file_blocks;
	char *full_fn;
	const char *filename;
	int fd, saved_errno;
	uint64_t offset;
	struct timespec file_t1, file_t2;

	assert(GIGABYTE_ORDER >= block_order);

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
	offset = number << GIGABYTE_ORDER;
	assert(!clock_gettime(CLOCK_MONOTONIC, &file_t1));
	start_measurement(fw);
	while (remaining_blocks > 0) {
		size_t bytes_written;
		uint64_t written_blocks;

		saved_errno = write_chunk(fw, dbuf, fd, remaining_blocks,
			&offset, &bytes_written);
		if (bytes_written == 0)
			break;

		/* Push data to drive and tip the kernel. */
		if (fdatasync(fd) < 0) {
			/* Preserve the first error. */
			if (saved_errno == 0)
				saved_errno = errno;
		}
		if (saved_errno == 0) {
			saved_errno = posix_fadvise(fd, 0, 0,
				POSIX_FADV_DONTNEED);
		}

		assert((bytes_written & (block_size - 1)) == 0);
		written_blocks = bytes_written >> block_order;
		measure(fw, written_blocks);
		remaining_blocks -= written_blocks;

		if (saved_errno != 0)
			break;
	}
	end_measurement(fw);
	assert(!clock_gettime(CLOCK_MONOTONIC, &file_t2));
	close(fd);
	free(full_fn);

	if (saved_errno == 0 || saved_errno == ENOSPC) {
		uint64_t file_time_ns = diff_timespec_ns(&file_t1, &file_t2);

		if (saved_errno == 0)
			assert(remaining_blocks == 0);
		
		if (file_time_ns > 0) {
			const uint64_t written_bytes =
				(total_file_blocks - remaining_blocks) <<
				block_order;
			double file_avg_speed =
				written_bytes * 1000000000.0 / file_time_ns;
			const char *unit = adjust_unit(&file_avg_speed);

			printf("OK! (Average: %.2f %s/s)\n",
				file_avg_speed, unit);
		} else {
			printf("OK!\n");
		}
		return saved_errno == ENOSPC;
	}

	/* Something went wrong. */
	assert(saved_errno != 0);
	printf("Write failure: %s\n", strerror(saved_errno));
	if (saved_errno == EIO && !*phas_suggested_max_write_rate) {
		*phas_suggested_max_write_rate = true;
		printf("\nWARNING:\nThe write error above may be due to your memory card overheating\nunder constant, maximum write rate. You can test this hypothesis\ntouching your memory card. If it is hot, you can try f3write\nagain, once your card has cooled down, using parameter --max-write-rate=2048\nto limit the maximum write rate to 2MB/s, or another suitable rate.\n\n");
	}
	return false;
}

static inline void pr_freespace(uint64_t fs)
{
	double f = (double)fs;
	const char *unit = adjust_unit(&f);
	printf("Free space: %.2f %s\n", f, unit);
}

static int fill_fs(const char *path, uint64_t start_at, uint64_t end_at,
	uint64_t max_write_rate, int progress)
{
	const unsigned int block_order = get_block_order(path);
	uint64_t free_blocks = get_free_blocks(path);
	struct flow fw;
	struct dynamic_buffer dbuf;
	uint64_t i;
	int has_suggested_max_write_rate = max_write_rate > 0;

	pr_freespace(free_blocks << block_order);
	if (free_blocks == 0) {
		printf("No space!\n");
		return 1;
	}

	assert(start_at <= end_at);
	assert(GIGABYTE_ORDER >= block_order);
	i = end_at - start_at + 1;
	if (i <= (free_blocks >> (GIGABYTE_ORDER - block_order))) {
		/* The amount of data to write is less than the space available,
		 * update free_blocks to improve estimate of time to finish.
		 */
		free_blocks = i << (GIGABYTE_ORDER - block_order);
	} else {
		/* There are more data to write than space available.
		 * Reduce end_at to reduce the number of error messages
		 * due to multiple write failures.
		 */
		end_at = start_at +
			(free_blocks >> (GIGABYTE_ORDER - block_order));
	}

	init_flow(&fw, block_order, free_blocks, max_write_rate,
		progress ? printf_flush_cb : dummy_cb, 0);
	dbuf_init(&dbuf);
	for (i = start_at; i <= end_at; i++) {
		if (create_and_fill_file(&fw, &dbuf, path, i,
				&has_suggested_max_write_rate))
			break;
	}
	dbuf_free(&dbuf);

	/* Final report. */
	pr_freespace(get_free_blocks(path) << block_order);
	print_avg_seq_speed(&fw, "write", true);
	return 0;
}

static void unlink_old_files(const char *path,
	uint64_t start_at, uint64_t end_at)
{
	const uint64_t *files = ls_my_files(path, start_at, end_at);
	const uint64_t *number = files;
	while (*number != (uint64_t)-1) {
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

	adjust_dev_path(&args.dev_path);

	unlink_old_files(args.dev_path, args.start_at, args.end_at);

	return fill_fs(args.dev_path, args.start_at, args.end_at,
		args.max_write_rate, args.show_progress);
}
