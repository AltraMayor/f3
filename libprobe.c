#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <errno.h>
#include <time.h>	/* For time().		*/
#include <sys/time.h>	/* For gettimeofday().	*/

#include "libutils.h"
#include "libprobe.h"

static int write_blocks(struct device *dev,
	uint64_t first_pos, uint64_t last_pos, uint64_t salt)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	/* Aligning these pointers is necessary to directly read and write
	 * the block device.
	 * For the file device, this is superfluous.
	 */
	char stack[align_head(block_order) + BIG_BLOCK_SIZE_BYTE];
	char *buffer = align_mem(stack, block_order);
	char *stamp_blk = buffer;
	char *flush_blk = buffer + BIG_BLOCK_SIZE_BYTE;
	uint64_t offset = first_pos << block_order;
	uint64_t pos, write_pos = first_pos;

	for (pos = first_pos; pos <= last_pos; pos++) {
		fill_buffer_with_block(stamp_blk, block_order, offset, salt);
		stamp_blk += block_size;
		offset += block_size;

		if (stamp_blk == flush_blk || pos == last_pos) {
			if (dev_write_blocks(dev, buffer, write_pos, pos) &&
				dev_write_blocks(dev, buffer, write_pos, pos))
				return true;
			stamp_blk = buffer;
			write_pos = pos + 1;
		}
	}

	return false;
}

static int high_level_reset(struct device *dev, uint64_t start_pos,
	uint64_t cache_size_block, int need_reset, uint64_t salt)
{
	if (write_blocks(dev,
		start_pos, start_pos + cache_size_block - 1, salt))
		return true;

	/* Reset. */
	if (need_reset && dev_reset(dev) && dev_reset(dev))
		return true;

	return false;
}

/* Statistics used by bisect() in order to optimize the proportion
 * between writes and resets.
 */
struct bisect_stats {
	int		write_count;
	int		reset_count;
	uint64_t	write_time_us;
	uint64_t	reset_time_us;
};

