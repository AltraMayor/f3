#include <spawn.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <crt_externs.h>  // for _NSGetEnviron()

#include "devices/partition.h"

/* Supported types. */
static const char *const disk_types[] = { "msdos", "mbr", "gpt", NULL };
static const char *const fs_types[]   = { "fat32", "exfat", "hfs+", "apfs", NULL };

static const char *scheme_from_type(const char *type)
{
	if (strcasecmp(type, "msdos") == 0) return "MBR";
	if (strcasecmp(type, "mbr")   == 0) return "MBR";
	if (strcasecmp(type, "gpt")   == 0) return "GPT";
	return NULL;  /* unsupported */
}

/* Type validation functions */
bool is_valid_disk_type(const char *t)
{
	for (const char *const *p = disk_types; *p; ++p)
		if (strcasecmp(*p, t) == 0)
			return true;
	return false;
}

bool is_valid_fs_type(const char *t)
{
	for (const char *const *p = fs_types; *p; ++p)
		if (strcasecmp(*p, t) == 0)
			return true;
	return false;
}

/* Spawn wrapper. */
static int run_diskutil(const char *disk, const char *scheme,
	const char *start_str, const char *fs_type,
	const char *label, const char *size_str)
{
	char **environ = *_NSGetEnviron();
	pid_t pid;
	int status;

	char *args[16]; int n = 0;
	args[n++] = "/usr/sbin/diskutil";
	args[n++] = "partitionDisk";
	args[n++] = (char *)disk;
	args[n++] = (char *)scheme;
	if (start_str) {
		/* Leading gap */
		args[n++] = "Free Space";
		args[n++] = "gap";
		args[n++] = (char *)start_str;
	}
	/* Main partition */
	args[n++] = (char *)fs_type; args[n++] = (char *)label; args[n++] = (char *)size_str;
	/* Tail gap to consume remainder (size 0) */
	args[n++] = "Free Space"; args[n++] = "tail"; args[n++] = "0";
	args[n++] = NULL;

#ifdef DEBUG
	fprintf(stderr, "DEBUG: ");
	for (int i = 0; args[i]; ++i) {
		fprintf(stderr, "%s%s", args[i], args[i+1] ? " " : "\n");
	}
#endif

	if (posix_spawn(&pid, args[0], NULL, NULL, args, environ) != 0)
		return 1;
	if (waitpid(pid, &status, 0) < 0)
		return 1;
	return (status == 0) ? 0 : 1;
}

/* Function to unmount a disk using diskutil. */
int unmount_disk(const char *disk) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd),
		"/usr/sbin/diskutil unmountDisk %s", disk);
#ifdef DEBUG
	fprintf(stderr, "DEBUG: %s\n", cmd);
#endif
	return system(cmd) == 0 ? 0 : 1;
}

/* Mark first slice bootable (ACTIVE) via fdisk.
 * Returns 0 on success. GPT is ignored.
 */
static int mbr_set_active(const char *disk, bool active)
{
	/* fdisk -e -y -u /dev/diskN  (then "f 1" or "f 0" + "w" + "q")   */
	char cmd[128];
	const char *redirect = "> /dev/null 2>&1";
#ifdef DEBUG
	redirect = "";
#endif
	snprintf(cmd, sizeof cmd,
		"echo '%s\nw\nq\n' | fdisk -e -y -u %s %s",
		active ? "f 1" : "f 0", disk, redirect);
#ifdef DEBUG
	fprintf(stderr, "DEBUG: %s\n", cmd);
#endif
	return system(cmd) == 0 ? 0 : 1;
}

/* Fix disk. */
int partition_create(const char *dev_filename, const struct partition_options *opt)
{
	const char *label = "f3fix";
	const char *disk = dev_filename;
	const char *scheme;
	int ret = 1;  // assume failure
	uint64_t length;
	char start_str[32];
	char size_str[32];

	/* Validate requested types. */
	if (!is_valid_disk_type(opt->disk_type) || !is_valid_fs_type(opt->fs_type))
		goto out;

	/* Device node (e.g. /dev/disk2). */
	if (!disk)
		goto out;

	scheme = scheme_from_type(opt->disk_type);
	if (!scheme)
		goto out;

	/* Compute slice length in sectors.
	 * GPT requires every user slice start/end on an 8-sector boundary (4 KiB).
	 * Trim *down* so we never trespass past last_sector even after alignment.
	 */
	length = opt->last_sector - opt->first_sector + 1ULL;
	if (strcasecmp(opt->disk_type, "gpt") == 0) {
		/* Round DOWN to 8-sector boundary (clear low 3 bits). */
		length &= ~7ULL;
	}

	snprintf(start_str, sizeof start_str, "%lluS",
		(unsigned long long)opt->first_sector);
	snprintf(size_str,  sizeof size_str,  "%lluS",
		(unsigned long long)length);

	ret = run_diskutil(disk,
		scheme,
		opt->first_sector > 0 ? start_str : NULL,
		opt->fs_type, label, size_str);
	if (ret)
		goto out;
	
	unmount_disk(disk);

	if (opt->boot && strcmp(scheme, "MBR") == 0)
		ret = mbr_set_active(disk, true);

out:
	return ret;
}

/* Array helpers. */
static size_t build_types_array(const char *const src[], char ***out)
{
	char **types = NULL;
	size_t count = 0;
	size_t i = 0;

	while (src[count])
		++count;

	types = calloc(count + 1, sizeof(char *));
	if (!types)
		goto err;

	for (i = 0; i < count; ++i) {
		types[i] = strdup(src[i]);
		if (!types[i])
			goto oom;
	}

	*out = types;
	return count;

oom:
	/* Free partial array. */
	while (i)
		free(types[--i]);
	free(types);
err:
	*out = NULL;
	return 0;
}

/* Public wrappers. */
size_t partition_list_disk_types(char ***out)
{
	return build_types_array(disk_types, out);
}

size_t partition_list_fs_types(char ***out)
{
	return build_types_array(fs_types, out);
}

void partition_free_types_array(char **array)
{
	if (!array) return;
	for (char **p = array; *p; ++p)
		free(*p);
	free(array);
}
