#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <errno.h>

#include "libutils.h"
#include "libprobe.h"

static inline int equal_blk(struct device *dev, const char *b1, const char *b2)
{
	return !memcmp(b1, b2, dev_get_block_size(dev));
}

/* Return true if @b1 and b2 are at most @tolerance_byte bytes different. */
static int similar_blk(struct device *dev, const char *b1, const char *b2,
	int tolerance_byte)
{
	const int block_size = dev_get_block_size(dev);
	int i;

	for (i = 0; i < block_size; i++) {
		if (*b1 != *b2) {
			tolerance_byte--;
			if (tolerance_byte <= 0)
				return false;
		}
		b1++;
		b2++;
	}
	return true;
}

/* Return true if the block at @pos is damaged. */
static int test_block(struct device *dev,
	const char *stamp_blk, char *probe_blk, uint64_t pos)
{
	/* Write block. */
	if (dev_write_block(dev, stamp_blk, pos) &&
		dev_write_block(dev, stamp_blk, pos))
		return true;

	/* Reset. */
	if (dev_reset(dev) && dev_reset(dev))
		return true;

	/*
	 *	Test block.
	 */

	if (dev_read_block(dev, probe_blk, pos) &&
		dev_read_block(dev, probe_blk, pos))
		return true;

	if (equal_blk(dev, stamp_blk, probe_blk))
		return false;

	/* Save time with certainly damaged blocks. */
	if (!similar_blk(dev, stamp_blk, probe_blk, 8)) {
		/* The probe block is damaged. */
		return true;
	}

	/* The probe block seems to be damaged.
	 * Trying a second time...
	 */
	return 	dev_write_and_reset(dev, stamp_blk, pos) ||
		dev_read_block(dev, probe_blk, pos)  ||
		!equal_blk(dev, stamp_blk, probe_blk);
}

/* Minimum size of the memory chunk used to build flash drives.
 * It must be a power of two.
 */
static inline uint64_t initial_high_bit_block(struct device *dev)
{
	int block_order = dev_get_block_order(dev);
	assert(block_order <= 20);
	return 1ULL << (20 - block_order);
}

/* Caller must guarantee that the left bock is good, and written. */
static int search_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t high_bit = initial_high_bit_block(dev);
	uint64_t pos = high_bit + left_pos;

	/* The left block must be in the first memory chunk. */
	assert(left_pos < high_bit);

	/* Check that the drive has at least one memory chunk. */
	assert((high_bit - 1) <= *pright_pos);

	while (pos < *pright_pos) {
		if (dev_read_block(dev, probe_blk, pos) &&
			dev_read_block(dev, probe_blk, pos))
			return true;
		/* XXX Deal with flipped bit on reception. */
		if (equal_blk(dev, stamp_blk, probe_blk)) {
			/* XXX Test wraparound hypothesis. */
			*pright_pos = high_bit - 1;
			return false;
		}
		high_bit <<= 1;
		pos = high_bit + left_pos;
	}

	return false;
}

#define MAX_N_BLOCK_ORDER	10