static void init_bisect_stats(struct bisect_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

#define MAX_N_BLOCK_ORDER	10

static uint64_t estimate_n_bisect_blocks(struct bisect_stats *pstats)
{
	double t_w_us, t_2w_us, t_r_us;
	uint64_t n_block_order;

	if (pstats->write_count < 3 || pstats->reset_count < 1) {
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
	t_w_us = (double)pstats->write_time_us / pstats->write_count;
	t_r_us = (double)pstats->reset_time_us / pstats->reset_count;
	t_2w_us = t_w_us > 0. ? 2. * t_w_us : 1.; /* Avoid zero division. */
	n_block_order = ilog2(round(t_r_us / t_2w_us + 3.));

	/* Bound the maximum number of blocks per pass to limit
	 * the necessary amount of memory struct safe_device pre-allocates.
	 */
	if (n_block_order > MAX_N_BLOCK_ORDER)
		n_block_order = MAX_N_BLOCK_ORDER;

	return (1 << n_block_order) - 1;
}

/* Write blocks whose offsets are after @left_pos and before @right_pos. */
static int write_bisect_blocks(struct device *dev,
	uint64_t left_pos, uint64_t right_pos, uint64_t n_blocks,
	uint64_t salt, uint64_t *pa, uint64_t *pb, uint64_t *pmax_idx)
{
	uint64_t pos, last_pos;

	assert(n_blocks >= 1);

	/* Find coeficients of function a*idx + b where idx <= max_idx. */
	assert(left_pos < right_pos);
	assert(right_pos - left_pos >= 2);
	*pb = left_pos + 1;
	*pa = round((right_pos - *pb - 1.) / (n_blocks + 1.));
	*pa = !*pa ? 1ULL : *pa;
	*pmax_idx = (right_pos - *pb - 1) / *pa;
	if (*pmax_idx >= n_blocks) {
		/* Shift the zero of the function to the right.
		 * This avoids picking the leftmost block when a more
		 * informative block to the right is available.
		 */
		*pb += *pa;

		*pmax_idx = n_blocks - 1;
	}
	last_pos = *pa * *pmax_idx + *pb;
	assert(last_pos < right_pos);

	/* Write test blocks. */
	for (pos = *pb; pos <= last_pos; pos += *pa)
		if (write_blocks(dev, pos, pos, salt))
			return true;
	return false;
}

static int is_block_good(struct device *dev, uint64_t pos, int *pis_good,
	uint64_t salt)
{
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	char stack[align_head(block_order) + block_size];
	char *probe_blk = align_mem(stack, block_order);
	uint64_t found_offset;

	if (dev_read_blocks(dev, probe_blk, pos, pos) &&
		dev_read_blocks(dev, probe_blk, pos, pos))
		return true;

	*pis_good = !validate_buffer_with_block(probe_blk, block_order,
			&found_offset, salt) &&
		found_offset == (pos << block_order);
	return false;
}

static int probe_bisect_blocks(struct device *dev,
	uint64_t *pleft_pos, uint64_t *pright_pos, uint64_t salt,
	uint64_t a, uint64_t b, uint64_t max_idx)
{
	/* Signed variables. */
	int64_t left_idx = 0;
	int64_t right_idx = max_idx;
	while (left_idx <= right_idx) {
		int64_t idx = (left_idx + right_idx) / 2;
		uint64_t pos = a * idx + b;
		int is_good;
		if (is_block_good(dev, pos, &is_good, salt))
			return true;
		if (is_good) {
			left_idx = idx + 1;
			*pleft_pos = pos;
		} else {
			right_idx = idx - 1;
			*pright_pos = pos;
		}
	}
	return false;
}

/* This function assumes that the block at @left_pos is good, and
 *	that the block at @*pright_pos is bad.
 */
static int bisect(struct device *dev, struct bisect_stats *pstats,
	uint64_t left_pos, uint64_t *pright_pos, uint64_t reset_pos,
	uint64_t cache_size_block, int need_reset, uint64_t salt)
{
	uint64_t gap = *pright_pos - left_pos;
	struct timeval t1, t2;

	assert(*pright_pos > left_pos);
	while (gap >= 2) {
		uint64_t a, b, max_idx;
		uint64_t n_blocks = estimate_n_bisect_blocks(pstats);

		assert(!gettimeofday(&t1, NULL));
		if (write_bisect_blocks(dev, left_pos, *pright_pos, n_blocks,
			salt, &a, &b, &max_idx))
			return true;
		assert(!gettimeofday(&t2, NULL));
		pstats->write_count += max_idx + 1;
		pstats->write_time_us += diff_timeval_us(&t1, &t2);

		/* Reset. */
		assert(!gettimeofday(&t1, NULL));
		if (high_level_reset(dev, reset_pos,
			cache_size_block, need_reset, salt))
			return true;
		assert(!gettimeofday(&t2, NULL));
		pstats->reset_count++;
		pstats->reset_time_us += diff_timeval_us(&t1, &t2);

		if (probe_bisect_blocks(dev, &left_pos, pright_pos, salt,
			 a, b, max_idx))
			return true;

		gap = *pright_pos - left_pos;
	}
	assert(gap == 1);
	return false;
}

static int count_good_blocks(struct device *dev, uint64_t *pcount,
	uint64_t first_pos, uint64_t last_pos, uint64_t salt)
{
	const int block_size = dev_get_block_size(dev);
	const int block_order = dev_get_block_order(dev);
	char stack[align_head(block_order) + BIG_BLOCK_SIZE_BYTE];
	char *buffer = align_mem(stack, block_order);
	uint64_t expected_sector_offset = first_pos << block_order;
	uint64_t start_pos = first_pos;
	uint64_t step = (BIG_BLOCK_SIZE_BYTE >> block_order) - 1;
	uint64_t count = 0;

	assert(BIG_BLOCK_SIZE_BYTE >= block_size);

	while (start_pos <= last_pos) {
		char *probe_blk = buffer;
		uint64_t pos, next_pos = start_pos + step;

		if (next_pos > last_pos)
			next_pos = last_pos;
		if (dev_read_blocks(dev, buffer, start_pos, next_pos) &&
			dev_read_blocks(dev, buffer, start_pos, next_pos))
			return true;

		for (pos = start_pos; pos <= next_pos; pos++) {
			uint64_t found_sector_offset;
			if (!validate_buffer_with_block(probe_blk, block_order,
					&found_sector_offset, salt) &&
				expected_sector_offset == found_sector_offset)
				count++;
			expected_sector_offset += block_size;
			probe_blk += block_size;
		}

		start_pos = next_pos + 1;
	}

	*pcount = count;
	return false;
}

static int assess_reset_effect(struct device *dev,
	uint64_t *pcache_size_block, int *pneed_reset, int *pdone,
	uint64_t first_pos, uint64_t last_pos, uint64_t salt)
{
	uint64_t write_target = (last_pos + 1) - first_pos;
	uint64_t b4_reset_count_block, after_reset_count_block;

	if (count_good_blocks(dev, &b4_reset_count_block,
		first_pos, last_pos, salt))
		return true;

	/* Reset. */
	if (dev_reset(dev) && dev_reset(dev))
		return true;

	if (count_good_blocks(dev, &after_reset_count_block,
		first_pos, last_pos, salt))
		return true;

	if (after_reset_count_block < write_target) {
		assert(after_reset_count_block <= b4_reset_count_block);
		*pcache_size_block = after_reset_count_block;
		*pneed_reset = after_reset_count_block < b4_reset_count_block;
		*pdone = true;
		return false;
	}

	*pdone = false;
	return false;
}

static inline uint64_t uint64_rand(void)
{
	return ((uint64_t)rand() << 32) | rand();
}

static uint64_t uint64_rand_range(uint64_t a, uint64_t b)
{
	uint64_t r = uint64_rand();
	assert(a <= b);
	return a + (r % (b - a + 1));
}

#define N_BLOCK_SAMPLES	64

static int probabilistic_test(struct device *dev,
	uint64_t first_pos, uint64_t last_pos, int *pfound_a_bad_block,
	uint64_t salt)
{
	uint64_t gap;
	int i, n, is_linear;

	if (first_pos > last_pos)
		goto not_found;

	/* Let g be the number of good blocks between
	 *   @first_pos and @last_pos including them.
	 * Let b be the number of bad and overwritten blocks between
	 *   @first_pos and @last_pos including them.
	 *
	 * The probability Pr_g of sampling a good block at random between
	 *	@first_pos and @last_pos is Pr_g = g / (g + b), and
	 *	the probability Pr_1b that among k block samples at least
	 *	one block is bad is Pr_1b = 1 - Pr_g^k.
	 *
	 * Assuming Pr_g <= 95% and k = 64, Pr_1b >= 96.2%.
	 *	That is, with high probability (i.e. Pr_1b),
	 *	one can find at least a bad block with k samples
	 *	when most blocks are good (Pr_g).
	 */

	/* Test @samples. */
	gap = last_pos - first_pos + 1;
	is_linear = gap <= N_BLOCK_SAMPLES;
	n = is_linear ? gap : N_BLOCK_SAMPLES;
	for (i = 0; i < n; i++) {
		uint64_t sample_pos = is_linear
			? first_pos + i
			: uint64_rand_range(first_pos, last_pos);
		int is_good;

		if (is_block_good(dev, sample_pos, &is_good, salt))
			return true;
		if (!is_good) {
			/* Found a bad block. */
			*pfound_a_bad_block = true;
			return false;
		}
	}

not_found:
	*pfound_a_bad_block = false;
	return false;
}

static int uint64_cmp(const void *pa, const void *pb)
{
	const uint64_t *pia = pa;
	const uint64_t *pib = pb;
	return *pia - *pib;
}

static int find_a_bad_block(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos, int *found_a_bad_block,
	uint64_t reset_pos, uint64_t cache_size_block, int need_reset,
	uint64_t salt)
{
	/* We need to list all sampled blocks because
	 * we need a sorted array; read the code to find the why.
	 * If the sorted array were not needed, one could save the seed
	 * of the random sequence and repeat the sequence to read the blocks
	 * after writing them.
	 */
	uint64_t samples[N_BLOCK_SAMPLES];
	uint64_t gap, prv_sample;
	int n, i;

	if (*pright_pos <= left_pos + 1)
		goto not_found;

	/* The code below relies on the same analytical result derived
	 * in probabilistic_test().
	 */

	/* Fill up @samples. */
	gap = *pright_pos - left_pos - 1;
	if (gap <= N_BLOCK_SAMPLES) {
		n = gap;
		for (i = 0; i < n; i++)
			samples[i] = left_pos + 1 + i;

		/* Write @samples. */
		if (write_blocks(dev, left_pos + 1, *pright_pos - 1, salt))
			return true;
	} else {
		n = N_BLOCK_SAMPLES;
		for (i = 0; i < n; i++)
			samples[i] = uint64_rand_range(left_pos + 1,
				*pright_pos - 1);

		/* Sort entries of @samples to minimize reads.
		 * As soon as one finds a bad block, one can stop and ignore
		 * the remaining blocks because the found bad block is
		 * the leftmost bad block.
		 */
		qsort(samples, n, sizeof(uint64_t), uint64_cmp);

		/* Write @samples. */
		prv_sample = left_pos;
		for (i = 0; i < n; i++) {
			if (samples[i] == prv_sample)
				continue;
			prv_sample = samples[i];
			if (write_blocks(dev, prv_sample, prv_sample, salt))
				return true;
		}
	}

	/* Reset. */
	if (high_level_reset(dev, reset_pos,
		cache_size_block, need_reset, salt))
		return true;

	/* Test @samples. */
	prv_sample = left_pos;
	for (i = 0; i < n; i++) {
		int is_good;

		if (samples[i] == prv_sample)
			continue;

		prv_sample = samples[i];
		if (is_block_good(dev, prv_sample, &is_good, salt))
			return true;
		if (!is_good) {
			/* Found the leftmost bad block. */
			*pright_pos = prv_sample;
			*found_a_bad_block = true;
			return false;
		}
	}

not_found:
	*found_a_bad_block = false;
	return false;
}

/* Both need to be a power of 2 and larger than, or equal to 2^block_order. */
#define MIN_CACHE_SIZE_BYTE	(1ULL << 20)
#define MAX_CACHE_SIZE_BYTE	(1ULL << 30)

static int find_cache_size(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos, uint64_t *pcache_size_block,
	int *pneed_reset, int *pgood_drive, const uint64_t salt)
{
	const int block_order = dev_get_block_order(dev);
	uint64_t write_target = MIN_CACHE_SIZE_BYTE >> block_order;
	uint64_t final_write_target = MAX_CACHE_SIZE_BYTE >> block_order;
	uint64_t first_pos, last_pos, end_pos;
	int done;

	/*
	 *	Basis
	 *
	 * The key difference between the basis and the inductive step is
	 * the fact that the basis always calls assess_reset_effect().
	 * This difference is not for correctness, that is, one can remove it,
	 * and fold the basis into the inductive step.
	 * However, this difference is an important speedup because many
	 * fake drives do not have permanent cache.
	 */

	assert(write_target > 0);
	assert(write_target < final_write_target);

	last_pos = end_pos = *pright_pos - 1;
	/* This convoluted test is needed because
	 * the variables are unsigned.
	 * In a simplified form, it tests the following:
	 *	*pright_pos - write_target > left_pos
	 */
	if (*pright_pos > left_pos + write_target) {
		first_pos = *pright_pos - write_target;
	} else if (*pright_pos > left_pos + 1) {
		/* There's no room to write @write_target blocks,
		 * so write what's possible.
		 */
		first_pos = left_pos + 1;
	} else {
		goto good;
	}

	if (write_blocks(dev, first_pos, last_pos, salt))
		goto bad;

	if (assess_reset_effect(dev, pcache_size_block,
		pneed_reset, &done, first_pos, end_pos, salt))
		goto bad;
	if (done) {
		*pright_pos = first_pos;
		*pgood_drive = false;
		return false;
	}

	/*
	 *	Inductive step
	 */

	while (write_target < final_write_target) {
		int found_a_bad_block;

		write_target <<= 1;
		last_pos = first_pos - 1;
		if (first_pos > left_pos + write_target)
			first_pos -= write_target;
		else if (first_pos > left_pos + 1)
			first_pos = left_pos + 1;
		else
			break; /* Cannot write any further. */

		/* Write @write_target blocks before
		 * the previously written blocks.
		 */
		if (write_blocks(dev, first_pos, last_pos, salt))
			goto bad;

		if (probabilistic_test(dev, first_pos, end_pos,
			&found_a_bad_block, salt))
			goto bad;
		if (found_a_bad_block) {
			if (assess_reset_effect(dev, pcache_size_block,
				pneed_reset, &done, first_pos, end_pos, salt))
				goto bad;
			assert(done);
			*pright_pos = first_pos;
			*pgood_drive = false;
			return false;
		}
	}

good:
	*pright_pos = end_pos + 1;
	*pcache_size_block = 0;
	*pneed_reset = false;
	*pgood_drive = true;
	return false;

bad:
	/* *pright_pos does not change. */
	*pcache_size_block = 0;
	*pneed_reset = false;
	*pgood_drive = false;
	return true;
}

static int find_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	uint64_t reset_pos, uint64_t cache_size_block, int need_reset,
	uint64_t salt)
{
	uint64_t offset, high_bit, pos = left_pos + 1;
	int is_good, block_order;

	/*
	 *	Basis
	 */

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */

	if (pos >= *pright_pos)
		return false;

	if (write_blocks(dev, pos, pos, salt) ||
		high_level_reset(dev, reset_pos,
			cache_size_block, need_reset, salt) ||
		is_block_good(dev, pos, &is_good, salt) ||
		!is_good)
		return true;

	/*
	 *	Inductive step
	 */

	block_order = dev_get_block_order(dev);
	offset = pos << block_order;
	high_bit = clp2(pos);
	if (high_bit <= pos)
		high_bit <<= 1;
	pos += high_bit;

	while (pos < *pright_pos) {
		char stack[align_head(block_order) + (1 << block_order)];
		char *probe_blk = align_mem(stack, block_order);
		uint64_t found_offset;

		if (dev_read_blocks(dev, probe_blk, pos, pos) &&
			dev_read_blocks(dev, probe_blk, pos, pos))
			return true;

		if (!validate_buffer_with_block(probe_blk, block_order,
			&found_offset, salt) &&
			found_offset == offset) {
			*pright_pos = high_bit;
			return false;
		}

		high_bit <<= 1;
		pos = high_bit + left_pos + 1;
	}

	return false;
}

uint64_t probe_device_max_blocks(struct device *dev)
{
	const int block_order = dev_get_block_order(dev);
	uint64_t num_blocks = dev_get_size_byte(dev) >> block_order;
	int n = ceiling_log2(num_blocks);

	/* Make sure that there is no overflow in the formula below.
	 * The number 10 is arbitrary here, that is, it's not tight.
	 */
	assert(MAX_N_BLOCK_ORDER < sizeof(int) - 10);

	return
		/* find_cache_size() */
		(MAX_CACHE_SIZE_BYTE >> (block_order - 1)) +
		/* find_wrap() */
		1 +
		/* The number below is just an educated guess. */
		128 * (
			/* bisect()
			 *
			 * The number of used blocks is (p * w); see comments
			 * in estimate_n_bisect_blocks() for the definition of
			 * the variables.
			 *
			 * p * w = n/m * (2^m - 1) < n/m * 2^m = n * (2^m / m)
			 *
			 * Let f(m) be 2^m / m. One can prove that
			 * f(m + 1) >= f(m) for all m >= 1.
			 * Therefore, the following bound is true.
			 *
			 * p * w < n * f(max_m)
			 */
			((n << MAX_N_BLOCK_ORDER) / MAX_N_BLOCK_ORDER) +
			/* find_a_bad_block() */
			N_BLOCK_SAMPLES
		);
}

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap,
	uint64_t *pcache_size_block, int *pneed_reset, int *pblock_order)
{
	const uint64_t dev_size_byte = dev_get_size_byte(dev);
	const int block_order = dev_get_block_order(dev);
	struct bisect_stats stats;
	uint64_t salt, cache_size_block;
	uint64_t left_pos, right_pos, mid_drive_pos, reset_pos;
	int need_reset, good_drive, wrap, found_a_bad_block;

	assert(block_order <= 20);

	/* @left_pos must point to a good block.
	 * We just point to the last block of the first 1MB of the card
	 * because this region is reserved for partition tables.
	 *
	 * Given that all writing is confined to the interval
	 * (@left_pos, @right_pos), we avoid losing the partition table.
	 */
	left_pos = (1ULL << (20 - block_order)) - 1;

	/* @right_pos must point to a bad block.
	 * We just point to the block after the very last block.
	 */
	right_pos = dev_size_byte >> block_order;

	/* @left_pos cannot be equal to @right_pos since
	 * @left_pos points to a good block, and @right_pos to a bad block.
	 */
	assert(left_pos < right_pos);

	/* I, Michel Machado, define that any drive with less than
	 * this number of blocks is fake.
	 */
	mid_drive_pos = clp2(right_pos / 2);

	assert(left_pos < mid_drive_pos);
	assert(mid_drive_pos < right_pos);

	/* This call is needed due to rand(). */
	srand(time(NULL));

	salt = uint64_rand();

	if (find_cache_size(dev, mid_drive_pos - 1, &right_pos,
		&cache_size_block, &need_reset, &good_drive, salt))
		goto bad;
	assert(mid_drive_pos <= right_pos);
	reset_pos = right_pos;

	if (find_wrap(dev, left_pos, &right_pos,
		reset_pos, cache_size_block, need_reset, salt))
		goto bad;
	wrap = ceiling_log2(right_pos << block_order);

	init_bisect_stats(&stats);
	if (!good_drive) {
		if (mid_drive_pos < right_pos)
			right_pos = mid_drive_pos;
		if (bisect(dev, &stats, left_pos, &right_pos,
			reset_pos, cache_size_block, need_reset, salt))
			goto bad;
	}

	do {
		if (find_a_bad_block(dev, left_pos, &right_pos,
			&found_a_bad_block, reset_pos, cache_size_block,
			need_reset, salt))
			goto bad;

		if (found_a_bad_block &&
			bisect(dev, &stats, left_pos, &right_pos,
				reset_pos, cache_size_block, need_reset, salt))
			goto bad;
	} while (found_a_bad_block);

	if (right_pos == left_pos + 1) {
		/* Bad drive. */
		right_pos = 0;
	}

	*preal_size_byte = right_pos << block_order;
	*pwrap = wrap;
	goto out;

bad:
	*preal_size_byte = 0;
	*pwrap = ceiling_log2(dev_size_byte);

out:
	*pannounced_size_byte = dev_size_byte;
	*pcache_size_block = cache_size_block;
	*pneed_reset = need_reset;
	*pblock_order = block_order;
	return false;
}
