#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>	/* For time().		*/
#include <inttypes.h>

#include "libutils.h"
#include "libflow.h"
#include "libprobe.h"

static int _write_blocks(struct device *dev, const char *buf,
	uint64_t first_pos, uint64_t last_pos, struct flow *fw,
	progress_cb cb, unsigned int indent)
{
	if (dev_write_blocks(dev, buf, first_pos, last_pos) &&
			dev_write_blocks(dev, buf, first_pos, last_pos)) {
		clear_progress(fw);
		cb(indent, "I/O ERROR: Write error at block%s [%" PRIu64 ", %\" PRIu64 \"]!\n",
			first_pos != last_pos ? "s" : "", first_pos, last_pos);
		return true;
	}
	return false;
}

/* Some fake drives have a "tiny" (e.g. 8KB) cache for random accesses and
 * a "large" (e.g. 4MB) cache for sequential accesses.  So, for these
 * fake drives, a random read may return a bad block, while a sequential
 * read that includes that block returns it as a good block.
 * This situation has been verified with the donated drive from
 * issue #50 (https://github.com/AltraMayor/f3/issues/50).
 *
 * The example cache sizes come from the following
 * discussion among Linux kernel developers:
 * https://linux-arm-kernel.infradead.narkive.com/h3crV0D3/mmc-quirks-relating-to-performance-lifetime
 *
 * To circunvent this problem, the probe must only issue random reads.
 */
struct rdwr_info {
	uint64_t cache_pos;
	uint64_t cache_size_block;
	uint64_t salt;

	struct dynamic_buffer seqw_dbuf;
	struct flow seqw_fw;
	struct flow randw_fw;

	struct flow randr_fw;
};

static int write_random_blocks(struct device *dev, const uint64_t pos[],
	uint32_t n_pos, struct rdwr_info *rwi, progress_cb cb,
	unsigned int indent)
{
	const unsigned int block_order = dev_get_block_order(dev);
	const unsigned int block_size = dev_get_block_size(dev);
	/* Aligning these pointers is necessary to directly read and write
	 * the block device. For the file device, this is superfluous.
	 */
	char stack[align_head(block_order) + block_size];
	char *buffer = align_mem(stack, block_order);
	uint32_t i;

	if (n_pos == 0)
		return false;

	inc_total_blocks(&rwi->randw_fw, n_pos);
	fw_set_indent(&rwi->randw_fw, indent);

	start_measurement(&rwi->randw_fw);
	for (i = 0; i < n_pos; i++) {
		fill_buffer_with_block(buffer, block_order,
			pos[i] << block_order, rwi->salt);
		if (_write_blocks(dev, buffer, pos[i], pos[i], &rwi->randw_fw,
				cb, indent))
			return true;
		measure(&rwi->randw_fw, 1, NULL);
	}
	end_measurement(&rwi->randw_fw);
	return false;
}

static int write_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	const unsigned int block_order = dev_get_block_order(dev);
	const unsigned int block_size = dev_get_block_size(dev);
	uint64_t offset = first_block << block_order;
	uint64_t first_pos = first_block;

	if (first_block > last_block)
		return false;

	inc_total_blocks(&rwi->seqw_fw, last_block - first_block + 1);
	fw_set_indent(&rwi->seqw_fw, indent);

	start_measurement(&rwi->seqw_fw);
	while (first_pos <= last_block) {
		const uint64_t max_blocks_to_write = last_block - first_pos + 1;
		uint64_t blocks_to_write = MIN(
			get_rem_chunk_blocks(&rwi->seqw_fw),
			max_blocks_to_write);
		size_t buf_len = blocks_to_write << block_order;
		char *buffer, *stamp_blk;
		uint64_t pos, next_pos;

		buffer = dbuf_get_buf(&rwi->seqw_dbuf, block_order, &buf_len);
		blocks_to_write = buf_len >> block_order;
		assert(blocks_to_write > 0);
		next_pos = first_pos + blocks_to_write;

		stamp_blk = buffer;
		for (pos = first_pos; pos < next_pos; pos++) {
			fill_buffer_with_block(stamp_blk, block_order, offset,
				rwi->salt);
			stamp_blk += block_size;
			offset += block_size;
		}

		if (_write_blocks(dev, buffer, first_pos, next_pos - 1,
				&rwi->seqw_fw, cb, indent))
			return true;

		measure(&rwi->seqw_fw, blocks_to_write, NULL);
		first_pos = next_pos;
	}
	end_measurement(&rwi->seqw_fw);
	return false;
}

