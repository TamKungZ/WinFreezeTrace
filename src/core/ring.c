#include "ring.h"

#include <stdlib.h>
#include <string.h>

bool ring_init(SampleRing *r, int cap) {
    memset(r, 0, sizeof *r);
    if (cap < 1) cap = 1;
    if (cap > WFT_MAX_RING) cap = WFT_MAX_RING;
    r->buf = (Sample *)calloc((size_t)cap, sizeof(Sample));
    if (!r->buf) return false;
    r->cap = cap;
    return true;
}

void ring_free(SampleRing *r) {
    free(r->buf);
    memset(r, 0, sizeof *r);
}

void ring_push(SampleRing *r, const Sample *s) {
    if (!r->buf) return;
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) ++r->count;
}

int ring_linearize(const SampleRing *r, Sample *out, int out_cap) {
    if (!r->buf || !out) return 0;
    int n = r->count < out_cap ? r->count : out_cap;
    int start = (r->head - n + r->cap * 2) % r->cap;    /* newest n samples */
    for (int i = 0; i < n; ++i) out[i] = r->buf[(start + i) % r->cap];
    return n;
}
