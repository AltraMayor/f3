#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <unistd.h>
#include <err.h>
#include <alloca.h>
#include <math.h>

#include "utils.h"

static uint64_t fill_buffer(void *buf, size_t size, uint64_t offset)
{
	uint8_t *p, *ptr_next_sector, *ptr_end;
	uint64_t rn;

	assert(size > 0);
	assert(size % SECTOR_SIZE == 0);
	assert(SECTOR_SIZE >= sizeof(offset) + sizeof(rn));
	assert((SECTOR_SIZE - sizeof(offset)) % sizeof(rn) == 0);

	p = buf;
	ptr_end = p + size;
	while (p < ptr_end) {
		rn = offset;
		memmove(p, &offset, sizeof(offset));
		ptr_next_sector = p + SECTOR_SIZE;
		p += sizeof(offset);
		for (; p < ptr_next_sector; p += sizeof(rn)) {
			rn = random_number(rn);
			memmove(p, &rn, sizeof(rn));
		}
		assert(p == ptr_next_sector);
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

static void measure(int fd, struct flow *fw)
{
	long delay;

	fw->written_blocks++;
	fw->total_written += fw->block_size;

	if (fw->written_blocks < fw->blocks_per_delay)
		return;

	assert(!fdatasync(fd));
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
}

static inline void end_measurement(int fd, struct flow *fw)
{
	assert(!fdatasync(fd));
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	erase(fw->erase);
	fw->erase = 0;
	fflush(stdout);
}

static int create_and_fill_file(const char *path, int number, size_t size,
	struct flow *fw)
{
	char *full_fn;
	const char *filename;
	int fd, fine;
	void *buf;
	size_t remaining;
	uint64_t offset;
	ssize_t written;

	assert(size > 0);
	assert(size % fw->block_size == 0);

	/* Create the file. */
	
	fine = 0;
	full_fn = full_fn_from_number(&filename, path, number);
	assert(full_fn);
	printf("Creating file %s ... ", filename);
	fflush(stdout);
	fd = open(full_fn, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == ENOSPC) {
			printf("No space left.\n");
			goto out;
		}
		err(errno, "Can't create file %s", full_fn);
	}
	assert(fd >= 0);

	/* Obtain the buffer. */
	buf = alloca(fw->block_size);
	assert(buf);

	/* Write content. */
	fine = 1;
	offset = (uint64_t)number * GIGABYTES;
	remaining = size;
	start_measurement(fw);
	while (remaining > 0) {
		offset = fill_buffer(buf, fw->block_size, offset);
		written = write(fd, buf, fw->block_size);
		if (written < 0) {
			if (errno == ENOSPC) {
				fine = 0;
				break;
			} else
				err(errno, "Write to file %s failed", full_fn);
		}
		assert(written == fw->block_size);
		remaining -= written;
		measure(fd, fw);
	}
	assert(!fine || remaining == 0);
	end_measurement(fd, fw);
	close(fd);
	
	printf("OK!\n");

out:
	free(full_fn);
	return fine;
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

static int fill_fs(const char *path, int start_at, int progress)
{
	uint64_t free_space;
	struct flow fw;
	int i, fine;

	free_space = get_freespace(path);
	pr_freespace(free_space);
	if (free_space <= 0) {
		printf("No space!\n");
		return 1;
	}

	init_flow(&fw, free_space, progress);
	i = start_at;
	fine = 1;
	do {
		fine = create_and_fill_file(path, i, GIGABYTES, &fw);
		i++;
	} while (fine);

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

static void unlink_old_files(const char *path, int start_at)
{
	const int *files = ls_my_files(path, start_at);
	const int *number = files;
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

int main(int argc, char *argv[])
{
	int start_at;
	const char *path;
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

	unlink_old_files(path, start_at);
	/* If stdout isn't a terminal, supress progress. */
	progress = isatty(STDOUT_FILENO);
	return fill_fs(path, start_at, progress);

error:
	print_header(stderr, "write");
	fprintf(stderr, "Usage: f3write [%sNUM] <PATH>\n", START_AT_TEXT);
	return 1;
}
