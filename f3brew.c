#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <err.h>

#include "libutils.h"
#include "libdevs.h"
#include "utils.h"

/* XXX Add parameters. */

/* XXX Avoid code duplication. This function is copied from f3write.c. */
static uint64_t fill_buffer(void *buf, size_t size, uint64_t offset)
{
	uint8_t *p, *ptr_next_sector, *ptr_end;
	uint64_t rn;

	assert(size > 0);
	assert(size % SECTOR_SIZE == 0);
	assert(SECTOR_SIZE >= sizeof(offset) + sizeof(rn));
	assert((SECTOR_SIZE - sizeof(offset)) % sizeof(rn) == 0);

	p = buf;
	ptr_end = p + size;
	while (p < ptr_end) {
		rn = offset;
		memmove(p, &offset, sizeof(offset));
		ptr_next_sector = p + SECTOR_SIZE;
		p += sizeof(offset);
		for (; p < ptr_next_sector; p += sizeof(rn)) {
			rn = random_number(rn);
			memmove(p, &rn, sizeof(rn));
		}
		assert(p == ptr_next_sector);
		offset += SECTOR_SIZE;
	}

	return offset;
}

static void write_blocks(char *stamp_blk, struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t sector_offset = first_block << dev_get_block_order(dev);
	uint64_t i;

	for (i = first_block; i <= last_block; i++) {
		sector_offset =
			fill_buffer(stamp_blk, block_size, sector_offset);
		if (dev_write_block(dev, stamp_blk, i))
			warn("Failed writing block 0x%" PRIx64, i);
	}
}

/* XXX Avoid code duplication. Some code of this function is copied from
 * f3read.c.
 */
/* XXX Group the results so it is not too verbose.
 * For now, the less important reports are commented.
 */
#define TOLERANCE	2
static void validate_sector(uint64_t expected_sector_offset,
	const char *sector)
{
	uint64_t sector_offset, rn;
	const char *p, *ptr_end;
	int error_count;

	sector_offset = *((__typeof__(sector_offset) *) sector);
	rn = sector_offset;
	p = sector + sizeof(sector_offset);
	ptr_end = sector + SECTOR_SIZE;
	error_count = 0;
	for (; error_count <= TOLERANCE && p < ptr_end; p += sizeof(rn)) {
		rn = random_number(rn);
		if (rn != *((__typeof__(rn) *) p))
			error_count++;
	}

	if (sector_offset == expected_sector_offset) {
		if (error_count == 0)
			/*printf("GOOD sector 0x%" PRIx64 "\n",
				expected_sector_offset)*/;
		else if (error_count <= TOLERANCE)
			printf("Changed sector 0x%" PRIx64 "\n",
				expected_sector_offset);
		else
			printf("BAD matching sector 0x%" PRIx64 "\n",
				expected_sector_offset);
	} else if (error_count == 0) {
		printf("Overwritten sector 0x%" PRIx64
			", found 0x%" PRIx64 "\n",
			expected_sector_offset, sector_offset);
	} else if (error_count <= TOLERANCE) {
		printf("Overwritten and changed sector 0x%" PRIx64
			", found 0x%" PRIx64 "\n",
			expected_sector_offset, sector_offset);
	} else {
		/*printf("BAD sector 0x%" PRIx64 "\n", expected_sector_offset)*/;
	}
}

static void validate_block(uint64_t expected_sector_offset,
	const char *probe_blk, int block_size)
{
	const char *sector = probe_blk;
	const char *stop_sector = sector + block_size;

	assert(block_size % SECTOR_SIZE == 0);

	while (sector < stop_sector) {
		validate_sector(expected_sector_offset, sector);
		expected_sector_offset += SECTOR_SIZE;
		sector += SECTOR_SIZE;
	}
}

static void read_blocks(char *probe_blk, struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t expected_sector_offset =
		first_block << dev_get_block_order(dev);
	uint64_t i;

	for (i = first_block; i <= last_block; i++) {
		if (!dev_read_block(dev, probe_blk, i))
			validate_block(expected_sector_offset, probe_blk,
				block_size);
		else
			warn("Failed reading block 0x%" PRIx64, i);
		expected_sector_offset += block_size;
	}
}

/* XXX Properly handle return errors. */
static void write_and_read_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + block_size];
	char *blk = align_mem(stack, block_order);

	printf("Writing blocks from 0x%" PRIx64 " to 0x%" PRIx64 "...",
		first_block, last_block);
	fflush(stdout);
	write_blocks(blk, dev, first_block, last_block);
	printf(" Done\n\n");

	assert(!dev_reset(dev));

	printf("Reading those blocks...");
	fflush(stdout);
	read_blocks(blk, dev, first_block, last_block);
	printf(" Done\n\n");
}

int main(void)
{
	struct device *dev;
	uint64_t first_block = 20;
	uint64_t last_block  = 1ULL << 18;

	dev = false
		? create_file_device("xuxu",
			1ULL << 21,	/* Real size.	*/
			1ULL << 30,	/* Fake size.	*/
			22,		/* Wrap.	*/
			9,		/* Block order. */
			false)		/* Keep file?	*/
		: create_block_device("/dev/sdc", RT_MANUAL_USB);
	assert(dev);

	write_and_read_blocks(dev, first_block, last_block);

	free_device(dev);
	return 0;
}
