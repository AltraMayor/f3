#ifndef HEADER_UTILS_H
#define HEADER_UTILS_H

#include <features.h>

#define SECTOR_SIZE (512)
#define GIGABYTES   (1024 * 1024 * 1024)

const char *adjust_unit(double *ptr_bytes);

#ifndef __GLIBC__

/*
 * The following functions were copied from GNU Library C to make F3
 * more portable.
 */

/* Data structure for communication with thread safe versions.  This
 * type is to be regarded as opaque.  It's only exported because users
 * have to allocate objects of this type.
 */
struct drand48_data {
	unsigned short int __x[3];	/* Current state.		    */
	unsigned short int __old_x[3];	/* Old state.			    */
	unsigned short int __c;	/* Additive const. in congruential formula. */
	unsigned short int __init;	/* Flag for initializing.	    */
	unsigned long long int __a;	/* Factor in congruential formula.  */
};

/* Seed random number generator.  */
extern int srand48_r(long int __seedval, struct drand48_data *__buffer)
	__attribute__ ((nonnull(2)));

/* Return non-negative, long integer in [0,2^31).  */
extern int lrand48_r(struct drand48_data *__restrict __buffer,
	long int *__restrict __result) __attribute__ ((nonnull(1, 2)));

#endif	/* __GLIBC__ */

#endif	/* HEADER_UTILS_H */
