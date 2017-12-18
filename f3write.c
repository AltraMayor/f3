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
#include <math.h>
#include <argp.h>

#include "utils.h"
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
	{"show-progress",	'p',	"NUM",		0,
		"Show progress if NUM is not zero",			0},
	{ 0 }
};

struct args {
	long        start_at;
	long        end_at;
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

struct flow {
	/* Total number of bytes to be written. */
	uint64_t	total_size;
	/* Total number of bytes already written. */
	uint64_t	total_written;
	/* If true, show progress. */
	int		progress;
	/* Writing rate in bytes. */
	int		block_size;
	/* Increment to apply to @blocks_per_delay. */
	int		step;
	/* Blocks to write before measurement. */
	int		blocks_per_delay;
	/* Delay in miliseconds. */
	int		delay_ms;
	/* Number of measurements after reaching FW_STEADY state. */
	uint64_t	measurements;
	/* Number of measured blocks. */
	uint64_t	measured_blocks;
	/* State. */
	enum {FW_INC, FW_DEC, FW_SEARCH, FW_STEADY} state;
	/* Number of characters to erase before printing out progress. */
	int		erase;

	/*
	 * Initialized while measuring
	 */

	/* Number of blocks written since last measurement. */
	int		written_blocks;
	/* Range of blocks_per_delay while in FW_SEARCH state. */
	int		bpd1, bpd2;
	/* Time measurements. */
	struct timeval	t1, t2;
};

static inline void move_to_inc_at_start(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_INC;
}

static void init_flow(struct flow *fw, uint64_t total_size, int progress)
{
	fw->total_size		= total_size;
	fw->total_written	= 0;
	fw->progress		= progress;
	fw->block_size		= 1024;	/* 1KB		*/
	fw->blocks_per_delay	= 1;	/* 1KB/s	*/
	fw->delay_ms		= 1000;	/* 1s		*/
	fw->measurements	= 0;
	fw->measured_blocks	= 0;
	fw->erase		= 0;
	assert(fw->block_size > 0);
	assert(fw->block_size % SECTOR_SIZE == 0);

	move_to_inc_at_start(fw);
}

static inline void start_measurement(struct flow *fw)
{
	fw->written_blocks = 0;
	assert(!gettimeofday(&fw->t1, NULL));
}

static inline void repeat_ch(char ch, int count)
{
	while (count > 0) {
		printf("%c", ch);
		count--;
	}
}

static void erase(int count)
{
	if (count <= 0)
		return;
	repeat_ch('\b',	count);
	repeat_ch(' ',	count);
	repeat_ch('\b',	count);
}

/* Average writing speed in byte/s. */
static inline double get_avg_speed(struct flow *fw)
{
	return	(double)(fw->measured_blocks * fw->block_size * 1000) /
		(double)(fw->measurements * fw->delay_ms);
}

static int pr_time(double sec)
{
	int has_h, has_m;
	int c, tot;

	tot = printf(" -- ");
	assert(tot > 0);

	has_h = sec >= 3600;
	if (has_h) {
		double h = floor(sec / 3600);
		c = printf("%i:", (int)h);
		assert(c > 0);
		tot += c;
		sec -= h * 3600;
	}

	has_m = has_h || sec >= 60;
	if (has_m) {
		double m = floor(sec / 60);
		if (has_h)
			c = printf("%02i:", (int)m);
		else
			c = printf("%i:", (int)m);
		assert(c > 0);
		tot += c;
		sec -= m * 60;
	}

	if (has_m)
		c = printf("%02i", (int)round(sec));
	else
		c = printf("%is", (int)round(sec));
	assert(c > 0);
	return tot + c;
}

static inline void update_mean(struct flow *fw)
{
	fw->measurements++;
	fw->measured_blocks += fw->written_blocks;
}

static inline void move_to_steady(struct flow *fw)
{
	update_mean(fw);
	fw->state = FW_STEADY;
}

static void move_to_search(struct flow *fw, int bpd1, int bpd2)
{
	assert(bpd1 > 0);
	assert(bpd2 >= bpd1);

	fw->blocks_per_delay = (bpd1 + bpd2) / 2;
	if (bpd2 - bpd1 <= 3) {
		move_to_steady(fw);
		return;
	}

	fw->bpd1 = bpd1;
	fw->bpd2 = bpd2;
	fw->state = FW_SEARCH;
}

static inline void dec_step(struct flow *fw)
{
	if (fw->blocks_per_delay - fw->step > 0) {
		fw->blocks_per_delay -= fw->step;
		fw->step *= 2;
	} else
		move_to_search(fw, 1, fw->blocks_per_delay + fw->step / 2);
}

static inline void inc_step(struct flow *fw)
{
	fw->blocks_per_delay += fw->step;
	fw->step *= 2;
}

static inline void move_to_inc(struct flow *fw)
{
	move_to_inc_at_start(fw);
	inc_step(fw);
}

static inline void move_to_dec(struct flow *fw)
{
	fw->step = 1;
	fw->state = FW_DEC;
	dec_step(fw);
}

static int measure(int fd, struct flow *fw, ssize_t written)
{
	long delay;
	div_t result = div(written, fw->block_size);

	assert(result.rem == 0);
	fw->written_blocks += result.quot;
	fw->total_written += written;

	if (fw->written_blocks < fw->blocks_per_delay)
		return 0;

	if (fdatasync(fd) < 0)
		return -1; /* Caller can read errno(3). */

	assert(!gettimeofday(&fw->t2, NULL));
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
	delay = delay_ms(&fw->t1, &fw->t2);

	switch (fw->state) {
	case FW_INC:
		if (delay > fw->delay_ms) {
			move_to_search(fw,
				fw->blocks_per_delay - fw->step / 2,
				fw->blocks_per_delay);
		} else if (delay < fw->delay_ms) {
			inc_step(fw);
		} else
			move_to_steady(fw);
		break;

	case FW_DEC:
		if (delay > fw->delay_ms) {
			dec_step(fw);
		} else if (delay < fw->delay_ms) {
			move_to_search(fw, fw->blocks_per_delay,
				fw->blocks_per_delay + fw->step / 2);
		} else
			move_to_steady(fw);
		break;

	case FW_SEARCH:
		if (fw->bpd2 - fw->bpd1 <= 3) {
			move_to_steady(fw);
			break;
		}

		if (delay > fw->delay_ms) {
			fw->bpd2 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else if (delay < fw->delay_ms) {
			fw->bpd1 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else
			move_to_steady(fw);
		break;

	case FW_STEADY:
		update_mean(fw);

		if (delay <= fw->delay_ms) {
			move_to_inc(fw);
		}
		else if (fw->blocks_per_delay > 1) {
			move_to_dec(fw);
		}
		break;

	default:
		assert(0);
	}

	if (fw->progress) {
		/* Instantaneous speed. */
		double inst_speed =
			(double)fw->blocks_per_delay * fw->block_size * 1000 /
			fw->delay_ms;
		const char *unit = adjust_unit(&inst_speed);
		double percent;
		/* The following shouldn't be necessary, but sometimes
		 * the initial free space isn't exactly reported
		 * by the kernel; this issue has been seen on Macs.
		 */
		if (fw->total_size < fw->total_written)
			fw->total_size = fw->total_written;
		percent = (double)fw->total_written * 100 / fw->total_size;
		erase(fw->erase);
		fw->erase = printf("%.2f%% -- %.2f %s/s",
			percent, inst_speed, unit);
		assert(fw->erase > 0);
		if (fw->measurements > 0)
			fw->erase += pr_time(
				(fw->total_size - fw->total_written) /
				get_avg_speed(fw));
		fflush(stdout);
	}

	start_measurement(fw);
	return 0;
}

static int end_measurement(int fd, struct flow *fw)
{
	/* Erase progress information. */
	erase(fw->erase);
	fw->erase = 0;
	fflush(stdout);

	if (fdatasync(fd) < 0)
		return -1; /* Caller can read errno(3). */
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
	return 0;
}

#define MAX_WRITE_SIZE	(1<<21)	/* 2MB */

/* Return true when disk is full. */
static int create_and_fill_file(const char *path, long number, size_t size,
	struct flow *fw)
{
	char *full_fn;
	const char *filename;
	int fd, saved_errno;
	char buf[MAX_WRITE_SIZE];
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
		ssize_t written;
		ssize_t write_size = fw->block_size *
			(fw->blocks_per_delay - fw->written_blocks);
		assert(write_size > 0);
		if (write_size > MAX_WRITE_SIZE)
			write_size = MAX_WRITE_SIZE;
		if ((size_t)write_size > remaining)
			write_size = remaining;
		offset = fill_buffer(buf, write_size, offset);
		written = write(fd, buf, write_size);
		if (written < 0) {
			saved_errno = errno;
			break;
		}
		if (written < write_size) {
			/* It must have filled up the file system.
			 * errno should be equal to ENOSPC.
			 */
			assert(write(fd, buf + written, write_size - written)
				< 0);
			saved_errno = errno;
			break;
		}
		assert(written == write_size);
		remaining -= written;
		if (measure(fd, fw, written) < 0) {
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

	if (saved_errno == ENOSPC || remaining == 0) {
		printf("OK!\n");
		return saved_errno == ENOSPC;
	}

	/* Something went wrong. */
	assert(saved_errno);
	printf("Write failure: %s\n", strerror(saved_errno));
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

static int fill_fs(const char *path, long start_at, long end_at, int progress)
{
	uint64_t free_space;
	struct flow fw;
	long i;

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

	init_flow(&fw, free_space, progress);
	for (i = start_at; i <= end_at; i++)
		if (create_and_fill_file(path, i, GIGABYTES, &fw))
			break;

	/* Final report. */
	pr_freespace(get_freespace(path));
	/* Writing speed. */
	if (fw.measurements > 0) {
		double speed = get_avg_speed(&fw);
		const char *unit = adjust_unit(&speed);
		printf("Average writing speed: %.2f %s/s\n", speed, unit);
	} else
		printf("Writing speed not available\n");

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
		/* If stdout isn't a terminal, supress progress. */
		.show_progress	= isatty(STDOUT_FILENO),
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "write");

	unlink_old_files(args.dev_path, args.start_at, args.end_at);

	return fill_fs(args.dev_path, args.start_at, args.end_at,
		args.show_progress);
}
