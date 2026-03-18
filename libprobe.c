#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>	/* For time().		*/
#include <sys/time.h>	/* For gettimeofday().	*/
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
		cb(indent, "I/O ERROR: Write error at blocks [%" PRIu64 ", %" PRIu64 "]!\n",
			first_pos, last_pos);
		return true;
	}
	return false;
}

struct write_info {
	uint64_t cache_pos;
	uint64_t cache_size_block;
	uint64_t salt;

	struct dynamic_buffer seqw_dbuf;
	struct flow seqw_fw;
	struct flow randw_fw;
};

static int write_random_blocks(struct device *dev, const uint64_t pos[],
	uint32_t n_pos, struct write_info *wi, progress_cb cb,
	unsigned int indent)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	/* Aligning these pointers is necessary to directly read and write
	 * the block device. For the file device, this is superfluous.
	 */
	char stack[align_head(block_order) + block_size];
	char *buffer = align_mem(stack, block_order);
	uint32_t i;

	if (n_pos == 0)
		return false;

	inc_total_size(&wi->randw_fw, n_pos << block_order);
	fw_set_indent(&wi->randw_fw, indent);

	start_measurement(&wi->randw_fw);
	for (i = 0; i < n_pos; i++) {
		fill_buffer_with_block(buffer, block_order,
			pos[i] << block_order, wi->salt);
		if (_write_blocks(dev, buffer, pos[i], pos[i], &wi->randw_fw,
				cb, indent))
			return true;
		measure(0, &wi->randw_fw, block_size);
	}
	end_measurement(0, &wi->randw_fw);
	return false;
}

static int write_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block,
	struct write_info *wi, progress_cb cb, unsigned int indent)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	uint64_t offset = first_block << block_order;
	uint64_t first_pos = first_block;

	if (first_block > last_block)
		return false;

	inc_total_size(&wi->seqw_fw,
		(last_block - first_block + 1) << block_order);
	fw_set_indent(&wi->seqw_fw, indent);

	start_measurement(&wi->seqw_fw);
	while (first_pos <= last_block) {
		const uint64_t chunk_bytes = get_rem_chunk_size(&wi->seqw_fw);
		const uint64_t needed_size =
			align_head(block_order) + chunk_bytes;
		const uint64_t max_blocks_to_write =
			last_block - first_pos + 1;
		uint64_t blocks_to_write;
		int shift;
		char *buffer, *stamp_blk;
		size_t buf_len;
		uint64_t pos, next_pos;

		buffer = align_mem2(dbuf_get_buf(&wi->seqw_dbuf, needed_size),
			block_order, &shift);
		buf_len = dbuf_get_len(&wi->seqw_dbuf);

		blocks_to_write = buf_len >= needed_size
			? chunk_bytes >> block_order
			: (buf_len - shift) >> block_order;
		if (blocks_to_write > max_blocks_to_write)
			blocks_to_write = max_blocks_to_write;

		next_pos = first_pos + blocks_to_write - 1;

		stamp_blk = buffer;
		for (pos = first_pos; pos <= next_pos; pos++) {
			fill_buffer_with_block(stamp_blk, block_order, offset,
				wi->salt);
			stamp_blk += block_size;
			offset += block_size;
		}

		if (_write_blocks(dev, buffer, first_pos, next_pos,
				&wi->seqw_fw, cb, indent))
			return true;

		/* Since parameter func_flush_chunk of init_flow() is NULL,
		 * the parameter fd of measure() is ignored.
		 */
		measure(0, &wi->seqw_fw, blocks_to_write << block_order);
		first_pos = next_pos + 1;
	}
	end_measurement(0, &wi->seqw_fw);
	return false;
}