static int overwhelm_cache(struct device *dev,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	if (rwi->cache_size_block == 0)
		return false;
	cb(indent, "Overwhelming cache\n");
	return write_blocks(dev, rwi->cache_pos,
		rwi->cache_pos + rwi->cache_size_block - 1, rwi, cb, indent);
}

static int read_block(struct device *dev, char *buf, uint64_t pos,
	struct flow *fw, progress_cb cb, unsigned int indent)
{
	if (dev_read_blocks(dev, buf, pos, pos) &&
		dev_read_blocks(dev, buf, pos, pos)) {
		clear_progress(fw);
		cb(indent, "I/O ERROR: Read error at block %" PRIu64 "!\n",
			pos);
		return true;
	}
	return false;
}

static uint64_t bs_to_set(enum block_state bs)
{
	switch (bs) {
	case bs_unknown:
	case bs_good:
	case bs_bad:
	case bs_changed:
	case bs_overwritten:
		assert(bs < sizeof(uint64_t) * 8);
		return 1ULL << bs;

	default:
		assert(0);
	}
}

static uint64_t bss_to_set(const enum block_state bss[], uint32_t n_bs)
{
	uint64_t bs_set = 0;
	uint32_t i;

	for (i = 0; i < n_bs; i++)
		bs_set |= bs_to_set(bss[i]);
	return bs_set;
}

static inline uint64_t neg_bs_set(uint64_t bs_set)
{
	return ~bs_set;
}

static inline bool in_bs_set(uint64_t bs_set, enum block_state bs)
{
	assert(bs < sizeof(bs_set) * 8);
	return (bs_set >> bs) & 1;
}

struct def_x_block {
	uint64_t pos;
	uint64_t expected_offset;
};

static int find_first_x_block(struct device *dev,
	const struct def_x_block x_blocks[], uint32_t n_blocks,
	uint64_t bs_set, uint32_t *pfirst_x_block_idx,
	enum block_state *pstate, struct rdwr_info *rwi,
	progress_cb cb, unsigned int indent)
{
	const unsigned int block_order = dev_get_block_order(dev);
	const unsigned int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + block_size];
	char *probe_blk = align_mem(stack, block_order);
	uint32_t i;

	if (n_blocks == 0)
		goto not_found;

	inc_total_blocks(&rwi->randr_fw, n_blocks);
	fw_set_indent(&rwi->randr_fw, indent);

	start_measurement(&rwi->randr_fw);
	for (i = 0; i < n_blocks; i++) {
		uint64_t found_offset;
		enum block_state bs;

		if (read_block(dev, probe_blk, x_blocks[i].pos, &rwi->randr_fw,
				cb, indent))
			return true;
		bs = validate_buffer_with_block(probe_blk, block_order,
			x_blocks[i].expected_offset, &found_offset, rwi->salt);
		measure(&rwi->randr_fw, 1, NULL);

		if (in_bs_set(bs_set, bs)) {
			/* Found the first x_block. */
			*pfirst_x_block_idx = i;
			*pstate = bs;
			end_measurement(&rwi->randr_fw);
			return false;
		}
	}
	end_measurement(&rwi->randr_fw);

not_found:
	*pfirst_x_block_idx = n_blocks;
	return false;
}

