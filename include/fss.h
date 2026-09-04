/*
 * fss — fractal substring search
 *
 * Exact substring find / count / batch-presence / repeat discovery.
 * Gist-then-foveate: cheap profile of the whole, then SIMD+Two-Way only
 * where needed. fmem/fcache optional for interned haystacks and memoization.
 */
#ifndef FSS_H
#define FSS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- kinds for fss_should_use ---------- */
enum {
	FSS_KIND_FIND = 0,
	FSS_KIND_COUNT = 1,
	FSS_KIND_BATCH = 2,
	FSS_KIND_REPEATS = 3
};

/* Return 1 when the library path is worth taking for this size class. */
int fss_should_use(size_t hay_n, size_t needle_m, int kind);

/* ---------- profile (lazy gist of a haystack) ---------- */

typedef struct fss_profile fss_profile;

/* budget: 0 = L0 only, 1 = L0+L1, 2 = L0+L1+L2 blocks */
fss_profile *fss_profile_build(const uint8_t *h, size_t n, int budget);
void         fss_profile_free(fss_profile *p);

/* 1 if every byte of needle is present in the haystack (L0). */
int fss_profile_may_contain(const fss_profile *p, const uint8_t *nd, size_t m);

/* "high" | "mixed" | "low" compressibility label (L1); "mixed" if unknown. */
const char *fss_profile_label(const fss_profile *p);

/* ---------- find / count / find-all ---------- */

/* First occurrence offset, or -1. Empty needle => 0 (memmem convention). */
ssize_t fss_find(const uint8_t *h, size_t n, const uint8_t *nd, size_t m);

/* Same with optional prebuilt profile (may be NULL). */
ssize_t fss_find_ex(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    const fss_profile *prof);

/*
 * Count occurrences. overlap=0 => PHP substr_count (non-overlapping greedy).
 * overlap=1 => every starting position.
 */
size_t fss_count(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                 int overlap);
size_t fss_count_ex(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    int overlap, const fss_profile *prof);

/* Fill offsets[]; returns number written (capped at out_cap). */
size_t fss_find_all(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    int overlap, size_t *offsets, size_t out_cap);

/* ---------- batch presence (hastok successor) ---------- */

/*
 * For each of k needles, write out_bits[i] = 1 if needle occurs as a substring
 * of h, else 0. Empty needle => 1.
 */
void fss_has_batch(const uint8_t *h, size_t n,
                   const uint8_t *const *nds, const size_t *ms, size_t k,
                   uint8_t *out_bits);

/* Contiguous blob form: each needle is (len, bytes) in blob; lens[i] lengths. */
void fss_has_batch_blob(const uint8_t *h, size_t n,
                        const uint8_t *blob, const uint32_t *offs,
                        const uint32_t *lens, size_t k, uint8_t *out_bits);

/*
 * Batch non-overlapping counts (PHP substr_count semantics when overlap=0).
 * out_counts[i] gets the count for needle i. Empty needle => 0.
 */
void fss_count_batch(const uint8_t *h, size_t n,
                     const uint8_t *const *nds, const size_t *ms, size_t k,
                     int overlap, uint32_t *out_counts);

void fss_count_batch_blob(const uint8_t *h, size_t n,
                          const uint8_t *blob, const uint32_t *offs,
                          const uint32_t *lens, size_t k, int overlap,
                          uint32_t *out_counts);

/* ---------- repeat discovery ---------- */

typedef struct {
	size_t min_len;
	size_t max_len;
	uint32_t min_count;
	size_t top_k;
	int marker_len_base; /* for estLin; 0 => use len/4+4 heuristic */
} fss_repeat_opts;

typedef struct {
	const uint8_t *p; /* points into haystack (not owned) */
	size_t len;
	uint32_t count;
	int est_lin;
} fss_repeat;

/*
 * Discover repeated substrings, score with estLin =
 *   len + n - count*(len - markerLen), keep best top_k.
 * Returns number of results written to out[].
 */
size_t fss_repeats(const uint8_t *h, size_t n, const fss_repeat_opts *o,
                   fss_repeat *out, size_t out_cap);

size_t fss_repeats_ex(const uint8_t *h, size_t n, const fss_repeat_opts *o,
                      fss_repeat *out, size_t out_cap,
                      const fss_profile *prof);

/* ---------- store: fmem + fcache + attached profile ---------- */

typedef struct fss_store fss_store;
typedef uint32_t fss_handle;
#define FSS_NULL ((fss_handle)0xFFFFFFFFu)

fss_store *fss_store_create(void);
void       fss_store_free(fss_store *s);

/* Intern haystack bytes; builds/attaches profile lazily on first search. */
fss_handle fss_intern(fss_store *s, const void *p, size_t n);
void       fss_release(fss_store *s, fss_handle h);

ssize_t fss_find_in(fss_store *s, fss_handle hay,
                    const uint8_t *nd, size_t m);
size_t  fss_count_in(fss_store *s, fss_handle hay,
                     const uint8_t *nd, size_t m, int overlap);

/* Batch presence with fcache memo; out_bits[i] = 0/1. */
void fss_has_batch_in(fss_store *s, fss_handle hay,
                      const uint8_t *const *nds, const size_t *ms, size_t k,
                      uint8_t *out_bits);

/*
 * Repeat discovery with fcache memo of (hay_hash, opts) → top-K.
 * out[i].p points into a store-pinned buffer valid until the next
 * fss_*_in call that rematerializes a different (or same) haystack.
 */
size_t fss_repeats_in(fss_store *s, fss_handle hay, const fss_repeat_opts *o,
                      fss_repeat *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* FSS_H */
