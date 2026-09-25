/*
 * f3fix for macOS — create a partition table sized to a drive's real capacity.
 *
 * The Linux f3fix uses libparted, which is not available on macOS.
 * Fixing a fake drive only ever needs a single MBR partition whose last
 * sector is the last *good* sector reported by f3probe, so this
 * implementation writes the MBR directly and needs no library at all.
 *
 * Usage mirrors the Linux tool:
 *   sudo f3fix --last-sec=SEC /dev/rdiskN
 *
 * Copyright (C) 2010 Digirati Internet LTDA.
 * This file is part of F3. F3 is free software: GPLv3 or later.
 */

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <argp.h>

#include "version.h"
#include "libutils.h"

/* MBR partition type bytes for the filesystems f3fix supports. */
struct fs_type_entry {
	const char *name;
	uint8_t mbr_type;
};

static const struct fs_type_entry fs_types[] = {
	{"fat32", 0x0C},	/* FAT32 LBA */
	{"fat16", 0x0E},	/* FAT16 LBA */
	{"exfat", 0x07},
	{"ntfs",  0x07},
	{NULL, 0}
};

static const char doc[] = "F3 Fix -- edit the partition table of "
	"a fake flash drive to have a single partition that fully covers "
	"the real capacity of the drive (macOS backend, MBR only)";

static struct argp_option options[] = {
	{"disk-type",	'd',	"TYPE",	0,
		"Disk type of the partition table; only `msdos' (MBR) is supported on macOS", 2},
	{"fs-type",	'f',	"TYPE",	0,
		"Type of the file system of the partition: fat32, fat16, exfat, ntfs", 0},
	{"boot",	'b',	NULL,	0, "Mark the partition for boot", 0},
	{"no-boot",	'n',	NULL,	0, "Do not mark the partition for boot", 0},
	{"first-sec",	'a',	"SEC",	0, "Sector where the partition starts", 0},
	{"last-sec",	'l',	"SEC",	0, "Sector where the partition ends", 0},
	{ 0 }
};

struct args {
	const char	*dev_filename;
	uint8_t		fs_mbr_type;
	bool		boot;
	uint64_t	first_sec;
	uint64_t	last_sec;
	bool		last_sec_set;
};

static uint8_t lookup_fs_type(const char *name)
{
	const struct fs_type_entry *e;
	for (e = fs_types; e->name; e++)
		if (!strcasecmp(e->name, name))
			return e->mbr_type;
	return 0;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		if (strcasecmp(arg, "msdos"))
			argp_error(state,
				"only `msdos' (MBR) partition tables are supported on macOS; GPT is unnecessary for fixing fake drives");
		break;
	case 'f':
		args->fs_mbr_type = lookup_fs_type(arg);
		if (!args->fs_mbr_type)
			argp_error(state,
				"file system type `%s' is not supported; use fat32, fat16, exfat, or ntfs", arg);
		break;
	case 'b':
		args->boot = true;
		break;
	case 'n':
		args->boot = false;
		break;
	case 'a':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state, "the first sector must be at least 0");
		args->first_sec = ll;
		break;
	case 'l':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state, "the last sector must be at least 0");
		args->last_sec = ll;
		args->last_sec_set = true;
		break;
	case ARGP_KEY_ARG:
		if (args->dev_filename)
			argp_error(state, "too many arguments; only one device is expected");
		args->dev_filename = arg;
		break;
	case ARGP_KEY_END:
		if (!args->dev_filename)
			argp_error(state, "the disk device was not specified");
		if (!args->last_sec_set)
			argp_error(state, "option --last-sec is required");
		if (args->first_sec > args->last_sec)
			argp_error(state, "the first sector (%" PRIu64 ") must be less than or equal to the last sector (%" PRIu64 ")",
				args->first_sec, args->last_sec);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, "<DISK_DEV>", doc, NULL, NULL, NULL};

static void write_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

static int fix_disk(const char *filename, uint8_t fs_type, bool boot,
	uint64_t first_sec, uint64_t last_sec)
{
	uint8_t mbr[512];
	uint8_t zero_lba1[512];
	uint8_t *entry = &mbr[446];
	uint64_t sectors = last_sec - first_sec + 1;
	uint64_t dev_block_count = 0;
	uint32_t dev_block_size = 0;
	int fd;
	ssize_t written;

	if (first_sec > UINT32_MAX || sectors > UINT32_MAX) {
		warnx("partition does not fit in an MBR (first sector %" PRIu64 ", %" PRIu64 " sectors)",
			first_sec, sectors);
		return 1;
	}

	memset(mbr, 0, sizeof(mbr));

	entry[0] = boot ? 0x80 : 0x00;
	/* CHS fields set to the conventional LBA placeholders. */
	entry[1] = 0xFE; entry[2] = 0xFF; entry[3] = 0xFF;
	entry[4] = fs_type;
	entry[5] = 0xFE; entry[6] = 0xFF; entry[7] = 0xFF;
	write_le32(&entry[8], (uint32_t)first_sec);
	write_le32(&entry[12], (uint32_t)sectors);

	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	fd = open(filename, O_RDWR);
	if (fd < 0) {
		if (errno == EBUSY)
			warnx("device `%s' is busy; unmount it first:\n"
				"diskutil unmountDisk %s", filename, filename);
		else if (errno == EACCES && getuid())
			warnx("your user doesn't have access to device `%s'; run with sudo",
				filename);
		else
			warn("can't open device `%s'", filename);
		return 1;
	}

	/* Refuse to create a partition that runs past the device.
	 * (The Linux backend gets this check from libparted.)
	 */
	if (!ioctl(fd, DKIOCGETBLOCKCOUNT, &dev_block_count) &&
		!ioctl(fd, DKIOCGETBLOCKSIZE, &dev_block_size) &&
		dev_block_size == 512 && last_sec >= dev_block_count) {
		warnx("the last sector (%" PRIu64 ") is beyond the end of device `%s' (%" PRIu64 " sectors)",
			last_sec, filename, dev_block_count);
		close(fd);
		return 1;
	}

	written = pwrite(fd, mbr, sizeof(mbr), 0);
	if (written != (ssize_t)sizeof(mbr)) {
		warn("can't write the partition table to `%s'", filename);
		close(fd);
		return 1;
	}

	/* Zero LBA 1 so a leftover GPT header does not shadow the new MBR;
	 * tools that support GPT prefer it over the MBR when both look valid.
	 */
	memset(zero_lba1, 0, sizeof(zero_lba1));
	written = pwrite(fd, zero_lba1, sizeof(zero_lba1), 512);
	if (written != (ssize_t)sizeof(zero_lba1)) {
		warn("can't clear the old GPT header on `%s'", filename);
		close(fd);
		return 1;
	}
	if (fsync(fd)) {
		warn("can't flush the partition table to `%s'", filename);
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	struct args args = {
		.dev_filename	= NULL,
		.fs_mbr_type	= 0x0C,	/* fat32 */
		.boot		= true,
		.first_sec	= 2048,	/* 1MB-aligned */
		.last_sec	= 0,
		.last_sec_set	= false,
	};
	int rc;

	argp_parse(&argp, argc, argv, 0, NULL, &args);

	rc = fix_disk(args.dev_filename, args.fs_mbr_type, args.boot,
		args.first_sec, args.last_sec);

	if (!rc) {
		printf("Drive `%s' was successfully fixed\n\n"
			"Now run on macOS:\n"
			"diskutil eraseDisk ExFAT NAME %s\n"
			"or remove and reinsert the drive and format it "
			"with Disk Utility.\n",
			args.dev_filename, args.dev_filename);
	}
	return rc;
}