static int find_first_bad_block(struct device *dev, const uint64_t pos[],
	uint32_t n_pos, bool *pany_bad, uint64_t *pbad_pos,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	const unsigned int block_order = dev_get_block_order(dev);
	struct def_x_block x_blocks[n_pos];
	enum block_state bs;
	uint32_t i;

	for (i = 0; i < n_pos; i++) {
		x_blocks[i].pos = pos[i];
		x_blocks[i].expected_offset = pos[i] << block_order;
	}

	if (find_first_x_block(dev, x_blocks, n_pos,
			neg_bs_set(bs_to_set(bs_good)),
			&i, &bs, rwi, cb, indent))
		return true;
	*pany_bad = i < n_pos;
	if (*pany_bad) {
		*pbad_pos = x_blocks[i].pos;
		cb(indent, "INFO: Block %" PRIu64 " is %s!\n",
			*pbad_pos, block_state_to_str(bs));
	}
	return false;
}

static inline uint64_t uint64_rand(void)
{
	/* Use exclusive OR to avoid correlation between the two random
	 * numbers. For the lower 32 bits, the zeros from the left shift make
	 * the exclusive OR equivalent to OR. For each bit pair in the higher
	 * 32 bits, there are 2 cases for which the exclusive OR produces 1
	 * (i.e., 0^1 and 1^0), and 2 cases to produce 0 (i.e., 0^0 and 1^1).
	 * If OR were used For each bit pair in the higher 32 bits, there
	 * would be 3 cases to produce 1 (i.e., 0^1, 1^0, and 1^1), and
	 * 1 case to produce 0 (i.e., 0^0). Therefore, the exclusive OR avoids
	 * a bias towards higher values.
	 */
	return ((uint64_t)rand() << 32) ^ rand();
}

static uint64_t uint64_rand_range(uint64_t a, uint64_t b)
{
	uint64_t r = uint64_rand();
	assert(a <= b);
	return a + (r % (b - a + 1));
}

/* Since the list size is small, at most SAMPLING_MAX blocks,
 * the O(n_samples^2) complexity is not a problem.
 */
static void fill_with_unique_samples(uint64_t *samples, uint32_t n_samples,
	uint64_t first_pos, uint64_t last_pos)
{
	uint32_t i, j;

	assert(n_samples < last_pos - first_pos + 1);
	for (i = 0; i < n_samples; ) {
		uint64_t r = uint64_rand_range(first_pos, last_pos);
		bool unique = true;
		for (j = 0; j < i; j++) {
			if (samples[j] == r) {
				unique = false;
				break;
			}
		}
		if (unique) {
			samples[i] = r;
			i++;
		}
	}
}

static int uint64_cmp(const void *pa, const void *pb)
{
	const uint64_t *pia = pa;
	const uint64_t *pib = pb;
	return *pia - *pib;
}

/* Fill @samples with @n_samples unique random positions in the range
 * [@first_pos, @last_pos]. If @sorted is true, sort the entries of
 * @samples. If @is_linear is true, the entries of @samples are linear
 * (i.e. @first_pos, @first_pos + 1, ...).
 */
static void fill_samples(uint64_t *samples, uint32_t *pn_samples,
	uint64_t first_pos, uint64_t last_pos, bool sorted, bool *pis_linear)
{
	const uint64_t gap = last_pos - first_pos + 1;
	*pis_linear = gap <= *pn_samples;
	if (*pis_linear) {
		uint32_t i;
		*pn_samples = gap;
		for (i = 0; i < gap; i++)
			samples[i] = first_pos + i;

		/* Treat single blocks as random reads instead of
		 * sequential ones.
		 */
		*pis_linear = gap > 1;
	} else {
		fill_with_unique_samples(samples, *pn_samples, first_pos,
			last_pos);
		if (sorted) {
			qsort(samples, *pn_samples, sizeof(uint64_t),
				uint64_cmp);
		}
	}
}

