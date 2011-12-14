#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define SECTOR_SIZE 512
#define GIGABYTES   (1024 * 1024 * 1024)

int is_my_file (const char *filename)
{
  return
    (strlen(filename) == 8) &&
    isdigit(filename[0]) &&
    isdigit(filename[1]) &&
    isdigit(filename[2]) &&
    isdigit(filename[3]) &&
    (filename[4] == '.') &&
    (filename[5] == 'f') &&
    (filename[6] == 'f') &&
    (filename[7] == 'f');
}

uint64_t offset_from_filename (const char *filename)
{
  char str[5];
  uint64_t number;

  /* Obtain number. */
  assert(is_my_file(filename));
  strncpy(str, filename, 4);
  str[4] = '\0';
  number = (uint64_t)strtol(str, NULL, 10) - 1;

  return number * GIGABYTES;
}

#define ERRMSG_SIZE 256

void validate_file (const char *path, const char *filename)
{
  uint8_t sector[SECTOR_SIZE], *p, *ptr_end;
  FILE *f;
  int fine;
  size_t sectors_read;
  uint64_t offset, expected_offset;
  struct drand48_data state;
  long int rand_int;
  char full_fn[PATH_MAX], err_msg[ERRMSG_SIZE];

  printf("Validating file %s ...", filename);
  fflush(stdout);

  snprintf(full_fn, PATH_MAX, "%s/%s", path, filename);
  f = fopen(full_fn, "rb");
  if (!f)
    err(errno, "Can't open file %s", full_fn);

  ptr_end = sector + SECTOR_SIZE;
  fine = 1;
  sectors_read = fread(sector, SECTOR_SIZE, 1, f);
  expected_offset = offset_from_filename(filename);
  err_msg[0] = '\0';
  while (fine && sectors_read > 0)
  {
    assert(sectors_read == 1);
    offset = *((uint64_t *)sector);
    fine = offset == expected_offset;
    
    if (fine)
    {
      srand48_r(offset, &state);
      p = sector + sizeof(offset);
      for (; fine && p < ptr_end; p += sizeof(long int))
      {
        lrand48_r(&state, &rand_int);
        fine = rand_int == *((long int *)p);
      }

      if (fine)
      {
        sectors_read = fread(sector, SECTOR_SIZE, 1, f);
        expected_offset += SECTOR_SIZE;
      }
      else
        snprintf(err_msg, ERRMSG_SIZE, "Lost bits in sector %llu", offset);
    }
    else
      snprintf(err_msg, ERRMSG_SIZE,
        "Sector mismatch, expected %llu, found %llu", expected_offset, offset);
  }
  assert(!fine || feof(f));

  fclose(f);

  if (fine)
    printf(" OK!\n");
  else
    printf(" ERROR: %s\n", err_msg);
}

void iterate_path (const char *path)
{
  DIR *ptr_dir;
  struct dirent *entry;
  const char *filename;

  ptr_dir = opendir(path);
  assert(ptr_dir);

  entry = readdir(ptr_dir);
  while (entry)
  {
    filename = entry->d_name;
    if (is_my_file(filename))
      validate_file(path, filename);
    entry = readdir(ptr_dir);
  }  

  closedir(ptr_dir);
}

int main (int argc, char *argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "Usage: f3read <PATH>\n");
    return 1;
  }
  iterate_path(argv[1]);
  return 0;
}
