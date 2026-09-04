/*
 * fss_profile.c — cheap gist of a haystack (vision L0/L1/L2).
 *
 * L0: 256-bit byte presence — reject impossible needles with no scan.
 * L1: histogram + entropy + compressibility label (high/mixed/low).
 * L2: per-block 3-gram bloom filters — skip impossible blocks.
 */
#include "fss_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double entropy_from_hist(const uint32_t *hist, size_t n) {
	if (n == 0) return 0.0;
	double H = 0.0;
	double inv = 1.0 / (double)n;
	for (int i = 0; i < 256; i++) {
		if (!hist[i]) continue;
		double p = (double)hist[i] * inv;
		H -= p * log2(p);
	}
	return H;
}

/* Map entropy (bits/byte) to reciprocal-style label without calling deflate. */
static int label_from_entropy(double H) {
	/* High redundancy ≈ low entropy (repetitive). */
	if (H <= 3.5) return 2; /* high */
	if (H <= 5.5) return 1; /* mixed */
	return 0;               /* low */
}

static uint64_t gram3_hash(uint8_t a, uint8_t b, uint8_t c) {
	uint64_t x = ((uint64_t)a << 16) | ((uint64_t)b << 8) | (uint64_t)c;
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	return x;
}

static void bloom_add(uint64_t *bloom4, uint64_t h) {
	/* 4 hash positions into 256 bits */
	for (int k = 0; k < 4; k++) {
		uint64_t hh = h + (uint64_t)k * 0x9e3779b97f4a7c15ULL;
		unsigned bit = (unsigned)(hh & 255u);
		bloom4[bit >> 6] |= 1ull << (bit & 63);
	}
}

static int bloom_may(const uint64_t *bloom4, uint64_t h) {
	for (int k = 0; k < 4; k++) {
		uint64_t hh = h + (uint64_t)k * 0x9e3779b97f4a7c15ULL;
		unsigned bit = (unsigned)(hh & 255u);
		if (!(bloom4[bit >> 6] & (1ull << (bit & 63)))) return 0;
	}
	return 1;
}

fss_profile *fss_profile_build(const uint8_t *h, size_t n, int budget) {
	fss_profile *p = (fss_profile *)calloc(1, sizeof(*p));
	if (!p) return NULL;
	p->n = n;
	p->budget = budget < 0 ? 0 : (budget > 2 ? 2 : budget);
	p->label = 1; /* mixed default */

	/* Sample for huge n: full pass for L0 presence, sample for hist. */
	size_t sample_n = n;
	const uint8_t *sample = h;
	if (n > 131072) {
		sample_n = 131072;
		/* Take head sample (matches fractal_zip reciprocal_profile). */
	}

	for (size_t i = 0; i < n; i++) {
		fss_presence_set(p->presence, h[i]);
		if (i < sample_n) p->hist[h[i]]++;
	}
	/* If we only hist-sampled head, that's intentional. */

	if (p->budget >= 1) {
		p->entropy = entropy_from_hist(p->hist, sample_n ? sample_n : n);
		p->label = label_from_entropy(p->entropy);
	}

	if (p->budget >= 2 && n >= 4096) {
		p->block_bytes = 8192;
		p->nblocks = (n + p->block_bytes - 1) / p->block_bytes;
		p->blooms = (uint64_t *)calloc(p->nblocks * 4, sizeof(uint64_t));
		if (!p->blooms) {
			p->budget = 1;
			return p;
		}
		for (size_t b = 0; b < p->nblocks; b++) {
			size_t off = b * p->block_bytes;
			size_t len = p->block_bytes;
			if (off + len > n) len = n - off;
			uint64_t *bl = p->blooms + b * 4;
			if (len < 3) continue;
			for (size_t i = 0; i + 2 < len; i++) {
				bloom_add(bl, gram3_hash(h[off + i], h[off + i + 1],
				                         h[off + i + 2]));
			}
		}
	}
	(void)sample;
	return p;
}

void fss_profile_free(fss_profile *p) {
	if (!p) return;
	free(p->blooms);
	free(p);
}

int fss_profile_may_contain(const fss_profile *p, const uint8_t *nd, size_t m) {
	if (!p || m == 0) return 1;
	for (size_t i = 0; i < m; i++) {
		if (!fss_presence_has(p->presence, nd[i])) return 0;
	}
	/* L2: if needle has a 3-gram absent from EVERY block, still may span
	 * blocks — only reject if no block could hold all 3-grams. */
	if (p->blooms && p->nblocks && m >= 3) {
		int any_block = 0;
		for (size_t b = 0; b < p->nblocks; b++) {
			const uint64_t *bl = p->blooms + b * 4;
			int ok = 1;
			for (size_t i = 0; i + 2 < m; i++) {
				if (!bloom_may(bl, gram3_hash(nd[i], nd[i + 1], nd[i + 2]))) {
					ok = 0;
					break;
				}
			}
			if (ok) {
				any_block = 1;
				break;
			}
		}
		if (!any_block) return 0;
	}
	return 1;
}

/*
 * Fill idxs[] with L2 block indices that may contain nd.
 * Returns count written, or (size_t)-1 if L2 unavailable / too many survivors
 * (caller should scan the whole haystack).
 */
size_t fss_profile_candidate_blocks(const fss_profile *p, const uint8_t *nd,
                                    size_t m, size_t *idxs, size_t cap) {
	if (!p || !p->blooms || !p->nblocks || !idxs || cap == 0 || m < 3)
		return (size_t)-1;
	size_t written = 0;
	size_t half = p->nblocks / 2 + 1;
	for (size_t b = 0; b < p->nblocks; b++) {
		const uint64_t *bl = p->blooms + b * 4;
		int ok = 1;
		for (size_t i = 0; i + 2 < m; i++) {
			if (!bloom_may(bl, gram3_hash(nd[i], nd[i + 1], nd[i + 2]))) {
				ok = 0;
				break;
			}
		}
		if (!ok) continue;
		if (written >= cap || written >= half) return (size_t)-1;
		idxs[written++] = b;
	}
	return written;
}

const char *fss_profile_label(const fss_profile *p) {
	if (!p) return "mixed";
	if (p->label == 2) return "high";
	if (p->label == 0) return "low";
	return "mixed";
}
