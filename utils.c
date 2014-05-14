#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>

#include "utils.h"

const char *adjust_unit(double *ptr_bytes)
{
	const char *units[] = { "Byte", "KB", "MB", "GB", "TB", "PB", "EB" };
	int i = 0;
	double final = *ptr_bytes;

	while (i < 7 && final >= 1024) {
		final /= 1024;
		i++;
	}
	*ptr_bytes = final;
	return units[i];
}

int is_my_file(const char *filename)
{
	const char *p = filename;

	if (!p || !isdigit(*p))
		return 0;

	/* Skip digits. */
	do {
		p++;
	} while (isdigit(*p));

	return	(p[0] == '.') && (p[1] == 'h') && (p[2] == '2') &&
		(p[3] == 'w') && (p[4] == '\0');
}

void full_fn_from_number(char *full_fn, const char **filename,
	const char *path, int num)
{
	assert(snprintf(full_fn, PATH_MAX, "%s/%i.h2w", path, num + 1) <
		PATH_MAX);
	*filename = full_fn + strlen(path) + 1;
}

int parse_start_at_param(const char *param)
{
	int text_len = strlen(START_AT_TEXT);
	long start_at;

	if (strncmp(param, START_AT_TEXT, text_len))
		return -1;

	start_at = strtol(param + text_len, NULL, 10);
	return (start_at <= 0 || start_at == LONG_MAX) ? -1 : start_at - 1;
}

static int number_from_filename(const char *filename)
{
	const char *p;
	int num;

	assert(is_my_file(filename));

	p = filename;
	num = 0;
	do {
		num = num * 10 + (*p - '0');
		p++;
	} while (isdigit(*p));

	return num - 1;
}

/* Don't call this function directly, use ls_my_files() instead. */
static int *__ls_my_files(DIR *dir, int start_at, int *pcount, int *pindex)
{
	struct dirent *entry;
	const char *filename;
	int number, my_index, *ret;

	entry = readdir(dir);
	if (!entry) {
		int *ret = malloc(sizeof(const int) * (*pcount + 1));
		assert(ret);
		*pindex = *pcount - 1;
		ret[*pcount] = -1;
		closedir(dir);
		return ret;
	}

	filename = entry->d_name;
	if (!is_my_file(filename))
		return __ls_my_files(dir, start_at, pcount, pindex);

	/* Cache @number because @entry may go away. */
	number = number_from_filename(filename);

	/* Ignore files before @start_at. */
	if (number < start_at)
		return __ls_my_files(dir, start_at, pcount, pindex);

	(*pcount)++;
	ret = __ls_my_files(dir, start_at, pcount, &my_index);
	ret[my_index] = number;
	*pindex = my_index - 1;
	return ret;
}

/* To be used with qsort(3). */
static int cmpintp(const void *p1, const void *p2)
{
	return *(const int *)p1 - *(const int *)p2;
}

const int *ls_my_files(const char *path, int start_at)
{
	DIR *dir = opendir(path);
	int my_count;
	int my_index;
	int *ret;

	if (!dir)
		err(errno, "Can't open path %s", path);

	my_count = 0;
	ret = __ls_my_files(dir, start_at, &my_count, &my_index);
	assert(my_index == -1);
	qsort(ret, my_count, sizeof(*ret), cmpintp);
	return ret;
}

void print_header(FILE *f, char *name)
{
	fprintf(f,
	"F3 %s 3.0\n"
	"Copyright (C) 2010 Digirati Internet LTDA.\n"
	"This is free software; see the source for copying conditions.\n"
	"\n", name);
}