static int overwhelm_cache(struct device *dev,
	struct write_info *wi, progress_cb cb, unsigned int indent)
{
	if (wi->cache_size_block == 0)
		return false;
	cb(indent, "Overwhelming cache\n");
	return write_blocks(dev, wi->cache_pos,
		wi->cache_pos + wi->cache_size_block - 1, wi, cb, indent);
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
static int read_block(struct device *dev, char *buf, uint64_t pos,
	progress_cb cb, unsigned int indent)
{
	if (dev_read_blocks(dev, buf, pos, pos) &&
		dev_read_blocks(dev, buf, pos, pos)) {
		cb(indent, "I/O ERROR: Read error at block %" PRIu64 "!\n", pos);
		return true;
	}
	return false;
}

static int is_block_good_with_offset(struct device *dev, uint64_t pos,
	uint64_t expected_offset, enum block_state *pbs, uint64_t salt,
	progress_cb cb, unsigned int indent)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + block_size];
	char *probe_blk = align_mem(stack, block_order);
	uint64_t found_offset;

	if (read_block(dev, probe_blk, pos, cb, indent))
		return true;

	*pbs = validate_buffer_with_block(probe_blk, block_order,
		expected_offset, &found_offset, salt);
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
	enum block_state *pstate, struct write_info *wi,
	progress_cb cb, unsigned int indent)
{
	uint32_t i;

	for (i = 0; i < n_blocks; i++) {
		enum block_state bs;
		if (is_block_good_with_offset(dev, x_blocks[i].pos,
				x_blocks[i].expected_offset, &bs, wi->salt,
				cb, indent))
			return true;
		if (in_bs_set(bs_set, bs)) {
			*pfirst_x_block_idx = i;
			*pstate = bs;
			return false;
		}
	}
	*pfirst_x_block_idx = n_blocks;
	return false;
}

static int find_first_bad_block(struct device *dev, const uint64_t pos[],
	uint32_t n_pos, bool *pany_bad, uint64_t *pbad_pos,
	struct write_info *wi, progress_cb cb, unsigned int indent)
{
	const int block_order = dev_get_block_order(dev);
	/* All but bs_good. */
	const enum block_state bss[] = {bs_unknown, bs_bad, bs_changed,
		bs_overwritten};
	struct def_x_block x_blocks[n_pos];
	enum block_state bs;
	uint32_t i;

	for (i = 0; i < n_pos; i++) {
		x_blocks[i].pos = pos[i];
		x_blocks[i].expected_offset = pos[i] << block_order;
	}

	if (find_first_x_block(dev, x_blocks, n_pos,
			bss_to_set(bss, DIM(bss)),
			&i, &bs, wi, cb, indent))
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
	struct write_info *wi, progress_cb cb, unsigned int indent)
{
	uint32_t n_samples = 64;
	uint64_t samples[n_samples];
	bool is_linear, any_bad;
	uint64_t bad_pos;

	if (first_pos > last_pos)
		goto not_found;

	fill_samples(samples, &n_samples, first_pos, last_pos, false,
		&is_linear);
	cb(indent, "Sampling %" PRIu32 " blocks from blocks [%" PRIu64 ", %" PRIu64 "]\n",
		n_samples, first_pos, last_pos);
	if (find_first_bad_block(dev, samples, n_samples, &any_bad, &bad_pos,
			wi, cb, indent))
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
	struct write_info *wi, progress_cb cb, unsigned int indent)
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
	cb(indent, "## Sampling %" PRIu32 " blocks from blocks (%" PRIu64 ", %" PRIu64 ")\n",
		n_samples, left_pos, *pright_pos);

	cb(indent + 1, "Writing random blocks\n");

	if (is_linear) {
		if (write_blocks(dev, left_pos + 1, *pright_pos - 1, wi, cb,
				indent + 1))
			return true;
	} else {
		if (write_random_blocks(dev, samples, n_samples, wi,
				cb, indent + 1))
			return true;
	}

	if (overwhelm_cache(dev, wi, cb, indent + 1))
		return true;

	/* Test samples. */
	cb(indent + 1, "Reading written blocks\n");
	if (find_first_bad_block(dev, samples, n_samples, &any_bad, &bad_pos,
			wi, cb, indent + 1))
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
 */
#define SAMPLING_MIN (8)
#define SAMPLING_MAX (1024)

/* This function assumes that the block at @left_pos is good, and
 *	that the block at @*pright_pos is bad.
 */
