#ifndef HEADER_LIBETA_H
#define HEADER_LIBETA_H

#include <stdint.h>

struct etabar {
	uint64_t plan;
	uint64_t done;
	uint64_t start_ms;
	uint64_t last_ms;
	double pace; // milliseconds per unit-of-work
};

void eta_init(struct etabar *, uint64_t plan);
void eta_stamp(struct etabar *, uint64_t done);
int eta_print(struct etabar *);
int eta_redraw(struct etabar *);
int eta_clear(void);

#endif
