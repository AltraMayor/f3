#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

#include "version.h"
#include "libutils.h"

#define SCSI_TIMEOUT_MS		10000
#define SCSI_SENSE_SIZE		32
#define MAX_FLASH_IDS		8
#define MAX_FLASH_ID_SIZE	16

/* Argp's global variables. */
const char *argp_program_version = "F3 Chip " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<DISK_DEV>";

static char doc[] = "F3 Chip -- detect the controller and raw flash chip ID "
	"of a USB flash drive";

static struct argp_option options[] = {
	{"verbose",	'v',	NULL,	0,
		"Show failed controller probes",	0},
	{ 0 }
};

struct args {
	const char	*filename;
	bool		verbose;
};

struct scsi_dev {
	const char	*filename;
	int		fd;
	bool		verbose;
};

struct flash_id {
	unsigned int	channel;
	unsigned char	id[MAX_FLASH_ID_SIZE];
	size_t		len;
};

struct chip_result {
	const char	*controller;
	struct flash_id	flash_ids[MAX_FLASH_IDS];
	unsigned int	n_flash_ids;
};

struct chip_driver {
	const char	*name;
	int (*detect)(struct scsi_dev *dev, struct chip_result *result);
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;

	switch (key) {
	case 'v':
		args->verbose = true;
		break;

	case ARGP_KEY_INIT:
		args->filename = NULL;
		break;

	case ARGP_KEY_ARG:
		if (args->filename)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->filename = arg;
		break;

	case ARGP_KEY_END:
		if (!args->filename)
			argp_error(state,
				"The disk device was not specified");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

static bool is_repeated_byte(const unsigned char *buf, size_t len,
	unsigned char value)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] != value)
			return false;
	return true;
}

static bool flash_id_equal(const struct flash_id *fid,
	unsigned int channel, const unsigned char *id, size_t len)
{
	return fid->channel == channel && fid->len == len &&
		!memcmp(fid->id, id, len);
}

static void add_flash_id(struct chip_result *result,
	unsigned int channel, const unsigned char *id, size_t len)
{
	unsigned int i;

	assert(len <= MAX_FLASH_ID_SIZE);

	if (!len || is_repeated_byte(id, len, 0x00) ||
			is_repeated_byte(id, len, 0xff))
		return;

	for (i = 0; i < result->n_flash_ids; i++)
		if (flash_id_equal(&result->flash_ids[i], channel, id, len))
			return;

	if (result->n_flash_ids >= MAX_FLASH_IDS)
		return;

	result->flash_ids[result->n_flash_ids].channel = channel;
	result->flash_ids[result->n_flash_ids].len = len;
	memcpy(result->flash_ids[result->n_flash_ids].id, id, len);
	result->n_flash_ids++;
}

static int sg_io(struct scsi_dev *dev, const unsigned char *cdb,
	unsigned char cdb_len, unsigned char *buf, unsigned int buf_len,
	int dxfer_direction)
{
	unsigned char sense[SCSI_SENSE_SIZE];
	sg_io_hdr_t hdr;

	memset(&hdr, 0, sizeof(hdr));
	memset(sense, 0, sizeof(sense));
	hdr.interface_id = 'S';
	hdr.dxfer_direction = dxfer_direction;
	hdr.cmd_len = cdb_len;
	hdr.mx_sb_len = sizeof(sense);
	hdr.dxfer_len = buf_len;
	hdr.dxferp = buf;
	hdr.cmdp = (unsigned char *)cdb;
	hdr.sbp = sense;
	hdr.timeout = SCSI_TIMEOUT_MS;

	if (ioctl(dev->fd, SG_IO, &hdr) < 0)
		return - errno;

	if ((hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK)
		return - EIO;

	return 0;
}

static bool fatal_sg_error(int rc)
{
	return rc == -EACCES || rc == -EPERM || rc == -ENOTTY;
}

static const char *sg_error_help(int rc)
{
	switch (-rc) {
	case EACCES:
	case EPERM:
		return "permission denied; vendor-specific SG_IO commands "
			"may require root or CAP_SYS_RAWIO";
	case ENOTTY:
		return "the device does not support SG_IO pass-through";
	default:
		return strerror(-rc);
	}
}

static char *trim(char *str)
{
	size_t len = strlen(str);

	while (len > 0 && str[len - 1] == ' ')
		str[--len] = '\0';
	return str;
}

static int scsi_inquiry(struct scsi_dev *dev, char *str, size_t str_size)
{
	static const unsigned char cdb[6] = {
		0x12, 0x00, 0x00, 0x00, 0x24, 0x00
	};
	unsigned char buf[36];
	char vendor[9], product[17], revision[5];
	int rc;

	assert(str_size > 0);
	str[0] = '\0';

	memset(buf, 0, sizeof(buf));
	rc = sg_io(dev, cdb, sizeof(cdb), buf, sizeof(buf), SG_DXFER_FROM_DEV);
	if (rc)
		return rc;

	memcpy(vendor, buf + 8, 8);
	vendor[8] = '\0';
	memcpy(product, buf + 16, 16);
	product[16] = '\0';
	memcpy(revision, buf + 32, 4);
	revision[4] = '\0';

	snprintf(str, str_size, "%s %s %s",
		trim(vendor), trim(product), trim(revision));
	return 0;
}

static bool buf_contains(const unsigned char *buf, size_t buf_len,
	const char *needle)
{
	const size_t needle_len = strlen(needle);
	size_t i;

	if (needle_len > buf_len)
		return false;

	for (i = 0; i <= buf_len - needle_len; i++)
		if (!memcmp(buf + i, needle, needle_len))
			return true;
	return false;
}

static void print_hex_prefix(const unsigned char *buf, size_t buf_len,
	size_t max_len)
{
	size_t i, len = buf_len < max_len ? buf_len : max_len;

	for (i = 0; i < len; i++)
		fprintf(stderr, "%02X", buf[i]);
	if (len < buf_len)
		fprintf(stderr, "...");
	fprintf(stderr, "\n");
}

struct cbm2199_variant {
	const char	*needle;
	const char	*controller;
};

static const struct cbm2199_variant cbm2199_variants[] = {
	{"ChipsBank2199EB", "ChipsBank CBM2199EB"},	/* UNTESTED */
	{"ChipsBank2199ES", "ChipsBank CBM2199ES"},	/* UNTESTED */
	{"ChipsBank2199E", "ChipsBank CBM2199E"},
	{"ChipsBank2199SC", "ChipsBank CBM2199SC"},	/* UNTESTED */
	{"ChipsBank2199S", "ChipsBank CBM2199S"},
	{"ChipsBank2199C", "ChipsBank CBM2199C"},	/* UNTESTED */
	{"ChipsBank2199", "ChipsBank CBM2199"},		/* UNTESTED */
};

static int cbm2199_read_info(struct scsi_dev *dev, unsigned char *buf,
	size_t buf_len)
{
	static const unsigned char cdb[16] = {
		0xea, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe3
	};

	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len, SG_DXFER_FROM_DEV);
}

