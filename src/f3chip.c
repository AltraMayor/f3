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
#define F3_ISSUE_URL		"https://github.com/AltraMayor/f3/issues"

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
	char		controller_name[64];
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

static bool flash_id_has_padding_tail(const unsigned char *id, size_t len)
{
	return len > 1 &&
		(is_repeated_byte(id + 1, len - 1, 0x00) ||
			is_repeated_byte(id + 1, len - 1, 0xff));
}

static bool flash_id_valid(const unsigned char *id, size_t len)
{
	/*
	 * Some controller tables use slots whose first byte is status-like
	 * data and whose remaining bytes are padding.  Those are not useful
	 * NAND IDs.
	 */
	return len && !is_repeated_byte(id, len, 0x00) &&
		!is_repeated_byte(id, len, 0xff) &&
		!flash_id_has_padding_tail(id, len);
}

static void set_controller(struct chip_result *result, const char *controller)
{
	snprintf(result->controller_name, sizeof(result->controller_name),
		"%s", controller);
	result->controller = result->controller_name;
}

static void add_flash_id(struct chip_result *result,
	unsigned int channel, const unsigned char *id, size_t len)
{
	unsigned int i;

	assert(len <= MAX_FLASH_ID_SIZE);

	if (!flash_id_valid(id, len))
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

static bool is_printable_ascii(unsigned char ch)
{
	return ch >= 0x20 && ch <= 0x7e;
}

static bool find_printable_token(const unsigned char *buf, size_t buf_len,
	const char *anchor, char *str, size_t str_size)
{
	const size_t anchor_len = strlen(anchor);
	size_t i, len;

	assert(str_size > 0);
	str[0] = '\0';

	if (anchor_len > buf_len)
		return false;

	for (i = 0; i <= buf_len - anchor_len; i++) {
		if (memcmp(buf + i, anchor, anchor_len))
			continue;

		for (len = 0; i + len < buf_len && len + 1 < str_size &&
				is_printable_ascii(buf[i + len]); len++)
			str[len] = buf[i + len];
		str[len] = '\0';
		return len > 0;
	}
	return false;
}

static void format_hex_prefix(char *str, size_t str_size,
	const unsigned char *buf, size_t buf_len, size_t max_len)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i, len = buf_len < max_len ? buf_len : max_len;
	size_t pos = 0;

	assert(str_size > 0);
	str[0] = '\0';

	if (str_size > 1)
		str[pos++] = '0';
	if (str_size > 2)
		str[pos++] = 'x';

	for (i = 0; i < len && pos + 2 < str_size; i++) {
		str[pos++] = hex[buf[i] >> 4];
		str[pos++] = hex[buf[i] & 0x0f];
	}
	if (len < buf_len && pos + 3 < str_size) {
		str[pos++] = '.';
		str[pos++] = '.';
		str[pos++] = '.';
	}
	str[pos < str_size ? pos : str_size - 1] = '\0';
}

static void response_variant_id(char *str, size_t str_size,
	const unsigned char *buf, size_t buf_len, const char *anchor)
{
	if (find_printable_token(buf, buf_len, anchor, str, str_size))
		return;

	format_hex_prefix(str, str_size, buf, buf_len, 16);
}

static void warn_unknown_variant(const char *driver, const char *variant_id)
{
	warnx("Unknown %s variant %s! Please report this to " F3_ISSUE_URL,
		driver, variant_id);
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
	if (!variant) {
		char variant_id[64];

		response_variant_id(variant_id, sizeof(variant_id), info,
			sizeof(info), "ChipsBank");
		warn_unknown_variant("ChipsBank CBM2199", variant_id);
		return 0;
	}

	set_controller(result, variant->controller);

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

	return buf[8] == 0x01 && buf[9] == 0x01 &&
		flash_id_valid(buf + id_offset, id_len);
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
		set_controller(result, "FirstChip FC2279");
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

	set_controller(result, "FirstChip FC2279");
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

struct fc3379_variant {
	const char	*needle;
	const char	*controller;
};

static const struct fc3379_variant fc3379_variants[] = {
	{"FC3281C", "FirstChip FC3379"},
	{"FC3379", "FirstChip FC3379"},		/* UNTESTED */
};

static int fc3379_read_controller_info(struct scsi_dev *dev,
	unsigned char *buf, size_t buf_len)
{
	static const unsigned char cdb[16] = {
		0xf1, 0x00, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0
	};

	memset(buf, 0, buf_len);
	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len,
		SG_DXFER_FROM_DEV);
}

