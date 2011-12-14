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

#define TOLERANCE 2

void validate_file (const char *path, const char *filename,
  uint64_t *ptr_ok, uint64_t *ptr_corrupted, uint64_t *ptr_changed,
  uint64_t *ptr_overwritten, uint64_t *ptr_size)
{
  uint8_t sector[SECTOR_SIZE], *p, *ptr_end;
  FILE *f;
  int offset_match, error_count;
  size_t sectors_read;
  uint64_t offset, expected_offset;
  struct drand48_data state;
  long int rand_int;
  char full_fn[PATH_MAX];

  snprintf(full_fn, PATH_MAX, "%s/%s", path, filename);
  f = fopen(full_fn, "rb");
  if (!f)
    err(errno, "Can't open file %s", full_fn);

  ptr_end = sector + SECTOR_SIZE;
  sectors_read = fread(sector, SECTOR_SIZE, 1, f);
  expected_offset = offset_from_filename(filename);
  while (sectors_read > 0)
  {
    assert(sectors_read == 1);
    offset = *((uint64_t *)sector);
    offset_match = offset == expected_offset;
    
    srand48_r(offset, &state);
    p = sector + sizeof(offset);
    error_count = 0;
    for (; error_count <= TOLERANCE && p < ptr_end; p += sizeof(long int))
    {
      lrand48_r(&state, &rand_int);
      if (rand_int != *((long int *)p))
        error_count++;
    }

    sectors_read = fread(sector, SECTOR_SIZE, 1, f);
    expected_offset += SECTOR_SIZE;

    if (offset_match)
    {
      if (error_count == 0)
        (*ptr_ok)++;
      else if (error_count <= TOLERANCE)
        (*ptr_changed)++;
      else
        (*ptr_corrupted)++;
    }
    else if (error_count <= TOLERANCE)
      (*ptr_overwritten)++;
    else
      (*ptr_corrupted)++;
  }
  assert(feof(f));
  *ptr_size += ftell(f);

  fclose(f);
}

char *adjust_unit (double *ptr_bytes)
{
  char *units[] = {"Byte", "KB", "MB", "GB", "TB"};
  int i = 0;
  double final = *ptr_bytes;
  
  while (i < 5 && final >= 1024)
  {
    final /= 1024;
    i++;
  }
  *ptr_bytes = final;
  return units[i];
}

void report (const char *prefix, uint64_t i)
{
  double f = (double)(i * SECTOR_SIZE);
  char *unit = adjust_unit(&f); 
  printf("%s %.2f %s (%llu sectors)\n", prefix, f, unit, i);
}

void iterate_path (const char *path)
{
  DIR *ptr_dir;
  struct dirent *entry;
  const char *filename, *unit;
  uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
  time_t t1, t2, dt;
  double read_speed;

  ptr_dir = opendir(path);
  if (!ptr_dir)
    err(errno, "Can't open path %s", path);
  assert(ptr_dir);

  /* Obtain initial time. */
  t1 = time(NULL);

  entry = readdir(ptr_dir);
  tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
  printf("                     SECTORS ok/corrupted/changed/overwritten\n");
  while (entry)
  {
    filename = entry->d_name;
    if (is_my_file(filename))
    {
      uint64_t sec_ok, sec_corrupted, sec_changed, sec_overwritten, file_size;
      printf("Validating file %s ...", filename);
      fflush(stdout);
      sec_ok = sec_corrupted = sec_changed = sec_overwritten = file_size = 0;
      validate_file(path, filename, &sec_ok, &sec_corrupted,
        &sec_changed, &sec_overwritten, &file_size);
      printf(" %llu/%llu/%llu/%llu\n", sec_ok, sec_corrupted,
        sec_changed, sec_overwritten);
      tot_ok += sec_ok;
      tot_corrupted += sec_corrupted;
      tot_changed += sec_changed;
      tot_overwritten += sec_overwritten;
      tot_size += file_size;
    }
    entry = readdir(ptr_dir);
  }  
  t2 = time(NULL);
  assert(tot_size/SECTOR_SIZE ==
    (tot_ok + tot_corrupted + tot_changed + tot_overwritten));

  closedir(ptr_dir);
  report("\n  Data OK:", tot_ok);
  report(  "Data LOST:", tot_corrupted + tot_changed + tot_overwritten);
  report("\t       Corrupted:", tot_corrupted);
  report("\tSlightly changed:", tot_changed);
  report("\t     Overwritten:", tot_overwritten);
  
  /* Reading speed. */
  dt = t2 - t1;
  dt = dt > 0 ? dt : 1;
  read_speed = (double)tot_size / (double)dt;
  unit = adjust_unit(&read_speed);
  printf("Reading speed: %.2f %s/s\n", read_speed, unit);
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