static int cbm2199_read_flash_blob(struct scsi_dev *dev, unsigned char *buf,
	size_t buf_len)
{
	static const unsigned char cdbs[][16] = {
		{
			0xea, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6
		},
		{
			0xea, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6
		},
	};
	unsigned int i;

	for (i = 0; i < DIM(cdbs); i++) {
		int rc;

		memset(buf, 0, buf_len);
		rc = sg_io(dev, cdbs[i], sizeof(cdbs[i]), buf, buf_len,
			SG_DXFER_FROM_DEV);
		if (!rc)
			return 0;
		if (dev->verbose)
			warnx("ChipsBank flash-info command %u failed: %s",
				i + 1, strerror(-rc));
	}
	return - EIO;
}

static int detect_cbm2199(struct scsi_dev *dev, struct chip_result *result)
{
	enum {
		info_len = 0x600,
		flash_blob_len = 0x800,
		flash_id_offset = 0x684,
		flash_id_len = 6,
	};
	unsigned char info[info_len];
	unsigned char flash_blob[flash_blob_len];
	const struct cbm2199_variant *variant = NULL;
	unsigned int i;
	int rc;

	memset(info, 0, sizeof(info));
	rc = cbm2199_read_info(dev, info, sizeof(info));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("ChipsBank controller-info command failed: %s",
				strerror(-rc));
		return 0;
	}

	for (i = 0; i < DIM(cbm2199_variants); i++) {
		if (buf_contains(info, sizeof(info),
				cbm2199_variants[i].needle)) {
			variant = &cbm2199_variants[i];
			break;
		}
	}
	if (!variant)
		return 0;

	result->controller = variant->controller;

	rc = cbm2199_read_flash_blob(dev, flash_blob, sizeof(flash_blob));
	if (rc)
		return 1;

	add_flash_id(result, 0, flash_blob + flash_id_offset, flash_id_len);
	return 1;
}

static bool fc2279_has_flash_info(const unsigned char *buf, size_t buf_len,
	size_t id_offset, size_t id_len)
{
	if (id_offset + id_len > buf_len)
		return false;

	return buf[8] == 0x01 && buf[9] == 0x01 && buf[13] == 0xa2 &&
		!is_repeated_byte(buf + id_offset, id_len, 0x00) &&
		!is_repeated_byte(buf + id_offset, id_len, 0xff);
}

static int fc2279_read_flash_info(struct scsi_dev *dev, unsigned char *buf,
	size_t buf_len)
{
	static const unsigned char cdbs[][16] = {
		{
			0xf1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1
		},
		{
			0xf1, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1
		},
	};
	unsigned int i;

	for (i = 0; i < DIM(cdbs); i++) {
		int rc;

		memset(buf, 0, buf_len);
		rc = sg_io(dev, cdbs[i], sizeof(cdbs[i]), buf, buf_len,
			SG_DXFER_FROM_DEV);
		if (!rc)
			return 0;
		if (dev->verbose)
			warnx("FirstChip flash-info command %u failed: %s",
				i + 1, strerror(-rc));
	}
	return - EIO;
}