static int fc3379_read_flash_id_table(struct scsi_dev *dev,
	unsigned char *buf, size_t buf_len)
{
	static const unsigned char cdbs[][16] = {
		{
			0xf1, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1
		},
		{
			0xf1, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
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
			warnx("FC3379 flash-ID table command %u failed: %s",
				i + 1, strerror(-rc));
	}
	return - EIO;
}

static void fc3379_add_flash_id_table(struct chip_result *result,
	const unsigned char *buf, size_t buf_len)
{
	enum {
		id_count_offset = 8,
		id_table_offset = 0x10,
		id_entry_len = 0x10,
		id_len = 6,
		max_ids = 8,
	};
	unsigned int i, id_count;

	if (buf_len < id_table_offset + id_len ||
			buf[id_count_offset] > max_ids)
		return;

	id_count = buf[id_count_offset];
	for (i = 0; i < id_count && i < max_ids; i++) {
		const unsigned char *id = buf + id_table_offset +
			i * id_entry_len;

		if (id + id_entry_len > buf + buf_len)
			break;
		add_flash_id(result, i, id, id_len);
	}
}

static int detect_fc3379(struct scsi_dev *dev,
	struct chip_result *result)
{
	enum {
		controller_info_len = 0x200,
		flash_id_table_len = 0x100,
	};
	unsigned char info[controller_info_len];
	unsigned char flash_ids[flash_id_table_len];
	unsigned int i;
	int rc;

	rc = fc3379_read_controller_info(dev, info, sizeof(info));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("FC3379 controller-info probe failed: %s",
				strerror(-rc));
		return 0;
	}

	for (i = 0; i < DIM(fc3379_variants); i++) {
		if (buf_contains(info, sizeof(info),
			fc3379_variants[i].needle)) {
			set_controller(result, fc3379_variants[i].controller);
			rc = fc3379_read_flash_id_table(dev, flash_ids,
				sizeof(flash_ids));
			if (!rc)
				fc3379_add_flash_id_table(result, flash_ids,
					sizeof(flash_ids));
			else if (dev->verbose)
				warnx("FC3379 flash-ID table probe failed: %s",
					strerror(-rc));
			return 1;
		}
	}

	{
		char variant_id[64];

		response_variant_id(variant_id, sizeof(variant_id), info,
			sizeof(info), "FC");
		warn_unknown_variant("FirstChip FC3379", variant_id);
	}
	if (dev->verbose) {
		warnx("FC3379 controller-info response was not recognized");
		fprintf(stderr, "FC3379 controller-info prefix: ");
		print_hex_prefix(info, sizeof(info), 32);
	}
	return 0;
}

static void alcor_add_flash_id_table(struct chip_result *result,
	const unsigned char *buf, size_t buf_len)
{
	enum {
		id_table_offset = 0x00,
		id_entry_len = 0x10,
		id_len = 6,
		max_ids = 8,
	};
	unsigned int i;

	if (buf_len < id_table_offset + id_len)
		return;

	for (i = 0; i < max_ids; i++) {
		const unsigned char *id = buf + id_table_offset +
			i * id_entry_len;

		if (id + id_len > buf + buf_len)
			break;
		add_flash_id(result, i, id, id_len);
	}
}

static int alcor_read_au8910x_flash_id_table(struct scsi_dev *dev,
	unsigned char *buf, size_t buf_len)
{
	static const unsigned char cdb[10] = {
		0xfa, 0x17, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
		0x01, 0x00
	};

	memset(buf, 0, buf_len);
	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len,
		SG_DXFER_FROM_DEV);
}