/* Let g be the number of good blocks between
 *	@first_pos and @last_pos including them.
 * Let b be the number of bad and overwritten blocks between
 *	@first_pos and @last_pos including them.
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
static int probabilistic_test(struct device *dev,
	uint64_t first_pos, uint64_t last_pos, int *pfound_a_bad_block,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	uint32_t n_samples = 64;
	uint64_t samples[n_samples];
	bool is_linear, any_bad;
	uint64_t bad_pos;

	if (first_pos > last_pos)
		goto not_found;

	fill_samples(samples, &n_samples, first_pos, last_pos, false,
		&is_linear);
	cb(indent, "Sampling %" PRIu32 " block%s from block%s [%" PRIu64 ", %" PRIu64 "]\n",
		n_samples, n_samples != 1 ? "s" : "",
		first_pos != last_pos ? "s" : "", first_pos, last_pos);
	if (find_first_bad_block(dev, samples, n_samples, &any_bad, &bad_pos,
			rwi, cb, indent))
		return true;
	if (any_bad) {
		/* Found a bad block. */
		*pfound_a_bad_block = true;
		return false;
	}

not_found:
	*pfound_a_bad_block = false;
	return false;
}

/* Find a bad block in the range (left_pos, right_pos) using up to
 * n_samples random samples.
 *
 * If a bad block is found, set *pright_pos to the position of the
 * leftmost bad block.
 *
 * The code relies on the same analytical result derived
 * in probabilistic_test().
 */
static int find_a_bad_block(struct device *dev, uint32_t n_samples,
	uint64_t left_pos, uint64_t *pright_pos, int *found_a_bad_block,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	uint64_t samples[n_samples];
	bool is_linear, any_bad;
	uint64_t bad_pos;

	if (n_samples == 0 || *pright_pos <= left_pos + 1) {
		/* Nothing to sample. */
		goto not_found;
	}

	/* Sort entries of samples to minimize reads.
	 * As soon as one finds a bad block, one can ignore the remaining
	 * samples because the found bad block is the leftmost bad block.
	 */
	fill_samples(samples, &n_samples, left_pos + 1, *pright_pos - 1, true,
		&is_linear);
	cb(indent, "### Sampling %" PRIu32 " block%s from block%s (%" PRIu64 ", %" PRIu64 ")\n",
		n_samples, n_samples != 1 ? "s" : "",
		*pright_pos != left_pos + 2 ? "s" : "", left_pos, *pright_pos);

	cb(indent + 1, "Writing random blocks\n");

	if (is_linear) {
		if (write_blocks(dev, left_pos + 1, *pright_pos - 1, rwi,
				cb, indent + 1))
			return true;
	} else {
		if (write_random_blocks(dev, samples, n_samples, rwi,
				cb, indent + 1))
			return true;
	}

	if (overwhelm_cache(dev, rwi, cb, indent + 1))
		return true;

	/* Test samples. */
	cb(indent + 1, "Reading written blocks\n");
	if (find_first_bad_block(dev, samples, n_samples, &any_bad, &bad_pos,
			rwi, cb, indent + 1))
		return true;
	if (any_bad) {
		/* Found the leftmost bad block. */
		*pright_pos = bad_pos;
		*found_a_bad_block = true;
		return false;
	}

not_found:
	*found_a_bad_block = false;
	return false;
}

/* The following probabilities are caculated using the analytical result
 * derived in probabilistic_test().
 *
 * min_n_samples: Pr_g <= 50% and k = 8		=> Pr_1b >= 99.6%
 * max_n_samples: Pr_g <= 99% and k = 1024	=> Pr_1b >= 99.9966%
 *
 * These parameters must be powers of 2 to satisfy the bounds in
 * probe_max_written_blocks().
 */
#define SAMPLING_MIN (8)
#define SAMPLING_MAX (1024)

/* This function assumes that the block at @left_pos is good, and
 *	that the block at @*pright_pos is bad.
 */