static int fc2279_read_config(struct scsi_dev *dev, unsigned char *buf,
	size_t buf_len)
{
	static const unsigned char cdb[16] = {
		0xf1, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa
	};

	memset(buf, 0, buf_len);
	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len,
		SG_DXFER_FROM_DEV);
}

static int detect_fc2279(struct scsi_dev *dev, struct chip_result *result)
{
	enum {
		flash_info_len = 0x800,
		config_len = 0x4000,
		flash_id_offset = 0x10,
		config_flash_id_offset = 0x12d,
		flash_id_len = 6,
	};
	unsigned char flash_info[flash_info_len];
	unsigned char config[config_len];
	int rc;

	rc = fc2279_read_flash_info(dev, flash_info, sizeof(flash_info));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("FirstChip flash-info probe failed: %s",
				strerror(-rc));
	} else if (fc2279_has_flash_info(flash_info, sizeof(flash_info),
			flash_id_offset, flash_id_len)) {
		result->controller = "FirstChip FC2279";
		add_flash_id(result, 0, flash_info + flash_id_offset,
			flash_id_len);
		return 1;
	} else if (dev->verbose) {
		warnx("FirstChip flash-info response was not recognized");
		fprintf(stderr, "FirstChip flash-info prefix: ");
		print_hex_prefix(flash_info, sizeof(flash_info), 32);
	}

	rc = fc2279_read_config(dev, config, sizeof(config));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("FirstChip config probe failed: %s",
				strerror(-rc));
		return 0;
	}

	result->controller = "FirstChip FC2279";
	add_flash_id(result, 0, config + config_flash_id_offset,
		flash_id_len);

	if (result->n_flash_ids)
		return 1;

	if (dev->verbose) {
		warnx("FirstChip config response did not contain a flash ID");
		fprintf(stderr, "FirstChip config prefix: ");
		print_hex_prefix(config, sizeof(config), 32);
	}
	return 0;
}

static const struct chip_driver chip_drivers[] = {
	{"ChipsBank CBM2199", detect_cbm2199},
	{"FirstChip FC2279", detect_fc2279},
};

static int detect_chip(struct scsi_dev *dev, struct chip_result *result)
{
	unsigned int i;

	memset(result, 0, sizeof(*result));
	for (i = 0; i < DIM(chip_drivers); i++) {
		int rc = chip_drivers[i].detect(dev, result);

		if (rc < 0)
			return rc;
		if (rc > 0)
			return 0;
	}

	return - ENODEV;
}

static void print_hex_id(const struct flash_id *fid)
{
	size_t i;

	for (i = 0; i < fid->len; i++)
		printf("%02X", fid->id[i]);
	printf("\n");
}

static void print_results(const struct args *args,
	const char *inquiry, const struct chip_result *result)
{
	unsigned int i;

	print_header(stdout, "Chip");
	printf("Device: %s\n", args->filename);
	printf("SCSI inquiry: %s\n", inquiry[0] ? inquiry : "unavailable");
	printf("Controller: %s\n",
		result->controller ? result->controller : "unknown");

	if (!result->n_flash_ids) {
		printf("Flash IDs: none\n");
		return;
	}

	printf("Flash IDs:\n");
	for (i = 0; i < result->n_flash_ids; i++) {
		printf("  Channel %u: ", result->flash_ids[i].channel);
		print_hex_id(&result->flash_ids[i]);
	}
}

int main(int argc, char **argv)
{
	struct args args = {
		.filename = NULL,
		.verbose = false,
	};
	struct scsi_dev dev;
	struct chip_result result;
	struct stat dev_stat;
	char inquiry[64];
	int rc;

	argp_parse(&argp, argc, argv, 0, NULL, &args);

	dev.filename = args.filename;
	dev.verbose = args.verbose;
	dev.fd = open(args.filename, O_RDONLY);
	if (dev.fd < 0)
		err(errno, "Can't open device `%s'", args.filename);
	if (fstat(dev.fd, &dev_stat))
		err(errno, "Can't stat device `%s'", args.filename);
	if (!S_ISBLK(dev_stat.st_mode))
		errx(1, "`%s' is not a block device", args.filename);

	rc = scsi_inquiry(&dev, inquiry, sizeof(inquiry));
	if (fatal_sg_error(rc))
		errx(1, "Can't send SCSI commands to `%s': %s",
			args.filename, sg_error_help(rc));
	else if (rc)
		inquiry[0] = '\0';

	rc = detect_chip(&dev, &result);
	if (rc == -ENODEV)
		memset(&result, 0, sizeof(result));
	else if (rc)
		errx(1, "Can't detect chip on `%s': %s",
			args.filename, sg_error_help(rc));

	print_results(&args, inquiry, &result);

	assert(!close(dev.fd));
	return rc ? 1 : 0;
}
