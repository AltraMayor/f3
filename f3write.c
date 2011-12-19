#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <unistd.h>
#include <err.h>
#include <alloca.h>
#include <math.h>
#include <dirent.h>

#include "utils.h"

static uint64_t fill_buffer(void *buf, size_t size, uint64_t offset)
{
	uint8_t *p, *ptr_next_sector, *ptr_end;
	struct drand48_data state;

	/* Assumed that size is not zero and a sector-size multiple. */
	assert(size % SECTOR_SIZE == 0);

	p = buf;
	ptr_end = p + size;
	while (p < ptr_end) {
		memmove(p, &offset, sizeof(offset));
		srand48_r(offset, &state);
		ptr_next_sector = p + SECTOR_SIZE;
		p += sizeof(offset);
		for (; p < ptr_next_sector; p += sizeof(long int))
			lrand48_r(&state, (long int *) p);
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
	/* Writing rate. */
	int		block_size;
	/* Blocks to write before measurement. */
	int		blocks_per_delay;
	/* Delay in miliseconds. */
	int		delay_ms;
	/* Number of measurements after reaching FW_STEADY state. */
	uint64_t	measurements;
	/* Number of measured blocks. */
	uint64_t	measured_blocks;
	/* State. */
	enum {FW_START, FW_SEARCH, FW_STEADY} state;
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

static inline void init_flow(struct flow *fw, uint64_t total_size, int progress)
{
	fw->total_size		= total_size;
	fw->total_written	= 0;
	fw->progress		= progress;
	fw->block_size		= 1024;	/* 1KB		*/
	fw->blocks_per_delay	= 1;	/* 1KB/s	*/
	fw->delay_ms		= 1000;	/* 1s		*/
	fw->measurements	= 0;
	fw->measured_blocks	= 0;
	fw->state		= FW_START;
	fw->erase		= 0;
	assert(fw->block_size % SECTOR_SIZE == 0);
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
	case FW_START:
		if (delay > fw->delay_ms) {
			fw->bpd1 = fw->blocks_per_delay / 2;
			fw->bpd2 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
			assert(fw->bpd1 > 0);
			fw->state = FW_SEARCH;
		} else if (delay < fw->delay_ms) {
			fw->blocks_per_delay *= 2;
		} else
			fw->state = FW_STEADY;
		break;

	case FW_SEARCH:
		if (fw->bpd2 - fw->bpd1 <= 3) {
			fw->state = FW_STEADY;
			break;
		}

		if (delay > fw->delay_ms) {
			fw->bpd2 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else if (delay < fw->delay_ms) {
			fw->bpd1 = fw->blocks_per_delay;
			fw->blocks_per_delay = (fw->bpd1 + fw->bpd2) / 2;
		} else
			fw->state = FW_STEADY;
		break;

	case FW_STEADY:
		fw->measurements++;
		fw->measured_blocks += fw->written_blocks;

		if (delay > fw->delay_ms) {
			if (fw->blocks_per_delay > 0)
				fw->blocks_per_delay--;
		} else if (delay < fw->delay_ms)
			fw->blocks_per_delay++;
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
	char filename[PATH_MAX];
	int fd, fine;
	void *buf;
	size_t remaining;
	uint64_t offset;
	ssize_t written;

	assert(size > 0);
	assert(size % fw->block_size == 0);

	/* Create the file. */
	assert(snprintf(filename, sizeof(filename), "%s/%04i.fff",
		path, number + 1) < sizeof(filename));
	printf("Creating file %04i.fff ... ", number + 1);
	fflush(stdout);
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == ENOSPC) {
			printf("No space left.\n");
			return 0;
		}
		err(errno, "Can't create file %s", filename);
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
				err(errno, "Write to file %s failed", filename);
		}
		assert(written == fw->block_size);
		remaining -= written;
		measure(fd, fw);
	}
	assert(!fine || remaining == 0);
	end_measurement(fd, fw);
	close(fd);
	
	printf("OK!\n");
	return fine;
}

static inline uint64_t get_freespace(const char *path)
{
	struct statvfs fs;
	assert(statvfs(path, &fs) == 0);
	return (uint64_t)fs.f_frsize * (uint64_t)fs.f_bfree;
}

static inline void pr_freespace(uint64_t fs)
{
	double f = (double)fs;
	const char *unit = adjust_unit(&f);
	printf("Free space: %.2f %s\n", f, unit);
}

static int fill_fs(const char *path, int progress)
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
	i = 0;
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

static void unlink_old_files(const char *path)
{
	DIR *ptr_dir;
	struct dirent *entry;
	const char *filename;

	ptr_dir = opendir(path);
	if (!ptr_dir)
		err(errno, "Can't open path %s", path);

	entry = readdir(ptr_dir);
	while (entry) {
		filename = entry->d_name;
		if (is_my_file(filename)) {
			char full_fn[PATH_MAX];
			get_full_fn(full_fn, sizeof(full_fn), path, filename);
			printf("Removing old file %s ...\n", filename);
			if (unlink(full_fn))
				err(errno, "Can't remove file %s", full_fn);
		}
		entry = readdir(ptr_dir);
	}
	closedir(ptr_dir);
}

int main(int argc, char *argv[])
{
	if (argc == 2) {
		const char *path = argv[1];
		unlink_old_files(path);
		/* If stdout isn't a terminal, supress progress. */
		return fill_fs(path, isatty(STDOUT_FILENO));
	}

	fprintf(stderr, "Usage: f3write <PATH>\n");
	return 1;
}