static int sampling_probe(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	uint32_t n_samples = SAMPLING_MIN;
	int found_a_bad_block;
	bool phase1 = true;

	assert(SAMPLING_MAX >= SAMPLING_MIN);
	cb(indent, "## Sampling\n");

	while (*pright_pos > left_pos + n_samples + 1) {
		if (find_a_bad_block(dev, n_samples, left_pos, pright_pos,
				&found_a_bad_block, rwi, cb, indent + 1))
			return true;
		if (found_a_bad_block)
			continue;
		if (phase1) {
			n_samples <<= 1;
			if (n_samples <= SAMPLING_MAX)
				continue;
			phase1 = false;
			n_samples = SAMPLING_MIN;
		}

		/* Phase 2: Minimize the probability that
		 * the rightmost block is bad.
		 */
		left_pos = (*pright_pos + left_pos) / 2;
	}
	if (find_a_bad_block(dev, n_samples, left_pos, pright_pos,
			&found_a_bad_block, rwi, cb, indent + 1))
		return true;
	return false;
}

static void report_cache_size_test(unsigned int indent, progress_cb cb,
	const struct device *dev, uint64_t first_pos, uint64_t last_pos)
{
	double f_size = (last_pos - first_pos + 1) << dev_get_block_order(dev);
	const char *unit = adjust_unit(&f_size);
	cb(indent, "### Testing cache size: %.2f %s; Block%s [%" PRIu64 ", %" PRIu64 "]\n",
		f_size, unit, first_pos != last_pos ? "s" : "",
		first_pos, last_pos);
}

/* This constant needs to be a power of 2 and larger than 2^block_order. */
#define MAX_CACHE_SIZE_BYTE	GIGABYTE_SIZE

static int find_cache_size(struct device *dev, const uint64_t left_pos,
	uint64_t *pright_pos, struct rdwr_info *rwi, progress_cb cb,
	unsigned int indent)
{
	const unsigned int block_order = dev_get_block_order(dev);
	const uint64_t end_pos = *pright_pos - 1;
	uint64_t write_target = 1;
	uint64_t final_write_target = MAX_CACHE_SIZE_BYTE >> block_order;
	uint64_t first_pos = *pright_pos;

	cb(indent, "## Find cache size\n");

	assert(write_target > 0);
	assert(write_target < final_write_target);

	do {
		uint64_t last_pos = first_pos - 1;
		int found_a_bad_block;

		/* This convoluted test is needed because the variables are
		 * unsigned. In a simplified form, it tests the following:
		 *  first_pos - write_target > left_pos
		 */
		if (first_pos > left_pos + write_target) {
			first_pos -= write_target;
		} else if (first_pos > left_pos + 1) {
			/* There's no room to write @write_target blocks,
			 * so write what's possible.
			 */
			first_pos = left_pos + 1;
		} else {
			/* Cannot write any further. */
			break;
		}

		report_cache_size_test(indent + 1, cb, dev, first_pos, end_pos);

		/* Write @write_target blocks before
		 * the previously written blocks.
		 */
		cb(indent + 2, "Writing block%s [%" PRIu64 ", %" PRIu64 "]\n",
			first_pos != last_pos ? "s" : "", first_pos, last_pos);
		if (write_blocks(dev, first_pos, last_pos, rwi, cb, indent + 2))
			goto bad;

		if (probabilistic_test(dev, first_pos, end_pos,
				&found_a_bad_block, rwi, cb, indent + 2))
			goto bad;
		if (found_a_bad_block) {
			*pright_pos = first_pos;
			rwi->cache_size_block = write_target == 1
				? 0 /* There is no cache. */
				: end_pos - first_pos + 1;
			return false;
		}

		write_target <<= 1;

	} while (write_target <= final_write_target);

	/* Good drive. */
	*pright_pos = end_pos + 1;
	rwi->cache_size_block = 0;
	return false;

bad:
	/* *pright_pos does not change. */
	rwi->cache_size_block = 0;
	return true;
}

