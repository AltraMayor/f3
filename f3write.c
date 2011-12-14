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

#define SECTOR_SIZE 512
#define GIGABYTES   (1024 * 1024 * 1024)

uint64_t fill_buffer (void *buf, size_t size, uint64_t offset)
{
  void *p, *ptr_next_sector, *ptr_end;
  struct drand48_data state;

  /* Assumed that size is a sector-size multiple. */
  assert(size % SECTOR_SIZE == 0);

  p = buf;
  ptr_end = buf + size;
  while (p < ptr_end)
  {
    memmove(p, (void *)&offset, sizeof(offset));
    srand48_r(offset, &state);
    ptr_next_sector = p + SECTOR_SIZE;
    p += sizeof(offset);
    for (; p < ptr_next_sector; p += sizeof(long int)) 
      lrand48_r(&state, (long int *)p);
    assert (p == ptr_next_sector);
    offset += SECTOR_SIZE;
  }

  return offset;
}

int create_and_fill_file (const char *path, int number,
                           size_t block_size, size_t size)
{
  char filename[PATH_MAX];
  int fd, fine;
  void *buf;
  uint64_t offset;
  size_t to_write;
  ssize_t written;

  /* Assumed that sizes are sector-size multiples. */
  assert(block_size % SECTOR_SIZE == 0);
  assert(size % SECTOR_SIZE == 0);

  /* Create the file. */
  snprintf(filename, PATH_MAX, "%s/%04i.fff", path, number + 1);
  printf("Creating file %04i.fff ...", number + 1);
  fflush(stdout);
  fd = open(filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);
  if (fd < 0)
    err(errno, "Can't create file %s", filename);
  assert(fd >= 0);
  
  /* Obtain the buffer. */
  buf = malloc(block_size);
  assert(buf);

  /* Write content. */
  fine = 1;
  offset = number * GIGABYTES;
  while (size > 0)
  {
    offset = fill_buffer(buf, block_size, offset);
    to_write = block_size <= size ? block_size : size;
    written = write(fd, buf, to_write);
    if (written < 0)
    {
      if (errno == ENOSPC)
      {
        fine = 0;
        break;
      }
      else
        err(errno, "Write to file %s failed", filename);
    }
    assert(written == to_write);
    size -= written;
  }
  assert(!fine || size == 0);

  /* Release resources. */
  free(buf);
  close(fd);
  printf(" OK!\n");
  return fine;
}

void fill_fs (const char *path)
{
  struct statvfs fs;
  uint64_t free_space;
  int i, fine;
  size_t block_size;

  /* Obtain initial free_space, and block_size. */
  assert(statvfs(path, &fs) == 0);
  free_space = (uint64_t)fs.f_bsize * (uint64_t)fs.f_bfree;
  block_size = fs.f_bsize;
  printf("Free space: %lli\n", free_space);

  i = 0;
  fine = 1;
  do
  {
    fine = create_and_fill_file(path, i, block_size, GIGABYTES);
    i++;
  } while (fine);

  /* Final report. */
  assert(statvfs(path, &fs) == 0);
  free_space = (uint64_t)fs.f_bsize * (uint64_t)fs.f_bfree;
  printf("Free space: %lli\n", free_space);
}

int main (int argc, char *argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "Usage: f3write <PATH>\n");
    return 1;
  }
  fill_fs(argv[1]);
  return 0;
}
