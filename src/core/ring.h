/* ring.h - heap-allocated ring buffer of Samples (the original kept 2 MiB on the stack). */
#ifndef WFT_RING_H
#define WFT_RING_H
#include "wft_types.h"

typedef struct SampleRing {
    Sample *buf;
    int cap, head, count;
} SampleRing;

bool ring_init(SampleRing *r, int cap);
void ring_free(SampleRing *r);
void ring_push(SampleRing *r, const Sample *s);
/* Copy samples oldest->newest into out; returns number copied. */
int  ring_linearize(const SampleRing *r, Sample *out, int out_cap);

#endif