static int find_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	struct rdwr_info *rwi, progress_cb cb, unsigned int indent)
{
	const uint64_t good_block = left_pos + 1;
	/* The smallest integer m such that 2^m > good_block. */
	const uint32_t m = ceiling_log2(good_block + 1);
	/* Let k be the *smallest* integer such that
	 *	2^(m+k) + good_block >= *pright_pos
	 *
	 * Since this function has to test the blocks
	 * 2^m + good_block, 2^(m+1) + good_block, ..., 2^(m+k-1) + good_block,
	 * k corresponds to the number of samples to test.
	 *
	 * 2^(m+k) + good_block >= *pright_pos [=>]
	 * 2^(m+k) >= *pright_pos - good_block [=>]
	 * m + k >= log2(*pright_pos - good_block) [=>]
	 * k >= log2(*pright_pos - good_block) - m [=>]
	 * k = ceiling_log2(*pright_pos - good_block) - m
	 */
	const uint32_t aux = *pright_pos > good_block
		? ceiling_log2(*pright_pos - good_block)
		: 0;
	const uint32_t n_samples = aux > m ? aux - m : 0;
	const enum block_state bss[] = {bs_good, bs_changed};
	struct def_x_block x_blocks[n_samples];
	bool any_bad;
	uint64_t bad_pos;
	unsigned int block_order;
	uint64_t expected_offset, high_bit;
	uint32_t i;
	enum block_state bs;

	cb(indent, "## Find module\n");

	/*
	 *	Basis
	 */

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */

	if (good_block >= *pright_pos)
		return false;

	cb(indent + 1, "Writing reference block %" PRIu64 "\n", good_block);
	if (write_random_blocks(dev, &good_block, 1, rwi, cb, indent + 1) ||
			overwhelm_cache(dev, rwi, cb, indent + 1))
		return true;

	cb(indent + 1, "Reading reference block\n");
	if (find_first_bad_block(dev, &good_block, 1, &any_bad, &bad_pos,
			rwi, cb, indent + 1) || any_bad)
		return true;

	/*
	 *	Inductive step
	 */

	cb(indent + 1, "Probing module (reading %" PRIu32 " block%s)\n",
		n_samples, n_samples != 1 ? "s" : "");

	block_order = dev_get_block_order(dev);
	expected_offset = good_block << block_order;

	/* high_bit starts as the smallest power of 2 greater than
	 * good_block.
	 */
	high_bit = 1ULL << m; /* 2^m */
	assert(high_bit > good_block);

	/* Fill x_blocks in. */
	for (i = 0; i < n_samples; i++) {
		uint64_t pos = high_bit + good_block;
		assert(pos < *pright_pos);
		x_blocks[i].pos = pos;
		x_blocks[i].expected_offset = expected_offset;
		high_bit <<= 1;
	}
	assert(high_bit + good_block >= *pright_pos);

	if (find_first_x_block(dev, x_blocks, n_samples,
			bss_to_set(bss, DIM(bss)), &i, &bs, rwi,
			cb, indent + 1))
		return true;
	if (i < n_samples) {
		*pright_pos = x_blocks[i].pos - good_block; /* = high_bit */
		cb(indent + 1, "INFO: Block %" PRIu64 " overwrites %s block %" PRIu64 "\n",
			x_blocks[i].pos, block_state_to_str(bs), good_block);
	}
	return false;
}

static uint64_t drive_mid_block(const struct device *dev)
{
	const uint64_t dev_size_byte = dev_get_size_byte(dev);
	const unsigned int block_order = dev_get_block_order(dev);
	return clp2((dev_size_byte >> block_order) / 2);
}