static int sampling_probe(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	struct write_info *wi, progress_cb cb, unsigned int indent)
{
	uint32_t n_samples = SAMPLING_MIN;
	int found_a_bad_block;
	bool phase1 = true;

	assert(SAMPLING_MAX >= SAMPLING_MIN);
	cb(indent, "# Sampling\n");

	while (*pright_pos > left_pos + n_samples + 1) {
		if (find_a_bad_block(dev, n_samples, left_pos, pright_pos,
				&found_a_bad_block, wi, cb, indent + 1))
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
			&found_a_bad_block, wi, cb, indent + 1))
		return true;
	return false;
}

static void report_cache_size_test(unsigned int indent, progress_cb cb,
	const struct device *dev, uint64_t first_pos, uint64_t last_pos)
{
	double f_size = (last_pos - first_pos + 1) * dev_get_block_size(dev);
	const char *unit = adjust_unit(&f_size);
	cb(indent, "## Testing cache size: %.2f %s; Blocks [%" PRIu64 ", %" PRIu64 "]\n",
		f_size, unit, first_pos, last_pos);
}

/* This constant needs to be a power of 2 and larger than 2^block_order. */
#define MAX_CACHE_SIZE_BYTE	(1ULL << 30)

static int find_cache_size(struct device *dev, const uint64_t left_pos,
	uint64_t *pright_pos, struct write_info *wi, progress_cb cb,
	unsigned int indent)
{
	const int block_order = dev_get_block_order(dev);
	const uint64_t end_pos = *pright_pos - 1;
	uint64_t write_target = 1;
	uint64_t final_write_target = MAX_CACHE_SIZE_BYTE >> block_order;
	uint64_t first_pos = *pright_pos;

	cb(indent, "# Find cache size\n");

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
		cb(indent + 2, "Writing blocks [%" PRIu64 ", %" PRIu64 "]\n",
			first_pos, last_pos);
		if (write_blocks(dev, first_pos, last_pos, wi, cb, indent + 2))
			goto bad;

		if (probabilistic_test(dev, first_pos, end_pos,
				&found_a_bad_block, wi, cb, indent + 2))
			goto bad;
		if (found_a_bad_block) {
			*pright_pos = first_pos;
			wi->cache_size_block = write_target == 1
				? 0 /* There is no cache. */
				: end_pos - first_pos + 1;
			return false;
		}

		write_target <<= 1;

	} while (write_target <= final_write_target);

	/* Good drive. */
	*pright_pos = end_pos + 1;
	wi->cache_size_block = 0;
	return false;

bad:
	/* *pright_pos does not change. */
	wi->cache_size_block = 0;
	return true;
}

static int find_wrap(struct device *dev,
	uint64_t left_pos, uint64_t *pright_pos,
	struct write_info *wi, progress_cb cb, unsigned int indent)
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
	struct def_x_block x_blocks[n_samples];
	bool any_bad;
	uint64_t bad_pos;
	int block_order;
	uint64_t expected_offset, high_bit;
	uint32_t i;
	enum block_state bs;

	cb(indent, "# Find module\n");

	/*
	 *	Basis
	 */

	/* Make sure that there is at least a good block at the beginning
	 * of the drive.
	 */

	if (good_block >= *pright_pos)
		return false;

	cb(indent + 1, "Writing reference block %" PRIu64 "\n", good_block);
	if (write_random_blocks(dev, &good_block, 1, wi, cb, indent + 1) ||
			overwhelm_cache(dev, wi, cb, indent + 1))
		return true;

	cb(indent + 1, "Reading reference block\n");
	if (find_first_bad_block(dev, &good_block, 1, &any_bad, &bad_pos,
			wi, cb, indent + 1) || any_bad)
		return true;

	/*
	 *	Inductive step
	 */

	cb(indent + 1, "Probing module (reading %" PRIu32 " blocks)\n",
		n_samples);

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

	if (find_first_x_block(dev, x_blocks, n_samples, bs_to_set(bs_good),
			&i, &bs, wi, cb, indent + 1))
		return true;
	if (i < n_samples) {
		assert(bs == bs_good);
		*pright_pos = x_blocks[i].pos - good_block; /* = high_bit */
		cb(indent + 1, "INFO: Block %" PRIu64 " overwrites block %" PRIu64 "\n",
			x_blocks[i].pos, good_block);
	}
	return false;
}

