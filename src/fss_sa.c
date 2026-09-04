/*
 * fss_sa.c — block-local suffix array + LCP (L4).
 *
 * Only for high-redundancy blocks ≤ 64 KiB. qsort SA is fine at that size;
 * never used for full-n immense low-redundancy text.
 */
#include "fss_internal.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t *g_sa_h;
static size_t g_sa_n;

static int sa_idx_cmp(const void *a, const void *b) {
	size_t i = *(const size_t *)a;
	size_t j = *(const size_t *)b;
	size_t ra = g_sa_n - i;
	size_t rb = g_sa_n - j;
	size_t m = ra < rb ? ra : rb;
	int c = memcmp(g_sa_h + i, g_sa_h + j, m);
	if (c) return c;
	if (ra < rb) return -1;
	if (ra > rb) return 1;
	return 0;
}

size_t fss_harvest_sa_block(const uint8_t *h, size_t n, size_t base_off,
                            size_t bn, size_t min_len, size_t max_len,
                            uint32_t min_count, int marker_base,
                            const fss_profile *prof, fss_cand *cands,
                            size_t max_cands, size_t nc, size_t verify_budget,
                            size_t *verifies) {
	if (!h || !cands || bn < min_len * 2 || bn > 65536 ||
	    base_off + bn > n)
		return nc;
	if (nc >= max_cands || *verifies >= verify_budget) return nc;

	size_t *sa = (size_t *)malloc(bn * sizeof(size_t));
	size_t *rank = (size_t *)malloc(bn * sizeof(size_t));
	size_t *lcp = (size_t *)malloc(bn * sizeof(size_t));
	if (!sa || !rank || !lcp) {
		free(sa);
		free(rank);
		free(lcp);
		return nc;
	}
	for (size_t i = 0; i < bn; i++) sa[i] = i;
	g_sa_h = h + base_off;
	g_sa_n = bn;
	qsort(sa, bn, sizeof(size_t), sa_idx_cmp);

	for (size_t i = 0; i < bn; i++) rank[sa[i]] = i;
	size_t k = 0;
	lcp[0] = 0;
	for (size_t i = 0; i < bn; i++) {
		if (rank[i] == 0) {
			k = 0;
			continue;
		}
		size_t j = sa[rank[i] - 1];
		while (i + k < bn && j + k < bn &&
		       h[base_off + i + k] == h[base_off + j + k])
			k++;
		lcp[rank[i]] = k;
		if (k) k--;
	}

	for (size_t i = 1; i < bn && nc < max_cands && *verifies < verify_budget;
	     i++) {
		size_t L = lcp[i];
		if (L < min_len) continue;
		if (L > max_len) L = max_len;
		size_t a = sa[i - 1];
		size_t b = sa[i];
		size_t off = base_off + (a < b ? a : b);
		int dup = 0;
		for (size_t c = 0; c < nc; c++) {
			if (cands[c].len == L &&
			    memcmp(h + cands[c].off, h + off, L) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup) continue;
		(*verifies)++;
		uint32_t cnt =
		    (uint32_t)fss_count_ex(h, n, h + off, L, 0, prof);
		if (cnt < min_count) continue;
		int ml = marker_base > 0
		             ? marker_base
		             : (int)(4 + (L >= 10 ? 2 : 0) + (L >= 100 ? 1 : 0));
		long long est = (long long)L + (long long)n -
		                (long long)cnt * ((long long)L - ml);
		if (est > 2147483647LL) est = 2147483647LL;
		if (est < -2147483647LL) est = -2147483647LL;
		cands[nc].off = off;
		cands[nc].len = L;
		cands[nc].count = cnt;
		cands[nc].est_lin = (int)est;
		nc++;
		while (i + 1 < bn && lcp[i + 1] >= L) i++;
	}

	free(sa);
	free(rank);
	free(lcp);
	return nc;
}
