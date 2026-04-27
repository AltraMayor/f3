#ifndef HEADER_LIBUTILS_H
#define HEADER_LIBUTILS_H

#include <stdint.h>
#include <argp.h>	/* For struct argp_state.	*/
#include <time.h>	/* For struct timespec.		*/

#define SECTOR_ORDER	(9)
#define KILOBYTE_ORDER	(10)
#define MEGABYTE_ORDER	(20)
#define GIGABYTE_ORDER	(30)
#define TERABYTE_ORDER	(40)

#define SECTOR_SIZE	(1ULL << SECTOR_ORDER)
#define GIGABYTE_SIZE	(1ULL << GIGABYTE_ORDER)

#define UNUSED(x)	((void)x)
#define DIM(x)		(sizeof(x) / sizeof((x)[0]))

#define GEN_MIN(name, type)				\
	static inline type name##_min(type a, type b)	\
	{						\
		return a < b ? a : b;			\
	}

GEN_MIN(ul, unsigned long)
GEN_MIN(ull, unsigned long long)

#define MIN(a, b) _Generic(1 ? (a) : (b),	\
	unsigned long: ul_min,			\
	unsigned long long: ull_min		\
	)(a, b)

typedef void (*progress_cb)(unsigned int indent, const char *format, ...);

void printf_cb(unsigned int indent, const char *format, ...);
void printf_flush_cb(unsigned int indent, const char *format, ...);
void dummy_cb(unsigned int indent, const char *format, ...);

static inline bool is_power_of_2(uint64_t x)
{
	return x && !(x & (x - 1));
}

int ilog2(uint64_t x);

/* Least power of 2 greater than or equal to x. */
uint64_t clp2(uint64_t x);

static inline int ceiling_log2(uint64_t x)
{
	return ilog2(clp2(x));
}

const char *adjust_unit(double *ptr_bytes);

#define TIME_STR_SIZE	128

int nsec_to_str(uint64_t nsec, char *str);

/*
 * The functions align_head() and align_mem() are used to align pointers.
 *
 * The following example allocates two block on stack and makes sure that
 * the blocks are aligned with the block size.
 *
 *	// The number 2 below means two blocks.
 *	char stack[align_head(block_order) + (2 << block_order)];
 *	char *stamp_blk, *probe_blk;
 *	stamp_blk = align_mem(stack, block_order);
 *	probe_blk = stamp_blk + block_size;
 */

static inline unsigned int align_head(unsigned int order)
{
	return (1U << order) - 1;
}

void *align_mem2(void *p, unsigned int order, int *shift);

static inline void *align_mem(void *p, unsigned int order)
{
	int shift;
	return align_mem2(p, order, &shift);
}

/* Return true if @ptr is aligned to @alignment.
 * @alignment must be a power of 2.
 */
static inline bool is_aligned(const void *ptr, size_t alignment)
{
	return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

void print_header(FILE *f, const char *name);

long long arg_to_ll_bytes(const struct argp_state *state, const char *arg);

/* Dependent on the byte order of the processor (i.e. endianness). */
void fill_buffer_with_block(void *buf, unsigned int block_order,
	uint64_t offset, uint64_t salt);

enum block_state {
	bs_unknown,
	bs_good,
	bs_bad,
	bs_changed,
	bs_overwritten,
};

const char *block_state_to_str(enum block_state state);

struct block_stats {
	uint64_t ok;
	uint64_t bad;
	uint64_t changed;
	uint64_t overwritten;
};

/* Dependent on the byte order of the processor (i.e. endianness). */
enum block_state validate_buffer_with_block(const void *buf,
	unsigned int block_order, uint64_t expected_offset,
	uint64_t *pfound_offset, uint64_t salt);

enum block_state validate_block_update_stats(const void *buf,
	unsigned int block_order, uint64_t expected_offset,
	uint64_t *pfound_offset, uint64_t salt, struct block_stats *stats);

static inline uint64_t diff_timespec_ns(const struct timespec *t1,
	const struct timespec *t2)
{
	return (t2->tv_sec - t1->tv_sec) * 1000000000ULL +
		t2->tv_nsec - t1->tv_nsec;
}

void print_stats(const struct block_stats *stats, unsigned int block_order,
	const char *unit_name);

void print_avg_min_max_samples(const char *prefix, const char *suffix,
	double avg_speed, double min_speed, double max_speed, uint64_t samples);

void report_io_speed(unsigned int indent, progress_cb cb, const char *prefix,
	uint64_t blocks, const char *block_unit, uint64_t time_ns,
	unsigned int block_order);

/* Return speed in bytes per second. */
static inline double calc_avg_speed(unsigned int block_order, uint64_t blocks,
	uint64_t time_ns)
{
	return (blocks << block_order) * 1000000000.0 / time_ns;
}

#endif	/* HEADER_LIBUTILS_H */