uint64_t probe_device_max_blocks(const struct device *dev)
{
	const int block_order = dev_get_block_order(dev);
	const uint64_t num_blocks = dev_get_size_byte(dev) >> block_order;
	const int n = ceiling_log2(num_blocks);

	return
		/* find_cache_size(): sum all write targets. */
		(MAX_CACHE_SIZE_BYTE >> block_order) * 2 - 1 +
		/* find_wrap(): only one block is written. */
		1 +
		/* sampling_probe() */
		(3 * n) * SAMPLING_MAX +	/* Upper bound for phase 1. */
		n * SAMPLING_MIN;		/* Upper bound for phase 2. */
}

void report_probed_size(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t bytes, int block_order)
{
	double f = bytes;
	const char *unit = adjust_unit(&f);
	cb(indent, "%s %.2f %s (%" PRIu64 " blocks)\n",
		prefix, f, unit, bytes >> block_order);
}

void report_probed_order(unsigned int indent, progress_cb cb,
	const char *prefix, int order)
{
	double f = (1ULL << order);
	const char *unit = adjust_unit(&f);
	cb(indent, "%s %.2f %s (2^%i Bytes)\n", prefix, f, unit, order);
}

void report_probed_cache(unsigned int indent, progress_cb cb,
	const char *prefix, uint64_t cache_size_block, int block_order)
{
	double f = (cache_size_block << block_order);
	const char *unit = adjust_unit(&f);
	cb(indent, "%s %.2f %s (%" PRIu64 " blocks)\n",
		prefix, f, unit, cache_size_block);
}

int probe_device(struct device *dev, uint64_t *preal_size_byte,
	uint64_t *pannounced_size_byte, int *pwrap, uint64_t *pcache_size_block,
	int *pblock_order, progress_cb cb, int show_progress)
{
	const uint64_t dev_size_byte = dev_get_size_byte(dev);
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	const progress_cb fw_cb = show_progress ? cb : dummy_cb;
	uint64_t left_pos, right_pos, mid_drive_pos;
	struct write_info wi;
	int wrap;

	assert(block_order <= 20);

	dbuf_init(&wi.seqw_dbuf);
	/* We initialize total_size to 0 because inc_total_size() is called
	 * to update it when new blocks become available.
	 */
	init_flow(&wi.seqw_fw, block_size, 0, 0, fw_cb, 0, NULL);
	init_flow(&wi.randw_fw, block_size, 0, 0, fw_cb, 0, NULL);

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
	if (left_pos >= right_pos) {
		wi.cache_size_block = 0;
		goto bad;
	}

	/* I, Michel Machado, define that any drive with less than
	 * this number of blocks is fake.
	 */
	mid_drive_pos = clp2(right_pos / 2);

	assert(left_pos < mid_drive_pos);
	assert(mid_drive_pos < right_pos);

	/* This call is needed due to rand(). */
	srand(time(NULL));

	wi.salt = uint64_rand();

	cb(0, "# Device geometry\n");
	report_probed_size(0, cb, "=> Announced size:", dev_size_byte,
		block_order);
	report_probed_order(0, cb, "=> Physical block size:", block_order);

	if (find_cache_size(dev, mid_drive_pos - 1, &right_pos, &wi, cb, 0))
		goto bad;
	assert(mid_drive_pos <= right_pos);
	wi.cache_pos = right_pos;
	report_probed_cache(0, cb, "=> Approximate cache size:",
		wi.cache_size_block, block_order);

	if (find_wrap(dev, left_pos, &right_pos, &wi, cb, 0))
		goto bad;
	wrap = ceiling_log2(right_pos << block_order);
	report_probed_order(0, cb, "=> Module:", wrap);

	if (sampling_probe(dev, left_pos, &right_pos, &wi, cb, 0))
		goto bad;

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
	dbuf_free(&wi.seqw_dbuf);
	report_probed_size(0, cb, "=> Usable size:",
		*preal_size_byte, block_order);
	*pannounced_size_byte = dev_size_byte;
	*pcache_size_block = wi.cache_size_block;
	*pblock_order = block_order;
	return false;
}