static uint64_t estimate_best_n_block(struct device *dev)
{
	uint64_t write_count, write_time_us;
	uint64_t reset_count, reset_time_us;
	double t_w_us, t_2w_us, t_r_us;
	uint64_t n_block_order;

	perf_device_sample(dev, NULL, NULL, &write_count, &write_time_us,
		&reset_count, &reset_time_us);
	if (write_count < 3 || reset_count < 2) {
		/* There is not enough measurements. */
		return (1 << 2) - 1;
	}

	/* Let 2^n be the total number of blocks on the drive.
	 * Let p be the total number of passes.
	 * Let w = (2^m - 1) be the number of blocks written on each pass,
	 *   where m >= 1.
	 *
	 * A pass is an iteration of the loop in search_edge(), that is,
	 * a call to write_test_blocks(), dev_reset(), and probe_test_blocks().
	 *
	 * The reason to have w = (2^m - 1) instead of w = 2^m is because
	 * the former leads to a clean relationship between n, p, and m
	 * when m is constant: 2^n / (w + 1)^p = 1 => p = n/m
	 *
	 * Let Tr be the time to reset the device.
	 * Let Tw be the time to write a block to @dev.
	 * Let Tw' be the time to write a block to the underlying device
	 *   of @dev, that is, without overhead due to chaining multiple
	 *   struct device. For example, when struct safe_device is used
	 *   Tw > Tw'.
	 * Let Trd be the time to read a block from @dev.
	 *
	 * Notice that each single-block pass reduces the search space in half,
	 * and that to reduce the search space in half writing blocks,
	 * one has to increase m of one.
	 *
	 * Thus, in order to be better writing more blocks than
	 * going for another pass, the following relation must be true:
	 *
	 * Tr + Tw + Tw' >= (w - 1)(Tw + Tw')
	 *
	 * The relation above assumes Trd = 0.
	 *
	 * The left side of the relation above is the time to do _another_
	 * pass writing a single block, whereas the right side is the time to
	 * stay in the same pass and write (w - 1) more blocks.
	 * In order words, if there is no advantage to write more blocks,
	 * we stick to single-block passes.
	 *
	 * Tw' is there to account for any operation that writes
	 * the blocks back (e.g. using struct safe_device), otherwise
	 * processing operations related per written blocks that is not
	 * being accounted for (e.g. reading the blocks back to test).
	 *
	 * Solving the relation for w: w <= Tr/(Tw + Tw') + 2
	 *
	 * However, we are not interested in any w, but only those of
	 * of the form (2^m - 1) to make sure that we are not better off
	 * calling another pass. Thus, solving the previous relation for m:
	 *
	 * m <= log_2(Tr/(Tw + Tw') + 3)
	 *
	 * We approximate Tw' making it equal to Tw.
	 */
	t_w_us = (double)write_time_us / write_count;
	t_r_us = (double)reset_time_us / reset_count;
	t_2w_us = t_w_us > 0. ? 2. * t_w_us : 1.; /* Avoid zero division. */
	n_block_order = ilog2(round(t_r_us / t_2w_us + 3.));

	/* Bound the maximum number of blocks per pass to limit
	 * the necessary amount of memory struct safe_device pre-allocates.
	 */
	if (n_block_order > MAX_N_BLOCK_ORDER)
		n_block_order = MAX_N_BLOCK_ORDER;

	return (1 << n_block_order) - 1;
}

/* Write blocks whose offsets are after @left_pos but
 * less or equal to @right_pos.
 */
static int write_test_blocks(struct device *dev, const char *stamp_blk,
	uint64_t left_pos, uint64_t right_pos,
	uint64_t *pa, uint64_t *pb, uint64_t *pmax_idx)
{
	uint64_t pos, last_pos;
	uint64_t n_block = estimate_best_n_block(dev);

	assert(n_block >= 1);

	/* Find coeficients of function a*idx + b where idx <= max_idx. */
	assert(left_pos < right_pos);
	*pb = left_pos + 1;
	*pa = round((right_pos - *pb) / (n_block + 1.));
	*pa = !*pa ? 1ULL : *pa;
	*pmax_idx = (right_pos - *pb) / *pa;
	if (*pmax_idx >= n_block) {
		/* Shift the zero of the function to the right.
		 * This avoids picking the leftmost block when a more
		 * informative block to the right is available.
		 * This also biases toward righter blocks,
		 * what improves the time to test good flash drives.
		 */
		*pb += *pa;

		*pmax_idx = n_block - 1;
	}
	last_pos = *pa * *pmax_idx + *pb;
	assert(last_pos <= right_pos);

	/* Write test blocks. */
	for (pos = *pb; pos <= last_pos; pos += *pa)
		if (dev_write_block(dev, stamp_blk, pos) &&
			dev_write_block(dev, stamp_blk, pos))
			return true;
	return false;
}

/* Return true if the test block at @pos is damaged. */
static int test_test_block(struct device *dev,
	const char *stamp_blk, char *probe_blk, uint64_t pos)
{
	if (dev_read_block(dev, probe_blk, pos) &&
		dev_read_block(dev, probe_blk, pos))
		return true;

	return !equal_blk(dev, stamp_blk, probe_blk);
}

static int probe_test_blocks(struct device *dev,
	const char *stamp_blk, char *probe_blk,
	uint64_t *pleft_pos, uint64_t *pright_pos,
	uint64_t a, uint64_t b, uint64_t max_idx)
{
	/* Signed variables. */
	int64_t left_idx = 0;
	int64_t right_idx = max_idx;
	int64_t idx = right_idx;
	while (left_idx <= right_idx) {
		uint64_t pos = a * idx + b;
		if (test_test_block(dev, stamp_blk, probe_blk, pos)) {
			right_idx = idx - 1;
			*pright_pos = pos;
		} else {
			left_idx = idx + 1;
			*pleft_pos = pos;
		}
		idx = (left_idx + right_idx) / 2;
	}
	return  false;
}

