#include <stdlib.h>

#include "libprobe.h"

static const char const *ftype_to_name[] = {"good", "limbo", "wraparound"};

const char *fake_type_to_name(enum fake_type fake_type)
{
	return ftype_to_name[fake_type];
}

struct device {
	int dummy;
};

struct device *create_file_device(const char *filename,
	int file_size_gb, int fake_size_gb, enum fake_type fake_type)
{
	/* TODO */
	return NULL;
}

/* XXX Test if it's a device, or a partition.
 * If a partition, warn user, and ask for confirmation before
 * going ahead.
 * Suggest how to call f3probe with the correct device name if
 * the block device is a partition.
 */
/* XXX Test for write access of the block device to give
 * a nice error message.
 * If it fails, suggest running f3probe as root.
 */
struct device *create_block_device(const char *filename)
{
	/* TODO */
	return NULL;
}

void free_device(struct device *dev)
{
	/* TODO */
}

/* XXX Don't write at the very beginning of the card to avoid
 * losing the partition table.
 * But write at a random locations to make harder for fake chips
 * to become "smarter".
 */
/* XXX Write random data for testing.
 * There would be a random seed, and all the other blocks would be
 * this seed XOR'd with the number of the test.
 */
/* XXX Finish testing the last block, and the next one that should fail.
 * Then report the last block, so user can create the largest partition.
 */
enum fake_type test_device(struct device *dev, int *preal_size_gb)
{
	/* TODO */
	*preal_size_gb = 0;
	return FKTY_GOOD;
}