uint64_t probe_max_written_blocks(const struct device *dev)
{
	const int block_order = dev_get_block_order(dev);
	const uint64_t num_blocks = dev_get_size_byte(dev) >> block_order;
	const int n = ceiling_log2(num_blocks);

	return
		/* find_cache_size() */
		MIN(
			/* The maximum number of written blocks. */
			(MAX_CACHE_SIZE_BYTE >> block_order) * 2 - 1,
			/* High half of the drive. */
			num_blocks - drive_mid_block(dev)
		) +
		/* find_wrap(): only one block is written.
		 *
		 * Note: Both find_wrap() and sampling_probe() call
		 * overwhelm_cache(), which writes rwi->cache_size_block blocks.
		 * However, these blocks are written over the exact same
		 * block range previously written and saved during
		 * find_cache_size(). Thus, the safe device (sdev) deduplicates
		 * these cache writes, and they contribute 0 to the maximum
		 * number of unique written blocks bounded here.
		 */
		1 +
		/* sampling_probe():
		 *
		 * We assume that Phase 1 has at most n successes (finding a bad
		 * block). A success reduces the search space by moving the
		 * right boundary to the leftmost bad block found. Because the
		 * random samples are uniformly distributed, even if only one
		 * sample falls into the "bad" region (the portion of the interval
		 * containing bad blocks), the expected distance to that sample
		 * halves the size of the bad region. Since each success halves
		 * the bad region on average, and the initial bad region is at
		 * most N blocks, n (log2(N)) is a safe probabilistic upper bound
		 * for the number of successes. Each success writes at most
		 * SAMPLING_MAX blocks.
		 *
		 * Each failure (not finding a bad block) doubles the sample
		 * size, starting from SAMPLING_MIN up to SAMPLING_MAX. Since
		 * the sample size only doubles on failures, the number of
		 * blocks written during failures forms a geometric series:
		 *
		 *	SAMPLING_MIN + 2*SAMPLING_MIN + 4*SAMPLING_MIN + ... +
		 *		SAMPLING_MAX
		 *	= 2 * SAMPLING_MAX - SAMPLING_MIN
		 *
		 * Therefore, the total number of blocks written in Phase 1 is
		 * bounded by:
		 *
		 *	n * SAMPLING_MAX + 2 * SAMPLING_MAX - SAMPLING_MIN
		 *	= (n + 2) * SAMPLING_MAX - SAMPLING_MIN
		 *
		 * Phase 2 is a binary search (halving the range or narrowing
		 * it) which takes at most n iterations. In Phase 2, the sample
		 * size is fixed at SAMPLING_MIN. The total number of blocks
		 * written in Phase 2 is bounded by:
		 *
		 *	n * SAMPLING_MIN
		 *
		 * Finally, after the loop finishes, there is one last call to
		 * find_a_bad_block(). In the worst case, this call uses
		 * SAMPLING_MAX blocks.
		 *
		 * Summing Phase 1, Phase 2, and the last call yields:
		 *
		 *	(n + 2) * SAMPLING_MAX - SAMPLING_MIN +
		 *		n * SAMPLING_MIN + SAMPLING_MAX
		 *	= (n + 3) * SAMPLING_MAX + (n - 1) * SAMPLING_MIN
		 */
		(n + 3) * SAMPLING_MAX + (n - 1) * SAMPLING_MIN;
}

void report_probed_size(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t bytes, unsigned int block_order)
{
	double f = bytes;
	const char *unit = adjust_unit(&f);
	uint64_t blocks = bytes >> block_order;
	cb(indent, "%s %.2f %s (%" PRIu64 " block%s)\n",
		prefix, f, unit, blocks, blocks != 1 ? "s" : "");
}

void report_probed_order(unsigned int indent, progress_cb cb,
	const char *prefix, unsigned int order)
{
	double f = (1ULL << order);
	const char *unit = adjust_unit(&f);
	cb(indent, "%s %.2f %s (2^%i Byte%s)\n", prefix, f, unit, order,
		order != 0 ? "s" : "");
}

void report_probed_cache(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t cache_size_block,
	unsigned int block_order)
{
	double f = (cache_size_block << block_order);
	const char *unit = adjust_unit(&f);
	cb(indent, "%s %.2f %s (%" PRIu64 " block%s)\n",
		prefix, f, unit, cache_size_block,
		cache_size_block != 1 ? "s" : "");
}