static int alcor_read_au6989_flash_id_table(struct scsi_dev *dev,
	unsigned char *buf, size_t buf_len)
{
	static const unsigned char cdb[8] = {
		0xfa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	memset(buf, 0, buf_len);
	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len,
		SG_DXFER_FROM_DEV);
}

static int alcor_read_au6989_info(struct scsi_dev *dev,
	unsigned char *buf, size_t buf_len)
{
	static const unsigned char cdb[8] = {
		0xfa, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	memset(buf, 0, buf_len);
	return sg_io(dev, cdb, sizeof(cdb), buf, buf_len,
		SG_DXFER_FROM_DEV);
}

static bool alcor_au6989_model_id(const unsigned char *info,
	size_t info_len, unsigned char *model_id)
{
	enum {
		model_offset = 0x14,
	};

	if (info_len <= model_offset)
		return false;

	*model_id = info[model_offset];
	return true;
}

static const char *alcor_au6989_controller(unsigned char model_id)
{
	switch (model_id) {
	case 0x11:
		return "Alcor AU6989SN";
	case 0x22:
		return "Alcor AU6989SN-GTC";
	default:
		return NULL;
	}
}

static int detect_au8910x(struct scsi_dev *dev, struct chip_result *result)
{
	enum {
		flash_id_table_len = 0x200,
	};
	unsigned char flash_ids[flash_id_table_len];
	int rc;

	rc = alcor_read_au8910x_flash_id_table(dev, flash_ids,
		sizeof(flash_ids));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("AU8910x flash-ID table probe failed: %s",
				strerror(-rc));
		return 0;
	}

	alcor_add_flash_id_table(result, flash_ids, sizeof(flash_ids));
	if (!result->n_flash_ids) {
		if (dev->verbose) {
			warnx("AU8910x flash-ID table did not contain a flash ID");
			fprintf(stderr, "AU8910x flash-ID table prefix: ");
			print_hex_prefix(flash_ids, sizeof(flash_ids), 32);
		}
		return 0;
	}

	set_controller(result, "Alcor AU89103");
	return 1;
}

static int detect_au6989(struct scsi_dev *dev, struct chip_result *result)
{
	enum {
		info_len = 0x200,
		flash_id_table_len = 0x200,
	};
	unsigned char info[info_len];
	unsigned char flash_ids[flash_id_table_len];
	const char *controller = "Alcor AU6989";
	unsigned char model_id;
	int rc;

	rc = alcor_read_au6989_info(dev, info, sizeof(info));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("AU6989 info probe failed: %s", strerror(-rc));
	} else if (alcor_au6989_model_id(info, sizeof(info), &model_id)) {
		controller = alcor_au6989_controller(model_id);
		if (!controller) {
			char variant_id[8];

			snprintf(variant_id, sizeof(variant_id), "0x%02X",
				model_id);
			warn_unknown_variant("Alcor AU6989", variant_id);
			controller = "Alcor AU6989";
		}
	}

	rc = alcor_read_au6989_flash_id_table(dev, flash_ids,
		sizeof(flash_ids));
	if (rc) {
		if (fatal_sg_error(rc))
			return rc;
		if (dev->verbose)
			warnx("AU6989 flash-ID table probe failed: %s",
				strerror(-rc));
		return 0;
	}

	alcor_add_flash_id_table(result, flash_ids, sizeof(flash_ids));
	if (!result->n_flash_ids) {
		if (dev->verbose) {
			warnx("AU6989 flash-ID table did not contain a flash ID");
			fprintf(stderr, "AU6989 flash-ID table prefix: ");
			print_hex_prefix(flash_ids, sizeof(flash_ids), 32);
		}
		return 0;
	}

	set_controller(result, controller);
	return 1;
}

static const struct chip_driver chip_drivers[] = {
	{"ChipsBank CBM2199", detect_cbm2199},
	{"FirstChip FC3379", detect_fc3379},
	{"FirstChip FC2279", detect_fc2279},
	{"Alcor AU8910x", detect_au8910x},
	{"Alcor AU6989", detect_au6989},
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