/* Caller must guarantee that the left bock is good, and written. */
static int search_edge(struct device *dev,
	uint64_t *pleft_pos, uint64_t right_pos,
	const char *stamp_blk, char *probe_blk)
{
	uint64_t gap = right_pos - *pleft_pos;
	uint64_t prv_gap = gap + 1;
	while (prv_gap > gap && gap >= 1) {
		uint64_t a, b, max_idx;
		if (write_test_blocks(dev, stamp_blk, *pleft_pos, right_pos,
			&a, &b, &max_idx))
			return true;
		/* Reset. */
		if (dev_reset(dev) && dev_reset(dev))
			return true;
		if (probe_test_blocks(dev, stamp_blk, probe_blk,
			pleft_pos, &right_pos, a, b, max_idx))
			return true;

		prv_gap = gap;
		gap = right_pos - *pleft_pos;
	}
	return false;
}

/* XXX Write random data to make it harder for fake chips to become "smarter".
 * There would be a random seed.
 * Buffer cannot be all 0x00 or all 0xFF.
 */
static void fill_buffer(char *buf, int len)
{
	memset(buf, 0xAA, len);
}

int probe_device_max_blocks(struct device *dev)
{
	uint64_t num_blocks = dev_get_size_byte(dev) >>
		dev_get_block_order(dev);
	int n = ceiling_log2(num_blocks);

	/* Make sure that there is no overflow in the formula below.
	 * The number 10 is arbitrary here, that is, it's not tight.
	 */
	assert(MAX_N_BLOCK_ORDER < sizeof(int) - 10);

	return
		/* search_wrap() */
		1 +
		/* Search_edge()
		 *
		 * The number of used blocks is (p * w); see comments in
		 * estimate_best_n_block() for the definition of the variables.
		 *
		 * p * w = n/m * (2^m - 1) < n/m * 2^m = n * (2^m / m)
		 *
		 * Let f(m) be 2^m / m. One can prove that f(m + 1) >= f(m)
		 * for all m >= 1. Therefore, the following bound is true.
		 *
		 * p * w < n * f(max_m)
		 */
		((n << MAX_N_BLOCK_ORDER) / MAX_N_BLOCK_ORDER);
}

/* XXX Properly handle read and write errors.
 * Review each assert to check if them can be removed.
 */
int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap, int *pblock_order)
{
	uint64_t dev_size_byte = dev_get_size_byte(dev);
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	char stack[511 + (2 << block_order)];
	char *stamp_blk, *probe_blk;
	/* XXX Don't write at the very beginning of the card to avoid
	 * losing the partition table.
	 * But write at a random locations to make harder for fake chips
	 * to become "smarter".
	 * And try a couple of blocks if they keep failing.
	 */
	uint64_t left_pos = 10;
	uint64_t right_pos = (dev_size_byte >> block_order) - 1;
	struct device *pdev;

	assert(dev_size_byte % block_size == 0);
	assert(left_pos < right_pos);

	pdev = create_perf_device(dev);
	if (!pdev)
		return -ENOMEM;

	/* Aligning these pointers is necessary to directly read and write
	 * the block device.
	 * For the file device, this is superfluous.
	 */
	stamp_blk = align_512(stack);
	probe_blk = stamp_blk + block_size;

	fill_buffer(stamp_blk, block_size);

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */
	if (test_block(pdev, stamp_blk, probe_blk, left_pos))
		goto bad;

	if (search_wrap(pdev, left_pos, &right_pos, stamp_blk, probe_blk))
		goto bad;

	if (search_edge(pdev, &left_pos, right_pos, stamp_blk, probe_blk))
		goto bad;

	*preal_size_byte = (left_pos + 1) << block_order;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(right_pos << block_order);
	*pblock_order = block_order;
	goto out;

bad:
	*preal_size_byte = 0;
	*pannounced_size_byte = dev_size_byte;
	*pwrap = ceiling_log2(dev_size_byte);
	*pblock_order = block_order;

out:
	pdev_detach_and_free(pdev);
	return 0;
}