int probe_device(struct device *dev, struct probe_results *results,
	progress_cb cb, int show_progress,
	long max_read_rate, long max_write_rate)
{
	const uint64_t dev_size_byte = dev_get_size_byte(dev);
	const unsigned int block_order = dev_get_block_order(dev);
	const progress_cb fw_cb = show_progress ? cb : dummy_cb;
	uint64_t left_pos, right_pos, mid_drive_pos;
	struct rdwr_info rwi;
	int wrap;

	dbuf_init(&rwi.seqw_dbuf);
	/* We initialize total_blocks to 0 because inc_total_blocks() is called
	 * to update it when new blocks become available.
	 */
	init_flow(&rwi.seqw_fw, block_order, 0, max_write_rate,
		FW_MAX_BLOCKS_PER_DELAY_NONE, fw_cb, 0);
	init_flow(&rwi.randw_fw, block_order, 0, max_write_rate,
		FW_MAX_BLOCKS_PER_DELAY_NONE, fw_cb, 0);
	init_flow(&rwi.randr_fw, block_order, 0, max_read_rate,
		FW_MAX_BLOCKS_PER_DELAY_NONE, fw_cb, 0);

	/* @left_pos must point to a good block.
	 * We just point to the last block of the first 1MB of the card
	 * because this region is reserved for partition tables.
	 *
	 * Given that all writing is confined to the interval
	 * (@left_pos, @right_pos), we avoid losing the partition table.
	 */
	assert(block_order <= MEGABYTE_ORDER);
	left_pos = (MEGABYTE_SIZE >> block_order) - 1;

	/* @right_pos must point to a bad block.
	 * We just point to the block after the very last block.
	 */
	right_pos = dev_size_byte >> block_order;

	/* @left_pos cannot be equal to @right_pos since
	 * @left_pos points to a good block, and @right_pos to a bad block.
	 */
	if (left_pos >= right_pos) {
		rwi.cache_size_block = 0;
		goto bad;
	}

	/* I, Michel Machado, define that any drive with less than
	 * this number of blocks is fake.
	 */
	mid_drive_pos = drive_mid_block(dev);

	assert(left_pos < mid_drive_pos);
	assert(mid_drive_pos < right_pos);

	/* This call is needed due to rand(). */
	srand(time(NULL));

	rwi.salt = uint64_rand();

	cb(0, "# Device geometry\n");
	report_probed_size(0, cb, "=> Announced size:", dev_size_byte,
		block_order);
	report_probed_order(0, cb, "=> Physical block size:", block_order);

	if (find_cache_size(dev, mid_drive_pos - 1, &right_pos, &rwi, cb, 0))
		goto bad;
	assert(mid_drive_pos <= right_pos);
	rwi.cache_pos = right_pos;
	report_probed_cache(0, cb, "=> Approximate cache size:",
		rwi.cache_size_block, block_order);

	if (find_wrap(dev, left_pos, &right_pos, &rwi, cb, 0))
		goto bad;
	wrap = ceiling_log2(right_pos << block_order);
	report_probed_order(0, cb, "=> Module:", wrap);

	if (sampling_probe(dev, left_pos, &right_pos, &rwi, cb, 0))
		goto bad;

	if (right_pos == left_pos + 1) {
		/* Bad drive. */
		right_pos = 0;
	}

	results->real_size_byte = right_pos << block_order;
	results->wrap = wrap;
	goto out;

bad:
	results->real_size_byte = 0;
	results->wrap = ceiling_log2(dev_size_byte);

out:
	dbuf_free(&rwi.seqw_dbuf);
	report_probed_size(0, cb, "=> Usable size:",
		results->real_size_byte, block_order);
	cb(0, "# I/O average speeds\n");
	fw_get_measurements(&rwi.seqw_fw, &results->seqw_blocks,
		&results->seqw_time_ns);
	fw_get_measurements(&rwi.randw_fw, &results->randw_blocks,
		&results->randw_time_ns);
	fw_get_measurements(&rwi.randr_fw, &results->randr_blocks,
		&results->randr_time_ns);
	report_io_speed(0, cb, "=> Sequential write:",
		results->seqw_blocks, "block", results->seqw_time_ns,
		block_order);
	report_io_speed(0, cb, "=> Random write:",
		results->randw_blocks, "block", results->randw_time_ns,
		block_order);
	report_io_speed(0, cb, "=> Random read:",
		results->randr_blocks, "block", results->randr_time_ns,
		block_order);
	results->announced_size_byte = dev_size_byte;
	results->cache_size_block = rwi.cache_size_block;
	results->block_order = block_order;
	return false;
}
