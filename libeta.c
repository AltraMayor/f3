#include "libeta.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static uint64_t now_ms(void)
{
#ifdef CLOCK_MONOTONIC
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	uint64_t ret = t.tv_sec;
	ret *= 1000;
	ret += t.tv_nsec / 1000000;
#else
	struct timeval t;
	gettimeofday(&t, NULL);
	uint64_t ret = t.tv_sec;
	ret *= 1000;
	ret += t.tv_usec / 1000;
#endif
	return ret;
}

// _Generic is C11, the code is C99, so it's macro based dispatch.
#if INT64_MAX == INT_MAX
typedef div_t divi64_t;
divi64_t divi64(int64_t numer, int64_t denom) { return div(numer, denom); }
#elif INT64_MAX == LONG_MAX
typedef ldiv_t divi64_t;
divi64_t divi64(int64_t numer, int64_t denom) { return ldiv(numer, denom); }
#elif INT64_MAX == LLONG_MAX
typedef lldiv_t divi64_t;
divi64_t divi64(int64_t numer, int64_t denom) { return lldiv(numer, denom); }
#else
#error Unabled to dedice divi64()
#endif

void eta_init(struct etabar *e, uint64_t plan)
{
	memset(e, 0, sizeof(*e));
	e->plan = plan;
	e->start_ms = e->last_ms = now_ms();
}

// Exponential average of `speed` is an imprecise estimate as `speed` might
// fluctuate drastically.  E.g. the flash drive that made me write this patch
// has a major (x40) speed dip on every 18th sample.  Exponential average is
// way easier than an actual sliding window and it's good enough as the widget
// shows percentage and runtime as well.  Improvement over average of `speed`
// might be average of `pace`.  Time/work is meaningfully additive, while
// work/time is not.
//
// The damping factor might be calculated using following formula to remember
// _approximately_ N samples: alpha(N) = 1 - exp(-1 * log(2) / N)
void eta_stamp(struct etabar *e, uint64_t done)
{
	assert(e->done <= done && done <= e->plan);
	const uint64_t now = now_ms();
	const uint64_t dt = now - e->last_ms;
	const uint64_t dx = done - e->done;
	if (!dt || !dx) // low timer resolution or no-op
		return;
	const double sample = ((double)dt) / dx;
	const double alpha = 1. / 64; // 0.015625 ~ alpha(44)=0.01562
	const double beta = 1. - alpha;
	e->pace = e->done ? (alpha * sample + beta * e->pace) : sample;
	e->last_ms = now;
	e->done = done;
}

static const int64_t second_ms = 1000;
static const int64_t minute_ms = 60 * second_ms;
static const int64_t hour_ms = 60 * minute_ms;
static const int64_t day_ms = 24 * hour_ms;
static const int64_t day100_ms = 100 * day_ms;

static int sprintf_us(char *s, uint64_t udt)
{
	int a, b;
	int64_t dt = udt;
	if (0 <= dt && dt < hour_ms) {
		divi64_t q = divi64(dt, minute_ms);
		a = q.quot;
		b = q.rem / second_ms;
		return snprintf(s, 7, "%02im%02is", a, b);
	} else if (hour_ms <= dt && dt < day_ms) {
		divi64_t q = divi64(dt, hour_ms);
		a = q.quot;
		b = q.rem / minute_ms;
		return snprintf(s, 7, "%02ih%02im", a, b);
	} else if (day_ms <= dt && dt < day100_ms) {
		divi64_t q = divi64(dt, day_ms);
		a = q.quot;
		b = q.rem / hour_ms;
		return snprintf(s, 7, "%02id%02ih", a, b);
	} else {
		// 100 days is basically eternity for a disk check run
		return snprintf(s, 7, "(+inf)");
	}
}

static const size_t widget_len = 23; // PP.P% NNhNNm/(+inf) [/]

static const char spinchar[4] = {'-', '\\', '|', '/'};
static const char spinbegin = '_';
static const char spinend = '+';
static unsigned spint = 0;

static void mkwidget(char *widget, const struct etabar *e)
{
	char *it = widget;
	assert(e->done <= e->plan);
	const bool end = e->done == e->plan;
	if (!end)
		it += snprintf(it, 6, "%04.1f%%", floor(1000. * e->done / e->plan) / 10.);
	else
		it += snprintf(it, 6, " 100%%");
	*it++ = ' ';
	it += sprintf_us(it, e->last_ms - e->start_ms);
	*it++ = '/';
	if (e->pace != 0)
		it += sprintf_us(it, (e->plan - e->done) * e->pace);
	else
		it += sprintf_us(it, INT64_MAX);
	const char spinner = !e->done ? spinbegin : end ? spinend : spinchar[spint];
	spint = (spint + 1) % sizeof(spinchar);
	it += snprintf(it, 5, " [%c]", spinner);
	assert((size_t)(it - widget) == widget_len);
}

int eta_print(struct etabar *e)
{
	char widget[widget_len + 1];
	mkwidget(widget, e);
	fputs(widget, stdout);
	return fflush(stdout);
}

int eta_redraw(struct etabar *e)
{
	char widget[2 * widget_len + 1];
	memset(widget, '\b', widget_len);
	mkwidget(widget + widget_len, e);
	fputs(widget, stdout);
	return fflush(stdout);
}

int eta_clear(void)
{
	char widget[3 * widget_len + 1];
	memset(widget + 0 * widget_len, '\b', widget_len);
	memset(widget + 1 * widget_len, ' ', widget_len);
	memset(widget + 2 * widget_len, '\b', widget_len);
	widget[3 * widget_len] = '\0';
	fputs(widget, stdout);
	return fflush(stdout);
}
