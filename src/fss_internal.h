#ifndef FSS_INTERNAL_H
#define FSS_INTERNAL_H

#include "fss.h"
#include <string.h>
#include <stdlib.h>

struct fss_profile {
	size_t n;
	uint8_t presence[32]; /* bit i set => byte i appears */
	uint32_t hist[256];
	double entropy;       /* bits/byte estimate, 0 if unknown */
	int label;            /* 0=low, 1=mixed, 2=high */
	int budget;
	/* L2: block blooms (optional) */
	size_t block_bytes;
	size_t nblocks;
	uint64_t *blooms; /* nblocks * 4 words (256-bit bloom of 3-grams) */
};

/* L2: candidate block indices for needle, or (size_t)-1 => scan all. */
size_t fss_profile_candidate_blocks(const fss_profile *p, const uint8_t *nd,
                                    size_t m, size_t *idxs, size_t cap);

/* FNV-1a */
static inline uint64_t fss_hash_bytes(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

static inline int fss_presence_has(const uint8_t *pres, uint8_t b) {
	return (pres[b >> 3] >> (b & 7)) & 1;
}

static inline void fss_presence_set(uint8_t *pres, uint8_t b) {
	pres[b >> 3] |= (uint8_t)(1u << (b & 7));
}

/* Core search primitives (implemented in fss_find.c) */
ssize_t fss_find_raw(const uint8_t *h, size_t n, const uint8_t *nd, size_t m);
size_t  fss_count_raw(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                      int overlap);

typedef struct {
	size_t off;
	size_t len;
	uint32_t count;
	int est_lin;
} fss_cand;

/* L4: SA+LCP harvest inside one block (≤64 KiB). */
size_t fss_harvest_sa_block(const uint8_t *h, size_t n, size_t base_off,
                            size_t bn, size_t min_len, size_t max_len,
                            uint32_t min_count, int marker_base,
                            const fss_profile *prof, fss_cand *cands,
                            size_t max_cands, size_t nc, size_t verify_budget,
                            size_t *verifies);

#endif /* FSS_INTERNAL_H */
