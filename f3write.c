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
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <err.h>

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

#define BLANK	"          "
#define CLEAR	"\b\b\b\b\b\b\b\b\b\b"
#define CLEAR2	(CLEAR BLANK CLEAR)

static int create_and_fill_file(const char *path, int number,
	size_t block_size, size_t size, int progress)
{
	char filename[PATH_MAX];
	int fd, fine;
	void *buf;
	uint64_t offset;
	size_t to_write;
	ssize_t written;
	struct timeval t1 = { .tv_sec = -1000, .tv_usec = 0 };

	/* Assumed that sizes are sector-size multiples. */
	assert(block_size % SECTOR_SIZE == 0);
	assert(size % SECTOR_SIZE == 0);

	/* Create the file. */
	snprintf(filename, PATH_MAX, "%s/%04i.fff", path, number + 1);
	printf("Creating file %04i.fff ... %s", number + 1,
		progress ? BLANK : "");
	fflush(stdout);
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == ENOSPC) {
			printf("%sNo space left.\n", progress ? CLEAR2 : "");
			return 0;
		}
		err(errno, "Can't create file %s", filename);
	}
	assert(fd >= 0);

	/* Obtain the buffer. */
	buf = malloc(block_size);
	assert(buf);

	/* Write content. */
	fine = 1;
	offset = (uint64_t) number *GIGABYTES;
	while (size > 0) {
		offset = fill_buffer(buf, block_size, offset);
		to_write = block_size <= size ? block_size : size;
		written = write(fd, buf, to_write);
		if (written < 0) {
			if (errno == ENOSPC) {
				fine = 0;
				break;
			} else
				err(errno, "Write to file %s failed", filename);
		}
		assert(written == to_write);
		size -= written;
		if (progress) {
			struct timeval t2;
			int delay_ms;
			assert(!gettimeofday(&t2, NULL));
			delay_ms =	(t2.tv_sec  - t1.tv_sec)  * 1000 +
					(t2.tv_usec - t1.tv_usec) / 1000;
			if (delay_ms >= 200) { /* Avoid often printouts. */
				printf(CLEAR "%10lu", size);
				fflush(stdout);
				t1 = t2;
			}
		}
	}
	assert(!fine || size == 0);

	/* Release resources. */
	free(buf);
	close(fd);
	printf("%sOK!\n", progress ? CLEAR2 : "");
	return fine;
}

static int fill_fs(const char *path, int progress)
{
	struct statvfs fs;
	double free_space1, free_space2;
	time_t t1, t2, dt;
	int i, fine;
	size_t block_size;
	double f, write_speed;
	const char *unit;

	/* Obtain initial free_space, and block_size. */
	assert(statvfs(path, &fs) == 0);
	block_size = fs.f_frsize;
	free_space1 = (double)block_size * (double)fs.f_bfree;
	f = free_space1;
	unit = adjust_unit(&f);
	printf("Free space: %.2f %s\n", f, unit);

	/* Obtain initial time. */
	t1 = time(NULL);

	i = 0;
	fine = 1;
	do {
		fine = create_and_fill_file(path, i, block_size,
			GIGABYTES, progress);
		i++;
	} while (fine);

	/* Final report. */
	assert(statvfs(path, &fs) == 0);
	free_space2 = (double)block_size * (double)fs.f_bfree;
	f = free_space2;
	unit = adjust_unit(&f);
	printf("Free space: %.2f %s\n", f, unit);

	/* Writing speed. */
	t2 = time(NULL);
	dt = t2 - t1;
	dt = dt > 0 ? dt : 1;
	write_speed = (free_space1 - free_space2) / (double)dt;
	unit = adjust_unit(&write_speed);
	printf("Writing speed: %.2f %s/s\n", write_speed, unit);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 3 && !strcmp(argv[1], "--no-progress")) {
		return fill_fs(argv[2], 0);
	} else if (argc == 2) {
		/* If stdout isn't a terminal, supress progress. */
		return fill_fs(argv[1], isatty(STDOUT_FILENO));
	}

	fprintf(stderr, "Usage: f3write [--no-progress] <PATH>\n\n"
		"Use parameter --no-progress if you think that the progress "
		"provided while\na file is being written is negatively "
		"affecting the writing speed measurement.\n"
		"Please notice that progress is only shown on terminals.\n");
	return 1;
}
